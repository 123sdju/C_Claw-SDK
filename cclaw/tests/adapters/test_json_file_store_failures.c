#include "cc/adapters/cc_json_file_session_store.h"
#include "cc/core/cc_message.h"
#include "cc/ports/cc_path.h"
#include "cc/util/cc_json.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 每个 CTest 进程独占这些故障开关，序号只统计 cJSON 的实际分配。 */
static size_t allocation_count;
static size_t fail_at;
static int fail_parent_path;

/**
 * @brief 在指定 cJSON 分配点返回 NULL，其他分配交给真实 malloc。
 * @param[in] bytes 请求字节数。
 * @return 命中非零故障序号时为 NULL，其他为调用者拥有的分配。
 */
static void *json_test_allocate(size_t bytes)
{
    allocation_count++;
    if (fail_at && allocation_count == fail_at) return NULL;
    return malloc(bytes);
}

/**
 * @brief 注入原子替换完成后无法分配父目录路径的失败。
 * @param[in] path 待解析的文件路径借用。
 * @return 故障启用时为 NULL，其他为调用者 free 的真实目录副本。
 */
static char *json_test_dirname(const char *path)
{
    return fail_parent_path ? NULL : cc_path_dirname(path);
}

/* 为路径故障编译真实后端的私有副本，JSON、文件系统、锁仍链接当前 SDK。 */
#define cc_path_dirname json_test_dirname
#define cc_json_file_store_create json_test_create_store
#include "../../adapters/src/storage/cc_json_file_store.c"
#undef cc_json_file_store_create
#undef cc_path_dirname

/**
 * @brief 检查一个测试条件，失败时报告行号并结束当前测试。
 * @param[in] condition 仅求值一次的条件表达式。
 * @note 成功路径回收全部资源，失败返回进程错误供 CTest 定位。
 */
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "JSON file check failed at %d\n", __LINE__); return 1; } } while (0)

/**
 * @brief 检查并消费一次操作结果。
 * @param[in] result 按值移交的拥有结果，函数释放其诊断。
 * @param[in] expected 期望结果码。
 * @return 结果码相符时为一，否则为零。
 */
static int json_test_result_is(cc_result_t result, cc_error_code_t expected)
{
    int matches = result.code == expected;
    cc_result_free(&result);
    return matches;
}

/**
 * @brief 通过真实默认文件系统读取整份测试文件，用于逐字节比较事务结果。
 * @param[in] path 测试工作目录内的文件路径借用。
 * @return 成功为调用者 free 的文本，失败为 NULL，内部诊断及端口均已释放。
 */
static char *json_test_read_file(const char *path)
{
    cc_filesystem_t fs = {0};
    char *text = NULL;
    cc_result_t result = cc_filesystem_get_default(&fs);
    if (result.code == CC_OK) {
        cc_result_free(&result);
        result = fs.vtable->read_text(fs.self, path, &text);
    }
    if (result.code != CC_OK) {
        free(text);
        text = NULL;
    }
    cc_result_free(&result);
    if (fs.vtable && fs.vtable->destroy) fs.vtable->destroy(fs.self);
    return text;
}

/**
 * @brief 使用真实文件系统把测试基线恢复到独占工作文件。
 * @param[in] path 测试工作目录内的目标路径借用。
 * @param[in] text 要恢复的完整 JSON 文本借用。
 * @return 写入成功为一，其他为零，端口与结果诊断均在返回前回收。
 */
static int json_test_write_file(const char *path, const char *text)
{
    cc_filesystem_t fs = {0};
    cc_result_t result = cc_filesystem_get_default(&fs);
    if (result.code == CC_OK) {
        cc_result_free(&result);
        result = fs.vtable->write_text(fs.self, path, text);
    }
    int success = result.code == CC_OK;
    cc_result_free(&result);
    if (fs.vtable && fs.vtable->destroy) fs.vtable->destroy(fs.self);
    return success;
}

