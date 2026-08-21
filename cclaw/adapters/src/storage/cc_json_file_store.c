



#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_thread.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_path.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"
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
} cc_json_file_store_t;

/*
 * 确保 JSON store 文件存在。
 *
 * 文件不存在时写入最小 schema，包含 sessions/messages/tool_calls/tool_results 四个数组。
 * 这里不创建父目录，父目录应由 builder/filesystem 初始化阶段准备。
 */
static cc_result_t ensure_file_exists(cc_json_file_store_t *store)
{
    int exists = 0;
    cc_result_t rc = store->fs.vtable->exists(store->fs.self, store->file_path, &exists);
    if (rc.code != CC_OK || exists) return rc;
    return store->fs.vtable->write_text(store->fs.self, store->file_path,
        "{\"sessions\":[],\"messages\":[],\"tool_calls\":[],\"tool_results\":[]}");
}

/*
 * 从磁盘加载 JSON store。
 *
 * 每次写操作前重新读取文件，保证进程内 root 与磁盘状态同步；如果文件内容不是对象，
 * 会重建最小结构，避免后续 array 访问崩溃。
 */
static cc_result_t load_store(cc_json_file_store_t *store)
{
    if (store->root) cc_json_destroy(store->root);

    char *text = NULL;
    cc_result_t rc = store->fs.vtable->read_text(
        store->fs.self, store->file_path, &text);
    if (rc.code == CC_OK) rc = cc_json_parse(text ? text : "{}", &store->root);
    free(text);
    if (rc.code != CC_OK) {
        return rc;
    }

    if (!store->root || !cc_json_is_object(store->root)) {
        if (store->root) cc_json_destroy(store->root);
        store->root = cc_json_create_object();
        cc_json_object_set(store->root, "sessions", cc_json_create_array());
        cc_json_object_set(store->root, "messages", cc_json_create_array());
        cc_json_object_set(store->root, "tool_calls", cc_json_create_array());
        cc_json_object_set(store->root, "tool_results", cc_json_create_array());
    }

    if (!cc_json_object_get(store->root, "sessions")) {
        cc_json_object_set(store->root, "sessions", cc_json_create_array());
    }
    if (!cc_json_object_get(store->root, "messages")) {
        cc_json_object_set(store->root, "messages", cc_json_create_array());
    }
    if (!cc_json_object_get(store->root, "tool_calls")) {
        cc_json_object_set(store->root, "tool_calls", cc_json_create_array());
    }
    if (!cc_json_object_get(store->root, "tool_results")) {
        cc_json_object_set(store->root, "tool_results", cc_json_create_array());
    }

    return cc_result_ok();
}

/*
 * 将当前 root 保存到磁盘。
 *
 * 调用方必须已经持有 mutex；函数把 JSON AST 序列化成文本后覆盖写入 file_path。
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
        }
    }
    free(temporary);
    free(json_text);
    return rc;
}

/*
 * 创建 session 记录。
 *
 * 已存在同名 session 时直接成功；新 session 会保存 workspace_dir，供后续文件工具和 UI
 * 读取。函数持锁覆盖 load/append/save 全流程。
 */
static cc_result_t json_file_create_session(
    void *self,
    const char *session_id,
    const char *workspace_dir
)
{
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

    int count = cc_json_array_size(sessions);
    for (int i = 0; i < count; i++) {
        cc_json_value_t *s = cc_json_array_get(sessions, i);
        cc_json_value_t *id = cc_json_object_get(s, "id");
        const char *id_str = cc_json_string_value(id);
        if (id_str && strcmp(id_str, session_id) == 0) {
            cc_mutex_unlock(store->mutex);
            return cc_result_ok();
        }
    }

    cc_json_value_t *session = cc_json_create_object();
    cc_json_object_set(session, "id", cc_json_create_string(session_id));
    cc_json_object_set(session, "workspace_dir",
        cc_json_create_string(workspace_dir ? workspace_dir : CC_DEFAULT_WORKSPACE_PATH));
    cc_json_object_set(session, "status", cc_json_create_string("active"));

    cc_json_array_append(sessions, session);

    rc = save_store(store);
    cc_mutex_unlock(store->mutex);
    return rc;
}

/*
 * 从 JSON 文件加载某个 session 的消息。
 *
 * 返回数组由调用方拥有，需要逐条 cc_message_cleanup 后 free。load 失败时该实现返回空
 * 列表而不是中断 agent，这是一种“历史不可用但当前 run 继续”的降级策略。
 */
