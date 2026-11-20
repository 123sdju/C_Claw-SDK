



#include "cc/ports/cc_memory_store.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_thread.h"
#include <sqlite3.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/** @brief 拥有 SQLite 连接与一把 mutex，所有已接纳方法串行访问该连接。 */
typedef struct {
    sqlite3 *db;
    cc_mutex_t mutex;
} cc_sqlite_memory_store_t;

/**
 * @brief 在连接仍受独占时复制 SQLite 当前诊断。
 * @param[in] db 存活连接借用，打开失败时可为 NULL。
 * @param[in] code 原生失败码，NOMEM 分类为 OUT_OF_MEMORY。
 * @param[in] operation 调用期借用的操作说明。
 * @return 拥有错误结果，后续可 finalize/close/解锁，调用者最终 result_free。
 */
static cc_result_t sqlite_failure(sqlite3 *db, int code, const char *operation)
{
    return cc_result_errf((code & 0xff) == SQLITE_NOMEM ? CC_ERR_OUT_OF_MEMORY : CC_ERR_STORAGE,
        "%s: %s", operation, sqlite3_errmsg(db));
}

/**
 * @brief 结束语句并保留首个错误，成功路径另检查 finalize 结果。
 * @param[in] db 存活连接，调用者持锁或独占创建期。
 * @param[in,out] stmt 本次独占语句，可为 NULL，调用后失效。
 * @param[in,out] result 按值接管的已有结果，错误诊断保持原样。
 * @return 原结果或 finalize 的拥有错误，后续由调用者消费。
 */
static cc_result_t finish_statement(sqlite3 *db, sqlite3_stmt *stmt, cc_result_t result)
{
    int code = sqlite3_finalize(stmt);
    if (result.code == CC_OK && code != SQLITE_OK) {
        cc_result_free(&result);
        return sqlite_failure(db, code, "SQLite memory finalize failed");
    }
    return result;
}

/**
 * @brief 在创建期建立 memory 表及分类、会话索引。
 * @param[in,out] db 存活独占连接。
 * @return 原生 SQLITE_OK 或失败码，exec 分配的错误文本在本函数归还。
 * @note 多条 DDL 可在失败前已执行，初始化不擦除原数据库。
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
    if (err) sqlite3_free(err);
    return rc;
}

/**
 * @brief 持锁写入一条记忆，保留已有 created_at 并使用 time 更新 updated_at。
 * @param[in,out] self 非 NULL 存活后端。
 * @param[in] key 非 NULL 调用期键借用，空串沿 SQLite 语义。
 * @param[in] value 非 NULL 调用期正文借用。
 * @param[in] category 可空调用期分类借用，NULL 绑定为 SQL NULL。
 * @param[in] session_id 可空调用期会话借用，NULL 绑定为 SQL NULL。
 * @return 成功为 CC_OK，空参数、SQLite OOM 或存储错误返回对应拥有结果。
 * @note 每个 prepare/bind/step 均检查，诊断在 finalize 和解锁前复制，失败可能已有提交副作用。
 */
static cc_result_t sqlite_set(void *self, const char *key, const char *value,
                               const char *category, const char *session_id)
{
    cc_sqlite_memory_store_t *s = (cc_sqlite_memory_store_t *)self;
    if (!s || !key || !value)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid SQLite memory set");
    cc_mutex_lock(s->mutex);

    time_t now = time(NULL);
    const char *sql = "INSERT OR REPLACE INTO memory (key, value, category, session_id, created_at, updated_at) "
                      "VALUES (?, ?, ?, ?, COALESCE((SELECT created_at FROM memory WHERE key=?), ?), ?);";
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL);
    if (code == SQLITE_OK) code = sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK) code = sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK) code = sqlite3_bind_text(stmt, 3, category, category ? -1 : 0, SQLITE_TRANSIENT);
    if (code == SQLITE_OK) code = sqlite3_bind_text(stmt, 4, session_id, session_id ? -1 : 0, SQLITE_TRANSIENT);
    if (code == SQLITE_OK) code = sqlite3_bind_text(stmt, 5, key, -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK) code = sqlite3_bind_int64(stmt, 6, (sqlite3_int64)now);
    if (code == SQLITE_OK) code = sqlite3_bind_int64(stmt, 7, (sqlite3_int64)now);
    if (code == SQLITE_OK) code = sqlite3_step(stmt);
    cc_result_t rc = code == SQLITE_DONE ? cc_result_ok() :
        sqlite_failure(s->db, code, "SQLite memory set failed");
    rc = finish_statement(s->db, stmt, rc);
    cc_mutex_unlock(s->mutex);
    return rc;
}

