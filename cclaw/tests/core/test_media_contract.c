#include "cc/core/cc_media.h"
#include "cc/util/cc_json.h"
#include "../../core/vendor/cjson/cJSON.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct allocation_record {
    void *pointer;
    size_t bytes;
} allocation_record_t;

static allocation_record_t allocations[512];
static size_t live_allocations;
static size_t allocation_attempt;
static size_t failed_attempt = SIZE_MAX;
static char long_path[800];

/**
 * @brief 查找本测试私有分配的登记槽。
 * @param[in] pointer 待查地址，可为 NULL。
 * @return 找到时为槽下标，未登记为数组长度。
 */
static size_t allocation_index(const void *pointer)
{
    for (size_t i = 0; i < sizeof(allocations) / sizeof(allocations[0]); i++) {
        if (allocations[i].pointer == pointer) return i;
    }
    return sizeof(allocations) / sizeof(allocations[0]);
}

/**
 * @brief 对局部媒体、builder 和串行 JSON 分配注入单点失败。
 * @param[in] bytes 所需字节数。
 * @return 测试登记的拥有分配，命中故障序号为 NULL。
 */
static void *media_malloc(size_t bytes)
{
    if (allocation_attempt++ == failed_attempt) return NULL;
    void *pointer = malloc(bytes);
    assert(pointer);
    size_t slot = allocation_index(NULL);
    assert(slot < sizeof(allocations) / sizeof(allocations[0]));
    allocations[slot].pointer = pointer;
    allocations[slot].bytes = bytes;
    live_allocations++;
    return pointer;
}

/**
 * @brief 归还局部登记或外部 SDK 交付的拥有分配。
 * @param[in,out] pointer 可为 NULL，非空指针只释放一次。
 * @post 已登记指针移除记录，未登记指针沿标准 free 消费。
 */
static void media_free(void *pointer)
{
    if (!pointer) return;
    size_t slot = allocation_index(pointer);
    if (slot < sizeof(allocations) / sizeof(allocations[0])) {
        allocations[slot].pointer = NULL;
        allocations[slot].bytes = 0;
        assert(live_allocations);
        live_allocations--;
    }
    free(pointer);
}

/**
 * @brief 强制成功扩容搬迁，令跨 realloc 的数组元素借用确定失效。
 * @param[in,out] pointer 本测试登记的旧缓冲，可为 NULL。
 * @param[in] bytes 新容量，非零。
 * @return 新拥有缓冲，故障时为 NULL 且旧缓冲保留。
 */
static void *media_realloc(void *pointer, size_t bytes)
{
    if (allocation_attempt++ == failed_attempt) return NULL;
    if (!pointer) {
        allocation_attempt--;
        return media_malloc(bytes);
    }
    size_t slot = allocation_index(pointer);
    assert(slot < sizeof(allocations) / sizeof(allocations[0]));
    void *next = malloc(bytes);
    assert(next);
    size_t copy = allocations[slot].bytes < bytes ? allocations[slot].bytes : bytes;
    memcpy(next, pointer, copy);
    free(pointer);
    allocations[slot].pointer = next;
    allocations[slot].bytes = bytes;
    return next;
}

