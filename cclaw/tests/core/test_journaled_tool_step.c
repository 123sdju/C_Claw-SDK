#include "../../core/src/app/cc_agent_runtime_internal.h"
#include "cc/core/cc_id.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_memory_tool_factory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 创建由夹具主线程持有的内存会话端口。
 * @param[out] out_store 新输出槽，成功取得实例并以 vtable.destroy 回收。
 * @return 构造状态及拥有诊断，由调用者消费。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "check failed at line %d\n", __LINE__); return 1; } } while (0)
/**
 * @brief 检查状态、打印失败诊断并消费结果。
 * @param[in] rc 按值移交的拥有结果。
 * @return 成功一，失败零，动态字段在返回前释放。
 */
static int ok(cc_result_t rc) { int good = rc.code == CC_OK; if (!good) fprintf(stderr, "%s\n", rc.message ? rc.message : "error"); cc_result_free(&rc); return good; }

typedef struct fake_tool {
    const char *name;
    int calls;
    int fail;
    int saw_operation_id;
    int name_calls;
    int description_calls;
    int schema_calls;
} fake_tool_t;

/**
 * @brief 统计名称钩子访问并返回借用名称。
 * @param[in,out] self 存活的独占 fake_tool_t 借用，测试串行调用。
 * @return 工具状态中的名称借用，无分配，保持到注册表销毁。
 */
static const char *tool_name(void *self) { fake_tool_t *tool = self; tool->name_calls++; return tool->name; }
/**
 * @brief 统计描述钩子访问。
 * @param[in,out] self 测试串行使用的 fake_tool_t 借用。
 * @return 常驻描述借用，无拥有权转移。
 */
static const char *tool_description(void *self) { ((fake_tool_t *)self)->description_calls++; return "test tool"; }
/**
 * @brief 统计 schema 钩子访问。
 * @param[in,out] self 测试串行使用的 fake_tool_t 借用。
 * @return 常驻 JSON schema 借用，无分配。
 */
static const char *tool_schema(void *self) { ((fake_tool_t *)self)->schema_calls++; return "{}"; }
/**
 * @brief 记录派发与幂等键，模拟成功、结果失败或派发后取消。
 * @param[in,out] self 存活的工具状态借用，串行更新计数。
 * @param[in] args 未使用参数文本借用。
 * @param[in] ctx 本次上下文借用，检查其 operation ID 前缀，不保留。
 * @param[out] result 已初始化独占结果，文本副本交调用者 cleanup。
 * @return 系统取消错误或结果构造成功，文本 OOM 返回内存错误，诊断由调用者消费。
 */
static cc_result_t tool_call(void *self, const char *args, const cc_tool_context_t *ctx,
                             cc_tool_result_t *result)
{
    (void)args;
    fake_tool_t *tool = self;
    tool->calls++;
    tool->saw_operation_id = ctx->idempotency_key &&
        strncmp(ctx->idempotency_key, "op-", 3) == 0;
    if (tool->fail == 2) return cc_result_error(CC_ERR_CANCELLED, "device call cancelled");
    result->ok = !tool->fail;
    if (tool->fail) result->error = cc_copy_string("device response lost");
    else result->text = cc_copy_string("done");
    if (!result->error && !result->text)
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot create fake tool result");
    return cc_result_ok();
}
static const cc_tool_vtable_t tool_vtable = {
    tool_name, tool_description, tool_schema, tool_call, NULL
};
static cc_cancel_source_t *cancel_during_policy;
/**
 * @brief 允许策略检查并按全局夹具开关在派发前取消。
 * @param[in,out] self 可 NULL 的策略调用计数借用，测试串行更新。
 * @param[in] call 未使用调用借用。
 * @param[in] ctx 未使用工具上下文借用。
 * @param[out] decision 独占决策槽，清零并允许，无拥有字符串。
 * @return 成功结果，全局取消源由主线程在同步策略返回后销毁。
 */
static cc_result_t allow(void *self, const cc_tool_call_t *call, const cc_tool_context_t *ctx,
                         cc_policy_decision_t *decision)
{
    if (self) (*(int *)self)++;
    (void)call; (void)ctx;
    memset(decision, 0, sizeof(*decision));
    decision->allowed = 1;
    if (cancel_during_policy) cc_cancel_source_cancel(cancel_during_policy);
    return cc_result_ok();
}
static const cc_policy_engine_vtable_t policy_vtable = {allow, NULL};

