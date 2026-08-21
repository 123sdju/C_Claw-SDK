#include "cc/ports/cc_memory_store.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>
#include <string.h>

/* 构造 fake memory entry 深拷贝，模拟真实 store 返回数组的所有权规则。 */
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

/* fake search 的轻量文本匹配逻辑。 */
static int matches(const char *key, const char *value, const char *category, const char *query)
{
    if (!query || !query[0]) return 1;
    return strstr(key, query) || strstr(value, query) || strstr(category, query);
}

/*
 * 只实现旧 search vtable 的 fake store。
 *
 * 测试 cc_memory_store_query 在 adapter 没有 query 扩展时，能基于 search 结果执行
 * category/session 过滤并生成 score。
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
        count++;
    }

    *out_entries = entries;
    *out_count = count;
    return cc_result_ok();
}

/* fake vtable 故意不实现 query 槽位，触发 core fallback 路径。 */
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

/*
 * 验证结构化 memory query port 的 fallback 契约。
 *
 * 第一段同时过滤 query/category/session；第二段只过滤 session，确认返回 top-k 数组和
 * score 字段可用。
 */
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

    return failed ? 1 : 0;
}
