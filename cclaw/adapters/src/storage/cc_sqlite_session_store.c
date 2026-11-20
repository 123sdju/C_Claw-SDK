



#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_thread.h"
#include "cc/internal/cc_alloc.h"
#include "cc_json_record_validation.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>

#ifndef CC_DEFAULT_STORAGE_PATH
#define CC_DEFAULT_STORAGE_PATH "runtime/data/c-claw.db"
#endif

#ifndef CC_DEFAULT_WORKSPACE_PATH
#define CC_DEFAULT_WORKSPACE_PATH "runtime/workspace"
#endif

/** @brief 后端独占连接和进程内操作锁，写入事务覆盖整次批量调用。
 * @note transaction_failed 锁存无法确认退出的原生事务，随后方法拒绝操作直到 owner 重开。
 * owner 在 destroy 前关闭入口并排空所有方法，SQLite 自身处理其他连接的竞争。
 */
typedef struct {
    sqlite3 *db;
    cc_mutex_t mutex;
    int transaction_failed;
} cc_sqlite_session_store_t;

/** @brief 取得本地日历时间并复制为秒精度 ISO-like 文本。
 * @param[out] out_text 必填空拥有槽，先清空，成功由调用者 free。
 * @return 时钟/日历/格式化失败为 STORAGE，复制失败为 OUT_OF_MEMORY，诊断归接收者释放。
 * @note 使用调用栈内 tm，可并发调用，时间文本沿当前系统本地区域规则解释。
 */
static cc_result_t now_string(char **out_text)
{
    *out_text = NULL;
    time_t t = time(NULL);
    char buf[64] = {0};
    struct tm tm_info;
    if (t == (time_t)-1)
        return cc_result_error(CC_ERR_STORAGE, "Cannot read SQLite timestamp clock");
#ifdef _WIN32
    if (localtime_s(&tm_info, &t) != 0)
#else
    if (!localtime_r(&t, &tm_info))
#endif
        return cc_result_error(CC_ERR_STORAGE, "Cannot convert SQLite timestamp");
    if (!strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_info))
        return cc_result_error(CC_ERR_STORAGE, "Cannot format SQLite timestamp");
    *out_text = cc_copy_string(buf);
    return *out_text ? cc_result_ok() :
        cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy SQLite timestamp");
}

/** @brief 复制数据库路径，NULL 使用编译默认值。
 * @param[in] db_path 同步 NUL 文本借用，可空，空串沿 SQLite 临时数据库语义。
 * @return 独占副本或 NULL，接收者 free，无连接或文件副作用。
 */
static char *build_db_path(const char *db_path)
{
    return cc_copy_string(db_path ? db_path : CC_DEFAULT_STORAGE_PATH);
}

/** @brief 配置 journal、外键、等待时限、同步和缓存策略。
 * @param[in,out] db 新建连接借用，尚未向外发布。
 * @param[in] use_wal 非零请求 WAL，零请求 DELETE，原生内存库沿自身 journal 模式。
 * @return 首个原生错误或成功，OUT_OF_MEMORY 单独分类，结果诊断由接收者释放。
 */
static cc_result_t setup_pragmas(sqlite3 *db, int use_wal)
{
    const char *statements[] = {
        use_wal ? "PRAGMA journal_mode=WAL" : "PRAGMA journal_mode=DELETE",
        "PRAGMA foreign_keys=ON", "PRAGMA busy_timeout=5000",
        "PRAGMA synchronous=NORMAL", "PRAGMA cache_size=-8000"
    };
    for (size_t i = 0; i < sizeof(statements) / sizeof(statements[0]); i++) {
        int rc = sqlite3_exec(db, statements[i], NULL, NULL, NULL);
        if (rc != SQLITE_OK)
            return cc_result_error(rc == SQLITE_NOMEM ? CC_ERR_OUT_OF_MEMORY : CC_ERR_STORAGE,
                                   sqlite3_errmsg(db));
    }
    return cc_result_ok();
}

