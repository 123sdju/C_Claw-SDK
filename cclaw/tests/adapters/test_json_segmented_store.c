#include "cc/core/cc_media.h"
#include "cc/adapters/cc_json_segmented_history.h"
#include "cc/core/cc_message.h"
#include "cc/core/cc_tool_call.h"
#include "cc/ports/cc_platform.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_session_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CC_PLATFORM == CC_PLATFORM_WINDOWS
#include <direct.h>
#include <sys/stat.h>
#include <windows.h>
#define cc_mkdir(path) _mkdir(path)
#define cc_rmdir(path) _rmdir(path)
#define cc_stat _stat
typedef struct _stat cc_stat_t;
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define cc_mkdir(path) mkdir(path, 0755)
#define cc_rmdir(path) rmdir(path)
#define cc_stat stat
typedef struct stat cc_stat_t;
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFDIR) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & S_IFREG) == S_IFREG)
#endif

/** @brief 创建本测试独占的分段后端。
 * @param[in] root_path 根路径借用，NULL 使用默认。
 * @param[in] segment_bytes 软分段阈值，零使用默认。
 * @param[in] media_dir 安全相对目录借用，NULL/空使用默认。
 * @param[in] max_base64_bytes 媒体回填限制，零无限制。
 * @param[out] out_store 必填新槽，成功交付最终 destroy 的端口。
 * @return 创建结果，失败清候选，测试串行调用。
 */
extern cc_result_t cc_json_segmented_store_create(
    const char *root_path,
    size_t segment_bytes,
    const char *media_dir,
    size_t max_base64_bytes,
    cc_session_store_t *out_store
);

/** @brief 用宿主 stat 确认目录。
 * @param[in] path 可为 NULL 的调用期路径借用。
 * @return 确认目录为一，否则零，无分配，测试独占路径。
 */
static int path_is_dir(const char *path)
{
    cc_stat_t st;
    return path && cc_stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/** @brief 用宿主 stat 确认普通文件。
 * @param[in] path 可为 NULL 的调用期路径借用。
 * @return 确认文件为一，否则零，无分配，测试独占路径。
 */
static int path_is_file(const char *path)
{
    cc_stat_t st;
    return path && cc_stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/** @brief 拼接本测试的两个短路径片段。
 * @param[in] a 必填稳定父路径借用。
 * @param[in] b 必填稳定后缀借用。
 * @return 调用者 free 的字符串，分配失败 NULL，无共享状态。
 */
static char *join_path(const char *a, const char *b)
{
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    char *out = malloc(alen + blen + 2);
    if (!out) return NULL;
    memcpy(out, a, alen);
    out[alen] = '/';
    memcpy(out + alen + 1, b, blen);
    out[alen + blen + 1] = '\0';
    return out;
}

/** @brief 尽力清理本测试独占的目录树。
 * @param[in] path 可为 NULL 的测试路径借用，调用者限定测试目录范围。
 * @note 分配、枚举和删除失败继续尽力清理，临时路径/枚举句柄内部释放，串行调用。
 */
static void remove_tree_best_effort(const char *path)
{
    if (!path) return;
#if CC_PLATFORM == CC_PLATFORM_WINDOWS
    char *pattern = join_path(path, "*");
    if (pattern) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
                char *child = join_path(path, fd.cFileName);
                if (!child) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    remove_tree_best_effort(child);
                } else {
                    remove(child);
                }
                free(child);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        free(pattern);
    }
#else
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            char *child = join_path(path, ent->d_name);
            if (!child) continue;
            if (path_is_dir(child)) remove_tree_best_effort(child);
            else remove(child);
            free(child);
        }
        closedir(dir);
    }
#endif
    cc_rmdir(path);
}

/** @brief 覆盖写入测试文本并检查关闭结果。
 * @param[in] path 必填独占测试路径借用。
 * @param[in] data 必填 NUL 正文借用。
 * @return 写入及关闭成功为一，否则零，FILE 在返回前关闭。
 */
static int write_file(const char *path, const char *data)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t len = strlen(data);
    int ok = fwrite(data, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    return ok;
}

/** @brief 取得测试根下遇到的首个会话目录。
 * @param[in] root 必填测试根路径借用，调用时目录树稳定。
 * @return 调用者 free 的路径，枚举/分配失败或未找到为 NULL，内部关闭枚举句柄。
 */
static char *find_first_session_dir(const char *root)
{
#if CC_PLATFORM == CC_PLATFORM_WINDOWS
    char *pattern = join_path(root, "*");
    if (!pattern) return NULL;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char *dir = join_path(root, fd.cFileName);
        FindClose(h);
        return dir;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return NULL;
#else
    DIR *dir = opendir(root);
    if (!dir) return NULL;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char *child = join_path(root, ent->d_name);
        if (child && path_is_dir(child)) {
            closedir(dir);
            return child;
        }
        free(child);
    }
    closedir(dir);
    return NULL;
#endif
}

/** @brief 统计测试目录的文件数。
 * @param[in] path 必填稳定目录借用。
 * @return 可枚举文件数，失败兼容零或已计数前缀，路径和枚举句柄内部释放。
 */
static int count_files_in_dir(const char *path)
{
    int count = 0;
#if CC_PLATFORM == CC_PLATFORM_WINDOWS
    char *pattern = join_path(path, "*");
    if (!pattern) return 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *dir = opendir(path);
    if (!dir) return 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char *child = join_path(path, ent->d_name);
        if (child && path_is_file(child)) count++;
        free(child);
    }
    closedir(dir);
#endif
    return count;
}

/** @brief 在前 31 个连续分段的 512 字节读取块中查找文本。
 * @param[in] session_dir 必填稳定会话目录借用。
 * @param[in] needle 必填片段借用，测试所查片段须落在同一读取块。
 * @return 找到为一，未找到或读/分配失败为零，内部关闭 FILE 并释放路径。
 */
static int segment_contains(const char *session_dir, const char *needle)
{
    for (int i = 1; i < 32; i++) {
        char name[32];
        snprintf(name, sizeof(name), "events-%06d.jsonl", i);
        char *path = join_path(session_dir, name);
        if (!path) return 0;
        if (!path_is_file(path)) {
            free(path);
            break;
        }
        FILE *f = fopen(path, "rb");
        free(path);
        if (!f) return 0;
        char buf[512];
        while (fgets(buf, sizeof(buf), f)) {
            if (strstr(buf, needle)) {
                fclose(f);
                return 1;
            }
        }
        fclose(f);
    }
    return 0;
}

/** @brief 统计前 31 个连续存在的分段。
 * @param[in] session_dir 必填稳定目录借用。
 * @return 首个缺失/分配失败前的分段数，临时路径内部释放。
 */
