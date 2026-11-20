#include "cc/app/cc_mcp_runtime_manager.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_platform.h"
#include "cc/ports/cc_thread.h"
#include "cc/util/cc_json.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief 本地协议故障，分别作用于握手、发现或工具响应。 */
enum fault {
    FAULT_NONE, FAULT_INIT_ERROR, FAULT_VERSION, FAULT_CAPABILITY, FAULT_SERVER_INFO,
    FAULT_NO_NOTIFY, FAULT_NOTIFY, FAULT_CALL_ERROR, FAULT_WRONG_ID, FAULT_DUPLICATE_ID,
    FAULT_ENVELOPE, FAULT_BOTH, FAULT_NEITHER, FAULT_TAIL, FAULT_TRANSPORT,
    FAULT_PAGES, FAULT_CURSOR_LOOP, FAULT_CONTENT, FAULT_IS_ERROR_TYPE
};

/** @brief main 保管到所有调用退出的传输状态，mutex/cond 只保护计数和调用屏障。 */
typedef struct protocol_state {
    cc_mutex_t mutex;
    cc_cond_t cond;
    enum fault fault;
    int initialize_count, notify_count, list_count, call_count, reset_count, destroy_count;
    int hold_call, held, release_call;
    int advance_initialize_ms, advance_notify_ms;
    int initialize_budget, notify_budget, list_budget, call_budget;
    cc_cancel_source_t *cancel_on_notify;
    char trace[64];
    size_t trace_length;
} protocol_state_t;

/** @brief 一组拥有配置、registry、manager 和同步原语的独立 fixture。 */
typedef struct fixture {
    protocol_state_t state;
    cc_config_t config;
    cc_tool_registry_t *registry;
    cc_mcp_runtime_manager_t *manager;
    cc_runtime_diagnostics_t diagnostics;
} fixture_t;

#ifdef CC_MCP_TEST_WRAP_CLOCK
static uint64_t clock_ms = 100;
/** @brief 读取测试原子单调时钟。
 * @return 当前毫秒值，允许主线程和 fake 工作线程并发读取。
 */
uint64_t __wrap_cc_platform_monotonic_ms(void)
{
    return __atomic_load_n(&clock_ms, __ATOMIC_RELAXED);
}
#endif

#ifdef CC_MCP_TEST_WRAP_FREE
static void *watched_response;
static int watched_frees;
/** @brief 由链接器提供的真实 free 入口，绕过本目标包装。
 * @param[in,out] pointer 消费标准分配器拥有地址，允许 NULL。
 * @post 原分配按标准 free 语义释放，不再访问该地址。
 */
void __real_free(void *pointer);
/** @brief 跟踪唯一故障响应缓冲的释放，其余分配按真实 free 处理。
 * @param[in,out] pointer 消费真实分配地址，允许 NULL。
 * @post 故障响应的精确地址释放次数累计，其余释放行为保持原样。
 */
void __wrap_free(void *pointer)
{
    if (pointer && pointer == watched_response) {
        watched_frees++;
        watched_response = NULL;
    }
    __real_free(pointer);
}
#endif

/** @brief 消费一次拥有结果，并要求该操作成功。
 * @param[in] result 按值移交的拥有结果。
 * @post 结果消息释放，错误通过断言报告。
 */
static void require_ok(cc_result_t result)
{
    if (result.code != CC_OK) fprintf(stderr, "unexpected error %d: %s\n", result.code,
                                     result.message ? result.message : "");
    int success = result.code == CC_OK;
    cc_result_free(&result);
    assert(success);
}

/** @brief 推进 fake 时钟，其他平台通过独立条件变量进行短等待。
 * @param[in] milliseconds 正毫秒增量，本回归最大一百毫秒。
 * @post 后续生产时钟读取观察到指定增量或真实短等待后的时间。
 */