#define cc_media_kind_string review_media_kind_string
#define cc_media_kind_from_string review_media_kind_from_string
#define cc_media_limits_text_only review_media_limits_text_only
#define cc_media_limits_default_multimodal review_media_limits_default_multimodal
#define cc_media_artifact_init review_media_artifact_init
#define cc_media_artifact_cleanup review_media_artifact_cleanup
#define cc_media_artifact_copy review_media_artifact_copy
#define cc_media_artifact_validate review_media_artifact_validate
#define cc_media_artifact_list_init review_media_artifact_list_init
#define cc_media_artifact_list_cleanup review_media_artifact_list_cleanup
#define cc_media_artifact_list_append review_media_artifact_list_append
#define cc_media_artifact_list_copy review_media_artifact_list_copy
#define cc_media_artifact_list_validate review_media_artifact_list_validate
#define cc_media_artifact_list_to_json review_media_artifact_list_to_json
#define cc_media_artifact_list_from_json review_media_artifact_list_from_json
#define cc_media_artifact_list_summarize review_media_artifact_list_summarize
#define cc_content_part_cleanup review_content_part_cleanup
#define cc_content_part_copy review_content_part_copy
#define cc_content_parts_init review_content_parts_init
#define cc_content_parts_cleanup review_content_parts_cleanup
#define cc_content_parts_append_text review_content_parts_append_text
#define cc_content_parts_append_artifact review_content_parts_append_artifact
#define cc_content_parts_copy review_content_parts_copy
#define cc_content_parts_validate review_content_parts_validate
#define cc_content_parts_to_json review_content_parts_to_json
#define cc_content_parts_from_json review_content_parts_from_json
#define cc_content_parts_text_summary review_content_parts_text_summary
#define cc_string_builder_init review_string_builder_init
#define cc_string_builder_append review_string_builder_append
#define cc_string_builder_appendf review_string_builder_appendf
#define cc_string_builder_append_char review_string_builder_append_char
#define cc_string_builder_take review_string_builder_take
#define cc_string_builder_deinit review_string_builder_deinit
#define cc_string_builder_clear review_string_builder_clear
#define cc_string_builder_cstr review_string_builder_cstr
#define cc_string_builder_error review_string_builder_error
#define malloc media_malloc
#define realloc media_realloc
#define free media_free
#include "../../core/src/util/cc_string_builder.c"
#ifndef CC_MEDIA_TEST_SOURCE
#define CC_MEDIA_TEST_SOURCE "../../core/src/core/cc_media.c"
#endif
#include CC_MEDIA_TEST_SOURCE
#undef malloc
#undef realloc
#undef free

/**
 * @brief 返回包含全部字符串字段的只读媒体视图。
 * @return 借用静态字符串的值，禁止对返回视图执行 cleanup。
 */
static cc_media_artifact_t artifact_view(void)
{
    cc_media_artifact_t artifact = {0};
    artifact.kind = CC_MEDIA_IMAGE;
    artifact.id = "image-1";
    artifact.mime = "image/png";
    artifact.path = long_path;
    artifact.uri = "https://example.test/image.png";
    artifact.data_base64 = "YQ==";
    artifact.created_at = "2026-11-14T00:00:00Z";
    artifact.metadata = "metadata";
    artifact.bytes = 1;
    artifact.width = 2;
    artifact.height = 3;
    artifact.duration_ms = 4;
    return artifact;
}

/**
 * @brief 要求调用结果成功并消费其拥有诊断。
 * @param[in] result 按值交接的唯一结果。
 * @post 成功结果已清理，失败通过断言终止测试。
 */
static void require_success(cc_result_t result)
{
    if (result.code != CC_OK) fprintf(stderr, "media result %d: %s\n", result.code, result.message ? result.message : "");
    assert(result.code == CC_OK);
    cc_result_free(&result);
}

/**
 * @brief 验证四种对象完全自身复制保留拥有指针和有效项。
 * @post 本用例所有媒体、文本和数组分配归零。
 */
static void test_self_copy(void)
{
    cc_media_artifact_t source = artifact_view();
    cc_media_artifact_t artifact;
    require_success(cc_media_artifact_copy(&source, &artifact));
    char *id = artifact.id;
    require_success(cc_media_artifact_copy(&artifact, &artifact));
    assert(artifact.id == id && strcmp(artifact.id, "image-1") == 0);
    cc_media_artifact_cleanup(&artifact);
    cc_content_part_t view = {0};
    view.kind = CC_MEDIA_IMAGE;
    view.text = "caption";
    view.artifact = source;
    cc_content_part_t part;
    require_success(cc_content_part_copy(&view, &part));
    char *text = part.text;
    require_success(cc_content_part_copy(&part, &part));
    assert(part.text == text && strcmp(part.artifact.id, "image-1") == 0);
    cc_content_part_cleanup(&part);
    cc_media_artifact_list_t list = {0};
    require_success(cc_media_artifact_list_append(&list, &source));
    cc_media_artifact_t *items = list.items;
    require_success(cc_media_artifact_list_copy(&list, &list));
    assert(list.count == 1 && list.items == items);
    cc_media_artifact_list_cleanup(&list);
    cc_content_parts_t parts = {0};
    require_success(cc_content_parts_append_text(&parts, "text", CC_CONTENT_PART_INPUT));
    require_success(cc_content_parts_append_artifact(&parts, &source, CC_CONTENT_PART_OUTPUT));
    cc_content_part_t *content = parts.items;
    require_success(cc_content_parts_copy(&parts, &parts));
    assert(parts.count == 2 && parts.items == content);
    cc_content_parts_cleanup(&parts);
    assert(!live_allocations);
}

