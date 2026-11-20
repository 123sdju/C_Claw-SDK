



#include "cc/ports/cc_memory_store.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_thread.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAP 64

/** @brief RAM 记忆存储私有状态，互斥锁保护有效记录及其数组容量，实例寿命由外层协调。 */
typedef struct {
    cc_memory_entry_t *entries; /**< 拥有的记录槽，前 count 项及其字符串有效。 */
    size_t count; /**< 当前有效记录条数。 */
    size_t cap; /**< 已分配记录槽数，按倍增扩容。 */
    cc_mutex_t mutex; /**< 串行保护读写和结果复制，销毁前须排空全部调用。 */
} cc_inmem_memory_store_t;

/**
 * @brief 深拷贝记录全部字符串及时间字段，失败清理部分副本。
 * @param[in] src 有效的借用记录，各字符串可为 NULL。
 * @param[out] dst 无已有资源的输出结构，进入后清零，成功副本由调用者 cc_memory_entry_free。
 * @return 成功为 CC_OK，任一字段分配失败为 CC_ERR_OUT_OF_MEMORY 且输出清零。
 */
static cc_result_t copy_entry(const cc_memory_entry_t *src, cc_memory_entry_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->key = src->key ? cc_copy_string(src->key) : NULL;
    dst->value = src->value ? cc_copy_string(src->value) : NULL;
    dst->category = src->category ? cc_copy_string(src->category) : NULL;
    dst->session_id = src->session_id ? cc_copy_string(src->session_id) : NULL;
    dst->created_at = src->created_at;
    dst->updated_at = src->updated_at;
    if ((src->key && !dst->key) || (src->value && !dst->value) ||
        (src->category && !dst->category) ||
        (src->session_id && !dst->session_id)) {
        cc_memory_entry_free(dst);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy memory entry");
    }
    return cc_result_ok();
}

/**
 * @brief 在锁内复制新内容，分配成功后更新已有记录或追加新记录。
 * @param[in,out] self 有效的借用私有实例，公共包装保证输入指针有效。
 * @param[in] key 借用主键，允许空字符串，按字节匹配。
 * @param[in] value 借用正文，允许空字符串，复制后保存。
 * @param[in] category 可为 NULL 的借用分类，更新时替换旧分类。
 * @param[in] session_id 可为 NULL 的借用来源会话，仅新增时记录，更新时保留原会话及创建时间。
 * @return 成功为 CC_OK，字符串或数组分配失败为 CC_ERR_OUT_OF_MEMORY，容量溢出为 CC_ERR_LIMIT_EXCEEDED。
 * @note 失败保留全部已有记录，新增项在字符串及容量全部准备好后才计入 count。
 */
static cc_result_t inmem_set(void *self, const char *key, const char *value,
                              const char *category, const char *session_id)
{
    cc_inmem_memory_store_t *s = (cc_inmem_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].key, key) == 0) {
            char *new_value = cc_copy_string(value);
            char *new_category = category ? cc_copy_string(category) : NULL;
            if (!new_value || (category && !new_category)) {
                free(new_value);
                free(new_category);
                cc_mutex_unlock(s->mutex);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to update memory entry");
            }
            free(s->entries[i].value);
            s->entries[i].value = new_value;
            free(s->entries[i].category);
            s->entries[i].category = new_category;
            s->entries[i].updated_at = time(NULL);
            cc_mutex_unlock(s->mutex);
            return cc_result_ok();
        }
    }

    cc_memory_entry_t source = {.key = (char *)key, .value = (char *)value,
        .category = (char *)category, .session_id = (char *)session_id, .created_at = time(NULL)};
    source.updated_at = source.created_at;
    cc_memory_entry_t entry = {0};
    cc_result_t rc = copy_entry(&source, &entry);
    if (rc.code != CC_OK) { cc_mutex_unlock(s->mutex); return rc; }
    if (s->count >= s->cap) {
        if (s->cap > SIZE_MAX / 2U || s->cap * 2U > SIZE_MAX / sizeof(*s->entries)) {
            cc_memory_entry_free(&entry);
            cc_mutex_unlock(s->mutex);
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory store capacity overflow");
        }
        size_t new_cap = s->cap * 2U;
        cc_memory_entry_t *next = realloc(s->entries, new_cap * sizeof(*next));
        if (!next) {
            cc_memory_entry_free(&entry);
            cc_mutex_unlock(s->mutex);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow memory store");
        }
        s->entries = next;
        s->cap = new_cap;
    }
    s->entries[s->count++] = entry;

    cc_mutex_unlock(s->mutex);
    return cc_result_ok();
}