static void advance_clock(int milliseconds)
{
    if (milliseconds <= 0) return;
#ifdef CC_MCP_TEST_WRAP_CLOCK
    __atomic_add_fetch(&clock_ms, (uint64_t)milliseconds, __ATOMIC_RELAXED);
#else
    cc_mutex_t mutex = NULL;
    cc_cond_t cond = NULL;
    require_ok(cc_mutex_create(&mutex));
    require_ok(cc_cond_create(&cond));
    cc_mutex_lock(mutex);
    cc_cond_timedwait(cond, mutex, milliseconds);
    cc_mutex_unlock(mutex);
    cc_cond_destroy(cond);
    cc_mutex_destroy(mutex);
#endif
}

/** @brief 在状态锁内追加一次协议阶段字符。
 * @param[in,out] state 已持 mutex 的有效 fixture。
 * @param[in] stage 单字节阶段 I/N/L/C/R。
 * @post trace 保持 NUL 结尾，容量不足通过断言报告。
 */
static void note_locked(protocol_state_t *state, char stage)
{
    assert(state->trace_length + 1U < sizeof(state->trace));
    state->trace[state->trace_length++] = stage;
    state->trace[state->trace_length] = '\0';
}

/** @brief 将结果正文包装为精确小整数 ID 的 JSON-RPC 响应。
 * @param[in] id 当前请求标识。
 * @param[in] member 本次同步借用的 result 或 error 成员文本。
 * @return 需 free 的拥有完整正文，测试分配失败以断言报告。
 */
static char *response(double id, const char *member)
{
    size_t capacity = strlen(member) + 128U;
    char *text = malloc(capacity);
    assert(text);
    snprintf(text, capacity, "{\"jsonrpc\":\"2.0\",\"id\":%.0f,%s}", id, member);
    return text;
}

/** @brief 返回当前握手故障对应的响应成员。
 * @param[in] fault 已固定的初始化故障模式。
 * @return 静态只读字符串借用，调用者不释放。
 */
static const char *initialize_member(enum fault fault)
{
    if (fault == FAULT_INIT_ERROR)
        return "\"error\":{\"code\":-32000,\"message\":\"initialize rejected\"}";
    if (fault == FAULT_VERSION)
        return "\"result\":{\"protocolVersion\":\"2099-01-01\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"fake\",\"version\":\"1\"}}";
    if (fault == FAULT_CAPABILITY)
        return "\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"serverInfo\":{\"name\":\"fake\",\"version\":\"1\"}}";
    if (fault == FAULT_SERVER_INFO)
        return "\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"fake\"}}";
    return "\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"fake\",\"version\":\"1\"}}";
}

/** @brief 构造当前工具调用故障的响应，保持请求 ID 动态匹配。
 * @param[in] fault 仅本次调用使用的故障模式。
 * @param[in] id 当前请求精确小整数 ID。
 * @return 需 free 的拥有正文，分配失败通过断言报告。
 */
static char *call_response(enum fault fault, double id)
{
    if (fault == FAULT_CALL_ERROR)
        return response(id, "\"result\":{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":\"business failure\"}]}" );
    if (fault == FAULT_WRONG_ID) return response(id + 1, "\"result\":{\"content\":[]}");
    if (fault == FAULT_CONTENT) return response(id, "\"result\":{\"content\":{}}");
    if (fault == FAULT_IS_ERROR_TYPE) return response(id, "\"result\":{\"isError\":\"true\",\"content\":[]}");
    if (fault == FAULT_BOTH)
        return response(id, "\"result\":{\"content\":[]},\"error\":{\"code\":-1,\"message\":\"both\"}");
    if (fault == FAULT_NEITHER) return response(id, "\"other\":{}");
    if (fault == FAULT_DUPLICATE_ID || fault == FAULT_ENVELOPE) {
        char text[256];
        if (fault == FAULT_DUPLICATE_ID)
            snprintf(text, sizeof(text), "{\"jsonrpc\":\"2.0\",\"id\":%.0f,\"id\":%.0f,\"result\":{\"content\":[]}}", id, id);
        else
            snprintf(text, sizeof(text), "{\"jsonrpc\":\"1.0\",\"id\":%.0f,\"result\":{\"content\":[]}}", id);
        return cc_copy_string(text);
    }
    char *text = response(id, "\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]}");
    if (fault == FAULT_TAIL) {
        size_t length = strlen(text);
        char *extended = realloc(text, length + 9U);
        assert(extended);
        text = extended;
        memcpy(text + length, " garbage", 9U);
    }
    return text;
}

