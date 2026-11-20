#include "cc/ports/cc_memory_tool_factory.h"
#include <stdlib.h>
#include <string.h>

static int fail_next_allocation;
static int fail_next_growth;

/** @brief 为私有包含的工具与构造器注入一次分配失败。 @param[in] size 所需字节数。 @return 拥有分配或指定失败的 NULL。 */
static void *budget_malloc(size_t size)
{
    if (fail_next_allocation) { fail_next_allocation = 0; return NULL; }
    return malloc(size);
}

/** @brief 为工具实例注入一次清零分配失败。 @param[in] count 元素数。 @param[in] size 元素字节。 @return 拥有分配或 NULL。 */
static void *budget_calloc(size_t count, size_t size)
{
    if (fail_next_allocation) { fail_next_allocation = 0; return NULL; }
    return calloc(count, size);
}

/** @brief 在实际构造器扩容处失败并保留旧块。 @param[in,out] data 原独占分配。 @param[in] size 新容量字节。 @return 新块或指定失败的 NULL。 */
static void *budget_realloc(void *data, size_t size)
{
    if (fail_next_growth) { fail_next_growth = 0; return NULL; }
    return realloc(data, size);
}

/* 私有重命名包含实际构造器和工具，只替换该翻译单元分配，JSON 解析及端口仍使用真实 SDK。 */
#define malloc budget_malloc
#define calloc budget_calloc
#define realloc budget_realloc
#define cc_string_builder_init budget_sb_init
#define cc_string_builder_append budget_sb_append
#define cc_string_builder_appendf budget_sb_appendf
#define cc_string_builder_append_char budget_sb_append_char
#define cc_string_builder_take budget_sb_take
#define cc_string_builder_deinit budget_sb_deinit
#define cc_string_builder_clear budget_sb_clear
#define cc_string_builder_cstr budget_sb_cstr
#define cc_string_builder_error budget_sb_error
#include "../../core/src/util/cc_string_builder.c"
#define cc_memory_tool_create budget_tool_create
#define cc_memory_tool_create_readonly budget_tool_create_readonly
#include "../../adapters/src/tools/common/cc_memory_tool.c"
#undef cc_memory_tool_create
#undef cc_memory_tool_create_readonly
#undef cc_string_builder_init
#undef cc_string_builder_append
#undef cc_string_builder_appendf
#undef cc_string_builder_append_char
#undef cc_string_builder_take
#undef cc_string_builder_deinit
#undef cc_string_builder_clear
#undef cc_string_builder_cstr
#undef cc_string_builder_error
#undef malloc
#undef calloc
#undef realloc

static int fail_search;
static int fail_set;
static int fail_delete;
static int set_calls;
static int delete_calls;
/**
 * @brief 记录写入调用并交付可控后端错误。
 * @param[in] self 忽略，可为 NULL。
 * @param[in] key 键借用，仅调用期有效。
 * @param[in] value 正文借用。
 * @param[in] category 可选分类借用。
 * @param[in] session_id 可选来源借用。
 * @return 指定时交付 IO，否则成功，诊断由调用方消费。
 */
static cc_result_t set(void *self, const char *key, const char *value,
                       const char *category, const char *session_id)
{
    (void)self; (void)key; (void)value; (void)category; (void)session_id;
    set_calls++;
    return fail_set ? cc_result_error(CC_ERR_IO, "test write failure") : cc_result_ok();
}
/** @brief 记录删除并模拟后端失败。 @param[in] self 忽略。 @param[in] key 借用键。 @return STORAGE 或成功拥有结果。 */
static cc_result_t delete_entry(void *self, const char *key)
{
    (void)self; (void)key;
    delete_calls++;
    return fail_delete ? cc_result_error(CC_ERR_STORAGE, "test delete failure") : cc_result_ok();
}
/**
 * @brief 构造含长 UTF-8 正文的有限拥有数组，供检索和列表复用。
 * @param[in] self 忽略，可为 NULL。
 * @param[in] query 忽略，列表调用时可为 NULL。
 * @param[in] limit 工具保证 1 至 20。
 * @param[out] entries 空拥有槽，失败可能保留 calloc 初始化的部分数组供 wrapper 清理。
 * @param[out] count 可安全释放的条目数，首次分配前置零。
 * @return 可控 IO、分配 OOM 或成功，结果及数组由调用方释放。
 */
static cc_result_t search(void *self, const char *query, int limit,
                          cc_memory_entry_t **entries, size_t *count)
{
    (void)self; (void)query;
    *entries = NULL;
    *count = 0;
    if (fail_search) return cc_result_error(CC_ERR_IO, "test storage failure");
    *entries = calloc((size_t)limit, sizeof(**entries));
    if (!*entries) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM");
    *count = (size_t)limit;
    for (int i = 0; i < limit; i++) {
        (*entries)[i].key = cc_copy_string("test-key");
        (*entries)[i].value = malloc(6001);
        if (!(*entries)[i].key || !(*entries)[i].value)
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM");
        for (int j = 0; j < 6000; j += 3)
            memcpy((*entries)[i].value + j, "\xe6\xb5\x8b", 3);
        (*entries)[i].value[6000] = 0;
    }
    return cc_result_ok();
}

