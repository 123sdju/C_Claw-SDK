



#include "cc/ports/cc_memory_store.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_thread.h"
#include "cc/ports/cc_filesystem.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * JSON 文件 memory store 私有状态。
 *
 * 启动时把文件加载到 entries 数组，后续 set/delete 会把整个数组重新写回文件。mutex
 * 保护内存数组和文件写入临界区；该实现简单可读，适合学习和小规模持久化。
 */
typedef struct {
    char *file_path;
    cc_memory_entry_t *entries;
    size_t count;
    size_t cap;
    cc_mutex_t mutex;
    cc_filesystem_t fs;
} cc_json_memory_store_t;

static char *path_with_suffix(const char *path, const char *suffix)
{
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    if (path_len > SIZE_MAX - suffix_len - 1U) return NULL;
    char *result = malloc(path_len + suffix_len + 1U);
    if (!result) return NULL;
    memcpy(result, path, path_len);
    memcpy(result + path_len, suffix, suffix_len + 1U);
    return result;
}

static char *parent_directory(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *separator = slash;
    if (!separator || (backslash && backslash > separator)) separator = backslash;
    if (!separator) return cc_copy_string(".");

    size_t length = (size_t)(separator - path);
    if (length == 0U) length = 1U;
    if (length == 2U && path[1] == ':') length = 3U;
    char *parent = malloc(length + 1U);
    if (!parent) return NULL;
    memcpy(parent, path, length);
    parent[length] = '\0';
    return parent;
}

static cc_result_t sync_parent_directory(cc_json_memory_store_t *s)
{
    if (!(s->fs.capabilities & CC_FS_CAP_DIR_SYNC) || !s->fs.vtable->sync_dir) {
        return cc_result_ok();
    }
    char *parent = parent_directory(s->file_path);
    if (!parent) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate parent path");
    cc_result_t rc = s->fs.vtable->sync_dir(s->fs.self, parent);
    free(parent);
    return rc;
}

static cc_result_t reserve_entries(cc_json_memory_store_t *s, size_t needed)
{
    if (needed <= s->cap) return cc_result_ok();
    size_t new_cap = s->cap ? s->cap : 64U;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2U) {
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory store capacity overflow");
        }
        new_cap *= 2U;
    }
    if (new_cap > SIZE_MAX / sizeof(*s->entries)) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory store allocation overflow");
    }
    cc_memory_entry_t *resized = realloc(s->entries, new_cap * sizeof(*s->entries));
    if (!resized) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow memory store");
    s->entries = resized;
    s->cap = new_cap;
    return cc_result_ok();
}

static cc_result_t copy_entry(const cc_memory_entry_t *source, cc_memory_entry_t *destination)
{
    memset(destination, 0, sizeof(*destination));
    destination->key = cc_copy_string(source->key);
    destination->value = cc_copy_string(source->value);
    destination->category = source->category ? cc_copy_string(source->category) : NULL;
    destination->session_id = source->session_id ? cc_copy_string(source->session_id) : NULL;
    destination->created_at = source->created_at;
    destination->updated_at = source->updated_at;
    if (!destination->key || !destination->value ||
        (source->category && !destination->category) ||
        (source->session_id && !destination->session_id)) {
        cc_memory_entry_free(destination);
        memset(destination, 0, sizeof(*destination));
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy memory entry");
    }
    return cc_result_ok();
}

static cc_result_t create_entry(
    const char *key,
    const char *value,
    const char *category,
    const char *session_id,
    cc_memory_entry_t *entry)
{
    cc_memory_entry_t source = {
        .key = (char *)key,
        .value = (char *)value,
        .category = (char *)category,
        .session_id = (char *)session_id,
        .created_at = time(NULL),
    };
    source.updated_at = source.created_at;
    return copy_entry(&source, entry);
}

/*
 * 将当前内存 entries 保存到 JSON 文件。
 *
 * 采用临时文件、文件同步、原子替换以及平台支持时的父目录同步。调用方只有在该函数
 * 成功后才提交内存状态；失败时必须恢复原数组。
 */
