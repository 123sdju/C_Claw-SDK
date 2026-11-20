#include "cc/adapters/cc_builtin_tools.h"
#include "cc/util/cc_json.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "CHECK %d: %s\n", __LINE__, #condition); return 1; } } while (0)

typedef struct {
    int reads;
    int writes;
    int fail_read;
    int fail_write;
    int owned_success;
    int confirmation_oom;
    char path[128];
    char content[128];
} fixture_t;

#ifdef CC_TEST_FILE_TOOL_OOM
/** @brief 使用真实分配器。 @param[in] size 请求字节数。 @return 拥有缓冲或 NULL。 */
void *__real_malloc(size_t size);
/** @brief 使用真实零分配器。 @param[in] count 元素数。 @param[in] size 元素字节数。 @return 拥有缓冲或 NULL。 */
void *__real_calloc(size_t count, size_t size);
/** @brief 归还真实分配。 @param[in,out] pointer 拥有地址，可空。 @post 地址归还。 */
void __real_free(void *pointer);
static int fail_malloc;
static int fail_calloc;
static void *watched[2];
static int released[2];

/** @brief 注入一次 malloc 失败。 @param[in] size 请求字节数。 @return 拥有缓冲或 NULL，故障开关随后清除。 */
void *__wrap_malloc(size_t size)
{
    if (fail_malloc) { fail_malloc = 0; return NULL; }
    return __real_malloc(size);
}
/** @brief 注入一次 calloc 失败。 @param[in] count 元素数。 @param[in] size 元素字节数。 @return 拥有缓冲或 NULL，故障开关随后清除。 */
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_calloc) { fail_calloc = 0; return NULL; }
    return __real_calloc(count, size);
}
/** @brief 计数目标释放并转交真实 free。 @param[in,out] pointer 拥有地址，可空。 @post 分配归还，匹配地址的计数增加。 */
void __wrap_free(void *pointer)
{
    for (size_t i = 0; i < 2; i++) {
        if (pointer && pointer == watched[i]) released[i]++;
    }
    __real_free(pointer);
}
#endif

static size_t json_allocations;
static size_t json_fail_at;
static size_t json_live;