/** @brief 同步响应握手、分页发现或工具调用，并可用条件屏障保留一个在途请求。
 * @param[in,out] self main 保管的有效状态，计数和屏障在其 mutex 内访问。
 * @param[in] request_json 借用完整请求，解析树返回前释放。
 * @param[in] timeout_ms 当前剩余正毫秒预算，分别记录各阶段。
 * @param[in] token 可选借用令牌，本回调只核对生产层传递而不持有。
 * @param[out] out_response_json 必填空槽，先置 NULL，所有交付正文均由 manager 释放。
 * @return 通常 CC_OK，故障传输在交付正文后返回 PLATFORM 以验证错误路径清理。
 */
static cc_result_t protocol_send(void *self, const char *request_json, int timeout_ms,
                                 cc_cancel_token_t *token, char **out_response_json)
{
    (void)token;
    protocol_state_t *state = self;
    *out_response_json = NULL;
    cc_json_value_t *request = NULL;
    cc_result_t rc = cc_json_parse_strict(request_json, &request);
    if (rc.code != CC_OK) return rc;
    double id = cc_json_number_value(cc_json_object_get(request, "id"));
    const char *method = cc_json_string_value(cc_json_object_get(request, "method"));
    assert(method && timeout_ms > 0);
    int is_call = strcmp(method, "tools/call") == 0;
    cc_mutex_lock(state->mutex);
    enum fault fault = state->fault;
    if (strcmp(method, "initialize") == 0) {
        state->initialize_count++;
        state->initialize_budget = timeout_ms;
        note_locked(state, 'I');
        *out_response_json = response(id, initialize_member(fault));
        advance_clock(state->advance_initialize_ms);
    } else if (strcmp(method, "tools/list") == 0) {
        assert(state->notify_count == state->initialize_count);
        state->list_count++;
        state->list_budget = timeout_ms;
        note_locked(state, 'L');
        cc_json_value_t *params = cc_json_object_get(request, "params");
        const char *cursor = cc_json_string_value(cc_json_object_get(params, "cursor"));
        if (cursor) assert(strcmp(cursor, "page2") == 0);
        if (fault == FAULT_PAGES || fault == FAULT_CURSOR_LOOP) {
            const char *member = !cursor ?
                "\"result\":{\"tools\":[{\"name\":\"echo\",\"inputSchema\":{\"type\":\"object\"}}],\"nextCursor\":\"page2\"}" :
                fault == FAULT_CURSOR_LOOP ? "\"result\":{\"tools\":[],\"nextCursor\":\"page2\"}" :
                "\"result\":{\"tools\":[{\"name\":\"second\",\"inputSchema\":{\"type\":\"object\"}}]}";
            *out_response_json = response(id, member);
        } else {
            *out_response_json = response(id,
                "\"result\":{\"tools\":[{\"name\":\"echo\",\"inputSchema\":{\"type\":\"object\"}}]}");
        }
    } else {
        assert(strcmp(method, "tools/call") == 0);
        state->call_count++;
        state->call_budget = timeout_ms;
        note_locked(state, 'C');
        if (state->hold_call && state->call_count == 1) {
            state->held = 1;
            cc_cond_broadcast(state->cond);
            while (!state->release_call) cc_cond_wait(state->cond, state->mutex);
        }
        *out_response_json = call_response(fault, id);
    }
    cc_mutex_unlock(state->mutex);
    cc_json_destroy(request);
    assert(*out_response_json);
    if (fault == FAULT_TRANSPORT && is_call) {
#ifdef CC_MCP_TEST_WRAP_FREE
        watched_response = *out_response_json;
        watched_frees = 0;
#endif
        return cc_result_error(CC_ERR_PLATFORM, "transport supplied an error and response");
    }
    return cc_result_ok();
}