/** @brief 消费已 prepare 的语句，先保存执行错误再 finalize。
 * @param[in] store 持有操作锁的存活后端借用。
 * @param[in] stmt 可空原生语句，返回后即失效。
 * @param[in] rc 最近 prepare/bind/step 结果，SQLITE_DONE 表示正常执行完毕。
 * @return 原执行或 finalize 首错，NOMEM 映射 OUT_OF_MEMORY，其余 STORAGE，诊断由接收者释放。
 */
static cc_result_t sqlite_finish_statement(cc_sqlite_session_store_t *store,
                                          sqlite3_stmt *stmt, int rc)
{
    cc_result_t result = rc == SQLITE_DONE ? cc_result_ok() :
        cc_result_error(rc == SQLITE_NOMEM ? CC_ERR_OUT_OF_MEMORY : CC_ERR_STORAGE,
                        sqlite3_errmsg(store->db));
    int finish_rc = sqlite3_finalize(stmt);
    if (result.code == CC_OK && finish_rc != SQLITE_OK)
        result = cc_result_error(finish_rc == SQLITE_NOMEM ? CC_ERR_OUT_OF_MEMORY : CC_ERR_STORAGE,
                                 sqlite3_errmsg(store->db));
    return result;
}

/** @brief 提交成功操作或回滚失败操作，并核对连接已经退出事务。
 * @param[in,out] store 当前任务持操作锁的后端借用，失败可锁存 transaction_failed。
 * @param[in] result 按值移动的操作结果，首错继续交付给接收者释放。
 * @return 原错误、提交错误或成功，回滚失败且仍在事务时锁存并阻止后续方法。
 */
static cc_result_t sqlite_finish_transaction(cc_sqlite_session_store_t *store, cc_result_t result)
{
    if (result.code == CC_OK) {
        int rc = sqlite3_exec(store->db, "COMMIT", NULL, NULL, NULL);
        if (rc != SQLITE_OK)
            result = cc_result_error(rc == SQLITE_NOMEM ? CC_ERR_OUT_OF_MEMORY : CC_ERR_STORAGE,
                                     sqlite3_errmsg(store->db));
        else if (!sqlite3_get_autocommit(store->db))
            result = cc_result_error(CC_ERR_STORAGE, "SQLite commit left an active transaction");
    }
    if (!sqlite3_get_autocommit(store->db)) {
        int rc = sqlite3_exec(store->db, "ROLLBACK", NULL, NULL, NULL);
        if (!sqlite3_get_autocommit(store->db)) {
            store->transaction_failed = 1;
            if (result.code == CC_OK)
                result = cc_result_error(rc == SQLITE_NOMEM ? CC_ERR_OUT_OF_MEMORY : CC_ERR_STORAGE,
                                         "SQLite transaction requires reopening");
        }
    }
    return result;
}

/*
 * SQLite 初始化 SQL。
 *
 * session/message/tool_call/tool_result 分表存储，messages 同时保存 text summary 和
 * content_parts/tool_calls JSON，用于多模态和工具调用恢复。
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
    "  tool_outcome TEXT,"
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

/** @brief 在操作锁内确保指定会话存在，重复 ID 保留原元数据。
 * @param[in,out] self 存活后端借用，NULL 拒绝。
 * @param[in] session_id 非 NULL 同步 NUL 标识借用，空串由原生键语义接受。
 * @param[in] workspace_dir 可空同步路径借用，NULL 使用默认工作目录。
 * @return 参数、时间、分配或原生存储结果，诊断归接收者释放，输入始终归调用者。
 */