static int segment_file_count(const char *session_dir)
{
    int count = 0;
    for (int i = 1; i < 32; i++) {
        char name[32];
        snprintf(name, sizeof(name), "events-%06d.jsonl", i);
        char *path = join_path(session_dir, name);
        if (!path) return count;
        int exists = path_is_file(path);
        free(path);
        if (!exists) break;
        count++;
    }
    return count;
}

/** @brief 消费消息数组及全部字段。
 * @param[in,out] messages 拥有数组，count 为零可为 NULL，读者已退出。
 * @param[in] count 有效条数，调用者变量自行失效。
 */
static void cleanup_messages(cc_message_t *messages, size_t count)
{
    for (size_t i = 0; i < count; i++) cc_message_cleanup(&messages[i]);
    free(messages);
}

/** @brief 消费会话数组及所有复制字段。
 * @param[in,out] sessions 拥有数组，count 为零可为 NULL，读者已退出。
 * @param[in] count 有效条数，调用者变量自行失效。
 */
static void cleanup_sessions(cc_session_t *sessions, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(sessions[i].id);
        free(sessions[i].name);
        free(sessions[i].workspace_dir);
        free(sessions[i].model);
        free(sessions[i].created_at);
        free(sessions[i].updated_at);
    }
    free(sessions);
}

/** @brief 注入损坏记录，断言历史读取失败且既有工具禁用记录保留。
 * @param[in] store 必填存活后端借用，测试独占其文件。
 * @param[in] session_id 必填会话名借用。
 * @param[in] path 必填被替换分段路径借用，返回前尝试恢复原字节。
 * @return 全部场景通过为一，否则零，内部清理消息、账本、文件系统及原始副本。
 */
static int check_rejects_damaged_records(cc_session_store_t *store, const char *session_id,
                                         const char *path)
{
    static const char nul_record[] = "{\"type\":\"tool_call\"}\0hidden\n";
    static const char *cases[] = {
        "\n",
        "{\"type\":\"mystery\"}\n",
        "{\"type\":\"tool_call\",\"records_json\":\"[]\"}\n",
        "{\"type\":\"tool_call\",\"tool_call\":{\"session_id\":\"segmented_session\"},\"records_json\":\"[]\"}\n",
        "{\"type\":\"tool_call\",\"tool_call\":{\"session_id\":\"segmented_session\"},\"message\":{}}\n",
        "{\"type\":\"tool_call\",\"tool_call\":{\"session_id\":\"segmented_session\"}}\n",
        "{\"type\":\"tool_call\",\"tool_call\":{\"id\":\"a\",\"session_id\":\"segmented_session\",\"name\":\"write\",\"arguments_json\":\"{}\"}}\n",
        "{\"type\":\"tool_call\",\"tool_call\":{\"id\":\"a\",\"session_id\":\"segmented_session\",\"name\":\"write\",\"NAME\":\"read\",\"arguments_json\":\"{}\",\"status\":\"completed\"}}\n",
        "{\"type\":\"tool_result\",\"tool_result\":{\"id\":\"a\",\"session_id\":\"segmented_session\",\"tool_call_id\":\"b\",\"ok\":true,\"text\":\"\",\"error\":\"\",\"metadata\":\"\",\"artifacts\":\"[]\"}}\n",
        "{\"type\":\"tool_result\",\"tool_result\":{\"id\":\"a\",\"session_id\":\"segmented_session\",\"tool_call_id\":\"a\",\"ok\":true,\"text\":\"\",\"error\":\"\",\"metadata\":\"\",\"artifacts\":\"{}\"}}\n",
        "{\"type\":\"batch\",\"schema_version\":2,\"length\":42,\"checksum\":\"81222dffeb6ca167\",\"records_json\":\"[{\\\"type\\\":\\\"tool_call\\\",\\\"records_json\\\":\\\"[]\\\"}]\"}\n",
        "{\"type\":\"tool_result\",\"tool_result\":{\"session_id\":\"another-session\"}}\n",
        "{\"type\":\"message\",\"message\":{\"id\":\"x\",\"session_id\":\"another-session\",\"role\":\"assistant\",\"content_parts\":\"[]\",\"tool_calls\":\"[]\"}}\n",
        "{\"type\":\"message\",\"message\":{\"id\":\"x\",\"session_id\":\"segmented_session\",\"role\":\"assistnt\",\"content_parts\":\"[]\",\"tool_calls\":\"[]\"}}\n",
        "{\"type\":\"message\",\"message\":{\"id\":\"x\",\"session_id\":\"segmented_session\",\"role\":\"assistant\",\"content_parts\":\"[]\"}}\n",
        "{\"type\":\"message\",\"message\":{\"id\":\"x\",\"session_id\":\"segmented_session\",\"role\":\"assistant\",\"content_parts\":\"[null]\",\"tool_calls\":\"[]\"}}\n",
        "{\"type\":\"message\",\"message\":{\"id\":\"x\",\"session_id\":\"segmented_session\",\"role\":\"user\",\"content_parts\":\"[{\\\"type\\\":\\\"file\\\",\\\"artifact\\\":{\\\"kind\\\":\\\"file\\\",\\\"bytes\\\":1e99,\\\"data_base64\\\":\\\"Zg==\\\"}}]\",\"tool_calls\":\"[]\"}}\n",
        "{\"type\":\"message\",\"message\":{\"id\":\"x\",\"session_id\":\"segmented_session\",\"role\":\"user\",\"content_parts\":\"[{\\\"type\\\":\\\"image\\\",\\\"artifact\\\":{\\\"kind\\\":\\\"audio\\\",\\\"data_base64\\\":\\\"Zg==\\\"}}]\",\"tool_calls\":\"[]\"}}\n",
        "{\"type\":\"message\",\"message\":{\"id\":\"x\",\"session_id\":\"segmented_session\",\"role\":\"assistant\",\"content_parts\":\"[]\",\"tool_calls\":\"[{}]\"}}\n",
        "{\"type\":\"message\",\"type\":\"tool_call\",\"message\":{}}\n",
        "{\"type\":\"message\",\"TYPE\":\"tool_call\",\"message\":{}}\n",
        "{\"type\":\"message\",\"message\":{\"id\":\"x\\u0000hidden\",\"session_id\":\"segmented_session\",\"role\":\"assistant\",\"content_parts\":\"[]\",\"tool_calls\":\"[]\"}}\n"
    };
    cc_filesystem_t fs = {0};
    cc_result_t rc = cc_filesystem_get_default(&fs);
    if (rc.code != CC_OK) { cc_result_free(&rc); return 0; }
    const char *journal_path = "runtime/data/test_segmented_corruption_operations";
    remove_tree_best_effort(journal_path);
    rc = fs.vtable->make_dir(fs.self, journal_path);
    if (rc.code != CC_OK) { cc_result_free(&rc); fs.vtable->destroy(fs.self); return 0; }
    cc_operation_journal_t *journal = NULL;
    rc = cc_operation_journal_open(&fs, journal_path, 64, &journal);
    if (rc.code != CC_OK) { cc_result_free(&rc); fs.vtable->destroy(fs.self); return 0; }
    cc_tool_call_t pending = {"damaged-call", "write", "{}"};
    char *operation_id = NULL;
    rc = cc_operation_journal_import_unknown(journal, session_id, &pending, &operation_id);
    if (rc.code != CC_OK) { cc_result_free(&rc); cc_operation_journal_destroy(journal); fs.vtable->destroy(fs.self); return 0; }
    cc_result_free(&rc);
    void *original = NULL;
    size_t original_size = 0;
    rc = fs.vtable->read_bytes(fs.self, path, &original, &original_size);
    int good = rc.code == CC_OK;
    cc_result_free(&rc);
    if (!good) {
        free(operation_id);
        cc_operation_journal_destroy(journal);
        fs.vtable->destroy(fs.self);
        remove_tree_best_effort(journal_path);
        return 0;
    }
    for (size_t i = 0; good && i <= sizeof(cases) / sizeof(cases[0]); i++) {
        const void *data = i == 0 ? (const void *)nul_record : (const void *)cases[i - 1U];
        size_t size = i == 0 ? sizeof(nul_record) - 1U : strlen(cases[i - 1U]);
        rc = fs.vtable->write_bytes(fs.self, path, data, size);
        good = rc.code == CC_OK;
        cc_result_free(&rc);
        if (!good) break;
        cc_message_t *messages = NULL;
        size_t count = 0;
        int more = 0;
        rc = cc_json_segmented_store_load_page(store, session_id, NULL, 2, &messages, &count, &more);
        good = rc.code != CC_OK && !messages && count == 0;
        cc_result_free(&rc);
        cleanup_messages(messages, count);
        if (good) {
            size_t unresolved = 0;
            rc = cc_json_segmented_store_import_interrupted_calls(store, journal, &unresolved);
            good = rc.code != CC_OK;
            cc_result_free(&rc);
            int blocked = 0;
            rc = cc_operation_journal_is_blocked(journal, "write", &blocked);
            good = good && rc.code == CC_OK && blocked;
            cc_result_free(&rc);
        }
        if (!good) fprintf(stderr, "Damaged record case %zu was accepted\n", i);
    }
    rc = fs.vtable->write_bytes(fs.self, path, original, original_size);
    if (rc.code != CC_OK) good = 0;
    cc_result_free(&rc);
    free(original);
    free(operation_id);
    cc_operation_journal_destroy(journal);
    fs.vtable->destroy(fs.self);
    remove_tree_best_effort(journal_path);
    return good;
}

