#include "cc/core/cc_result.h"
#include "cc/util/cc_json.h"
#include "cc/util/cc_string_builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief 单线程私有配置副本的分配记录及定点故障开关。 */
static void *live_allocations[512];
static size_t live_count, allocation_count, fail_at;
static int fail_schema_print, fail_append;

/**
 * @brief 记录一次私有分配并判断是否故意失败。
 * @return 当前序号等于非零 fail_at 时为一，其余为零。
 */
static int allocation_fails(void)
{
    allocation_count++;
    return fail_at && allocation_count == fail_at;
}

/**
 * @brief 记录本测试私有配置路径拥有的缓冲。
 * @param[in] pointer 新分配指针，NULL 时跳过。
 * @post 非 NULL 指针加入固定记录数组，记录溢出结束测试进程。
 */
static void track_allocation(void *pointer)
{
    if (!pointer) return;
    if (live_count == sizeof(live_allocations) / sizeof(live_allocations[0])) abort();
    live_allocations[live_count++] = pointer;
}

/**
 * @brief 从私有拥有记录中移除即将交给真实 free 或 realloc 的指针。
 * @param[in] pointer 原缓冲，可为 NULL，非私有来源指针不计入记录。
 * @post 命中记录时删除该项，其余记录继续有效。
 */
static void untrack_allocation(void *pointer)
{
    for (size_t i = 0; i < live_count; i++) {
        if (live_allocations[i] == pointer) {
            live_allocations[i] = live_allocations[--live_count];
            return;
        }
    }
}

/**
 * @brief 注入私有 malloc 故障并登记成功缓冲。
 * @param[in] bytes 申请字节数。
 * @return 故障为 NULL，其余真实 malloc 结果由对应拥有者释放。
 */
static void *test_malloc(size_t bytes)
{
    void *pointer = allocation_fails() ? NULL : malloc(bytes);
    track_allocation(pointer);
    return pointer;
}

/**
 * @brief 注入私有 calloc 故障并登记成功数组。
 * @param[in] count 元素数。
 * @param[in] bytes 元素字节数。
 * @return 故障为 NULL，其余真实 calloc 结果由对应拥有者释放。
 */
static void *test_calloc(size_t count, size_t bytes)
{
    void *pointer = allocation_fails() ? NULL : calloc(count, bytes);
    track_allocation(pointer);
    return pointer;
}

/**
 * @brief 注入私有 realloc 故障，成功后更新拥有记录。
 * @param[in,out] pointer 原拥有缓冲，可为 NULL，失败保持原缓冲及记录。
 * @param[in] bytes 新长度，本测试配置路径始终大于零。
 * @return 故障为 NULL，其余真实 realloc 结果。
 */
static void *test_realloc(void *pointer, size_t bytes)
{
    if (allocation_fails()) return NULL;
    untrack_allocation(pointer);
    void *next = realloc(pointer, bytes);
    track_allocation(next ? next : pointer);
    return next;
}

/**
 * @brief 归还缓冲并更新私有分配记录。
 * @param[in] pointer 消费拥有缓冲，允许 NULL 或其他 SDK 路径交出的缓冲。
 * @post 指针已交真实 free，命中的私有记录已移除。
 */
static void test_free(void *pointer)
{
    untrack_allocation(pointer);
    free(pointer);
}

/**
 * @brief 注入存在参数 schema 的序列化失败。
 * @param[in] value 借用 JSON 节点。
 * @return 开关启用时 NULL，其他情况返回真实序列化拥有正文。
 */
static char *test_stringify(const cc_json_value_t *value)
{
    return fail_schema_print ? NULL : cc_json_stringify_unformatted(value);
}

/**
 * @brief 注入提示词 append 的粘性失败并交付拥有错误消息。
 * @param[in,out] builder 借用真实 builder，故障时设置其 error_code。
 * @param[in] text 借用待追加正文。
 * @return 故障为带拥有消息的 OUT_OF_MEMORY，其他沿真实 append。
 */
static cc_result_t test_append(cc_string_builder_t *builder, const char *text)
{
    if (fail_append) {
        builder->error_code = CC_ERR_OUT_OF_MEMORY;
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Injected prompt append failure");
    }
    return cc_string_builder_append(builder, text);
}

/* 私有重命名编译配置实现，仅此 include 内注入分配与 append。
 * 真实 JSON、文件系统、string builder 和其他生产对象继续沿 SDK 原实现，避免修改公共分配 API。 */
