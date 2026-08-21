



#include "cc/ports/cc_memory_store.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_thread.h"
#include <sqlite3.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * SQLite memory store 私有状态。
 *
 * db 保存长期记忆表，mutex 串行化 SQLite 操作。该实现比 JSON 文件后端更适合长期运行的
 * 嵌入式 Linux/桌面环境，但仍通过 cc_memory_store_t 端口与 core 解耦。
 */
typedef struct {
    sqlite3 *db;
    cc_mutex_t mutex;
} cc_sqlite_memory_store_t;

/*
 * 初始化 memory 表和索引。
 *
 * key 是主键，category/session_id 建索引用于过滤；时间字段用整数 time_t，便于跨语言
 * 查询和排序。
 */
static int init_db(sqlite3 *db)
{
    const char *sql =
        "CREATE TABLE IF NOT EXISTS memory ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL,"
        "  category TEXT,"
        "  session_id TEXT,"
        "  created_at INTEGER NOT NULL,"
        "  updated_at INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_memory_category ON memory(category);"
        "CREATE INDEX IF NOT EXISTS idx_memory_session ON memory(session_id);";
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return rc;
    }
    return SQLITE_OK;
}

/*
 * 写入或替换一条记忆。
 *
 * INSERT OR REPLACE 会保留已有 created_at，并更新 updated_at。函数持锁覆盖 prepare/bind/
 * step/finalize，保证同一连接不会被多个线程交错使用。
 */
static cc_result_t sqlite_set(void *self, const char *key, const char *value,
                               const char *category, const char *session_id)
{
    cc_sqlite_memory_store_t *s = (cc_sqlite_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    time_t now = time(NULL);
    const char *sql = "INSERT OR REPLACE INTO memory (key, value, category, session_id, created_at, updated_at) "
                      "VALUES (?, ?, ?, ?, COALESCE((SELECT created_at FROM memory WHERE key=?), ?), ?);";
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL);
    if (!stmt) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_STORAGE, "SQL prepare failed"); }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, category, category ? -1 : 0, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, session_id, session_id ? -1 : 0, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)now);
    sqlite3_bind_int64(stmt, 7, (sqlite3_int64)now);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    cc_mutex_unlock(s->mutex);
    if (rc != SQLITE_DONE)
        return cc_result_errf(CC_ERR_STORAGE, "SQLite set failed: %s", sqlite3_errmsg(s->db));
    return cc_result_ok();
}

/*
 * 按 key 读取记忆。
 *
 * out_entry 的字符串由本函数深拷贝，调用方需要 cc_memory_entry_free。未命中返回
 * CC_ERR_NOT_FOUND。
 */
static cc_result_t sqlite_get(void *self, const char *key, cc_memory_entry_t *out_entry)
{
    cc_sqlite_memory_store_t *s = (cc_sqlite_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s->db, "SELECT key, value, category, session_id, created_at, updated_at FROM memory WHERE key=?",
                       -1, &stmt, NULL);
    if (!stmt) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_STORAGE, "SQL prepare failed"); }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    cc_result_t result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out_entry->key = cc_copy_string((const char *)sqlite3_column_text(stmt, 0));
        out_entry->value = cc_copy_string((const char *)sqlite3_column_text(stmt, 1));
        const char *cat = (const char *)sqlite3_column_text(stmt, 2);
        out_entry->category = cat ? cc_copy_string(cat) : NULL;
        const char *sid = (const char *)sqlite3_column_text(stmt, 3);
        out_entry->session_id = sid ? cc_copy_string(sid) : NULL;
        out_entry->created_at = (time_t)sqlite3_column_int64(stmt, 4);
        out_entry->updated_at = (time_t)sqlite3_column_int64(stmt, 5);
        result = cc_result_ok();
    } else {
        result = cc_result_errf(CC_ERR_NOT_FOUND, "Memory key not found: %s", key);
    }

    sqlite3_finalize(stmt);
    cc_mutex_unlock(s->mutex);
    return result;
}

/*
 * 绑定 LIKE 查询参数。
 *
 * query 被包裹为 `%query%`，同时匹配 key/value/category；limit 已由调用方归一化。
 */
static void bind_search_query(sqlite3_stmt *stmt, const char *query, int limit)
{
    char like[1024];
    snprintf(like, sizeof(like), "%%%s%%", query);
    sqlite3_bind_text(stmt, 1, like, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, like, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, like, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, limit);
}