/** @brief 验证 initialized 无 id 通知，记录预算并可在交接时取消当前调用。
 * @param[in,out] self 有效状态，覆盖同步调用。
 * @param[in] json 借用完整通知，解析树在返回前释放。
 * @param[in] timeout_ms 当前正毫秒剩余预算。
 * @param[in] token 可选借用令牌，取消场景核对与配置 source 一致。
 * @return 正常 CC_OK，通知故障返回 PLATFORM，不产生 JSON-RPC 响应。
 */
static cc_result_t protocol_notify(void *self, const char *json, int timeout_ms,
                                   cc_cancel_token_t *token)
{
    protocol_state_t *state = self;
    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse_strict(json, &root);
    if (rc.code != CC_OK) return rc;
    const char *method = cc_json_string_value(cc_json_object_get(root, "method"));
    assert(method && strcmp(method, "notifications/initialized") == 0 &&
           !cc_json_object_get(root, "id") && timeout_ms > 0);
    cc_json_destroy(root);
    cc_mutex_lock(state->mutex);
    state->notify_count++;
    state->notify_budget = timeout_ms;
    note_locked(state, 'N');
    assert(state->notify_count == state->initialize_count);
    advance_clock(state->advance_notify_ms);
    if (state->cancel_on_notify) {
        assert(token == cc_cancel_source_token(state->cancel_on_notify));
        cc_cancel_source_cancel(state->cancel_on_notify);
    }
    int fail = state->fault == FAULT_NOTIFY;
    cc_mutex_unlock(state->mutex);
    return fail ? cc_result_error(CC_ERR_PLATFORM, "initialized notification failed") : cc_result_ok();
}

/** @brief 记录重置且要求 fixture 没有被保留的在途工具请求。
 * @param[in,out] self 有效状态。
 * @return 始终 CC_OK，重置时机错误由断言报告。
 */
static cc_result_t protocol_reset(void *self)
{
    protocol_state_t *state = self;
    cc_mutex_lock(state->mutex);
    assert(!state->held || state->release_call);
    state->reset_count++;
    note_locked(state, 'R');
    cc_mutex_unlock(state->mutex);
    return cc_result_ok();
}

/** @brief 允许 manager 在普通调用期间解开 server 锁。
 * @param[in] self 忽略的借用状态。
 * @return 零，传输可并行。
 */
static int protocol_parallel(void *self) { (void)self; return 0; }

/** @brief 结束 transport 对 fixture 的借用，实际状态由外层保管。
 * @param[in,out] self 有效独占状态，所有调用已退出。
 * @post 销毁计数增加，mutex/cond 留待 fixture 清理。
 */
static void protocol_destroy(void *self) { ((protocol_state_t *)self)->destroy_count++; }

static const cc_mcp_transport_vtable_t protocol_vtable = {
    protocol_send, protocol_reset, protocol_parallel, protocol_destroy, protocol_notify
};
static const cc_mcp_transport_vtable_t no_notify_vtable = {
    protocol_send, protocol_reset, protocol_parallel, protocol_destroy, NULL
};

/** @brief 将 fixture 状态借给一个 manager 传输。
 * @param[in] config 只在调用期借用，本 fixture 不使用其字段。
 * @param[out] out 必填空资源槽，交付状态与静态虚表。
 * @param[in,out] user_data fixture 保管的状态，覆盖 manager 寿命。
 * @return 始终 CC_OK，无资源分配。
 */
static cc_result_t protocol_factory(const cc_config_mcp_server_t *config,
                                    cc_mcp_transport_t *out, void *user_data)
{
    (void)config;
    protocol_state_t *state = user_data;
    out->self = state;
    out->vtable = state->fault == FAULT_NO_NOTIFY ? &no_notify_vtable : &protocol_vtable;
    return cc_result_ok();
}