/**
 * @brief 通过真实后端入口执行一种被测变更，构造包含全部可选字段的消息和审计。
 * @param[in,out] store 当前独占测试后端借用。
 * @param[in] operation 零为创建，1 至 3 为单类记录，4 为三类批次，5 为清除。
 * @return 真实入口的拥有结果，由调用者消费，所有输入只在同步调用期间借用。
 */
static cc_result_t json_test_mutate(cc_session_store_t *store, unsigned operation)
{
    if (operation == 0) return store->vtable->create_session(store->self, "new-session", "new-workspace");
    if (operation == 5) return store->vtable->clear_session(store->self, "session");
    cc_message_t *message = NULL;
    cc_result_t result = cc_message_create_text("message", "session", CC_ROLE_TOOL,
                                                "body", "call", &message);
    if (result.code != CC_OK) return result;
    cc_result_free(&result);
    message->reasoning_content = cc_copy_string("reasoning");
    message->created_at = cc_copy_string("2026-11-15T00:00:00Z");
    message->tool_outcome = CC_MESSAGE_TOOL_OUTCOME_UNKNOWN;
    if (!message->reasoning_content || !message->created_at) {
        cc_message_destroy(message);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot create test message");
    }
    cc_tool_call_t call = {.id = "call", .name = "operation", .arguments_json = "{}"};
    cc_tool_result_t tool_result = {0};
    tool_result.ok = 1;
    tool_result.text = "result-text";
    tool_result.error = "result-error";
    tool_result.metadata = "result-metadata";
    cc_session_record_t records[] = {
        {.type = CC_SESSION_RECORD_MESSAGE, .session_id = "session", .data.message = message},
        {.type = CC_SESSION_RECORD_TOOL_CALL, .session_id = "session", .data.tool_call = &call},
        {.type = CC_SESSION_RECORD_TOOL_RESULT, .session_id = "session",
            .data.tool_result = {.tool_call_id = "call", .result = &tool_result}}
    };
    result = operation == 4 ? store->vtable->append_records(store->self, records, 3) :
        store->vtable->append_records(store->self, &records[operation - 1U], 1);
    cc_message_destroy(message);
    return result;
}

/**
 * @brief 独立核对 JSON 对象中的必需字符串字段。
 * @param[in] object 只读对象借用，允许 NULL，此时匹配失败。
 * @param[in] key 必需的字段名借用。
 * @param[in] expected 必需的期望字符串借用。
 * @return 字段为字符串且值相同时为一，其他情况为零，不转移所有权。
 */
static int json_test_string_equals(const cc_json_value_t *object, const char *key, const char *expected)
{
    const char *actual = cc_json_string_value(cc_json_object_get(object, key));
    return actual && strcmp(actual, expected) == 0;
}

/**
 * @brief 独立验证无故障输出包含完整会话、消息或审计字段。
 * @param[in] text 当前操作正常生成的完整 JSON 文本借用。
 * @param[in] operation 被测操作编号，五为清除，由后续原文比较和归属断言验证。
 * @param[in] remaining_records 清除后每个记录数组应保留的其他会话条数。
 * @return 所有适用字段符合预期时为一，其他为零，解析树及结果诊断均已回收。
 */
