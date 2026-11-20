#include "cc/core/cc_message.h"
#include "cc/util/cc_json.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 独立 CTest 进程串行切换，统计真实 cJSON 分配。 */
static size_t allocations;
static size_t fail_at;

/** @brief 在指定 cJSON 分配点注入失败。
 * @param[in] bytes 请求字节数。
 * @return 命中返回 NULL，其余真实 malloc，拥有缓冲由 cJSON 最终释放。
 */
static void *fault_allocate(size_t bytes)
{
    allocations++;
    return fail_at && allocations == fail_at ? NULL : malloc(bytes);
}

/* 直接编译当前真实实现，私有 static helper 也可执行容量与媒体故障反例。 */
#include "../../adapters/src/storage/cc_json_segmented_store.c"

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "segmented failure at %d\n", __LINE__); return 1; } } while (0)

/** @brief 检查并消费独占结果。
 * @param[in] result 按值移交结果，内部释放诊断。
 * @return 成功一，否则报告后零，串行调用。
 */
static int success(cc_result_t result)
{
    int good = result.code == CC_OK;
    if (!good) fprintf(stderr, "%s\n", result.message ? result.message : "error");
    cc_result_free(&result);
    return good;
}

/** @brief 返回夹具唯一会话的首段路径。
 * @param[in] store 存活独占端口借用。
 * @return 调用者 free 的路径，分配失败 NULL，临时目录路径内部回收。
 */
static char *first_segment(cc_session_store_t *store)
{
    char *dir = session_dir_path(store->self, "session");
    char *path = dir ? segment_path_for_session_dir(dir, 1) : NULL;
    free(dir);
    return path;
}

/** @brief 读取测试首段供完整字节比较。
 * @param[in] store 存活端口借用。
 * @return 调用者 free 的文本，失败 NULL，路径与诊断内部释放。
 */
static char *read_segment(cc_session_store_t *store)
{
    cc_json_segmented_store_t *impl = store->self;
    char *path = first_segment(store);
    char *text = NULL;
    if (path) {
        cc_result_t rc = impl->fs.vtable->read_text(impl->fs.self, path, &text);
        if (rc.code != CC_OK) { free(text); text = NULL; }
        cc_result_free(&rc);
    }
    free(path);
    return text;
}

/** @brief 穷举单类及混合批次的 JSON 分配故障并核对完整磁盘结果。
 * @param[in] store 独占测试端口借用。
 * @param[in] records 三条稳定输入记录借用，覆盖全部可选字段。
 * @param[in] operation 零至二为单类，三为三类混合。
 * @return 全部通过零，否则一，成功清理全部快照，串行切换全局 hook。
 */
static int check_allocations(cc_session_store_t *store, const cc_session_record_t *records, unsigned operation)
{
    CHECK(success(store->vtable->clear_session(store->self, "session")));
    const cc_session_record_t *input = operation < 3 ? &records[operation] : records;
    size_t count = operation < 3 ? 1 : 3;
    allocations = 0;
    CHECK(success(store->vtable->append_records(store->self, input, count)));
    size_t total = allocations;
    char *expected = read_segment(store);
    CHECK(expected && total);
    if (operation == 0 || operation == 3)
        CHECK(strstr(expected, "reasoning") && strstr(expected, "2026-11-15") && strstr(expected, "tool_outcome"));
    if (operation == 2 || operation == 3)
        CHECK(strstr(expected, "metadata") && strstr(expected, "artifacts"));
    for (size_t point = 1; point <= total; point++) {
        CHECK(success(store->vtable->clear_session(store->self, "session")));
        allocations = 0;
        fail_at = point;
        cc_result_t result = store->vtable->append_records(store->self, input, count);
        fail_at = 0;
        char *actual = read_segment(store);
        CHECK(actual);
        int valid = strcmp(actual, result.code == CC_OK ? expected : "") == 0;
        if (!valid) fprintf(stderr, "operation %u allocation %zu wrote partial record, result %d\n",
                            operation, point, (int)result.code);
        free(actual);
        cc_result_free(&result);
        CHECK(valid);
    }
    printf("segmented operation %u checked %zu cJSON allocation points\n", operation, total);
    free(expected);
    return 0;
}

typedef struct media_fault {
    const cc_filesystem_vtable_t *base;
    int stat_error;
    int read_error;
    int inflate_read;
    int replace_error;
    size_t reads;
} media_fault_t;
static media_fault_t media_fault;

