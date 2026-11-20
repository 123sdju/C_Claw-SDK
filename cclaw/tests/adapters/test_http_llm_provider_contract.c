#include "cc/adapters/cc_http_llm_provider.h"
#include "cc/ports/cc_event_bus.h"
#include "../../core/vendor/cjson/cJSON.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief 独立串行 fixture，所有借用覆盖一次 provider 的创建、调用和销毁。 */
typedef struct fixture_state {
    int protocol_destroys, http_destroys, builds, parses, events, calls;
    int build_error, perform_error, empty_body, fail_empty_copy;
    int sized_body, invalid_size, missing_body, fail_text_resize, owned_success, report_metrics;
    int defer_finished;
    cc_llm_stream_kind_t stream_kind;
    long status;
    const char *packet;
    const char *expected_response;
    size_t packet_length, body_fragment;
    const char *retry_after;
} fixture_state_t;

/** @brief 主线程独占的输出计数及顺序记录。 */
typedef struct chunk_state {
    int text, usage, finished, reject_text, owned_success;
    char order[16];
    size_t order_length;
} chunk_state_t;

#ifdef CC_TEST_HTTP_LLM_OOM
static size_t fail_malloc_bytes;
static int fail_malloc_once, fail_calloc_once, fail_realloc_once;
static void *watched[32];
static size_t watched_frees;
static int metrics_fault;
static size_t metrics_nodes;

/** @brief 前置声明拥有地址登记器。 @param[in] pointer 本次待跟踪的有效地址。 @post 所有权保留在实际请求或响应。 */
static void watch_allocation(void *pointer);

/** @brief 绕过链接包装的真实分配。 @param[in] bytes 字节数。 @return 标准 malloc 结果。 */
void *__real_malloc(size_t bytes);
/** @brief 绕过链接包装的真实零分配。 @param[in] count 数量。 @param[in] bytes 单项字节数。 @return 标准 calloc 结果。 */
void *__real_calloc(size_t count, size_t bytes);
/** @brief 绕过包装扩容标准拥有地址。 @param[in,out] pointer 标准分配地址。 @param[in] bytes 新容量。 @return 标准 realloc 结果。 */
void *__real_realloc(void *pointer, size_t bytes);
/** @brief 绕过链接包装释放拥有地址。 @param[in,out] pointer 可为 NULL 的消费地址。 @post 标准 free 完成。 */
void __real_free(void *pointer);

/**
 * @brief 在明确目标长度处拒绝一次副本分配。
 * @param[in] bytes 本次分配字节数。
 * @return 命中故障时 NULL，其他交真实 malloc。
 */
void *__wrap_malloc(size_t bytes)
{
    if (fail_malloc_once && bytes == fail_malloc_bytes) {
        fail_malloc_once = 0;
        return NULL;
    }
    return __real_malloc(bytes);
}

/**
 * @brief 拒绝构造器的下一次零分配。
 * @param[in] count 元素数量。
 * @param[in] bytes 单项字节数。
 * @return 主动故障时 NULL，其余交真实 calloc。
 */
void *__wrap_calloc(size_t count, size_t bytes)
{
    if (fail_calloc_once) {
        fail_calloc_once = 0;
        return NULL;
    }
    return __real_calloc(count, bytes);
}

/**
 * @brief 拒绝一次响应补尾扩容，并维护移动后的拥有地址记录。
 * @param[in,out] pointer 标准拥有地址，可为 NULL。
 * @param[in] bytes 新字节容量，本回归只登记正容量扩容。
 * @return 主动故障时 NULL，其他返回真实 realloc 结果，失败保留旧记录。
 */
void *__wrap_realloc(void *pointer, size_t bytes)
{
    if (fail_realloc_once) { fail_realloc_once = 0; return NULL; }
    size_t slot = sizeof(watched) / sizeof(watched[0]);
    for (size_t i = 0; pointer && i < sizeof(watched) / sizeof(watched[0]); i++)
        if (watched[i] == pointer) { slot = i; break; }
    void *result = __real_realloc(pointer, bytes);
    if (result && slot < sizeof(watched) / sizeof(watched[0])) watched[slot] = result;
    return result;
}