static cc_result_t sqlite_create_session(
    void *self,
    const char *session_id,
    const char *workspace_dir
)
{
    if (!self || !session_id)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid SQLite session creation");
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;
    char *now = NULL;

    cc_mutex_lock(store->mutex);
    if (store->transaction_failed) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_STORAGE, "SQLite transaction requires reopening");
    }
    cc_result_t result = now_string(&now);
    if (result.code != CC_OK) {
        cc_mutex_unlock(store->mutex);
        return result;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT OR IGNORE INTO sessions (id, workspace_dir, status, created_at, updated_at) "
                      "VALUES (?1, ?2, 'active', ?3, ?3)";
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2,
        workspace_dir ? workspace_dir : CC_DEFAULT_WORKSPACE_PATH,
        -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, now, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    result = sqlite_finish_statement(store, stmt, rc);
    free(now);
    cc_mutex_unlock(store->mutex);
    return result;
}

/** @brief 在外层事务锁内序列化并插入一条完整消息。
 * @param[in,out] self 存活后端借用，调用者已经持操作锁并开启事务。
 * @param[in] message 必填稳定消息借用，非空身份、有效角色及工具结果语义必需。
 * @return 序列化、时间、分配或原生首错，临时 JSON/摘要/语句均在本层回收。
 * @note 输入存活到 finalize，显式 created_at 原样保存，NULL 使用当前本地时间。
 */
static cc_result_t sqlite_append_message(
    void *self,
    const cc_message_t *message
)
{
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;
    if (!message || !message->id || !message->id[0] ||
        !message->session_id || !message->session_id[0] ||
        message->role < CC_ROLE_SYSTEM || message->role > CC_ROLE_TOOL ||
        !cc_message_tool_outcome_valid(message))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid message tool outcome");
    char *now = NULL;
    char *summary = NULL;
    char *content_parts = NULL;
    char *tool_calls = NULL;
    cc_result_t result = message->created_at ? cc_result_ok() : now_string(&now);
    if (result.code == CC_OK) result = cc_message_get_text_summary(message, &summary);
    if (result.code == CC_OK) result = cc_content_parts_to_json(&message->content, &content_parts);
    if (result.code == CC_OK) result = cc_tool_call_list_to_json(&message->tool_calls, &tool_calls);
    if (result.code != CC_OK) {
        free(now);
        free(summary);
        free(content_parts);
        free(tool_calls);
        return result;
    }
    const char *role_str = cc_message_role_string(message->role);

    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO messages (id, session_id, role, text, content_parts, tool_calls, reasoning_content, tool_call_id, created_at, tool_outcome) "
                      "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)";
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, message->id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, message->session_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, role_str, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 4, summary ? summary : "", -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 5, content_parts, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 6, tool_calls, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 7, message->reasoning_content ? message->reasoning_content : "", -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 8, message->tool_call_id ? message->tool_call_id : "", -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 9, message->created_at ? message->created_at : now, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 10, cc_message_tool_outcome_string(message->tool_outcome), -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    result = sqlite_finish_statement(store, stmt, rc);
    free(summary);
    free(content_parts);
    free(tool_calls);
    free(now);
    return result;
}

/** @brief 操作锁内读取会话最近若干消息并按历史顺序交付独立深副本。
 * @param[in] self 存活后端借用，事务故障锁存时拒绝读取。
 * @param[in] session_id 必填同步 NUL 过滤键借用。
 * @param[in] limit 正值取最近若干条，非正值读取全部，排序按时间及 rowid 稳定处理。
 * @param[out] out_messages 必填新拥有槽，参数有效后先清空，成功逐条 cleanup 后 free。
 * @param[out] out_count 必填计数槽，参数有效后先清零。
 * @return 参数、OOM、损坏存储或原生错误，失败清理全部部分结果，诊断归接收者释放。
 */