/** @brief 对媒体查询注入错误，其余路径转发真实 stat。
 * @param[in] self 真实文件系统 self 借用。
 * @param[in] path 路径借用。
 * @param[out] info 元数据槽，沿真实端口规则。
 * @return 指定媒体错误或真实结果，串行开关，诊断归调用者。
 */
static cc_result_t media_stat(void *self, const char *path, cc_filesystem_stat_t *info)
{
    if (strstr(path, "media/") && media_fault.stat_error)
        return cc_result_error((cc_error_code_t)media_fault.stat_error, "injected media stat");
    return media_fault.base->stat(self, path, info);
}

/** @brief 统计媒体读取，可注入资源/I/O 错误或模拟读取期间增长。
 * @param[in] self 真实文件系统 self 借用。
 * @param[in] path 路径借用。
 * @param[out] data 新槽，成功由调用者 free。
 * @param[out] size 实际字节数槽。
 * @return 注入或真实结果，成功增长缓冲由测试分配，串行开关。
 */
static cc_result_t media_read(void *self, const char *path, void **data, size_t *size)
{
    media_fault.reads++;
    *data = NULL;
    *size = 0;
    if (media_fault.read_error)
        return cc_result_error((cc_error_code_t)media_fault.read_error, "injected media read");
    if (media_fault.inflate_read) {
        *data = calloc(64, 1);
        *size = *data ? 64 : 0;
        return *data ? cc_result_ok() : cc_result_error(CC_ERR_OUT_OF_MEMORY, "inflate media");
    }
    return media_fault.base->read_bytes(self, path, data, size);
}

/** @brief 注入清单替换前失败或替换完成后的未知结果。
 * @param[in] self 真实文件系统 self 借用。
 * @param[in] temporary 暂存路径借用。
 * @param[in] destination 目标路径借用。
 * @return 模式一直接 IO，模式二原替换成功后 IO，其余透传，串行开关。
 */
static cc_result_t fault_replace(void *self, const char *temporary, const char *destination)
{
    if (media_fault.replace_error == 1)
        return cc_result_error(CC_ERR_IO, "injected pre-install failure");
    cc_result_t rc = media_fault.base->atomic_replace(self, temporary, destination);
    if (rc.code == CC_OK && media_fault.replace_error == 2)
        return cc_result_error(CC_ERR_IO, "injected post-install failure");
    return rc;
}

/** @brief 验证滚动替换失败保留证据，替换后失败保留完整新清单和空新段。
 * @param[in] store 独占端口借用，返回前恢复函数表和软分段阈值。
 * @param[in] record 稳定有效追加记录借用。
 * @return 全部通过零，否则一，成功清理所有快照/路径，故障不执行工具。
 */
static int check_rollover_failures(cc_session_store_t *store, const cc_session_record_t *record)
{
    cc_json_segmented_store_t *impl = store->self;
    const cc_filesystem_vtable_t *original = impl->fs.vtable;
    cc_filesystem_vtable_t wrapped = *original;
    wrapped.atomic_replace = fault_replace;
    media_fault = (media_fault_t){.base = original};
    for (int mode = 1; mode <= 2; mode++) {
        impl->segment_bytes = 16U * 1024U * 1024U;
        CHECK(success(store->vtable->clear_session(store->self, "session")));
        CHECK(success(store->vtable->append_records(store->self, record, 1)));
        char *before = read_segment(store);
        char *dir = session_dir_path(impl, "session");
        char *manifest = dir ? manifest_path_for_session_dir(dir) : NULL;
        char *temporary = manifest ? dup_printf("%s.tmp", manifest) : NULL;
        char *next = dir ? segment_path_for_session_dir(dir, 2) : NULL;
        CHECK(before && dir && manifest && temporary && next);
        impl->segment_bytes = 1;
        impl->fs.vtable = &wrapped;
        media_fault.replace_error = mode;
        cc_result_t rc = store->vtable->append_records(store->self, record, 1);
        media_fault.replace_error = 0;
        impl->fs.vtable = original;
        CHECK(rc.code == CC_ERR_IO);
        cc_result_free(&rc);
        char *after = read_segment(store);
        CHECK(after && strcmp(before, after) == 0);
        cc_filesystem_stat_t info = {.size = sizeof(info)};
        CHECK(success(original->stat(impl->fs.self, temporary, &info)));
        CHECK(info.type == (mode == 1 ? CC_FS_ENTRY_FILE : CC_FS_ENTRY_MISSING));
        CHECK(success(original->stat(impl->fs.self, next, &info)) && info.type == CC_FS_ENTRY_FILE && info.byte_size == 0);
        if (mode == 1) {
            cc_message_t *messages = NULL;
            size_t count = 0;
            rc = store->vtable->load_messages(store->self, "session", 1, &messages, &count);
            CHECK(rc.code == CC_ERR_STORAGE && !messages && !count);
            cc_result_free(&rc);
        } else {
            int current = 0;
            CHECK(success(load_current_segment(impl, dir, "session", &current)) && current == 2);
        }
        free(before); free(after); free(dir); free(manifest); free(temporary); free(next);
    }
    impl->segment_bytes = 16U * 1024U * 1024U;
    return 0;
}

