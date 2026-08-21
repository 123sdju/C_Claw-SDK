



#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_thread.h"
#include "cc/internal/cc_alloc.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifndef CC_DEFAULT_STORAGE_PATH
#define CC_DEFAULT_STORAGE_PATH "runtime/data/c-claw.db"
#endif

#ifndef CC_DEFAULT_WORKSPACE_PATH
#define CC_DEFAULT_WORKSPACE_PATH "runtime/workspace"
#endif

/*
 * SQLite session store 私有状态。
 *
 * db 是 sqlite3 连接，mutex 在 SDK 层串行化访问，use_wal 记录预期 journaling 策略。
 * 即使 SQLite 以 FULLMUTEX 打开，外层 mutex 仍让 vtable 操作的事务边界更容易推理。
 */
typedef struct {
    sqlite3 *db;
    int use_wal;
    cc_mutex_t mutex;
    cc_mutex_t batch_mutex;
} cc_sqlite_session_store_t;

/*
 * 生成 ISO-like 当前时间字符串。
 *
 * 返回值由调用方 free；使用 localtime_r/localtime_s 避免静态 tm 缓冲导致的线程安全问题。
 */
static char *now_string(void)
{
    time_t t = time(NULL);
    char buf[64];
#ifdef _WIN32
    struct tm tm_info;
    localtime_s(&tm_info, &t);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_info);
#else
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_info);
#endif
    return cc_copy_string(buf);
}

/* 解析数据库路径；传入 NULL 时使用 SDK 默认 SQLite 路径。返回字符串由调用方释放。 */
static char *build_db_path(const char *db_path)
{
    return cc_copy_string(db_path ? db_path : CC_DEFAULT_STORAGE_PATH);
}

/*
 * 设置 SQLite pragma。
 *
 * WAL 提升并发读写体验，foreign_keys 保证 tool_results/tool_calls 关联，busy_timeout 避免
 * 短暂锁竞争直接失败。use_wal 为假时回退 DELETE journal，适配不支持 WAL 的文件系统。
 */
static void setup_pragmas(sqlite3 *db, int use_wal)
{
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA busy_timeout=5000", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA cache_size=-8000", NULL, NULL, NULL);
    if (!use_wal) {
        sqlite3_exec(db, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL);
    }
}

/*
 * SQLite 初始化 SQL。
 *
 * session/message/tool_call/tool_result 分表存储，messages 同时保存 text summary 和
 * content_parts/tool_calls JSON，兼顾旧文本上下文和多模态/工具调用恢复。
 */
static const char *init_sql =
    "CREATE TABLE IF NOT EXISTS sessions ("
    "  id TEXT PRIMARY KEY,"
    "  name TEXT,"
    "  workspace_dir TEXT,"
    "  model TEXT,"
    "  status TEXT,"
    "  created_at TEXT,"
    "  updated_at TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS messages ("
    "  id TEXT PRIMARY KEY,"
    "  session_id TEXT NOT NULL,"
    "  role TEXT NOT NULL,"
    "  text TEXT,"
    "  content_parts TEXT,"
    "  tool_calls TEXT,"
    "  reasoning_content TEXT,"
    "  tool_call_id TEXT,"
    "  created_at TEXT,"
    "  FOREIGN KEY(session_id) REFERENCES sessions(id)"
    ");"
    "CREATE TABLE IF NOT EXISTS tool_calls ("
    "  id TEXT PRIMARY KEY,"
    "  session_id TEXT NOT NULL,"
    "  name TEXT NOT NULL,"
    "  arguments_json TEXT,"
    "  status TEXT,"
    "  created_at TEXT,"
    "  finished_at TEXT,"
    "  FOREIGN KEY(session_id) REFERENCES sessions(id)"
    ");"
    "CREATE TABLE IF NOT EXISTS tool_results ("
    "  id TEXT PRIMARY KEY,"
    "  tool_call_id TEXT NOT NULL,"
    "  ok INTEGER,"
    "  text TEXT,"
    "  error TEXT,"
    "  metadata TEXT,"
    "  artifacts TEXT,"
    "  created_at TEXT,"
    "  FOREIGN KEY(tool_call_id) REFERENCES tool_calls(id)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id, created_at);"
    "CREATE INDEX IF NOT EXISTS idx_tool_calls_session ON tool_calls(session_id, created_at);"
    "CREATE INDEX IF NOT EXISTS idx_tool_results_tool_call ON tool_results(tool_call_id);";