/**
 * @brief 记录指定请求与错误响应分配的最终释放。
 * @param[in,out] pointer 消费地址，可为 NULL。
 * @post 匹配记录先清空防止地址复用误计，再调用真实 free。
 */
void __wrap_free(void *pointer)
{
    if (pointer) {
        for (size_t i = 0; i < sizeof(watched) / sizeof(watched[0]); i++) {
            if (watched[i] == pointer) {
                watched[i] = NULL;
                watched_frees++;
                break;
            }
        }
    }
    __real_free(pointer);
}

/**
 * @brief 在 stage 字段名分配处制造一次真实 cJSON OOM，并跟踪此前创建的独立值节点。
 * @param[in] bytes cJSON 请求字节数。
 * @return 目标字段名为 NULL，其他为标准 malloc 地址。
 */
static void *metrics_allocate(size_t bytes)
{
    if (metrics_fault && metrics_nodes >= 2 && bytes == sizeof("stage")) {
        metrics_fault = 0;
        return NULL;
    }
    void *result = malloc(bytes);
    if (result && metrics_fault && bytes == sizeof(cJSON)) {
        metrics_nodes++;
        if (metrics_nodes == 2) watch_allocation(result);
    }
    return result;
}
#endif

/**
 * @brief 在 Linux 登记一个需精确检查释放的地址，其余平台保持普通资源流程。
 * @param[in] pointer 有效拥有地址，此函数只登记，所有权留在请求或响应。
 * @post 活跃记录增加；容量不足由断言报告。
 */
static void watch_allocation(void *pointer)
{
    assert(pointer);
#ifdef CC_TEST_HTTP_LLM_OOM
    for (size_t i = 0; i < sizeof(watched) / sizeof(watched[0]); i++) {
        if (!watched[i]) { watched[i] = pointer; return; }
    }
    assert(!"fixture allocation tracker is full");
#else
    (void)pointer;
#endif
}

/**
 * @brief 深拷贝固定 fixture 字符串并登记释放责任。
 * @param[in] text 有效借用文本。
 * @return 标准 malloc 拥有副本，交接后由真实生产 cleanup 释放。
 */
static char *fixture_copy(const char *text)
{
    size_t bytes = strlen(text) + 1;
    char *copy = malloc(bytes);
    assert(copy);
    memcpy(copy, text, bytes);
    watch_allocation(copy);
    return copy;
}

/**
 * @brief 检查此前登记的生产拥有字段全部释放。
 * @post Linux 所有活跃记录均为空，其他平台由 sanitizer 和正常清理路径验证。
 */
static void check_released(void)
{
#ifdef CC_TEST_HTTP_LLM_OOM
    for (size_t i = 0; i < sizeof(watched) / sizeof(watched[0]); i++) assert(!watched[i]);
#endif
}

/**
 * @brief 可选生成带拥有消息的成功结果，验证中间层释放被覆盖的成功诊断。
 * @param[in] with_message 非零请求动态成功消息。
 * @return 拥有结果，消息登记后须由真实调用者 cc_result_free。
 */
static cc_result_t success_result(int with_message)
{
    if (!with_message) return cc_result_ok();
    cc_result_t rc = cc_result_error(CC_OK, "fixture owned successful diagnostic");
    assert(rc.message);
    watch_allocation(rc.message);
    return rc;
}

/**
 * @brief 返回可空协议名称，验证能力查询对未知名称的处理。
 * @param[in] self 未使用的 fixture 借用。
 * @return NULL，表示未知协议。
 */
static const char *protocol_name(void *self) { (void)self; return NULL; }

/**
 * @brief 构造拥有请求，允许故障时留下合法部分输出供 provider 清理。
 * @param[in,out] self 有效 fixture 借用。
 * @param[in] base_url 已复制地址借用，本 fixture 忽略。
 * @param[in] api_key 可选已复制凭据借用，本 fixture 忽略。
 * @param[in] model 已复制模型借用，本 fixture 忽略。
 * @param[in] request 有效统一请求借用，本 fixture 忽略。
 * @param[in] stream 请求是否流式。
 * @param[in,out] output 已清零请求槽，接管本次分配。
 * @return 指定构建故障时 IO，否则 CC_OK，两个分支均由调用者 cleanup 输出。
 */