typedef struct fake_memory { int calls; int value_installed; } fake_memory_t;
/**
 * @brief 模拟值已安装而同步返回失败的记忆写入。
 * @param[in,out] self 串行使用的 fake_memory_t 借用，更新计数及安装标志。
 * @param[in] key 未使用键名借用。
 * @param[in] value 未使用正文借用。
 * @param[in] category 未使用分类借用。
 * @param[in] session_id 未使用会话借用。
 * @return 拥有 I/O 错误诊断，调用者消费，状态标志保留模拟已发生的副作用。
 */
static cc_result_t memory_set_then_fail(void *self, const char *key, const char *value,
                                        const char *category, const char *session_id)
{
    (void)key; (void)value; (void)category; (void)session_id;
    fake_memory_t *memory = self;
    memory->calls++;
    memory->value_installed = 1; /* 模拟提交完成后返回的歧义错误。 */
    return cc_result_error(CC_ERR_IO, "memory sync failed after installation");
}
static const cc_memory_store_vtable_t fake_memory_vtable = {.set = memory_set_then_fail};

typedef struct session_proxy {
    cc_session_store_t base;
    int fail_results;
} session_proxy_t;
/**
 * @brief 转发会话创建到借用的真实内存后端。
 * @param[in,out] self 存活的 session_proxy_t 借用，base 由主线程拥有。
 * @param[in] id 同步会话 ID 借用。
 * @param[in] workspace 同步工作区借用。
 * @return 下层拥有结果原样交调用者消费，不转移 base 拥有权。
 */
static cc_result_t proxy_create(void *self, const char *id, const char *workspace)
{ session_proxy_t *s = self; return s->base.vtable->create_session(s->base.self, id, workspace); }
/**
 * @brief 按夹具开关拒绝结果批次或转发真实追加。
 * @param[in,out] self 串行更新故障开关的代理借用。
 * @param[in] records 调用期只读批次借用，下层复制字段。
 * @param[in] count 批次数，故障开启时三条及以上直接失败。
 * @return 注入的存储错误或真实后端结果，诊断交调用者，不保留输入。
 */
static cc_result_t proxy_append(void *self, const cc_session_record_t *records, size_t count)
{
    session_proxy_t *s = self;
    if (s->fail_results && count >= 3) return cc_result_error(CC_ERR_STORAGE, "injected result write failure");
    return s->base.vtable->append_records(s->base.self, records, count);
}
/**
 * @brief 转发消息加载并交付下层拥有数组。
 * @param[in,out] self 存活代理借用。
 * @param[in] id 调用期会话 ID 借用。
 * @param[in] limit 下层解释的条数上限。
 * @param[out] messages 新拥有数组槽，成功逐项 cleanup 后 free。
 * @param[out] count 成功消息数槽。
 * @return 下层结果，诊断由调用者消费，base 生命周期由主线程维持。
 */
static cc_result_t proxy_load(void *self, const char *id, int limit, cc_message_t **messages, size_t *count)
{ session_proxy_t *s = self; return s->base.vtable->load_messages(s->base.self, id, limit, messages, count); }
/**
 * @brief 转发会话枚举。
 * @param[in,out] self 存活代理借用，不拥有 base。
 * @param[out] sessions 新拥有数组槽，成功以 cc_session_free_array 回收。
 * @param[out] count 成功条数槽。
 * @return 下层拥有结果原样交付，代理不缓存输出。
 */
static cc_result_t proxy_list(void *self, cc_session_t **sessions, size_t *count)
{ session_proxy_t *s = self; return s->base.vtable->list_sessions(s->base.self, sessions, count); }
/**
 * @brief 转发会话清除。
 * @param[in,out] self 存活代理借用，调用由测试串行驱动。
 * @param[in] id 调用期会话 ID 借用。
 * @return 下层拥有结果原样交付，不保留 ID。
 */
static cc_result_t proxy_clear(void *self, const char *id)
{ session_proxy_t *s = self; return s->base.vtable->clear_session(s->base.self, id); }
static const cc_session_store_vtable_t proxy_vtable = {
    proxy_create, proxy_append, proxy_load, proxy_list, proxy_clear, NULL
};
typedef struct fake_chat_state { int requests; int saw_tools; int send_tool_call; } fake_chat_state_t;
/**
 * @brief 记录 provider 是否收到 tools，并按夹具模式返回文本或意外调用。
 * @param[in,out] self 可 NULL 的串行 fake_chat_state_t 借用。
 * @param[in] request 调用期请求借用，只检查 tools_json。
 * @param[out] response 已初始化独占响应，取得文本或调用副本并由 runtime 清理。
 * @return 真实响应构造状态与拥有诊断，不保留请求借用。
 */
