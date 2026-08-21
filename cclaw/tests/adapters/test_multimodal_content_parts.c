#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_context_builder.h"
#include "cc/core/cc_media.h"
#include "cc/core/cc_message.h"
#include "cc/core/cc_tool_call.h"
#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_tool_registry.h"

#include <stdlib.h>
#include <string.h>

/*
 * 引入内存 session store adapter，测试多模态消息进入历史上下文后的序列化结果。
 *
 * 这里不走存储工厂，是为了让测试目标集中在 media/message/context 三层的数据契约。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

/*
 * 最小 fake LLM，用于满足 runtime 对 provider 的依赖。
 *
 * 多模态测试不关心模型响应内容，只需要 runtime 能正常创建并让 context builder
 * 读取 session history；因此 fake_chat 返回固定完整文本即可。
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
 * fake provider 的 vtable。
 *
 * 只填 chat 能力，体现“struct + 函数指针”的 C 语言面向对象接口；
 * 没有实现 stream/tool 专属能力时，运行时能力协商会在对应入口 fail-fast。
 */
static cc_llm_provider_vtable_t fake_llm_vtable = {
    fake_chat,
    NULL,
    NULL
};

/*
 * 验证多模态 content parts 在 SDK 各层之间保持结构化表示。
 *
 * 覆盖点包括：
 * 1. text + image artifact 可以组成 content parts 并序列化为 JSON。
 * 2. cc_message_create_parts 与 cc_message_copy 会深拷贝 content part 元数据。
 * 3. tool result 也能携带 artifact，供后续多模态工具链使用。
 * 4. 写入 session store 后再由 context builder 取出，provider 看到的 messages JSON
 *    仍包含 image/data_base64，而不是退化成普通文本。
 *
 * 嵌入式面试可重点说明：artifact 结构允许 path/base64/metadata 并存，下游平台可以
 * 按内存预算选择传引用、传摘要或传小尺寸 base64，而核心 SDK 保持统一数据模型。
 */
int main(void)
{
    cc_media_artifact_t image = {
        .id = "img_1",
        .kind = CC_MEDIA_IMAGE,
        .mime = "image/png",
        .path = "/tmp/image.png",
        .data_base64 = "aW1hZ2U=",
        .bytes = 5,
        .width = 2,
        .height = 3,
        .created_at = "2026-05-25T00:00:00"
    };
    cc_content_parts_t parts;
    cc_content_parts_init(&parts);
    cc_result_t rc = cc_content_parts_append_text(
        &parts, "observe image", CC_CONTENT_PART_INPUT);
    if (rc.code != CC_OK) return 2;
    rc = cc_content_parts_append_artifact(&parts, &image, CC_CONTENT_PART_INPUT);
    if (rc.code != CC_OK) return 3;
    cc_result_free(&rc);

    char *parts_json = NULL;
    rc = cc_content_parts_to_json(&parts, &parts_json);
    if (rc.code != CC_OK || !parts_json ||
        !strstr(parts_json, "\"type\":\"image\"") ||
        !strstr(parts_json, "\"data_base64\":\"aW1hZ2U=\"")) return 3;

    cc_message_t *msg = NULL;
    rc = cc_message_create_parts("m1", "ses_mm", CC_ROLE_USER, &parts, NULL, &msg);
    if (rc.code != CC_OK || !msg) return 4;
    cc_message_t copied;
    if (cc_message_copy(msg, &copied).code != CC_OK ||
        copied.content.count != parts.count ||
        copied.content.items[1].kind != CC_MEDIA_IMAGE) {
        return 4;
    }
    cc_message_cleanup(&copied);

    cc_tool_result_t tr = {0};
    rc = cc_tool_result_add_artifact(&tr, &image);
    if (rc.code != CC_OK || tr.artifacts.count != 1 ||
        tr.artifacts.items[0].kind != CC_MEDIA_IMAGE) return 5;
    cc_tool_result_cleanup(&tr);

    cc_tool_registry_t *registry = NULL;
    cc_session_store_t store = {0};
    cc_agent_runtime_t *runtime = NULL;
    if (cc_tool_registry_create(&registry).code != CC_OK) return 1;
    cc_tool_registry_freeze(registry);
    if (cc_memory_session_store_create(&store).code != CC_OK) return 1;
    store.vtable->create_session(store.self, "ses_mm", ".");
    cc_session_record_t record = {
        .type = CC_SESSION_RECORD_MESSAGE,
        .session_id = "ses_mm",
        .data.message = msg,
    };
    store.vtable->append_records(store.self, &record, 1);

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
    if (cc_agent_runtime_create(&deps, &options, &runtime).code != CC_OK) return 1;

    cc_message_t *messages = NULL;
    size_t message_count = 0;
    rc = cc_context_builder_build_messages(runtime, "ses_mm", "system", NULL,
                                            &messages, &message_count);
    char *messages_text = NULL;
    if (rc.code == CC_OK) {
        rc = cc_messages_to_json(messages, message_count, 1, &messages_text);
    }
    if (rc.code != CC_OK || !messages_text ||
        !strstr(messages_text, "\"content\"") ||
        !strstr(messages_text, "image") ||
        !strstr(messages_text, "data_base64")) {
        return 6;
    }

    free(messages_text);
    for (size_t i = 0; i < message_count; i++) cc_message_cleanup(&messages[i]);
    free(messages);
    cc_result_free(&rc);
    cc_agent_runtime_destroy(runtime);
    store.vtable->destroy(store.self);
    cc_tool_registry_destroy(registry);
    cc_message_destroy(msg);
    free(parts_json);
    cc_content_parts_cleanup(&parts);
    return 0;
}