static cc_result_t protocol_build(void *self, const char *base_url, const char *api_key,
                                   const char *model, const cc_llm_chat_request_t *request,
                                   int stream, cc_llm_http_request_t *output)
{
    fixture_state_t *state = self;
    (void)base_url; (void)api_key; (void)model; (void)request;
    state->builds++;
    output->url = fixture_copy("http://fixture.test/model");
    output->body_json = fixture_copy("{}");
    output->headers = calloc(1, sizeof(*output->headers));
    assert(output->headers);
    watch_allocation(output->headers);
    output->header_count = 1;
    output->headers[0].name = fixture_copy("Content-Type");
    output->headers[0].value = fixture_copy("application/json");
    output->stream_kind = stream ? state->stream_kind : CC_LLM_STREAM_NONE;
    output->defer_finished_until_end = state->defer_finished;
    return state->build_error ? cc_result_error(CC_ERR_IO, "fixture build failed") : success_result(state->owned_success);
}

/**
 * @brief 记录成功同步正文已到达协议层。
 * @param[in,out] self 有效 fixture 借用。
 * @param[in] json 有效 NUL 结尾正文借用，可为空串。
 * @param[in,out] output 已清零响应，fixture 保持空结果。
 * @return 始终 CC_OK。
 */
static cc_result_t protocol_parse(void *self, const char *json, cc_llm_response_t *output)
{
    fixture_state_t *state = self;
    (void)output;
    assert(json);
    if (state->expected_response) assert(strcmp(json, state->expected_response) == 0);
    state->parses++;
    return success_result(state->owned_success);
}

/**
 * @brief 将固定事件转换成文本、用量或完成信号，验证通用分帧与完成阶段。
 * @param[in,out] self 有效 fixture 借用。
 * @param[in] event 借用已分帧文本。
 * @param[in] callback 有效同步块回调。
 * @param[in,out] data 回调上下文借用。
 * @param[out] finished 有效零值槽，finish 事件置一。
 * @return 下游结果、CC_OK 或未知事件 JSON 错误。
 */
static cc_result_t protocol_event(void *self, const char *event,
                                   cc_llm_stream_callback_fn callback, void *data, int *finished)
{
    fixture_state_t *state = self;
    state->events++;
    cc_stream_chunk_t chunk = {0};
    chunk.tool_index = -1;
    if (strcmp(event, "text") == 0) {
        chunk.type = CC_STREAM_CHUNK_TEXT;
        chunk.content = "x";
    } else if (strcmp(event, "usage") == 0) {
        chunk.type = CC_STREAM_CHUNK_USAGE;
        chunk.usage.completion_tokens = 1;
    } else if (strcmp(event, "finish") == 0) {
        *finished = 1;
        return success_result(state->owned_success);
    } else if (strcmp(event, "directfinish") == 0) {
        chunk.type = CC_STREAM_CHUNK_FINISHED;
        cc_result_t rc = callback(&chunk, data);
        if (rc.code != CC_OK) return rc;
        cc_result_free(&rc);
        return callback(&chunk, data);
    } else return cc_result_error(CC_ERR_JSON, "unexpected fixture event");
    return callback(&chunk, data);
}

/**
 * @brief 记录协议析构，fixture 状态本身由测试栈保管。
 * @param[in,out] self 有效 fixture 借用。
 * @post 协议析构计数增加一次。
 */
static void protocol_destroy(void *self) { ((fixture_state_t *)self)->protocol_destroys++; }

/**
 * @brief 模拟同步 HTTP，支持错误部分响应、任意 body 切块和空正文。
 * @param[in,out] self 有效 fixture 借用。
 * @param[in] request 在调用期间借用的 HTTP 请求，回调全部在返回前结束。
 * @param[in,out] response 已清零拥有响应，成功或失败均由调用者释放。
 * @return 指定传输错误、body 回调错误或 CC_OK。
 */
