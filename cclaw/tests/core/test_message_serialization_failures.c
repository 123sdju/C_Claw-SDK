#include "cc/core/cc_message.h"
#include "../../core/vendor/cjson/cJSON.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 检查一次测试条件，失败报告行号并返回一。
 * @param[in] expression 求值一次的条件。
 */
#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "serialization check failed at %d\n", __LINE__); return 1; \
} } while (0)

/** @brief 仅供本测试串行使用的 JSON 分配故障序号。 */
static size_t allocation_count;
static size_t failed_allocation = SIZE_MAX;

/**
 * @brief 指定分配序号返回 NULL，其余委托 malloc。
 * @param[in] bytes 本次分配字节数。
 * @return 独占分配地址，故障序号或系统分配失败返回 NULL。
 */
static void *allocate_json(size_t bytes)
{
    if (allocation_count++ == failed_allocation) return NULL;
    return malloc(bytes);
}

/**
 * @brief 消费一个测试 JSON 分配。
 * @param[in,out] allocation 来自 allocate_json 的独占地址，可为 NULL。
 * @post 非空地址已释放，测试串行调用以避免全局钩子竞争。
 */
static void release_json(void *allocation)
{
    free(allocation);
}

/**
 * @brief 用相同的完整媒体、内容和工具身份构造五条序列化入口的输入。
 * @param[in] kind 从零到四选择媒体、内容、工具、单消息、消息数组。
 * @param[out] out_json 接收独占序列化字符串，成功由测试释放，失败为 NULL。
 * @return 被调用序列化入口的拥有结果，由测试调用者释放。
 * @note 输入字符串仅供此次只读调用借用，禁止对这些栈上组合执行 cleanup。
 */
static cc_result_t serialize_fixture(int kind, char **out_json)
{
    cc_media_artifact_t artifact = {
        .id = "image-1", .kind = CC_MEDIA_IMAGE, .mime = "image/png",
        .path = "/media/image.png", .uri = "https://example.test/image.png",
        .data_base64 = "YQ==", .bytes = 1, .width = 2, .height = 3,
        .duration_ms = 4, .created_at = "2026-11-13T00:00:00Z", .metadata = "metadata"
    };
    cc_media_artifact_list_t artifacts = {.items = &artifact, .count = 1, .capacity = 1};
    cc_content_part_t items[] = {
        {.kind = CC_MEDIA_TEXT, .direction = CC_CONTENT_PART_INPUT, .text = "keep text"},
        {.kind = CC_MEDIA_IMAGE, .direction = CC_CONTENT_PART_OUTPUT, .artifact = artifact}
    };
    cc_content_parts_t parts = {.items = items, .count = 2, .capacity = 2};
    cc_tool_call_t call = {.id = "call-1", .name = "lookup", .arguments_json = "{\"x\":1}"};
    cc_tool_call_list_t calls = {.items = &call, .count = 1, .capacity = 1};
    cc_message_t message = {
        .role = CC_ROLE_TOOL, .content = parts, .tool_calls = calls,
        .reasoning_content = "keep reasoning", .tool_call_id = "call-1"
    };
    switch (kind) {
        case 0: return cc_media_artifact_list_to_json(&artifacts, out_json);
        case 1: return cc_content_parts_to_json(&parts, out_json);
        case 2: return cc_tool_call_list_to_json(&calls, out_json);
        case 3: return cc_message_to_json(&message, 1, out_json);
        default: return cc_messages_to_json(&message, 1, 1, out_json);
    }
}

/**
 * @brief 对五个入口逐个 JSON 分配注入单次失败，验证整份失败。
 * @return 全部断言通过为零，失败为一，成功路径恢复全局 JSON 钩子。
 */
static int test_allocation_failures(void)
{
    cJSON_Hooks hooks = {allocate_json, release_json};
    for (int kind = 0; kind < 5; kind++) {
        char *json = NULL;
        allocation_count = 0;
        failed_allocation = SIZE_MAX;
        cJSON_InitHooks(&hooks);
        cc_result_t rc = serialize_fixture(kind, &json);
        size_t baseline_allocations = allocation_count;
        int complete = rc.code == CC_OK && json && strstr(json, kind == 2 ? "lookup" : "image-1");
        release_json(json);
        cJSON_InitHooks(NULL);
        cc_result_free(&rc);
        CHECK(complete && baseline_allocations > 0);
        for (size_t failure = 0; failure < baseline_allocations; failure++) {
            allocation_count = 0;
            failed_allocation = failure;
            json = (char *)(uintptr_t)1;
            cJSON_InitHooks(&hooks);
            rc = serialize_fixture(kind, &json);
            int rejected = rc.code != CC_OK && !json;
            if (json && json != (char *)(uintptr_t)1) release_json(json);
            cJSON_InitHooks(NULL);
            cc_result_free(&rc);
            if (!rejected) fprintf(stderr, "kind %d allocation %zu\n", kind, failure);
            CHECK(rejected);
        }
    }
    failed_allocation = SIZE_MAX;
    return 0;
}

/**
 * @brief 验证非空数量配空数组的输入会被拒绝并清空输出槽。
 * @return 全部通过为零，失败为一，成功路径释放所有结果消息。
 */
static int test_invalid_arrays(void)
{
    cc_media_artifact_list_t artifacts = {.count = 1};
    cc_content_parts_t parts = {.count = 1};
    cc_tool_call_list_t calls = {.count = 1};
    cc_message_t message = {.content = parts};
    for (int kind = 0; kind < 5; kind++) {
        char *json = (char *)(uintptr_t)1;
        cc_result_t rc;
        switch (kind) {
            case 0: rc = cc_media_artifact_list_to_json(&artifacts, &json); break;
            case 1: rc = cc_content_parts_to_json(&parts, &json); break;
            case 2: rc = cc_tool_call_list_to_json(&calls, &json); break;
            case 3: rc = cc_message_to_json(&message, 1, &json); break;
            default: rc = cc_messages_to_json(NULL, 1, 1, &json); break;
        }
        int rejected = rc.code == CC_ERR_INVALID_ARGUMENT && !json;
        cc_result_free(&rc);
        CHECK(rejected);
    }
    return 0;
}

/**
 * @brief 执行完整消息序列化的分配故障及无效数组回归。
 * @return 全部通过为零，任一子测试失败为一。
 */
int main(void)
{
    CHECK(test_allocation_failures() == 0);
    CHECK(test_invalid_arrays() == 0);
    return 0;
}
