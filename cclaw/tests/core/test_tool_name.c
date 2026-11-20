#include "cc/util/cc_tool_name.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "check failed at %d\n", __LINE__); return 1; } } while (0)

#ifdef CC_TEST_TOOL_NAME_OOM
static int fail_next_allocation;

/**
 * @brief 声明分配包装调用的真实 malloc。
 * @param[in] size 请求的字节数。
 * @return 拥有内存或 NULL，最终按 malloc/free 契约归还。
 */
void *__real_malloc(size_t size);

/**
 * @brief 在单线程 helper 回归中故意拒绝下一次输出分配。
 * @param[in] size 传给真实 malloc 的字节数。
 * @return 故障开关消费一次时返回 NULL，否则返回真实拥有分配。
 */
void *__wrap_malloc(size_t size)
{
    if (fail_next_allocation) {
        fail_next_allocation = 0;
        return NULL;
    }
    return __real_malloc(size);
}
#endif

/**
 * @brief 消费结果诊断并检查是否成功。
 * @param[in] rc 按值移交的拥有结果，原副本消息指针随后失效。
 * @return CC_OK 为一，其他为零。
 */
static int ok(cc_result_t rc)
{
    int success = rc.code == CC_OK;
    cc_result_free(&rc);
    return success;
}

/**
 * @brief 检查两个组件分别编码，点号和原始百分号序列保持不同身份。
 * @return 全部逐字节对照成功零，失败一，每次成功输出在本函数释放。
 */
static int test_build(void)
{
    const struct {
        const char *namespace_name;
        const char *source;
        const char *tool;
        const char *expected;
    } cases[] = {
        { "mcp", "a", "b.c", "mcp.a.b%2Ec" },
        { "mcp", "a.b", "c", "mcp.a%2Eb.c" },
        { "mcp", "a%2Eb", "c", "mcp.a%252Eb.c" },
        { "mcp", "a", "%2E", "mcp.a.%252E" },
        { "mcp", "a", ".", "mcp.a.%2E" },
        { "plugin", "a.b", "c%d.e", "plugin.a%2Eb.c%25d%2Ee" },
        { "mcp", "plain", "echo", "mcp.plain.echo" },
        { "mcp", "", NULL, "mcp." },
        { "mcp", "", "", "mcp.." },
        { "plugin", "源.一%", "工具.甲%", "plugin.源%2E一%25.工具%2E甲%25" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *name = NULL;
        CHECK(ok(cc_tool_name_build(cases[i].namespace_name, cases[i].source, cases[i].tool, &name)));
        CHECK(name && strcmp(name, cases[i].expected) == 0);
        free(name);
    }
    return 0;
}

/**
 * @brief 检查来源 lane 的完整编码组件及普通工具全名兼容。
 * @return 全部对照成功零，失败一，成功输出逐次 free。
 */
static int test_lane(void)
{
    const struct {
        const char *name;
        const char *expected;
    } cases[] = {
        { "mcp.a.b%2Ec", "mcp.a" },
        { "mcp.a%2Eb.c", "mcp.a%2Eb" },
        { "mcp.a%252Eb.c", "mcp.a%252Eb" },
        { "plugin.a%2Eb.c%25d%2Ee", "plugin.a%2Eb" },
        { "plugin.a%2Eb", "plugin.a%2Eb" },
        { "http.request", "tool.http.request" },
        { "tool.local", "tool.tool.local" },
        { NULL, "tool.unknown" },
        { "", "tool.unknown" },
        { "mcp.", "mcp." },
        { "mcp..", "mcp." },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *lane = NULL;
        CHECK(ok(cc_tool_name_lane(cases[i].name, &lane)));
        CHECK(lane && strcmp(lane, cases[i].expected) == 0);
        free(lane);
    }
    return 0;
}

/**
 * @brief 检查必填参数及结构前缀格式，失败时已有输出槽先清空。
 * @return 全部错误码及空输出匹配为零，失败一，拥有诊断逐次释放。
 */
static int test_invalid_arguments(void)
{
    const char *invalid_namespaces[] = { NULL, "", "mcp.bad", "mcp%bad" };
    for (size_t i = 0; i < sizeof(invalid_namespaces) / sizeof(invalid_namespaces[0]); i++) {
        char *name = (char *)1;
        cc_result_t rc = cc_tool_name_build(invalid_namespaces[i], "source", "tool", &name);
        CHECK(rc.code == CC_ERR_INVALID_ARGUMENT && name == NULL);
        cc_result_free(&rc);
    }
    char *name = (char *)1;
    cc_result_t rc = cc_tool_name_build("mcp", NULL, "tool", &name);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT && name == NULL);
    cc_result_free(&rc);
    rc = cc_tool_name_build("mcp", "source", "tool", NULL);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT);
    cc_result_free(&rc);
    rc = cc_tool_name_lane("name", NULL);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT);
    cc_result_free(&rc);
    return 0;
}

