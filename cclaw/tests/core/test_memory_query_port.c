#include "cc/ports/cc_memory_store.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>
#include <string.h>

static int fail_result_allocation;
static int backend_mode;
static int last_search_limit;

/** @brief 只对实际核心包装的结果数组注入分配失败。 @param[in] count 元素数。 @param[in] size 元素字节。 @return 清零拥有数组或指定失败的 NULL。 */
static void *query_calloc(size_t count, size_t size)
{
    if (fail_result_allocation) { fail_result_allocation = 0; return NULL; }
    return calloc(count, size);
}

/* 私有包含真实核心包装以隔离数组 OOM，后端替身的分配仍由真实 allocator 执行。 */
#define calloc query_calloc
#define cc_memory_store_set review_memory_set
#define cc_memory_store_get review_memory_get
#define cc_memory_store_search review_memory_search
#define cc_memory_store_query review_memory_query
#define cc_memory_search_result_free_array review_memory_free_results
#define cc_memory_store_list review_memory_list
#define cc_memory_store_delete review_memory_delete
#define cc_memory_store_delete_category review_memory_delete_category
#define cc_memory_store_destroy review_memory_destroy
/** @brief 声明私有包装的结果销毁入口。 @param[in] results 消费拥有数组，NULL 安全。 @param[in] count 可释放条目数。 @post 数组与字段归还。 */
void review_memory_free_results(cc_memory_search_result_t *results, size_t count);
#include "../../core/src/core/cc_memory_store.c"
#undef calloc

/**
 * @brief 深拷贝夹具条目的四个字段，失败清空部分分配。
 * @param[in] key 必需键借用。
 * @param[in] value 必需正文借用。
 * @param[in] category 必需分类借用。
 * @param[in] session_id 必需会话借用。
 * @param[out] out 新条目槽，入口清零，成功字段由调用方释放。
 * @return 成功或 OOM 拥有结果。
 */
static cc_result_t copy_entry(
    const char *key,
    const char *value,
    const char *category,
    const char *session_id,
    cc_memory_entry_t *out
)
{
    memset(out, 0, sizeof(*out));
    out->key = cc_copy_string(key);
    out->value = cc_copy_string(value);
    out->category = cc_copy_string(category);
    out->session_id = cc_copy_string(session_id);
    if (!out->key || !out->value || !out->category || !out->session_id) {
        cc_memory_entry_free(out);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM");
    }
    return cc_result_ok();
}

/** @brief 对三个夹具字段进行区分大小写子串匹配。 @param[in] key 必需键借用。 @param[in] value 必需正文借用。 @param[in] category 必需分类借用。 @param[in] query 可选查询借用。 @return 空查询或任一字段命中为一，否则为零。 */
static int matches(const char *key, const char *value, const char *category, const char *query)
{
    if (!query || !query[0]) return 1;
    return strstr(key, query) || strstr(value, query) || strstr(category, query);
}

/**
 * @brief 返回可检索的三条拥有条目，并可交付安全可释放的部分失败输出。
 * @param[in] self 忽略，可为 NULL。
 * @param[in] query 可选文本借用，空串匹配全部。
 * @param[in] limit 正值截取条数，非正值返回全部匹配，记录实参供过滤顺序断言。
 * @param[out] out_entries 必需空槽，成功或指定失败时交付 calloc 初始化的拥有数组。
 * @param[out] out_count 必需计数槽，与可释放条目数一致。
 * @return 正常成功携带动态诊断，部分失败返回 STORAGE，形状模式成功交付 NULL 配非零数量。
 */
static cc_result_t fake_search(
    void *self,
    const char *query,
    int limit,
    cc_memory_entry_t **out_entries,
    size_t *out_count
)
{
    (void)self;
    *out_entries = NULL;
    *out_count = 0;
    last_search_limit = limit;
    if (backend_mode == 2) { *out_count = 1; return cc_result_ok(); }
    const char *keys[] = { "alpha", "beta", "pref" };
    const char *values[] = { "project alpha note", "project beta note", "alpha preference" };
    const char *categories[] = { "project", "project", "user_pref" };
    const char *sessions[] = { "s1", "s2", "s1" };
    cc_memory_entry_t *entries = calloc(3, sizeof(*entries));
    if (!entries) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM");

    size_t count = 0;
    for (int i = 0; i < 3 && (limit <= 0 || count < (size_t)limit); i++) {
        if (!matches(keys[i], values[i], categories[i], query)) continue;
        cc_result_t rc = copy_entry(keys[i], values[i], categories[i], sessions[i], &entries[count]);
        if (rc.code != CC_OK) {
            cc_memory_entry_free_array(entries, count);
            return rc;
        }
        cc_result_free(&rc);
        count++;
    }

    *out_entries = entries;
    *out_count = count;
    return cc_result_error(backend_mode == 1 ? CC_ERR_STORAGE : CC_OK, "Owned search result");
}