static cc_result_t sqlite_load_messages(
    void *self,
    const char *session_id,
    int limit,
    cc_message_t **out_messages,
    size_t *out_count
)
{
    if (!self || !session_id || !out_messages || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid session history request");
    *out_messages = NULL;
    *out_count = 0;
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;

    cc_mutex_lock(store->mutex);
    if (store->transaction_failed) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_STORAGE, "SQLite transaction requires reopening");
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, session_id, role, text, content_parts, tool_calls, reasoning_content, tool_call_id, created_at, tool_outcome "
                      "FROM (SELECT *, rowid AS sequence FROM messages WHERE session_id = ?1 "
                      "ORDER BY created_at DESC, rowid DESC LIMIT ?2) "
                      "ORDER BY created_at ASC, sequence ASC";
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cc_result_t result = sqlite_finish_statement(store, stmt, rc);
        cc_mutex_unlock(store->mutex);
        return result;
    }

    rc = sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 2, limit > 0 ? limit : -1);
    if (rc != SQLITE_OK) {
        cc_result_t result = sqlite_finish_statement(store, stmt, rc);
        cc_mutex_unlock(store->mutex);
        return result;
    }

    size_t cap = 16;
    size_t count = 0;
    cc_message_t *messages = calloc(cap, sizeof(cc_message_t));
    if (!messages) {
        sqlite3_finalize(stmt);
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate messages");
    }

    cc_result_t result = cc_result_ok();
    int step_rc = SQLITE_OK;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count >= cap) {
            if (cap > SIZE_MAX / 2U / sizeof(cc_message_t)) {
                result = cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Too many session messages");
                break;
            }
            void *new_ptr = realloc(messages, cap * 2U * sizeof(cc_message_t));
            if (!new_ptr) { result = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow messages"); break; }
            messages = new_ptr;
            cap *= 2U;
        }

        cc_message_t *msg = &messages[count++];
        memset(msg, 0, sizeof(cc_message_t));

        for (int i = 0; i < 10; i++) {
            int type = sqlite3_column_type(stmt, i);
            if ((i <= 2 && type != SQLITE_TEXT) ||
                (i > 2 && type != SQLITE_NULL && type != SQLITE_TEXT)) {
                result = cc_result_error(CC_ERR_STORAGE, "Invalid stored message field type");
                break;
            }
            if (type == SQLITE_TEXT) {
                const unsigned char *text = sqlite3_column_text(stmt, i);
                int bytes = sqlite3_column_bytes(stmt, i);
                if (!text || bytes < 0 || strlen((const char *)text) != (size_t)bytes) {
                    result = cc_result_error(CC_ERR_STORAGE, "Stored message contains invalid text");
                    break;
                }
            }
        }
        if (result.code != CC_OK) break;

        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        const char *sid = (const char *)sqlite3_column_text(stmt, 1);
        const char *role_str = (const char *)sqlite3_column_text(stmt, 2);
        const char *cpj = (const char *)sqlite3_column_text(stmt, 4);
        const char *tcj = (const char *)sqlite3_column_text(stmt, 5);
        const char *reasoning = (const char *)sqlite3_column_text(stmt, 6);
        const char *tci = (const char *)sqlite3_column_text(stmt, 7);
        const char *created_at = (const char *)sqlite3_column_text(stmt, 8);
        if (!id || !id[0] || !sid || !sid[0] || !role_str || !role_str[0]) {
            result = cc_result_error(CC_ERR_STORAGE, "Invalid stored message identity");
            break;
        }

        msg->id = cc_copy_string(id);
        msg->session_id = cc_copy_string(sid);
        cc_content_parts_init(&msg->content);
        cc_tool_call_list_init(&msg->tool_calls);
        if (!msg->id || !msg->session_id) {
            result = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy stored message identity");
            break;
        }
        if (!cpj || !cpj[0] || !tcj || !tcj[0])
            result = cc_result_error(CC_ERR_STORAGE, "Stored message arrays are missing");
        if (result.code == CC_OK) {
            result = cc_storage_json_validate_array(cpj, 0);
            if (result.code == CC_OK) result = cc_content_parts_from_json(cpj, &msg->content);
        }
        if (result.code == CC_OK) {
            result = cc_storage_json_validate_array(tcj, 1);
            if (result.code == CC_OK) result = cc_tool_call_list_from_json(tcj, &msg->tool_calls);
        }
        msg->reasoning_content = (reasoning && reasoning[0]) ? cc_copy_string(reasoning) : NULL;
        msg->tool_call_id = (tci && tci[0]) ? cc_copy_string(tci) : NULL;
        msg->created_at = created_at ? cc_copy_string(created_at) : NULL;
        if (result.code == CC_OK && ((reasoning && reasoning[0] && !msg->reasoning_content) ||
                                     (tci && tci[0] && !msg->tool_call_id) ||
                                     (created_at && !msg->created_at)))
            result = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy stored message fields");

        if (strcmp(role_str, "system") == 0) msg->role = CC_ROLE_SYSTEM;
        else if (strcmp(role_str, "user") == 0) msg->role = CC_ROLE_USER;
        else if (strcmp(role_str, "assistant") == 0) msg->role = CC_ROLE_ASSISTANT;
        else if (strcmp(role_str, "tool") == 0) msg->role = CC_ROLE_TOOL;
        else if (result.code == CC_OK) result = cc_result_error(CC_ERR_STORAGE, "Invalid stored message role");

        const char *outcome_text = (const char *)sqlite3_column_text(stmt, 9);
        if (result.code == CC_OK && outcome_text && outcome_text[0])
            result = cc_message_tool_outcome_parse(outcome_text, &msg->tool_outcome);
        if (result.code == CC_OK && !cc_message_tool_outcome_valid(msg))
            result = cc_result_error(CC_ERR_STORAGE, "Stored tool outcome has invalid message fields");
        if (result.code != CC_OK) break;
    }
    if (result.code == CC_OK) result = sqlite_finish_statement(store, stmt, step_rc);
    else sqlite3_finalize(stmt);

    if (result.code != CC_OK) {
        for (size_t i = 0; i < count; i++) cc_message_cleanup(&messages[i]);
        free(messages);
    } else if (count) {
        *out_messages = messages;
        *out_count = count;
    } else {
        free(messages);
    }
    cc_mutex_unlock(store->mutex);
    return result;
}