static cc_result_t http_perform(void *self, const cc_http_request_t *request,
                                 cc_http_response_t *response)
{
    fixture_state_t *state = self;
    state->calls++;
    response->status_code = state->status ? state->status : 200;
    if (state->retry_after) {
        response->headers = calloc(1, sizeof(*response->headers));
        assert(response->headers);
        watch_allocation(response->headers);
        response->header_count = 1;
        response->headers[0].name = fixture_copy("Retry-After");
        response->headers[0].value = fixture_copy(state->retry_after);
    }
    if (state->perform_error) {
        response->body = fixture_copy("partial response");
        return cc_result_error(CC_ERR_NETWORK, "fixture transport failure");
    }
    if (state->missing_body) response->body_size = 1;
    else if (state->sized_body) {
        response->body = malloc(state->packet_length);
        assert(response->body);
        watch_allocation(response->body);
        memcpy(response->body, state->packet, state->packet_length);
        response->body_size = state->invalid_size ? SIZE_MAX : state->packet_length;
    }
    else if (request->on_body && response->status_code >= 200 && response->status_code < 300) {
        size_t offset = 0;
        while (offset < state->packet_length) {
            size_t length = state->packet_length - offset;
            if (state->body_fragment && length > state->body_fragment) length = state->body_fragment;
            cc_result_t rc = request->on_body(state->packet + offset, length, request->user_data);
            if (rc.code != CC_OK) return rc;
            cc_result_free(&rc);
            offset += length;
        }
    } else if (!state->empty_body) response->body = fixture_copy("password=do-not-print");
#ifdef CC_TEST_HTTP_LLM_OOM
    if (state->fail_empty_copy) { fail_malloc_bytes = 1; fail_malloc_once = 1; }
    if (state->fail_text_resize) fail_realloc_once = 1;
    if (state->report_metrics) {
        assert(request->on_attempt_metrics);
        cc_http_attempt_metrics_t metrics = {0};
        metrics.stage = "ok";
        metrics.first_body_ms = -1;
        cJSON_Hooks hooks = {metrics_allocate, free};
        metrics_nodes = 0; metrics_fault = 1;
        cJSON_InitHooks(&hooks);
        request->on_attempt_metrics(&metrics, request->attempt_metrics_user_data);
        cJSON_InitHooks(NULL);
        assert(!metrics_fault);
    }
#endif
    return success_result(state->owned_success);
}

/**
 * @brief 记录外层 HTTP 端口析构，证明 provider 仅借用它。
 * @param[in,out] self 有效 fixture 借用。
 * @post HTTP 析构计数增加一次。
 */
static void http_destroy(void *self) { ((fixture_state_t *)self)->http_destroys++; }

static const cc_http_client_vtable_t http_vtable = {http_perform, NULL, http_destroy};
static const cc_llm_protocol_vtable_t protocol_vtable = {
    protocol_name, protocol_build, protocol_parse, protocol_event, protocol_destroy
};
static const cc_llm_protocol_vtable_t no_stream_vtable = {
    protocol_name, protocol_build, protocol_parse, NULL, protocol_destroy
};

/**
 * @brief 创建使用独立栈状态的 provider，并保持 HTTP 端口由调用者拥有。
 * @param[in,out] state 有效 fixture，覆盖 provider 全部使用期。
 * @param[out] client 有效空 HTTP 槽，填入借用状态及静态 vtable。
 * @param[out] provider 有效空资源槽，成功取得 provider。
 * @param[in] stream_protocol 非零提供流事件解析回调。
 * @post 创建成功由断言确认，结果诊断已释放。
 */
static void make_provider(fixture_state_t *state, cc_http_client_t *client,
                            cc_llm_provider_t *provider, int stream_protocol)
{
    memset(client, 0, sizeof(*client));
    client->self = state;
    client->vtable = &http_vtable;
    cc_llm_protocol_t protocol = {state, stream_protocol ? &protocol_vtable : &no_stream_vtable};
    cc_result_t rc = cc_http_llm_provider_create(client, "base-fixture", "key-fixture-token",
                                               "model-fixture", protocol, provider);
    assert(rc.code == CC_OK);
    cc_result_free(&rc);
}

/**
 * @brief 收集流顺序，并允许在文本回调处返回取消。
 * @param[in] chunk 有效同步块借用。
 * @param[in,out] user_data 有效独占计数状态。
 * @return 文本故障时 CANCELLED，否则 CC_OK。
 */