/** @brief 断言坏清单同时阻止分页、列举和追加。
 * @param[in] store 存活测试后端借用。
 * @param[in] session_id 必填会话名借用。
 * @param[in] record 必填测试追加记录借用。
 * @return 三入口均拒绝为一，否则零，输出和诊断内部回收，串行调用。
 */
static int manifest_rejected(cc_session_store_t *store, const char *session_id,
                             const cc_session_record_t *record)
{
    cc_message_t *messages = NULL; size_t count = 0; int more = 0;
    cc_result_t rc = cc_json_segmented_store_load_page(store, session_id, NULL, 2, &messages, &count, &more);
    int good = rc.code != CC_OK && !messages && count == 0;
    cc_result_free(&rc); cleanup_messages(messages, count);
    cc_session_t *sessions = NULL; size_t session_count = 0;
    rc = store->vtable->list_sessions(store->self, &sessions, &session_count);
    good = good && rc.code != CC_OK && !sessions && session_count == 0;
    cc_result_free(&rc); cleanup_sessions(sessions, session_count);
    rc = store->vtable->append_records(store->self, record, 1);
    good = good && rc.code != CC_OK;
    cc_result_free(&rc);
    return good;
}

/** @brief 注入重复字段、NUL、结构错误及恢复旁路文件。
 * @param[in] store 存活后端借用，测试独占清单。
 * @param[in] session_id 必填会话名借用。
 * @param[in] session_dir 必填目录借用。
 * @param[in] record 必填追加记录借用。
 * @return 全部拒绝且旁路证据保留为一，否则零，恢复原文并清理临时资源。
 */
static int check_damaged_manifest(cc_session_store_t *store, const char *session_id,
                                  const char *session_dir, const cc_session_record_t *record)
{
    cc_filesystem_t fs = {0};
    cc_result_t rc = cc_filesystem_get_default(&fs);
    if (rc.code != CC_OK) { cc_result_free(&rc); return 0; }
    char *path = join_path(session_dir, "manifest.json");
    char *tmp = join_path(session_dir, "manifest.json.tmp");
    char *bak = join_path(session_dir, "manifest.json.bak");
    void *original = NULL; size_t original_size = 0;
    int good = path && tmp && bak;
    if (good) {
        rc = fs.vtable->read_bytes(fs.self, path, &original, &original_size);
        good = rc.code == CC_OK; cc_result_free(&rc);
    }
    if (!good) goto done;
    const char *broken[] = {
        "{\"id\":\"segmented_session\",\"ID\":\"different\",\"current_segment\":1}",
        "{\"id\":\"segmented_session\",\"current_segment\":1,\"CURRENT_SEGMENT\":2}",
        "{\"id\":\"segmented_session\\u0000hidden\",\"current_segment\":1}",
        "{\"id\":\"segmented_session\",\"current_segment\":2,\"segments\":[]}",
        "{\"id\":\"segmented_session\",\"current_segment\":1,\"name\":\"a\",\"NAME\":\"b\"}",
        "{\"id\":\"segmented_session\",\"current_segment\":1,\"model\":42}"
    };
    for (size_t i = 0; good && i < sizeof(broken)/sizeof(broken[0]); i++) {
        rc = fs.vtable->write_text(fs.self, path, broken[i]);
        good = rc.code == CC_OK; cc_result_free(&rc);
        if (good) good = manifest_rejected(store, session_id, record);
    }
    if (good) {
        char *with_nul = malloc(original_size + 8U);
        if (!with_nul) good = 0;
        else {
            memcpy(with_nul, original, original_size);
            memcpy(with_nul + original_size, "\0hidden", 7U);
            rc = fs.vtable->write_bytes(fs.self, path, with_nul, original_size + 7U);
            good = rc.code == CC_OK; cc_result_free(&rc); free(with_nul);
            if (good) good = manifest_rejected(store, session_id, record);
        }
    }
    rc = fs.vtable->write_bytes(fs.self, path, original, original_size);
    if (rc.code != CC_OK) good = 0;
    cc_result_free(&rc);
    const char *const sidecars[] = {tmp, bak};
    for (size_t i = 0; good && i < 2U; i++) {
        rc = fs.vtable->write_text(fs.self, sidecars[i], "recoverable manifest");
        good = rc.code == CC_OK; cc_result_free(&rc);
        if (good) good = manifest_rejected(store, session_id, record);
        char *kept = NULL;
        rc = fs.vtable->read_text(fs.self, sidecars[i], &kept);
        good = good && rc.code == CC_OK && kept && strcmp(kept, "recoverable manifest") == 0;
        cc_result_free(&rc); free(kept);
        rc = fs.vtable->remove(fs.self, sidecars[i]);
        if (rc.code != CC_OK) good = 0;
        cc_result_free(&rc);
    }
done:
    free(original); free(path); free(tmp); free(bak);
    fs.vtable->destroy(fs.self);
    return good;
}

