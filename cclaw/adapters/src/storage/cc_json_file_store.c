



#include "cc/adapters/cc_json_file_session_store.h"
#include "cc/ports/cc_thread.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_path.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc_json_record_validation.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef CC_DEFAULT_WORKSPACE_PATH
#define CC_DEFAULT_WORKSPACE_PATH "runtime/workspace"
#endif

#ifndef CC_DEFAULT_STORAGE_PATH
#define CC_DEFAULT_STORAGE_PATH "runtime/data/sessions.json"
#endif

/*
 * JSON 文件 session store 私有状态。
 *
 * file_path 是深拷贝路径，root 是当前加载的 JSON AST，mutex 保护 load/modify/save 的完整
 * 临界区。这个 adapter 适合桌面/嵌入式 Linux 小规模持久化，不适合高并发大数据量。
 */
typedef struct {
    char *file_path;
    cc_filesystem_t fs;
    cc_json_value_t *root;
    cc_mutex_t mutex;
    int write_indeterminate;
} cc_json_file_store_t;

/**
 * @brief 为排序和二分查找按字节比较两个会话 ID。
 * @param[in] a 指向非空 NUL 结尾字符串指针的只读槽。
 * @param[in] b 指向非空 NUL 结尾字符串指针的只读槽。
 * @return strcmp 的负数、零或正数比较结果，不分配、不转移所有权。
 */
static int compare_session_ids(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/**
 * @brief 检查会话文件旁的临时/备份文件，存在待核对副本时阻止自动读写。
 * @param[in] store 初始化中的或已持锁的后端借用，须提供路径与 exists 文件系统端口。
 * @return 无旁路文件时成功，其他为端口不支持、路径溢出、分配、读取或待核对错误。
 * @note 回收临时路径，不删除或选择恢复副本。调用者释放结果诊断并协调外部文件写入。
 */
static cc_result_t reject_session_sidecars(cc_json_file_store_t *store)
{
    if (!store || !store->file_path || !store->fs.vtable || !store->fs.vtable->exists)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Cannot inspect session recovery files");
    size_t length = strlen(store->file_path);
    if (length > SIZE_MAX - 5U)
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "JSON session path is too long");
    char *sidecar = malloc(length + 5U);
    if (!sidecar) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate session recovery path");
    cc_result_t rc = cc_result_ok();
    const char *const suffixes[] = {".tmp", ".bak"};
    for (size_t i = 0; i < 2U && rc.code == CC_OK; i++) {
        memcpy(sidecar, store->file_path, length);
        memcpy(sidecar + length, suffixes[i], 5U);
        int exists = 0;
        rc = store->fs.vtable->exists(store->fs.self, sidecar, &exists);
        if (rc.code == CC_OK && exists)
            rc = cc_result_error(CC_ERR_STORAGE, "Session recovery file requires inspection");
    }
    free(sidecar);
    return rc;
}

/**
 * @brief 核对会话 ID 唯一性、元数据类型以及消息/审计记录的会话归属。
 * @param[in] root 已解析的 JSON 根借用，检查期间保持稳定。
 * @return 有效时成功，或结构、重复 ID、孤立记录、分配错误，调用者释放诊断。
 * @note 分配借用 ID 的排序数组并在退出时释放，不修改 JSON 树和 ID 字符串。
 *       调用者持有后端互斥量，防止恢复枚举遗漏指向不存在会话的记录。
 */
static cc_result_t validate_stored_sessions(cc_json_value_t *root)
{
    cc_json_value_t *array = cc_json_object_get(root, "sessions");
    if (!cc_json_is_array(array)) return cc_result_error(CC_ERR_STORAGE, "Invalid JSON sessions array");
    int count = cc_json_array_size(array);
    const char **ids = count ? calloc((size_t)count, sizeof(*ids)) : NULL;
    if (count && !ids) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot validate JSON sessions");
    cc_result_t rc = cc_result_ok();
    const char *const optional[] = {"workspace_dir", "name", "model", "created_at", "updated_at", "status"};
    static const char *const session_fields[] = {"id", "workspace_dir", "name", "model", "created_at", "updated_at", "status"};
    int index = 0;
    for (const cc_json_value_t *session = cc_json_array_first(array); session && rc.code == CC_OK;
         session = cc_json_array_next(session), index++) {
        const char *id = cc_json_string_value(cc_json_object_get(session, "id"));
        if (!cc_storage_json_fields_unique(session, session_fields, 7) || !id || !id[0]) {
            rc = cc_result_error(CC_ERR_STORAGE, "Invalid JSON session id");
            break;
        }
        ids[index] = id;
        for (size_t j = 0; j < sizeof(optional) / sizeof(optional[0]); j++) {
            cc_json_value_t *value = cc_json_object_get(session, optional[j]);
            if (value && !cc_json_is_null(value) && !cc_json_is_string(value)) {
                rc = cc_result_error(CC_ERR_STORAGE, "Invalid JSON session metadata");
                break;
            }
        }
        const char *status = cc_json_string_value(cc_json_object_get(session, "status"));
        if (rc.code == CC_OK && status && strcmp(status, "active") != 0 &&
            strcmp(status, "completed") != 0 && strcmp(status, "error") != 0)
            rc = cc_result_error(CC_ERR_STORAGE, "Invalid JSON session status");
    }
    if (rc.code == CC_OK && count > 1) {
        qsort(ids, (size_t)count, sizeof(*ids), compare_session_ids);
        for (int i = 1; i < count; i++) {
            if (strcmp(ids[i - 1], ids[i]) == 0) {
                rc = cc_result_error(CC_ERR_STORAGE, "Duplicate JSON session id");
                break;
            }
        }
    }
    /* 会话枚举无法覆盖指向缺失会话的消息/审计记录，此处检测并报告孤立记录。 */
    const char *const arrays[] = {"messages", "tool_calls", "tool_results"};
    for (size_t a = 0; rc.code == CC_OK && a < 3U; a++) {
        cc_json_value_t *records = cc_json_object_get(root, arrays[a]);
        if (!records || !cc_json_is_array(records)) continue;
        for (const cc_json_value_t *record = cc_json_array_first(records); record;
             record = cc_json_array_next(record)) {
            const char *sid = cc_json_string_value(cc_json_object_get(record, "session_id"));
            if (!cc_json_is_object(record) || !sid || !sid[0] || count == 0 ||
                !bsearch(&sid, ids, (size_t)count, sizeof(*ids), compare_session_ids)) {
                rc = cc_result_error(CC_ERR_STORAGE, "JSON record refers to a missing session");
                break;
            }
        }
    }
    free(ids);
    return rc;
}

