#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_context_builder.h"
#include "cc/core/cc_media.h"
#include "cc/core/cc_message.h"
#include "cc/core/cc_tool_call.h"
#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_tool_registry.h"

#include <stdlib.h>
#include <string.h>

/** @brief 创建多模态历史测试使用的内存后端。
 * @param[out] out_store 必填新拥有槽，成功由测试 owner 在 runtime 结束后 destroy。
 * @return 拥有结果，由调用者释放诊断，后端深拷贝消息并保管副本。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

/** @brief 构造固定完整文本以提供上下文测试所需的同步 provider。
 * @param[in] self 未使用，可空状态借用。
 * @param[in] request 未读取的同步请求借用。
 * @param[out] out 必填新响应槽，成功拥有文本，接收者 cleanup。
 * @return 文本分配错误或成功，拥有诊断归接收者，无异步持有。
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

/** @brief 检查一次结果并消费其拥有诊断。
 * @param[in] result 按值移入的调用结果。
 * @return CC_OK 为一，其余为零，内部诊断已释放。
 */
static int result_ok(cc_result_t result)
{
    int valid = result.code == CC_OK;
    cc_result_free(&result);
    return valid;
}

/** @brief 核对文本与图像分片深拷贝、工具媒体及 store/context JSON 恢复。
 * @return 全部通过为零，分阶段构造或断言错误为一至六。
 * @note 字面量 artifact 只借用，parts/msg/copied/result 各持深副本，统一清理覆盖失败路径。
 * 单线程独占 fixture，runtime 先销毁再归还其借用的 registry/store。
 */
int main(void)
{
    int failed = 2;
    char *parts_json = NULL;
    cc_message_t *msg = NULL;
    cc_message_t copied = {0};
    cc_tool_result_t tr = {0};
    cc_tool_registry_t *registry = NULL;
    cc_session_store_t store = {0};
    cc_agent_runtime_t *runtime = NULL;
    cc_message_t *messages = NULL;
    size_t message_count = 0;
    char *messages_text = NULL;
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
    if (!result_ok(rc)) goto cleanup;
    failed = 3;
    rc = cc_content_parts_append_artifact(&parts, &image, CC_CONTENT_PART_INPUT);
    if (!result_ok(rc)) goto cleanup;

    rc = cc_content_parts_to_json(&parts, &parts_json);
    if (!result_ok(rc) || !parts_json ||
        !strstr(parts_json, "\"type\":\"image\"") ||
        !strstr(parts_json, "\"data_base64\":\"aW1hZ2U=\"")) goto cleanup;

    failed = 4;
    rc = cc_message_create_parts("m1", "ses_mm", CC_ROLE_USER, &parts, NULL, &msg);
    if (!result_ok(rc) || !msg) goto cleanup;
    if (!result_ok(cc_message_copy(msg, &copied)) ||
        copied.content.count != parts.count ||
        copied.content.items[1].kind != CC_MEDIA_IMAGE) {
        goto cleanup;
    }
    cc_message_cleanup(&copied);

    failed = 5;
    rc = cc_tool_result_add_artifact(&tr, &image);
    if (!result_ok(rc) || tr.artifacts.count != 1 ||
        tr.artifacts.items[0].kind != CC_MEDIA_IMAGE) goto cleanup;
    cc_tool_result_cleanup(&tr);

    failed = 1;
    if (!result_ok(cc_tool_registry_create(&registry))) goto cleanup;
    if (!result_ok(cc_tool_registry_freeze(registry))) goto cleanup;
    if (!result_ok(cc_memory_session_store_create(&store))) goto cleanup;
    if (!result_ok(store.vtable->create_session(store.self, "ses_mm", "."))) goto cleanup;
    cc_session_record_t record = {
        .type = CC_SESSION_RECORD_MESSAGE,
        .session_id = "ses_mm",
        .data.message = msg,
    };
    if (!result_ok(store.vtable->append_records(store.self, &record, 1))) goto cleanup;

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
    if (!result_ok(cc_agent_runtime_create(&deps, &options, &runtime))) goto cleanup;

    failed = 6;
    rc = cc_context_builder_build_messages(runtime, "ses_mm", "system", NULL,
                                            &messages, &message_count);
    if (!result_ok(rc)) goto cleanup;
    rc = cc_messages_to_json(messages, message_count, 1, &messages_text);
    if (!result_ok(rc) || !messages_text ||
        !strstr(messages_text, "\"content\"") ||
        !strstr(messages_text, "image") ||
        !strstr(messages_text, "data_base64")) {
        goto cleanup;
    }
    failed = 0;

cleanup:
    free(messages_text);
    for (size_t i = 0; i < message_count; i++) cc_message_cleanup(&messages[i]);
    free(messages);
    cc_agent_runtime_destroy(runtime);
    if (store.vtable) store.vtable->destroy(store.self);
    cc_tool_registry_destroy(registry);
    cc_message_destroy(msg);
    cc_message_cleanup(&copied);
    cc_tool_result_cleanup(&tr);
    free(parts_json);
    cc_content_parts_cleanup(&parts);
    return failed;
}