/** @brief 验证空紧邻下一段可复用，非空未列出分段阻止读写。
 * @return 全部断言通过为一，否则零，内部回收测试实例、消息、路径及目录，串行运行。
 */
static int check_orphan_segments(void)
{
    const char *root = "runtime/data/test_json_orphan_segment";
    remove_tree_best_effort(root);
    cc_session_store_t store = {0};
    cc_result_t rc = cc_json_segmented_store_create(root, 1, "media", 1024, &store);
    if (rc.code != CC_OK) { cc_result_free(&rc); return 0; }
    rc = store.vtable->create_session(store.self, "orphan", ".");
    if (rc.code != CC_OK) { cc_result_free(&rc); store.vtable->destroy(store.self); return 0; }
    cc_message_t *first = NULL, *second = NULL;
    rc = cc_message_create_text("first", "orphan", CC_ROLE_USER, "first", NULL, &first);
    if (rc.code == CC_OK) rc = cc_message_create_text("second", "orphan", CC_ROLE_USER, "second", NULL, &second);
    cc_session_record_t record = {.type = CC_SESSION_RECORD_MESSAGE, .session_id = "orphan", .data.message = first};
    if (rc.code == CC_OK) rc = store.vtable->append_records(store.self, &record, 1);
    int good = rc.code == CC_OK; cc_result_free(&rc);
    char *dir = good ? find_first_session_dir(root) : NULL;
    char *next = dir ? join_path(dir, "events-000002.jsonl") : NULL;
    char *later = dir ? join_path(dir, "events-000003.jsonl") : NULL;
    if (!dir || !next || !later || !write_file(next, "")) good = 0;
    cc_message_t *loaded = NULL; size_t count = 0; int more = 0;
    if (good) {
        rc = cc_json_segmented_store_load_page(&store, "orphan", NULL, 8, &loaded, &count, &more);
        good = rc.code == CC_OK && count == 1 && !more;
        cc_result_free(&rc); cleanup_messages(loaded, count); loaded = NULL; count = 0;
    }
    record.data.message = second;
    if (good) {
        rc = store.vtable->append_records(store.self, &record, 1);
        good = rc.code == CC_OK; cc_result_free(&rc);
    }
    if (good) {
        rc = cc_json_segmented_store_load_page(&store, "orphan", NULL, 8, &loaded, &count, &more);
        good = rc.code == CC_OK && count == 2 && !more &&
               strcmp(loaded[0].id, "first") == 0 && strcmp(loaded[1].id, "second") == 0;
        cc_result_free(&rc); cleanup_messages(loaded, count); loaded = NULL; count = 0;
    }
    if (good && !write_file(later, "{\"type\":\"batch\"}\n")) good = 0;
    if (good) {
        rc = cc_json_segmented_store_load_page(&store, "orphan", NULL, 8, &loaded, &count, &more);
        good = rc.code == CC_ERR_STORAGE && !loaded && count == 0;
        cc_result_free(&rc);
        rc = store.vtable->append_records(store.self, &record, 1);
        good = good && rc.code == CC_ERR_STORAGE; cc_result_free(&rc);
        FILE *f = fopen(later, "rb");
        if (!f) good = 0;
        else { good = good && fgetc(f) == '{'; fclose(f); }
    }
    free(dir); free(next); free(later);
    cc_message_destroy(first); cc_message_destroy(second);
    store.vtable->destroy(store.self); remove_tree_best_effort(root);
    return good;
}

/** @brief 比较测试小文件与预期文本的完整字节。
 * @param[in] path 必填独占路径借用。
 * @param[in] expected 必填最多 32 字节文本借用。
 * @return 长度及字节相同为一，否则零，内部关闭 FILE。
 */
static int file_equals(const char *path, const char *expected)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t length = strlen(expected);
    char buffer[32];
    size_t read = fread(buffer, 1, sizeof(buffer), f);
    int good = !ferror(f) && length == read && memcmp(buffer, expected, length) == 0;
    fclose(f);
    return good;
}

/** @brief 验证媒体内容匹配、遗留副本保留、路径约束和上下文预算。
 * @return 全部断言通过为一，否则零，统一退出清理消息、内容、路径、实例及目录。
 * @note 本测试独占目录，模拟完整和部分暂存后重试，当前用户超额报错，旧附件改描述。
 */