static cc_result_t collect_chunk(const cc_stream_chunk_t *chunk, void *user_data)
{
    chunk_state_t *state = user_data;
    char marker = 0;
    if (chunk->type == CC_STREAM_CHUNK_TEXT) {
        state->text++;
        if (state->reject_text) return cc_result_error(CC_ERR_CANCELLED, "fixture callback cancelled");
        marker = 'T';
    } else if (chunk->type == CC_STREAM_CHUNK_USAGE) { state->usage++; marker = 'U'; }
    else if (chunk->type == CC_STREAM_CHUNK_FINISHED) { state->finished++; marker = 'F'; }
    if (marker && state->order_length + 1 < sizeof(state->order)) {
        state->order[state->order_length++] = marker;
        state->order[state->order_length] = '\0';
    }
    return success_result(state->owned_success);
}

/**
 * @brief 验证构造 OOM 只清理本次副本、成功才接管协议，且 HTTP 始终由外层拥有。
 * @post 所有 provider 销毁，错误结果释放，故障开关恢复。
 */
static void test_create_ownership(void)
{
#ifdef CC_TEST_HTTP_LLM_OOM
    const char *copies[] = {"base-fixture", "key-fixture-token", "model-fixture"};
    for (size_t i = 0; i < 4; i++) {
        fixture_state_t state = {0};
        cc_http_client_t client = {0}; client.self = &state; client.vtable = &http_vtable;
        cc_llm_protocol_t protocol = {&state, &protocol_vtable};
        cc_llm_provider_t provider = {0};
        if (!i) fail_calloc_once = 1;
        else { fail_malloc_bytes = strlen(copies[i - 1]) + 1; fail_malloc_once = 1; }
        cc_result_t rc = cc_http_llm_provider_create(&client, copies[0], copies[1], copies[2], protocol, &provider);
        assert(rc.code == CC_ERR_OUT_OF_MEMORY && !provider.self && !provider.vtable &&
               !state.protocol_destroys && !state.http_destroys);
        cc_result_free(&rc);
        assert(!fail_calloc_once && !fail_malloc_once);
        protocol_destroy(&state);
    }
#endif
    fixture_state_t state = {0};
    cc_http_client_t client = {0}; cc_llm_provider_t provider = {0};
    make_provider(&state, &client, &provider, 0);
    cc_llm_provider_capabilities_t caps;
    cc_result_t rc = provider.vtable->capabilities(provider.self, &caps);
    assert(rc.code == CC_OK && caps.size == sizeof(caps) && !caps.streaming && caps.text_input);
    cc_result_free(&rc);
    cc_llm_chat_request_t request = {0}; chunk_state_t chunks = {0};
    rc = provider.vtable->chat_stream(provider.self, &request, collect_chunk, &chunks);
    assert(rc.code == CC_ERR_PLATFORM);
    cc_result_free(&rc);
    check_released();
    provider.vtable->destroy(provider.self);
    assert(state.protocol_destroys == 1 && !state.http_destroys);
    cc_http_client_destroy(&client);
    assert(state.http_destroys == 1);
}

/**
 * @brief 验证请求构建/传输错误部分资源、空正文 OOM、NULL 输入和结构化 HTTP 错误。
 * @post 配置、请求及错误响应全部归还，协议只由成功 provider 最终销毁一次。
 */