/**
 * @brief 强制数组搬迁后验证从自身元素追加的媒体和文本保持正确。
 * @post 源项与新项独立拥有字符串，所有测试分配归零。
 */
static void test_alias_append(void)
{
    cc_media_artifact_t source = artifact_view();
    cc_media_artifact_list_t list = {0};
    for (size_t i = 0; i < 4; i++) require_success(cc_media_artifact_list_append(&list, &source));
    assert(list.count == list.capacity);
    require_success(cc_media_artifact_list_append(&list, &list.items[0]));
    assert(list.count == 5 && list.items[0].id != list.items[4].id);
    assert(strcmp(list.items[4].path, long_path) == 0);
    cc_media_artifact_list_cleanup(&list);
    cc_content_parts_t parts = {0};
    for (size_t i = 0; i < 4; i++) require_success(cc_content_parts_append_artifact(&parts, &source, CC_CONTENT_PART_INPUT));
    require_success(cc_content_parts_append_artifact(&parts, &parts.items[0].artifact, CC_CONTENT_PART_OUTPUT));
    assert(parts.count == 5 && parts.items[4].direction == CC_CONTENT_PART_OUTPUT);
    assert(strcmp(parts.items[4].artifact.id, "image-1") == 0);
    cc_content_parts_cleanup(&parts);
    for (size_t i = 0; i < 4; i++) require_success(cc_content_parts_append_text(&parts, "same", CC_CONTENT_PART_INPUT));
    require_success(cc_content_parts_append_text(&parts, parts.items[0].text, CC_CONTENT_PART_OUTPUT));
    assert(parts.count == 5 && strcmp(parts.items[4].text, "same") == 0);
    cc_content_parts_cleanup(&parts);
    assert(!live_allocations);
}

/**
 * @brief 保留只读低容量视图，并检查写入状态、数组参数和输出初始化。
 * @post 借用视图未被消费，复制与摘要资源均释放。
 */