/** @brief 验证媒体查询失败禁止读取、OOM传播、普通IO保留元数据和预算增长处理。
 * @param[in] store 独占测试后端借用，返回前恢复原函数表。
 * @return 全部通过零，否则一，成功释放内容、文件与路径，串行运行。
 */
static int check_media_faults(cc_session_store_t *store)
{
    cc_json_segmented_store_t *impl = store->self;
    const cc_filesystem_vtable_t *original = impl->fs.vtable;
    cc_filesystem_vtable_t wrapped = *original;
    wrapped.stat = media_stat;
    wrapped.read_bytes = media_read;
    media_fault = (media_fault_t){.base = original};
    char *dir = session_dir_path(impl, "session");
    char *path = dir ? join_path2(dir, "media/test.bin") : NULL;
    CHECK(dir && path);
    CHECK(success(original->write_bytes(impl->fs.self, path, "data", 4)));
    impl->fs.vtable = &wrapped;
    cc_content_part_t part = {0};
    part.kind = CC_MEDIA_FILE;
    part.artifact.kind = CC_MEDIA_FILE;
    part.artifact.path = cc_copy_string("media/test.bin");
    CHECK(part.artifact.path);
    size_t remaining = 32;
    media_fault.stat_error = CC_ERR_IO;
    CHECK(success(hydrate_context_part(impl, dir, &part, &remaining, 1)));
    CHECK(media_fault.reads == 0 && !part.artifact.data_base64 && remaining == 32);
    media_fault.stat_error = CC_ERR_OUT_OF_MEMORY;
    cc_result_t rc = hydrate_context_part(impl, dir, &part, &remaining, 1);
    CHECK(rc.code == CC_ERR_OUT_OF_MEMORY && media_fault.reads == 0);
    cc_result_free(&rc);
    media_fault.stat_error = 0;
    media_fault.read_error = CC_ERR_OUT_OF_MEMORY;
    rc = hydrate_context_part(impl, dir, &part, &remaining, 1);
    CHECK(rc.code == CC_ERR_OUT_OF_MEMORY);
    cc_result_free(&rc);
    media_fault.read_error = CC_ERR_IO;
    CHECK(success(hydrate_context_part(impl, dir, &part, &remaining, 1)));
    CHECK(!part.artifact.data_base64 && remaining == 32);
    media_fault.read_error = 0;
    media_fault.inflate_read = 1;
    impl->max_base64_bytes = 32;
    rc = hydrate_context_part(impl, dir, &part, &remaining, 1);
    CHECK(rc.code == CC_ERR_LIMIT_EXCEEDED);
    cc_result_free(&rc);
    CHECK(success(hydrate_context_part(impl, dir, &part, &remaining, 0)));
    CHECK(part.kind == CC_MEDIA_TEXT && remaining == 32);
    cc_content_part_cleanup(&part);
    impl->fs.vtable = original;
    impl->max_base64_bytes = 1024;
    CHECK(success(original->remove(impl->fs.self, path)));
    free(path);
    free(dir);
    return 0;
}

/** @brief 验证正文精确上限可读取，超限记录写前拒绝。
 * @param[in] store 独占后端借用。
 * @return 全部通过零，否则一，候选树/正文内部清理，拒绝后保留精确上限首行。
 */