static void test_sync_failure_cleanup(void)
{
    fixture_state_t state = {0}; cc_http_client_t client = {0}; cc_llm_provider_t provider = {0};
    make_provider(&state, &client, &provider, 1);
    cc_llm_chat_request_t request = {0}; cc_llm_response_t response = {0}; chunk_state_t chunks = {0};
    cc_result_t rc = provider.vtable->chat(provider.self, NULL, &response);
    assert(rc.code == CC_ERR_INVALID_ARGUMENT); cc_result_free(&rc);
    rc = provider.vtable->chat(provider.self, &request, NULL);
    assert(rc.code == CC_ERR_INVALID_ARGUMENT); cc_result_free(&rc);
    rc = provider.vtable->chat_stream(provider.self, &request, NULL, NULL);
    assert(rc.code == CC_ERR_INVALID_ARGUMENT); cc_result_free(&rc);
    state.build_error = 1;
#ifdef CC_TEST_HTTP_LLM_OOM
    size_t frees_before = watched_frees;
#endif
    rc = provider.vtable->chat(provider.self, &request, &response);
    assert(rc.code == CC_ERR_IO && !state.calls); cc_result_free(&rc); check_released();
#ifdef CC_TEST_HTTP_LLM_OOM
    assert(watched_frees == frees_before + 5);
#endif
    rc = provider.vtable->chat_stream(provider.self, &request, collect_chunk, &chunks);
    assert(rc.code == CC_ERR_IO && !state.calls); cc_result_free(&rc); check_released();
    state.build_error = 0; state.perform_error = 1;
    rc = provider.vtable->chat(provider.self, &request, &response);
    assert(rc.code == CC_ERR_NETWORK); cc_result_free(&rc); check_released();
    state.perform_error = 0; state.empty_body = 1;
#ifdef CC_TEST_HTTP_LLM_OOM
    state.fail_empty_copy = 1;
    rc = provider.vtable->chat(provider.self, &request, &response);
    assert(rc.code == CC_ERR_OUT_OF_MEMORY && !fail_malloc_once && !state.parses);
    cc_result_free(&rc); check_released();
    state.fail_empty_copy = 0;
#endif
    rc = provider.vtable->chat(provider.self, &request, &response);
    assert(rc.code == CC_OK && state.parses == 1); cc_result_free(&rc); check_released();
    state.empty_body = 0;
    const long statuses[] = {429, 503, 700};
    const char *retry[] = {" 3 ", "999999suffix", "999999999999"};
    const int waits[] = {3000, 0, 86400000};
    for (size_t i = 0; i < 3; i++) {
        state.status = statuses[i]; state.retry_after = retry[i];
        rc = provider.vtable->chat(provider.self, &request, &response);
        assert(rc.code != CC_OK && rc.detail && rc.detail->retry_after_ms == waits[i] &&
               rc.detail->recoverable == (i != 2) &&
               (!rc.message || !strstr(rc.message, "do-not-print")));
        cc_result_free(&rc); check_released();
    }
    provider.vtable->destroy(provider.self);
    assert(state.protocol_destroys == 1 && !state.http_destroys);
}

/**
 * @brief 验证显式长度正文补尾、内嵌 NUL、长度溢出及补尾 OOM 的清理。
 * @post 精确字节分配均经真实 provider 释放，HTTP 错误正文完整脱敏，协议销毁一次。
 */
static void test_sized_response_body(void)
{
    fixture_state_t state = {0}; cc_http_client_t client = {0}; cc_llm_provider_t provider = {0};
    make_provider(&state, &client, &provider, 1);
    cc_llm_chat_request_t request = {0}; cc_llm_response_t response = {0};
    const char valid[] = {'{', '}', ' '};
    state.sized_body = 1; state.packet = valid; state.packet_length = sizeof(valid);
    state.expected_response = "{} ";
    cc_result_t rc = provider.vtable->chat(provider.self, &request, &response);
    assert(rc.code == CC_OK && state.parses == 1); cc_result_free(&rc); check_released();
    const char raw_nul[] = {'{', '\0', '}'};
    state.packet = raw_nul; state.packet_length = sizeof(raw_nul);
    rc = provider.vtable->chat(provider.self, &request, &response);
    assert(rc.code == CC_ERR_JSON && state.parses == 1); cc_result_free(&rc); check_released();
    state.missing_body = 1;
    rc = provider.vtable->chat(provider.self, &request, &response);
    assert(rc.code == CC_ERR_JSON && state.parses == 1); cc_result_free(&rc); check_released();
    state.missing_body = 0; state.packet = valid; state.packet_length = sizeof(valid); state.invalid_size = 1;
    rc = provider.vtable->chat(provider.self, &request, &response);
    assert(rc.code == CC_ERR_LIMIT_EXCEEDED && state.parses == 1); cc_result_free(&rc); check_released();
    state.invalid_size = 0;
#ifdef CC_TEST_HTTP_LLM_OOM
    state.fail_text_resize = 1;
    rc = provider.vtable->chat(provider.self, &request, &response);
    assert(rc.code == CC_ERR_OUT_OF_MEMORY && !fail_realloc_once && state.parses == 1);
    cc_result_free(&rc); check_released(); state.fail_text_resize = 0;
#endif
    const char error_body[] = "failure\0password=do-not-print";
    state.packet = error_body; state.packet_length = sizeof(error_body) - 1; state.status = 503;
    rc = provider.vtable->chat(provider.self, &request, &response);
    assert(rc.code == CC_ERR_NETWORK && rc.message && strstr(rc.message, "failure") &&
           !strstr(rc.message, "do-not-print"));
    cc_result_free(&rc); check_released();
    state.stream_kind = CC_LLM_STREAM_SSE;
    chunk_state_t chunks = {0};
    rc = provider.vtable->chat_stream(provider.self, &request, collect_chunk, &chunks);
    assert(rc.code == CC_ERR_NETWORK && !chunks.finished && rc.message && !strstr(rc.message, "do-not-print"));
    cc_result_free(&rc); check_released();
    provider.vtable->destroy(provider.self);
    assert(state.protocol_destroys == 1 && !state.http_destroys);
}

