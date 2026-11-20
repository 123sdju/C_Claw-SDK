#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_thread.h"
#include "cc/internal/cc_alloc.h"
#include "cJSON.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fail_prepare, fail_bind, fail_step, fail_commit, fail_rollback, fail_time;
static int fail_copy, fail_growth, force_inplace;
static void *session_array;
static size_t json_allocations, json_failure;
static sqlite3_mem_methods native_memory;
static size_t native_allocations, native_failure;
static int count_native;
static cc_mutex_t gate_mutex, observed_store_mutex;
static cc_cond_t gate_condition;
static int pause_first, first_insert_ready, reader_attempted, reader_done, insert_count;
static _Thread_local int writer_context, reader_context;

/** @brief 经 SQLite 公共 allocator 配置注入真实原生堆申请失败。
 * @param[in] bytes 原生要求的正字节数。
 * @return 单次故障为 NULL，其他交给原生 allocator，释放继续沿原生 xFree。
 * @note 计数窗口只在单线程测试阶段开启，SQLite 自身 allocator 同步规则保持。
 */
static void *native_allocate(int bytes)
{
    if (count_native && ++native_allocations == native_failure) return NULL;
    return native_memory.xMalloc(bytes);
}

/** @brief 经 SQLite 公共 allocator 配置注入原生扩容失败。
 * @param[in,out] pointer 原生拥有块，失败时保持有效。
 * @param[in] bytes 原生要求的新正字节数。
 * @return 单次故障 NULL 或原生 realloc 结果，成功拥有关系继续留给 SQLite。
 */
static void *native_resize(void *pointer, int bytes)
{
    if (count_native && ++native_allocations == native_failure) return NULL;
    return native_memory.xRealloc(pointer, bytes);
}

/** @brief 注入单次 prepare OOM，其他调用交给真实 SQLite。
 * @param[in] db 存活连接借用。
 * @param[in] sql SQL 文本借用。
 * @param[in] bytes SQL 长度，沿原生负数语义。
 * @param[out] statement 原生语句拥有槽，失败置 NULL。
 * @param[out] tail 可空 SQL 剩余位置借用槽。
 * @return SQLITE_NOMEM 或真实原生结果，成功语句由后端 finalize。
 */
static int fault_prepare(sqlite3 *db, const char *sql, int bytes,
                         sqlite3_stmt **statement, const char **tail)
{
    if (fail_prepare) {
        fail_prepare = 0;
        *statement = NULL;
        return SQLITE_NOMEM;
    }
    return sqlite3_prepare_v2(db, sql, bytes, statement, tail);
}

/** @brief 在一次文本绑定处注入 OOM。
 * @param[in,out] statement 原生语句借用。
 * @param[in] index 占位参数编号。
 * @param[in] text 同步文本借用，复制规则沿 destructor。
 * @param[in] bytes 原生长度。
 * @param[in] destructor 原生文本清理策略。
 * @return 注入 NOMEM 或真实绑定结果，语句继续归后端拥有。
 */
static int fault_bind(sqlite3_stmt *statement, int index, const char *text, int bytes,
                      void (*destructor)(void *))
{
    if (fail_bind) {
        fail_bind = 0;
        return SQLITE_NOMEM;
    }
    return sqlite3_bind_text(statement, index, text, bytes, destructor);
}

/** @brief 执行真实 step，并在首条写入后建立确定性读者交错。
 * @param[in,out] statement 后端拥有的语句借用。
 * @return 单次注入 IOERR 或真实结果，首条写入在等待读者尝试锁后继续。
 * @note 调度谓词由独立 gate_mutex 保护，等待时归还该锁，生产操作锁保持原语义。
 */
static int fault_step(sqlite3_stmt *statement)
{
    if (fail_step) {
        fail_step = 0;
        return SQLITE_IOERR;
    }
    int rc = sqlite3_step(statement);
    const char *sql = sqlite3_sql(statement);
    if (writer_context && pause_first && rc == SQLITE_DONE && sql &&
        strncmp(sql, "INSERT INTO messages", 20) == 0) {
        cc_mutex_lock(gate_mutex);
        insert_count++;
        if (insert_count == 1) {
            first_insert_ready = 1;
            cc_cond_broadcast(gate_condition);
            while (!reader_attempted) cc_cond_wait(gate_condition, gate_mutex);
        }
        cc_mutex_unlock(gate_mutex);
    }
    return rc;
}