/**
 * @brief 从当前 SELECT 行复制六列到一个独立拥有条目。
 * @param[in] stmt 已停在 SQLITE_ROW 的语句，外层持连接锁，列顺序为四字符串和两时间。
 * @param[out] entry 必填新或已清理槽，先清零，失败清空本次字段。
 * @return 成功为 CC_OK，字段复制失败为 OUT_OF_MEMORY，缺失必需列或转换失败为存储错误。
 * @note 可选分类和会话 SQL NULL 保留 NULL，时间按平台 time_t 转换。
 */
static cc_result_t copy_entry(sqlite3_stmt *stmt, cc_memory_entry_t *entry)
{
    cc_memory_entry_init(entry);
    const char *key = (const char *)sqlite3_column_text(stmt, 0);
    const char *value = (const char *)sqlite3_column_text(stmt, 1);
    const char *category = (const char *)sqlite3_column_text(stmt, 2);
    const char *session = (const char *)sqlite3_column_text(stmt, 3);
    if (!key || !value || (!category && sqlite3_column_type(stmt, 2) != SQLITE_NULL) ||
        (!session && sqlite3_column_type(stmt, 3) != SQLITE_NULL)) {
        return sqlite_failure(sqlite3_db_handle(stmt), sqlite3_errcode(sqlite3_db_handle(stmt)),
            "Cannot read SQLite memory columns");
    }
    entry->key = cc_copy_string(key);
    entry->value = cc_copy_string(value);
    entry->category = category ? cc_copy_string(category) : NULL;
    entry->session_id = session ? cc_copy_string(session) : NULL;
    entry->created_at = (time_t)sqlite3_column_int64(stmt, 4);
    entry->updated_at = (time_t)sqlite3_column_int64(stmt, 5);
    if (!entry->key || !entry->value || (category && !entry->category) || (session && !entry->session_id)) {
        cc_memory_entry_free(entry);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy SQLite memory fields");
    }
    return cc_result_ok();
}

/**
 * @brief 在连接锁内收集条目，任一分配或 step 失败撤销整个结果数组。
 * @param[in] stmt 已准备并绑定的 SELECT，外层负责 finalize。
 * @param[in] limit 正数为本地行数上限，非正数读至 SQLITE_DONE。
 * @param[out] out_entries 新空槽，成功数组交调用者 memory_entry_free_array。
 * @param[out] out_count 新零槽，成功为实际项数。
 * @return 成功、容量上限、OOM 或原生存储错误，失败输出保持空。
 */
static cc_result_t collect_entries(sqlite3_stmt *stmt, int limit,
    cc_memory_entry_t **out_entries, size_t *out_count)
{
    size_t capacity = 16, count = 0;
    cc_memory_entry_t *entries = calloc(capacity, sizeof(*entries));
    if (!entries) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate SQLite memory entries");
    cc_result_t rc = cc_result_ok();
    int step = SQLITE_DONE;
    while (limit <= 0 || count < (size_t)limit) {
        step = sqlite3_step(stmt);
        if (step != SQLITE_ROW) break;
        if (count == capacity) {
            size_t maximum = SIZE_MAX / sizeof(*entries);
            if (capacity >= maximum) {
                rc = cc_result_error(CC_ERR_LIMIT_EXCEEDED, "SQLite memory entry capacity overflow");
                break;
            }
            size_t next_capacity = capacity > maximum / 2 ? maximum : capacity * 2;
            cc_memory_entry_t *next = realloc(entries, next_capacity * sizeof(*entries));
            if (!next) {
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow SQLite memory entries");
                break;
            }
            memset(next + capacity, 0, (next_capacity - capacity) * sizeof(*next));
            entries = next;
            capacity = next_capacity;
        }
        rc = copy_entry(stmt, &entries[count]);
        if (rc.code != CC_OK) break;
        count++;
    }
    if (rc.code == CC_OK && step != SQLITE_DONE && step != SQLITE_ROW)
        rc = sqlite_failure(sqlite3_db_handle(stmt), step, "SQLite memory read failed");
    if (rc.code != CC_OK) {
        cc_memory_entry_free_array(entries, count);
        return rc;
    }
    *out_entries = entries;
    *out_count = count;
    return rc;
}