/**
 * @brief 检查全部当前格式消息的身份、角色、两个序列化数组及工具结果关联。
 * @param[in] root 受调用者保护的稳定 JSON 根借用，messages 已由上层验证为数组。
 * @return 全部消息有效时成功，其他为结构、数组解码、工具状态或资源错误。
 * @note 不修改树，数组校验可能分配临时解码数据，调用者消费结果诊断。
 */
static cc_result_t validate_stored_messages(cc_json_value_t *root)
{
    cc_json_value_t *array = cc_json_object_get(root, "messages");
    static const char *const fields[] = {"id", "session_id", "role", "content", "content_parts", "tool_calls",
                                         "reasoning_content", "tool_call_id", "tool_outcome", "created_at"};
    for (const cc_json_value_t *message = cc_json_array_first(array); message;
         message = cc_json_array_next(message)) {
        if (!cc_storage_json_fields_unique(message, fields, 10))
            return cc_result_error(CC_ERR_STORAGE, "Invalid JSON message fields");
        const char *id = cc_json_string_value(cc_json_object_get(message, "id"));
        const char *sid = cc_json_string_value(cc_json_object_get(message, "session_id"));
        const char *role = cc_json_string_value(cc_json_object_get(message, "role"));
        if (!id || !id[0] || !sid || !sid[0] || !role ||
            (strcmp(role, "system") && strcmp(role, "user") && strcmp(role, "assistant") && strcmp(role, "tool")))
            return cc_result_error(CC_ERR_STORAGE, "Invalid JSON message identity or role");
        cc_json_value_t *parts = cc_json_object_get(message, "content_parts");
        cc_json_value_t *calls = cc_json_object_get(message, "tool_calls");
        if (!parts || !calls)
            return cc_result_error(CC_ERR_STORAGE, "Incomplete JSON message arrays");
        const char *parts_json = cc_json_string_value(parts);
        const char *calls_json = cc_json_string_value(calls);
        if (!parts_json || !calls_json)
            return cc_result_error(CC_ERR_STORAGE, "Invalid JSON message arrays");
        cc_result_t rc = cc_storage_json_validate_array(parts_json, 0);
        if (rc.code != CC_OK) return rc;
        rc = cc_storage_json_validate_array(calls_json, 1);
        if (rc.code != CC_OK) return rc;
        const char *const optional[] = {"reasoning_content", "tool_call_id", "created_at"};
        for (size_t j = 0; j < 3; j++) {
            cc_json_value_t *value = cc_json_object_get(message, optional[j]);
            if (value && !cc_json_is_null(value) && !cc_json_is_string(value))
                return cc_result_error(CC_ERR_STORAGE, "Invalid JSON message metadata");
        }
        cc_json_value_t *outcome = cc_json_object_get(message, "tool_outcome");
        if (outcome) {
            cc_message_tool_outcome_t parsed;
            const char *text = cc_json_string_value(outcome);
            if (!text) return cc_result_error(CC_ERR_STORAGE, "Invalid JSON message tool outcome");
            cc_result_t rc = cc_message_tool_outcome_parse(text, &parsed);
            if (rc.code != CC_OK) return rc;
            if (parsed != CC_MESSAGE_TOOL_OUTCOME_RECORDED) {
                const char *call_id = cc_json_string_value(cc_json_object_get(message, "tool_call_id"));
                if (!call_id || !call_id[0] ||
                    (parsed == CC_MESSAGE_TOOL_OUTCOME_UNKNOWN ? strcmp(role, "tool") : strcmp(role, "user")))
                    return cc_result_error(CC_ERR_STORAGE, "Invalid JSON tool outcome association");
            }
        }
    }
    return cc_result_ok();
}

/**
 * @brief 拒绝遗留恢复文件，并在目标缺失时创建含四个空数组的会话文件。
 * @param[in,out] store 尚未向调用者发布的独占后端借用，父目录须已存在。
 * @return 文件已存在或初始化成功，其他返回旁路文件检查、exists、写入错误。
 * @note 初始化只创建目标文件，已有文件在后续 load_store 中验证。调用者释放结果诊断。
 */
static cc_result_t ensure_file_exists(cc_json_file_store_t *store)
{
    cc_result_t rc = reject_session_sidecars(store);
    if (rc.code != CC_OK) return rc;
    int exists = 0;
    rc = store->fs.vtable->exists(store->fs.self, store->file_path, &exists);
    if (rc.code != CC_OK || exists) return rc;
    return store->fs.vtable->write_text(store->fs.self, store->file_path,
        "{\"sessions\":[],\"messages\":[],\"tool_calls\":[],\"tool_results\":[]}");
}

/**
 * @brief 丢弃旧内存树，重新读取并验证整份会话文件。
 * @param[in,out] store 已持有互斥量的后端借用，成功安装新拥有树。
 * @return 成功或待核对写入、旁路文件、读取、长度/修改时间变化、JSON/结构/分配错误。
 * @note 读取前后核对文件元数据及 NUL，缺失审计数组仅在内存补为空数组。
 *       失败后的部分树仍由后端拥有，由下一次加载或销毁回收，禁止在失败后保存。
 *       互斥量只串行化同一实例，多个实例或进程访问同一路径由拥有者额外协调。
 */