/** @brief 只在 COMMIT/ROLLBACK 边界注入原生错误，其余 SQL 使用真实 SQLite。
 * @param[in,out] db 存活连接借用。
 * @param[in] sql 同步 SQL 文本借用。
 * @param[in] callback 可空原生回调。
 * @param[in,out] data 回调上下文借用。
 * @param[out] error 可空原生错误拥有槽，由调用者 sqlite3_free。
 * @return 注入 IOERR 或真实原生结果，故障路径保留事务供恢复状态检查。
 */
static int fault_exec(sqlite3 *db, const char *sql,
                      int (*callback)(void *, int, char **, char **), void *data, char **error)
{
    if ((fail_commit && strcmp(sql, "COMMIT") == 0) ||
        (fail_rollback && strcmp(sql, "ROLLBACK") == 0)) return SQLITE_IOERR;
    return sqlite3_exec(db, sql, callback, data, error);
}

/** @brief 捕获时钟失败，避免测试读取未初始化日历状态。
 * @param[out] output 可空 time_t 值槽。
 * @return 故障为负一，其他为真实时间，无拥有资源。
 */
static time_t fault_time(time_t *output)
{
    if (fail_time) return (time_t)-1;
    return time(output);
}

/** @brief 在后端文本复制序列中注入单次 OOM。
 * @param[in] text 同步可空文本借用。
 * @return 独占副本或 NULL，成功交给后端及其输出对象释放。
 */
static char *fault_copy(const char *text)
{
    if (fail_copy && --fail_copy == 0) return NULL;
    return cc_copy_string(text);
}

/** @brief 为会话数组扩容测试预留可原地扩展的真实分配。
 * @param[in] count 元素数。
 * @param[in] bytes 单项字节数。
 * @return 独占清零块或 NULL，最终由正常 free 消费。
 */
static void *fault_calloc(size_t count, size_t bytes)
{
    if (force_inplace && count == 16 && bytes == sizeof(cc_session_t)) {
        session_array = calloc(64, bytes);
        return session_array;
    }
    return calloc(count, bytes);
}

/** @brief 注入扩容失败或真实预留块上的原地成功。
 * @param[in,out] pointer 原拥有块，失败继续有效。
 * @param[in] bytes 新字节容量。
 * @return 新拥有块、原地成功块或失败 NULL，正常路径沿 realloc。
 */
static void *fault_realloc(void *pointer, size_t bytes)
{
    if (fail_growth) return NULL;
    if (force_inplace && pointer == session_array && bytes <= 64 * sizeof(cc_session_t))
        return pointer;
    return realloc(pointer, bytes);
}

/** @brief 在读者获取生产锁前发布尝试谓词。
 * @param[in,out] mutex 存活锁借用，原生取得成功后由生产路径配对 unlock。
 * @note gate 仅表达测试调度，其他线程及锁调用原端口。
 */
static void fault_lock(cc_mutex_t mutex)
{
    if (reader_context && mutex == observed_store_mutex) {
        cc_mutex_lock(gate_mutex);
        reader_attempted = 1;
        cc_cond_broadcast(gate_condition);
        cc_mutex_unlock(gate_mutex);
    }
    cc_mutex_lock(mutex);
}

/** @brief 归还生产锁，旧逐条解锁实现会被确定性读者抢先采样。
 * @param[in,out] mutex 当前任务持有的锁借用。
 * @note 整批持锁实现的首次归还发生在两条写入后，直接继续正常清理。
 */
static void fault_unlock(cc_mutex_t mutex)
{
    cc_mutex_unlock(mutex);
    if (writer_context && pause_first && mutex == observed_store_mutex && insert_count == 1) {
        cc_mutex_lock(gate_mutex);
        while (!reader_done) cc_cond_wait(gate_condition, gate_mutex);
        cc_mutex_unlock(gate_mutex);
    }
}

/* 当前后端编译成测试私有副本，SQLite、JSON、线程和结果继续链接真实 SDK。 */
#define sqlite3_prepare_v2 fault_prepare
#define sqlite3_bind_text fault_bind
#define sqlite3_step fault_step
#define sqlite3_exec fault_exec
#define time fault_time
#define cc_copy_string fault_copy
#define calloc fault_calloc
#define realloc fault_realloc
#define cc_mutex_lock fault_lock
#define cc_mutex_unlock fault_unlock
#define cc_sqlite_session_store_create sqlite_test_create_store
#include "../../adapters/src/storage/cc_sqlite_session_store.c"
#undef cc_sqlite_session_store_create
#undef cc_mutex_unlock
#undef cc_mutex_lock
#undef realloc
#undef calloc
#undef cc_copy_string
#undef time
#undef sqlite3_exec
#undef sqlite3_step
#undef sqlite3_bind_text
#undef sqlite3_prepare_v2

