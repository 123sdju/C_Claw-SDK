

#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_context_builder.h"
#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/core/cc_message.h"

#include <stdlib.h>
#include <string.h>


/*
 * 使用内存 session store 作为持久化 adapter，避免测试依赖文件系统或 SQLite。
 *
 * 这里直接链接 adapter 创建函数，是为了验证 session store 保存后的消息 envelope
 * 能被 context builder 正确还原，而不是验证 runtime builder 的工厂分发。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);


/*
 * 最小 fake LLM：只返回一个完整 assistant 文本。
 *
 * 本测试真正关注的是发起请求前的上下文构建结果，fake_chat 不读取 request；
 * 它存在的意义是让 runtime create 通过 provider 依赖校验。
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


/*
 * 验证 tool_calls 和 reasoning_content 经过 store/context/json 三层后没有被二次转义。
 *
 * 测试流程：
 * 1. 建立只含 fake LLM、空 tool registry 和内存 store 的 runtime。
 * 2. 手工写入 assistant tool call 消息和 tool result 消息，模拟一次工具调用历史。
 * 3. 通过 context builder 读取历史并序列化为 LLM messages JSON。
 * 4. 断言 JSON 中存在结构化 tool_calls/reasoning_content，且没有出现被转义的
 *    "\\\"tool_calls\\\""，避免 provider adapter 收到字符串套字符串。
 */
int main(void)
{
    int failed = 0;
    cc_tool_registry_t *registry = NULL;
    cc_session_store_t store = {0};
    cc_agent_runtime_t *runtime = NULL;

    if (cc_tool_registry_create(&registry).code != CC_OK) return 1;
    cc_tool_registry_freeze(registry);
    if (cc_memory_session_store_create(&store).code != CC_OK) return 1;
    store.vtable->create_session(store.self, "ses_msg", ".");

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
    if (cc_agent_runtime_create(&deps, &options, &runtime).code != CC_OK) return 1;

    cc_message_t *msg = NULL;
    cc_message_create_text("m1", "ses_msg", CC_ROLE_ASSISTANT, NULL, NULL, &msg);
    cc_tool_call_t call = {
        .id = "call_1",
        .name = "file_read",
        .arguments_json = "{}"
    };
    cc_message_add_tool_call(msg, &call);
    cc_message_set_reasoning_content(msg, "thinking");
    cc_session_record_t record = {
        .type = CC_SESSION_RECORD_MESSAGE,
        .session_id = "ses_msg",
        .data.message = msg,
    };
    store.vtable->append_records(store.self, &record, 1);
    cc_message_destroy(msg);

    cc_message_create_text("m2", "ses_msg", CC_ROLE_TOOL, "{\"ok\":true}", "call_1", &msg);
    record.data.message = msg;
    store.vtable->append_records(store.self, &record, 1);
    cc_message_destroy(msg);

    cc_message_t *messages = NULL;
    size_t message_count = 0;
    cc_result_t rc = cc_context_builder_build_messages(
        runtime, "ses_msg", "system", NULL, &messages, &message_count);
    char *messages_text = NULL;
    if (rc.code == CC_OK) {
        rc = cc_messages_to_json(messages, message_count, 1, &messages_text);
    }
    if (rc.code != CC_OK || !messages_text) failed = 1;
    if (messages_text && !strstr(messages_text, "\"tool_calls\"")) failed = 1;
    if (messages_text && !strstr(messages_text, "\"reasoning_content\"")) failed = 1;
    if (messages_text && strstr(messages_text, "\\\"tool_calls\\\"")) failed = 1;

    free(messages_text);
    for (size_t i = 0; i < message_count; i++) cc_message_cleanup(&messages[i]);
    free(messages);
    cc_result_free(&rc);
    cc_agent_runtime_destroy(runtime);
    store.vtable->destroy(store.self);
    cc_tool_registry_destroy(registry);
    return failed ? 1 : 0;
}