static cc_result_t fake_chat(void *self, const cc_llm_chat_request_t *request, cc_llm_response_t *response)
{
    fake_chat_state_t *state = self;
    if (state) {
        state->requests++;
        if (request->tools_json) state->saw_tools++;
        if (state->send_tool_call) {
            response->finished = 1;
            return cc_llm_response_add_tool_call(response, "unexpected", "write", "{}");
        }
    }
    cc_result_t rc = cc_llm_response_set_text(response, "continued");
    response->finished = 1;
    return rc;
}
static const cc_llm_provider_vtable_t fake_llm_vtable = {fake_chat, NULL, NULL, NULL};
typedef struct stream_observer { int tool_chunks; int errors; } stream_observer_t;
/**
 * @brief 统计工具流块及错误块以检查降级时工具出口关闭。
 * @param[in] chunk 同步回调期块借用，不保留正文。
 * @param[in,out] self 独占观察计数借用，保持到流调用退出。
 * @return 无拥有字段的成功结果，调用者消费。
 */
static cc_result_t observe_stream(const cc_stream_chunk_t *chunk, void *self)
{
    stream_observer_t *observer=self;
    if (chunk->type==CC_STREAM_CHUNK_TOOL_START || chunk->type==CC_STREAM_CHUNK_TOOL_DELTA ||
        chunk->type==CC_STREAM_CHUNK_TOOL_END) observer->tool_chunks++;
    if (chunk->type==CC_STREAM_CHUNK_ERROR) observer->errors++;
    return cc_result_ok();
}

/**
 * @brief 验证普通准入满额时只读入口、取消后 UNKNOWN 及重开后的同名阻断。
 * @param[in,out] fs 借用的真实文件端口，主函数维持到测试退出。
 * @param[in] root 当前独占测试目录借用，内部创建 readonly 子目录。
 * @return 全部断言通过零，失败一，成功路径排空 runtime 并清理账本记录、端口及子目录。
 * @note 工具状态在本函数栈内，registry/runtime 销毁完成后才退出，夹具串行运行。
 */