static void test_views_and_invalid_shapes(void)
{
    cc_media_artifact_t source = artifact_view();
    cc_media_artifact_list_t view = {&source, 1, 0};
    require_success(cc_media_artifact_list_validate(&view, NULL));
    cc_media_artifact_list_t copy;
    require_success(cc_media_artifact_list_copy(&view, &copy));
    cc_media_artifact_list_cleanup(&copy);
    char *output = NULL;
    require_success(cc_media_artifact_list_to_json(&view, &output));
    media_free(output);
    require_success(cc_media_artifact_list_summarize(&view, &output));
    assert(strstr(output, long_path));
    media_free(output);
    cc_result_t rc = cc_media_artifact_list_append(&view, &source);
    assert(rc.code == CC_ERR_INVALID_ARGUMENT && view.count == 1 && view.items == &source);
    cc_result_free(&rc);
    cc_content_part_t item = {0}; item.kind = CC_MEDIA_TEXT; item.text = "text";
    cc_content_parts_t parts_view = {&item, 1, 0};
    require_success(cc_content_parts_validate(&parts_view, NULL));
    cc_content_parts_t parts_copy;
    require_success(cc_content_parts_copy(&parts_view, &parts_copy));
    cc_content_parts_cleanup(&parts_copy);
    rc = cc_content_parts_append_text(&parts_view, "bad", CC_CONTENT_PART_INPUT);
    assert(rc.code == CC_ERR_INVALID_ARGUMENT && parts_view.items == &item);
    cc_result_free(&rc);
    cc_media_artifact_list_t bad = {NULL, 1, 0};
    output = (char *)1;
    rc = cc_media_artifact_list_summarize(&bad, &output);
    assert(rc.code == CC_ERR_INVALID_ARGUMENT && output == NULL); cc_result_free(&rc);
    output = (char *)1;
    rc = cc_media_artifact_list_to_json(&bad, &output);
    assert(rc.code == CC_ERR_INVALID_ARGUMENT && output == NULL); cc_result_free(&rc);
    cc_content_parts_t bad_parts = {NULL, 1, 0};
    output = (char *)1;
    rc = cc_content_parts_text_summary(&bad_parts, &output);
    assert(rc.code == CC_ERR_INVALID_ARGUMENT && output == NULL); cc_result_free(&rc);
    rc = cc_content_parts_validate(&bad_parts, NULL);
    assert(rc.code == CC_ERR_INVALID_ARGUMENT); cc_result_free(&rc);
    bad.items = (cc_media_artifact_t *)1;
    bad.capacity = bad.count = SIZE_MAX / sizeof(*bad.items) + 1U;
    rc = cc_media_artifact_list_append(&bad, &source);
    assert(rc.code == CC_ERR_INVALID_ARGUMENT); cc_result_free(&rc);
    rc = cc_media_artifact_list_validate(&bad, NULL);
    assert(rc.code == CC_ERR_INVALID_ARGUMENT); cc_result_free(&rc);
    bad_parts.items = (cc_content_part_t *)1;
    bad_parts.capacity = bad_parts.count = SIZE_MAX / sizeof(*bad_parts.items) + 1U;
    rc = cc_content_parts_append_artifact(&bad_parts, &source, CC_CONTENT_PART_INPUT);
    assert(rc.code == CC_ERR_INVALID_ARGUMENT); cc_result_free(&rc);
    rc = cc_content_parts_validate(&bad_parts, NULL);
    assert(rc.code == CC_ERR_INVALID_ARGUMENT); cc_result_free(&rc);
    cc_media_limits_t limits = {0}; limits.allowed_mime_prefix_count = 1;
    rc = cc_media_artifact_validate(&source, &limits);
    assert(rc.code == CC_ERR_INVALID_ARGUMENT); cc_result_free(&rc);
    limits.allowed_mime_prefixes = (const char **)1;
    limits.allowed_mime_prefix_count = SIZE_MAX / sizeof(*limits.allowed_mime_prefixes) + 1U;
    rc = cc_media_artifact_validate(&source, &limits);
    assert(rc.code == CC_ERR_INVALID_ARGUMENT); cc_result_free(&rc);
    assert(!live_allocations);
}

/**
 * @brief 验证严格媒体/内容列表拒绝错误字段并撤销前序项，保留有效旧格式。
 * @post 所有解析输出清理，源文本保持只读。
 */
static void test_json_schema(void)
{
    const char *bad_artifacts[] = {
        "[null]", "[{\"id\":7}]", "[{\"mime\":null}]", "[{\"bytes\":-1}]",
        "[{\"bytes\":1e999}]", "[{\"bytes\":18446744073709551616}]",
        "[{\"bytes\":true}]", "[{\"width\":\"1\"}]", "[{\"kind\":false}]",
        "[{\"id\":\"a\",\"ID\":\"b\"}]", "[{\"id\":\"a\",\"\\u0069d\":\"b\"}]",
        "[{\"id\":\"a\\u0000b\"}]", "[] tail"
    };
    for (size_t i = 0; i < sizeof(bad_artifacts) / sizeof(bad_artifacts[0]); i++) {
        cc_media_artifact_list_t list;
        cc_result_t rc = cc_media_artifact_list_from_json(bad_artifacts[i], &list);
        assert(rc.code == CC_ERR_JSON && !list.items && !list.count);
        cc_result_free(&rc); cc_media_artifact_list_cleanup(&list);
    }
    const char *bad_parts[] = {
        "[1]", "[{\"type\":7}]", "[{\"text\":null}]", "[{\"artifact\":false}]",
        "[{\"direction\":false}]", "[{\"type\":\"image\",\"artifact\":{\"mime\":8}}]",
        "[{\"TEXT\":\"a\",\"text\":\"b\"}]", "[{\"text\":\"a\\u0000b\"}]",
        "[{\"text\":\"keep\"},null]", "[] tail"
    };
    for (size_t i = 0; i < sizeof(bad_parts) / sizeof(bad_parts[0]); i++) {
        cc_content_parts_t parts;
        cc_result_t rc = cc_content_parts_from_json(bad_parts[i], &parts);
        assert(rc.code == CC_ERR_JSON && !parts.items && !parts.count);
        cc_result_free(&rc); cc_content_parts_cleanup(&parts);
    }
    cc_media_artifact_list_t list;
    require_success(cc_media_artifact_list_from_json(
        "[{\"KIND\":\"future\",\"MIME\":\"image/png\",\"bytes\":3.9,\"width\":-2.9,\"height\":2147483649}]", &list));
    assert(list.count == 1 && list.items[0].kind == CC_MEDIA_FILE && list.items[0].bytes == 3);
    assert(list.items[0].width == -2 && list.items[0].height == INT_MAX);
    cc_media_artifact_list_cleanup(&list);
    require_success(cc_media_artifact_list_from_json(
        "[{\"id\":\"literal\\\\u0000\",\"extension\":1,\"extension\":2}]", &list));
    assert(strcmp(list.items[0].id, "literal\\u0000") == 0);
    cc_media_artifact_list_cleanup(&list);
    cc_content_parts_t parts;
    require_success(cc_content_parts_from_json(
        "[{\"type\":\"image\",\"direction\":\"future\",\"mime\":\"image/png\",\"id\":\"flat\"},{}]", &parts));
    assert(parts.count == 2 && parts.items[0].kind == CC_MEDIA_IMAGE);
    assert(parts.items[0].direction == CC_CONTENT_PART_INPUT && strcmp(parts.items[0].artifact.id, "flat") == 0);
    assert(parts.items[1].kind == CC_MEDIA_TEXT && strcmp(parts.items[1].text, "") == 0);
    cc_content_parts_cleanup(&parts);
    assert(!live_allocations);
}