/**
 * @brief 验证被覆盖的成功诊断及指标键分配失败时的独立 JSON 值归还。
 * @post cJSON 全局钩子恢复默认，默认同步总线与 provider 均销毁，跟踪地址全部释放。
 */
static void test_metrics_and_success_messages(void)
{
    cc_event_bus_t *bus = NULL;
    cc_result_t rc = cc_event_bus_create(&bus);
    assert(rc.code == CC_OK && bus); cc_result_free(&rc);
    fixture_state_t state = {0}; state.owned_success = 1;
    cc_http_client_t client = {0}; cc_llm_provider_t provider = {0};
    make_provider(&state, &client, &provider, 1);
    cc_llm_chat_request_t request = {0}; request.event_bus = bus;
    cc_llm_response_t response = {0};
#ifdef CC_TEST_HTTP_LLM_OOM
    state.report_metrics = 1;
#endif
    rc = provider.vtable->chat(provider.self, &request, &response);
    assert(rc.code == CC_OK && state.parses == 1); cc_result_free(&rc); check_released();
    state.report_metrics = 0;
    state.stream_kind = CC_LLM_STREAM_SSE; state.defer_finished = 1;
    state.packet = "data: text\n\ndata: finish\n\ndata: usage\n\n";
    state.packet_length = strlen(state.packet);
    chunk_state_t chunks = {0}; chunks.owned_success = 1;
    rc = provider.vtable->chat_stream(provider.self, &request, collect_chunk, &chunks);
    assert(rc.code == CC_OK && strcmp(chunks.order, "TUF") == 0);
    cc_result_free(&rc); check_released();
    provider.vtable->destroy(provider.self);
    assert(state.protocol_destroys == 1 && !state.http_destroys);
    cc_event_bus_destroy(bus);
}

/**
 * @brief 对一组字节运行真实 SSE/NDJSON 流入口并检查块数量和完成顺序。
 * @param[in] packet 在同步调用期间借用的字节。
 * @param[in] length 包含原始 NUL 的明确长度也可输入，用于错误对照。
 * @param[in] kind 选择 SSE 或 NDJSON。
 * @param[in] fragment HTTP body 切块字节数，零表示一次交付全部。
 * @param[in] defer 非零保留语义完成后的 usage 阶段。
 * @param[in] expected 预期返回码。
 * @param[in] text_count 预期文本块数。
 * @param[in] order 可选精确顺序字符串，NULL 时仅检查数量。
 * @param[in] reject_text 非零在首个文本块返回取消。
 * @post 请求和响应资源全部清理，provider 及协议销毁。
 */