/**
 * @brief 持锁读取指定键并深拷贝一个独立条目。
 * @param[in] self 存活后端借用。
 * @param[in] key 非 NULL 稳定键，空串按数据库规则匹配。
 * @param[out] out_entry 必填新槽，先清零，成功字段由调用者 memory_entry_free。
 * @return 成功、NOT_FOUND、参数/OOM 或 SQLite 错误，失败输出保持空。
 */
static cc_result_t sqlite_get(void *self, const char *key, cc_memory_entry_t *out_entry)
{
    if (out_entry) cc_memory_entry_init(out_entry);
    if (!self || !key || !out_entry)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid SQLite memory get");
    cc_sqlite_memory_store_t *s = self;
    cc_mutex_lock(s->mutex);
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(s->db,
        "SELECT key, value, category, session_id, created_at, updated_at FROM memory WHERE key=?",
        -1, &stmt, NULL);
    if (code == SQLITE_OK) code = sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    cc_result_t rc;
    if (code != SQLITE_OK) {
        rc = sqlite_failure(s->db, code, "SQLite memory get prepare/bind failed");
    } else {
        code = sqlite3_step(stmt);
        if (code == SQLITE_ROW) rc = copy_entry(stmt, out_entry);
        else if (code == SQLITE_DONE) rc = cc_result_errf(CC_ERR_NOT_FOUND, "Memory key not found: %s", key);
        else rc = sqlite_failure(s->db, code, "SQLite memory get failed");
    }
    rc = finish_statement(s->db, stmt, rc);
    if (rc.code != CC_OK) cc_memory_entry_free(out_entry);
    cc_mutex_unlock(s->mutex);
    return rc;
}

/**
 * @brief 为旧搜索接口动态构造完整的百分号 LIKE 模式并绑定四个参数。
 * @param[in,out] stmt 存活 SELECT，外层持连接锁。
 * @param[in] query 非 NULL 稳定文本，百分号和下划线沿原有 SQL 通配符语义。
 * @param[in] limit 已归一化的正行数上限。
 * @return 成功、容量/OOM 或原生绑定错误，模式始终在本函数归还。
 */
static cc_result_t bind_search_query(sqlite3_stmt *stmt, const char *query, int limit)
{
    size_t length = strlen(query);
    if (length > SIZE_MAX - 3)
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "SQLite search pattern overflow");
    char *like = malloc(length + 3);
    if (!like) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate SQLite search pattern");
    like[0] = '%';
    memcpy(like + 1, query, length);
    like[length + 1] = '%';
    like[length + 2] = '\0';
    int code = sqlite3_bind_text(stmt, 1, like, -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK) code = sqlite3_bind_text(stmt, 2, like, -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK) code = sqlite3_bind_text(stmt, 3, like, -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK) code = sqlite3_bind_int(stmt, 4, limit);
    free(like);
    return code == SQLITE_OK ? cc_result_ok() :
        sqlite_failure(sqlite3_db_handle(stmt), code, "SQLite search bind failed");
}

/**
 * @brief 持锁按 key/value/category 的 SQL LIKE 条件检索完整文本。
 * @param[in] self 存活后端借用。
 * @param[in] query 非 NULL 调用期文本，空串匹配全部，保留 SQL 通配符语义。
 * @param[in] limit 正数为 SQL 行数上限，非正数采用 100。
 * @param[out] out_entries 必填新拥有槽，存在时先清空，成功由调用者 free_array。
 * @param[out] out_count 必填数量槽，存在时先置零。
 * @return 成功或参数/容量/OOM/SQLite 错误，失败撤销整数组，不提供排序或分值。
 */