static cc_result_t json_file_load_messages(
    void *self,
    const char *session_id,
    int limit,
    cc_message_t **out_messages,
    size_t *out_count
)
{
    cc_json_file_store_t *store = (cc_json_file_store_t *)self;

    cc_mutex_lock(store->mutex);
    cc_result_t rc = load_store(store);
    if (rc.code != CC_OK) {
        *out_messages = NULL;
        *out_count = 0;
        cc_mutex_unlock(store->mutex);
        return cc_result_ok();
    }

    cc_json_value_t *messages_arr = cc_json_object_get(store->root, "messages");
    if (!messages_arr || !cc_json_is_array(messages_arr)) {
        *out_messages = NULL;
        *out_count = 0;
        cc_mutex_unlock(store->mutex);
        return cc_result_ok();
    }

    size_t cap = 16;
    size_t count = 0;
    cc_message_t *messages = calloc(cap, sizeof(cc_message_t));
    if (!messages) {
        *out_messages = NULL;
        *out_count = 0;
        cc_mutex_unlock(store->mutex);
        return cc_result_ok();
    }

    int arr_size = cc_json_array_size(messages_arr);
    int loaded = 0;
    int max_loaded = limit > 0 ? limit : arr_size;
    for (int i = 0; i < arr_size && loaded < max_loaded; i++) {
        cc_json_value_t *msg = cc_json_array_get(messages_arr, i);

        cc_json_value_t *sid = cc_json_object_get(msg, "session_id");
        const char *sid_str = cc_json_string_value(sid);
        if (!sid_str || strcmp(sid_str, session_id) != 0) continue;

        if (count >= cap) {
            void *new_ptr = realloc(messages, cap * 2 * sizeof(cc_message_t));
            if (!new_ptr) break;
            messages = new_ptr;
            cap *= 2;
        }

        cc_message_t *m = &messages[count];
        memset(m, 0, sizeof(cc_message_t));

        cc_json_value_t *v = cc_json_object_get(msg, "id");
        m->id = cc_copy_string(cc_json_string_value(v) ? cc_json_string_value(v) : "");

        cc_content_parts_init(&m->content);
        cc_tool_call_list_init(&m->tool_calls);

        v = cc_json_object_get(msg, "content_parts");
        const char *cpj = cc_json_string_value(v);
        if (cpj) {
            cc_content_parts_from_json(cpj, &m->content);
        } else {
            v = cc_json_object_get(msg, "content");
            cc_content_parts_append_text(&m->content,
                cc_json_string_value(v) ? cc_json_string_value(v) : "",
                CC_CONTENT_PART_INPUT);
        }

        v = cc_json_object_get(msg, "tool_calls");
        const char *tcj = cc_json_string_value(v);
        if (tcj) cc_tool_call_list_from_json(tcj, &m->tool_calls);

        v = cc_json_object_get(msg, "reasoning_content");
        const char *reasoning = cc_json_string_value(v);
        m->reasoning_content = reasoning ? cc_copy_string(reasoning) : NULL;

        v = cc_json_object_get(msg, "tool_call_id");
        const char *tci = cc_json_string_value(v);
        m->tool_call_id = tci ? cc_copy_string(tci) : NULL;

        v = cc_json_object_get(msg, "role");
        const char *role = cc_json_string_value(v);
        if (role) {
            if (strcmp(role, "system") == 0) m->role = CC_ROLE_SYSTEM;
            else if (strcmp(role, "user") == 0) m->role = CC_ROLE_USER;
            else if (strcmp(role, "assistant") == 0) m->role = CC_ROLE_ASSISTANT;
            else if (strcmp(role, "tool") == 0) m->role = CC_ROLE_TOOL;
        }

        m->session_id = sid_str ? cc_copy_string(sid_str) : NULL;
        count++;
        loaded++;
    }

    *out_messages = messages;
    *out_count = count;
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/*
 * 列举 JSON 文件中的 session。
 *
 * 返回数组和字符串由调用方拥有。这里只还原 id/workspace/name 等基础字段，状态字段未来
 * 可按 session schema 扩展。
 */
static cc_result_t json_file_list_sessions(
    void *self,
    cc_session_t **out_sessions,
    size_t *out_count
)
{
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
        *out_sessions = NULL;
        *out_count = 0;
        cc_mutex_unlock(store->mutex);
        return cc_result_ok();
    }

    int count = cc_json_array_size(sessions_arr);
    cc_session_t *sessions = calloc(count, sizeof(cc_session_t));
    if (!sessions) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate sessions");
    }

    for (int i = 0; i < count; i++) {
        cc_json_value_t *s = cc_json_array_get(sessions_arr, i);
        cc_session_t *out = &sessions[i];

        cc_json_value_t *v = cc_json_object_get(s, "id");
        out->id = cc_copy_string(cc_json_string_value(v) ? cc_json_string_value(v) : "");

        v = cc_json_object_get(s, "workspace_dir");
        out->workspace_dir = cc_copy_string(cc_json_string_value(v) ? cc_json_string_value(v) : "");

        v = cc_json_object_get(s, "workspace_dir");
        const char *name_str = cc_json_string_value(v);
        out->name = name_str ? cc_copy_string(name_str) : NULL;
    }

    *out_sessions = sessions;
    *out_count = (size_t)count;
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/*
 * 从一个 JSON object 拷贝 string 字段到另一个 object。
 *
 * clear session 需要重建数组，不能把旧 root 里的节点直接移动到新数组，否则销毁旧数组时
 * 会出现所有权混乱；因此 clone helper 都通过 create_string 新建节点。
 */