/*
 * 旧版 search API：用 SQL LIKE 做轻量全文检索。
 *
 * 返回 entry 数组由调用方释放。该实现不提供 score；结构化向量检索应通过扩展 query
 * vtable 或其它 adapter 实现。
 */
static cc_result_t sqlite_search(void *self, const char *query, int limit,
                                  cc_memory_entry_t **out_entries, size_t *out_count)
{
    cc_sqlite_memory_store_t *s = (cc_sqlite_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    const char *sql = "SELECT key, value, category, session_id, created_at, updated_at FROM memory "
                      "WHERE key LIKE ? OR value LIKE ? OR category LIKE ? LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL);
    if (!stmt) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_STORAGE, "SQL prepare failed"); }

    bind_search_query(stmt, query, limit > 0 ? limit : 100);

    size_t cap = 16, count = 0;
    cc_memory_entry_t *results = malloc(cap * sizeof(cc_memory_entry_t));
    if (!results) { sqlite3_finalize(stmt); cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) { cap *= 2; results = realloc(results, cap * sizeof(cc_memory_entry_t)); if (!results) break; }
        results[count].key = cc_copy_string((const char *)sqlite3_column_text(stmt, 0));
        results[count].value = cc_copy_string((const char *)sqlite3_column_text(stmt, 1));
        const char *cat = (const char *)sqlite3_column_text(stmt, 2);
        results[count].category = cat ? cc_copy_string(cat) : NULL;
        const char *sid = (const char *)sqlite3_column_text(stmt, 3);
        results[count].session_id = sid ? cc_copy_string(sid) : NULL;
        results[count].created_at = (time_t)sqlite3_column_int64(stmt, 4);
        results[count].updated_at = (time_t)sqlite3_column_int64(stmt, 5);
        count++;
    }

    sqlite3_finalize(stmt);
    cc_mutex_unlock(s->mutex);
    *out_entries = results;
    *out_count = count;
    return cc_result_ok();
}

/*
 * Structured SQLite query applies category/session filters in SQL before LIMIT. The score is a
 * deterministic lexical relevance placeholder; a vector/FTS adapter can later replace it without
 * changing the port contract.
 */