static int check_media_recovery(void)
{
    const char *root = "runtime/data/test_json_media_recovery";
    const char *sid = "media-recovery";
    remove_tree_best_effort(root);
    cc_session_store_t store = {0}, invalid_store = {0};
    cc_message_t *message = NULL, *reference = NULL, *unsafe = NULL;
    cc_message_t *loaded = NULL; size_t count = 0;
    cc_content_parts_t parts, reference_parts, unsafe_parts;
    cc_content_parts_init(&parts); cc_content_parts_init(&reference_parts); cc_content_parts_init(&unsafe_parts);
    char *dir = NULL, *path = NULL, *tmp = NULL, *bak = NULL, *relative = NULL;
    cc_result_t rc = cc_result_ok(); int good = 1;
#define MEDIA_CHECK(expr) do { if (!(expr)) { fprintf(stderr, "media check failed at %d\n", __LINE__); good = 0; goto done; } } while (0)
#define MEDIA_TRY(expr) do { rc = (expr); if (rc.code != CC_OK) { fprintf(stderr, "media error at %d: %s\n", __LINE__, rc.message ? rc.message : "error"); cc_result_free(&rc); good = 0; goto done; } cc_result_free(&rc); } while (0)
    MEDIA_TRY(cc_json_segmented_store_create(root, 1024, "media", 1024, &store));
    MEDIA_TRY(store.vtable->create_session(store.self, sid, "."));
    cc_media_artifact_t image = {.id="img", .kind=CC_MEDIA_IMAGE, .mime="image/png", .data_base64="aW1hZ2U="};
    MEDIA_TRY(cc_content_parts_append_artifact(&parts, &image, CC_CONTENT_PART_INPUT));
    MEDIA_TRY(cc_message_create_parts("source", sid, CC_ROLE_USER, &parts, NULL, &message));
    cc_session_record_t record = {.type=CC_SESSION_RECORD_MESSAGE, .session_id=sid, .data.message=message};
    MEDIA_TRY(store.vtable->append_records(store.self, &record, 1));
    MEDIA_TRY(store.vtable->load_messages(store.self, sid, -1, &loaded, &count));
    MEDIA_CHECK(count == 1 && loaded[0].content.count == 1 && loaded[0].content.items[0].artifact.path);
    const char *stored = loaded[0].content.items[0].artifact.path;
    relative = malloc(strlen(stored) + 1U); MEDIA_CHECK(relative);
    strcpy(relative, stored);
    cleanup_messages(loaded, count); loaded = NULL; count = 0;
    dir = find_first_session_dir(root); path = dir ? join_path(dir, relative) : NULL;
    tmp = path ? malloc(strlen(path) + 5U) : NULL;
    bak = path ? malloc(strlen(path) + 5U) : NULL;
    MEDIA_CHECK(dir && path && tmp && bak);
    sprintf(tmp, "%s.tmp", path); sprintf(bak, "%s.bak", path);
    MEDIA_CHECK(file_equals(path, "image"));

    /* 短哈希和文件名相同后仍要求实际字节相符。 */
    MEDIA_CHECK(write_file(path, "wrong"));
    rc = store.vtable->append_records(store.self, &record, 1);
    MEDIA_CHECK(rc.code == CC_ERR_STORAGE); cc_result_free(&rc);
    MEDIA_CHECK(file_equals(path, "wrong") && write_file(path, "image"));
    MEDIA_TRY(store.vtable->load_messages(store.self, sid, -1, &loaded, &count));
    MEDIA_CHECK(count == 1); cleanup_messages(loaded, count); loaded = NULL; count = 0;

    /* 完整暂存文件可继续安装。 */
    MEDIA_CHECK(rename(path, tmp) == 0);
    MEDIA_TRY(store.vtable->append_records(store.self, &record, 1));
    MEDIA_CHECK(file_equals(path, "image") && !path_is_file(tmp));
    MEDIA_TRY(store.vtable->load_messages(store.self, sid, -1, &loaded, &count));
    MEDIA_CHECK(count == 2); cleanup_messages(loaded, count); loaded = NULL; count = 0;

    /* 部分暂存原位保留，重试通过另一暂存文件安装完整字节。 */
    MEDIA_CHECK(remove(path) == 0 && write_file(tmp, "im"));
    MEDIA_TRY(store.vtable->append_records(store.self, &record, 1));
    MEDIA_CHECK(file_equals(tmp, "im") && file_equals(path, "image"));
    MEDIA_TRY(store.vtable->load_messages(store.self, sid, -1, &loaded, &count));
    MEDIA_CHECK(count == 3); cleanup_messages(loaded, count); loaded = NULL; count = 0;

    /* 正确目标已存在时也保留同长不同字节的暂存，夹具自行替换该暂存测试下一场景。 */
    MEDIA_CHECK(write_file(tmp, "wrong"));
    MEDIA_TRY(store.vtable->append_records(store.self, &record, 1));
    MEDIA_CHECK(file_equals(path, "image") && file_equals(tmp, "wrong"));
    MEDIA_CHECK(write_file(tmp, "image"));
    MEDIA_TRY(store.vtable->append_records(store.self, &record, 1));
    MEDIA_CHECK(file_equals(path, "image") && !path_is_file(tmp));

    MEDIA_CHECK(write_file(bak, "preserve"));
    rc = store.vtable->append_records(store.self, &record, 1);
    MEDIA_CHECK(rc.code == CC_ERR_STORAGE); cc_result_free(&rc);
    MEDIA_CHECK(file_equals(bak, "preserve") && file_equals(path, "image") && remove(bak) == 0);

    /* 相对引用与 inline 正文并存时先核对目标字节，再释放复制的 inline 数据。 */
    cc_media_artifact_t ref = {.id="ref", .kind=CC_MEDIA_IMAGE, .mime="image/png",
                               .path=relative, .data_base64="d3Jvbmc="};
    MEDIA_TRY(cc_content_parts_append_artifact(&reference_parts, &ref, CC_CONTENT_PART_INPUT));
    MEDIA_TRY(cc_message_create_parts("reference", sid, CC_ROLE_USER, &reference_parts, NULL, &reference));
    record.data.message = reference;
    rc = store.vtable->append_records(store.self, &record, 1);
    MEDIA_CHECK(rc.code == CC_ERR_STORAGE); cc_result_free(&rc);
    MEDIA_CHECK(file_equals(path, "image"));
    free(reference->content.items[0].artifact.data_base64);
    reference->content.items[0].artifact.data_base64 = malloc(sizeof("aW1hZ2U="));
    MEDIA_CHECK(reference->content.items[0].artifact.data_base64);
    strcpy(reference->content.items[0].artifact.data_base64, "aW1hZ2U=");
    MEDIA_CHECK(remove(path) == 0);
    MEDIA_TRY(store.vtable->append_records(store.self, &record, 1));
    MEDIA_CHECK(file_equals(path, "image"));
    MEDIA_TRY(store.vtable->load_messages(store.self, sid, -1, &loaded, &count));
    MEDIA_CHECK(count == 6 && loaded[5].content.items[0].artifact.data_base64 &&
                strcmp(loaded[5].content.items[0].artifact.data_base64, "aW1hZ2U=") == 0);
    cleanup_messages(loaded, count); loaded = NULL; count = 0;

    /* 上下文先读描述，优先为当前用户预留回填预算，再分配较旧附件。 */
    MEDIA_TRY(store.vtable->load_message_metadata(store.self, sid, -1, &loaded, &count));
    MEDIA_CHECK(count == 6);
    for (size_t i = 0; i < count; i++)
        MEDIA_CHECK(!loaded[i].content.items[0].artifact.data_base64);
    MEDIA_TRY(store.vtable->hydrate_context_media(store.self, sid, loaded, count, 5, 8));
    MEDIA_CHECK(loaded[5].content.items[0].artifact.data_base64 &&
        strcmp(loaded[5].content.items[0].artifact.data_base64, "aW1hZ2U=") == 0);
    for (size_t i = 0; i < 5; i++)
        MEDIA_CHECK(loaded[i].content.items[0].kind == CC_MEDIA_TEXT &&
            strstr(loaded[i].content.items[0].text, "memory budget"));
    cleanup_messages(loaded, count); loaded = NULL; count = 0;
    MEDIA_TRY(store.vtable->load_message_metadata(store.self, sid, -1, &loaded, &count));
    rc = store.vtable->hydrate_context_media(store.self, sid, loaded, count, 5, 7);
    MEDIA_CHECK(rc.code == CC_ERR_LIMIT_EXCEEDED && !loaded[5].content.items[0].artifact.data_base64);
    cc_result_free(&rc);
    cleanup_messages(loaded, count); loaded = NULL; count = 0;
    MEDIA_TRY(store.vtable->load_message_metadata(store.self, sid, -1, &loaded, &count));
    MEDIA_TRY(store.vtable->hydrate_context_media(store.self, sid, loaded, count, count, 8));
    MEDIA_CHECK(loaded[5].content.items[0].artifact.data_base64 &&
        loaded[4].content.items[0].kind == CC_MEDIA_TEXT);
    cleanup_messages(loaded, count); loaded = NULL; count = 0;
    MEDIA_TRY(store.vtable->load_message_metadata(store.self, sid, -1, &loaded, &count));
    MEDIA_TRY(store.vtable->hydrate_context_media(store.self, sid, loaded, count, 0, 16));
    MEDIA_CHECK(loaded[0].content.items[0].artifact.data_base64 &&
        loaded[5].content.items[0].artifact.data_base64 &&
        loaded[4].content.items[0].kind == CC_MEDIA_TEXT);
    cleanup_messages(loaded, count); loaded = NULL; count = 0;
    MEDIA_TRY(store.vtable->load_messages(store.self, sid, -1, &loaded, &count));
    MEDIA_CHECK(count == 6 && loaded[0].content.items[0].artifact.data_base64);
    cleanup_messages(loaded, count); loaded = NULL; count = 0;

    cc_media_artifact_t escaped = {.id="escape", .kind=CC_MEDIA_IMAGE, .mime="image/png",
                                   .path="media/../../outside.png", .data_base64="aW1hZ2U="};
    MEDIA_TRY(cc_content_parts_append_artifact(&unsafe_parts, &escaped, CC_CONTENT_PART_INPUT));
    MEDIA_TRY(cc_message_create_parts("unsafe", sid, CC_ROLE_USER, &unsafe_parts, NULL, &unsafe));
    record.data.message = unsafe;
    rc = store.vtable->append_records(store.self, &record, 1);
    MEDIA_CHECK(rc.code == CC_ERR_PERMISSION_DENIED); cc_result_free(&rc);
    rc = cc_json_segmented_store_create(root, 1024, "../escape", 1024, &invalid_store);
    MEDIA_CHECK(rc.code == CC_ERR_INVALID_ARGUMENT && !invalid_store.self); cc_result_free(&rc);
done:
    cleanup_messages(loaded, count);
    cc_message_destroy(message); cc_message_destroy(reference); cc_message_destroy(unsafe);
    cc_content_parts_cleanup(&parts); cc_content_parts_cleanup(&reference_parts); cc_content_parts_cleanup(&unsafe_parts);
    free(dir); free(path); free(tmp); free(bak); free(relative);
    if (store.vtable) store.vtable->destroy(store.self);
    if (invalid_store.vtable) invalid_store.vtable->destroy(invalid_store.self);
    remove_tree_best_effort(root);
#undef MEDIA_CHECK
#undef MEDIA_TRY
    return good;
}

