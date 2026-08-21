



#include "cc/ports/cc_memory_store.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_thread.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAP 64

/*
 * 内存版 memory store 私有状态。
 *
 * entries 保存长期记忆条目，mutex 保护 set/get/search/list/delete 的并发访问。这个实现
 * 不持久化，适合测试、demo profile 或 MCU/RTOS 原型；生产环境可以通过同一端口替换为
 * SQLite、JSON 文件或向量数据库 adapter。
 */
typedef struct {
    cc_memory_entry_t *entries;
    size_t count;
    size_t cap;
    cc_mutex_t mutex;
} cc_inmem_memory_store_t;

/*
 * 写入或更新一条记忆。
 *
 * key 已存在时只更新 value/category/updated_at，不改变 session_id；key 不存在时追加新
 * entry 并深拷贝所有字符串。调用方传入的字符串只在调用期间借用。
 */
static cc_result_t inmem_set(void *self, const char *key, const char *value,
                              const char *category, const char *session_id)
{
    cc_inmem_memory_store_t *s = (cc_inmem_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].key, key) == 0) {
            free(s->entries[i].value);
            s->entries[i].value = cc_copy_string(value);
            free(s->entries[i].category);
            s->entries[i].category = category ? cc_copy_string(category) : NULL;
            s->entries[i].updated_at = time(NULL);
            cc_mutex_unlock(s->mutex);
            return cc_result_ok();
        }
    }

    if (s->count >= s->cap) {
        s->cap *= 2;
        s->entries = realloc(s->entries, s->cap * sizeof(cc_memory_entry_t));
        if (!s->entries) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }
    }

    cc_memory_entry_t *e = &s->entries[s->count++];
    e->key = cc_copy_string(key);
    e->value = cc_copy_string(value);
    e->category = category ? cc_copy_string(category) : NULL;
    e->session_id = session_id ? cc_copy_string(session_id) : NULL;
    e->created_at = time(NULL);
    e->updated_at = e->created_at;

    cc_mutex_unlock(s->mutex);
    return cc_result_ok();
}

/*
 * 按 key 读取一条记忆。
 *
 * out_entry 由调用方提供，成功后其中字符串为深拷贝，调用方需要 cc_memory_entry_free。
 * 未找到返回 CC_ERR_NOT_FOUND，而不是空 entry，便于 memory tool 区分业务缺失。
 */
static cc_result_t inmem_get(void *self, const char *key, cc_memory_entry_t *out_entry)
{
    cc_inmem_memory_store_t *s = (cc_inmem_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].key, key) == 0) {
            cc_memory_entry_t *src = &s->entries[i];
            out_entry->key = cc_copy_string(src->key);
            out_entry->value = cc_copy_string(src->value);
            out_entry->category = src->category ? cc_copy_string(src->category) : NULL;
            out_entry->session_id = src->session_id ? cc_copy_string(src->session_id) : NULL;
            out_entry->created_at = src->created_at;
            out_entry->updated_at = src->updated_at;
            cc_mutex_unlock(s->mutex);
            return cc_result_ok();
        }
    }

    cc_mutex_unlock(s->mutex);
    return cc_result_errf(CC_ERR_NOT_FOUND, "Memory key not found: %s", key);
}

/*
 * 简单全文匹配。
 *
 * 这是轻量 fallback，不做分词或向量检索；嵌入式场景可以先用这种 O(n) 扫描满足小数据量，
 * 后续由 query port 接入更强的 embedding/vector adapter。
 */
static int matches_query(const cc_memory_entry_t *e, const char *query)
{
    if (!query || !query[0]) return 1;
    if (e->key && strstr(e->key, query)) return 1;
    if (e->value && strstr(e->value, query)) return 1;
    if (e->category && strstr(e->category, query)) return 1;
    return 0;
}

/*
 * 判断 entry 是否满足结构化 query 的 category/session/query 过滤。
 *
 * 过滤条件为空表示不限制；最后复用 matches_query 做文本匹配。
 */
static int matches_filters(const cc_memory_entry_t *e, const cc_memory_query_t *query)
{
    if (!query) return 1;
    if (query->category && query->category[0] &&
        (!e->category || strcmp(e->category, query->category) != 0)) {
        return 0;
    }
    if (query->session_id && query->session_id[0] &&
        (!e->session_id || strcmp(e->session_id, query->session_id) != 0)) {
        return 0;
    }
    return matches_query(e, query->query);
}