/** @brief 注入 cJSON 单点分配失败。 @param[in] size 请求字节数。 @return 拥有缓冲或 NULL，成功登记存活数。 */
static void *json_allocate(size_t size)
{
    json_allocations++;
    if (json_fail_at && json_allocations == json_fail_at) return NULL;
    void *pointer = malloc(size);
    if (pointer) json_live++;
    return pointer;
}
/** @brief 归还 cJSON 内存。 @param[in,out] pointer 本钩子拥有地址，可空。 @post 存活计数减少，内存归还。 */
static void json_release(void *pointer)
{
    if (pointer) json_live--;
    free(pointer);
}
/** @brief 复制 fixture 文本。 @param[in] text 必填文本借用。 @return 拥有副本或 NULL，接收方 free。 */
static char *copy_text(const char *text)
{
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

/**
 * @brief 捕获根相对读取，返回成功正文或带部分缓冲的失败。
 * @param[in,out] self fixture 借用，记录次数和路径。
 * @param[in] root 调用期根路径，须为预期根。
 * @param[in] path 调用期相对路径借用。
 * @param[out] out_text 必填空槽，正文或失败部分分配由工具归还。
 * @return fixture 选择的成功或 IO，拥有诊断由工具消费。
 */
static cc_result_t read_relative(void *self, const char *root, const char *path, char **out_text)
{
    fixture_t *fixture = self;
    fixture->reads++;
    snprintf(fixture->path, sizeof(fixture->path), "%s", path);
    if (strcmp(root, "fixture-root") != 0) return cc_result_error(CC_ERR_IO, "Wrong root");
    *out_text = copy_text("file contents");
    if (fixture->fail_read) {
        cc_result_t rc = cc_result_error(CC_ERR_IO, "Read failed after allocation");
#ifdef CC_TEST_FILE_TOOL_OOM
        watched[0] = *out_text;
        watched[1] = rc.message;
#endif
        return rc;
    }
    if (fixture->owned_success) {
        cc_result_t rc = cc_result_error(CC_OK, "Owned successful read diagnostic");
#ifdef CC_TEST_FILE_TOOL_OOM
        watched[1] = rc.message;
#endif
        return rc;
    }
    return cc_result_ok();
}

/**
 * @brief 捕获写入并按 fixture 注入原生失败或写后确认 OOM。
 * @param[in,out] self fixture 借用，记录次数、路径和正文。
 * @param[in] root 调用期根目录，须匹配 fixture。
 * @param[in] path 调用期相对路径借用。
 * @param[in] text 调用期正文借用。
 * @return 成功或 IO，可能附带拥有诊断，工具消费。
 */
static cc_result_t write_relative(void *self, const char *root, const char *path, const char *text)
{
    fixture_t *fixture = self;
    fixture->writes++;
    snprintf(fixture->path, sizeof(fixture->path), "%s", path);
    snprintf(fixture->content, sizeof(fixture->content), "%s", text);
    if (strcmp(root, "fixture-root") != 0 || fixture->fail_write)
        return cc_result_error(CC_ERR_IO, "Write may already have changed the file");
#ifdef CC_TEST_FILE_TOOL_OOM
    if (fixture->confirmation_oom) fail_malloc = 1;
#endif
    return fixture->owned_success ? cc_result_error(CC_OK, "Owned successful write diagnostic") : cc_result_ok();
}

static const cc_filesystem_vtable_t filesystem_vtable = {
    .read_relative_text = read_relative,
    .write_relative_text = write_relative
};

/** @brief 核对直接工具入口的严格参数与完整原文传递。 @param[in] read 读取工具借用。 @param[in] write 写入工具借用。 @param[in,out] fixture 主测试状态。 @param[in] context 稳定上下文借用。 @return 全部通过为零，失败为一。 */
static int test_parameters(cc_tool_t read, cc_tool_t write, fixture_t *fixture, const cc_tool_context_t *context)
{
    const char *invalid[] = {
        NULL, "{", "[]", "{} tail", "{\"path\":1}",
        "{\"path\":\"x\\u0000hidden\"}",
        "{\"path\":\"first\",\"PATH\":\"second\"}"
    };
    cc_tool_result_t result;
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        int before = fixture->reads;
        cc_result_t rc = read.vtable->call(read.self, invalid[i], context, &result);
        CHECK(rc.code == CC_OK && !result.ok && result.error && fixture->reads == before);
        cc_result_free(&rc);
        cc_tool_result_cleanup(&result);
    }
    const char *bad_write[] = {
        "{\"path\":\"x\",\"content\":false}",
        "{\"path\":\"x\",\"content\":\"a\\u0000hidden\"}",
        "{\"path\":\"x\",\"content\":\"a\",\"CONTENT\":\"b\"}",
        "{\"path\":\"x\",\"content\":\"a\"} tail"
    };
    for (size_t i = 0; i < sizeof(bad_write) / sizeof(bad_write[0]); i++) {
        int before = fixture->writes;
        cc_result_t rc = write.vtable->call(write.self, bad_write[i], context, &result);
        CHECK(rc.code == CC_OK && !result.ok && result.error && fixture->writes == before);
        cc_result_free(&rc);
        cc_tool_result_cleanup(&result);
    }
    cc_result_t rc = read.vtable->call(read.self, "{\"PATH\":\"nested/file\"}", context, &result);
    CHECK(rc.code == CC_OK && result.ok && strcmp(result.text, "file contents") == 0);
    CHECK(strcmp(fixture->path, "nested/file") == 0);
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    rc = write.vtable->call(write.self, "{\"PATH\":\"nested/file\",\"CONTENT\":\"literal \\\\u0000\"}", context, &result);
    CHECK(rc.code == CC_OK && result.ok && strcmp(fixture->content, "literal \\u0000") == 0);
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    return 0;
}

/** @brief 核对缺失能力、空地址、部分失败和成功诊断归还。 @param[in] read 读取工具借用。 @param[in] write 写入工具借用。 @param[in,out] fixture 主测试状态。 @param[in] context 稳定上下文借用。 @return 全部通过为零，失败为一。 */
static int test_failures(cc_tool_t read, cc_tool_t write, fixture_t *fixture, const cc_tool_context_t *context)
{
    cc_tool_result_t result;
    cc_result_t rc = read.vtable->call(NULL, "{}", context, &result);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT && !result.text && !result.error);
    cc_result_free(&rc);
    rc = write.vtable->call(write.self, "{}", context, NULL);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT);
    cc_result_free(&rc);
    cc_tool_t unsupported = {0};
    rc = cc_file_read_tool_create((cc_filesystem_t){0}, &unsupported);
    CHECK(rc.code == CC_OK);
    cc_result_free(&rc);
    rc = unsupported.vtable->call(unsupported.self, "{\"path\":\"x\"}", context, &result);
    CHECK(rc.code == CC_OK && !result.ok && result.error);
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    unsupported.vtable->destroy(unsupported.self);
    fixture->fail_read = 1;
    rc = read.vtable->call(read.self, "{\"path\":\"x\"}", context, &result);
    CHECK(rc.code == CC_OK && !result.ok && strstr(result.error, "Read failed"));
    cc_result_free(&rc);
#ifdef CC_TEST_FILE_TOOL_OOM
    CHECK(released[0] == 1 && released[1] == 1);
    watched[0] = watched[1] = NULL;
    released[0] = released[1] = 0;
