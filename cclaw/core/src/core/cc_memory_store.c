



#include "cc/ports/cc_memory_store.h"
#include <stdlib.h>
#include <string.h>

/**
 * @brief 校验基础入口并同步委托后端写入或覆盖记忆。
 * @param[in,out] store 调用期存活的端口借用，self 的有效性由后端约定。
 * @param[in] key 必需借用键，空串范围由后端决定。
 * @param[in] value 必需借用正文。
 * @param[in] category 可选分类借用。
 * @param[in] session_id 可选来源会话借用。
 * @return 参数或方法缺失返回 INVALID_ARGUMENT，其余原样交接后端拥有结果，调用方 cc_result_free。
 * @note 后端决定失败时的文件状态，同键竞争及端口寿命由 owner 协调。
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

/**
 * @brief 读取拥有型条目，并撤销后端失败时交付的部分字段。
 * @param[in] store 调用期存活端口借用。
 * @param[in] key 必需借用键。
 * @param[out] out_entry 必需新建或已清理槽，入口清零，成功字段由调用方 cc_memory_entry_free。
 * @return 参数错误或后端拥有结果，失败保持空条目，诊断由调用方释放。
 */
cc_result_t cc_memory_store_get(
    cc_memory_store_t *store,
    const char *key,
    cc_memory_entry_t *out_entry
)
{
    if (out_entry) memset(out_entry, 0, sizeof(*out_entry));
    if (!store || !store->vtable || !store->vtable->get || !key || !out_entry)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory store get arguments");
    cc_result_t rc = store->vtable->get(store->self, key, out_entry);
    if (rc.code != CC_OK) cc_memory_entry_free(out_entry);
    return rc;
}

/**
 * @brief 委托文本检索，成功交付独立条目数组。
 * @param[in] store 调用期存活端口借用。
 * @param[in] query 必需借用查询串，空串匹配规则由后端决定。
 * @param[in] limit 条数上限，非正数含义由后端决定。
 * @param[out] out_entries 必需空拥有槽，入口置 NULL，成功用 cc_memory_entry_free_array 释放。
 * @param[out] out_count 必需数量槽，入口置零。
 * @return 参数错误或后端拥有结果，失败释放部分数组并清空输出。
 */