static int json_test_expected_output(const char *text, unsigned operation, size_t remaining_records)
{
    cc_json_value_t *root = NULL;
    cc_result_t result = cc_json_parse_strict(text, &root);
    int valid = result.code == CC_OK;
    cc_result_free(&result);
    if (valid && operation == 0) {
        cc_json_value_t *sessions = cc_json_object_get(root, "sessions");
        cc_json_value_t *session = cc_json_array_get(sessions, cc_json_array_size(sessions) - 1);
        valid = json_test_string_equals(session, "id", "new-session") &&
            json_test_string_equals(session, "workspace_dir", "new-workspace") &&
            json_test_string_equals(session, "status", "active");
    }
    if (valid && (operation == 1 || operation == 4)) {
        cc_json_value_t *messages = cc_json_object_get(root, "messages");
        cc_json_value_t *message = cc_json_array_get(messages, cc_json_array_size(messages) - 1);
        valid = json_test_string_equals(message, "id", "message") &&
            json_test_string_equals(message, "session_id", "session") &&
            json_test_string_equals(message, "role", "tool") &&
            json_test_string_equals(message, "reasoning_content", "reasoning") &&
            json_test_string_equals(message, "tool_call_id", "call") &&
            json_test_string_equals(message, "created_at", "2026-11-15T00:00:00Z") &&
            json_test_string_equals(message, "tool_outcome", cc_message_tool_outcome_string(CC_MESSAGE_TOOL_OUTCOME_UNKNOWN));
    }
    if (valid && (operation == 2 || operation == 4)) {
        cc_json_value_t *calls = cc_json_object_get(root, "tool_calls");
        cc_json_value_t *call = cc_json_array_get(calls, cc_json_array_size(calls) - 1);
        valid = json_test_string_equals(call, "id", "call") &&
            json_test_string_equals(call, "session_id", "session") &&
            json_test_string_equals(call, "name", "operation") &&
            json_test_string_equals(call, "arguments_json", "{}") &&
            json_test_string_equals(call, "status", "completed");
    }
    if (valid && (operation == 3 || operation == 4)) {
        cc_json_value_t *results = cc_json_object_get(root, "tool_results");
        cc_json_value_t *record = cc_json_array_get(results, cc_json_array_size(results) - 1);
        valid = json_test_string_equals(record, "id", "call") &&
            json_test_string_equals(record, "session_id", "session") &&
            json_test_string_equals(record, "tool_call_id", "call") &&
            cc_json_is_bool(cc_json_object_get(record, "ok")) &&
            cc_json_bool_value(cc_json_object_get(record, "ok")) &&
            json_test_string_equals(record, "text", "result-text") &&
            json_test_string_equals(record, "error", "result-error") &&
            json_test_string_equals(record, "metadata", "result-metadata") &&
            json_test_string_equals(record, "artifacts", "[]");
    }
    if (valid && operation == 5) {
        const char *const arrays[] = {"messages", "tool_calls", "tool_results"};
        for (size_t index = 0; valid && index < 3; index++) {
            const cc_json_value_t *array = cc_json_object_get(root, arrays[index]);
            valid = cc_json_is_array(array) && (size_t)cc_json_array_size(array) == remaining_records;
            for (const cc_json_value_t *record = cc_json_array_first(array); valid && record;
                 record = cc_json_array_next(record)) {
                valid = json_test_string_equals(record, "session_id", "other") &&
                    json_test_string_equals(record, "extension", "keep");
            }
        }
    }
    cc_json_destroy(root);
    return valid;
}

/**
 * @brief 穷举一次变更中的 cJSON 分配点，成功须完整持久化，失败须保留原文件。
 * @param[in] path 独占测试文件路径借用。
 * @param[in] baseline 合法的原始会话库完整文本借用。
 * @param[in] operation 交给 json_test_mutate 的变更编号。
 * @param[in] remaining_records 清除后每个数组应保留的其他会话条数。
 * @return 全部断言通过为零，失败为一，成功时回收每个测试实例及文本副本。
 */
static int json_test_allocation_failures(const char *path, const char *baseline, unsigned operation,
                                         size_t remaining_records)
{
    cc_session_store_t store = {0};
    CHECK(json_test_write_file(path, baseline));
    CHECK(json_test_result_is(json_test_create_store(path, &store), CC_OK));
    allocation_count = 0;
    CHECK(json_test_result_is(json_test_mutate(&store, operation), CC_OK));
    size_t allocations = allocation_count;
    store.vtable->destroy(store.self);
    char *expected = json_test_read_file(path);
    CHECK(expected && allocations);
    CHECK(json_test_expected_output(expected, operation, remaining_records));
    for (size_t failure = 1; failure <= allocations; failure++) {
        CHECK(json_test_write_file(path, baseline));
        CHECK(json_test_result_is(json_test_create_store(path, &store), CC_OK));
        allocation_count = 0;
        fail_at = failure;
        cc_result_t result = json_test_mutate(&store, operation);
        fail_at = 0;
        char *actual = json_test_read_file(path);
        CHECK(actual);
        const char *wanted = result.code == CC_OK ? expected : baseline;
        if (strcmp(actual, wanted) != 0) {
            fprintf(stderr, "operation %u allocation %zu returned %d with incomplete persistence\n",
                    operation, failure, result.code);
            free(actual);
            cc_result_free(&result);
            store.vtable->destroy(store.self);
            free(expected);
            return 1;
        }
        free(actual);
        cc_result_free(&result);
        store.vtable->destroy(store.self);
    }
    printf("JSON file operation %u checked %zu allocation points\n", operation, allocations);
    free(expected);
    return 0;
}