/** @brief 在外层事务操作锁内插入工具调用审计。
 * @param[in,out] self 存活后端借用，外层持锁并开启事务。
 * @param[in] session_id 必填同步 NUL 会话键借用。
 * @param[in] call 必填工具调用借用，id/name/arguments_json 可空时沿既有空串存储语义。
 * @return 参数、时间、分配或原生首错，语句及时间副本本层消费，输入保持调用者拥有。
 */
static cc_result_t sqlite_append_tool_call(
    void *self,
    const char *session_id,
    const cc_tool_call_t *call
)
{
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;
    if (!session_id || !call)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid SQLite tool call");
    char *now = NULL;
    cc_result_t result = now_string(&now);
    if (result.code != CC_OK) return result;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO tool_calls (id, session_id, name, arguments_json, status, created_at) "
                      "VALUES (?1, ?2, ?3, ?4, 'completed', ?5)";
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, call->id ? call->id : "", -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, session_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, call->name ? call->name : "", -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 4, call->arguments_json ? call->arguments_json : "", -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 5, now, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    result = sqlite_finish_statement(store, stmt, rc);
    free(now);
    return result;
}

/** @brief 在外层事务锁内序列化媒体并插入属于指定会话的工具结果。
 * @param[in,out] self 存活后端借用，外层持锁并开启事务。
 * @param[in] session_id 必填同步 NUL 会话键借用，须与已有工具调用归属匹配。
 * @param[in] tool_call_id 必填同步 NUL 工具调用键借用。
 * @param[in] result 必填稳定结果借用，可空 text/error/metadata 按既有空串保存。
 * @return 参数、序列化、时间或原生错误，工具调用缺失/归属不符为 STORAGE。
 * @note 媒体 JSON 与时间副本本层释放，源结果及媒体始终由调用者拥有。
 */