static cc_result_t save_to_file(cc_json_memory_store_t *s)
{
    cc_json_value_t *arr = cc_json_create_array();
    if (!arr) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create memory JSON array");
    for (size_t i = 0; i < s->count; i++) {
        cc_json_value_t *obj = cc_json_create_object();
        if (!obj) {
            cc_json_destroy(arr);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create memory JSON object");
        }
        cc_json_object_set(obj, "key", cc_json_create_string(s->entries[i].key));
        cc_json_object_set(obj, "value", cc_json_create_string(s->entries[i].value));
        if (s->entries[i].category)
            cc_json_object_set(obj, "category", cc_json_create_string(s->entries[i].category));
        if (s->entries[i].session_id)
            cc_json_object_set(obj, "session_id", cc_json_create_string(s->entries[i].session_id));
        cc_json_object_set(obj, "created_at", cc_json_create_number((double)s->entries[i].created_at));
        cc_json_object_set(obj, "updated_at", cc_json_create_number((double)s->entries[i].updated_at));
        cc_json_array_append(arr, obj);
    }
    char *json_str = cc_json_stringify(arr);
    cc_json_destroy(arr);
    if (!json_str) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to serialize memory store");
    char *tmp = path_with_suffix(s->file_path, ".tmp");
    if (!tmp) {
        free(json_str);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate memory store temp path");
    }
    cc_result_t rc = s->fs.vtable->write_text(s->fs.self, tmp, json_str);
    if (rc.code == CC_OK && s->fs.vtable->sync_file) {
        rc = s->fs.vtable->sync_file(s->fs.self, tmp);
    }
    if (rc.code == CC_OK) {
        rc = s->fs.vtable->atomic_replace(s->fs.self, tmp, s->file_path);
    }
    if (rc.code == CC_OK) {
        rc = sync_parent_directory(s);
    }
    if (rc.code != CC_OK && s->fs.vtable->remove) {
        cc_result_t cleanup = s->fs.vtable->remove(s->fs.self, tmp);
        cc_result_free(&cleanup);
    }
    free(tmp);
    free(json_str);
    return rc;
}

/*
 * 从 JSON 文件加载 entries。
 *
 * 文件不存在或解析失败时保持空 store；每条记录会深拷贝 key/value/category/session_id。
 * 这个函数只在创建阶段调用，因此没有单独加锁。
 */
static cc_json_value_t *parse_array(char *content)
{
    if (!content) return NULL;
    cc_json_value_t *array = NULL;
    cc_result_t parse_rc = cc_json_parse(content, &array);
    if (parse_rc.code != CC_OK) {
        cc_result_free(&parse_rc);
        if (array) cc_json_destroy(array);
        return NULL;
    }
    cc_result_free(&parse_rc);
    if (!cc_json_is_array(array)) {
        cc_json_destroy(array);
        return NULL;
    }
    return array;
}

static void load_from_file(cc_json_memory_store_t *s)
{
    char *content = NULL;
    cc_result_t read_rc = s->fs.vtable->read_text(
        s->fs.self, s->file_path, &content);
    cc_json_value_t *arr = NULL;
    if (read_rc.code == CC_OK) arr = parse_array(content);
    cc_result_free(&read_rc);
    free(content);

    char *tmp = path_with_suffix(s->file_path, ".tmp");
    if (!arr && tmp) {
        content = NULL;
        read_rc = s->fs.vtable->read_text(s->fs.self, tmp, &content);
        if (read_rc.code == CC_OK) arr = parse_array(content);
        cc_result_free(&read_rc);
        free(content);
        if (arr) {
            cc_result_t replace_rc = s->fs.vtable->atomic_replace(
                s->fs.self, tmp, s->file_path);
            if (replace_rc.code == CC_OK) {
                cc_result_t sync_rc = sync_parent_directory(s);
                cc_result_free(&sync_rc);
            } else {
                cc_json_destroy(arr);
                arr = NULL;
            }
            cc_result_free(&replace_rc);
        }
    } else if (arr && tmp && s->fs.vtable->remove) {
        cc_result_t cleanup_rc = s->fs.vtable->remove(s->fs.self, tmp);
        cc_result_free(&cleanup_rc);
    }
    free(tmp);
    if (!arr) return;

    size_t len = (size_t)cc_json_array_size(arr);
    cc_result_t reserve_rc = reserve_entries(s, len);
    if (reserve_rc.code != CC_OK) {
        cc_result_free(&reserve_rc);
        cc_json_destroy(arr);
        return;
    }
    cc_result_free(&reserve_rc);

    for (size_t i = 0; i < len; i++) {
        cc_json_value_t *obj = cc_json_array_get(arr, i);
        if (!obj) continue;
        const char *key = cc_json_string_value(cc_json_object_get(obj, "key"));
        const char *value = cc_json_string_value(cc_json_object_get(obj, "value"));
        if (!key || !value) continue;
        const char *cat = cc_json_string_value(cc_json_object_get(obj, "category"));
        const char *sid = cc_json_string_value(cc_json_object_get(obj, "session_id"));
        cc_memory_entry_t source = {
            .key = (char *)key,
            .value = (char *)value,
            .category = (char *)cat,
            .session_id = (char *)sid,
            .created_at = (time_t)cc_json_int_value(cc_json_object_get(obj, "created_at")),
            .updated_at = (time_t)cc_json_int_value(cc_json_object_get(obj, "updated_at")),
        };
        cc_memory_entry_t *e = &s->entries[s->count];
        cc_result_t copy_rc = copy_entry(&source, e);
        if (copy_rc.code != CC_OK) {
            cc_result_free(&copy_rc);
            continue;
        }
        if (e->created_at == 0) e->created_at = time(NULL);
        if (e->updated_at == 0) e->updated_at = e->created_at;
        s->count++;
    }
    cc_json_destroy(arr);
}