/** @brief 串行验证分段、媒体、分页、清单元数据、损坏拒绝和显式历史重置。
 * @return 成功为零，失败返回定位码，成功路径释放全部对象及测试文件，早退交进程回收。
 */
int main(void)
{
    const char *root = "runtime/data/test_json_segmented_store";
    const char *session_id = "segmented_session";
    const char *video_src = "runtime/data/test_segmented_video.tmp";
    const char *file_src = "runtime/data/test_segmented_file.tmp";

    remove_tree_best_effort(root);
    remove(video_src);
    remove(file_src);
    cc_mkdir("runtime");
    cc_mkdir("runtime/data");
    if (!write_file(video_src, "video") || !write_file(file_src, "plain")) return 1;

    cc_session_store_t store = {0};
    cc_result_t rc = cc_json_segmented_store_create(root, 256, "media", 1024, &store);
    if (rc.code != CC_OK) return 2;
    rc = store.vtable->create_session(store.self, session_id, ".");
    if (rc.code != CC_OK) return 3;
    /* 滚动保留已有清单元数据及扩展字段，包含读取器接受的键名大小写。 */
    char *metadata_dir = find_first_session_dir(root);
    char *metadata_path = metadata_dir ? join_path(metadata_dir, "manifest.json") : NULL;
    const char *metadata_manifest =
        "{\"id\":\"segmented_session\",\"workspace_dir\":\".\",\"status\":\"completed\","
        "\"segment_bytes\":256,\"media_dir\":\"media\",\"CURRENT_SEGMENT\":1,"
        "\"SEGMENTS\":[\"events-000001.jsonl\"],\"name\":\"Project Alpha\",\"model\":\"model-a\","
        "\"created_at\":\"created\",\"updated_at\":\"updated\",\"custom_marker\":{\"value\":7}}";
    if (!metadata_path || !write_file(metadata_path, metadata_manifest)) return 54;
    free(metadata_path); free(metadata_dir);
    cc_message_t *wrong_session = NULL;
    rc = cc_message_create_text("wrong", "another-session", CC_ROLE_USER, "no", NULL, &wrong_session);
    if (rc.code != CC_OK) return 47;
    cc_session_record_t wrong_record = {
        .type = CC_SESSION_RECORD_MESSAGE, .session_id = session_id, .data.message = wrong_session
    };
    rc = store.vtable->append_records(store.self, &wrong_record, 1);
    cc_message_destroy(wrong_session);
    if (rc.code != CC_ERR_INVALID_ARGUMENT) return 48;
    cc_result_free(&rc);
    cc_tool_call_t invalid_call = {NULL, "write", "{}"};
    cc_session_record_t invalid_audit = {
        .type = CC_SESSION_RECORD_TOOL_CALL, .session_id = session_id, .data.tool_call = &invalid_call
    };
    rc = store.vtable->append_records(store.self, &invalid_audit, 1);
    if (rc.code != CC_ERR_INVALID_ARGUMENT) return 49;
    cc_result_free(&rc);
    invalid_audit.type = CC_SESSION_RECORD_TOOL_RESULT;
    invalid_audit.data.tool_result.tool_call_id = "call";
    invalid_audit.data.tool_result.result = NULL;
    rc = store.vtable->append_records(store.self, &invalid_audit, 1);
    if (rc.code != CC_ERR_INVALID_ARGUMENT) return 50;
    cc_result_free(&rc);

    char large_text[360];
    memset(large_text, 'x', sizeof(large_text) - 1);
    large_text[sizeof(large_text) - 1] = '\0';
    for (int i = 0; i < 4; i++) {
        char id[16];
        snprintf(id, sizeof(id), "m%d", i);
        cc_message_t *msg = NULL;
        rc = cc_message_create_text(id, session_id, CC_ROLE_USER,
                                    i == 0 ? "literal \\u0000" : large_text, NULL, &msg);
        if (rc.code != CC_OK || !msg) return 4;
        cc_session_record_t record = {
            .type = CC_SESSION_RECORD_MESSAGE,
            .session_id = session_id,
            .data.message = msg,
        };
        rc = store.vtable->append_records(store.self, &record, 1);
        cc_message_destroy(msg);
        if (rc.code != CC_OK) return 5;
    }

    cc_media_artifact_t video = {
        .id = "video_1",
        .kind = CC_MEDIA_VIDEO,
        .mime = "video/mp4",
        .path = (char *)video_src
    };
    cc_media_artifact_t file = {
        .id = "file_1",
        .kind = CC_MEDIA_FILE,
        .mime = "application/octet-stream",
        .path = (char *)file_src
    };
    cc_content_parts_t media_parts;
    cc_content_parts_init(&media_parts);
    rc = cc_content_parts_append_text(&media_parts, "media paths", CC_CONTENT_PART_INPUT);
    if (rc.code == CC_OK) rc = cc_content_parts_append_artifact(&media_parts, &video, CC_CONTENT_PART_INPUT);
    if (rc.code == CC_OK) rc = cc_content_parts_append_artifact(&media_parts, &file, CC_CONTENT_PART_INPUT);
    if (rc.code != CC_OK) return 6;
    cc_message_t *media_msg = NULL;
    rc = cc_message_create_parts("m_media", session_id, CC_ROLE_USER, &media_parts, NULL, &media_msg);
    if (rc.code != CC_OK || !media_msg) return 7;
    cc_session_record_t media_record = {
        .type = CC_SESSION_RECORD_MESSAGE,
        .session_id = session_id,
        .data.message = media_msg,
    };
    rc = store.vtable->append_records(store.self, &media_record, 1);
    if (rc.code != CC_OK) return 8;

    cc_media_artifact_t audio = {
        .id = "audio_1",
        .kind = CC_MEDIA_AUDIO,
        .mime = "audio/wav",
        .data_base64 = "YXVkaW8="
    };
    cc_tool_result_t tool_result = {0};
    tool_result.ok = 1;
    rc = cc_tool_result_add_artifact(&tool_result, &audio);
    if (rc.code != CC_OK) return 9;
    cc_tool_call_t audio_call = {"tool_audio", "listen", "{}"};
    cc_session_record_t audio_call_record = {
        .type = CC_SESSION_RECORD_TOOL_CALL, .session_id = session_id, .data.tool_call = &audio_call
    };
    rc = store.vtable->append_records(store.self, &audio_call_record, 1);
    if (rc.code != CC_OK) return 51;
    cc_session_record_t tool_record = {
        .type = CC_SESSION_RECORD_TOOL_RESULT,
        .session_id = session_id,
        .data.tool_result = {
            .tool_call_id = "tool_audio",
            .result = &tool_result,
        },
    };
    rc = store.vtable->append_records(store.self, &tool_record, 1);
    cc_tool_result_cleanup(&tool_result);
    if (rc.code != CC_OK) return 10;

    cc_media_artifact_t image = {
        .id = "image_1",
        .kind = CC_MEDIA_IMAGE,
        .mime = "image/png",
        .data_base64 = "aW1hZ2U=",
        .width = 2,
        .height = 3
    };
    cc_content_parts_t image_parts;
    cc_content_parts_init(&image_parts);
    rc = cc_content_parts_append_text(&image_parts, "image", CC_CONTENT_PART_INPUT);
    if (rc.code == CC_OK) rc = cc_content_parts_append_artifact(&image_parts, &image, CC_CONTENT_PART_INPUT);
    if (rc.code != CC_OK) return 11;
    cc_message_t *image_msg = NULL;
    rc = cc_message_create_parts("m_image", session_id, CC_ROLE_USER, &image_parts, NULL, &image_msg);
    if (rc.code != CC_OK || !image_msg) return 12;
    cc_session_record_t image_record = {
        .type = CC_SESSION_RECORD_MESSAGE,
        .session_id = session_id,
        .data.message = image_msg,
    };
    rc = store.vtable->append_records(store.self, &image_record, 1);
    if (rc.code != CC_OK) return 13;
    if (!image_msg->content.items[1].artifact.data_base64 ||
        strcmp(image_msg->content.items[1].artifact.data_base64, "aW1hZ2U=") != 0) {
        return 14;
    }

    char *session_dir = find_first_session_dir(root);
    if (!session_dir) return 15;
    if (segment_file_count(session_dir) < 2) return 16;
    if (segment_contains(session_dir, "data_base64")) return 17;
    if (!segment_contains(session_dir, "media/img/user") ||
        !segment_contains(session_dir, "media/audio/tool") ||
        segment_contains(session_dir, video_src) ||
        segment_contains(session_dir, file_src)) {
        return 17;
    }

    char *img_dir = join_path(session_dir, "media/img/user");
    char *audio_dir = join_path(session_dir, "media/audio/tool");
    char *video_dir = join_path(session_dir, "media/video/user");
    char *file_dir = join_path(session_dir, "media/file/user");
    if (!img_dir || !audio_dir || !video_dir || !file_dir) return 18;
    if (count_files_in_dir(img_dir) != 1 ||
        count_files_in_dir(audio_dir) != 1 ||
        count_files_in_dir(video_dir) != 1 ||
        count_files_in_dir(file_dir) != 1) {
        return 19;
    }

    cc_message_t *messages = NULL;
    size_t message_count = 0;
    rc = store.vtable->load_messages(store.self, session_id, 0, &messages, &message_count);
    if (rc.code != CC_OK || message_count != 6 ||
        strcmp(messages[0].id, "m0") != 0 ||
        strcmp(messages[0].content.items[0].text, "literal \\u0000") != 0 ||
        strcmp(messages[message_count - 1].id, "m_image") != 0) {
        return 20;
    }
    cleanup_messages(messages, message_count);

    int has_more = 0;
    messages = NULL; message_count = 0;
    rc = cc_json_segmented_store_load_page(&store, session_id, NULL, 2,
                                           &messages, &message_count, &has_more);
    if (rc.code != CC_OK || message_count != 2 || !has_more ||
        strcmp(messages[0].id, "m_media") || strcmp(messages[1].id, "m_image")) return 26;
    cleanup_messages(messages, message_count);
    rc = cc_json_segmented_store_load_page(&store, session_id, "m_media", 2,
                                           &messages, &message_count, &has_more);
    if (rc.code != CC_OK || message_count != 2 || !has_more ||
        strcmp(messages[0].id, "m2") || strcmp(messages[1].id, "m3")) return 27;
    cleanup_messages(messages, message_count);
    rc = cc_json_segmented_store_load_page(&store, session_id, "m2", 2,
                                           &messages, &message_count, &has_more);
    if (rc.code != CC_OK || message_count != 2 || has_more ||
        strcmp(messages[0].id, "m0") || strcmp(messages[1].id, "m1")) return 28;
    cleanup_messages(messages, message_count);
    rc = cc_json_segmented_store_load_page(&store, session_id, "missing", 2,
                                           &messages, &message_count, &has_more);
    if (rc.code != CC_ERR_NOT_FOUND) return 29;
    cc_result_free(&rc);

    messages = NULL;
    message_count = 0;
    rc = store.vtable->load_messages(store.self, session_id, 2, &messages, &message_count);
    if (rc.code != CC_OK || message_count != 2 ||
        strcmp(messages[0].id, "m_media") != 0 ||
        strcmp(messages[1].id, "m_image") != 0 ||
        !messages[1].content.items[1].artifact.data_base64 ||
        strcmp(messages[1].content.items[1].artifact.data_base64, "aW1hZ2U=") != 0) {
        return 21;
    }
    cleanup_messages(messages, message_count);

    cc_session_t *sessions = NULL;
    size_t session_count = 0;
    rc = store.vtable->list_sessions(store.self, &sessions, &session_count);
    if (rc.code != CC_OK || session_count != 1 || strcmp(sessions[0].id, session_id) != 0 ||
        strcmp(sessions[0].name, "Project Alpha") != 0 ||
        !sessions[0].model || strcmp(sessions[0].model, "model-a") != 0 ||
        sessions[0].status != CC_SESSION_COMPLETED || !sessions[0].created_at ||
        strcmp(sessions[0].created_at, "created") != 0 || !sessions[0].updated_at ||
        strcmp(sessions[0].updated_at, "updated") != 0) return 22;
    cleanup_sessions(sessions, session_count);
    char *current_manifest = join_path(session_dir, "manifest.json");
    cc_filesystem_t metadata_fs = {0}; char *saved_manifest = NULL;
    if (!current_manifest || cc_filesystem_get_default(&metadata_fs).code != CC_OK ||
        metadata_fs.vtable->read_text(metadata_fs.self, current_manifest, &saved_manifest).code != CC_OK ||
        !saved_manifest || !strstr(saved_manifest, "custom_marker") ||
        !strstr(saved_manifest, "CURRENT_SEGMENT") || !strstr(saved_manifest, "SEGMENTS")) return 55;
    free(saved_manifest); free(current_manifest); metadata_fs.vtable->destroy(metadata_fs.self);
    cc_session_store_t reopened = {0};
    rc = cc_json_segmented_store_create(root, 256, "media", 1024, &reopened);
    if (rc.code != CC_OK) return 56;
    sessions = NULL; session_count = 0;
    rc = reopened.vtable->list_sessions(reopened.self, &sessions, &session_count);
    if (rc.code != CC_OK || session_count != 1 || strcmp(sessions[0].name, "Project Alpha") != 0 ||
        !sessions[0].model || strcmp(sessions[0].model, "model-a") != 0 ||
        sessions[0].status != CC_SESSION_COMPLETED || !sessions[0].created_at ||
        strcmp(sessions[0].created_at, "created") != 0 || !sessions[0].updated_at ||
        strcmp(sessions[0].updated_at, "updated") != 0) return 57;
    cleanup_sessions(sessions, session_count); reopened.vtable->destroy(reopened.self);

    int last_segment = segment_file_count(session_dir);
    char segment_name[32];
    snprintf(segment_name, sizeof(segment_name), "events-%06d.jsonl", last_segment);
    char *last_path = join_path(session_dir, segment_name);
    if (!last_path) return 33;
    if (!check_rejects_damaged_records(&store, session_id, last_path)) return 46;
    if (!check_damaged_manifest(&store, session_id, session_dir, &image_record)) return 52;
    if (!check_orphan_segments()) return 53;
    if (!check_media_recovery()) return 59;
    FILE *partial = fopen(last_path, "ab");
    if (!partial || fputs("{\"type\":\"batch\"", partial) < 0 || fclose(partial) != 0) return 34;
    free(last_path);
    rc = cc_json_segmented_store_load_page(&store, session_id, NULL, 2,
                                           &messages, &message_count, &has_more);
    if (rc.code != CC_ERR_STORAGE) return 35;
    cc_result_free(&rc);
    rc = store.vtable->load_messages(store.self, session_id, 2, &messages, &message_count);
    if (rc.code != CC_ERR_STORAGE) return 36;
    cc_result_free(&rc);
    rc = store.vtable->append_records(store.self, &image_record, 1);
    if (rc.code != CC_ERR_STORAGE) return 40;
    cc_result_free(&rc);

    rc = store.vtable->clear_session(store.self, session_id);
    if (rc.code != CC_OK) return 23;
    if (count_files_in_dir(img_dir) != 0 || count_files_in_dir(audio_dir) != 0) return 24;
    messages = NULL;
    message_count = 0;
    rc = store.vtable->load_messages(store.self, session_id, 0, &messages, &message_count);
    if (rc.code != CC_OK || message_count != 0) return 25;
    cleanup_messages(messages, message_count);

    char *manifest = join_path(session_dir, "manifest.json");
    char *manifest_backup = join_path(session_dir, "manifest.json.bak");
    if (!manifest || !manifest_backup) return 30;
    cc_filesystem_t filesystem = {0};
    rc = cc_filesystem_get_default(&filesystem);
    if (rc.code != CC_OK) return 37;
    char *manifest_text = NULL;
    rc = filesystem.vtable->read_text(filesystem.self, manifest, &manifest_text);
    if (rc.code != CC_OK || !manifest_text || !write_file(manifest_backup, manifest_text) ||
        !write_file(manifest, "{broken")) return 38;
    free(manifest_text);
    filesystem.vtable->destroy(filesystem.self);
    sessions = NULL; session_count = 0;
    rc = store.vtable->list_sessions(store.self, &sessions, &session_count);
    if (rc.code == CC_OK) return 31;
    cc_result_free(&rc);
    rc = cc_json_segmented_store_load_page(&store, session_id, NULL, 2,
                                           &messages, &message_count, &has_more);
    if (rc.code == CC_OK) return 32;
    cc_result_free(&rc);
    rc = store.vtable->create_session(store.self, session_id, "workspace");
    if (rc.code == CC_OK) return 41;
    cc_result_free(&rc);
    rc = store.vtable->append_records(store.self, &image_record, 1);
    if (rc.code == CC_OK) return 42;
    cc_result_free(&rc);
    if (remove(manifest) != 0) return 43;
    rc = store.vtable->create_session(store.self, session_id, "workspace");
    if (rc.code == CC_OK) return 44;
    cc_result_free(&rc);
    rc = store.vtable->append_records(store.self, &image_record, 1);
    if (rc.code == CC_OK || path_is_file(manifest)) return 45;
    cc_result_free(&rc);
    if (!path_is_file(manifest_backup)) return 39;
    free(manifest_backup);
    free(manifest);

    free(img_dir);
    free(audio_dir);
    free(video_dir);
    free(file_dir);
    free(session_dir);
    cc_message_destroy(media_msg);
    cc_message_destroy(image_msg);
    cc_content_parts_cleanup(&media_parts);
    cc_content_parts_cleanup(&image_parts);
    store.vtable->destroy(store.self);
    remove_tree_best_effort(root);
    remove(video_src);
    remove(file_src);
    return 0;
}