/*
 * 执行兼容性 ALTER。
 *
 * 已存在列会被视为成功，便于旧数据库重复启动。其它 SQLite 错误映射为 CC_ERR_STORAGE。
 */
static cc_result_t sqlite_exec_compatible_alter(sqlite3 *db, const char *sql)
{
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc == SQLITE_OK) {
        return cc_result_ok();
    }
    if (errmsg && strstr(errmsg, "duplicate column name")) {
        sqlite3_free(errmsg);
        return cc_result_ok();
    }
    cc_result_t result = cc_result_error(CC_ERR_STORAGE,
        errmsg ? errmsg : sqlite3_errmsg(db));
    sqlite3_free(errmsg);
    return result;
}

/*
 * 运行轻量 schema 迁移。
 *
 * 只做向后兼容新增列，不做破坏性迁移；这符合 SDK adapter 的保守边界，复杂迁移应由
 * 下游产品数据库版本管理负责。
 */
static cc_result_t sqlite_run_compat_migrations(sqlite3 *db)
{
    cc_result_t rc = sqlite_exec_compatible_alter(
        db, "ALTER TABLE messages ADD COLUMN content_parts TEXT");
    if (rc.code != CC_OK) return rc;
    return sqlite_exec_compatible_alter(
        db, "ALTER TABLE tool_results ADD COLUMN artifacts TEXT");
}

/*
 * 创建或确保 session 存在。
 *
 * 使用 INSERT OR IGNORE 保证重复 create_session 幂等；workspace_dir 会持久化，后续文件
 * 工具和 UI 可从 session 元数据恢复工作区边界。
 */
static cc_result_t sqlite_create_session(
    void *self,
    const char *session_id,
    const char *workspace_dir
)
{
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;
    char *now = now_string();

    cc_mutex_lock(store->mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT OR IGNORE INTO sessions (id, workspace_dir, status, created_at, updated_at) "
                      "VALUES (?1, ?2, 'active', ?3, ?3)";
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        free(now);
        cc_mutex_unlock(store->mutex);
        return result;
    }

    sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2,
        workspace_dir ? workspace_dir : CC_DEFAULT_WORKSPACE_PATH,
        -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, now, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(now);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        cc_mutex_unlock(store->mutex);
        return result;
    }
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/*
 * 追加消息记录。
 *
 * 消息文本摘要、content_parts JSON、tool_calls JSON 同时写入，既兼容简单文本上下文，也
 * 支持多模态和工具调用历史恢复。所有 sqlite3_bind 使用 TRANSIENT/STATIC 按变量生命期选择。
 */