/*
 * 写入或更新一条长期记忆。
 *
 * key 已存在则更新 value/category/updated_at 并保存文件；key 不存在则追加新 entry。
 * session_id 在首次创建时记录，用于后续按会话过滤或调试。
 */
static cc_result_t json_set(void *self, const char *key, const char *value,
                             const char *category, const char *session_id)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
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
            char *old_value = s->entries[i].value;
            char *old_category = s->entries[i].category;
            time_t old_updated_at = s->entries[i].updated_at;
            s->entries[i].value = new_value;
            s->entries[i].category = new_category;
            s->entries[i].updated_at = time(NULL);
            cc_result_t save_rc = save_to_file(s);
            if (save_rc.code == CC_OK) {
                free(old_value);
                free(old_category);
            } else {
                s->entries[i].value = old_value;
                s->entries[i].category = old_category;
                s->entries[i].updated_at = old_updated_at;
                free(new_value);
                free(new_category);
            }
            cc_mutex_unlock(s->mutex);
            return save_rc;
        }
    }

    cc_result_t reserve_rc = reserve_entries(s, s->count + 1U);
    if (reserve_rc.code != CC_OK) {
        cc_mutex_unlock(s->mutex);
        return reserve_rc;
    }
    cc_result_free(&reserve_rc);

    cc_memory_entry_t *e = &s->entries[s->count];
    cc_result_t create_rc = create_entry(key, value, category, session_id, e);
    if (create_rc.code != CC_OK) {
        cc_mutex_unlock(s->mutex);
        return create_rc;
    }
    cc_result_free(&create_rc);
    s->count++;

    cc_result_t save_rc = save_to_file(s);
    if (save_rc.code != CC_OK) {
        s->count--;
        cc_memory_entry_free(e);
        memset(e, 0, sizeof(*e));
    }
    cc_mutex_unlock(s->mutex);
    return save_rc;
}

/*
 * 按 key 读取一条长期记忆。
 *
 * 成功后 out_entry 中所有字符串为深拷贝，调用方用 cc_memory_entry_free 释放。
 * 未找到返回 CC_ERR_NOT_FOUND。
 */
static cc_result_t json_get(void *self, const char *key, cc_memory_entry_t *out_entry)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].key, key) == 0) {
            cc_result_t copy_rc = copy_entry(&s->entries[i], out_entry);
            cc_mutex_unlock(s->mutex);
            return copy_rc;
        }
    }

    cc_mutex_unlock(s->mutex);
    return cc_result_errf(CC_ERR_NOT_FOUND, "Memory key not found: %s", key);
}

/*
 * 简单子串匹配 helper。
 *
 * JSON 文件后端不做向量检索，只在 key/value/category 上做 strstr；needle 为空时不会匹配。
 */
static int match_query(const char *haystack, const char *needle)
{
    return haystack && needle && strstr(haystack, needle);
}

