#include "cc/ports/cc_memory_store.h"
#include "cc/ports/cc_thread.h"
#include "../../adapters/vendor/sqlite/sqlite3.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum api_failure { API_NONE, API_PREPARE, API_BIND_TEXT, API_BIND_INT, API_BIND_INT64 };
static enum api_failure failed_api;
static size_t allocation_calls, failed_allocation = SIZE_MAX;
static size_t step_calls, failed_step = SIZE_MAX;
static int fail_reallocation, fail_open, fail_mutex, fail_schema, fail_finalize;
static int connections, statements, mutexes;

/** @brief 注入后端局部分配失败。 @param[in] bytes 字节数。 @return 独占分配或 NULL。 */
static void *fault_malloc(size_t bytes)
{
    if (allocation_calls++ == failed_allocation) return NULL;
    return malloc(bytes);
}

/** @brief 注入后端清零分配失败。 @param[in] count 项数。 @param[in] bytes 项大小。 @return 清零分配或 NULL。 */
static void *fault_calloc(size_t count, size_t bytes)
{
    if (allocation_calls++ == failed_allocation) return NULL;
    return calloc(count, bytes);
}

/** @brief 保留旧数组并注入扩容失败。 @param[in,out] pointer 原分配。 @param[in] bytes 新容量。 @return 新地址或 NULL。 */
static void *fault_realloc(void *pointer, size_t bytes)
{
    size_t attempt = allocation_calls++;
    if (fail_reallocation) {
        fail_reallocation = 0;
        return NULL;
    }
    if (attempt == failed_allocation) return NULL;
    return realloc(pointer, bytes);
}

/** @brief 创建真实连接或带真实错误文本的失败句柄。 @param[in] path 路径借用。 @param[out] out 连接。 @return SQLite 状态。 */
static int fault_open(const char *path, sqlite3 **out)
{
    int code = sqlite3_open(path, out);
    if (*out) connections++;
    if (code == SQLITE_OK && fail_open) {
        fail_open = 0;
        return sqlite3_exec(*out, "SELECT * FROM missing_for_open", NULL, NULL, NULL);
    }
    return code;
}

/** @brief 关闭真实连接并登记释放。 @param[in,out] db 连接。 @return SQLite 状态。 */
static int fault_close(sqlite3 *db)
{
    int code = sqlite3_close(db);
    if (code == SQLITE_OK && db) connections--;
    return code;
}

/** @brief 准备真实语句或注入 NOMEM。 @param[in] db 连接。 @param[in] sql SQL借用。 @param[in] bytes SQL长度。 @param[out] out 必填语句槽。 @param[out] tail 可空尾部槽。 @return 原生状态。 */
static int fault_prepare(sqlite3 *db, const char *sql, int bytes, sqlite3_stmt **out, const char **tail)
{
    if (failed_api == API_PREPARE) {
        failed_api = API_NONE;
        *out = NULL;
        return SQLITE_NOMEM;
    }
    int code = sqlite3_prepare_v2(db, sql, bytes, out, tail);
    if (*out) statements++;
    return code;
}

/** @brief 完整消费真实语句后可报告错误。 @param[in,out] stmt 可空语句。 @return 原生或注入状态。 */
static int fault_finalize(sqlite3_stmt *stmt)
{
    int code = sqlite3_finalize(stmt);
    if (stmt) statements--;
    if (code == SQLITE_OK && fail_finalize) {
        fail_finalize = 0;
        return SQLITE_IOERR;
    }
    return code;
}

/** @brief 在指定 step 前注入 IO，其他推进真实数据库。 @param[in,out] stmt 语句。 @return 原生状态。 */
static int fault_step(sqlite3_stmt *stmt)
{
    if (step_calls++ == failed_step) return SQLITE_IOERR;
    return sqlite3_step(stmt);
}

/** @brief 绑定真实文本或注入 NOMEM。 @param[in,out] stmt 语句。 @param[in] index 参数号。 @param[in] text 可空文本。 @param[in] bytes 长度。 @param[in] destroy 销毁策略或 SQLITE_TRANSIENT 哨兵。 @return 原生状态。 */
static int fault_bind_text(sqlite3_stmt *stmt, int index, const char *text, int bytes, void (*destroy)(void *))
{
    if (failed_api == API_BIND_TEXT) {
        failed_api = API_NONE;
        return SQLITE_NOMEM;
    }
    return sqlite3_bind_text(stmt, index, text, bytes, destroy);
}

/** @brief 绑定真实 int 或注入 NOMEM。 @param[in,out] stmt 语句。 @param[in] index 参数号。 @param[in] value 值。 @return 原生状态。 */
static int fault_bind_int(sqlite3_stmt *stmt, int index, int value)
{
    if (failed_api == API_BIND_INT) {
        failed_api = API_NONE;
        return SQLITE_NOMEM;
    }
    return sqlite3_bind_int(stmt, index, value);
}