static cc_result_t sqlite_append_tool_result(
    void *self,
    const char *session_id,
    const char *tool_call_id,
    const cc_tool_result_t *result
)
{
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;
    if (!session_id || !tool_call_id || !result)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid SQLite tool result");
    char *now = NULL;
    char *artifacts = NULL;
    cc_result_t operation = now_string(&now);
    if (operation.code == CC_OK)
        operation = cc_media_artifact_list_to_json(&result->artifacts, &artifacts);
    if (operation.code != CC_OK) {
        free(now);
        free(artifacts);
        return operation;
    }

    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO tool_results (id, tool_call_id, ok, text, error, metadata, artifacts, created_at) "
                      "SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8 "
                      "WHERE EXISTS (SELECT 1 FROM tool_calls WHERE id=?2 AND session_id=?9)";
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, tool_call_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, tool_call_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 3, result->ok ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 4, result->text ? result->text : "", -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 5, result->error ? result->error : "", -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 6, result->metadata ? result->metadata : "", -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 7, artifacts, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 8, now, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 9, session_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    int inserted = rc == SQLITE_DONE && sqlite3_changes(store->db) == 1;
    operation = sqlite_finish_statement(store, stmt, rc);
    free(artifacts);
    free(now);
    if (operation.code == CC_OK && !inserted)
        operation = cc_result_error(CC_ERR_STORAGE, "Tool call does not belong to this session");
    return operation;
}

/** @brief 操作锁内按 updated_at 倒序读取全部会话元数据。
 * @param[in] self 存活后端借用，事务故障锁存时拒绝读取。
 * @param[out] out_sessions 必填新拥有槽，参数有效后清空，成功交 session_free_array 回收。
 * @param[out] out_count 必填计数槽，参数有效后清零，空结果交付零和 NULL。
 * @return 参数、OOM、字段损坏或原生错误，部分结果和语句在本层清理。
 * @note name/workspace/model/timestamps 的 SQL NULL 保持 NULL，旧空状态沿 ACTIVE 兼容。
 */
static cc_result_t sqlite_list_sessions(
    void *self,
    cc_session_t **out_sessions,
    size_t *out_count
)
{
    if (!self || !out_sessions || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid SQLite session list request");
    *out_sessions = NULL;
    *out_count = 0;
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;

    cc_mutex_lock(store->mutex);
    if (store->transaction_failed) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_STORAGE, "SQLite transaction requires reopening");
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, name, workspace_dir, model, status, created_at, updated_at "
                      "FROM sessions ORDER BY updated_at DESC";
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cc_result_t result = sqlite_finish_statement(store, stmt, rc);
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

    cc_result_t result = cc_result_ok();
    int step_rc = SQLITE_OK;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count >= cap) {
            if (cap > SIZE_MAX / 2U / sizeof(cc_session_t)) {
                result = cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Too many SQLite sessions");
                break;
            }
            void *new_ptr = realloc(sessions, cap * 2U * sizeof(cc_session_t));
            if (!new_ptr) {
                result = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow sessions");
                break;
            }
            sessions = new_ptr;
            cap *= 2U;
        }

        cc_session_t *s = &sessions[count++];
        memset(s, 0, sizeof(cc_session_t));
        char **fields[] = {&s->id, &s->name, &s->workspace_dir, &s->model,
                           NULL, &s->created_at, &s->updated_at};
        for (int column = 0; column < 7; column++) {
            int type = sqlite3_column_type(stmt, column);
            if (type == SQLITE_NULL && column != 0) continue;
            const char *text = (const char *)sqlite3_column_text(stmt, column);
            int bytes = sqlite3_column_bytes(stmt, column);
            if (type != SQLITE_TEXT || !text || bytes < 0 || strlen(text) != (size_t)bytes) {
                result = cc_result_error(CC_ERR_STORAGE, "Invalid stored session text");
                break;
            }
            if (column == 4) {
                if (!text[0] || strcmp(text, "active") == 0) s->status = CC_SESSION_ACTIVE;
                else if (strcmp(text, "completed") == 0) s->status = CC_SESSION_COMPLETED;
                else if (strcmp(text, "error") == 0) s->status = CC_SESSION_ERROR;
                else result = cc_result_error(CC_ERR_STORAGE, "Invalid stored session status");
            } else {
                *fields[column] = cc_copy_string(text);
                if (!*fields[column])
                    result = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy SQLite session field");
            }
            if (result.code != CC_OK) break;
        }
        if (result.code != CC_OK) break;
    }
    if (result.code == CC_OK) result = sqlite_finish_statement(store, stmt, step_rc);
    else sqlite3_finalize(stmt);
    if (result.code != CC_OK || !count) {
        cc_session_free_array(sessions, count);
    } else {
        *out_sessions = sessions;
        *out_count = count;
    }
    cc_mutex_unlock(store->mutex);
    return result;
}