/** @brief 核对本夹具的 ASCII 和“测”字节序列完整性。 @param[in] s 借用 NUL 结尾结果。 @return 全部符合夹具编码为一，截断或其他多字节序列为零。 */
static int valid_utf8(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x80) { p++; continue; }
        if (*p++ != 0xe6 || *p++ != 0xb5 || *p++ != 0x8b) return 0;
    }
    return 1;
}

/**
 * @brief 校验直接调用的参数地址、JSON 边界、字段类型及未知操作。
 * @param[in] tool 借用存活工具。
 * @return 全部断言式检查通过返回零，失败返回一。
 */
static int test_argument_boundaries(cc_tool_t *tool)
{
    cc_tool_result_t result = {0};
    cc_result_t rc = tool->vtable->call(tool->self, "{}", NULL, NULL);
    int failed = rc.code != CC_ERR_INVALID_ARGUMENT;
    cc_result_free(&rc);
    rc = tool->vtable->call(NULL, "{}", NULL, &result);
    failed |= rc.code != CC_ERR_INVALID_ARGUMENT || result.ok || result.text || result.error;
    cc_result_free(&rc);
    const char *bad[] = {
        "[]", "{\"operation\":\"recall\"} trailing", "{\"operation\":\"unknown\"}",
        "{\"operation\":\"recall\\u0000store\"}",
        "{\"operation\":\"store\",\"Operation\":\"recall\",\"key\":\"k\",\"value\":\"v\"}",
        "{\"operation\":\"recall\",\"query\":false}",
        "{\"operation\":\"store\",\"key\":\"k\",\"value\":\"v\",\"category\":[]}",
        "{\"operation\":\"recall\",\"limit\":1.5}",
        "{\"operation\":\"recall\",\"limit\":1e400}"
    };
    int previous_sets = set_calls, previous_deletes = delete_calls;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        rc = tool->vtable->call(tool->self, bad[i], NULL, &result);
        failed |= rc.code != CC_OK || result.ok || !result.error;
        cc_result_free(&rc);
        cc_tool_result_cleanup(&result);
    }
    failed |= previous_sets != set_calls || previous_deletes != delete_calls;
    rc = tool->vtable->call(tool->self, "{\"Operation\":\"recall\",\"query\":\"literal\\\\u0000\"}", NULL, &result);
    failed |= rc.code != CC_OK || !result.ok || !result.text;
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    return failed ? 1 : 0;
}

/**
 * @brief 在错误副本、初始化和实际扩容点验证 OOM，写入后失败保持错误状态。
 * @param[in] tool 借用存活读写工具。
 * @return 全部检查通过返回零，失败返回一，所有诊断和结果均归还。
 */
static int test_allocation_failures(cc_tool_t *tool)
{
    cc_tool_result_t result = {0};
    fail_next_allocation = 1;
    cc_result_t rc = tool->vtable->call(tool->self, "{}", NULL, &result);
    int failed = rc.code != CC_ERR_OUT_OF_MEMORY || result.ok || result.error;
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    fail_next_allocation = 1;
    rc = tool->vtable->call(tool->self, "{\"operation\":\"recall\"}", NULL, &result);
    failed |= rc.code != CC_ERR_OUT_OF_MEMORY || result.ok;
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    fail_next_growth = 1;
    rc = tool->vtable->call(tool->self, "{\"operation\":\"recall\",\"limit\":20}", NULL, &result);
    failed |= rc.code != CC_ERR_OUT_OF_MEMORY || result.ok || result.text;
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    char args[4200];
    strcpy(args, "{\"operation\":\"store\",\"key\":\"k\",\"value\":\"");
    size_t prefix = strlen(args);
    memset(args + prefix, 'v', 4000);
    strcpy(args + prefix + 4000, "\"}");
    int previous_sets = set_calls;
    fail_next_growth = 1;
    rc = tool->vtable->call(tool->self, args, NULL, &result);
    failed |= rc.code != CC_ERR_OUT_OF_MEMORY || result.ok || result.text || set_calls != previous_sets + 1;
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    return failed ? 1 : 0;
}

/** @brief 核对真实工厂的无效、未知与禁用入口清空输出。 @return 全部检查通过为零，所有诊断均释放。 */
static int test_factory_failures(void)
{
    cc_memory_store_t store = {.self = (void *)1};
    cc_result_t rc = cc_memory_store_factory_create(&store, NULL, NULL);
    int failed = rc.code != CC_ERR_INVALID_ARGUMENT || store.self || store.vtable;
    cc_result_free(&rc);
    rc = cc_memory_store_factory_create(&store, "unknown", NULL);
    failed |= rc.code != CC_ERR_INVALID_ARGUMENT || store.self || store.vtable;
    cc_result_free(&rc);
    rc = cc_memory_store_factory_create(&store, "noop", NULL);
    failed |= rc.code != CC_ERR_PLATFORM || store.self || store.vtable;
    cc_result_free(&rc);
    return failed ? 1 : 0;
}