static cc_result_t sqlite_search(void *self, const char *query, int limit,
    cc_memory_entry_t **out_entries, size_t *out_count)
{
    if (out_entries) *out_entries = NULL;
    if (out_count) *out_count = 0;
    if (!self || !query || !out_entries || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid SQLite memory search");
    cc_sqlite_memory_store_t *s = self;
    cc_mutex_lock(s->mutex);
    const char *sql = "SELECT key, value, category, session_id, created_at, updated_at FROM memory "
        "WHERE key LIKE ? OR value LIKE ? OR category LIKE ? LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL);
    cc_result_t rc = code == SQLITE_OK ? bind_search_query(stmt, query, limit > 0 ? limit : 100) :
        sqlite_failure(s->db, code, "SQLite search prepare failed");
    if (rc.code == CC_OK) rc = collect_entries(stmt, 0, out_entries, out_count);
    rc = finish_statement(s->db, stmt, rc);
    if (rc.code != CC_OK) {
        cc_memory_entry_free_array(*out_entries, *out_count);
        *out_entries = NULL;
        *out_count = 0;
    }
    cc_mutex_unlock(s->mutex);
    return rc;
}

/**
 * @brief 持锁在 SQL LIMIT 前应用文本、分类、会话与两个排除分类，并返回拥有分值结果。
 * @param[in] self 非 NULL 存活后端。
 * @param[in] query 稳定条件借用，size 为零兼容基础布局，文本 NULL 为空串，最长一千字节。
 * @param[out] out_results 必填新拥有槽，存在时先置 NULL，成功交调用者 search_result_free_array。
 * @param[out] out_count 必填数量槽，存在时先置零。
 * @return 成功或参数/容量/OOM/SQLite 错误，失败撤销整个数组。
 * @note LIKE 转义百分号、下划线和反斜杠，按 updated_at 倒序，正 limit 生效，非正数默认 100。
 * 分值为固定词法命中等级，所有字段复制在锁内完成。
 */
static cc_result_t sqlite_query(
    void *self,
    const cc_memory_query_t *query,
    cc_memory_search_result_t **out_results,
    size_t *out_count)
{
    if (out_results) *out_results = NULL;
    if (out_count) *out_count = 0;
    if (!self || !query || !out_results || !out_count ||
        (query->size && query->size < offsetof(cc_memory_query_t, limit) + sizeof(query->limit))) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid SQLite memory query");
    }
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
        "AND (?6='' OR category IS NULL OR category<>?6) "
        "AND (?7='' OR category IS NULL OR category<>?7) "
        "ORDER BY updated_at DESC LIMIT ?5";
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL);
    if (code != SQLITE_OK) {
        cc_result_t error = sqlite_failure(s->db, code, "SQLite memory query prepare failed");
        sqlite3_finalize(stmt);
        cc_mutex_unlock(s->mutex);
        return error;
    }
    const char *text = query->query ? query->query : "";
    const char *category = query->category ? query->category : "";
    const char *session_id = query->session_id ? query->session_id : "";
    const char *excluded = cc_memory_query_excluded_category(query);
    const char *excluded2 = cc_memory_query_second_excluded_category(query);
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
    code = sqlite3_bind_text(stmt, 1, text, -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK) code = sqlite3_bind_text(stmt, 2, like, -1, SQLITE_TRANSIENT);
    free(like);
    if (code == SQLITE_OK) code = sqlite3_bind_text(stmt, 3, category, -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK) code = sqlite3_bind_text(stmt, 4, session_id, -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK) code = sqlite3_bind_int(stmt, 5, query->limit > 0 ? query->limit : 100);
    if (code == SQLITE_OK) code = sqlite3_bind_text(stmt, 6, excluded ? excluded : "", -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK) code = sqlite3_bind_text(stmt, 7, excluded2 ? excluded2 : "", -1, SQLITE_TRANSIENT);
    if (code != SQLITE_OK) {
        cc_result_t error = sqlite_failure(s->db, code, "SQLite memory query bind failed");
        sqlite3_finalize(stmt);
        cc_mutex_unlock(s->mutex);
        return error;
    }

    size_t capacity = query->limit > 0 && query->limit < 16 ?
        (size_t)query->limit : 16U;
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
        cc_result_t copied = copy_entry(stmt, &result->entry);
        if (copied.code != CC_OK) {
            cc_memory_search_result_free_array(results, count + 1);
            sqlite3_finalize(stmt);
            cc_mutex_unlock(s->mutex);
            return copied;
        }
        result->score = sqlite3_column_double(stmt, 6);
        count++;
    }
    cc_result_t rc = step_rc == SQLITE_DONE ? cc_result_ok() :
        sqlite_failure(s->db, step_rc, "SQLite memory query failed");
    rc = finish_statement(s->db, stmt, rc);
    cc_mutex_unlock(s->mutex);
    if (rc.code != CC_OK) {
        cc_memory_search_result_free_array(results, count);
        return rc;
    }
    *out_results = results;
    *out_count = count;
    return cc_result_ok();
}