static cc_result_t load_store(cc_json_file_store_t *store)
{
    if (store->write_indeterminate)
        return cc_result_error(CC_ERR_STORAGE, "Session write outcome requires reopen and inspection");
    cc_result_t sidecars = reject_session_sidecars(store);
    if (sidecars.code != CC_OK) return sidecars;
    if (store->root) {
        cc_json_destroy(store->root);
        store->root = NULL;
    }

    char *text = NULL;
    if (store->fs.vtable->size < offsetof(cc_filesystem_vtable_t, stat) +
                                  sizeof(store->fs.vtable->stat) || !store->fs.vtable->stat)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Session store cannot verify file length");
    cc_filesystem_stat_t before = {.size = sizeof(before)};
    cc_filesystem_stat_t after = {.size = sizeof(after)};
    cc_result_t rc = store->fs.vtable->stat(store->fs.self, store->file_path, &before);
    if (rc.code == CC_OK && before.type != CC_FS_ENTRY_FILE)
        rc = cc_result_error(CC_ERR_STORAGE, "Session history file is missing or invalid");
    if (rc.code == CC_OK) rc = store->fs.vtable->read_text(store->fs.self, store->file_path, &text);
    if (rc.code == CC_OK) rc = store->fs.vtable->stat(store->fs.self, store->file_path, &after);
    if (rc.code == CC_OK && (!text || before.byte_size != after.byte_size ||
        before.modified_ms != after.modified_ms || after.type != CC_FS_ENTRY_FILE ||
        (uint64_t)strlen(text) != after.byte_size))
        rc = cc_result_error(CC_ERR_STORAGE, "Session history changed or contains embedded NUL");
    if (rc.code == CC_OK) rc = reject_session_sidecars(store);
    if (rc.code == CC_OK && cc_storage_json_has_nul_escape(text))
        rc = cc_result_error(CC_ERR_STORAGE, "Session history contains encoded NUL");
    if (rc.code == CC_OK) rc = cc_json_parse_strict(text ? text : "", &store->root);
    free(text);
    if (rc.code != CC_OK) {
        return rc;
    }

    static const char *const root_fields[] = {"sessions", "messages", "tool_calls", "tool_results"};
    if (!cc_storage_json_fields_unique(store->root, root_fields, 4) ||
        !cc_json_is_array(cc_json_object_get(store->root, "sessions")) ||
        !cc_json_is_array(cc_json_object_get(store->root, "messages"))) {
        return cc_result_error(CC_ERR_STORAGE, "Invalid JSON session history structure");
    }
    rc = validate_stored_sessions(store->root);
    if (rc.code != CC_OK) return rc;
    rc = validate_stored_messages(store->root);
    if (rc.code != CC_OK) return rc;
    const char *optional_arrays[] = {"tool_calls", "tool_results"};
    for (size_t i = 0; i < sizeof(optional_arrays) / sizeof(optional_arrays[0]); i++) {
        cc_json_value_t *array = cc_json_object_get(store->root, optional_arrays[i]);
        if (array && !cc_json_is_array(array))
            return cc_result_error(CC_ERR_STORAGE, "Invalid JSON session audit structure");
        if (!array) {
            array = cc_json_create_array();
            if (!array) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot create session audit array");
            if (!cc_json_object_set_owned(store->root, optional_arrays[i], array)) {
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot install session audit array");
            }
        }
        static const char *const fields[] = {"session_id"};
        for (const cc_json_value_t *record = cc_json_array_first(array); record;
             record = cc_json_array_next(record)) {
            const char *sid = cc_json_string_value(cc_json_object_get(record, "session_id"));
            if (!cc_storage_json_fields_unique(record, fields, 1) || !sid || !sid[0])
                return cc_result_error(CC_ERR_STORAGE, "Invalid JSON audit session id");
        }
    }

    return cc_result_ok();
}

/**
 * @brief 序列化当前树并通过临时文件与原子替换安装会话库。
 * @param[in,out] store 已持有互斥量的后端借用，root 为空时直接成功。
 * @return 成功或序列化、路径、写入、同步、原子替换错误，调用者释放诊断。
 * @note 尝试写临时文件后发生的错误锁存 write_indeterminate，后续加载要求重新打开核对。
 *       文件/目录同步明确返回不支持时允许继续，其他同步失败向上传播。
 *       回收序列化文本和临时路径，失败保留磁盘证据，由拥有者处理外部同路径并发。
 */
static cc_result_t save_store(cc_json_file_store_t *store)
{
    if (!store->root) return cc_result_ok();

    char *json_text = cc_json_stringify(store->root);
    if (!json_text) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to serialize JSON store");

    size_t path_len = strlen(store->file_path);
    if (path_len > (size_t)-1 - 5) {
        free(json_text);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "JSON store path is too long");
    }
    char *temporary = malloc(path_len + 5);
    if (!temporary) {
        free(json_text);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate temporary path");
    }
    snprintf(temporary, path_len + 5, "%s.tmp", store->file_path);
    cc_result_t rc = store->fs.vtable->write_text(store->fs.self, temporary, json_text);
    if (rc.code == CC_OK && store->fs.vtable->sync_file) {
        rc = store->fs.vtable->sync_file(store->fs.self, temporary);
        if (rc.code == CC_ERR_UNSUPPORTED) {
            cc_result_free(&rc);
            rc = cc_result_ok();
        }
    }
    if (rc.code == CC_OK && store->fs.vtable->atomic_replace) {
        rc = store->fs.vtable->atomic_replace(
            store->fs.self, temporary, store->file_path);
    } else if (rc.code == CC_OK) {
        rc = cc_result_error(CC_ERR_UNSUPPORTED, "Filesystem lacks atomic replace");
    }
    if (rc.code == CC_OK && store->fs.vtable->sync_dir) {
        char *parent = cc_path_dirname(store->file_path);
        if (parent) {
            cc_result_t sync_rc = store->fs.vtable->sync_dir(store->fs.self, parent);
            if (sync_rc.code != CC_OK && sync_rc.code != CC_ERR_UNSUPPORTED) rc = sync_rc;
            else cc_result_free(&sync_rc);
            free(parent);
        } else {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate session parent path for sync");
        }
    }
    free(temporary);
    free(json_text);
    if (rc.code != CC_OK) store->write_indeterminate = 1;
    return rc;
}