/** @brief 运行预算、只读策略、错误透传、工厂和确定 OOM 回归。 @return 全部检查通过为零，失败返回对应阶段编号。 */
int main(void)
{
    cc_memory_store_vtable_t vtable = {0};
    vtable.set = set;
    vtable.delete_entry = delete_entry;
    vtable.search = search;
    vtable.list = search;
    cc_memory_store_t store = {0};
    store.vtable = &vtable;
    cc_tool_t tool = {0};
    cc_result_t rc = budget_tool_create(&store, &tool);
    if (rc.code != CC_OK) return 1;
    cc_result_free(&rc);
    if (test_argument_boundaries(&tool) || test_allocation_failures(&tool) || test_factory_failures()) return 14;
    size_t budgets[] = {1, 12, 70, 120, 256, 1024, 8192, 16384};
    const char *operations[] = {
        "{\"operation\":\"recall\",\"query\":\"test\",\"limit\":20}",
        "{\"operation\":\"list\",\"limit\":20}"
    };
    for (size_t op = 0; op < 2; op++) {
        for (size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++) {
            cc_tool_context_t ctx = {0};
            ctx.max_result_bytes = budgets[i];
            cc_tool_result_t result = {0};
            rc = tool.vtable->call(tool.self, operations[op], &ctx, &result);
            if (rc.code != CC_OK || !result.ok || !result.text ||
                strlen(result.text) > budgets[i] || !valid_utf8(result.text)) return 2;
            if (budgets[i] >= 256 && !strstr(result.text, "Truncated")) return 3;
            cc_result_free(&rc);
            cc_tool_result_cleanup(&result);
        }
    }
    fail_search = 1;
    cc_tool_result_t result = {0};
    rc = tool.vtable->call(tool.self, operations[0], NULL, &result);
    if (rc.code == CC_OK || result.ok) return 4;
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    const char *write = "{\"operation\":\"store\",\"key\":\"k\",\"value\":\"v\"}";
    const char *forget = "{\"operation\":\"forget\",\"key\":\"k\"}";
    fail_set = 1;
    rc = tool.vtable->call(tool.self, write, NULL, &result);
    if (rc.code != CC_ERR_IO || result.ok || !rc.message ||
        strcmp(rc.message, "test write failure") != 0) return 5;
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    fail_delete = 1;
    rc = tool.vtable->call(tool.self, forget, NULL, &result);
    if (rc.code != CC_ERR_STORAGE || result.ok || !rc.message ||
        strcmp(rc.message, "test delete failure") != 0) return 6;
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    fail_set = fail_delete = 0;
    rc = tool.vtable->call(tool.self, write, NULL, &result);
    if (rc.code != CC_OK || !result.ok || !result.text ||
        !strstr(result.text, "Memory stored")) return 7;
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    rc = tool.vtable->call(tool.self, forget, NULL, &result);
    if (rc.code != CC_OK || !result.ok || !result.text ||
        !strstr(result.text, "Memory forgotten")) return 8;
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    tool.vtable->destroy(tool.self);
    memset(&tool,0,sizeof(tool));
    rc=budget_tool_create_readonly(&store,&tool);
    if (rc.code!=CC_OK) return 9;
    cc_result_free(&rc);
    const char *schema=tool.vtable->schema_json(tool.self);
    if (!schema || !strstr(schema,"\"recall\"") || !strstr(schema,"\"list\"") ||
        strstr(schema,"\"store\"") || strstr(schema,"\"forget\"")) return 10;
    int previous_sets=set_calls, previous_deletes=delete_calls;
    rc=tool.vtable->call(tool.self,write,NULL,&result);
    if (rc.code!=CC_OK || result.ok || !result.error ||
        set_calls!=previous_sets || delete_calls!=previous_deletes) return 11;
    cc_result_free(&rc); cc_tool_result_cleanup(&result);
    rc=tool.vtable->call(tool.self,forget,NULL,&result);
    if (rc.code!=CC_OK || result.ok || !result.error ||
        set_calls!=previous_sets || delete_calls!=previous_deletes) return 12;
    cc_result_free(&rc); cc_tool_result_cleanup(&result);
    fail_search=0;
    rc=tool.vtable->call(tool.self,operations[0],NULL,&result);
    if (rc.code!=CC_OK || !result.ok || !result.text) return 13;
    cc_result_free(&rc); cc_tool_result_cleanup(&result);
    tool.vtable->destroy(tool.self);
    return 0;
}