static void json_copy_string_field(cc_json_value_t *dst, cc_json_value_t *src, const char *key)
{
    const char *value = cc_json_string_value(cc_json_object_get(src, key));
    if (value) cc_json_object_set(dst, key, cc_json_create_string(value));
}

/* 克隆一条 message 记录，只复制当前 JSON file schema 使用的字段。 */
static cc_json_value_t *json_clone_message_record(cc_json_value_t *src)
{
    cc_json_value_t *dst = cc_json_create_object();
    if (!dst) return NULL;
    json_copy_string_field(dst, src, "id");
    json_copy_string_field(dst, src, "session_id");
    json_copy_string_field(dst, src, "role");
    json_copy_string_field(dst, src, "content_parts");
    json_copy_string_field(dst, src, "tool_calls");
    json_copy_string_field(dst, src, "reasoning_content");
    json_copy_string_field(dst, src, "tool_call_id");
    return dst;
}

/* 克隆一条 tool call 记录，用于 clear_session 过滤非目标会话记录。 */
static cc_json_value_t *json_clone_tool_call_record(cc_json_value_t *src)
{
    cc_json_value_t *dst = cc_json_create_object();
    if (!dst) return NULL;
    json_copy_string_field(dst, src, "id");
    json_copy_string_field(dst, src, "session_id");
    json_copy_string_field(dst, src, "name");
    json_copy_string_field(dst, src, "arguments_json");
    json_copy_string_field(dst, src, "status");
    return dst;
}

/* 克隆一条 tool result 记录，包括 bool ok 和字符串化 artifacts。 */
static cc_json_value_t *json_clone_tool_result_record(cc_json_value_t *src)
{
    cc_json_value_t *dst = cc_json_create_object();
    if (!dst) return NULL;
    json_copy_string_field(dst, src, "id");
    json_copy_string_field(dst, src, "session_id");
    json_copy_string_field(dst, src, "tool_call_id");
    cc_json_object_set(dst, "ok",
        cc_json_create_bool(cc_json_bool_value(cc_json_object_get(src, "ok"))));
    json_copy_string_field(dst, src, "text");
    json_copy_string_field(dst, src, "error");
    json_copy_string_field(dst, src, "metadata");
    json_copy_string_field(dst, src, "artifacts");
    return dst;
}

/*
 * 过滤某个 session 的记录数组。
 *
 * 函数创建一个新数组，只克隆 session_id 不匹配的记录，然后替换 root[array_key]。这种
 * “重建数组”比原地删除 JSON 节点更简单，也更容易保证节点所有权清晰。
 */
static cc_result_t json_filter_session_array(
    cc_json_value_t *root,
    const char *array_key,
    const char *session_id,
    cc_json_value_t *(*clone_record)(cc_json_value_t *src)
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
    int count = cc_json_array_size(old_array);
    for (int i = 0; i < count; i++) {
        cc_json_value_t *item = cc_json_array_get(old_array, i);
        const char *sid = cc_json_string_value(cc_json_object_get(item, "session_id"));
        if (sid && strcmp(sid, session_id) == 0) continue;
        cc_json_value_t *copy = clone_record(item);
        if (!copy) {
            cc_json_destroy(new_array);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to clone session record");
        }
        cc_json_array_append(new_array, copy);
    }
    cc_json_object_set(root, array_key, new_array);
    return cc_result_ok();
}