/*
 * 旧版 search API：按 query 子串扫描 JSON memory entries。
 *
 * 返回数组由调用方释放。该实现保留为轻量 fallback；结构化 query/score 更完整的实现可以
 * 由其它 adapter 提供。
 */
static cc_result_t json_search(void *self, const char *query, int limit,
                                cc_memory_entry_t **out_entries, size_t *out_count)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    size_t cap = 16;
    cc_memory_entry_t *results = malloc(cap * sizeof(cc_memory_entry_t));
    if (!results) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }

    size_t count = 0;
    for (size_t i = 0; i < s->count && (limit <= 0 || count < (size_t)limit); i++) {
        cc_memory_entry_t *e = &s->entries[i];
        if (match_query(e->key, query) || match_query(e->value, query) || match_query(e->category, query)) {
            if (count >= cap) {
                if (cap > SIZE_MAX / 2U || cap * 2U > SIZE_MAX / sizeof(*results)) {
                    cc_memory_entry_free_array(results, count);
                    cc_mutex_unlock(s->mutex);
                    return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory search result overflow");
                }
                size_t next_cap = cap * 2U;
                cc_memory_entry_t *resized = realloc(results, next_cap * sizeof(*results));
                if (!resized) {
                    cc_memory_entry_free_array(results, count);
                    cc_mutex_unlock(s->mutex);
                    return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow search results");
                }
                results = resized;
                cap = next_cap;
            }
            cc_result_t copy_rc = copy_entry(e, &results[count]);
            if (copy_rc.code != CC_OK) {
                cc_memory_entry_free_array(results, count);
                cc_mutex_unlock(s->mutex);
                return copy_rc;
            }
            cc_result_free(&copy_rc);
            count++;
        }
    }

    cc_mutex_unlock(s->mutex);
    *out_entries = results;
    *out_count = count;
    return cc_result_ok();
}

static int json_query_matches(
    const cc_memory_entry_t *entry,
    const cc_memory_query_t *query)
{
    if (!entry || !query) return 0;
    if (query->category && query->category[0] &&
        (!entry->category || strcmp(entry->category, query->category) != 0)) {
        return 0;
    }
    if (query->session_id && query->session_id[0] &&
        (!entry->session_id || strcmp(entry->session_id, query->session_id) != 0)) {
        return 0;
    }
    if (!query->query || !query->query[0]) return 1;
    return match_query(entry->key, query->query) ||
           match_query(entry->value, query->query) ||
           match_query(entry->category, query->query);
}

static double json_query_score(
    const cc_memory_entry_t *entry,
    const char *query)
{
    if (!query || !query[0]) return 1.0;
    if (match_query(entry->key, query)) return 1.0;
    if (match_query(entry->value, query)) return 0.8;
    if (match_query(entry->category, query)) return 0.6;
    return 0.0;
}