static cc_result_t sqlite_query(
    void *self,
    const cc_memory_query_t *query,
    cc_memory_search_result_t **out_results,
    size_t *out_count)
{
    if (!query || !out_results || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid SQLite memory query");
    }
    *out_results = NULL;
    *out_count = 0;
    cc_sqlite_memory_store_t *s = (cc_sqlite_memory_store_t *)self;
    cc_mutex_lock(s->mutex);
    const char *sql =
        "SELECT key,value,category,session_id,created_at,updated_at,"
        "CASE WHEN ?1='' THEN 1.0 WHEN key LIKE ?2 ESCAPE '\\' THEN 1.0 "
        "WHEN value LIKE ?2 ESCAPE '\\' THEN 0.8 "
        "WHEN category LIKE ?2 ESCAPE '\\' THEN 0.6 ELSE 0.0 END "
        "FROM memory "
        "WHERE (?1='' OR key LIKE ?2 ESCAPE '\\' OR value LIKE ?2 ESCAPE '\\' "
        "OR category LIKE ?2 ESCAPE '\\') "
        "AND (?3='' OR category=?3) AND (?4='' OR session_id=?4) "
        "ORDER BY updated_at DESC LIMIT ?5";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) != SQLITE_OK || !stmt) {
        cc_mutex_unlock(s->mutex);
        return cc_result_error(CC_ERR_STORAGE, "SQLite memory query prepare failed");
    }
    const char *text = query->query ? query->query : "";
    const char *category = query->category ? query->category : "";
    const char *session_id = query->session_id ? query->session_id : "";
    size_t text_len = strlen(text);
    if (text_len > 1000U || text_len > (SIZE_MAX - 3U) / 2U) {
        sqlite3_finalize(stmt);
        cc_mutex_unlock(s->mutex);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "SQLite memory query is too long");
    }
    char *like = malloc(text_len * 2U + 3U);
    if (!like) {
        sqlite3_finalize(stmt);
        cc_mutex_unlock(s->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate SQLite query pattern");
    }
    size_t like_length = 0;
    like[like_length++] = '%';
    for (size_t i = 0; i < text_len; i++) {
        if (text[i] == '%' || text[i] == '_' || text[i] == '\\') {
            like[like_length++] = '\\';
        }
        like[like_length++] = text[i];
    }
    like[like_length++] = '%';
    like[like_length] = '\0';
    sqlite3_bind_text(stmt, 1, text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, like, -1, SQLITE_TRANSIENT);
    free(like);
    sqlite3_bind_text(stmt, 3, category, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, session_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, query->limit > 0 ? query->limit : 100);

    size_t capacity = query->limit > 0 && query->limit < 16 ?
        (size_t)query->limit : 16U;
    if (capacity == 0) capacity = 1;
    cc_memory_search_result_t *results = calloc(capacity, sizeof(*results));
    if (!results) {
        sqlite3_finalize(stmt);
        cc_mutex_unlock(s->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate SQLite memory results");
    }
    size_t count = 0;
    int step_rc = SQLITE_DONE;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count == capacity) {
            if (capacity > SIZE_MAX / 2U ||
                capacity * 2U > SIZE_MAX / sizeof(*results)) {
                cc_memory_search_result_free_array(results, count);
                sqlite3_finalize(stmt);
                cc_mutex_unlock(s->mutex);
                return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "SQLite memory result overflow");
            }
            size_t next_capacity = capacity * 2U;
            cc_memory_search_result_t *resized =
                realloc(results, next_capacity * sizeof(*results));
            if (!resized) {
                cc_memory_search_result_free_array(results, count);
                sqlite3_finalize(stmt);
                cc_mutex_unlock(s->mutex);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow SQLite memory results");
            }
            memset(resized + capacity, 0,
                   (next_capacity - capacity) * sizeof(*resized));
            results = resized;
            capacity = next_capacity;
        }
        cc_memory_search_result_t *result = &results[count];
        result->size = sizeof(*result);
        result->entry.key = cc_copy_string((const char *)sqlite3_column_text(stmt, 0));
        result->entry.value = cc_copy_string((const char *)sqlite3_column_text(stmt, 1));
        const char *cat = (const char *)sqlite3_column_text(stmt, 2);
        const char *sid = (const char *)sqlite3_column_text(stmt, 3);
        result->entry.category = cat ? cc_copy_string(cat) : NULL;
        result->entry.session_id = sid ? cc_copy_string(sid) : NULL;
        result->entry.created_at = (time_t)sqlite3_column_int64(stmt, 4);
        result->entry.updated_at = (time_t)sqlite3_column_int64(stmt, 5);
        result->score = sqlite3_column_double(stmt, 6);
        if (!result->entry.key || !result->entry.value ||
            (cat && !result->entry.category) || (sid && !result->entry.session_id)) {
            cc_memory_search_result_free_array(results, count + 1);
            sqlite3_finalize(stmt);
            cc_mutex_unlock(s->mutex);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy SQLite memory result");
        }
        count++;
    }
    sqlite3_finalize(stmt);
    cc_mutex_unlock(s->mutex);
    if (step_rc != SQLITE_DONE) {
        cc_memory_search_result_free_array(results, count);
        return cc_result_error(CC_ERR_STORAGE, "SQLite memory query failed");
    }
    *out_results = results;
    *out_count = count;
    return cc_result_ok();
}

/*
 * 按 category 列举记忆。
 *
 * category 为空时列出全部；limit 在读取循环中限制返回数量。所有返回字符串都是深拷贝。
 */