/** @brief 交付拥有字段后报告 get 失败。 @param[in] self 忽略。 @param[in] key 必需键借用。 @param[out] out 必需空条目。 @return STORAGE 或字段复制 OOM，部分字段可安全清理。 */
static cc_result_t fake_get(void *self, const char *key, cc_memory_entry_t *out)
{
    (void)self;
    cc_result_t rc = copy_entry(key, "value", "project", "s1", out);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    return cc_result_error(CC_ERR_STORAGE, "Partial get failure");
}

/**
 * @brief 模拟忽略排除项的旧原生查询，并提供部分失败和错误形状。
 * @param[in] self 忽略。
 * @param[in] query 条件借用，故意忽略排除项以验证回退。
 * @param[out] out_results 必需空槽，成功或指定失败拥有一个 project 条目。
 * @param[out] out_count 必需数量槽。
 * @return 拥有成功、STORAGE 或 OOM，形状模式输出 NULL 与非零数量。
 */
static cc_result_t fake_query(void *self, const cc_memory_query_t *query,
    cc_memory_search_result_t **out_results, size_t *out_count)
{
    (void)self; (void)query;
    *out_results = NULL; *out_count = 0;
    if (backend_mode == 2) { *out_count = 1; return cc_result_ok(); }
    cc_memory_search_result_t *result = calloc(1, sizeof(*result));
    if (!result) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM");
    cc_result_t rc = copy_entry("alpha", "project note", "project", "s1", &result->entry);
    if (rc.code != CC_OK) { free(result); return rc; }
    cc_result_free(&rc);
    result->size = sizeof(*result);
    result->score = 0.5;
    *out_results = result; *out_count = 1;
    return cc_result_error(backend_mode == 1 ? CC_ERR_STORAGE : CC_OK, "Owned native query result");
}

