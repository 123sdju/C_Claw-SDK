



#include "cc/ports/cc_memory_store.h"
#include <stdlib.h>
#include <string.h>

/*
 * memory 写入 wrapper。
 *
 * 核心层统一校验 store/vtable/必填字段，再把调用转发给 adapter。这样 runtime 和工具
 * 不需要知道具体存储实现，也不用重复写空指针保护。
 */
cc_result_t cc_memory_store_set(
    cc_memory_store_t *store,
    const char *key, const char *value,
    const char *category, const char *session_id
)
{
    if (!store || !store->vtable || !store->vtable->set || !key || !value)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory store set arguments");
    return store->vtable->set(store->self, key, value, category, session_id);
}

/*
 * 按 key 读取 memory。
 *
 * out_entry 的字段由 adapter 填充并转移给调用方；调用方必须用 cc_memory_entry_free()
 * 清理。wrapper 不初始化 out_entry，adapter 应在实现中负责成功/失败状态一致性。
 */
cc_result_t cc_memory_store_get(
    cc_memory_store_t *store,
    const char *key,
    cc_memory_entry_t *out_entry
)
{
    if (!store || !store->vtable || !store->vtable->get || !key || !out_entry)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory store get arguments");
    return store->vtable->get(store->self, key, out_entry);
}

/*
 * 简单文本检索 wrapper。
 *
 * 返回 entry 数组的所有权交给调用方。limit 语义由 adapter 解释，但核心调用方一般
 * 把它当 top-k 上限使用。
 */
cc_result_t cc_memory_store_search(
    cc_memory_store_t *store,
    const char *query,
    int limit,
    cc_memory_entry_t **out_entries,
    size_t *out_count
)
{
    if (!store || !store->vtable || !store->vtable->search || !query || !out_entries || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory store search arguments");
    return store->vtable->search(store->self, query, limit, out_entries, out_count);
}

/*
 * 结构化 memory query wrapper。
 *
 * 如果 adapter 实现了 query，就直接转发；否则用旧 search 接口做 fallback，然后在核心层
 * 应用 category/session 过滤，并给默认 score=1.0。这个设计保证新端口不强迫所有旧
 * adapter 立刻实现向量检索。
 */
cc_result_t cc_memory_store_query(
    cc_memory_store_t *store,
    const cc_memory_query_t *query,
    cc_memory_search_result_t **out_results,
    size_t *out_count
)
{
    if (!store || !store->vtable || !query || !out_results || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory store query arguments");
    }
    *out_results = NULL;
    *out_count = 0;
    if (store->vtable->query) {
        return store->vtable->query(store->self, query, out_results, out_count);
    }
    if (!store->vtable->search) {
        return cc_result_error(CC_ERR_UNSUPPORTED, "Memory store query is unsupported");
    }

    cc_memory_entry_t *entries = NULL;
    size_t entry_count = 0;
    cc_result_t rc = store->vtable->search(
        store->self,
        query->query ? query->query : "",
        query->limit,
        &entries,
        &entry_count);
    if (rc.code != CC_OK) return rc;

    cc_memory_search_result_t *results = calloc(entry_count, sizeof(*results));
    if (!results && entry_count > 0) {
        cc_memory_entry_free_array(entries, entry_count);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate memory query results");
    }

    size_t count = 0;
    for (size_t i = 0; i < entry_count; i++) {
        int category_ok = !query->category || !query->category[0] ||
            (entries[i].category && strcmp(entries[i].category, query->category) == 0);
        int session_ok = !query->session_id || !query->session_id[0] ||
            (entries[i].session_id && strcmp(entries[i].session_id, query->session_id) == 0);
        if (!category_ok || !session_ok) {
            cc_memory_entry_free(&entries[i]);
            continue;
        }
        results[count].size = sizeof(results[count]);
        results[count].entry = entries[i];
        results[count].score = 1.0;
        memset(&entries[i], 0, sizeof(entries[i]));
        count++;
    }
    free(entries);
    *out_results = results;
    *out_count = count;
    return cc_result_ok();
}

/*
 * 释放结构化 query 结果数组。
 *
 * 每个 result 内嵌一个拥有字符串字段的 entry，必须逐项释放后再释放数组本身。
 */
void cc_memory_search_result_free_array(
    cc_memory_search_result_t *results,
    size_t count
)
{
    if (!results) return;
    for (size_t i = 0; i < count; i++) {
        cc_memory_entry_free(&results[i].entry);
    }
    free(results);
}

/*
 * 按分类列举 memory。
 *
 * category 可由 adapter 解释为 NULL/空分类过滤；wrapper 只校验 out 参数存在。
 */
cc_result_t cc_memory_store_list(
    cc_memory_store_t *store,
    const char *category,
    int limit,
    cc_memory_entry_t **out_entries,
    size_t *out_count
)
{
    if (!store || !store->vtable || !store->vtable->list || !out_entries || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory store list arguments");
    return store->vtable->list(store->self, category, limit, out_entries, out_count);
}

/*
 * 删除单条 memory。
 *
 * key 是稳定主键，删除失败的具体原因由 adapter 映射到 cc_result_t。
 */
cc_result_t cc_memory_store_delete(
    cc_memory_store_t *store,
    const char *key
)
{
    if (!store || !store->vtable || !store->vtable->delete_entry || !key)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory store delete arguments");
    return store->vtable->delete_entry(store->self, key);
}

/*
 * 删除分类 memory。
 *
 * 这是批量操作入口，adapter 可以按自己的持久化模型实现事务或逐项删除。
 */
cc_result_t cc_memory_store_delete_category(
    cc_memory_store_t *store,
    const char *category
)
{
    if (!store || !store->vtable || !store->vtable->delete_by_category || !category)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory store delete_category arguments");
    return store->vtable->delete_by_category(store->self, category);
}

/*
 * 销毁 memory store adapter。
 *
 * wrapper 调用 vtable destroy 释放 self，然后清空句柄字段，防止调用方误用旧 self。
 * cc_memory_store_t 容器可能由上层嵌入在更大的对象中，所以这里不 free(store)。
 */
void cc_memory_store_destroy(cc_memory_store_t *store)
{
    if (!store || !store->vtable || !store->vtable->destroy) return;
    store->vtable->destroy(store->self);
    store->self = NULL;
    store->vtable = NULL;
}