/**
 * @brief 核对 size_t 边界的 JSON numeric 表示，避免交付自身不能解析的顶端值。
 * @post 所有输出清理，保留 double 表示范围内的既有数值行为。
 */
static void test_byte_number_range(void)
{
    cc_media_artifact_t source = artifact_view();
    cc_media_artifact_list_t view = {&source, 1, 0};
    source.bytes = UINT32_MAX;
    char *json = NULL;
    require_success(cc_media_artifact_list_to_json(&view, &json));
    cc_media_artifact_list_t parsed;
    require_success(cc_media_artifact_list_from_json(json, &parsed));
    assert(parsed.items[0].bytes == UINT32_MAX);
    cc_media_artifact_list_cleanup(&parsed); media_free(json);
    source.bytes = SIZE_MAX;
    cc_result_t rc = cc_media_artifact_list_to_json(&view, &json);
    double upper = (double)(SIZE_MAX / 2U + 1U) * 2.0;
    if ((double)SIZE_MAX >= upper) {
        assert(rc.code == CC_ERR_LIMIT_EXCEEDED && !json && rc.message && strstr(rc.message, "bytes"));
    } else {
        assert(rc.code == CC_OK && json);
        require_success(cc_media_artifact_list_from_json(json, &parsed));
        assert(parsed.items[0].bytes == SIZE_MAX);
        cc_media_artifact_list_cleanup(&parsed); media_free(json);
    }
    cc_result_free(&rc);
    assert(!live_allocations);
}

/**
 * @brief 执行单条需要分配的生产链，并在所有返回路径清理输出。
 * @param[in] operation 从零到十选择复制、追加、摘要、解析、验证或序列化。
 * @return 原生产结果，动态诊断交由测试循环释放。
 */
