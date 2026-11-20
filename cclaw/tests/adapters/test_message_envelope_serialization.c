

#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_context_builder.h"
#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/core/cc_message.h"

#include <stdlib.h>
#include <string.h>


/** @brief 创建本测试独占的内存会话端口，作为 context builder 的历史依赖。
 * @param[out] out_store 必填新拥有槽，成功先排空 runtime 借用再 destroy。
 * @return 拥有结果，创建者消费诊断，消息持久副本由后端拥有。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);


/** @brief 构造固定完整文本响应，为上下文测试提供同步 provider。
 * @param[in] self 未使用的状态借用，可 NULL。
 * @param[in] request 未读取的同步请求借用。
 * @param[out] out 必填新响应槽，初始化后拥有文本，由 runtime 或调用者 cleanup。
 * @return set_text 的分配错误或成功，返回诊断归接收者释放，无异步资源。
 */
static cc_result_t fake_chat(void *self, const cc_llm_chat_request_t *request, cc_llm_response_t *out)
{
    (void)self;
    (void)request;
    cc_llm_response_init(out);
    cc_result_t rc = cc_llm_response_set_text(out, "ok");
    if (rc.code != CC_OK) return rc;
    out->finished = 1;
    return cc_result_ok();
}

/*
 * provider vtable 只实现同步 chat。
 *
 * 这体现了 C 语言 OOP 的最小实现方式：结构体保存函数指针，测试只填需要的能力，
 * 其余能力保持 NULL，由 runtime/capability 校验决定是否可用。
 */
static cc_llm_provider_vtable_t fake_llm_vtable = {
    fake_chat,
    NULL,
    NULL
};


/** @brief 检查并消费一次拥有结果。
 * @param[in] result 按值移入，诊断在本函数释放。
 * @return CC_OK 为一，其他为零，无剩余拥有资源。
 */
static int result_ok(cc_result_t result)
{
    int valid = result.code == CC_OK;
    cc_result_free(&result);
    return valid;
}

/** @brief 验证 user、assistant tool call 与 tool result 经 store/context/JSON 完整恢复。
 * @return 初始化、存储、构建或结构断言失败为一，全部通过为零。
 * @note 单线程独占 fixture，runtime 借用 registry/store，先 destroy runtime 再归还依赖。
 * 失败统一清理消息、JSON、上下文数组、runtime、store 和 registry，所有拥有诊断均消费。
 */
int main(void)
{
    int failed = 1;
    cc_tool_registry_t *registry = NULL;
    cc_session_store_t store = {0};
    cc_agent_runtime_t *runtime = NULL;
    cc_message_t *msg = NULL;
    cc_message_t *messages = NULL;
    size_t message_count = 0;
    char *messages_text = NULL;

    if (!result_ok(cc_tool_registry_create(&registry))) goto cleanup;
    if (!result_ok(cc_tool_registry_freeze(registry))) goto cleanup;
    if (!result_ok(cc_memory_session_store_create(&store))) goto cleanup;
    if (!result_ok(store.vtable->create_session(store.self, "ses_msg", "."))) goto cleanup;

    cc_llm_provider_t llm = { NULL, &fake_llm_vtable };
    cc_agent_runtime_deps_t deps = {0};
    deps.llm = llm;
    deps.tool_registry = registry;
    deps.store = store;

    cc_agent_runtime_options_t options = {0};
    options.config.max_steps = 1;
    options.config.system_prompt = "system";
    options.config.workspace_dir = ".";
    options.config.model = "fake";
    options.thinking_mode = 1;
    if (!result_ok(cc_agent_runtime_create(&deps, &options, &runtime))) goto cleanup;

    if (!result_ok(cc_message_create_text("m0", "ses_msg", CC_ROLE_USER,
                                          "read the file", NULL, &msg))) goto cleanup;
    cc_session_record_t record = {
        .type = CC_SESSION_RECORD_MESSAGE,
        .session_id = "ses_msg",
        .data.message = msg,
    };
    if (!result_ok(store.vtable->append_records(store.self, &record, 1))) goto cleanup;
    cc_message_destroy(msg);
    msg = NULL;

    if (!result_ok(cc_message_create_text("m1", "ses_msg", CC_ROLE_ASSISTANT,
                                          NULL, NULL, &msg))) goto cleanup;
    cc_tool_call_t call = {
        .id = "call_1",
        .name = "file_read",
        .arguments_json = "{}"
    };
    if (!result_ok(cc_message_add_tool_call(msg, &call))) goto cleanup;
    if (!result_ok(cc_message_set_reasoning_content(msg, "thinking"))) goto cleanup;
    record.data.message = msg;
    if (!result_ok(store.vtable->append_records(store.self, &record, 1))) goto cleanup;
    cc_message_destroy(msg);
    msg = NULL;

    if (!result_ok(cc_message_create_text("m2", "ses_msg", CC_ROLE_TOOL,
                                          "{\"ok\":true}", "call_1", &msg))) goto cleanup;
    record.data.message = msg;
    if (!result_ok(store.vtable->append_records(store.self, &record, 1))) goto cleanup;
    cc_message_destroy(msg);
    msg = NULL;

    cc_result_t rc = cc_context_builder_build_messages(
        runtime, "ses_msg", "system", NULL, &messages, &message_count);
    if (!result_ok(rc)) goto cleanup;
    if (!result_ok(cc_messages_to_json(messages, message_count, 1, &messages_text))) goto cleanup;
    failed = !messages_text;
    if (messages_text && !strstr(messages_text, "\"tool_calls\"")) failed = 1;
    if (messages_text && !strstr(messages_text, "\"reasoning_content\"")) failed = 1;
    if (messages_text && strstr(messages_text, "\\\"tool_calls\\\"")) failed = 1;

cleanup:
    cc_message_destroy(msg);
    free(messages_text);
    for (size_t i = 0; i < message_count; i++) cc_message_cleanup(&messages[i]);
    free(messages);
    cc_agent_runtime_destroy(runtime);
    if (store.vtable) store.vtable->destroy(store.self);
    cc_tool_registry_destroy(registry);
    return failed ? 1 : 0;
}