/** @brief 建立拥有 fixture 并加载一个无真实网络的 server。
 * @param[out] fixture 必填未拥有资源的对象，进入清零，成功后需 fixture_destroy。
 * @param[in] fault 在加载前固定的故障模式。
 * @param[in] ttl 空闲毫秒期限，零禁用。
 * @post 同步资源、配置、registry、manager 均已建立，单 server 错误保留 diagnostics。
 */
static void fixture_create(fixture_t *fixture, enum fault fault, int ttl)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->state.fault = fault;
    require_ok(cc_mutex_create(&fixture->state.mutex));
    require_ok(cc_cond_create(&fixture->state.cond));
    fixture->config.mcp.enabled = 1;
    fixture->config.mcp.session_idle_ttl_ms = ttl;
    fixture->config.mcp.server_count = 1;
    fixture->config.mcp.servers = calloc(1, sizeof(*fixture->config.mcp.servers));
    assert(fixture->config.mcp.servers);
    fixture->config.mcp.servers[0].connection_timeout_ms = 1000;
    fixture->config.mcp.servers[0].name = cc_copy_string("protocol");
    fixture->config.mcp.servers[0].transport = cc_copy_string("fake");
    require_ok(cc_tool_registry_create(&fixture->registry));
    require_ok(cc_mcp_runtime_manager_create(protocol_factory, &fixture->state, &fixture->manager));
    cc_runtime_diagnostics_reset(&fixture->diagnostics);
    require_ok(cc_mcp_runtime_manager_load_tools(fixture->manager, &fixture->config,
                                                fixture->registry, &fixture->diagnostics));
}

/** @brief 按工具、server、配置、同步原语顺序释放完整 fixture。
 * @param[in,out] fixture 有效已建立对象，所有工作线程已 join。
 * @post 传输恰好销毁一次，全部拥有资源释放，fixture 清零。
 */
static void fixture_destroy(fixture_t *fixture)
{
    cc_tool_registry_destroy(fixture->registry);
    cc_mcp_runtime_manager_destroy(fixture->manager);
    assert(fixture->state.destroy_count == 1);
    cc_config_destroy(&fixture->config);
    cc_cond_destroy(fixture->state.cond);
    cc_mutex_destroy(fixture->state.mutex);
    memset(fixture, 0, sizeof(*fixture));
}

/** @brief 查找并借用 echo 工具，registry 在本次调用后继续保管实例。
 * @param[in] fixture 有效已成功加载的 fixture。
 * @return 工具浅借用，寿命覆盖到 registry 销毁前。
 */
static cc_tool_t echo_tool(const fixture_t *fixture)
{
    cc_tool_t tool;
    require_ok(cc_tool_registry_find(fixture->registry, "mcp.protocol.echo", &tool));
    return tool;
}

/** @brief 验证不完整握手不发现工具，完整握手严格按 I/N/L 顺序。
 * @post 各独立 fixture 均完成销毁，无工具或传输借用遗留。
 */
static void test_handshake(void)
{
    enum fault faults[] = {FAULT_INIT_ERROR, FAULT_VERSION, FAULT_CAPABILITY,
                           FAULT_SERVER_INFO, FAULT_NO_NOTIFY, FAULT_NOTIFY};
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        fixture_t fixture;
        fixture_create(&fixture, faults[i], 0);
        assert(fixture.diagnostics.count == 1 && cc_tool_registry_count(fixture.registry) == 0);
        assert(fixture.state.list_count == 0);
        assert(fixture.state.notify_count == (faults[i] == FAULT_NOTIFY ? 1 : 0));
        fixture_destroy(&fixture);
    }
    fixture_t fixture;
    fixture_create(&fixture, FAULT_NONE, 0);
    assert(fixture.diagnostics.count == 0 && strcmp(fixture.state.trace, "INL") == 0);
    fixture_destroy(&fixture);
}