#endif
    cc_tool_result_cleanup(&result);
    fixture->fail_read = 0;
    fixture->fail_write = 1;
    rc = write.vtable->call(write.self, "{\"path\":\"x\",\"content\":\"data\"}", context, &result);
    CHECK(rc.code == CC_OK && !result.ok && strstr(result.error, "already have changed"));
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    fixture->fail_write = 0;
    fixture->owned_success = 1;
    rc = read.vtable->call(read.self, "{\"path\":\"x\"}", context, &result);
    CHECK(rc.code == CC_OK && result.ok);
    cc_result_free(&rc);
#ifdef CC_TEST_FILE_TOOL_OOM
    CHECK(released[1] == 1);
    watched[1] = NULL;
    released[1] = 0;
#endif
    cc_tool_result_cleanup(&result);
    rc = write.vtable->call(write.self, "{\"path\":\"x\",\"content\":\"\"}", context, &result);
    CHECK(rc.code == CC_OK && result.ok && !fixture->content[0]);
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    fixture->owned_success = 0;
#ifdef CC_TEST_FILE_TOOL_OOM
    fail_malloc = 1;
    rc = read.vtable->call(read.self, NULL, context, &result);
    CHECK(rc.code == CC_ERR_OUT_OF_MEMORY && !result.error && !result.text && !fail_malloc);
    cc_result_free(&rc);
    fixture->confirmation_oom = 1;
    int before = fixture->writes;
    rc = write.vtable->call(write.self, "{\"path\":\"x\",\"content\":\"written\"}", context, &result);
    CHECK(rc.code == CC_ERR_OUT_OF_MEMORY && fixture->writes == before + 1 && !result.ok && !result.text);
    CHECK(rc.message && strstr(rc.message, "File was written"));
    cc_result_free(&rc);
    fixture->confirmation_oom = 0;
#endif
    return 0;
}

/** @brief 逐 cJSON 分配故障核对整次参数拒绝和树释放。 @param[in] tool 读取工具借用。 @param[in] context 稳定上下文借用。 @return 全部通过为零，失败为一，每轮退出工具调用后恢复钩子。 */
static int test_json_oom(cc_tool_t tool, const cc_tool_context_t *context)
{
    cJSON_Hooks hooks = {json_allocate, json_release};
    cc_tool_result_t result;
    json_allocations = 0;
    cJSON_InitHooks(&hooks);
    cc_result_t rc = tool.vtable->call(tool.self, "{\"path\":\"x\"}", context, &result);
    cJSON_InitHooks(NULL);
    CHECK(rc.code == CC_OK && result.ok && !json_live);
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    size_t points = json_allocations;
    for (size_t i = 1; i <= points; i++) {
        json_allocations = 0;
        json_fail_at = i;
        cJSON_InitHooks(&hooks);
        rc = tool.vtable->call(tool.self, "{\"path\":\"x\"}", context, &result);
        cJSON_InitHooks(NULL);
        CHECK(rc.code == CC_OK && !result.ok && result.error && !json_live);
        cc_result_free(&rc);
        cc_tool_result_cleanup(&result);
    }
    json_fail_at = 0;
    return 0;
}

/** @brief 创建借用 fixture 文件系统的两工具并顺序执行契约。 @return 全部通过为零，失败为一，成功收尾销毁工具状态。 */
int main(void)
{
    fixture_t fixture = {0};
    cc_filesystem_t filesystem = {0};
    filesystem.self = &fixture;
    filesystem.vtable = &filesystem_vtable;
    filesystem.capabilities = CC_FS_CAP_ROOT_SCOPED;
    cc_tool_t read = {0}, write = {0};
    cc_result_t rc = cc_file_read_tool_create(filesystem, &read);
    CHECK(rc.code == CC_OK);
    cc_result_free(&rc);
    rc = cc_file_write_tool_create(filesystem, &write);
    CHECK(rc.code == CC_OK);
    cc_result_free(&rc);
    cc_tool_context_t context = {0};
    context.workspace_dir = "fixture-root";
    context.max_result_bytes = 1;
    CHECK(test_parameters(read, write, &fixture, &context) == 0);
    CHECK(test_failures(read, write, &fixture, &context) == 0);
    CHECK(test_json_oom(read, &context) == 0);
#ifdef CC_TEST_FILE_TOOL_OOM
    cc_tool_t failed = {(void *)1, (const cc_tool_vtable_t *)1};
    fail_calloc = 1;
    rc = cc_file_read_tool_create(filesystem, &failed);
    CHECK(rc.code == CC_ERR_OUT_OF_MEMORY && !failed.self && !failed.vtable && !fail_calloc);
    cc_result_free(&rc);
    fail_calloc = 1;
    rc = cc_file_write_tool_create(filesystem, &failed);
    CHECK(rc.code == CC_ERR_OUT_OF_MEMORY && !failed.self && !failed.vtable && !fail_calloc);
    cc_result_free(&rc);
#endif
    read.vtable->destroy(read.self);
    write.vtable->destroy(write.self);
    return 0;
}