static cc_result_t sqlite_append_message(
    void *self,
    const cc_message_t *message
)
{
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;
    char *now = now_string();
    const char *role_str = cc_message_role_string(message->role);

    cc_mutex_lock(store->mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO messages (id, session_id, role, text, content_parts, tool_calls, reasoning_content, tool_call_id, created_at) "
                      "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)";
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        free(now);
        cc_mutex_unlock(store->mutex);
        return result;
    }

    sqlite3_bind_text(stmt, 1, message->id ? message->id : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, message->session_id ? message->session_id : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, role_str, -1, SQLITE_STATIC);
    char *summary = NULL;
    cc_message_get_text_summary(message, &summary);
    char *content_parts = NULL;
    cc_content_parts_to_json(&message->content, &content_parts);
    char *tool_calls = NULL;
    cc_tool_call_list_to_json(&message->tool_calls, &tool_calls);
    sqlite3_bind_text(stmt, 4, summary ? summary : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, content_parts ? content_parts : "[]", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, tool_calls ? tool_calls : "[]", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, message->reasoning_content ? message->reasoning_content : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, message->tool_call_id ? message->tool_call_id : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 9, now, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    free(summary);
    free(content_parts);
    free(tool_calls);
    sqlite3_finalize(stmt);
    free(now);

    if (rc != SQLITE_DONE) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        cc_mutex_unlock(store->mutex);
        return result;
    }
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/*
 * 安全 realloc helper。
 *
 * realloc 失败时返回原指针，调用方通过“返回值仍等于原指针”判断失败并清理旧数组，避免
 * 直接覆盖指针造成内存泄漏。
 */
static void *safe_realloc(void *ptr, size_t new_size)
{
    void *new_ptr = realloc(ptr, new_size);
    return new_ptr ? new_ptr : ptr;
}

/*
 * 加载指定 session 的消息。
 *
 * 返回数组由调用方拥有，需要逐条 cc_message_cleanup 后 free。结果按 created_at 升序，
 * 用于 context builder 恢复历史上下文。
 */
static cc_result_t sqlite_load_messages(
    void *self,
    const char *session_id,
    int limit,
    cc_message_t **out_messages,
    size_t *out_count
)
{
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;

    cc_mutex_lock(store->mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, session_id, role, text, content_parts, tool_calls, reasoning_content, tool_call_id, created_at "
                      "FROM messages WHERE session_id = ?1 "
                      "ORDER BY created_at ASC LIMIT ?2";
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        cc_mutex_unlock(store->mutex);
        return result;
    }

    sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, limit);

    size_t cap = 16;
    size_t count = 0;
    cc_message_t *messages = calloc(cap, sizeof(cc_message_t));
    if (!messages) {
        sqlite3_finalize(stmt);
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate messages");
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            void *new_ptr = safe_realloc(messages, cap * 2 * sizeof(cc_message_t));
            if (new_ptr == messages && cap * 2 > cap) {
                sqlite3_finalize(stmt);
                for (size_t i = 0; i < count; i++) {
                    cc_message_cleanup(&messages[i]);
                }
                free(messages);
                cc_mutex_unlock(store->mutex);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow messages");
            }
            messages = new_ptr;
            cap *= 2;
        }

        cc_message_t *msg = &messages[count];
        memset(msg, 0, sizeof(cc_message_t));

        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        const char *sid = (const char *)sqlite3_column_text(stmt, 1);
        const char *role_str = (const char *)sqlite3_column_text(stmt, 2);
        const char *content = (const char *)sqlite3_column_text(stmt, 3);
        const char *cpj = (const char *)sqlite3_column_text(stmt, 4);
        const char *tcj = (const char *)sqlite3_column_text(stmt, 5);
        const char *reasoning = (const char *)sqlite3_column_text(stmt, 6);
        const char *tci = (const char *)sqlite3_column_text(stmt, 7);

        msg->id = id ? cc_copy_string(id) : NULL;
        msg->session_id = sid ? cc_copy_string(sid) : NULL;
        cc_content_parts_init(&msg->content);
        cc_tool_call_list_init(&msg->tool_calls);
        if (cpj && cpj[0]) {
            cc_content_parts_from_json(cpj, &msg->content);
        } else {
            cc_content_parts_append_text(&msg->content, content ? content : "", CC_CONTENT_PART_INPUT);
        }
        if (tcj && tcj[0]) {
            cc_tool_call_list_from_json(tcj, &msg->tool_calls);
        }
        msg->reasoning_content = (reasoning && reasoning[0]) ? cc_copy_string(reasoning) : NULL;
        msg->tool_call_id = (tci && tci[0]) ? cc_copy_string(tci) : NULL;

        if (role_str) {
            if (strcmp(role_str, "system") == 0) msg->role = CC_ROLE_SYSTEM;
            else if (strcmp(role_str, "user") == 0) msg->role = CC_ROLE_USER;
            else if (strcmp(role_str, "assistant") == 0) msg->role = CC_ROLE_ASSISTANT;
            else if (strcmp(role_str, "tool") == 0) msg->role = CC_ROLE_TOOL;
        }

        count++;
    }
    sqlite3_finalize(stmt);

    *out_messages = messages;
    *out_count = count;
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/*
 * 追加 tool call 记录。
 *
 * tool call 与普通 message 分表保存，便于审计和调试界面展示模型请求过哪些工具及参数。
 */