/** @brief 在已有操作锁及事务内执行单参数删除语句并消费语句。
 * @param[in,out] store 存活后端借用，外层负责事务结束。
 * @param[in] sql 必填同步 SQL 借用，只有第一个占位参数绑定 session_id。
 * @param[in] session_id 必填同步 NUL 过滤键借用。
 * @return prepare/bind/step/finalize 首错或成功，拥有诊断交接给调用者。
 */
static cc_result_t sqlite_exec_bound_delete(
    cc_sqlite_session_store_t *store,
    const char *sql,
    const char *session_id
)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    return sqlite_finish_statement(store, stmt, rc);
}

/** @brief 操作锁覆盖整次事务，按依赖顺序删除会话工具结果、调用与消息。
 * @param[in,out] self 存活后端借用，NULL 或已锁存事务失败时拒绝。
 * @param[in] session_id 必填同步 NUL 会话键借用，未命中沿成功空删除语义。
 * @return 原生事务/删除首错，提交失败尝试回滚，仍未退出事务则锁存拒绝后续方法。
 * @post 会话元数据保留，输入始终归调用者，返回诊断由接收者释放。
 */
static cc_result_t sqlite_clear_session(void *self, const char *session_id)
{
    if (!self || !session_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid session clear request");
    }
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;
    cc_mutex_lock(store->mutex);
    if (store->transaction_failed) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_STORAGE, "SQLite transaction requires reopening");
    }
    char *errmsg = NULL;
    int sql_rc = sqlite3_exec(store->db, "BEGIN IMMEDIATE", NULL, NULL, &errmsg);
    if (sql_rc != SQLITE_OK) {
        cc_result_t rc = cc_result_error(sql_rc == SQLITE_NOMEM ? CC_ERR_OUT_OF_MEMORY : CC_ERR_STORAGE,
                                         errmsg ? errmsg : sqlite3_errmsg(store->db));
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

    rc = sqlite_finish_transaction(store, rc);
    cc_mutex_unlock(store->mutex);
    return rc;
}

/** @brief 消费连接、操作锁及后端容器。
 * @param[in,out] self 可空独占实例，NULL 跳过，返回后旧借用失效。
 * @pre owner 已关闭入口并排空全部在途方法，普通方法锁不提供销毁并发准入。
 * @post 尽力 optimize，关闭连接后删除 mutex 并 free，关闭的原生事务按 SQLite 规则撤销。
 */
static void sqlite_destroy(void *self)
{
    cc_sqlite_session_store_t *store = (cc_sqlite_session_store_t *)self;
    if (!store) return;
    cc_mutex_lock(store->mutex);
    if (store->db) {
        if (!store->transaction_failed) {
            sqlite3_exec(store->db, "PRAGMA analysis_limit=0", NULL, NULL, NULL);
            sqlite3_exec(store->db, "PRAGMA optimize", NULL, NULL, NULL);
        }
        sqlite3_close_v2(store->db);
    }
    cc_mutex_unlock(store->mutex);
    cc_mutex_destroy(store->mutex);
    free(store);
}