/**
 * @brief 验证已替换文件后父目录路径分配失败会锁存未知写入并要求重新打开。
 * @param[in] path 独占测试文件路径借用。
 * @param[in] baseline 原始完整会话库文本借用。
 * @return 全部断言通过为零，失败为一，成功时回收所有输出与后端。
 */
static int json_test_indeterminate_parent_path(const char *path, const char *baseline)
{
    cc_session_store_t store = {0};
    CHECK(json_test_write_file(path, baseline));
    CHECK(json_test_result_is(json_test_create_store(path, &store), CC_OK));
    CHECK(((cc_json_file_store_t *)store.self)->fs.vtable->sync_dir);
    fail_parent_path = 1;
    CHECK(json_test_result_is(json_test_mutate(&store, 0), CC_ERR_OUT_OF_MEMORY));
    fail_parent_path = 0;
    cc_session_t *sessions = NULL;
    size_t count = 0;
    CHECK(json_test_result_is(store.vtable->list_sessions(store.self, &sessions, &count), CC_ERR_STORAGE));
    CHECK(!sessions && !count);
    store.vtable->destroy(store.self);
    CHECK(json_test_result_is(json_test_create_store(path, &store), CC_OK));
    CHECK(json_test_result_is(store.vtable->list_sessions(store.self, &sessions, &count), CC_OK));
    CHECK(count == 3);
    cc_session_free_array(sessions, count);
    store.vtable->destroy(store.self);
    return 0;
}

/**
 * @brief 执行整文件 JSON 后端创建、追加、清除与替换后故障回归。
 * @return 全部验证成功为零，失败为一，成功时恢复 cJSON 分配器并删除独占测试文件。
 */
int main(void)
{
    const char *path = "json-file-failures.json";
    const char *baseline = "{\"sessions\":[{\"id\":\"session\",\"workspace_dir\":\"workspace\",\"status\":\"active\"},"
        "{\"id\":\"other\",\"workspace_dir\":\"other-workspace\",\"status\":\"completed\"}],"
        "\"messages\":[{\"id\":\"old-message\",\"session_id\":\"session\",\"role\":\"user\","
        "\"content_parts\":\"[]\",\"tool_calls\":\"[]\"},{\"id\":\"other-message\",\"session_id\":\"other\","
        "\"role\":\"user\",\"content_parts\":\"[]\",\"tool_calls\":\"[]\",\"extension\":\"keep\"}],"
        "\"tool_calls\":[{\"session_id\":\"session\",\"extension\":\"call\"},"
        "{\"session_id\":\"other\",\"extension\":\"keep\"}],"
        "\"tool_results\":[{\"session_id\":\"session\",\"extension\":\"result\"},"
        "{\"session_id\":\"other\",\"extension\":\"keep\"}]}";
    const char *without_audits = "{\"sessions\":[{\"id\":\"session\"}],\"messages\":[]}";
    cJSON_Hooks hooks = {.malloc_fn = json_test_allocate, .free_fn = free};
    cJSON_InitHooks(&hooks);
    for (unsigned operation = 0; operation < 6; operation++) {
        CHECK(json_test_allocation_failures(path, baseline, operation, 1) == 0);
        CHECK(json_test_allocation_failures(path, without_audits, operation, 0) == 0);
    }
    CHECK(json_test_indeterminate_parent_path(path, baseline) == 0);
    cJSON_InitHooks(NULL);
    CHECK(remove(path) == 0);
    return 0;
}