/* 基础场景先省略原生 query，后续用例显式切换该方法。 */
static cc_memory_store_vtable_t fake_vtable = {
    NULL,
    NULL,
    fake_search,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

/**
 * @brief 核对参数失败、部分输出撤销、结果数组 OOM 及错误形状。
 * @param[in] store 借用夹具端口，其静态方法表由本线程临时切换。
 * @return 全部检查通过为零，失败为一，所有返回资源均清理。
 */
static int test_failures(cc_memory_store_t *store)
{
    cc_memory_query_t query = {.size = sizeof(query), .limit = 2};
    cc_memory_search_result_t *results = (void *)1;
    size_t count = 9;
    cc_result_t rc = cc_memory_store_query(NULL, &query, &results, &count);
    int failed = rc.code != CC_ERR_INVALID_ARGUMENT || results || count;
    cc_result_free(&rc);
    query.size = sizeof(size_t);
    rc = cc_memory_store_query(store, &query, &results, &count);
    failed |= rc.code != CC_ERR_INVALID_ARGUMENT || results || count;
    cc_result_free(&rc);
    query.size = sizeof(query);
    fail_result_allocation = 1;
    rc = cc_memory_store_query(store, &query, &results, &count);
    failed |= rc.code != CC_ERR_OUT_OF_MEMORY || results || count;
    cc_result_free(&rc);
    backend_mode = 1;
    rc = cc_memory_store_query(store, &query, &results, &count);
    failed |= rc.code != CC_ERR_STORAGE || results || count;
    cc_result_free(&rc);
    cc_memory_entry_t *entries = NULL;
    rc = cc_memory_store_search(store, "", 2, &entries, &count);
    failed |= rc.code != CC_ERR_STORAGE || entries || count;
    cc_result_free(&rc);
    fake_vtable.list = fake_search;
    rc = cc_memory_store_list(store, NULL, 2, &entries, &count);
    failed |= rc.code != CC_ERR_STORAGE || entries || count;
    cc_result_free(&rc);
    fake_vtable.get = fake_get;
    cc_memory_entry_t entry = {0};
    rc = cc_memory_store_get(store, "key", &entry);
    failed |= rc.code != CC_ERR_STORAGE || entry.key || entry.value || entry.category || entry.session_id;
    cc_result_free(&rc);
    fake_vtable.query = fake_query;
    rc = cc_memory_store_query(store, &query, &results, &count);
    failed |= rc.code != CC_ERR_STORAGE || results || count;
    cc_result_free(&rc);
    backend_mode = 2;
    rc = cc_memory_store_query(store, &query, &results, &count);
    failed |= rc.code != CC_ERR_INVALID_STATE || results || count;
    cc_result_free(&rc);
    rc = cc_memory_store_search(store, "", 2, &entries, &count);
    failed |= rc.code != CC_ERR_INVALID_STATE || entries || count;
    cc_result_free(&rc);
    rc = cc_memory_store_list(store, NULL, 2, &entries, &count);
    failed |= rc.code != CC_ERR_INVALID_STATE || entries || count;
    cc_result_free(&rc);
    backend_mode = 0;
    fake_vtable.query = NULL;
    return failed ? 1 : 0;
}

/** @brief 核对排除发生于条数截取前，以及尾部字段布局探测。 @param[in] store 借用夹具端口。 @return 全部通过为零，成功结果均释放。 */
static int test_exclusions(cc_memory_store_t *store)
{
    fake_vtable.query = fake_query;
    cc_memory_query_t query = {.size = sizeof(query), .limit = 1, .exclude_category = "project"};
    cc_memory_search_result_t *results = NULL;
    size_t count = 0;
    cc_result_t rc = cc_memory_store_query(store, &query, &results, &count);
    int failed = rc.code != CC_OK || count != 1 || last_search_limit != 0;
    if (count == 1) failed |= strcmp(results[0].entry.category, "user_pref") != 0;
    cc_result_free(&rc); cc_memory_search_result_free_array(results, count);
    query.exclude_category2 = "user_pref";
    rc = cc_memory_store_query(store, &query, &results, &count);
    failed |= rc.code != CC_OK || count != 0;
    cc_result_free(&rc); cc_memory_search_result_free_array(results, count);
    query.size = offsetof(cc_memory_query_t, exclude_category);
    rc = cc_memory_store_query(store, &query, &results, &count);
    failed |= rc.code != CC_OK || count != 1;
    if (count == 1) failed |= results[0].score != 0.5;
    cc_result_free(&rc); cc_memory_search_result_free_array(results, count);
    fake_vtable.query = NULL;
    return failed ? 1 : 0;
}

/** @brief 运行旧 size=0 检索及输出、排除和分配回归。 @return 全部检查通过返回零，失败返回一。 */
int main(void)
{
    cc_memory_store_t store = {0};
    cc_memory_search_result_t *results = NULL;
    size_t count = 0;
    int failed = 0;

    store.vtable = &fake_vtable;

    cc_memory_query_t query = {
        .query = "alpha",
        .category = "project",
        .session_id = "s1",
        .limit = 5
    };
    cc_result_t rc = cc_memory_store_query(&store, &query, &results, &count);
    if (rc.code != CC_OK) failed = 1;
    if (count != 1) failed = 1;
    if (count == 1) {
        if (!results[0].entry.key || strcmp(results[0].entry.key, "alpha") != 0) failed = 1;
        if (results[0].score <= 0.0) failed = 1;
    }
    cc_result_free(&rc);
    cc_memory_search_result_free_array(results, count);

    results = NULL;
    count = 0;
    query.category = NULL;
    query.session_id = "s1";
    rc = cc_memory_store_query(&store, &query, &results, &count);
    if (rc.code != CC_OK) failed = 1;
    if (count != 2) failed = 1;
    cc_result_free(&rc);
    cc_memory_search_result_free_array(results, count);

    failed |= test_failures(&store);
    failed |= test_exclusions(&store);
    return failed ? 1 : 0;
}