/** @brief 绑定真实时间整数或注入 NOMEM。 @param[in,out] stmt 语句。 @param[in] index 参数号。 @param[in] value 值。 @return 原生状态。 */
static int fault_bind_int64(sqlite3_stmt *stmt, int index, sqlite3_int64 value)
{
    if (failed_api == API_BIND_INT64) {
        failed_api = API_NONE;
        return SQLITE_NOMEM;
    }
    return sqlite3_bind_int64(stmt, index, value);
}

/** @brief 执行真实初始化 SQL 或注入 NOMEM。 @param[in] db 连接。 @param[in] sql SQL。 @param[in] callback 可空行回调。 @param[in] data 可空用户数据。 @param[out] error 可空错误槽。 @return 原生状态。 */
static int fault_exec(sqlite3 *db, const char *sql, int (*callback)(void *, int, char **, char **),
    void *data, char **error)
{
    if (fail_schema) {
        fail_schema = 0;
        if (error) *error = NULL;
        return SQLITE_NOMEM;
    }
    return sqlite3_exec(db, sql, callback, data, error);
}

/** @brief 创建真实 mutex 或返回平台错误。 @param[out] out 新拥有槽。 @return 拥有结果。 */
static cc_result_t fault_mutex_create(cc_mutex_t *out)
{
    if (fail_mutex) {
        fail_mutex = 0;
        *out = NULL;
        return cc_result_error(CC_ERR_PLATFORM, "Injected mutex failure");
    }
    cc_result_t rc = cc_mutex_create(out);
    if (rc.code == CC_OK) mutexes++;
    return rc;
}

/** @brief 消费真实 mutex。 @param[in,out] mutex 拥有句柄。 @post 登记数递减。 */
static void fault_mutex_destroy(cc_mutex_t mutex)
{
    cc_mutex_destroy(mutex);
    if (mutex) mutexes--;
}

#define malloc fault_malloc
#define calloc fault_calloc
#define realloc fault_realloc
#define sqlite3_open fault_open
#define sqlite3_close fault_close
#define sqlite3_prepare_v2 fault_prepare
#define sqlite3_finalize fault_finalize
#define sqlite3_step fault_step
#define sqlite3_bind_text fault_bind_text
#define sqlite3_bind_int fault_bind_int
#define sqlite3_bind_int64 fault_bind_int64
#define sqlite3_exec fault_exec
#define cc_mutex_create fault_mutex_create
#define cc_mutex_destroy fault_mutex_destroy
#define cc_memory_store_create_sqlite review_sqlite_memory_create
#ifndef CC_SQLITE_MEMORY_TEST_SOURCE
#define CC_SQLITE_MEMORY_TEST_SOURCE "../../adapters/src/storage/cc_sqlite_memory_store.c"
#endif
#include CC_SQLITE_MEMORY_TEST_SOURCE
#undef malloc
#undef calloc
#undef realloc
#undef sqlite3_open
#undef sqlite3_close
#undef sqlite3_prepare_v2
#undef sqlite3_finalize
#undef sqlite3_step
#undef sqlite3_bind_text
#undef sqlite3_bind_int
#undef sqlite3_bind_int64
#undef sqlite3_exec
#undef cc_mutex_create
#undef cc_mutex_destroy
#undef cc_memory_store_create_sqlite

/** @brief 清空一次性故障与调用计数。 @post 所有注入关闭，资源登记数保留。 */
static void reset_faults(void)
{
    failed_api = API_NONE;
    allocation_calls = step_calls = 0;
    failed_allocation = failed_step = SIZE_MAX;
    fail_reallocation = fail_open = fail_mutex = fail_schema = fail_finalize = 0;
}

/** @brief 断言结果码并消费诊断。 @param[in,out] rc 拥有结果。 @param[in] expected 期望码。 @post 诊断释放。 */
static void expect_code(cc_result_t rc, cc_error_code_t expected)
{
    if (rc.code != expected) fprintf(stderr, "actual %d expected %d: %s\n", rc.code, expected, rc.message ? rc.message : "");
    assert(rc.code == expected);
    cc_result_free(&rc);
}

/** @brief 写入十七条真实行以覆盖首次扩容。 @param[in,out] store 存活端口。 @post 每行有四个非空字符串字段。 */
static void seed_rows(cc_memory_store_t *store)
{
    for (int i = 0; i < 17; i++) {
        char key[16];
        snprintf(key, sizeof(key), "k%02d", i);
        expect_code(store->vtable->set(store->self, key, "value", "cat", "session"), CC_OK);
    }
}

/**
 * @brief 调用一个真实查询入口并核对成功形状或失败空输出。
 * @param[in] store 存活端口借用。
 * @param[in] operation 零为 get，一为 search，二为 list，三为 query。
 * @return 未消费的拥有结果，输出字段在本函数归还。
 */