/**
 * @brief 持锁按可选分类列举独立记忆条目。
 * @param[in] self 非 NULL 存活后端。
 * @param[in] category 调用期可空借用，NULL/空串选择全部，其他按准确分类匹配。
 * @param[in] limit 正数限制返回行数，非正数读取全部。
 * @param[out] out_entries 必填新拥有槽，存在时先置 NULL，成功交调用者 free_array。
 * @param[out] out_count 必填项数槽，存在时先置零。
 * @return 成功或参数/容量/OOM/SQLite 错误，失败撤销全部条目，顺序由 SQLite 决定。
 */
static cc_result_t sqlite_list(void *self, const char *category, int limit,
                                cc_memory_entry_t **out_entries, size_t *out_count)
{
    if (out_entries) *out_entries = NULL;
    if (out_count) *out_count = 0;
    if (!self || !out_entries || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid SQLite memory list");
    cc_sqlite_memory_store_t *s = self;
    cc_mutex_lock(s->mutex);

    const char *sql;
    if (category && category[0])
        sql = "SELECT key, value, category, session_id, created_at, updated_at FROM memory WHERE category=?";
    else
        sql = "SELECT key, value, category, session_id, created_at, updated_at FROM memory";

    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL);
    if (code == SQLITE_OK && category && category[0])
        code = sqlite3_bind_text(stmt, 1, category, -1, SQLITE_TRANSIENT);
    cc_result_t rc = code == SQLITE_OK ? collect_entries(stmt, limit, out_entries, out_count) :
        sqlite_failure(s->db, code, "SQLite memory list prepare/bind failed");
    rc = finish_statement(s->db, stmt, rc);
    if (rc.code != CC_OK) {
        cc_memory_entry_free_array(*out_entries, *out_count);
        *out_entries = NULL;
        *out_count = 0;
    }
    cc_mutex_unlock(s->mutex);
    return rc;
}

/**
 * @brief 持锁删除指定键，未命中保持幂等成功。
 * @param[in,out] self 非 NULL 存活后端。
 * @param[in] key 非 NULL 调用期稳定键，空串按 SQLite 语义。
 * @return 成功为 CC_OK，参数或原生错误为对应拥有结果，失败可能已有提交副作用。
 */
static cc_result_t sqlite_delete_entry(void *self, const char *key)
{
    cc_sqlite_memory_store_t *s = (cc_sqlite_memory_store_t *)self;
    if (!s || !key) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid SQLite memory delete");
    cc_mutex_lock(s->mutex);

    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(s->db, "DELETE FROM memory WHERE key=?", -1, &stmt, NULL);
    if (code == SQLITE_OK) code = sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK) code = sqlite3_step(stmt);
    cc_result_t rc = code == SQLITE_DONE ? cc_result_ok() :
        sqlite_failure(s->db, code, "SQLite memory delete failed");
    rc = finish_statement(s->db, stmt, rc);
    cc_mutex_unlock(s->mutex);
    return rc;
}