/*
 * 清空某个 session 的消息和工具审计记录。
 *
 * 该操作不会删除 sessions 数组中的 session 元数据，只清除上下文历史和工具记录；适合
 * reset 会话而保留 workspace/id。
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
        rc = json_filter_session_array(
            store->root, "messages", session_id, json_clone_message_record);
    }
    if (rc.code == CC_OK) {
        rc = json_filter_session_array(
            store->root, "tool_calls", session_id, json_clone_tool_call_record);
    }
    if (rc.code == CC_OK) {
        rc = json_filter_session_array(
            store->root, "tool_results", session_id, json_clone_tool_result_record);
    }
    if (rc.code == CC_OK) {
        rc = save_store(store);
    }
    cc_mutex_unlock(store->mutex);
    return rc;
}

/*
 * 销毁 JSON 文件 session store。
 *
 * 调用方必须保证没有并发操作仍在进行；函数释放 root/file_path/mutex 和私有对象。
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
            cc_json_value_t *msg = cc_json_create_object();
            if (!msg) { rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate message record"); break; }
            cc_json_object_set(msg, "id", cc_json_create_string(message->id ? message->id : ""));
            cc_json_object_set(msg, "session_id", cc_json_create_string(message->session_id ? message->session_id : ""));
            cc_json_object_set(msg, "role", cc_json_create_string(cc_message_role_string(message->role)));
            char *content_parts = NULL;
            rc = cc_content_parts_to_json(&message->content, &content_parts);
            if (rc.code != CC_OK) { cc_json_destroy(msg); break; }
            cc_json_object_set(msg, "content_parts", cc_json_create_string(content_parts ? content_parts : "[]"));
            free(content_parts);
            char *tool_calls = NULL;
            rc = cc_tool_call_list_to_json(&message->tool_calls, &tool_calls);
            if (rc.code != CC_OK) { cc_json_destroy(msg); break; }
            cc_json_object_set(msg, "tool_calls", cc_json_create_string(tool_calls ? tool_calls : "[]"));
            free(tool_calls);
            if (message->reasoning_content) cc_json_object_set(msg, "reasoning_content",
                cc_json_create_string(message->reasoning_content));
            if (message->tool_call_id) cc_json_object_set(msg, "tool_call_id",
                cc_json_create_string(message->tool_call_id));
            cc_json_array_append(messages, msg);
            rc = cc_result_ok();
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
            cc_json_object_set(tc, "id", cc_json_create_string(call->id ? call->id : ""));
            cc_json_object_set(tc, "session_id", cc_json_create_string(record->session_id ? record->session_id : ""));
            cc_json_object_set(tc, "name", cc_json_create_string(call->name ? call->name : ""));
            cc_json_object_set(tc, "arguments_json", cc_json_create_string(call->arguments_json ? call->arguments_json : ""));
            cc_json_object_set(tc, "status", cc_json_create_string("completed"));
            cc_json_array_append(tool_calls, tc);
            rc = cc_result_ok();
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
            cc_json_object_set(tr, "id", cc_json_create_string(tool_call_id ? tool_call_id : ""));
            cc_json_object_set(tr, "session_id", cc_json_create_string(record->session_id ? record->session_id : ""));
            cc_json_object_set(tr, "tool_call_id", cc_json_create_string(tool_call_id ? tool_call_id : ""));
            cc_json_object_set(tr, "ok", cc_json_create_bool(result->ok));
            cc_json_object_set(tr, "text", cc_json_create_string(result->text ? result->text : ""));
            cc_json_object_set(tr, "error", cc_json_create_string(result->error ? result->error : ""));
            cc_json_object_set(tr, "metadata", cc_json_create_string(result->metadata ? result->metadata : ""));
            char *artifacts = NULL;
            rc = cc_media_artifact_list_to_json(&result->artifacts, &artifacts);
            if (rc.code != CC_OK) { cc_json_destroy(tr); break; }
            cc_json_object_set(tr, "artifacts", cc_json_create_string(artifacts ? artifacts : "[]"));
            free(artifacts);
            cc_json_array_append(tool_results, tr);
            rc = cc_result_ok();
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
    rc = save_store(store);
    cc_mutex_unlock(store->mutex);
    return rc;
}

/* JSON 文件 session store vtable，把持久化实现绑定到 cc_session_store_t 端口。 */
static cc_session_store_vtable_t json_file_vtable = {
    json_file_create_session,
    json_file_append_records,
    json_file_load_messages,
    json_file_list_sessions,
    json_file_clear_session,
    json_file_destroy
};

/*
 * 创建 JSON 文件 session store。
 *
 * file_path 为空时使用默认路径；成功后 out_store 获得 self/vtable。该函数会确保文件
 * 存在但不负责创建父目录，父目录创建属于平台/filesystem 初始化职责。
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