/**
 * @brief 创建会话元数据，已有相同 ID 时幂等成功。
 * @param[in,out] self 已发布后端的借用私有实例，调用期间保持存活。
 * @param[in] session_id 必需的非空会话 ID，JSON 树保存其独立副本。
 * @param[in] workspace_dir 工作目录借用，NULL 使用默认工作目录。
 * @return 成功或参数、加载、分配、完整性、保存错误，调用者释放诊断。
 * @note 持互斥量覆盖加载、构造、验证和保存，字段构造失败回收候选，保存前失败保留磁盘。
 */
static cc_result_t json_file_create_session(
    void *self,
    const char *session_id,
    const char *workspace_dir
)
{
    if (!self || !session_id || !session_id[0])
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid JSON session create request");
    cc_json_file_store_t *store = (cc_json_file_store_t *)self;
    cc_mutex_lock(store->mutex);
    cc_result_t rc = load_store(store);
    if (rc.code != CC_OK) {
        cc_mutex_unlock(store->mutex);
        return rc;
    }

    cc_json_value_t *sessions = cc_json_object_get(store->root, "sessions");
    if (!sessions || !cc_json_is_array(sessions)) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_STORAGE, "Invalid sessions array");
    }

    for (const cc_json_value_t *s = cc_json_array_first(sessions); s;
         s = cc_json_array_next(s)) {
        cc_json_value_t *id = cc_json_object_get(s, "id");
        const char *id_str = cc_json_string_value(id);
        if (id_str && strcmp(id_str, session_id) == 0) {
            cc_mutex_unlock(store->mutex);
            return cc_result_ok();
        }
    }

    cc_json_value_t *session = cc_json_create_object();
    if (!session) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate JSON session");
    }
    if (!cc_json_object_set_owned(session, "id", cc_json_create_string(session_id)) ||
        !cc_json_object_set_owned(session, "workspace_dir",
            cc_json_create_string(workspace_dir ? workspace_dir : CC_DEFAULT_WORKSPACE_PATH)) ||
        !cc_json_object_set_owned(session, "status", cc_json_create_string("active"))) {
        cc_json_destroy(session);
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode JSON session metadata");
    }
    if (!cc_json_array_append_owned(sessions, session)) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot append JSON session");
    }

    rc = validate_stored_sessions(store->root);
    if (rc.code == CC_OK) rc = save_store(store);
    cc_mutex_unlock(store->mutex);
    return rc;
}

/**
 * @brief 从消息对象复制一个允许缺失或 null 的可选字符串。
 * @param[in] record 已验证的稳定消息对象借用。
 * @param[in] key 非空字段名借用。
 * @param[out] out 必需的新字符串槽，先置 NULL，成功副本由调用者 free。
 * @return 字段缺失/null 或复制成功时成功，类型错误或分配失败时返回诊断。
 */
static cc_result_t json_message_optional_string(const cc_json_value_t *record,
                                                 const char *key, char **out)
{
    cc_json_value_t *value = cc_json_object_get(record, key);
    *out = NULL;
    if (!value || cc_json_is_null(value)) return cc_result_ok();
    const char *text = cc_json_string_value(value);
    if (!text) return cc_result_error(CC_ERR_STORAGE, "Invalid JSON message metadata");
    *out = cc_copy_string(text);
    return *out ? cc_result_ok() : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy JSON message metadata");
}

/**
 * @brief 把当前格式记录解码为独立拥有的消息，包含工具状态和时间戳。
 * @param[in] record 调用期间稳定的消息 JSON 对象借用。
 * @param[out] out 必需的新/已清理消息槽，先清零初始化，成功拥有全部字段。
 * @return 成功或身份/角色、数组解码、元数据、工具状态、分配错误，调用者释放诊断。
 * @note 失败回收已复制字段，成功输出由调用者 cc_message_cleanup，调用者持有存储锁。
 */
static cc_result_t json_file_decode_message(const cc_json_value_t *record, cc_message_t *out)
{
    memset(out, 0, sizeof(*out));
    cc_content_parts_init(&out->content);
    cc_tool_call_list_init(&out->tool_calls);
    const char *id = cc_json_string_value(cc_json_object_get(record, "id"));
    const char *session_id = cc_json_string_value(cc_json_object_get(record, "session_id"));
    const char *role = cc_json_string_value(cc_json_object_get(record, "role"));
    cc_result_t rc = cc_result_ok();
    if (!id || !id[0] || !session_id || !session_id[0] || !role)
        rc = cc_result_error(CC_ERR_STORAGE, "Invalid JSON message identity or role");
    if (rc.code == CC_OK) {
        out->id = cc_copy_string(id);
        out->session_id = cc_copy_string(session_id);
        if (!out->id || !out->session_id)
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy JSON message identity");
    }
    if (rc.code == CC_OK) {
        if (strcmp(role, "system") == 0) out->role = CC_ROLE_SYSTEM;
        else if (strcmp(role, "user") == 0) out->role = CC_ROLE_USER;
        else if (strcmp(role, "assistant") == 0) out->role = CC_ROLE_ASSISTANT;
        else if (strcmp(role, "tool") == 0) out->role = CC_ROLE_TOOL;
        else rc = cc_result_error(CC_ERR_STORAGE, "Invalid JSON message role");
    }
    cc_json_value_t *parts = cc_json_object_get(record, "content_parts");
    if (rc.code == CC_OK && parts) {
        const char *json = cc_json_string_value(parts);
        rc = json ? cc_content_parts_from_json(json, &out->content) :
                    cc_result_error(CC_ERR_STORAGE, "Invalid JSON message content");
    }
    cc_json_value_t *calls = cc_json_object_get(record, "tool_calls");
    if (rc.code == CC_OK && calls) {
        const char *json = cc_json_string_value(calls);
        rc = json ? cc_tool_call_list_from_json(json, &out->tool_calls) :
                    cc_result_error(CC_ERR_STORAGE, "Invalid JSON message tool calls");
    }
    if (rc.code == CC_OK) rc = json_message_optional_string(record, "reasoning_content", &out->reasoning_content);
    if (rc.code == CC_OK) rc = json_message_optional_string(record, "tool_call_id", &out->tool_call_id);
    if (rc.code == CC_OK) rc = json_message_optional_string(record, "created_at", &out->created_at);
    cc_json_value_t *outcome = cc_json_object_get(record, "tool_outcome");
    if (rc.code == CC_OK && outcome) {
        const char *text = cc_json_string_value(outcome);
        rc = text ? cc_message_tool_outcome_parse(text, &out->tool_outcome) :
                    cc_result_error(CC_ERR_STORAGE, "Stored tool outcome is not a string");
    }
    if (rc.code == CC_OK && !cc_message_tool_outcome_valid(out))
        rc = cc_result_error(CC_ERR_STORAGE, "Stored tool outcome has invalid message fields");
    if (rc.code != CC_OK) cc_message_cleanup(out);
    return rc;
}