static cc_result_t sqlite_append_tool_call(
    void *self,
    const char *session_id,
    const cc_tool_call_t *call
)
{
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;
    char *now = now_string();

    cc_mutex_lock(store->mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO tool_calls (id, session_id, name, arguments_json, status, created_at) "
                      "VALUES (?1, ?2, ?3, ?4, 'completed', ?5)";
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        free(now);
        cc_mutex_unlock(store->mutex);
        return result;
    }

    sqlite3_bind_text(stmt, 1, call->id ? call->id : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, session_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, call->name ? call->name : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, call->arguments_json ? call->arguments_json : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, now, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(now);

    if (rc != SQLITE_DONE) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        cc_mutex_unlock(store->mutex);
        return result;
    }
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/*
 * 追加 tool result 记录。
 *
 * result 的 artifact list 序列化成 JSON 字符串保存，避免数据库 schema 需要为多模态资源
 * 单独建多张表；更复杂的资源生命周期可由 artifact store 承担。
 */
static cc_result_t sqlite_append_tool_result(
    void *self,
    const char *session_id,
    const char *tool_call_id,
    const cc_tool_result_t *result
)
{
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;
    char *now = now_string();
    (void)session_id;

    cc_mutex_lock(store->mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO tool_results (id, tool_call_id, ok, text, error, metadata, artifacts, created_at) "
                      "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)";
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        free(now);
        cc_mutex_unlock(store->mutex);
        return result;
    }

    sqlite3_bind_text(stmt, 1, tool_call_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, tool_call_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, result->ok ? 1 : 0);
    sqlite3_bind_text(stmt, 4, result->text ? result->text : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, result->error ? result->error : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, result->metadata ? result->metadata : "", -1, SQLITE_STATIC);
    char *artifacts = NULL;
    cc_media_artifact_list_to_json(&result->artifacts, &artifacts);
    sqlite3_bind_text(stmt, 7, artifacts ? artifacts : "[]", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, now, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    free(artifacts);
    sqlite3_finalize(stmt);
    free(now);

    if (rc != SQLITE_DONE) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        cc_mutex_unlock(store->mutex);
        return result;
    }
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/*
 * 列举所有 session。
 *
 * 返回数组由调用方拥有。查询按 updated_at 倒序，便于 UI 展示最近会话。
 */
static cc_result_t sqlite_list_sessions(
    void *self,
    cc_session_t **out_sessions,
    size_t *out_count
)
{
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;

    cc_mutex_lock(store->mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, name, workspace_dir, model, status, created_at, updated_at "
                      "FROM sessions ORDER BY updated_at DESC";
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
        cc_mutex_unlock(store->mutex);
        return result;
    }

    size_t cap = 16;
    size_t count = 0;
    cc_session_t *sessions = calloc(cap, sizeof(cc_session_t));
    if (!sessions) {
        sqlite3_finalize(stmt);
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate sessions");
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            void *new_ptr = safe_realloc(sessions, cap * 2 * sizeof(cc_session_t));
            if (new_ptr == sessions && cap * 2 > cap) {
                sqlite3_finalize(stmt);
                for (size_t i = 0; i < count; i++) {
                    free(sessions[i].id);
                    free(sessions[i].name);
                    free(sessions[i].workspace_dir);
                    free(sessions[i].model);
                    free(sessions[i].created_at);
                    free(sessions[i].updated_at);
                }
                free(sessions);
                cc_mutex_unlock(store->mutex);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow sessions");
            }
            sessions = new_ptr;
            cap *= 2;
        }

        cc_session_t *s = &sessions[count];
        memset(s, 0, sizeof(cc_session_t));

        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        const char *ws = (const char *)sqlite3_column_text(stmt, 2);
        const char *model = (const char *)sqlite3_column_text(stmt, 3);

        s->id = id ? cc_copy_string(id) : NULL;
        s->name = name ? cc_copy_string(name) : NULL;
        s->workspace_dir = ws ? cc_copy_string(ws) : NULL;
        s->model = model ? cc_copy_string(model) : NULL;
        count++;
    }
    sqlite3_finalize(stmt);

    *out_sessions = sessions;
    *out_count = count;
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/*
 * 执行带 session_id 参数的删除语句。
 *
 * clear_session 在事务中按依赖顺序删除 tool_results/tool_calls/messages；这个 helper
 * 统一 prepared statement 和错误映射。
 */
static cc_result_t sqlite_exec_bound_delete(
    cc_sqlite_session_store_t *store,
    const char *sql,
    const char *session_id
)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
    }
    sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return cc_result_error(CC_ERR_STORAGE, sqlite3_errmsg(store->db));
    }
    return cc_result_ok();
}