static cc_result_t read_case(cc_memory_store_t *store, int operation)
{
    cc_result_t rc;
    if (operation == 0) {
        cc_memory_entry_t entry = {0};
        rc = store->vtable->get(store->self, "k00", &entry);
        if (rc.code == CC_OK) assert(entry.key && entry.value && entry.category && entry.session_id);
        else assert(!entry.key && !entry.value && !entry.category && !entry.session_id);
        cc_memory_entry_free(&entry);
    } else if (operation == 3) {
        cc_memory_search_result_t *results = (void *)(uintptr_t)1;
        size_t count = 99;
        cc_memory_query_t query = {.size = sizeof(query), .query = "", .limit = 100};
        rc = store->vtable->query(store->self, &query, &results, &count);
        if (rc.code == CC_OK) assert(results && count == 17);
        else assert(!results && !count);
        cc_memory_search_result_free_array(results, count);
    } else {
        cc_memory_entry_t *entries = (void *)(uintptr_t)1;
        size_t count = 99;
        rc = operation == 1 ? store->vtable->search(store->self, "", 100, &entries, &count) :
            store->vtable->list(store->self, "cat", 0, &entries, &count);
        if (rc.code == CC_OK) {
            assert(entries && count == 17);
            for (size_t i = 0; i < count; i++) assert(entries[i].key && entries[i].value && entries[i].category && entries[i].session_id);
        } else assert(!entries && !count);
        cc_memory_entry_free_array(entries, count);
    }
    assert(!statements);
    return rc;
}

/** @brief 逐分配验证四种查询整批撤销。 @param[in] store 存活且有十七条行的端口。 @post 注入关闭且无在途语句。 */
static void test_allocation_failures(cc_memory_store_t *store)
{
    for (int operation = 0; operation < 4; operation++) {
        reset_faults();
        expect_code(read_case(store, operation), CC_OK);
        size_t attempts = allocation_calls;
        assert(attempts > 0);
        for (size_t failure = 0; failure < attempts; failure++) {
            reset_faults();
            failed_allocation = failure;
            expect_code(read_case(store, operation), CC_ERR_OUT_OF_MEMORY);
        }
    }
    reset_faults();
}

/** @brief 强制 list 的十六至三十二项扩容失败。 @param[in] store 存活端口。 @post 原行仍可查询且无语句残留。 */
static void test_growth_failure(cc_memory_store_t *store)
{
    reset_faults();
    fail_reallocation = 1;
    expect_code(read_case(store, 2), CC_ERR_OUT_OF_MEMORY);
    reset_faults();
    expect_code(read_case(store, 2), CC_OK);
}

/** @brief 原生 prepare/bind/step/finalize 错误不得被当成成功或缺失。 @param[in,out] store 存活端口。 @post 原记录保留，注入关闭。 */
static void test_native_failures(cc_memory_store_t *store)
{
    for (int operation = 0; operation < 4; operation++) {
        reset_faults(); failed_api = API_PREPARE;
        expect_code(read_case(store, operation), CC_ERR_OUT_OF_MEMORY);
        reset_faults(); failed_api = API_BIND_TEXT;
        expect_code(read_case(store, operation), CC_ERR_OUT_OF_MEMORY);
        reset_faults(); failed_step = operation == 0 ? 0 : 16;
        expect_code(read_case(store, operation), CC_ERR_STORAGE);
        reset_faults(); fail_finalize = 1;
        expect_code(read_case(store, operation), CC_ERR_STORAGE);
    }
    reset_faults(); failed_api = API_BIND_INT64;
    expect_code(store->vtable->set(store->self, "new", "value", NULL, NULL), CC_ERR_OUT_OF_MEMORY);
    reset_faults(); failed_api = API_BIND_INT;
    expect_code(read_case(store, 1), CC_ERR_OUT_OF_MEMORY);
    reset_faults(); failed_api = API_BIND_INT;
    expect_code(read_case(store, 3), CC_ERR_OUT_OF_MEMORY);
    reset_faults(); failed_step = 0;
    expect_code(store->vtable->set(store->self, "new", "value", NULL, NULL), CC_ERR_STORAGE);
    reset_faults(); failed_step = 0;
    expect_code(store->vtable->delete_entry(store->self, "k00"), CC_ERR_STORAGE);
    reset_faults(); failed_step = 0;
    expect_code(store->vtable->delete_by_category(store->self, "cat"), CC_ERR_STORAGE);
    reset_faults(); fail_finalize = 1;
    expect_code(store->vtable->set(store->self, "committed", "value", NULL, NULL), CC_ERR_STORAGE);
    reset_faults();
    cc_memory_entry_t entry = {0};
    expect_code(store->vtable->get(store->self, "committed", &entry), CC_OK);
    assert(entry.key && strcmp(entry.key, "committed") == 0);
    cc_memory_entry_free(&entry);
    expect_code(store->vtable->delete_entry(store->self, "committed"), CC_OK);
    reset_faults();
    expect_code(read_case(store, 2), CC_OK);
}