/** @brief 检查条件一次，失败报告行号并返回测试失败。 @param[in] value 条件表达式。 */
#define CHECK(value) do { if (!(value)) { fprintf(stderr, "SQLite check failed at %d\n", __LINE__); return 1; } } while (0)

/** @brief 消费结果并比较错误码。
 * @param[in] result 按值移入的拥有结果，诊断在本层释放。
 * @param[in] expected 期望码。
 * @return 匹配为一，不匹配输出诊断并返回零。
 */
static int result_is(cc_result_t result, cc_error_code_t expected)
{
    int matches = result.code == expected;
    if (!matches) fprintf(stderr, "SQLite result %d expected %d: %s\n",
                          result.code, expected, result.message ? result.message : "");
    cc_result_free(&result);
    return matches;
}

/** @brief 使用原生语句读取单个计数，用于独立核对数据库实际状态。
 * @param[in] db 存活连接借用，fixture 当前独占。
 * @param[in] table 固定测试表名借用。
 * @return 记录数，prepare/step 失败为负一，语句总会 finalize。
 */
static int row_count(sqlite3 *db, const char *table)
{
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s", table);
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) return -1;
    int count = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
    sqlite3_finalize(statement);
    return count;
}

/** @brief 构造包含 reasoning、显式时间与 UNKNOWN 工具语义的完整消息。
 * @param[in] id 本次消息 ID 同步借用。
 * @return 独占消息或 NULL，接收者 destroy，部分构造错误已消费并清理。
 */
static cc_message_t *test_message(const char *id)
{
    cc_message_t *message = NULL;
    if (!result_is(cc_message_create_text(id, "session", CC_ROLE_TOOL, "body", "call", &message), CC_OK))
        return NULL;
    if (!result_is(cc_message_set_reasoning_content(message, "reasoning"), CC_OK)) {
        cc_message_destroy(message);
        return NULL;
    }
    message->created_at = cc_copy_string("2026-11-15T00:00:00");
    message->tool_outcome = CC_MESSAGE_TOOL_OUTCOME_UNKNOWN;
    if (!message->created_at) {
        cc_message_destroy(message);
        return NULL;
    }
    return message;
}

/** @brief 对一次批次转发提供共享端口与记录借用，覆盖 join 完成。 */
typedef struct concurrent_context {
    cc_session_store_t *store;
    const cc_session_record_t *records;
    int failed;
} concurrent_context_t;

/** @brief 写者在线程中追加两条记录。
 * @param[in,out] arg 独占结果字段及借用输入，上层保管至 join。
 * @return NULL，failed 由 join 后主线程读取，拥有诊断本层消费。
 */
static void *write_batch(void *arg)
{
    concurrent_context_t *context = arg;
    writer_context = 1;
    context->failed = !result_is(context->store->vtable->append_records(
        context->store->self, context->records, 2), CC_OK);
    writer_context = 0;
    return NULL;
}

/** @brief 读者在首条原生 INSERT 后尝试读取，要求只能观察完整两条批次。
 * @param[in,out] arg 独占结果字段和共享端口借用，上层保管至 join。
 * @return NULL，输出数组及诊断本层消费，结束发布 reader_done 唤醒旧实现写者。
 */
static void *read_batch(void *arg)
{
    concurrent_context_t *context = arg;
    reader_context = 1;
    cc_message_t *messages = NULL;
    size_t count = 0;
    context->failed = !result_is(context->store->vtable->load_messages(
        context->store->self, "session", 100, &messages, &count), CC_OK) || count != 2;
    for (size_t i = 0; i < count; i++) cc_message_cleanup(&messages[i]);
    free(messages);
    reader_context = 0;
    cc_mutex_lock(gate_mutex);
    reader_done = 1;
    cc_cond_broadcast(gate_condition);
    cc_mutex_unlock(gate_mutex);
    return NULL;
}

/** @brief 使用真实线程与条件变量核对整批持锁，避免读到未提交前缀。
 * @param[in,out] store 当前 fixture 独占端口，调用期间共享给两个 worker。
 * @return 所有线程、记录数及资源断言通过为零，否则为一。
 * @note 消息覆盖两个 join，之后才销毁，gate 原语按创建/销毁配对。
 */