/*
 * 清空某个 session 的上下文历史和工具审计记录。
 *
 * 使用 BEGIN IMMEDIATE 获得写事务；任何一步失败都会 ROLLBACK，避免 messages 和 tool
 * 记录出现半清理状态。session 元数据本身保留。
 */
static cc_result_t sqlite_clear_session(void *self, const char *session_id)
{
    if (!self || !session_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid session clear request");
    }
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;
    cc_mutex_lock(store->mutex);
    char *errmsg = NULL;
    if (sqlite3_exec(store->db, "BEGIN IMMEDIATE", NULL, NULL, &errmsg) != SQLITE_OK) {
        cc_result_t rc = cc_result_error(CC_ERR_STORAGE, errmsg ? errmsg : sqlite3_errmsg(store->db));
        sqlite3_free(errmsg);
        cc_mutex_unlock(store->mutex);
        return rc;
    }

    cc_result_t rc = sqlite_exec_bound_delete(
        store,
        "DELETE FROM tool_results WHERE tool_call_id IN "
        "(SELECT id FROM tool_calls WHERE session_id=?1)",
        session_id
    );
    if (rc.code == CC_OK) {
        rc = sqlite_exec_bound_delete(
            store, "DELETE FROM tool_calls WHERE session_id=?1", session_id);
    }
    if (rc.code == CC_OK) {
        rc = sqlite_exec_bound_delete(
            store, "DELETE FROM messages WHERE session_id=?1", session_id);
    }

    if (rc.code == CC_OK) {
        sqlite3_exec(store->db, "COMMIT", NULL, NULL, NULL);
    } else {
        sqlite3_exec(store->db, "ROLLBACK", NULL, NULL, NULL);
    }
    cc_mutex_unlock(store->mutex);
    return rc;
}

/*
 * 销毁 SQLite session store。
 *
 * 关闭前执行 optimize；调用方必须保证没有并发操作仍在使用该 store。mutex 销毁前后顺序
 * 保证 sqlite3_close 不和其它 vtable 操作交错。
 */
static void sqlite_destroy(void *self)
{
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;
    if (!store) return;
    cc_mutex_lock(store->mutex);
    if (store->db) {
        sqlite3_exec(store->db, "PRAGMA analysis_limit=0", NULL, NULL, NULL);
        sqlite3_exec(store->db, "PRAGMA optimize", NULL, NULL, NULL);
        sqlite3_close(store->db);
    }
    cc_mutex_unlock(store->mutex);
    cc_mutex_destroy(store->mutex);
    cc_mutex_destroy(store->batch_mutex);
    free(store);
}