/** @brief 验证工具业务错误、非法参数和不完整/歧义响应均不作为成功结果交付。
 * @post 每次结果和拥有诊断已清理，传输错误附带正文已释放。
 */
static void test_calls_and_envelopes(void)
{
    fixture_t fixture;
    fixture_create(&fixture, FAULT_NONE, 0);
    cc_tool_t tool = echo_tool(&fixture);
    const char *invalid[] = {"[]", "{", "{} trailing", "{\"x\":\"a\\u0000tail\"}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        cc_tool_result_t result = {0};
        cc_result_t rc = tool.vtable->call(tool.self, invalid[i], NULL, &result);
        assert(rc.code != CC_OK && !result.ok && fixture.state.call_count == 0);
        cc_result_free(&rc);
        cc_tool_result_cleanup(&result);
    }
    fixture.state.fault = FAULT_CALL_ERROR;
    cc_tool_result_t result = {0};
    require_ok(tool.vtable->call(tool.self, "{}", NULL, &result));
    assert(!result.ok && result.error && strstr(result.error, "business failure") && !result.text);
    cc_tool_result_cleanup(&result);
    enum fault faults[] = {FAULT_WRONG_ID, FAULT_DUPLICATE_ID, FAULT_ENVELOPE, FAULT_BOTH,
                           FAULT_NEITHER, FAULT_TAIL, FAULT_CONTENT, FAULT_IS_ERROR_TYPE, FAULT_TRANSPORT};
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        fixture.state.fault = faults[i];
        cc_result_t rc = tool.vtable->call(tool.self, "{}", NULL, &result);
        assert(!result.ok && (rc.code != CC_OK || result.error));
        cc_result_free(&rc);
        cc_tool_result_cleanup(&result);
    }
#ifdef CC_MCP_TEST_WRAP_FREE
    assert(!watched_response && watched_frees == 1);
#endif
    fixture_destroy(&fixture);
}

/** @brief 验证两页工具独立接入，重复 cursor 产生有限失败诊断。
 * @post 两组 fixture 均释放，循环测试未依靠超时结束。
 */
static void test_pages(void)
{
    fixture_t fixture;
    fixture_create(&fixture, FAULT_PAGES, 0);
    assert(fixture.diagnostics.count == 0 && fixture.state.list_count == 2);
    assert(cc_tool_registry_count(fixture.registry) == 2);
    cc_tool_t second;
    require_ok(cc_tool_registry_find(fixture.registry, "mcp.protocol.second", &second));
    fixture_destroy(&fixture);
    fixture_create(&fixture, FAULT_CURSOR_LOOP, 0);
    assert(fixture.diagnostics.count == 1 && fixture.state.list_count <= 3);
    fixture_destroy(&fixture);
}

/** @brief 主线程保管到 join 的工具借用及独立拥有结果。 */
typedef struct call_job { cc_tool_t tool; cc_tool_result_t result; cc_result_t rc; } call_job_t;

/** @brief 在工具线程中保存一次调用结果，返回后交给主线程检查和清理。
 * @param[in,out] user_data 借用含工具、结果和返回值的工作对象，覆盖 join。
 * @return NULL，无线程返回对象。
 */
static void *call_worker(void *user_data)
{
    call_job_t *job = user_data;
    job->rc = job->tool.vtable->call(job->tool.self, "{}", NULL, &job->result);
    return NULL;
}

/** @brief 用确定屏障验证在途请求跨过 TTL 时不重置，完成后重新开始计算空闲期。
 * @post 首请求退出并 join 后才销毁 fixture，第二请求确实与首请求重叠。
 */