/**
 * @brief 持锁删除准确分类的全部记忆，未命中成功。
 * @param[in,out] self 非 NULL 存活后端。
 * @param[in] category 非 NULL 调用期稳定分类，空串只匹配空分类。
 * @return 语句完成为 CC_OK，参数或原生错误为对应拥有结果，失败副作用按 SQLite 提交状态核对。
 */
static cc_result_t sqlite_delete_by_category(void *self, const char *category)
{
    cc_sqlite_memory_store_t *s = (cc_sqlite_memory_store_t *)self;
    if (!s || !category)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid SQLite memory category delete");
    cc_mutex_lock(s->mutex);

    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(s->db, "DELETE FROM memory WHERE category=?", -1, &stmt, NULL);
    if (code == SQLITE_OK) code = sqlite3_bind_text(stmt, 1, category, -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK) code = sqlite3_step(stmt);
    cc_result_t rc = code == SQLITE_DONE ? cc_result_ok() :
        sqlite_failure(s->db, code, "SQLite memory category delete failed");
    rc = finish_statement(s->db, stmt, rc);
    cc_mutex_unlock(s->mutex);
    return rc;
}

/**
 * @brief 关闭连接、销毁 mutex 并归还后端状态。
 * @param[in,out] self 可空独占实例，owner 已关闭入口并排空全部方法和内部借用。
 * @post 非空 self 及其旧别名失效，端口描述符由调用者清空。
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

/** @brief 进程期静态方法表，结构化 query 在数据库内先过滤再 LIMIT。 */
static const cc_memory_store_vtable_t sqlite_vtable = {
    .set = sqlite_set,
    .get = sqlite_get,
    .search = sqlite_search,
    .list = sqlite_list,
    .delete_entry = sqlite_delete_entry,
    .delete_by_category = sqlite_delete_by_category,
    .destroy = sqlite_destroy,
    .query = sqlite_query,
};

/**
 * @brief 打开 SQLite、创建 mutex 并完成表初始化后交付实例。
 * @param[out] out_store 必填新拥有槽，存在时先清零，成功交付唯一 destroy 责任。
 * @param[in] db_path 必填稳定路径，父目录由外层准备，空串或 :memory: 沿 SQLite 语义。
 * @return 成功为 CC_OK，参数、OOM、mutex 或数据库失败为对应拥有结果，失败输出保持空。
 * @note 打开错误在 close 前复制诊断，任一创建失败关闭连接并释放本次状态/已创建锁。
 * 初始化失败可能已改变数据库表结构，实例成功后 owner 关闭入口并排空方法再 destroy。
 */
cc_result_t cc_memory_store_create_sqlite(cc_memory_store_t *out_store, const char *db_path)
{
    if (!out_store)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null SQLite memory store output");
    memset(out_store, 0, sizeof(*out_store));
    if (!db_path)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null SQLite memory database path");

    cc_sqlite_memory_store_t *s = calloc(1, sizeof(cc_sqlite_memory_store_t));
    if (!s) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate sqlite memory store");

    int rc = sqlite3_open(db_path, &s->db);
    if (rc != SQLITE_OK) {
        cc_result_t error = sqlite_failure(s->db, rc, "SQLite memory open failed");
        if (s->db) sqlite3_close(s->db);
        free(s);
        return error;
    }

    cc_result_t mutex_rc = cc_mutex_create(&s->mutex);
    if (mutex_rc.code != CC_OK) {
        sqlite3_close(s->db);
        free(s);
        return mutex_rc;
    }
    cc_result_free(&mutex_rc);
    rc = init_db(s->db);
    if (rc != SQLITE_OK) {
        cc_result_t error = sqlite_failure(s->db, rc, "SQLite memory schema initialization failed");
        sqlite3_close(s->db);
        cc_mutex_destroy(s->mutex);
        free(s);
        return error;
    }

    out_store->self = s;
    out_store->vtable = &sqlite_vtable;
    return cc_result_ok();
}