static cc_result_t sqlite_append_records(
    void *self,
    const cc_session_record_t *records,
    size_t count
)
{
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;
    if (!store || (!records && count > 0)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid SQLite store batch");
    }

    cc_mutex_lock(store->batch_mutex);
    cc_mutex_lock(store->mutex);
    int sql_rc = sqlite3_exec(store->db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    cc_mutex_unlock(store->mutex);
    if (sql_rc != SQLITE_OK) {
        cc_mutex_unlock(store->batch_mutex);
        return cc_result_error(CC_ERR_STORAGE, "Failed to begin session batch");
    }

    cc_result_t rc = cc_result_ok();
    for (size_t i = 0; i < count && rc.code == CC_OK; i++) {
        const cc_session_record_t *record = &records[i];
        switch (record->type) {
        case CC_SESSION_RECORD_MESSAGE:
            rc = sqlite_append_message(self, record->data.message);
            break;
        case CC_SESSION_RECORD_TOOL_CALL:
            rc = sqlite_append_tool_call(self, record->session_id, record->data.tool_call);
            break;
        case CC_SESSION_RECORD_TOOL_RESULT:
            rc = sqlite_append_tool_result(self, record->session_id,
                record->data.tool_result.tool_call_id,
                record->data.tool_result.result);
            break;
        default:
            rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Unknown session record type");
            break;
        }
    }

    cc_mutex_lock(store->mutex);
    sql_rc = sqlite3_exec(store->db,
        rc.code == CC_OK ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    cc_mutex_unlock(store->mutex);
    cc_mutex_unlock(store->batch_mutex);
    if (rc.code == CC_OK && sql_rc != SQLITE_OK) {
        return cc_result_error(CC_ERR_STORAGE, "Failed to commit session batch");
    }
    return rc;
}

/* SQLite session store vtable，将数据库实现绑定到 cc_session_store_t 端口。 */
static cc_session_store_vtable_t sqlite_vtable = {
    sqlite_create_session,
    sqlite_append_records,
    sqlite_load_messages,
    sqlite_list_sessions,
    sqlite_clear_session,
    sqlite_destroy
};

/*
 * 创建 SQLite session store。
 *
 * 成功后 out_store 获得 self/vtable；函数负责打开数据库、设置 pragma、初始化 schema 和
 * 运行兼容迁移。db_path 为空时使用默认路径，父目录仍应由 builder/filesystem 层创建。
 */
cc_result_t cc_sqlite_session_store_create(const char *db_path, cc_session_store_t *out_store)
{
    if (!out_store) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null SQLite session store output");
    }
    memset(out_store, 0, sizeof(*out_store));
    cc_sqlite_session_store_t *self = calloc(1, sizeof(cc_sqlite_session_store_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create SQLite store");

    cc_result_t mutex_rc = cc_mutex_create(&self->mutex);
    if (mutex_rc.code != CC_OK) {
        free(self);
        return mutex_rc;
    }
    mutex_rc = cc_mutex_create(&self->batch_mutex);
    if (mutex_rc.code != CC_OK) {
        cc_mutex_destroy(self->mutex);
        free(self);
        return mutex_rc;
    }

    char *resolved_path = build_db_path(db_path);

    int rc = sqlite3_open_v2(resolved_path, &self->db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        NULL);
    free(resolved_path);

    if (rc != SQLITE_OK) {
        const char *err = sqlite3_errmsg(self->db);
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, err ? err : "Cannot open database");
        if (self->db) sqlite3_close(self->db);
        cc_mutex_destroy(self->mutex);
        cc_mutex_destroy(self->batch_mutex);
        free(self);
        return result;
    }

    setup_pragmas(self->db, 1);

    char *err = NULL;
    rc = sqlite3_exec(self->db, init_sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        cc_result_t result = cc_result_error(CC_ERR_STORAGE, err ? err : "Failed to init tables");
        sqlite3_free(err);
        sqlite3_close(self->db);
        cc_mutex_destroy(self->mutex);
        cc_mutex_destroy(self->batch_mutex);
        free(self);
        return result;
    }

    cc_result_t mig_rc = sqlite_run_compat_migrations(self->db);
    if (mig_rc.code != CC_OK) {
        sqlite3_close(self->db);
        cc_mutex_destroy(self->mutex);
        cc_mutex_destroy(self->batch_mutex);
        free(self);
        return mig_rc;
    }

    out_store->self = self;
    out_store->vtable = &sqlite_vtable;
    return cc_result_ok();
}