/* Native structured query applies filters before top-k, avoiding the core legacy fallback's underfill. */
static cc_result_t json_query(
    void *self,
    const cc_memory_query_t *query,
    cc_memory_search_result_t **out_results,
    size_t *out_count)
{
    if (!query || !out_results || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid JSON memory query");
    }
    *out_results = NULL;
    *out_count = 0;
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    cc_mutex_lock(s->mutex);
    size_t capacity = query->limit > 0 && query->limit < 16 ?
        (size_t)query->limit : 16U;
    if (capacity == 0) capacity = 1;
    cc_memory_search_result_t *results = calloc(capacity, sizeof(*results));
    if (!results) {
        cc_mutex_unlock(s->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate memory query results");
    }
    size_t count = 0;
    /* Match newest records first, consistent with the SQLite updated_at ordering. */
    for (size_t cursor = s->count; cursor > 0 &&
         (query->limit <= 0 || count < (size_t)query->limit); cursor--) {
        size_t i = cursor - 1U;
        if (!json_query_matches(&s->entries[i], query)) continue;
        if (count == capacity) {
            if (capacity > SIZE_MAX / 2U ||
                capacity * 2U > SIZE_MAX / sizeof(*results)) {
                cc_memory_search_result_free_array(results, count);
                cc_mutex_unlock(s->mutex);
                return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory query result overflow");
            }
            size_t next_capacity = capacity * 2U;
            cc_memory_search_result_t *resized =
                realloc(results, next_capacity * sizeof(*results));
            if (!resized) {
                cc_memory_search_result_free_array(results, count);
                cc_mutex_unlock(s->mutex);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow memory query results");
            }
            memset(resized + capacity, 0,
                   (next_capacity - capacity) * sizeof(*resized));
            results = resized;
            capacity = next_capacity;
        }
        cc_result_t rc = copy_entry(&s->entries[i], &results[count].entry);
        if (rc.code != CC_OK) {
            cc_memory_search_result_free_array(results, count);
            cc_mutex_unlock(s->mutex);
            return rc;
        }
        results[count].size = sizeof(results[count]);
        results[count].score = json_query_score(&s->entries[i], query->query);
        count++;
    }
    cc_mutex_unlock(s->mutex);
    *out_results = results;
    *out_count = count;
    return cc_result_ok();
}

/*
 * 按 category 列举 entries。
 *
 * category 为空表示全部，limit <= 0 表示不限。返回数据是深拷贝，调用方不持有内部数组。
 */
static cc_result_t json_list(void *self, const char *category, int limit,
                              cc_memory_entry_t **out_entries, size_t *out_count)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    size_t cap = 16;
    cc_memory_entry_t *results = malloc(cap * sizeof(cc_memory_entry_t));
    if (!results) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }

    size_t count = 0;
    for (size_t i = 0; i < s->count && (limit <= 0 || count < (size_t)limit); i++) {
        cc_memory_entry_t *e = &s->entries[i];
        if (!category || !category[0] || (e->category && strcmp(e->category, category) == 0)) {
            if (count >= cap) {
                if (cap > SIZE_MAX / 2U || cap * 2U > SIZE_MAX / sizeof(*results)) {
                    cc_memory_entry_free_array(results, count);
                    cc_mutex_unlock(s->mutex);
                    return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory list result overflow");
                }
                size_t next_cap = cap * 2U;
                cc_memory_entry_t *resized = realloc(results, next_cap * sizeof(*results));
                if (!resized) {
                    cc_memory_entry_free_array(results, count);
                    cc_mutex_unlock(s->mutex);
                    return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow list results");
                }
                results = resized;
                cap = next_cap;
            }
            cc_result_t copy_rc = copy_entry(e, &results[count]);
            if (copy_rc.code != CC_OK) {
                cc_memory_entry_free_array(results, count);
                cc_mutex_unlock(s->mutex);
                return copy_rc;
            }
            cc_result_free(&copy_rc);
            count++;
        }
    }

    cc_mutex_unlock(s->mutex);
    *out_entries = results;
    *out_count = count;
    return cc_result_ok();
}

/*
 * 删除指定 key 的 entry。
 *
 * 删除后压缩数组并立即保存文件；未找到返回 CC_ERR_NOT_FOUND，便于 memory tool 给用户
 * 明确反馈。
 */
static cc_result_t json_delete_entry(void *self, const char *key)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].key, key) == 0) {
            cc_memory_entry_t removed = s->entries[i];
            if (i < s->count - 1)
                memmove(&s->entries[i], &s->entries[i + 1], (s->count - i - 1) * sizeof(cc_memory_entry_t));
            s->count--;
            cc_result_t save_rc = save_to_file(s);
            if (save_rc.code == CC_OK) {
                cc_memory_entry_free(&removed);
            } else {
                if (i < s->count) {
                    memmove(&s->entries[i + 1], &s->entries[i], (s->count - i) * sizeof(cc_memory_entry_t));
                }
                s->entries[i] = removed;
                s->count++;
            }
            cc_mutex_unlock(s->mutex);
            return save_rc;
        }
    }

    cc_mutex_unlock(s->mutex);
    return cc_result_error(CC_ERR_NOT_FOUND, "Memory key not found");
}

/*
 * 删除某个 category 下的所有 entries。
 *
 * 使用 write/read 下标原地压缩；匹配项先释放，不匹配项前移。最后保存整个文件。
 */