cc_result_t cc_memory_store_search(
    cc_memory_store_t *store,
    const char *query,
    int limit,
    cc_memory_entry_t **out_entries,
    size_t *out_count
)
{
    if (out_entries) *out_entries = NULL;
    if (out_count) *out_count = 0;
    if (!store || !store->vtable || !store->vtable->search || !query || !out_entries || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory store search arguments");
    cc_result_t rc = store->vtable->search(store->self, query, limit, out_entries, out_count);
    if (rc.code == CC_OK && *out_count && !*out_entries) {
        cc_result_free(&rc);
        *out_count = 0;
        return cc_result_error(CC_ERR_INVALID_STATE, "Memory search returned no entry array");
    }
    if (rc.code != CC_OK) {
        cc_memory_entry_free_array(*out_entries, *out_count);
        *out_entries = NULL;
        *out_count = 0;
    }
    return rc;
}

/**
 * @brief 优先使用原生结构化检索，缺少方法或原生忽略排除分类时回退完整检索再过滤。
 * @param[in] store 调用期存活端口借用，并发读写能力由后端提供。
 * @param[in] query 必需完整基础条件借用，size 为零兼容旧调用，尾部排除字段按 size 读取。
 * @param[out] out_results 必需新拥有槽，入口置 NULL，成功用 cc_memory_search_result_free_array 释放。
 * @param[out] out_count 必需数量槽，入口置零，仅成功后发布。
 * @return 参数、方法缺失、分配或后端错误，所有失败撤销部分拥有输出，诊断由调用方释放。
 * @note 回退存在过滤条件时传非正 limit 请求候选，再按正 limit 截取并赋 score=1，旧后端的非正上限仍由实现决定。
 * 原生方法负责全部过滤与排序，排除分类违规会触发第二次检索，两次调用之间由后端决定一致性。
 */
cc_result_t cc_memory_store_query(
    cc_memory_store_t *store,
    const cc_memory_query_t *query,
    cc_memory_search_result_t **out_results,
    size_t *out_count
)
{
    if (out_results) *out_results = NULL;
    if (out_count) *out_count = 0;
    if (!store || !store->vtable || !query || !out_results || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory store query arguments");
    }
    if (query->size && query->size < offsetof(cc_memory_query_t, limit) + sizeof(query->limit))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Memory query base fields are missing");
    const char *excluded = cc_memory_query_excluded_category(query);
    const char *excluded2 = cc_memory_query_second_excluded_category(query);
    if (store->vtable->query) {
        cc_result_t native_rc = store->vtable->query(store->self, query, out_results, out_count);
        if (native_rc.code != CC_OK) {
            cc_memory_search_result_free_array(*out_results, *out_count);
            *out_results = NULL;
            *out_count = 0;
            return native_rc;
        }
        if (*out_count && !*out_results) {
            cc_result_free(&native_rc);
            *out_count = 0;
            return cc_result_error(CC_ERR_INVALID_STATE, "Memory query returned no result array");
        }
        if ((!excluded || !excluded[0]) && (!excluded2 || !excluded2[0])) return native_rc;
        int adapter_ignored_exclusion = 0;
        for (size_t i = 0; i < *out_count; i++) {
            const char *category = (*out_results)[i].entry.category;
            if (cc_memory_query_excludes_category(query, category)) {
                adapter_ignored_exclusion = 1;
                break;
            }
        }
        if (!adapter_ignored_exclusion) return native_rc;
        cc_result_free(&native_rc);
        cc_memory_search_result_free_array(*out_results, *out_count);
        *out_results = NULL;
        *out_count = 0;
    }
    if (!store->vtable->search) {
        return cc_result_error(CC_ERR_UNSUPPORTED, "Memory store query is unsupported");
    }

    cc_memory_entry_t *entries = NULL;
    size_t entry_count = 0;
    int needs_filtering = (query->category && query->category[0]) ||
        (query->session_id && query->session_id[0]) || (excluded && excluded[0]) ||
        (excluded2 && excluded2[0]);
    /* 旧检索没有过滤和分页，存在条件时先取全部匹配项，再执行过滤及条数截取。 */
    cc_result_t rc = store->vtable->search(
        store->self,
        query->query ? query->query : "",
        needs_filtering ? 0 : query->limit,
        &entries,
        &entry_count);
    if (rc.code != CC_OK) {
        cc_memory_entry_free_array(entries, entry_count);
        return rc;
    }
    cc_result_free(&rc);
    if (entry_count && !entries)
        return cc_result_error(CC_ERR_INVALID_STATE, "Memory search returned no entry array");

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
        int excluded_ok = !cc_memory_query_excludes_category(query, entries[i].category);
        if (!category_ok || !session_ok || !excluded_ok ||
            (query->limit > 0 && count >= (size_t)query->limit)) {
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

/**
 * @brief 消费结构化查询数组及每项拥有的条目字段。
 * @param[in] results 交接可由 free 释放的独占数组，NULL 安全。
 * @param[in] count 已初始化条目数量，须不超过实际分配容量。
 * @post 数组及前 count 项字段释放，调用者须清空自身悬空指针，全部借用者已退出。
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

/**
 * @brief 委托分类列表并统一失败输出的清理。
 * @param[in] store 调用期存活端口借用。
 * @param[in] category 可选分类借用，NULL 或空串语义由后端决定。
 * @param[in] limit 条数上限，非正数含义由后端决定。
 * @param[out] out_entries 必需空拥有槽，入口置 NULL，成功数组由调用方释放。
 * @param[out] out_count 必需数量槽，入口置零。
 * @return 参数错误或后端拥有结果，失败释放部分数组并清空输出。
 */
cc_result_t cc_memory_store_list(
    cc_memory_store_t *store,
    const char *category,
    int limit,
    cc_memory_entry_t **out_entries,
    size_t *out_count
)
{
    if (out_entries) *out_entries = NULL;
    if (out_count) *out_count = 0;
    if (!store || !store->vtable || !store->vtable->list || !out_entries || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory store list arguments");
    cc_result_t rc = store->vtable->list(store->self, category, limit, out_entries, out_count);
    if (rc.code == CC_OK && *out_count && !*out_entries) {
        cc_result_free(&rc);
        *out_count = 0;
        return cc_result_error(CC_ERR_INVALID_STATE, "Memory list returned no entry array");
    }
    if (rc.code != CC_OK) {
        cc_memory_entry_free_array(*out_entries, *out_count);
        *out_entries = NULL;
        *out_count = 0;
    }
    return rc;
}

/**
 * @brief 委托删除单个键，保留后端的存在性及失败语义。
 * @param[in,out] store 调用期存活端口借用。
 * @param[in] key 必需借用键，空串由后端解释。
 * @return 参数错误或原样交接的后端拥有结果，失败可能已有外部副作用。
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

/**
 * @brief 将分类删除交给后端，批次原子性由后端实现决定。
 * @param[in,out] store 调用期存活端口借用。
 * @param[in] category 必需借用分类，范围及跨会话行为由后端约定。
 * @return 参数错误或后端拥有结果，调用方释放诊断并核对失败后的部分状态。
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

/**
 * @brief 调用后端销毁并清空描述符，保留描述符容器存储。
 * @param[in,out] store 唯一 owner 提供的对象，NULL 或缺少销毁方法时直接返回。
 * @pre owner 已停止入口、排空调用和全部 self/vtable 借用，浅拷贝不能重复销毁。
 * @post 有销毁方法时 self 释放，描述符字段置 NULL，容器继续归调用方。
 */
void cc_memory_store_destroy(cc_memory_store_t *store)
{
    if (!store || !store->vtable || !store->vtable->destroy) return;
    store->vtable->destroy(store->self);
    store->self = NULL;
    store->vtable = NULL;
}