/** @brief 操作锁覆盖整批 BEGIN、记录序列化/写入及 COMMIT/ROLLBACK。
 * @param[in,out] self 存活后端借用，NULL 或事务失败锁存时拒绝。
 * @param[in] records count 项稳定同步借用，零项时可 NULL，内容按 type 选择。
 * @param[in] count 真实记录数，健康连接的零项立即成功。
 * @return 参数、序列化、分配或原生首错，失败回滚整批，事务无法退出时要求重开。
 * @note 同一实例的读写方法无法观察批次中间态，外部连接并发由原生事务排序。
 */
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

    cc_mutex_lock(store->mutex);
    if (store->transaction_failed) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_STORAGE, "SQLite transaction requires reopening");
    }
    if (!count) {
        cc_mutex_unlock(store->mutex);
        return cc_result_ok();
    }
    int sql_rc = sqlite3_exec(store->db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (sql_rc != SQLITE_OK) {
        cc_result_t result = cc_result_error(sql_rc == SQLITE_NOMEM ? CC_ERR_OUT_OF_MEMORY : CC_ERR_STORAGE,
                                             sqlite3_errmsg(store->db));
        cc_mutex_unlock(store->mutex);
        return result;
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

    rc = sqlite_finish_transaction(store, rc);
    cc_mutex_unlock(store->mutex);
    return rc;
}

/* SQLite session store vtable，将数据库实现绑定到 cc_session_store_t 端口。 */
static cc_session_store_vtable_t sqlite_vtable = {
    .create_session = sqlite_create_session,
    .append_records = sqlite_append_records,
    .load_messages = sqlite_load_messages,
    .list_sessions = sqlite_list_sessions,
    .clear_session = sqlite_clear_session,
    .destroy = sqlite_destroy
};

/**
 * @brief 分配状态与操作 mutex，复制路径后打开 SQLite 并初始化表结构。
 * @param[in] db_path 同步借用路径，NULL 使用编译默认，空串沿 SQLite 临时数据库语义。
 * @param[out] out_store 必填新拥有槽，先清零，全部初始化成功后交付实例和方法表。
 * @return 成功为 CC_OK，参数、OOM、mutex 或数据库错误返回对应结果，调用者释放诊断。
 * @note 失败归还本次状态、锁、路径及已打开连接，路径复制 OOM 在调用 SQLite 前退出。
 * 父目录由外层准备，成功实例由 owner 关闭入口并排空方法后 destroy。
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

    char *resolved_path = build_db_path(db_path);
    if (!resolved_path) {
        cc_mutex_destroy(self->mutex);
        free(self);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy SQLite database path");
    }

    int rc = sqlite3_open_v2(resolved_path, &self->db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        NULL);
    free(resolved_path);

    if (rc != SQLITE_OK) {
        const char *err = sqlite3_errmsg(self->db);
        cc_result_t result = cc_result_error(rc == SQLITE_NOMEM ? CC_ERR_OUT_OF_MEMORY : CC_ERR_STORAGE,
                                             err ? err : "Cannot open database");
        if (self->db) sqlite3_close(self->db);
        cc_mutex_destroy(self->mutex);
        free(self);
        return result;
    }

    cc_result_t pragma_result = setup_pragmas(self->db, 1);
    if (pragma_result.code != CC_OK) {
        sqlite3_close(self->db);
        cc_mutex_destroy(self->mutex);
        free(self);
        return pragma_result;
    }

    char *err = NULL;
    rc = sqlite3_exec(self->db, init_sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        cc_result_t result = cc_result_error(rc == SQLITE_NOMEM ? CC_ERR_OUT_OF_MEMORY : CC_ERR_STORAGE,
                                             err ? err : "Failed to init tables");
        sqlite3_free(err);
        sqlite3_close(self->db);
        cc_mutex_destroy(self->mutex);
        free(self);
        return result;
    }

    out_store->self = self;
    out_store->vtable = &sqlite_vtable;
    return cc_result_ok();
}