static int test_concurrent_batch(cc_session_store_t *store)
{
    CHECK(result_is(store->vtable->clear_session(store->self, "session"), CC_OK));
    CHECK(result_is(cc_mutex_create(&gate_mutex), CC_OK));
    CHECK(result_is(cc_cond_create(&gate_condition), CC_OK));
    observed_store_mutex = ((cc_sqlite_session_store_t *)store->self)->mutex;
    cc_message_t *first = test_message("concurrent-1");
    cc_message_t *second = test_message("concurrent-2");
    CHECK(first && second);
    cc_session_record_t records[] = {
        {.type = CC_SESSION_RECORD_MESSAGE, .data.message = first},
        {.type = CC_SESSION_RECORD_MESSAGE, .data.message = second}
    };
    concurrent_context_t writer = {store, records, 0}, reader = {store, NULL, 0};
    cc_thread_t writer_thread = NULL, reader_thread = NULL;
    pause_first = 1;
    CHECK(result_is(cc_thread_create(write_batch, &writer, &writer_thread), CC_OK));
    cc_mutex_lock(gate_mutex);
    while (!first_insert_ready) cc_cond_wait(gate_condition, gate_mutex);
    cc_mutex_unlock(gate_mutex);
    CHECK(result_is(cc_thread_create(read_batch, &reader, &reader_thread), CC_OK));
    CHECK(result_is(cc_thread_join(writer_thread), CC_OK));
    CHECK(result_is(cc_thread_join(reader_thread), CC_OK));
    pause_first = 0;
    CHECK(!writer.failed && !reader.failed);
    cc_message_destroy(first);
    cc_message_destroy(second);
    cc_cond_destroy(gate_condition);
    cc_mutex_destroy(gate_mutex);
    return 0;
}

/** @brief 在指定真实 cJSON 申请处返回 NULL。
 * @param[in] bytes 字节数。
 * @return 独占堆块或故障 NULL，cJSON 使用配对 free。
 */
static void *json_allocate(size_t bytes)
{
    json_allocations++;
    return json_failure && json_allocations == json_failure ? NULL : malloc(bytes);
}

/** @brief 独立解码已提交批次，核对非空消息分片、工具调用与媒体结果完整性。
 * @param[in] db 单线程独占的真实连接借用，故障 hooks 已关闭。
 * @return 完整字段符合 fixture 输入为一，否则为零，语句及全部解析副本均在本层消费。
 */
static int complete_json_batch(sqlite3 *db)
{
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(db, "SELECT content_parts,tool_calls FROM messages", -1,
                           &statement, NULL) != SQLITE_OK) return 0;
    cc_content_parts_t parts = {0};
    cc_tool_call_list_t calls = {0};
    int valid = sqlite3_step(statement) == SQLITE_ROW;
    if (valid) {
        valid = result_is(cc_content_parts_from_json(
            (const char *)sqlite3_column_text(statement, 0), &parts), CC_OK) &&
            result_is(cc_tool_call_list_from_json(
            (const char *)sqlite3_column_text(statement, 1), &calls), CC_OK);
    }
    valid = valid && parts.count == 1 && parts.items[0].text &&
        strcmp(parts.items[0].text, "body") == 0 && calls.count == 1 &&
        calls.items[0].id && strcmp(calls.items[0].id, "call") == 0 &&
        calls.items[0].name && strcmp(calls.items[0].name, "tool") == 0 &&
        calls.items[0].arguments_json && strcmp(calls.items[0].arguments_json, "{}") == 0;
    cc_content_parts_cleanup(&parts);
    cc_tool_call_list_cleanup(&calls);
    sqlite3_finalize(statement);
    if (!valid) return 0;
    statement = NULL;
    if (sqlite3_prepare_v2(db, "SELECT artifacts,text,error,metadata FROM tool_results", -1,
                           &statement, NULL) != SQLITE_OK) return 0;
    cc_media_artifact_list_t artifacts = {0};
    valid = sqlite3_step(statement) == SQLITE_ROW;
    if (valid) {
        valid = result_is(cc_media_artifact_list_from_json(
            (const char *)sqlite3_column_text(statement, 0), &artifacts), CC_OK);
        const char *text = (const char *)sqlite3_column_text(statement, 1);
        const char *error = (const char *)sqlite3_column_text(statement, 2);
        const char *metadata = (const char *)sqlite3_column_text(statement, 3);
        valid = valid && text && strcmp(text, "result") == 0 &&
            error && strcmp(error, "error") == 0 && metadata && strcmp(metadata, "metadata") == 0;
    }
    valid = valid && artifacts.count == 1 && artifacts.items[0].id &&
        strcmp(artifacts.items[0].id, "image") == 0 && artifacts.items[0].kind == CC_MEDIA_IMAGE &&
        artifacts.items[0].mime && strcmp(artifacts.items[0].mime, "image/png") == 0 &&
        artifacts.items[0].bytes == 3;
    cc_media_artifact_list_cleanup(&artifacts);
    sqlite3_finalize(statement);
    return valid;
}