/**
 * @brief 在锁内按精确主键查找并深拷贝记录。
 * @param[in] self 有效的借用私有实例，公共包装校验主键及输出指针。
 * @param[in] key 借用主键，按字节比较，允许空字符串。
 * @param[out] out_entry 无已有资源的输出结构，参数校验在公共入口完成，本函数进入后清零，成功副本由调用者释放。
 * @return 成功为 CC_OK，主键缺失为 CC_ERR_NOT_FOUND，复制失败为 CC_ERR_OUT_OF_MEMORY，失败输出为空结构。
 */
static cc_result_t inmem_get(void *self, const char *key, cc_memory_entry_t *out_entry)
{
    cc_inmem_memory_store_t *s = (cc_inmem_memory_store_t *)self;
    memset(out_entry, 0, sizeof(*out_entry));
    cc_mutex_lock(s->mutex);

    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].key, key) == 0) {
            cc_result_t rc = copy_entry(&s->entries[i], out_entry);
            cc_mutex_unlock(s->mutex);
            return rc;
        }
    }

    cc_mutex_unlock(s->mutex);
    return cc_result_errf(CC_ERR_NOT_FOUND, "Memory key not found: %s", key);
}

/**
 * @brief 用区分大小写的子串匹配检查主键、正文和分类。
 * @param[in] e 有效的借用记录，各字符串可为 NULL。
 * @param[in] query 借用检索词，NULL 或空字符串匹配所有记录。
 * @return 任一字段包含检索词或检索词为空时为一，其他情况为零。
 */
static int matches_query(const cc_memory_entry_t *e, const char *query)
{
    if (!query || !query[0]) return 1;
    if (e->key && strstr(e->key, query)) return 1;
    if (e->value && strstr(e->value, query)) return 1;
    if (e->category && strstr(e->category, query)) return 1;
    return 0;
}

/**
 * @brief 依次检查分类、排除分类、会话和文本条件。
 * @param[in] e 有效的借用候选记录。
 * @param[in] query 借用查询，NULL 匹配全部，空分类或会话不限定该字段。
 * @return 所有指定条件均满足为一，任一条件不符为零。
 */
static int matches_filters(const cc_memory_entry_t *e, const cc_memory_query_t *query)
{
    if (!query) return 1;
    if (query->category && query->category[0] &&
        (!e->category || strcmp(e->category, query->category) != 0)) {
        return 0;
    }
    if (cc_memory_query_excludes_category(query, e->category)) return 0;
    if (query->session_id && query->session_id[0] &&
        (!e->session_id || strcmp(e->session_id, query->session_id) != 0)) {
        return 0;
    }
    return matches_query(e, query->query);
}

/**
 * @brief 按首个匹配字段计算结构化结果的固定分值。
 * @param[in] e 有效的借用记录。
 * @param[in] query 借用检索词，可为 NULL 或空字符串。
 * @return 空词或精确主键为 1.0，精确正文为 0.95，主键子串为 0.8，正文子串为 0.7，分类子串为 0.5，其余为零。
 */
static double simple_score(const cc_memory_entry_t *e, const char *query)
{
    if (!query || !query[0]) return 1.0;
    if (e->key && strcmp(e->key, query) == 0) return 1.0;
    if (e->value && strcmp(e->value, query) == 0) return 0.95;
    if (e->key && strstr(e->key, query)) return 0.8;
    if (e->value && strstr(e->value, query)) return 0.7;
    if (e->category && strstr(e->category, query)) return 0.5;
    return 0.0;
}