static int check_line_limit(cc_session_store_t *store)
{
    CHECK(success(store->vtable->clear_session(store->self, "session")));
    char *text = malloc(CC_SEGMENTED_MAX_LINE_READ + 1U);
    CHECK(text);
    memset(text, 'x', CC_SEGMENTED_MAX_LINE_READ);
    text[CC_SEGMENTED_MAX_LINE_READ] = '\0';
    size_t overhead = strlen("{\"oversized\":\"\"}");
    text[CC_SEGMENTED_MAX_LINE_READ - overhead] = '\0';
    cc_json_value_t *record = cc_json_create_object();
    CHECK(record && cc_json_object_set_owned(record, "oversized", cc_json_create_string(text)));
    char *dir = session_dir_path(store->self, "session");
    CHECK(dir);
    CHECK(success(append_record_line(store->self, dir, "session", record)));
    char *path = first_segment(store);
    CHECK(path);
    string_vec_t lines = {0};
    CHECK(success(load_segment_lines(store->self, path, &lines)));
    CHECK(lines.count == 1 && strlen(lines.items[0]) == CC_SEGMENTED_MAX_LINE_READ);
    string_vec_cleanup(&lines);
    free(path);
    text[CC_SEGMENTED_MAX_LINE_READ - overhead] = 'x';
    CHECK(cc_json_object_set_owned(record, "oversized", cc_json_create_string(text)));
    free(text);
    cc_result_t rc = append_record_line(store->self, dir, "session", record);
    CHECK(rc.code == CC_ERR_LIMIT_EXCEEDED);
    cc_result_free(&rc);
    cc_json_destroy(record);
    free(dir);
    char *actual = read_segment(store);
    CHECK(actual && strlen(actual) == CC_SEGMENTED_MAX_LINE_READ + 1U);
    free(actual);
    return 0;
}

/** @brief 执行分配点矩阵、媒体预算和读取上限回归。
 * @return 全部通过零，否则一，成功恢复hook并清理后端/输入/文件，失败交进程回收。
 */
int main(void)
{
    const char *root = "runtime/data/test_segmented_failures";
    cc_session_store_t store = {0};
    CHECK(success(cc_json_segmented_store_create(root, 16U * 1024U * 1024U, "media", 1024, &store)));
    CHECK(success(store.vtable->create_session(store.self, "session", ".")));
    cc_message_t *message = NULL;
    CHECK(success(cc_message_create_text("message", "session", CC_ROLE_TOOL, "body", "call", &message)));
    message->reasoning_content = cc_copy_string("reasoning");
    message->created_at = cc_copy_string("2026-11-15T00:00:00Z");
    message->tool_outcome = CC_MESSAGE_TOOL_OUTCOME_UNKNOWN;
    CHECK(message->reasoning_content && message->created_at);
    cc_tool_call_t call = {.id = "call", .name = "tool", .arguments_json = "{}"};
    cc_tool_result_t result = {.ok = 1, .text = "result", .error = "error", .metadata = "metadata"};
    cc_session_record_t records[] = {
        {.type = CC_SESSION_RECORD_MESSAGE, .session_id = "session", .data.message = message},
        {.type = CC_SESSION_RECORD_TOOL_CALL, .session_id = "session", .data.tool_call = &call},
        {.type = CC_SESSION_RECORD_TOOL_RESULT, .session_id = "session", .data.tool_result = {.tool_call_id = "call", .result = &result}}
    };
    cJSON_Hooks hooks = {.malloc_fn = fault_allocate, .free_fn = free};
    cJSON_InitHooks(&hooks);
    for (unsigned operation = 0; operation < 4; operation++) CHECK(check_allocations(&store, records, operation) == 0);
    cJSON_InitHooks(NULL);
    CHECK(check_media_faults(&store) == 0);
    CHECK(check_rollover_failures(&store, &records[0]) == 0);
    CHECK(check_line_limit(&store) == 0);
    string_vec_t strings = {.count = SIZE_MAX, .capacity = SIZE_MAX};
    cc_result_t rc = string_vec_append_take(&strings, "borrowed-on-failure");
    CHECK(rc.code == CC_ERR_LIMIT_EXCEEDED && !strings.items);
    cc_result_free(&rc);
    message_vec_t messages = {.count = SIZE_MAX, .capacity = SIZE_MAX};
    cc_message_t empty = {0};
    rc = message_vec_append_take(&messages, &empty);
    CHECK(rc.code == CC_ERR_LIMIT_EXCEEDED && !messages.items);
    cc_result_free(&rc);
    cc_message_destroy(message);
    CHECK(success(remove_tree(store.self, root)));
    store.vtable->destroy(store.self);
    return 0;
}