static cc_result_t json_delete_by_category(void *self, const char *category)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    if (s->cap > SIZE_MAX / sizeof(*s->entries)) {
        cc_mutex_unlock(s->mutex);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory store allocation overflow");
    }
    cc_memory_entry_t *survivors = malloc(s->cap * sizeof(*survivors));
    if (!survivors) {
        cc_mutex_unlock(s->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to stage category deletion");
    }

    size_t survivor_count = 0;
    for (size_t i = 0; i < s->count; i++) {
        if (!(s->entries[i].category && strcmp(s->entries[i].category, category) == 0)) {
            survivors[survivor_count++] = s->entries[i];
        }
    }
    cc_memory_entry_t *old_entries = s->entries;
    size_t old_count = s->count;
    s->entries = survivors;
    s->count = survivor_count;
    cc_result_t save_rc = save_to_file(s);
    if (save_rc.code == CC_OK) {
        for (size_t i = 0; i < old_count; i++) {
            if (old_entries[i].category && strcmp(old_entries[i].category, category) == 0) {
                cc_memory_entry_free(&old_entries[i]);
            }
        }
        free(old_entries);
    } else {
        free(survivors);
        s->entries = old_entries;
        s->count = old_count;
    }

    cc_mutex_unlock(s->mutex);
    return save_rc;
}

/*
 * 销毁 JSON memory store。
 *
 * 调用方需保证没有并发操作；函数释放 entries、file_path、mutex 和私有对象。
 */
static void json_destroy(void *self)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    if (!s) return;
    for (size_t i = 0; i < s->count; i++) cc_memory_entry_free(&s->entries[i]);
    free(s->entries);
    free(s->file_path);
    cc_mutex_destroy(s->mutex);
    if (s->fs.vtable && s->fs.vtable->destroy) s->fs.vtable->destroy(s->fs.self);
    free(s);
}

/* JSON file memory store implements structured query natively. */
static cc_memory_store_vtable_t json_vtable = {
    .set = json_set,
    .get = json_get,
    .search = json_search,
    .list = json_list,
    .delete_entry = json_delete_entry,
    .delete_by_category = json_delete_by_category,
    .destroy = json_destroy,
    .query = json_query,
};

/*
 * 创建 JSON 文件 memory store。
 *
 * 成功后 out_store 获得 self/vtable；file_path 由调用方传入并被深拷贝。该函数加载已有
 * 文件内容，后续写操作会覆盖保存整个数组。
 */
cc_result_t cc_memory_store_create_json_file(cc_memory_store_t *out_store, const char *file_path)
{
    if (!out_store || !file_path)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid json file memory store arguments");
    memset(out_store, 0, sizeof(*out_store));

    cc_json_memory_store_t *s = calloc(1, sizeof(cc_json_memory_store_t));
    if (!s) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate json memory store");

    cc_result_t fs_rc = cc_filesystem_get_default(&s->fs);
    if (fs_rc.code != CC_OK) { free(s); return fs_rc; }
    uint64_t required_caps = CC_FS_CAP_ATOMIC_REPLACE | CC_FS_CAP_FILE_SYNC;
    if (!s->fs.vtable ||
        s->fs.version < CC_FILESYSTEM_VTABLE_VERSION ||
        (s->fs.capabilities & required_caps) != required_caps ||
        !s->fs.vtable->read_text || !s->fs.vtable->write_text ||
        !s->fs.vtable->atomic_replace || !s->fs.vtable->sync_file) {
        if (s->fs.vtable && s->fs.vtable->destroy) s->fs.vtable->destroy(s->fs.self);
        free(s);
        return cc_result_error(CC_ERR_UNSUPPORTED,
            "JSON memory store requires atomic replace and file sync");
    }
    s->file_path = cc_copy_string(file_path);
    if (!s->file_path) {
        if (s->fs.vtable && s->fs.vtable->destroy) s->fs.vtable->destroy(s->fs.self);
        free(s);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy memory store path");
    }
    s->cap = 64;
    s->entries = calloc(s->cap, sizeof(cc_memory_entry_t));
    if (!s->entries) {
        free(s->file_path);
        if (s->fs.vtable && s->fs.vtable->destroy) s->fs.vtable->destroy(s->fs.self);
        free(s);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM");
    }

    cc_result_t mutex_rc = cc_mutex_create(&s->mutex);
    if (mutex_rc.code != CC_OK) {
        free(s->entries);
        free(s->file_path);
        if (s->fs.vtable && s->fs.vtable->destroy) s->fs.vtable->destroy(s->fs.self);
        free(s);
        return mutex_rc;
    }
    cc_result_free(&mutex_rc);
    load_from_file(s);

    out_store->self = s;
    out_store->vtable = &json_vtable;
    return cc_result_ok();
}