static void test_parallel_idle(void)
{
    fixture_t fixture;
    fixture_create(&fixture, FAULT_NONE, 50);
    fixture.state.hold_call = 1;
    call_job_t job = {0};
    job.tool = echo_tool(&fixture);
    cc_thread_t thread = NULL;
    require_ok(cc_thread_create(call_worker, &job, &thread));
    cc_mutex_lock(fixture.state.mutex);
    while (!fixture.state.held) cc_cond_wait(fixture.state.cond, fixture.state.mutex);
    cc_mutex_unlock(fixture.state.mutex);
    advance_clock(100);
    cc_tool_result_t result = {0};
    require_ok(job.tool.vtable->call(job.tool.self, "{}", NULL, &result));
    assert(result.ok && fixture.state.reset_count == 0 && fixture.state.initialize_count == 1);
    cc_tool_result_cleanup(&result);
    cc_mutex_lock(fixture.state.mutex);
    fixture.state.release_call = 1;
    cc_cond_broadcast(fixture.state.cond);
    cc_mutex_unlock(fixture.state.mutex);
    require_ok(cc_thread_join(thread));
    require_ok(job.rc);
    assert(job.result.ok);
    cc_tool_result_cleanup(&job.result);
    require_ok(job.tool.vtable->call(job.tool.self, "{}", NULL, &result));
    assert(result.ok && fixture.state.reset_count == 0);
    cc_tool_result_cleanup(&result);
    advance_clock(100);
    require_ok(job.tool.vtable->call(job.tool.self, "{}", NULL, &result));
    assert(result.ok && fixture.state.reset_count == 1 && fixture.state.initialize_count == 2 &&
           fixture.state.notify_count == 2);
    cc_tool_result_cleanup(&result);
    fixture_destroy(&fixture);
}

/** @brief 验证重握手通知与工具请求共享预算，通知期间取消后不发送工具请求。
 * @post 令牌在全部同步借用结束后释放，结果和 fixture 均已清理。
 */
static void test_budget_and_cancel(void)
{
    fixture_t fixture;
    fixture_create(&fixture, FAULT_NONE, 50);
    cc_tool_t tool = echo_tool(&fixture);
    fixture.state.advance_initialize_ms = 30;
    fixture.state.advance_notify_ms = 30;
    advance_clock(100);
    cc_tool_context_t context = {0};
    context.timeout_ms = 100;
    cc_tool_result_t result = {0};
    require_ok(tool.vtable->call(tool.self, "{}", &context, &result));
    assert(result.ok && fixture.state.call_budget > 0 && fixture.state.call_budget <= 40);
#ifdef CC_MCP_TEST_WRAP_CLOCK
    assert(fixture.state.initialize_budget == 100 && fixture.state.notify_budget == 70 &&
           fixture.state.call_budget == 40);
#endif
    cc_tool_result_cleanup(&result);
    fixture.state.advance_notify_ms = 80;
    advance_clock(100);
    int calls_before = fixture.state.call_count;
    require_ok(tool.vtable->call(tool.self, "{}", &context, &result));
    assert(!result.ok && result.error && fixture.state.call_count == calls_before);
    cc_tool_result_cleanup(&result);
    fixture.state.advance_initialize_ms = fixture.state.advance_notify_ms = 0;
    require_ok(cc_cancel_source_create(&fixture.state.cancel_on_notify));
    context.cancel_token = cc_cancel_source_token(fixture.state.cancel_on_notify);
    advance_clock(100);
    require_ok(tool.vtable->call(tool.self, "{}", &context, &result));
    assert(!result.ok && result.error && fixture.state.call_count == calls_before);
    cc_tool_result_cleanup(&result);
    cc_cancel_source_destroy(fixture.state.cancel_on_notify);
    fixture.state.cancel_on_notify = NULL;
    fixture_destroy(&fixture);
}

/** @brief 执行本地握手、响应、分页、并发空闲及预算取消回归。
 * @return 所有断言通过为零，失败通过断言终止并由测试运行器报告。
 * @note 本目标不连接进程或网络；Linux 包装单调时钟和唯一响应释放，其余平台短等待。
 */
int main(void)
{
    test_handshake();
    test_calls_and_envelopes();
    test_pages();
    test_parallel_idle();
    test_budget_and_cancel();
    return 0;
}