static cc_result_t allocation_case(int operation)
{
    cc_media_artifact_t source = artifact_view();
    cc_media_artifact_list_t view = {&source, 1, 0};
    cc_content_part_t item = {0}; item.kind = CC_MEDIA_IMAGE; item.text = "caption"; item.artifact = source;
    cc_content_parts_t parts_view = {&item, 1, 0};
    cc_result_t rc;
    char *output = NULL;
    if (operation == 0) {
        cc_media_artifact_t copy;
        rc = cc_media_artifact_copy(&source, &copy); cc_media_artifact_cleanup(&copy);
    } else if (operation == 1) {
        cc_media_artifact_list_t list = {0};
        rc = cc_media_artifact_list_append(&list, &source); cc_media_artifact_list_cleanup(&list);
    } else if (operation == 2) {
        cc_content_parts_t parts = {0};
        rc = cc_content_parts_append_artifact(&parts, &source, CC_CONTENT_PART_OUTPUT); cc_content_parts_cleanup(&parts);
    } else if (operation == 3) {
        rc = cc_media_artifact_list_summarize(&view, &output);
    } else if (operation == 4) {
        rc = cc_content_parts_text_summary(&parts_view, &output);
    } else if (operation == 5) {
        cc_content_parts_t parts;
        rc = cc_content_parts_from_json("[{\"text\":\"first\"},{\"type\":\"image\",\"artifact\":{\"id\":\"image\",\"mime\":\"image/png\"}}]", &parts);
        if (rc.code != CC_OK) assert(!parts.items && !parts.count);
        cc_content_parts_cleanup(&parts);
    } else if (operation == 6) {
        rc = cc_content_parts_validate(&parts_view, NULL);
    } else if (operation == 7) {
        rc = cc_content_parts_to_json(&parts_view, &output);
    } else if (operation == 8) {
        cc_content_part_t items[] = {item, item};
        parts_view.items = items; parts_view.count = 2;
        cc_content_parts_t copy;
        rc = cc_content_parts_copy(&parts_view, &copy); cc_content_parts_cleanup(&copy);
    } else if (operation == 9) {
        cc_media_artifact_t items[] = {source, source};
        view.items = items; view.count = 2;
        cc_media_artifact_list_t copy;
        rc = cc_media_artifact_list_copy(&view, &copy); cc_media_artifact_list_cleanup(&copy);
    } else {
        cc_media_artifact_list_t list;
        rc = cc_media_artifact_list_from_json(
            "[{\"kind\":\"image\",\"id\":\"first\",\"mime\":\"image/png\"},{\"kind\":\"file\",\"id\":\"second\",\"mime\":\"text/plain\"}]", &list);
        if (rc.code != CC_OK) assert(!list.items && !list.count);
        cc_media_artifact_list_cleanup(&list);
    }
    if (rc.code != CC_OK) assert(output == NULL);
    media_free(output);
    return rc;
}

/**
 * @brief 逐个分配失败点执行全部拥有链，禁止成功半输出并检查最终分配归零。
 * @post 全局 cJSON 钩子恢复，故障状态归零，全部输出已消费。
 */
static void test_allocation_failures(void)
{
    cJSON_Hooks hooks = {media_malloc, media_free};
    cJSON_InitHooks(&hooks);
    for (int operation = 0; operation < 11; operation++) {
        allocation_attempt = 0; failed_attempt = SIZE_MAX;
        cc_result_t rc = allocation_case(operation);
        size_t attempts = allocation_attempt;
        require_success(rc);
        assert(attempts > 0 && !live_allocations);
        for (size_t failure = 0; failure < attempts; failure++) {
            allocation_attempt = 0; failed_attempt = failure;
            rc = allocation_case(operation);
            assert(rc.code != CC_OK);
            assert(rc.code == CC_ERR_OUT_OF_MEMORY ||
                ((operation == 5 || operation == 10) && rc.code == CC_ERR_JSON));
            cc_result_free(&rc);
            assert(!live_allocations);
        }
    }
    failed_attempt = SIZE_MAX;
    cJSON_InitHooks(NULL);
}

/**
 * @brief 执行媒体拥有、别名、形状、严格 JSON 与分配失败回归。
 * @param[in] argc 参数数量，可选 self 或 alias 仅运行对应旧代码负对照入口。
 * @param[in] argv 调用期间借用参数数组。
 * @return 所有断言通过返回零，失败由断言报告并终止。
 */
int main(int argc, char **argv)
{
    memset(long_path, 'p', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    if (argc > 1 && strcmp(argv[1], "self") == 0) test_self_copy();
    else if (argc > 1 && strcmp(argv[1], "alias") == 0) test_alias_append();
    else {
        test_self_copy(); test_alias_append(); test_views_and_invalid_shapes();
        test_json_schema(); test_byte_number_range(); test_allocation_failures();
    }
    assert(!live_allocations);
    puts("media ownership, strict schema and failure cleanup: PASS");
    return 0;
}