static int test_readonly_at_pending_limit(cc_filesystem_t *fs, const char *root)
{
    char directory[512]; snprintf(directory,sizeof(directory),"%s/readonly",root);
    CHECK(ok(fs->vtable->make_dir(fs->self,directory)));
    cc_operation_journal_t *journal=NULL;
    CHECK(ok(cc_operation_journal_open(fs,directory,1,&journal)));
    cc_tool_call_t pending_call={"pending","write","{}"};
    char *pending_id=NULL;
    CHECK(ok(cc_operation_journal_import_unknown(journal,"s1",&pending_call,&pending_id)));
    cc_session_store_t sessions={0};
    CHECK(ok(cc_memory_session_store_create(&sessions)) &&
          ok(sessions.vtable->create_session(sessions.self,"s2",".")));
    fake_tool_t read={"read",0,0,0}, unmarked={"unmarked",0,0,0};
    cc_tool_t read_tool={&read,&tool_vtable}, unmarked_tool={&unmarked,&tool_vtable};
    cc_tool_registry_t *registry=NULL;
    CHECK(ok(cc_tool_registry_create(&registry)) &&
          ok(cc_tool_registry_add_with_capabilities(registry,read_tool,CC_TOOL_CAPABILITY_READ_ONLY)) &&
          ok(cc_tool_registry_add(registry,unmarked_tool)) && ok(cc_tool_registry_freeze(registry)));
    cc_tool_t found={0}; unsigned capabilities=0;
    CHECK(ok(cc_tool_registry_find_with_capabilities(registry,"read",&found,&capabilities)) &&
          capabilities==CC_TOOL_CAPABILITY_READ_ONLY && found.self==&read);
    cc_agent_runtime_deps_t deps={0}; deps.size=sizeof(deps);
    deps.tool_registry=registry; deps.store=sessions; deps.policy.vtable=&policy_vtable;
    deps.operation_journal=journal; deps.llm.vtable=&fake_llm_vtable;
    cc_agent_runtime_options_t options={0}; options.config.workspace_dir="."; options.config.max_steps=1;
    cc_agent_runtime_t *runtime=NULL; CHECK(ok(cc_agent_runtime_create(&deps,&options,&runtime)));

    cc_tool_call_t call={"unmarked-1","unmarked","{}"};
    CHECK(ok(cc_agent_runtime_execute_tool_step(runtime,"s2",&call,NULL,NULL,0)) && unmarked.calls==0);
    call=(cc_tool_call_t){"read-1","read","{}"};
    CHECK(ok(cc_agent_runtime_execute_tool_step(runtime,"s2",&call,NULL,NULL,0)) &&
          read.calls==1 && read.saw_operation_id);
    cc_pending_operation_t *items=NULL; size_t count=0;
    CHECK(ok(cc_operation_journal_list(journal,&items,&count)) && count==1);
    cc_pending_operation_array_free(items,count);
    read.fail=1; call.id="read-2";
    CHECK(ok(cc_agent_runtime_execute_tool_step(runtime,"s2",&call,NULL,NULL,0)) && read.calls==2);
    CHECK(ok(cc_operation_journal_list(journal,&items,&count)) && count==1);
    cc_pending_operation_array_free(items,count);
    read.fail=2; call.id="read-3";
    cc_result_t rc=cc_agent_runtime_execute_tool_step(runtime,"s2",&call,NULL,NULL,0);
    CHECK(rc.code==CC_ERR_CANCELLED && read.calls==3); cc_result_free(&rc);
    call.id="read-4";
    CHECK(ok(cc_agent_runtime_execute_tool_step(runtime,"s2",&call,NULL,NULL,0)) && read.calls==3);
    CHECK(ok(cc_operation_journal_list(journal,&items,&count)) && count==2);
    cc_pending_operation_array_free(items,count);
    cc_agent_runtime_destroy(runtime); cc_tool_registry_destroy(registry);
    sessions.vtable->destroy(sessions.self);
    cc_operation_journal_destroy(journal); journal=NULL;

    CHECK(ok(cc_operation_journal_open(fs,directory,1,&journal)));
    int blocked=0;
    CHECK(ok(cc_operation_journal_is_blocked(journal,"read",&blocked)) && blocked);
    char *again=NULL; call.id="read-5";
    rc=cc_operation_journal_begin_readonly(journal,"s2",&call,&again);
    CHECK(rc.code==CC_ERR_INVALID_STATE && !again); cc_result_free(&rc);
    CHECK(ok(cc_operation_journal_list(journal,&items,&count)) && count==2);
    for (size_t i=0;i<count;i++) CHECK(ok(cc_operation_journal_finish(journal,items[i].operation_id)));
    cc_pending_operation_array_free(items,count);
    cc_operation_journal_destroy(journal); free(pending_id);
    CHECK(ok(fs->vtable->remove(fs->self,directory)));
    return 0;
}

/**
 * @brief 验证派发意图、结果未知、跨会话阻断、人工核对和必需账本缺失时的文本降级。
 * @return 所有真实 runtime/账本断言通过零，失败一，正常路径消费结果并回收全部实例和目录。
 * @note 代理借用内存后端，fake 工具/策略状态覆盖 runtime 寿命，清理按 runtime、registry、store、journal、fs 顺序。
 */