/** @brief 穷举整批消息、调用及工具结果中的 JSON 申请失败，验证全有或全无。
 * @param[in,out] store 独占真实 SQLite 端口借用。
 * @return 完整持久化或完全回滚且无残语句时为零，其他为一。
 * @note cJSON 故障只在同步批次调用期间生效，输入构造和验证在故障窗口外执行。
 */
static int test_json_failures(cc_session_store_t *store)
{
    cc_sqlite_session_store_t *backend = store->self;
    CHECK(result_is(store->vtable->clear_session(store->self, "session"), CC_OK));
    cc_message_t *message = test_message("json-message");
    CHECK(message);
    cc_tool_call_t call = {.id = "call", .name = "tool", .arguments_json = "{}"};
    CHECK(result_is(cc_message_add_tool_call(message, &call), CC_OK));
    cc_tool_result_t tool = {.ok = 1};
    tool.text = cc_copy_string("result");
    tool.error = cc_copy_string("error");
    tool.metadata = cc_copy_string("metadata");
    CHECK(tool.text && tool.error && tool.metadata);
    cc_media_artifact_t artifact = {.id = "image", .kind = CC_MEDIA_IMAGE, .mime = "image/png", .bytes = 3};
    CHECK(result_is(cc_tool_result_add_artifact(&tool, &artifact), CC_OK));
    cc_session_record_t records[] = {
        {.type = CC_SESSION_RECORD_MESSAGE, .session_id = "session", .data.message = message},
        {.type = CC_SESSION_RECORD_TOOL_CALL, .session_id = "session", .data.tool_call = &call},
        {.type = CC_SESSION_RECORD_TOOL_RESULT, .session_id = "session",
         .data.tool_result = {.tool_call_id = "call", .result = &tool}}
    };
    cJSON_Hooks hooks = {.malloc_fn = json_allocate, .free_fn = free};
    cJSON_InitHooks(&hooks);
    json_allocations = 0;
    CHECK(result_is(store->vtable->append_records(store->self, records, 3), CC_OK));
    size_t allocations = json_allocations;
    cJSON_InitHooks(NULL);
    for (size_t point = 1; point <= allocations; point++) {
        CHECK(result_is(store->vtable->clear_session(store->self, "session"), CC_OK));
        cJSON_InitHooks(&hooks);
        json_allocations = 0;
        json_failure = point;
        cc_result_t result = store->vtable->append_records(store->self, records, 3);
        json_failure = 0;
        cJSON_InitHooks(NULL);
        int rows = result.code == CC_OK ? 1 : 0;
        CHECK(result.code == CC_OK || result.code == CC_ERR_OUT_OF_MEMORY);
        cc_result_free(&result);
        CHECK(row_count(backend->db, "messages") == rows);
        CHECK(row_count(backend->db, "tool_calls") == rows);
        CHECK(row_count(backend->db, "tool_results") == rows);
        if (rows) CHECK(complete_json_batch(backend->db));
        CHECK(sqlite3_get_autocommit(backend->db) && !sqlite3_next_stmt(backend->db, NULL));
    }
    printf("SQLite batch checked %zu JSON allocation points\n", allocations);
    cc_tool_result_cleanup(&tool);
    cc_message_destroy(message);
    return 0;
}

/** @brief 穷举一次真实 SQLite 追加的 xMalloc/xRealloc 点并验证原子性和可继续使用。
 * @param[in,out] store 单线程独占的真实连接端口借用。
 * @return 每点完整成功或 OOM 后零记录、事务退出且无残语句为零，其他为一。
 * @note SQLite 可以原生回收缓存并重试，成功分支仍需读取完整消息；输入始终在故障窗口外拥有。
 */
static int test_native_allocations(cc_session_store_t *store)
{
    cc_sqlite_session_store_t *backend = store->self;
    CHECK(result_is(store->vtable->clear_session(store->self, "session"), CC_OK));
    cc_message_t *message = test_message("native-oom");
    CHECK(message);
    cc_session_record_t record = {.type = CC_SESSION_RECORD_MESSAGE, .data.message = message};
    native_allocations = native_failure = 0;
    count_native = 1;
    cc_result_t result = store->vtable->append_records(store->self, &record, 1);
    count_native = 0;
    CHECK(result_is(result, CC_OK));
    size_t allocations = native_allocations;
    CHECK(allocations);
    for (size_t point = 1; point <= allocations; point++) {
        CHECK(result_is(store->vtable->clear_session(store->self, "session"), CC_OK));
        native_allocations = 0;
        native_failure = point;
        count_native = 1;
        result = store->vtable->append_records(store->self, &record, 1);
        count_native = 0;
        native_failure = 0;
        int rows = result.code == CC_OK ? 1 : 0;
        CHECK(result.code == CC_OK || result.code == CC_ERR_OUT_OF_MEMORY);
        cc_result_free(&result);
        CHECK(row_count(backend->db, "messages") == rows);
        CHECK(sqlite3_get_autocommit(backend->db) && !sqlite3_next_stmt(backend->db, NULL));
        cc_message_t *messages = NULL;
        size_t count = 0;
        CHECK(result_is(store->vtable->load_messages(store->self, "session", 0, &messages, &count), CC_OK));
        CHECK(count == (size_t)rows);
        if (count) {
            CHECK(strcmp(messages[0].id, "native-oom") == 0);
            CHECK(messages[0].reasoning_content && strcmp(messages[0].reasoning_content, "reasoning") == 0);
            cc_message_cleanup(&messages[0]);
        }
        free(messages);
    }
    printf("SQLite append checked %zu native allocation points\n", allocations);
    cc_message_destroy(message);
    return 0;
}