/**
 * @brief 加载指定会话最新的若干消息，结果保持原记录顺序。
 * @param[in,out] self 存活的后端实例借用。
 * @param[in] session_id 会话 ID 借用，NULL 无效，空串按未匹配处理。
 * @param[in] limit 正数限制最近条数，小于等于零读取全部匹配消息。
 * @param[out] out_messages 必需的新数组槽，合法参数入口置 NULL，成功移交数组所有权。
 * @param[out] out_count 必需的条数槽，合法参数入口置零。
 * @return 成功或参数、加载/校验、分配、消息解码错误，失败回收本次输出。
 * @note 全程持有实例锁，调用者逐项 cc_message_cleanup 后 free 数组并释放结果诊断。
 *       即使无匹配，也可能返回条数为零的拥有数组，调用者仍须 free。
 */
static cc_result_t json_file_load_messages(
    void *self,
    const char *session_id,
    int limit,
    cc_message_t **out_messages,
    size_t *out_count
)
{
    if (!self || !session_id || !out_messages || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid session history request");
    cc_json_file_store_t *store = (cc_json_file_store_t *)self;
    *out_messages = NULL;
    *out_count = 0;

    cc_mutex_lock(store->mutex);
    cc_result_t rc = load_store(store);
    if (rc.code != CC_OK) {
        cc_mutex_unlock(store->mutex);
        return rc;
    }

    cc_json_value_t *messages_arr = cc_json_object_get(store->root, "messages");
    if (!messages_arr || !cc_json_is_array(messages_arr)) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_STORAGE, "Invalid JSON session messages");
    }

    size_t cap = 16;
    size_t count = 0;
    cc_message_t *messages = calloc(cap, sizeof(cc_message_t));
    if (!messages) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate session messages");
    }

    size_t matching = 0;
    for (const cc_json_value_t *msg = cc_json_array_first(messages_arr); msg;
         msg = cc_json_array_next(msg)) {
        const char *sid_str = cc_json_string_value(cc_json_object_get(msg, "session_id"));
        if (!cc_json_is_object(msg) || !sid_str || !sid_str[0]) {
            rc = cc_result_error(CC_ERR_STORAGE, "Invalid JSON message session id");
            break;
        }
        if (strcmp(sid_str, session_id) == 0) matching++;
    }
    size_t skip = limit > 0 && matching > (size_t)limit ? matching - (size_t)limit : 0;
    for (const cc_json_value_t *msg = cc_json_array_first(messages_arr); msg && rc.code == CC_OK;
         msg = cc_json_array_next(msg)) {
        const char *sid_str = cc_json_string_value(cc_json_object_get(msg, "session_id"));
        if (!sid_str || strcmp(sid_str, session_id) != 0) continue;
        if (skip) { skip--; continue; }

        if (count >= cap) {
            if (cap > SIZE_MAX / 2U / sizeof(cc_message_t)) {
                rc = cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Session messages are too large");
                break;
            }
            void *new_ptr = realloc(messages, cap * 2 * sizeof(cc_message_t));
            if (!new_ptr) { rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot grow session messages"); break; }
            messages = new_ptr;
            cap *= 2;
        }

        rc = json_file_decode_message(msg, &messages[count]);
        if (rc.code != CC_OK) break;
        count++;
    }

    if (rc.code != CC_OK) {
        for (size_t j = 0; j < count; j++) cc_message_cleanup(&messages[j]);
        free(messages);
    } else {
        *out_messages = messages;
        *out_count = count;
    }
    cc_mutex_unlock(store->mutex);
    return rc;
}

/**
 * @brief 列举全部会话元数据并还原状态及可选时间/模型字段。
 * @param[in,out] self 存活的后端实例借用。
 * @param[out] out_sessions 必需的新数组槽，合法入口置 NULL，成功拥有全部会话副本。
 * @param[out] out_count 必需的条数槽，合法入口置零。
 * @return 成功或参数、加载、结构、分配错误，失败回收部分复制的数组。
 * @note 加载和复制受实例锁保护，成功数组通过 cc_session_free_array 释放，调用者消费诊断。
 */