#define malloc test_malloc
#define calloc test_calloc
#define realloc test_realloc
#define free test_free
#define cc_json_stringify_unformatted test_stringify
#define cc_string_builder_append test_append
#define cc_config_load test_config_load
#define cc_config_load_default test_config_load_default
#define cc_config_validate test_config_validate
#define cc_config_destroy test_config_destroy
#define cc_config_build_system_prompt test_config_build_system_prompt
#include "../../core/src/util/cc_config.c"
#undef cc_config_build_system_prompt
#undef cc_config_destroy
#undef cc_config_validate
#undef cc_config_load_default
#undef cc_config_load
#undef cc_string_builder_append
#undef cc_json_stringify_unformatted
#undef free
#undef realloc
#undef calloc
#undef malloc

/**
 * @brief 写入本测试配置夹具。
 * @param[in] path 借用输出路径。
 * @param[in] text 借用正文。
 * @return 完整写入并关闭为一，打开或写入失败为零。
 */
static int write_fixture(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int written = fputs(text, file) >= 0;
    int closed = fclose(file) == 0;
    return written && closed;
}

/**
 * @brief 遍历配置默认值和嵌套字段分配故障，核对部分输出可完整销毁。
 * @return 每个私有分配序号均返回 OOM 且销毁后记录为空为零，其余为一。
 */
static int test_partial_config_cleanup(void)
{
    const char *path = "__c_claw_config_oom__.json";
    const char *json =
        "{\"agents\":{\"list\":[{\"id\":\"one\",\"model\":\"model\",\"workspace\":\"dir\","
        "\"skills\":[\"skill\"]}]},\"tools\":{\"enabled\":[\"one\",\"two\"],"
        "\"perTool\":{\"one\":{\"timeoutMs\":10}}},\"plugins\":{\"entries\":{\"plugin\":{"
        "\"command\":\"command\",\"args\":[\"arg\"],\"tools\":[{\"name\":\"one\","
        "\"description\":\"description\",\"parameters\":{\"type\":\"object\"}}],\"skills\":[\"dir\"]}}},"
        "\"mcp\":{\"servers\":{\"server\":{\"command\":\"command\",\"args\":[\"arg\"],\"cwd\":\"dir\"}}}}";
    if (!write_fixture(path, json)) return 1;
    cc_config_t config = {0};
    cc_result_t rc = test_config_load(path, &config);
    size_t total = allocation_count;
    int failed = rc.code != CC_OK;
    cc_result_free(&rc);
    test_config_destroy(&config);
    if (live_count) failed = 1;
    for (size_t index = 1; index <= total && !failed; index++) {
        allocation_count = 0;
        fail_at = index;
        rc = test_config_load(path, &config);
        fail_at = 0;
        if (rc.code != CC_ERR_OUT_OF_MEMORY) failed = 1;
        cc_result_free(&rc);
        test_config_destroy(&config);
        if (live_count) failed = 1;
    }
    allocation_count = 0;
    fail_schema_print = 1;
    rc = test_config_load(path, &config);
    fail_schema_print = 0;
    if (rc.code != CC_ERR_OUT_OF_MEMORY) failed = 1;
    cc_result_free(&rc);
    test_config_destroy(&config);
    if (live_count) failed = 1;
    remove(path);
    return failed;
}

/**
 * @brief 检查提示词 append 失败返回原拥有诊断，并保持输出为空。
 * @return 故障及成功对照均匹配为零，任一失败为一，文件与正文均清理。
 */
static int test_prompt_append_failure(void)
{
    const char *path = "__c_claw_prompt_oom__.txt";
    if (!write_fixture(path, "soul")) return 1;
    cc_config_t config = {0};
    config.soul_file = (char *)path;
    config.user_file = (char *)"__c_claw_missing_prompt__.txt";
    char *prompt = NULL;
    fail_append = 1;
    cc_result_t rc = test_config_build_system_prompt(&config, &prompt);
    fail_append = 0;
    int failed = rc.code != CC_ERR_OUT_OF_MEMORY || prompt != NULL ||
        !rc.message || strcmp(rc.message, "Injected prompt append failure") != 0;
    cc_result_free(&rc);
    free(prompt);
    rc = test_config_build_system_prompt(&config, &prompt);
    failed |= rc.code != CC_OK || !prompt || strcmp(prompt, "soul\n\n") != 0;
    cc_result_free(&rc);
    free(prompt);
    remove(path);
    return failed;
}

/**
 * @brief 运行私有配置副本的部分分配、schema 和提示词故障回归。
 * @return 全部通过为零，失败为一，最终私有分配记录须为空。
 */
int main(void)
{
    if (test_partial_config_cleanup()) return 1;
    if (test_prompt_append_failure()) return 1;
    return live_count ? 1 : 0;
}