/*
 * 为 query 结果计算一个可解释的粗略分数。
 *
 * 完全匹配 key/value 分数最高，子串匹配次之。这个分数不是语义相关度，只是为统一
 * cc_memory_search_result_t 契约提供轻量排序/展示信息。
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

/*
 * 深拷贝 memory entry。
 *
 * query/search/list 返回的 entry 不能暴露内部数组指针；失败时释放已复制字段，保持
 * dst 处于可安全 cleanup 的状态。
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

/*
 * 旧版 search API：按 query 文本扫描并返回 entry 数组。
 *
 * 返回数组和每个 entry 字符串由调用方拥有。该接口不返回 score；新的结构化检索建议
 * 使用 inmem_query 对应的 vtable 方法。
 */
static cc_result_t inmem_search(void *self, const char *query, int limit,
                                 cc_memory_entry_t **out_entries, size_t *out_count)
{
    cc_inmem_memory_store_t *s = (cc_inmem_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    size_t cap = 16;
    cc_memory_entry_t *results = malloc(cap * sizeof(cc_memory_entry_t));
    if (!results) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }

    size_t count = 0;
    for (size_t i = 0; i < s->count && (limit <= 0 || count < (size_t)limit); i++) {
        if (matches_query(&s->entries[i], query)) {
            if (count >= cap) { cap *= 2; results = realloc(results, cap * sizeof(cc_memory_entry_t)); if (!results) break; }
            cc_memory_entry_t *src = &s->entries[i];
            results[count].key = cc_copy_string(src->key);
            results[count].value = cc_copy_string(src->value);
            results[count].category = src->category ? cc_copy_string(src->category) : NULL;
            results[count].session_id = src->session_id ? cc_copy_string(src->session_id) : NULL;
            results[count].created_at = src->created_at;
            results[count].updated_at = src->updated_at;
            count++;
        }
    }

    cc_mutex_unlock(s->mutex);
    *out_entries = results;
    *out_count = count;
    return cc_result_ok();
}

/*
 * 结构化 query API：支持 top-k、session/category 过滤和 score。
 *
 * 这是核心 memory 检索端口的主要实现形态。当前内存 adapter 用简单文本分数填充 score，
 * 但返回结构和所有权与未来向量 adapter 保持一致。
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
    /* Match newest records first, consistent with persistent adapters. */
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

/*
 * 按 category 列举记忆。
 *
 * category 为空表示列举所有；limit <= 0 表示不限。返回 entry 数组由调用方释放。
 */
static cc_result_t inmem_list(void *self, const char *category, int limit,
                               cc_memory_entry_t **out_entries, size_t *out_count)
{
    cc_inmem_memory_store_t *s = (cc_inmem_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    size_t cap = 16;
    cc_memory_entry_t *results = malloc(cap * sizeof(cc_memory_entry_t));
    if (!results) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }

    size_t count = 0;
    for (size_t i = 0; i < s->count && (limit <= 0 || count < (size_t)limit); i++) {
        if (!category || !category[0] ||
            (s->entries[i].category && strcmp(s->entries[i].category, category) == 0)) {
            if (count >= cap) { cap *= 2; results = realloc(results, cap * sizeof(cc_memory_entry_t)); if (!results) break; }
            cc_memory_entry_t *src = &s->entries[i];
            results[count].key = cc_copy_string(src->key);
            results[count].value = cc_copy_string(src->value);
            results[count].category = src->category ? cc_copy_string(src->category) : NULL;
            results[count].session_id = src->session_id ? cc_copy_string(src->session_id) : NULL;
            results[count].created_at = src->created_at;
            results[count].updated_at = src->updated_at;
            count++;
        }
    }

    cc_mutex_unlock(s->mutex);
    *out_entries = results;
    *out_count = count;
    return cc_result_ok();
}

/*
 * 删除指定 key 的记忆。
 *
 * 命中后释放 entry 并用 memmove 压缩数组；未命中返回 CC_ERR_NOT_FOUND，方便上层工具
 * 生成“未找到”类可恢复提示。
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

/*
 * 删除某个 category 下的所有记忆。
 *
 * 使用 write/read 双指针压缩数组；被删除 entry 先释放，保留 entry 通过结构体赋值前移。
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

/*
 * 销毁内存 memory store。
 *
 * 调用方必须保证没有并发操作仍在进行；函数释放全部 entry、数组、mutex 和私有对象。
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

/* 内存 memory store vtable，把本文件实现绑定到 cc_memory_store_t 端口。 */
static cc_memory_store_vtable_t inmem_vtable = {
    inmem_set, inmem_get, inmem_search, inmem_list,
    inmem_delete_entry, inmem_delete_by_category, inmem_destroy, inmem_query
};

/*
 * 创建内存 memory store。
 *
 * 成功后 out_store 拥有 self/vtable，销毁时通过 cc_memory_store_destroy 或 vtable->destroy。
 * 该实现不落盘，重启后数据丢失，但线程安全、依赖少，适合作为最小 SDK profile。
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
    cc_mutex_create(&s->mutex);

    out_store->self = s;
    out_store->vtable = &inmem_vtable;
    return cc_result_ok();
}