static void check_stream(const char *packet, size_t length, cc_llm_stream_kind_t kind,
                           size_t fragment, int defer, cc_error_code_t expected,
                           int text_count, const char *order, int reject_text)
{
    fixture_state_t state = {0}; state.packet = packet; state.packet_length = length;
    state.stream_kind = kind; state.body_fragment = fragment; state.defer_finished = defer;
    cc_http_client_t client = {0}; cc_llm_provider_t provider = {0};
    make_provider(&state, &client, &provider, 1);
    cc_llm_chat_request_t request = {0}; chunk_state_t chunks = {0}; chunks.reject_text = reject_text;
    cc_result_t rc = provider.vtable->chat_stream(provider.self, &request, collect_chunk, &chunks);
    assert(rc.code == expected && chunks.text == text_count);
    if (order) assert(strcmp(chunks.order, order) == 0);
    if (expected == CC_OK) assert(chunks.finished == 1);
    else assert(chunks.finished == 0);
    cc_result_free(&rc); check_released();
    provider.vtable->destroy(provider.self);
    assert(state.protocol_destroys == 1 && !state.http_destroys);
}

/**
 * @brief 检查任意分块、CR/LF、延迟 usage、重复完成和未成帧预算。
 * @post 大块 fixture 缓冲释放，所有请求和协议已销毁。
 */
static void test_stream_framing(void)
{
    const char *cr = "data: text\revent: ignored\r\rdata: finish\r\r";
    check_stream(cr, strlen(cr), CC_LLM_STREAM_SSE, 0, 0, CC_OK, 1, "TF", 0);
    const char *mixed = "data: text\r\n\r\ndata: finish\n\n";
    check_stream(mixed, strlen(mixed), CC_LLM_STREAM_SSE, 1, 0, CC_OK, 1, "TF", 0);
    const char *late = "data: text\n\ndata: directfinish\n\ndata: usage\n\ndata: [DONE]\n\n";
    check_stream(late, strlen(late), CC_LLM_STREAM_SSE, 0, 1, CC_OK, 1, "TUF", 0);
    const char *eof = "data: text\n\ndata: finish\n\ndata: usage\n\n";
    check_stream(eof, strlen(eof), CC_LLM_STREAM_SSE, 0, 1, CC_OK, 1, "TUF", 0);
    const char *duplicate = "data: directfinish\n\n";
    check_stream(duplicate, strlen(duplicate), CC_LLM_STREAM_SSE, 0, 0, CC_OK, 0, "F", 0);
    const char *after = "data: text\n\ndata: finish\n\ndata: text\n\n";
    check_stream(after, strlen(after), CC_LLM_STREAM_SSE, 0, 1, CC_ERR_JSON, 1, "T", 0);
    const char *ndjson = "text\nfinish";
    check_stream(ndjson, strlen(ndjson), CC_LLM_STREAM_NDJSON, 2, 0, CC_OK, 1, "TF", 0);
    check_stream(mixed, strlen(mixed), CC_LLM_STREAM_SSE, 0, 0, CC_ERR_CANCELLED, 1, "", 1);
    const char raw_nul[] = "data: text\0\n\n";
    check_stream(raw_nul, sizeof(raw_nul) - 1, CC_LLM_STREAM_SSE, 0, 0, CC_ERR_JSON, 0, "", 0);
    const char *event = "data: text\n\n", *finish = "data: finish\n\n";
    size_t event_bytes = strlen(event), finish_bytes = strlen(finish), repeats = 10000;
    size_t bytes = repeats * event_bytes + finish_bytes;
    char *large = malloc(bytes);
    assert(large);
    for (size_t i = 0; i < repeats; i++) memcpy(large + i * event_bytes, event, event_bytes);
    memcpy(large + repeats * event_bytes, finish, finish_bytes);
    check_stream(large, bytes, CC_LLM_STREAM_SSE, 0, 0, CC_OK, (int)repeats, NULL, 0);
    free(large);
    bytes = 64U * 1024U;
    large = malloc(bytes);
    assert(large); memset(large, 'x', bytes);
    check_stream(large, bytes, CC_LLM_STREAM_SSE, 0, 0, CC_ERR_LIMIT_EXCEEDED, 0, "", 0);
    free(large);
}

/**
 * @brief 执行 HTTP provider 的拥有权、失败清理和流式契约回归。
 * @return 全部断言通过返回零，失败由断言终止。
 */
int main(void)
{
    test_create_ownership();
    test_sync_failure_cleanup();
    test_sized_response_body();
    test_metrics_and_success_messages();
    test_stream_framing();
    check_released();
    return 0;
}