int main(void)
{
    cc_filesystem_t fs = {0};
    CHECK(ok(cc_filesystem_get_default(&fs)));
    char *directory = NULL;
    CHECK(ok(cc_id_generate_uuid_v4("journal-step-test-", &directory)));
    CHECK(ok(fs.vtable->make_dir(fs.self, directory)));
    CHECK(test_readonly_at_pending_limit(&fs,directory)==0);
    cc_operation_journal_t *journal = NULL;
    CHECK(ok(cc_operation_journal_open(&fs, directory, 0, &journal)));
    cc_session_store_t sessions = {0};
    CHECK(ok(cc_memory_session_store_create(&sessions)));
    CHECK(ok(sessions.vtable->create_session(sessions.self, "s1", ".")));
    CHECK(ok(sessions.vtable->create_session(sessions.self, "s2", ".")));
    CHECK(ok(sessions.vtable->create_session(sessions.self, "s3", ".")));
    CHECK(ok(sessions.vtable->create_session(sessions.self, "s4", ".")));
    fake_tool_t first = {"write", 0, 0, 0};
    fake_tool_t other = {"other", 0, 0, 0};
    fake_memory_t memory_state = {0};
    cc_memory_store_t memory_store = {&memory_state, &fake_memory_vtable};
    cc_tool_registry_t *registry = NULL;
    CHECK(ok(cc_tool_registry_create(&registry)));
    cc_tool_t tool1 = {&first, &tool_vtable}, tool2 = {&other, &tool_vtable};
    CHECK(ok(cc_tool_registry_add(registry, tool1)));
    CHECK(ok(cc_tool_registry_add(registry, tool2)));
    cc_tool_t memory_tool = {0};
    CHECK(ok(cc_memory_tool_create(&memory_store, &memory_tool)));
    CHECK(ok(cc_tool_registry_add(registry, memory_tool)));
    CHECK(ok(cc_tool_registry_freeze(registry)));
    cc_agent_runtime_deps_t deps = {0};
    session_proxy_t proxy = {0};
    proxy.base = sessions;
    deps.size = sizeof(deps);
    deps.tool_registry = registry;
    deps.store.self = &proxy;
    deps.store.vtable = &proxy_vtable;
    deps.policy.vtable = &policy_vtable;
    deps.operation_journal = journal;
    deps.llm.vtable = &fake_llm_vtable;
    cc_agent_runtime_options_t options = {0};
    options.config.workspace_dir = ".";
    options.config.max_steps = 1;
    cc_agent_runtime_t *runtime = NULL;
    CHECK(ok(cc_agent_runtime_create(&deps, &options, &runtime)));
    cc_tool_call_t call = {"call-1", "write", "{}"};
    cc_tool_result_t direct = {0};
    cc_result_t rc = cc_tool_executor_execute(runtime, "s1", &call, &direct);
    CHECK(rc.code == CC_ERR_UNSUPPORTED && first.calls == 0);
    cc_result_free(&rc);
    cc_tool_result_cleanup(&direct);

    CHECK(ok(cc_cancel_source_create(&cancel_during_policy)));
    call.id = "cancel-before-dispatch";
    rc = cc_agent_runtime_execute_tool_step(runtime, "s1", &call, NULL,
        cc_cancel_source_token(cancel_during_policy), 0);
    CHECK(rc.code == CC_ERR_CANCELLED && first.calls == 0);
    cc_result_free(&rc);
    cc_cancel_source_destroy(cancel_during_policy);
    cancel_during_policy = NULL;
    cc_pending_operation_t *cancelled_items = NULL;
    size_t cancelled_count = 0;
    CHECK(ok(cc_operation_journal_list(journal, &cancelled_items, &cancelled_count)) && cancelled_count == 0);
    cc_pending_operation_array_free(cancelled_items, cancelled_count);
    cc_message_t *cancelled_history = NULL;
    size_t cancelled_history_count = 0;
    CHECK(ok(sessions.vtable->load_messages(sessions.self,"s1",-1,&cancelled_history,&cancelled_history_count)));
    int saw_non_execution = 0;
    for (size_t i=0;i<cancelled_history_count;i++) {
        cc_message_t *message=&cancelled_history[i];
        if (message->role==CC_ROLE_TOOL && message->tool_call_id &&
            strcmp(message->tool_call_id,"cancel-before-dispatch")==0) {
            CHECK(!cc_message_tool_outcome_is_unknown(message));
            saw_non_execution = 1;
        }
        cc_message_cleanup(message);
    }
    free(cancelled_history);
    CHECK(saw_non_execution);
    call.id = "call-1";

    CHECK(ok(cc_agent_runtime_execute_tool_step(runtime, "s1", &call, NULL, NULL, 0)));
    CHECK(first.calls == 1 && first.saw_operation_id);
    cc_pending_operation_t *items = NULL;
    size_t count = 0;
    CHECK(ok(cc_operation_journal_list(journal, &items, &count)) && count == 0);
    cc_pending_operation_array_free(items, count);

    first.fail = 1;
    call.id = "call-2";
    CHECK(ok(cc_agent_runtime_execute_tool_step(runtime, "s1", &call, NULL, NULL, 0)));
    CHECK(first.calls == 2);
    CHECK(ok(cc_operation_journal_list(journal, &items, &count)) && count == 1);
    CHECK(!items[0].in_progress && strcmp(items[0].session_id, "s1") == 0);
    cc_tool_call_t blocked_call = {"call-3", "write", "{}"};
    CHECK(ok(cc_agent_runtime_execute_tool_step(runtime, "s2", &blocked_call, NULL, NULL, 0)));
    CHECK(first.calls == 2);
    cc_tool_call_t other_call = {"call-4", "other", "{}"};
    CHECK(ok(cc_agent_runtime_execute_tool_step(runtime, "s2", &other_call, NULL, NULL, 0)));
    CHECK(other.calls == 1 && other.saw_operation_id);
    cc_tool_call_t pending = {0};
    CHECK(ok(cc_agent_runtime_get_pending_tool_call(runtime, "s1", "call-2", &pending)));
    CHECK(strcmp(pending.name, "write") == 0);
    cc_tool_call_cleanup(&pending);
    CHECK(ok(cc_agent_runtime_resolve_pending_tool(runtime, "s1", "call-2",
        CC_TOOL_RECOVERY_NOT_EXECUTED, NULL)));
    cc_pending_operation_array_free(items, count);
    CHECK(first.calls == 2);
    int is_blocked = 1;
    CHECK(ok(cc_operation_journal_is_blocked(journal, "write", &is_blocked)) && !is_blocked);
    rc = cc_agent_runtime_get_pending_tool_call(runtime, "s1", "call-2", &pending);
    CHECK(rc.code == CC_ERR_NOT_FOUND);
    cc_result_free(&rc);

    /* 派发后系统错误保留未知状态，返回取消前持久化显式占位。 */
    first.fail = 2;
    call.id = "call-5";
    rc = cc_agent_runtime_execute_tool_step(runtime, "s1", &call, NULL, NULL, 0);
    CHECK(rc.code == CC_ERR_CANCELLED && first.calls == 3);
    cc_result_free(&rc);
    CHECK(ok(cc_agent_runtime_get_pending_tool_call(runtime, "s1", "call-5", &pending)));
    cc_tool_call_cleanup(&pending);
    cc_tool_result_t observed = {0};
    observed.ok = 1;
    observed.text = "verified on device";
    /* 账本清理未完成而重试时，当前格式人工核对消息继续约束结论。 */
    items = NULL; count = 0;
    CHECK(ok(cc_operation_journal_list(journal, &items, &count)) && count == 1);
    char existing_note_text[512];
    snprintf(existing_note_text, sizeof(existing_note_text),
        "Operator reconciliation for operation %s, tool write: executed. Observed result: verified on device",
        items[0].operation_id);
    cc_pending_operation_array_free(items, count);
    cc_message_t *existing_note = NULL;
    CHECK(ok(cc_message_create_text("existing-note", "s1", CC_ROLE_USER, existing_note_text, "call-5", &existing_note)));
    existing_note->tool_outcome = CC_MESSAGE_TOOL_OUTCOME_RECONCILED_EXECUTED;
    cc_session_record_t note_record = {0};
    note_record.type = CC_SESSION_RECORD_MESSAGE; note_record.session_id = "s1";
    note_record.data.message = existing_note;
    CHECK(ok(sessions.vtable->append_records(sessions.self, &note_record, 1)));
    cc_message_destroy(existing_note);
    rc = cc_agent_runtime_resolve_pending_tool(runtime, "s1", "call-5",
        CC_TOOL_RECOVERY_NOT_EXECUTED, NULL);
    CHECK(rc.code == CC_ERR_INVALID_STATE); cc_result_free(&rc);
    CHECK(ok(cc_agent_runtime_resolve_pending_tool(runtime, "s1", "call-5",
        CC_TOOL_RECOVERY_EXECUTED, &observed)));
    CHECK(first.calls == 3);

    /* 动作完成而结果写入失败时保留账本操作，同会话仍可处理其他轮次。 */
    proxy.fail_results = 1;
    cc_tool_call_t write_failure = {"call-6", "other", "{}"};
    rc = cc_agent_runtime_execute_tool_step(runtime, "s2", &write_failure, NULL, NULL, 0);
    CHECK(rc.code == CC_ERR_STORAGE && other.calls == 2);
    cc_result_free(&rc);
    proxy.fail_results = 0;
    CHECK(ok(cc_agent_runtime_get_pending_tool_call(runtime, "s2", "call-6", &pending)));
    cc_tool_call_cleanup(&pending);
    char *answer = NULL;
    CHECK(ok(cc_agent_runtime_handle_message(runtime, "s2", "continue with another request", &answer)));
    CHECK(answer && strcmp(answer, "continued") == 0);
    free(answer);
    CHECK(ok(cc_operation_journal_is_blocked(journal, "other", &is_blocked)) && is_blocked);
    CHECK(ok(cc_agent_runtime_resolve_pending_tool(runtime, "s2", "call-6",
        CC_TOOL_RECOVERY_EXECUTED, &observed)));
    cc_message_t *history = NULL; size_t history_count = 0; int tagged = 0;
    CHECK(ok(sessions.vtable->load_messages(sessions.self, "s2", -1, &history, &history_count)));
    for (size_t i = 0; i < history_count; i++) {
        if (history[i].role == CC_ROLE_USER && history[i].tool_call_id &&
            strcmp(history[i].tool_call_id, "call-6") == 0 &&
            history[i].tool_outcome == CC_MESSAGE_TOOL_OUTCOME_RECONCILED_EXECUTED) tagged++;
        cc_message_cleanup(&history[i]);
    }
    free(history); CHECK(tagged == 1);
    CHECK(ok(cc_operation_journal_is_blocked(journal, "other", &is_blocked)) && !is_blocked);
    CHECK(other.calls == 2);

    /* 记忆同步失败前可能已安装，保留待核对状态并跨会话阻断同名工具。 */
    cc_tool_call_t memory_call = {"memory-1", "memory",
        "{\"operation\":\"store\",\"key\":\"k\",\"value\":\"v\"}"};
    rc = cc_agent_runtime_execute_tool_step(runtime, "s1", &memory_call, NULL, NULL, 0);
    if (rc.code != CC_OK || memory_state.calls != 1 || !memory_state.value_installed)
        fprintf(stderr, "memory step: code=%d calls=%d installed=%d message=%s\n",
                (int)rc.code, memory_state.calls, memory_state.value_installed,
                rc.message ? rc.message : "(none)");
    CHECK(rc.code == CC_OK && memory_state.calls == 1 && memory_state.value_installed);
    cc_result_free(&rc);
    CHECK(ok(cc_operation_journal_is_blocked(journal, "memory", &is_blocked)) && is_blocked);
    memory_call.id = "memory-2";
    CHECK(ok(cc_agent_runtime_execute_tool_step(runtime, "s2", &memory_call, NULL, NULL, 0)));
    CHECK(memory_state.calls == 1);
    CHECK(ok(cc_agent_runtime_resolve_pending_tool(runtime, "s1", "memory-1",
        CC_TOOL_RECOVERY_EXECUTED, &observed)));
    CHECK(ok(cc_operation_journal_is_blocked(journal, "memory", &is_blocked)) && !is_blocked);

    first.fail = 0;
    call.id = "manual-call";
    cc_tool_result_t manual = {0};
    CHECK(ok(cc_agent_runtime_execute_manual_tool(runtime, "s1", &call, &manual)));
    CHECK(first.calls == 4 && manual.ok && manual.text && strcmp(manual.text, "done") == 0);
    cc_tool_result_cleanup(&manual);
    rc = cc_agent_runtime_execute_manual_tool(runtime, "s1", &call, &manual);
    CHECK(rc.code == CC_ERR_INVALID_STATE && first.calls == 4);
    cc_result_free(&rc);
    cc_tool_result_cleanup(&manual);
    CHECK(ok(cc_agent_runtime_execute_manual_tool(runtime, "s2", &call, &manual)));
    CHECK(first.calls == 5 && manual.ok);
    cc_tool_result_cleanup(&manual);

    /* 同 ID 的账本与历史意图还须名称和参数相同，冲突继续保留阻断。 */
    for (int mismatch=0;mismatch<2;mismatch++) {
        const char *sid=mismatch ? "conflict-args" : "conflict-name";
        CHECK(ok(sessions.vtable->create_session(sessions.self,sid,".")));
        cc_tool_call_t historical={"conflict-call","write","{\"value\":1}"};
        cc_tool_call_t journaled={"conflict-call",mismatch ? "write" : "other",
                                   mismatch ? "{\"value\":2}" : "{\"value\":1}"};
        cc_message_t *intent=NULL;
        CHECK(ok(cc_message_create_text("conflict-intent",sid,CC_ROLE_ASSISTANT,"",NULL,&intent)) &&
              ok(cc_message_add_tool_call(intent,&historical)));
        cc_session_record_t record={.type=CC_SESSION_RECORD_MESSAGE,.session_id=sid,.data.message=intent};
        CHECK(ok(sessions.vtable->append_records(sessions.self,&record,1)));
        cc_message_destroy(intent);
        char *operation=NULL; CHECK(ok(cc_operation_journal_import_unknown(journal,sid,&journaled,&operation)));
        rc=cc_agent_runtime_resolve_pending_tool(runtime,sid,"conflict-call",CC_TOOL_RECOVERY_EXECUTED,&observed);
        CHECK(rc.code==CC_ERR_STORAGE); cc_result_free(&rc);
        CHECK(ok(cc_operation_journal_is_blocked(journal,journaled.name,&is_blocked)) && is_blocked);
        history=NULL; history_count=0;
        CHECK(ok(sessions.vtable->load_messages(sessions.self,sid,-1,&history,&history_count)) && history_count==1);
        cc_message_cleanup(&history[0]); free(history);
        CHECK(ok(cc_operation_journal_finish(journal,operation))); free(operation);
    }
    cc_agent_runtime_destroy(runtime);

    /* 必需账本不可用时允许文本对话，关闭工具宣告、调用及策略/元数据钩子。 */
    deps.operation_journal = NULL;
    deps.require_operation_journal = 1;
    int policy_calls = 0;
    fake_chat_state_t model = {0};
    deps.policy.self = &policy_calls;
    deps.llm.self = &model;
    runtime = NULL;
    CHECK(ok(cc_agent_runtime_create(&deps, &options, &runtime)));
    first.fail = 0;
    first.name_calls = first.description_calls = first.schema_calls = 0;
    other.name_calls = other.description_calls = other.schema_calls = 0;
    call.id = "call-7";
    rc = cc_agent_runtime_execute_tool_step(runtime, "s3", &call, NULL, NULL, 0);
    CHECK(rc.code == CC_ERR_INVALID_STATE);
    cc_result_free(&rc);
    rc = cc_agent_runtime_execute_manual_tool(runtime, "s3", &call, &manual);
    CHECK(rc.code == CC_ERR_INVALID_STATE);
    cc_result_free(&rc);
    cc_tool_result_cleanup(&manual);
    rc = cc_agent_runtime_execute_manual_tool_with_impl(runtime, "s3", &call, &tool1, &manual);
    CHECK(rc.code == CC_ERR_INVALID_STATE);
    cc_result_free(&rc);
    cc_tool_result_cleanup(&manual);
    cc_tool_dispatch_checkpoint_t blocked_checkpoint = {0};
    cc_tool_result_t blocked_result = {0};
    CHECK(ok(cc_tool_executor_execute_guarded(runtime, "s3", &call, NULL,
        &blocked_result, &blocked_checkpoint, &tool1, NULL, 0)));
    CHECK(!blocked_result.ok && blocked_result.error && !blocked_checkpoint.operation_id);
    cc_tool_result_cleanup(&blocked_result);
    answer = NULL;
    CHECK(ok(cc_agent_runtime_handle_message(runtime, "s3", "text only", &answer)));
    CHECK(answer && strcmp(answer, "continued") == 0);
    free(answer);
    CHECK(model.requests == 1 && model.saw_tools == 0);
    history = NULL; history_count = 0;
    CHECK(ok(sessions.vtable->load_messages(sessions.self, "s3", -1, &history, &history_count)));
    CHECK(history_count == 2 && history[0].role == CC_ROLE_USER && history[1].role == CC_ROLE_ASSISTANT);
    for (size_t i = 0; i < history_count; i++) cc_message_cleanup(&history[i]);
    free(history);

    /* 降级期间 provider 主动给出的工具调用也不得形成持久意图。 */
    model.send_tool_call = 1;
    answer = NULL;
    stream_observer_t observer={0};
    cc_agent_runtime_stream_options_t stream_options={0};
    stream_options.size=sizeof(stream_options); stream_options.on_chunk=observe_stream;
    stream_options.user_data=&observer;
    rc = cc_agent_runtime_handle_message_stream_cb(runtime, "s4", "try a tool", &stream_options, &answer);
    CHECK(rc.code == CC_ERR_INVALID_STATE);
    cc_result_free(&rc);
    free(answer);
    CHECK(model.requests == 2 && model.saw_tools == 0);
    CHECK(observer.tool_chunks == 0 && observer.errors == 1);
    history = NULL; history_count = 0;
    CHECK(ok(sessions.vtable->load_messages(sessions.self, "s4", -1, &history, &history_count)));
    CHECK(history_count == 1 && history[0].role == CC_ROLE_USER);
    for (size_t i = 0; i < history_count; i++) cc_message_cleanup(&history[i]);
    free(history);
    CHECK(first.calls == 5);
    CHECK(first.name_calls == 0 && first.description_calls == 0 && first.schema_calls == 0);
    CHECK(other.name_calls == 0 && other.description_calls == 0 && other.schema_calls == 0);
    CHECK(policy_calls == 0);
    cc_agent_runtime_destroy(runtime);
    cc_tool_registry_destroy(registry);
    sessions.vtable->destroy(sessions.self);
    cc_operation_journal_destroy(journal);
    CHECK(ok(fs.vtable->remove(fs.self, directory)));
    fs.vtable->destroy(fs.self);
    free(directory);
    return 0;
}