/** @brief 构造阶段任一错误清空输出并归还数据库/锁。 @post 所有失败路径的资源登记均为零。 */
static void test_constructor_failures(void)
{
    cc_memory_store_t store = {0};
    reset_faults(); failed_allocation = 0;
    expect_code(review_sqlite_memory_create(&store, ":memory:"), CC_ERR_OUT_OF_MEMORY);
    assert(!store.self && !store.vtable && !connections && !mutexes);
    reset_faults(); fail_open = 1;
    cc_result_t rc = review_sqlite_memory_create(&store, ":memory:");
    assert(rc.message && strstr(rc.message, "missing_for_open"));
    expect_code(rc, CC_ERR_STORAGE);
    assert(!store.self && !store.vtable && !connections && !mutexes);
    reset_faults(); fail_mutex = 1;
    expect_code(review_sqlite_memory_create(&store, ":memory:"), CC_ERR_PLATFORM);
    assert(!store.self && !store.vtable && !connections && !mutexes);
    reset_faults(); fail_schema = 1;
    expect_code(review_sqlite_memory_create(&store, ":memory:"), CC_ERR_OUT_OF_MEMORY);
    assert(!store.self && !store.vtable && !connections && !mutexes);
    reset_faults();
}

/** @brief 长 LIKE 文本不被截断，结构化过滤及正 limit 保持有效。 @param[in,out] store 存活端口。 @post 临时长文本和查询数组归还。 */
static void test_query_semantics(cc_memory_store_t *store)
{
    char text[1501];
    memset(text, 'x', sizeof(text) - 1); text[sizeof(text) - 1] = '\0';
    expect_code(store->vtable->set(store->self, "long", text, "long", NULL), CC_OK);
    cc_memory_entry_t *entries = NULL;
    size_t count = 0;
    expect_code(store->vtable->search(store->self, text, 10, &entries, &count), CC_OK);
    assert(count == 1 && strcmp(entries[0].key, "long") == 0);
    cc_memory_entry_free_array(entries, count);
    cc_memory_query_t query = {.size = sizeof(query), .query = "", .category = "cat", .session_id = "session", .limit = 3};
    cc_memory_search_result_t *results = NULL;
    expect_code(store->vtable->query(store->self, &query, &results, &count), CC_OK);
    assert(count == 3);
    cc_memory_search_result_free_array(results, count);
    query.category = NULL;
    query.session_id = NULL;
    query.exclude_category = "cat";
    query.exclude_category2 = "long";
    expect_code(store->vtable->query(store->self, &query, &results, &count), CC_OK);
    assert(count == 0);
    cc_memory_search_result_free_array(results, count);
    expect_code(store->vtable->delete_entry(store->self, "long"), CC_OK);
    cc_sqlite_memory_store_t *state = store->self;
    assert(sqlite3_exec(state->db,
        "INSERT INTO memory VALUES(NULL,'malformed-row','malformed',NULL,0,0)", NULL, NULL, NULL) == SQLITE_OK);
    entries = NULL;
    expect_code(store->vtable->list(store->self, "malformed", 0, &entries, &count), CC_ERR_STORAGE);
    assert(!entries && !count);
    expect_code(store->vtable->search(store->self, "malformed-row", 10, &entries, &count), CC_ERR_STORAGE);
    assert(!entries && !count);
    query.category = "malformed";
    query.exclude_category = query.exclude_category2 = NULL;
    expect_code(store->vtable->query(store->self, &query, &results, &count), CC_ERR_STORAGE);
    assert(!results && !count);
    expect_code(store->vtable->delete_by_category(store->self, "malformed"), CC_OK);
}

/** @brief 运行真实 SQLite 的资源和故障回归，可单选 grow 供旧版对照。 @param[in] argc 参数数。 @param[in] argv 调用期参数。 @return 通过为零，assert失败中止。 */
int main(int argc, char **argv)
{
    int only_growth = argc == 2 && strcmp(argv[1], "grow") == 0;
    if (!only_growth) test_constructor_failures();
    cc_memory_store_t store = {0};
    expect_code(review_sqlite_memory_create(&store, ":memory:"), CC_OK);
    seed_rows(&store);
    test_growth_failure(&store);
    if (!only_growth) {
        test_allocation_failures(&store);
        test_native_failures(&store);
        test_query_semantics(&store);
    }
    store.vtable->destroy(store.self);
    assert(!connections && !mutexes && !statements);
    puts("SQLite memory allocation, native error and lifecycle contracts: PASS");
    return 0;
}