static cc_result_t sqlite_list(void *self, const char *category, int limit,
                                cc_memory_entry_t **out_entries, size_t *out_count)
{
    cc_sqlite_memory_store_t *s = (cc_sqlite_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    const char *sql;
    if (category && category[0])
        sql = "SELECT key, value, category, session_id, created_at, updated_at FROM memory WHERE category=?";
    else
        sql = "SELECT key, value, category, session_id, created_at, updated_at FROM memory";

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL);
    if (!stmt) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_STORAGE, "SQL prepare failed"); }

    if (category && category[0])
        sqlite3_bind_text(stmt, 1, category, -1, SQLITE_TRANSIENT);

    size_t cap = 16, count = 0;
    cc_memory_entry_t *results = malloc(cap * sizeof(cc_memory_entry_t));
    if (!results) { sqlite3_finalize(stmt); cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }

    while (sqlite3_step(stmt) == SQLITE_ROW && (limit <= 0 || count < (size_t)limit)) {
        if (count >= cap) { cap *= 2; results = realloc(results, cap * sizeof(cc_memory_entry_t)); if (!results) break; }
        results[count].key = cc_copy_string((const char *)sqlite3_column_text(stmt, 0));
        results[count].value = cc_copy_string((const char *)sqlite3_column_text(stmt, 1));
        const char *cat = (const char *)sqlite3_column_text(stmt, 2);
        results[count].category = cat ? cc_copy_string(cat) : NULL;
        const char *sid = (const char *)sqlite3_column_text(stmt, 3);
        results[count].session_id = sid ? cc_copy_string(sid) : NULL;
        results[count].created_at = (time_t)sqlite3_column_int64(stmt, 4);
        results[count].updated_at = (time_t)sqlite3_column_int64(stmt, 5);
        count++;
    }

    sqlite3_finalize(stmt);
    cc_mutex_unlock(s->mutex);
    *out_entries = results;
    *out_count = count;
    return cc_result_ok();
}

/*
 * 删除指定 key 的记忆。
 *
 * 当前实现把“未命中”视为幂等成功，适合工具层 forget 操作重复调用；如果产品需要精确
 * 区分，可检查 sqlite3_changes。
 */
static cc_result_t sqlite_delete_entry(void *self, const char *key)
{
    cc_sqlite_memory_store_t *s = (cc_sqlite_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s->db, "DELETE FROM memory WHERE key=?", -1, &stmt, NULL);
    if (!stmt) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_STORAGE, "SQL prepare failed"); }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    cc_mutex_unlock(s->mutex);
    return cc_result_ok();
}

/*
 * 删除某个 category 下的所有记忆。
 *
 * 作为批量清理接口，返回成功表示 SQL 执行完成，不表示一定删除了记录。
 */
static cc_result_t sqlite_delete_by_category(void *self, const char *category)
{
    cc_sqlite_memory_store_t *s = (cc_sqlite_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s->db, "DELETE FROM memory WHERE category=?", -1, &stmt, NULL);
    if (!stmt) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_STORAGE, "SQL prepare failed"); }

    sqlite3_bind_text(stmt, 1, category, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    cc_mutex_unlock(s->mutex);
    return cc_result_ok();
}

/*
 * 销毁 SQLite memory store。
 *
 * 调用方必须保证没有其它线程继续使用；函数关闭数据库、销毁 mutex 并释放私有对象。
 */
static void sqlite_destroy(void *self)
{
    cc_sqlite_memory_store_t *s = (cc_sqlite_memory_store_t *)self;
    if (!s) return;
    cc_mutex_lock(s->mutex);
    if (s->db) sqlite3_close(s->db);
    cc_mutex_unlock(s->mutex);
    cc_mutex_destroy(s->mutex);
    free(s);
}

/* SQLite memory store vtable with native structured query. */
static cc_memory_store_vtable_t sqlite_vtable = {
    .set = sqlite_set,
    .get = sqlite_get,
    .search = sqlite_search,
    .list = sqlite_list,
    .delete_entry = sqlite_delete_entry,
    .delete_by_category = sqlite_delete_by_category,
    .destroy = sqlite_destroy,
    .query = sqlite_query,
};

/*
 * 创建 SQLite memory store。
 *
 * 成功后 out_store 获得 self/vtable；db_path 由调用方配置，父目录应提前创建。函数打开
 * 数据库、创建 mutex、初始化表结构。
 */
cc_result_t cc_memory_store_create_sqlite(cc_memory_store_t *out_store, const char *db_path)
{
    if (!out_store || !db_path)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid sqlite memory store arguments");
    memset(out_store, 0, sizeof(*out_store));

    cc_sqlite_memory_store_t *s = calloc(1, sizeof(cc_sqlite_memory_store_t));
    if (!s) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate sqlite memory store");

    int rc = sqlite3_open(db_path, &s->db);
    if (rc != SQLITE_OK) {
        const char *msg = sqlite3_errmsg(s->db);
        sqlite3_close(s->db);
        free(s);
        return cc_result_errf(CC_ERR_STORAGE, "SQLite open failed: %s", msg);
    }

    cc_mutex_create(&s->mutex);
    init_db(s->db);

    out_store->self = s;
    out_store->vtable = &sqlite_vtable;
    return cc_result_ok();
}