/**
 * @brief 在锁内按数组正序匹配检索词，全部复制成功后交付结果。
 * @param[in] self 有效的借用私有实例，公共包装校验检索词和输出指针。
 * @param[in] query 借用检索词，空字符串匹配全部记录。
 * @param[in] limit 结果条数上限，小于等于零表示不限。
 * @param[out] out_entries 进入后置 NULL，成功取得拥有数组，调用者 cc_memory_entry_free_array。
 * @param[out] out_count 进入后置零，仅成功时填写条数，零条仍可能有已分配数组。
 * @return 成功为 CC_OK，分配或复制失败为 CC_ERR_OUT_OF_MEMORY，数组容量溢出为 CC_ERR_LIMIT_EXCEEDED，失败清理全部部分结果。
 */
static cc_result_t inmem_search(void *self, const char *query, int limit,
                                 cc_memory_entry_t **out_entries, size_t *out_count)
{
    *out_entries = NULL;
    *out_count = 0;
    cc_inmem_memory_store_t *s = (cc_inmem_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    size_t cap = 16;
    cc_memory_entry_t *results = malloc(cap * sizeof(cc_memory_entry_t));
    if (!results) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }

    size_t count = 0;
    for (size_t i = 0; i < s->count && (limit <= 0 || count < (size_t)limit); i++) {
        if (matches_query(&s->entries[i], query)) {
            if (count >= cap) {
                if (cap > SIZE_MAX / 2U || cap * 2U > SIZE_MAX / sizeof(*results)) {
                    cc_memory_entry_free_array(results, count);
                    cc_mutex_unlock(s->mutex);
                    return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory search result overflow");
                }
                size_t new_cap = cap * 2U;
                cc_memory_entry_t *next = realloc(results, new_cap * sizeof(*next));
                if (!next) {
                    cc_memory_entry_free_array(results, count);
                    cc_mutex_unlock(s->mutex);
                    return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow memory search results");
                }
                results = next;
                cap = new_cap;
            }
            cc_result_t rc = copy_entry(&s->entries[i], &results[count]);
            if (rc.code != CC_OK) {
                cc_memory_entry_free_array(results, count);
                cc_mutex_unlock(s->mutex);
                return rc;
            }
            count++;
        }
    }

    cc_mutex_unlock(s->mutex);
    *out_entries = results;
    *out_count = count;
    return cc_result_ok();
}

/**
 * @brief 在锁内按数组倒序应用过滤及 limit，复制记录并附固定文本分值。
 * @param[in] self 有效的借用私有实例，NULL 返回参数错误。
 * @param[in] query 必需的借用查询，limit 小于等于零时不限。
 * @param[out] out_results 必需输出槽，参数合法后置 NULL，成功拥有数组由调用者 cc_memory_search_result_free_array。
 * @param[out] out_count 参数合法后置零，仅成功时填写条数，零条仍可能返回已分配数组。
 * @return 成功为 CC_OK，NULL 参数为 CC_ERR_INVALID_ARGUMENT，分配或复制失败为 CC_ERR_OUT_OF_MEMORY，容量溢出为 CC_ERR_LIMIT_EXCEEDED。
 * @note 分值随结果返回，选取顺序取决于数组位置，原位更新保持原位置。
 */