/** @brief 核对会话增长、完整元数据、负/零 limit、NULL 输出及原生错误传播。
 * @param[in,out] store 独占真实 SQLite 端口借用。
 * @return 所有字段、拥有输出和错误码符合契约为零，其他为一。
 */
static int test_metadata_and_errors(cc_session_store_t *store)
{
    cc_sqlite_session_store_t *backend = store->self;
    for (unsigned i = 0; i < 20; i++) {
        char id[32];
        snprintf(id, sizeof(id), "extra-%u", i);
        CHECK(result_is(store->vtable->create_session(store->self, id, "."), CC_OK));
    }
    CHECK(sqlite3_exec(backend->db, "UPDATE sessions SET name='name',model='model',status='completed',"
        "created_at='created',updated_at='updated' WHERE id='session'", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_exec(backend->db, "UPDATE sessions SET status='error',created_at=NULL,updated_at=NULL "
        "WHERE id='extra-0'", NULL, NULL, NULL) == SQLITE_OK);
    cc_session_t *sessions = NULL;
    size_t count = 0;
    force_inplace = 1;
    CHECK(result_is(store->vtable->list_sessions(store->self, &sessions, &count), CC_OK));
    force_inplace = 0;
    CHECK(count == 21);
    int found = 0, found_nullable = 0;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(sessions[i].id, "extra-0") == 0) {
            CHECK(sessions[i].status == CC_SESSION_ERROR);
            CHECK(!sessions[i].name && !sessions[i].model &&
                  !sessions[i].created_at && !sessions[i].updated_at);
            found_nullable = 1;
        }
        if (strcmp(sessions[i].id, "session") != 0) continue;
        CHECK(sessions[i].status == CC_SESSION_COMPLETED);
        CHECK(sessions[i].name && strcmp(sessions[i].name, "name") == 0);
        CHECK(sessions[i].model && strcmp(sessions[i].model, "model") == 0);
        CHECK(sessions[i].created_at && strcmp(sessions[i].created_at, "created") == 0);
        CHECK(sessions[i].updated_at && strcmp(sessions[i].updated_at, "updated") == 0);
        found = 1;
    }
    CHECK(found && found_nullable);
    cc_session_free_array(sessions, count);
    sessions = NULL;
    fail_growth = 1;
    CHECK(result_is(store->vtable->list_sessions(store->self, &sessions, &count), CC_ERR_OUT_OF_MEMORY));
    fail_growth = 0;
    CHECK(!sessions && !count);
    fail_copy = 2;
    CHECK(result_is(store->vtable->list_sessions(store->self, &sessions, &count), CC_ERR_OUT_OF_MEMORY));
    CHECK(!sessions && !count);
    fail_step = 1;
    CHECK(result_is(store->vtable->list_sessions(store->self, &sessions, &count), CC_ERR_STORAGE));
    CHECK(!sessions && !count);
    CHECK(sqlite3_exec(backend->db, "UPDATE sessions SET status='invalid' WHERE id='extra-0'",
                       NULL, NULL, NULL) == SQLITE_OK);
    CHECK(result_is(store->vtable->list_sessions(store->self, &sessions, &count), CC_ERR_STORAGE));
    CHECK(!sessions && !count);
    CHECK(sqlite3_exec(backend->db, "UPDATE sessions SET status='error',name=CAST(X'610062' AS TEXT) "
        "WHERE id='extra-0'", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(result_is(store->vtable->list_sessions(store->self, &sessions, &count), CC_ERR_STORAGE));
    CHECK(!sessions && !count);
    CHECK(sqlite3_exec(backend->db, "UPDATE sessions SET name=NULL WHERE id='extra-0'",
                       NULL, NULL, NULL) == SQLITE_OK);
    fail_prepare = 1;
    CHECK(result_is(store->vtable->create_session(store->self, "prepare-fail", "."), CC_ERR_OUT_OF_MEMORY));
    fail_bind = 1;
    CHECK(result_is(store->vtable->create_session(store->self, "bind-fail", "."), CC_ERR_OUT_OF_MEMORY));
    fail_time = 1;
    CHECK(result_is(store->vtable->create_session(store->self, "time-fail", "."), CC_ERR_STORAGE));
    fail_time = 0;
    fail_copy = 1;
    CHECK(result_is(store->vtable->create_session(store->self, "copy-fail", "."), CC_ERR_OUT_OF_MEMORY));
    CHECK(!sqlite3_next_stmt(backend->db, NULL));
    CHECK(result_is(store->vtable->create_session(NULL, "null", "."), CC_ERR_INVALID_ARGUMENT));
    CHECK(result_is(store->vtable->create_session(store->self, NULL, "."), CC_ERR_INVALID_ARGUMENT));
    CHECK(result_is(store->vtable->list_sessions(store->self, NULL, &count), CC_ERR_INVALID_ARGUMENT));
    CHECK(result_is(store->vtable->clear_session(store->self, "session"), CC_OK));
    cc_message_t *message = test_message("limit-message");
    CHECK(message);
    cc_session_record_t record = {.type = CC_SESSION_RECORD_MESSAGE, .data.message = message};
    CHECK(result_is(store->vtable->append_records(store->self, &record, 1), CC_OK));
    for (int limit = -1; limit <= 1; limit++) {
        cc_message_t *messages = NULL;
        CHECK(result_is(store->vtable->load_messages(store->self, "session", limit, &messages, &count), CC_OK));
        CHECK(count == 1 && messages[0].tool_outcome == CC_MESSAGE_TOOL_OUTCOME_UNKNOWN);
        CHECK(messages[0].created_at && strcmp(messages[0].created_at, message->created_at) == 0);
        CHECK(messages[0].reasoning_content && strcmp(messages[0].reasoning_content, "reasoning") == 0);
        cc_message_cleanup(&messages[0]);
        free(messages);
    }
    cc_message_t *empty = NULL;
    CHECK(result_is(store->vtable->load_messages(store->self, "missing", 0, &empty, &count), CC_OK));
    CHECK(!empty && !count);
    fail_step = 1;
    CHECK(result_is(store->vtable->load_messages(store->self, "session", 1, &empty, &count), CC_ERR_STORAGE));
    CHECK(!empty && !count);
    CHECK(sqlite3_exec(backend->db, "UPDATE messages SET created_at=NULL,reasoning_content=NULL,"
        "tool_outcome=NULL,tool_call_id=NULL", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(result_is(store->vtable->load_messages(store->self, "session", 0, &empty, &count), CC_OK));
    CHECK(count == 1 && !empty[0].created_at && !empty[0].reasoning_content &&
          !empty[0].tool_call_id && empty[0].tool_outcome == CC_MESSAGE_TOOL_OUTCOME_RECORDED);
    cc_message_cleanup(&empty[0]);
    free(empty);
    cc_message_destroy(message);
    return 0;
}

/** @brief 使用真实延迟外键制造 COMMIT 失败，并验证清除提交及无法回滚时的锁存。
 * @param[in,out] store 独占真实 SQLite 端口借用，最后故意保留失败事务供 destroy 关闭。
 * @return 失败后状态、重试及拒绝后续方法均正确为零，否则为一。
 */
static int test_transactions(cc_session_store_t *store)
{
    cc_sqlite_session_store_t *backend = store->self;
    CHECK(result_is(store->vtable->clear_session(store->self, "session"), CC_OK));
    cc_session_record_t invalid = {.type = CC_SESSION_RECORD_MESSAGE, .data.message = NULL};
    CHECK(result_is(store->vtable->append_records(store->self, &invalid, 1), CC_ERR_INVALID_ARGUMENT));
    invalid.type = CC_SESSION_RECORD_TOOL_CALL;
    invalid.session_id = "session";
    invalid.data.tool_call = NULL;
    CHECK(result_is(store->vtable->append_records(store->self, &invalid, 1), CC_ERR_INVALID_ARGUMENT));
    invalid.type = CC_SESSION_RECORD_TOOL_RESULT;
    invalid.data.tool_result.tool_call_id = "missing";
    invalid.data.tool_result.result = NULL;
    CHECK(result_is(store->vtable->append_records(store->self, &invalid, 1), CC_ERR_INVALID_ARGUMENT));
    CHECK(sqlite3_exec(backend->db, "PRAGMA defer_foreign_keys=ON", NULL, NULL, NULL) == SQLITE_OK);
    cc_tool_call_t missing = {.id = "missing-call", .name = "tool", .arguments_json = "{}"};
    cc_session_record_t call = {.type = CC_SESSION_RECORD_TOOL_CALL, .session_id = "missing-session", .data.tool_call = &missing};
    CHECK(result_is(store->vtable->append_records(store->self, &call, 1), CC_ERR_STORAGE));
    CHECK(sqlite3_get_autocommit(backend->db) && row_count(backend->db, "tool_calls") == 0);
    CHECK(result_is(store->vtable->create_session(store->self, "other-session", "."), CC_OK));
    call.session_id = "other-session";
    CHECK(result_is(store->vtable->append_records(store->self, &call, 1), CC_OK));
    cc_tool_result_t tool = {0};
    cc_session_record_t wrong_session = {.type = CC_SESSION_RECORD_TOOL_RESULT, .session_id = "session",
        .data.tool_result = {.tool_call_id = "missing-call", .result = &tool}};
    CHECK(result_is(store->vtable->append_records(store->self, &wrong_session, 1), CC_ERR_STORAGE));
    CHECK(row_count(backend->db, "tool_results") == 0);
    CHECK(result_is(store->vtable->clear_session(store->self, "other-session"), CC_OK));
    cc_message_t *message = test_message("transaction-message");
    CHECK(message);
    cc_session_record_t record = {.type = CC_SESSION_RECORD_MESSAGE, .data.message = message};
    CHECK(result_is(store->vtable->append_records(store->self, &record, 1), CC_OK));
    fail_commit = 1;
    CHECK(result_is(store->vtable->clear_session(store->self, "session"), CC_ERR_STORAGE));
    fail_commit = 0;
    CHECK(sqlite3_get_autocommit(backend->db) && row_count(backend->db, "messages") == 1);
    CHECK(result_is(store->vtable->clear_session(store->self, "session"), CC_OK));
    fail_commit = fail_rollback = 1;
    CHECK(result_is(store->vtable->append_records(store->self, &record, 1), CC_ERR_STORAGE));
    fail_commit = fail_rollback = 0;
    CHECK(backend->transaction_failed && !sqlite3_get_autocommit(backend->db));
    CHECK(result_is(store->vtable->create_session(store->self, "blocked", "."), CC_ERR_STORAGE));
    CHECK(result_is(store->vtable->append_records(store->self, NULL, 0), CC_ERR_STORAGE));
    CHECK(result_is(store->vtable->clear_session(store->self, "session"), CC_ERR_STORAGE));
    cc_message_t *messages = NULL;
    cc_session_t *sessions = NULL;
    size_t count = 99;
    CHECK(result_is(store->vtable->load_messages(store->self, "session", 1, &messages, &count), CC_ERR_STORAGE));
    CHECK(!messages && !count);
    CHECK(result_is(store->vtable->list_sessions(store->self, &sessions, &count), CC_ERR_STORAGE));
    CHECK(!sessions && !count);
    cc_message_destroy(message);
    return 0;
}

/** @brief 执行真实 SQLite 的并发、元数据、JSON OOM、语句及事务失败回归。
 * @return 所有场景通过为零，断言失败为一并报告行号。
 * @note 进程独占故障开关和 cJSON hooks，正常结束先排空线程再关闭连接及临时对象。
 */
int main(void)
{
    CHECK(sqlite3_config(SQLITE_CONFIG_GETMALLOC, &native_memory) == SQLITE_OK);
    sqlite3_mem_methods injected = native_memory;
    injected.xMalloc = native_allocate;
    injected.xRealloc = native_resize;
    CHECK(sqlite3_config(SQLITE_CONFIG_MALLOC, &injected) == SQLITE_OK);
    cc_session_store_t store = {0};
    CHECK(result_is(sqlite_test_create_store(":memory:", &store), CC_OK));
    CHECK(result_is(store.vtable->create_session(store.self, "session", "."), CC_OK));
    CHECK(test_concurrent_batch(&store) == 0);
    CHECK(test_metadata_and_errors(&store) == 0);
    CHECK(test_json_failures(&store) == 0);
    CHECK(test_native_allocations(&store) == 0);
    CHECK(test_transactions(&store) == 0);
    store.vtable->destroy(store.self);
    CHECK(sqlite3_shutdown() == SQLITE_OK);
    CHECK(sqlite3_config(SQLITE_CONFIG_MALLOC, &native_memory) == SQLITE_OK);
    puts("SQLite session metadata, ownership, OOM, serialized batches and transaction recovery PASS");
    return 0;
}