static cc_result_t json_file_list_sessions(
    void *self,
    cc_session_t **out_sessions,
    size_t *out_count
)
{
    if (!self || !out_sessions || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid JSON session list request");
    *out_sessions = NULL;
    *out_count = 0;
    cc_json_file_store_t *store = (cc_json_file_store_t *)self;

    cc_mutex_lock(store->mutex);
    cc_result_t rc = load_store(store);
    if (rc.code != CC_OK) {
        *out_sessions = NULL;
        *out_count = 0;
        cc_mutex_unlock(store->mutex);
        return rc;
    }

    cc_json_value_t *sessions_arr = cc_json_object_get(store->root, "sessions");
    if (!sessions_arr || !cc_json_is_array(sessions_arr)) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_STORAGE, "Invalid JSON session history structure");
    }

    int count = cc_json_array_size(sessions_arr);
    cc_session_t *sessions = count ? calloc((size_t)count, sizeof(cc_session_t)) : NULL;
    if (count && !sessions) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate sessions");
    }

    int i = 0;
    for (const cc_json_value_t *s = cc_json_array_first(sessions_arr); s && rc.code == CC_OK;
         s = cc_json_array_next(s), i++) {
        cc_session_t *out = &sessions[i];
        const char *id = cc_json_string_value(cc_json_object_get(s, "id"));
        const char *workspace = cc_json_string_value(cc_json_object_get(s, "workspace_dir"));
        const char *name = cc_json_string_value(cc_json_object_get(s, "name"));
        const char *model = cc_json_string_value(cc_json_object_get(s, "model"));
        const char *created = cc_json_string_value(cc_json_object_get(s, "created_at"));
        const char *updated = cc_json_string_value(cc_json_object_get(s, "updated_at"));
        const char *status = cc_json_string_value(cc_json_object_get(s, "status"));
        out->id = cc_copy_string(id);
        out->workspace_dir = cc_copy_string(workspace ? workspace : "");
        out->name = cc_copy_string(name ? name : id);
        out->model = model ? cc_copy_string(model) : NULL;
        out->created_at = created ? cc_copy_string(created) : NULL;
        out->updated_at = updated ? cc_copy_string(updated) : NULL;
        out->status = status && strcmp(status, "completed") == 0 ? CC_SESSION_COMPLETED :
                      status && strcmp(status, "error") == 0 ? CC_SESSION_ERROR : CC_SESSION_ACTIVE;
        if (!out->id || !out->workspace_dir || !out->name || (model && !out->model) ||
            (created && !out->created_at) || (updated && !out->updated_at))
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy JSON session metadata");
    }
    cc_mutex_unlock(store->mutex);
    if (rc.code != CC_OK) { cc_session_free_array(sessions, (size_t)count); return rc; }
    *out_sessions = sessions;
    *out_count = (size_t)count;
    return rc;
}

/**
 * @brief 经紧凑序列化与严格解析复制记录，保留其他会话的扩展字段。
 * @param[in] src 调用期间稳定的源节点借用。
 * @param[out] out 必需的新节点槽，先置 NULL，成功副本由调用者销毁或挂接。
 * @return 成功或序列化/解析错误，临时文本在返回前释放，调用者释放结果诊断。
 */
static cc_result_t json_clone_record(const cc_json_value_t *src, cc_json_value_t **out)
{
    *out = NULL;
    char *text = cc_json_stringify_unformatted(src);
    if (!text) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot clone JSON session record");
    cc_result_t rc = cc_json_parse_strict(text, out);
    free(text);
    return rc;
}

/**
 * @brief 构造排除指定会话的新数组，全部复制成功后替换根字段。
 * @param[in,out] root 已持锁的后端根节点借用，成功接管新数组并释放原数组。
 * @param[in] array_key 必需的记录数组字段名借用。
 * @param[in] session_id 待清除的会话 ID 借用，字段精确匹配时排除。
 * @return 成功或结构、克隆、挂接错误，失败回收候选并保留该原数组。
 * @note 不更改其他会话记录的扩展字段，调用者释放结果诊断。
 */
static cc_result_t json_filter_session_array(
    cc_json_value_t *root,
    const char *array_key,
    const char *session_id
)
{
    cc_json_value_t *old_array = cc_json_object_get(root, array_key);
    if (!old_array || !cc_json_is_array(old_array)) {
        return cc_result_error(CC_ERR_STORAGE, "Invalid JSON store array");
    }
    cc_json_value_t *new_array = cc_json_create_array();
    if (!new_array) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate filtered session array");
    }
    for (const cc_json_value_t *item = cc_json_array_first(old_array); item;
         item = cc_json_array_next(item)) {
        const char *sid = cc_json_string_value(cc_json_object_get(item, "session_id"));
        if (!cc_json_is_object(item) || !sid || !sid[0]) {
            cc_json_destroy(new_array);
            return cc_result_error(CC_ERR_STORAGE, "Invalid JSON record session id");
        }
        if (strcmp(sid, session_id) == 0) continue;
        cc_json_value_t *copy = NULL;
        cc_result_t rc = json_clone_record(item, &copy);
        if (rc.code != CC_OK) { cc_json_destroy(new_array); return rc; }
        if (!cc_json_array_append_owned(new_array, copy)) {
            cc_json_destroy(new_array);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot append filtered session record");
        }
    }
    if (!cc_json_object_set_owned(root, array_key, new_array))
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot replace filtered session array");
    return cc_result_ok();
}

/**
 * @brief 清除会话的消息与工具审计记录，保留会话元数据。
 * @param[in,out] self 存活的后端实例借用。
 * @param[in] session_id 待清除的会话 ID 借用，NULL 无效，空串按未匹配处理。
 * @return 成功或参数、加载、数组重建、保存错误，调用者释放诊断。
 * @note 持锁覆盖三个数组的重建和整库保存，保存前任一步失败均不写磁盘。
 *       内存中的部分重建树由下次 load_store 丢弃，调用者先关闭该会话任务入口并等待退出。
 */
static cc_result_t json_file_clear_session(void *self, const char *session_id)
{
    if (!self || !session_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid session clear request");
    }
    cc_json_file_store_t *store = (cc_json_file_store_t *)self;
    cc_mutex_lock(store->mutex);
    cc_result_t rc = load_store(store);
    if (rc.code == CC_OK) {
        rc = json_filter_session_array(store->root, "messages", session_id);
    }
    if (rc.code == CC_OK) {
        rc = json_filter_session_array(store->root, "tool_calls", session_id);
    }
    if (rc.code == CC_OK) {
        rc = json_filter_session_array(store->root, "tool_results", session_id);
    }
    if (rc.code == CC_OK) {
        rc = save_store(store);
    }
    cc_mutex_unlock(store->mutex);
    return rc;
}

/**
 * @brief 销毁后端拥有的 JSON 树、路径、文件系统端口、互斥量和实例。
 * @param[in] self 待消费的私有实例拥有指针，允许 NULL。
 * @return 无返回值。
 * @note 拥有者须先关闭入口并等待所有已接纳调用退出，返回后禁止再访问端口或实例。
 *       内部互斥量只保护回收临界区，不承担并发进入销毁的生命周期屏障。
 */