static cc_result_t inmem_query(
    void *self,
    const cc_memory_query_t *query,
    cc_memory_search_result_t **out_results,
    size_t *out_count
)
{
    if (!self || !query || !out_results || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid in-memory query");
    }
    *out_results = NULL;
    *out_count = 0;
    cc_inmem_memory_store_t *s = (cc_inmem_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    size_t cap = query->limit > 0 && query->limit < 16 ?
        (size_t)query->limit : 16U;
    if (cap == 0) cap = 1;
    cc_memory_search_result_t *results = calloc(cap, sizeof(*results));
    if (!results) {
        cc_mutex_unlock(s->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM");
    }

    size_t count = 0;
    int limit = query->limit;
    /* 从数组尾部开始扫描，新追加的记录优先，普通更新保持已有数组位置。 */
    for (size_t cursor = s->count; cursor > 0 &&
         (limit <= 0 || count < (size_t)limit); cursor--) {
        size_t i = cursor - 1U;
        if (!matches_filters(&s->entries[i], query)) continue;
        if (count >= cap) {
            if (cap > SIZE_MAX / 2U || cap * 2U > SIZE_MAX / sizeof(*results)) {
                cc_memory_search_result_free_array(results, count);
                cc_mutex_unlock(s->mutex);
                return cc_result_error(CC_ERR_LIMIT_EXCEEDED,
                                       "In-memory query result overflow");
            }
            size_t new_cap = cap * 2U;
            cc_memory_search_result_t *new_results =
                realloc(results, new_cap * sizeof(*results));
            if (!new_results) {
                cc_memory_search_result_free_array(results, count);
                cc_mutex_unlock(s->mutex);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM");
            }
            memset(new_results + cap, 0, (new_cap - cap) * sizeof(*new_results));
            results = new_results;
            cap = new_cap;
        }
        cc_result_t rc = copy_entry(&s->entries[i], &results[count].entry);
        if (rc.code != CC_OK) {
            cc_memory_search_result_free_array(results, count);
            cc_mutex_unlock(s->mutex);
            return rc;
        }
        results[count].size = sizeof(results[count]);
        results[count].score = simple_score(&s->entries[i], query ? query->query : NULL);
        count++;
    }

    cc_mutex_unlock(s->mutex);
    *out_results = results;
    *out_count = count;
    return cc_result_ok();
}

/**
 * @brief 在锁内按数组正序复制分类匹配记录，失败清理本次全部副本。
 * @param[in] self 有效的借用私有实例，公共包装校验输出指针。
 * @param[in] category 借用分类，NULL 或空字符串匹配全部。
 * @param[in] limit 结果条数上限，小于等于零表示不限。
 * @param[out] out_entries 进入后置 NULL，成功取得拥有数组，由调用者 cc_memory_entry_free_array。
 * @param[out] out_count 进入后置零，仅成功时填写条数，零条仍可能有已分配数组。
 * @return 成功为 CC_OK，分配或复制失败为 CC_ERR_OUT_OF_MEMORY，容量溢出为 CC_ERR_LIMIT_EXCEEDED。
 */
static cc_result_t inmem_list(void *self, const char *category, int limit,
                               cc_memory_entry_t **out_entries, size_t *out_count)
{
    *out_entries = NULL;
    *out_count = 0;
    cc_inmem_memory_store_t *s = (cc_inmem_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    size_t cap = 16;
    cc_memory_entry_t *results = malloc(cap * sizeof(cc_memory_entry_t));
    if (!results) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }

    size_t count = 0;
    for (size_t i = 0; i < s->count && (limit <= 0 || count < (size_t)limit); i++) {
        if (!category || !category[0] ||
            (s->entries[i].category && strcmp(s->entries[i].category, category) == 0)) {
            if (count >= cap) {
                if (cap > SIZE_MAX / 2U || cap * 2U > SIZE_MAX / sizeof(*results)) {
                    cc_memory_entry_free_array(results, count);
                    cc_mutex_unlock(s->mutex);
                    return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory list result overflow");
                }
                size_t new_cap = cap * 2U;
                cc_memory_entry_t *next = realloc(results, new_cap * sizeof(*next));
                if (!next) {
                    cc_memory_entry_free_array(results, count);
                    cc_mutex_unlock(s->mutex);
                    return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow memory list results");
                }
                results = next;
                cap = new_cap;
            }
            cc_result_t rc = copy_entry(&s->entries[i], &results[count]);
            if (rc.code != CC_OK) {
                cc_memory_entry_free_array(results, count);
                cc_mutex_unlock(s->mutex);
                return rc;
            }
            count++;
        }
    }

    cc_mutex_unlock(s->mutex);
    *out_entries = results;
    *out_count = count;
    return cc_result_ok();
}

/**
 * @brief 在锁内释放匹配记录并压缩后续有效槽。
 * @param[in,out] self 有效的借用私有实例。
 * @param[in] key 借用主键，公共包装保证非 NULL，允许空字符串。
 * @return 删除成功为 CC_OK，主键缺失为 CC_ERR_NOT_FOUND。
 */
static cc_result_t inmem_delete_entry(void *self, const char *key)
{
    cc_inmem_memory_store_t *s = (cc_inmem_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].key, key) == 0) {
            cc_memory_entry_free(&s->entries[i]);
            if (i < s->count - 1)
                memmove(&s->entries[i], &s->entries[i + 1], (s->count - i - 1) * sizeof(cc_memory_entry_t));
            s->count--;
            cc_mutex_unlock(s->mutex);
            return cc_result_ok();
        }
    }

    cc_mutex_unlock(s->mutex);
    return cc_result_error(CC_ERR_NOT_FOUND, "Memory key not found");
}