/**
 * @brief 检查超过旧固定缓冲的组件不截断，构建 lane 与提取 lane 完全一致。
 * @return 长度、内容及 NUL 检查成功零，失败一，拥有输出均在本函数释放。
 */
static int test_long_components(void)
{
    char source[601];
    char tool[701];
    memset(source, 's', sizeof(source) - 1U);
    memset(tool, 't', sizeof(tool) - 1U);
    source[300] = '.';
    source[500] = '%';
    tool[300] = '.';
    tool[500] = '%';
    source[sizeof(source) - 1U] = '\0';
    tool[sizeof(tool) - 1U] = '\0';
    char *name = NULL;
    char *built_lane = NULL;
    char *extracted_lane = NULL;
    char *ordinary_lane = NULL;
    CHECK(ok(cc_tool_name_build("mcp", source, tool, &name)));
    CHECK(strlen(name) == 1313U && name[1313] == '\0');
    CHECK(ok(cc_tool_name_build("mcp", source, NULL, &built_lane)));
    CHECK(ok(cc_tool_name_lane(name, &extracted_lane)));
    CHECK(strlen(built_lane) == 608U && strcmp(built_lane, extracted_lane) == 0);
    CHECK(ok(cc_tool_name_lane(tool, &ordinary_lane)));
    CHECK(strlen(ordinary_lane) == 705U && strcmp(ordinary_lane + 5U, tool) == 0);
    free(name);
    free(built_lane);
    free(extracted_lane);
    free(ordinary_lane);
    return 0;
}

#ifdef CC_TEST_TOOL_NAME_OOM
/**
 * @brief 逐项拒绝 build、扩展 lane 和普通 lane 的唯一输出分配。
 * @return 全部返回 OOM 且清空输出为零，失败一，故障开关消费后诊断按真实分配释放。
 */
static int test_output_allocation_failures(void)
{
    char *name = (char *)1;
    fail_next_allocation = 1;
    cc_result_t rc = cc_tool_name_build("mcp", "source", "tool", &name);
    CHECK(rc.code == CC_ERR_OUT_OF_MEMORY && name == NULL && !fail_next_allocation);
    cc_result_free(&rc);
    const char *names[] = { "mcp.source.tool", "ordinary.tool" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        name = (char *)1;
        fail_next_allocation = 1;
        rc = cc_tool_name_lane(names[i], &name);
        CHECK(rc.code == CC_ERR_OUT_OF_MEMORY && name == NULL && !fail_next_allocation);
        cc_result_free(&rc);
    }
    return 0;
}
#endif

/**
 * @brief 运行组件编码、lane、参数、长组件及可选输出分配故障回归。
 * @return 全部成功零，任一检查失败一，测试只使用本地字符串与拥有结果。
 */
int main(void)
{
    if (test_build() || test_lane() || test_invalid_arguments() || test_long_components()) return 1;
#ifdef CC_TEST_TOOL_NAME_OOM
    if (test_output_allocation_failures()) return 1;
#endif
    return 0;
}