static void json_file_destroy(void *self)
{
    cc_json_file_store_t *store = (cc_json_file_store_t *)self;
    if (!store) return;
    cc_mutex_lock(store->mutex);
    cc_json_destroy(store->root);
    free(store->file_path);
    cc_mutex_unlock(store->mutex);
    cc_mutex_destroy(store->mutex);
    if (store->fs.vtable && store->fs.vtable->destroy) {
        store->fs.vtable->destroy(store->fs.self);
    }
    free(store);
}

/**
 * @brief 验证并追加消息、调用审计和结果审计批次，再以整文件事务保存。
 * @param[in,out] self 存活的后端实例借用。
 * @param[in] records 调用期间稳定的记录数组借用，count 为零时允许 NULL。
 * @param[in] count 记录数，消息与审计字段均复制进后端树，原记录仍归调用者。
 * @return 成功或参数、加载、序列化、分配、记录归属、保存错误，调用者释放诊断。
 * @note 参数预检后持锁覆盖加载、逐条构造、全库验证和保存，字段挂接失败立即回收候选。
 *       保存前失败不写磁盘，内存中已追加的部分由下一次加载丢弃。
 *       保存开始后的错误按 write_indeterminate 契约阻止后续写入，拥有者协调同路径外部写者。
 */
static cc_result_t json_file_append_records(
    void *self,
    const cc_session_record_t *records,
    size_t count
)
{
    if (!self || (!records && count > 0)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid JSON store batch");
    }
    cc_json_file_store_t *store = (cc_json_file_store_t *)self;
    for (size_t i = 0; i < count; i++) {
        const cc_session_record_t *record = &records[i];
        if (record->type == CC_SESSION_RECORD_MESSAGE) {
            const cc_message_t *message = record->data.message;
            if (!message || !message->id || !message->id[0] || !message->session_id ||
                !message->session_id[0] ||
                (record->session_id && record->session_id[0] && strcmp(record->session_id, message->session_id)) ||
                strcmp(cc_message_role_string(message->role), "unknown") == 0 ||
                !cc_message_tool_outcome_valid(message))
                return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid JSON message identity or role");
        } else if (record->type == CC_SESSION_RECORD_TOOL_CALL) {
            const cc_tool_call_t *call = record->data.tool_call;
            if (!record->session_id || !record->session_id[0] || !call || !call->id || !call->id[0] ||
                !call->name || !call->name[0])
                return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid JSON tool call record");
        } else if (record->type == CC_SESSION_RECORD_TOOL_RESULT) {
            if (!record->session_id || !record->session_id[0] || !record->data.tool_result.tool_call_id ||
                !record->data.tool_result.tool_call_id[0] || !record->data.tool_result.result)
                return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid JSON tool result record");
        } else {
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Unknown session record type");
        }
    }
    cc_mutex_lock(store->mutex);
    cc_result_t rc = load_store(store);
    if (rc.code != CC_OK) {
        cc_mutex_unlock(store->mutex);
        return rc;
    }
    for (size_t i = 0; i < count; i++) {
        const cc_session_record_t *record = &records[i];
        switch (record->type) {
        case CC_SESSION_RECORD_MESSAGE: {
            const cc_message_t *message = record->data.message;
            cc_json_value_t *messages = cc_json_object_get(store->root, "messages");
            if (!message || !messages || !cc_json_is_array(messages)) {
                rc = cc_result_error(CC_ERR_STORAGE, "Invalid message batch record");
                break;
            }
            if (!cc_message_tool_outcome_valid(message)) {
                rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid message tool outcome");
                break;
            }
            cc_json_value_t *msg = cc_json_create_object();
            if (!msg) { rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate message record"); break; }
            if (!cc_json_object_set_owned(msg, "id", cc_json_create_string(message->id)) ||
                !cc_json_object_set_owned(msg, "session_id", cc_json_create_string(message->session_id)) ||
                !cc_json_object_set_owned(msg, "role", cc_json_create_string(cc_message_role_string(message->role)))) {
                cc_json_destroy(msg);
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode JSON message identity");
                break;
            }
            char *content_parts = NULL;
            rc = cc_content_parts_to_json(&message->content, &content_parts);
            if (rc.code != CC_OK) { cc_json_destroy(msg); break; }
            int content_installed = cc_json_object_set_owned(msg, "content_parts",
                cc_json_create_string(content_parts ? content_parts : "[]"));
            free(content_parts);
            if (!content_installed) {
                cc_json_destroy(msg);
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode JSON message content");
                break;
            }
            char *tool_calls = NULL;
            rc = cc_tool_call_list_to_json(&message->tool_calls, &tool_calls);
            if (rc.code != CC_OK) { cc_json_destroy(msg); break; }
            int calls_installed = cc_json_object_set_owned(msg, "tool_calls",
                cc_json_create_string(tool_calls ? tool_calls : "[]"));
            free(tool_calls);
            if (!calls_installed ||
                (message->reasoning_content && !cc_json_object_set_owned(msg, "reasoning_content",
                    cc_json_create_string(message->reasoning_content))) ||
                (message->tool_call_id && !cc_json_object_set_owned(msg, "tool_call_id",
                    cc_json_create_string(message->tool_call_id))) ||
                (message->created_at && !cc_json_object_set_owned(msg, "created_at",
                    cc_json_create_string(message->created_at))) ||
                (message->tool_outcome != CC_MESSAGE_TOOL_OUTCOME_RECORDED &&
                    !cc_json_object_set_owned(msg, "tool_outcome",
                        cc_json_create_string(cc_message_tool_outcome_string(message->tool_outcome))))) {
                cc_json_destroy(msg);
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode JSON message metadata");
                break;
            }
            if (!cc_json_array_append_owned(messages, msg))
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot append JSON message");
            break;
        }
        case CC_SESSION_RECORD_TOOL_CALL: {
            const cc_tool_call_t *call = record->data.tool_call;
            cc_json_value_t *tool_calls = cc_json_object_get(store->root, "tool_calls");
            if (!call || !tool_calls || !cc_json_is_array(tool_calls)) {
                rc = cc_result_error(CC_ERR_STORAGE, "Invalid tool call batch record");
                break;
            }
            cc_json_value_t *tc = cc_json_create_object();
            if (!tc) { rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate tool call record"); break; }
            if (!cc_json_object_set_owned(tc, "id", cc_json_create_string(call->id)) ||
                !cc_json_object_set_owned(tc, "session_id", cc_json_create_string(record->session_id)) ||
                !cc_json_object_set_owned(tc, "name", cc_json_create_string(call->name)) ||
                !cc_json_object_set_owned(tc, "arguments_json",
                    cc_json_create_string(call->arguments_json ? call->arguments_json : "")) ||
                !cc_json_object_set_owned(tc, "status", cc_json_create_string("completed"))) {
                cc_json_destroy(tc);
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode JSON tool call");
                break;
            }
            if (!cc_json_array_append_owned(tool_calls, tc))
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot append JSON tool call");
            break;
        }
        case CC_SESSION_RECORD_TOOL_RESULT: {
            const cc_tool_result_t *result = record->data.tool_result.result;
            const char *tool_call_id = record->data.tool_result.tool_call_id;
            cc_json_value_t *tool_results = cc_json_object_get(store->root, "tool_results");
            if (!result || !tool_results || !cc_json_is_array(tool_results)) {
                rc = cc_result_error(CC_ERR_STORAGE, "Invalid tool result batch record");
                break;
            }
            cc_json_value_t *tr = cc_json_create_object();
            if (!tr) { rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate tool result record"); break; }
            if (!cc_json_object_set_owned(tr, "id", cc_json_create_string(tool_call_id)) ||
                !cc_json_object_set_owned(tr, "session_id", cc_json_create_string(record->session_id)) ||
                !cc_json_object_set_owned(tr, "tool_call_id", cc_json_create_string(tool_call_id)) ||
                !cc_json_object_set_owned(tr, "ok", cc_json_create_bool(result->ok)) ||
                !cc_json_object_set_owned(tr, "text", cc_json_create_string(result->text ? result->text : "")) ||
                !cc_json_object_set_owned(tr, "error", cc_json_create_string(result->error ? result->error : "")) ||
                !cc_json_object_set_owned(tr, "metadata", cc_json_create_string(result->metadata ? result->metadata : ""))) {
                cc_json_destroy(tr);
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode JSON tool result");
                break;
            }
            char *artifacts = NULL;
            rc = cc_media_artifact_list_to_json(&result->artifacts, &artifacts);
            if (rc.code != CC_OK) { cc_json_destroy(tr); break; }
            int artifacts_installed = cc_json_object_set_owned(tr, "artifacts",
                cc_json_create_string(artifacts ? artifacts : "[]"));
            free(artifacts);
            if (!artifacts_installed) {
                cc_json_destroy(tr);
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode JSON tool result artifacts");
                break;
            }
            if (!cc_json_array_append_owned(tool_results, tr))
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot append JSON tool result");
            break;
        }
        default:
            rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Unknown session record type");
            break;
        }
        if (rc.code != CC_OK) {
            cc_mutex_unlock(store->mutex);
            return rc;
        }
    }
    rc = validate_stored_sessions(store->root);
    if (rc.code == CC_OK) rc = validate_stored_messages(store->root);
    if (rc.code == CC_OK) rc = save_store(store);
    cc_mutex_unlock(store->mutex);
    return rc;
}

/* JSON 文件 session store vtable，把持久化实现绑定到 cc_session_store_t 端口。 */
static cc_session_store_vtable_t json_file_vtable = {
    .create_session = json_file_create_session,
    .append_records = json_file_append_records,
    .load_messages = json_file_load_messages,
    .list_sessions = json_file_list_sessions,
    .clear_session = json_file_clear_session,
    .destroy = json_file_destroy
};

/**
 * @brief 创建拥有路径、默认文件系统端口和实例锁的整文件 JSON 会话后端。
 * @param[in] file_path 调用期间借用的文件路径，NULL 使用默认路径，实例保存独立副本。
 * @param[out] out_store 必需的新端口槽，先清零，成功移交 self/vtable。
 * @return 成功或参数、分配、默认文件系统、互斥量、文件初始化错误，失败回收候选资源。
 * @note 父目录须已存在，已有文件延迟到首次操作时验证，由调用者释放结果诊断。
 *       拥有者关闭入口并等待在途调用退出后调用 vtable.destroy，多实例写同路径须外部协调。
 */
cc_result_t cc_json_file_store_create(const char *file_path, cc_session_store_t *out_store)
{
    if (!out_store) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null JSON file store output");
    }
    memset(out_store, 0, sizeof(*out_store));
    cc_json_file_store_t *self = calloc(1, sizeof(cc_json_file_store_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create JSON file store");

    self->file_path = file_path ? cc_copy_string(file_path) : cc_copy_string(CC_DEFAULT_STORAGE_PATH);
    if (!self->file_path) {
        free(self);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy JSON session path");
    }
    self->root = NULL;
    cc_result_t rc = cc_filesystem_get_default(&self->fs);
    if (rc.code != CC_OK) {
        free(self->file_path);
        free(self);
        return rc;
    }
    rc = cc_mutex_create(&self->mutex);
    if (rc.code != CC_OK) {
        if (self->fs.vtable && self->fs.vtable->destroy) self->fs.vtable->destroy(self->fs.self);
        free(self->file_path);
        free(self);
        return rc;
    }

    rc = ensure_file_exists(self);
    if (rc.code != CC_OK) {
        cc_json_destroy(self->root);
        cc_mutex_destroy(self->mutex);
        if (self->fs.vtable && self->fs.vtable->destroy) self->fs.vtable->destroy(self->fs.self);
        free(self->file_path);
        free(self);
        return rc;
    }

    out_store->self = self;
    out_store->vtable = &json_file_vtable;
    return cc_result_ok();
}