/**
 * @brief 在锁内释放指定分类的记录，再将保留项前移并更新有效计数。
 * @param[in,out] self 有效的借用私有实例。
 * @param[in] category 借用分类，公共包装保证非 NULL，按字节匹配，可为空字符串。
 * @return 始终为 CC_OK，无匹配记录时有效集合保持原值，操作无需新分配。
 */
static cc_result_t inmem_delete_by_category(void *self, const char *category)
{
    cc_inmem_memory_store_t *s = (cc_inmem_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    size_t write = 0;
    for (size_t i = 0; i < s->count; i++) {
        if (s->entries[i].category && strcmp(s->entries[i].category, category) == 0) {
            cc_memory_entry_free(&s->entries[i]);
        } else {
            if (write != i) s->entries[write] = s->entries[i];
            write++;
        }
    }
    s->count = write;

    cc_mutex_unlock(s->mutex);
    return cc_result_ok();
}

/**
 * @brief 释放有效记录及其字符串、记录槽、互斥锁和私有对象。
 * @pre 调用者已停止新入口并排空所有持锁、等待和直接借用的使用者。
 * @param[in,out] self 待销毁的私有实例，可为 NULL。
 * @post 非 NULL 实例寿命结束，所有内部拥有资源均已释放。
 */
static void inmem_destroy(void *self)
{
    cc_inmem_memory_store_t *s = (cc_inmem_memory_store_t *)self;
    if (!s) return;
    for (size_t i = 0; i < s->count; i++) cc_memory_entry_free(&s->entries[i]);
    free(s->entries);
    cc_mutex_destroy(s->mutex);
    free(s);
}

/* 端口表将本实例的同步读写及结构化查询绑定到公共包装入口。 */
static cc_memory_store_vtable_t inmem_vtable = {
    inmem_set, inmem_get, inmem_search, inmem_list,
    inmem_delete_entry, inmem_delete_by_category, inmem_destroy, inmem_query
};

/**
 * @brief 创建拥有 64 个初始记录槽和互斥锁的 RAM 记忆存储。
 * @param[out] out_store 无已有资源的输出句柄，非 NULL 时先清零，成功后由调用者 cc_memory_store_destroy。
 * @return 成功为 CC_OK，NULL 输出为 CC_ERR_INVALID_ARGUMENT，分配失败为 CC_ERR_OUT_OF_MEMORY，锁创建错误原样返回。
 * @note 失败释放本次资源并保持输出为空，普通调用由实例锁串行化，销毁同步由外层负责。
 */
cc_result_t cc_memory_store_create_inmem(cc_memory_store_t *out_store)
{
    if (!out_store) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null in-memory store output");
    }
    memset(out_store, 0, sizeof(*out_store));
    cc_inmem_memory_store_t *s = calloc(1, sizeof(cc_inmem_memory_store_t));
    if (!s) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate inmem memory store");
    s->cap = INITIAL_CAP;
    s->entries = malloc(s->cap * sizeof(cc_memory_entry_t));
    if (!s->entries) { free(s); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }
    cc_result_t rc = cc_mutex_create(&s->mutex);
    if (rc.code != CC_OK) { free(s->entries); free(s); return rc; }

    out_store->self = s;
    out_store->vtable = &inmem_vtable;
    return cc_result_ok();
}
