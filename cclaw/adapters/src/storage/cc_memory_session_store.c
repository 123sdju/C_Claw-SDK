



#include "cc/ports/cc_session_store.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_thread.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#ifndef CC_DEFAULT_WORKSPACE_PATH
#define CC_DEFAULT_WORKSPACE_PATH "runtime/workspace"
#endif

#define INITIAL_CAPACITY 64

/** @brief 拥有调用身份、会话、参数及 completed 状态副本的记录。 */
typedef struct {
    char *id;
    char *session_id;
    char *name;
    char *arguments_json;
    char *status;
} cc_memory_tool_call_record_t;

/** @brief 工具结果的独立文本和媒体 JSON 副本，本端口未提供单独的工具记录读取方法。 */
typedef struct {
    char *id;
    char *session_id;
    char *tool_call_id;
    int ok;
    char *text;
    char *error;
    char *metadata;
    char *artifacts;
} cc_memory_tool_result_record_t;

/** @brief 一把 mutex 保护四个拥有数组，批次先锁外复制再锁内一次发布，不落盘。 */
typedef struct {
    cc_message_t *messages;
    size_t message_count;
    size_t message_cap;

    cc_session_t *sessions;
    size_t session_count;
    size_t session_cap;

    cc_memory_tool_call_record_t *tool_calls;
    size_t tool_call_count;
    size_t tool_call_cap;

    cc_memory_tool_result_record_t *tool_results;
    size_t tool_result_count;
    size_t tool_result_cap;

    cc_mutex_t mutex;
} cc_memory_session_store_t;

/**
 * @brief 为内部工具记录复制字符串，缺失值保存为空串。
 * @param[in] value 调用期稳定借用，可为 NULL。
 * @return 独占字符串，调用者 free，分配失败为 NULL。
 */
static char *dup_required_string(const char *value)
{
    return cc_copy_string(value ? value : "");
}

/**
 * @brief 消费五个工具调用字段并清零槽位。
 * @param[in,out] record 可空独占记录，非空字段各自拥有独立分配。
 * @post 字段归还，记录存储继续由 owner 管理。
 */
static void free_tool_call_record(cc_memory_tool_call_record_t *record)
{
    if (!record) return;
    free(record->id);
    free(record->session_id);
    free(record->name);
    free(record->arguments_json);
    free(record->status);
    memset(record, 0, sizeof(*record));
}

/**
 * @brief 消费工具结果的六个普通字符串及媒体 JSON 后清零。
 * @param[in,out] record 可空独占记录，调用者持数组锁或拥有局部副本。
 * @post 动态字段归还，结构存储保留。
 */
static void free_tool_result_record(cc_memory_tool_result_record_t *record)
{
    if (!record) return;
    free(record->id);
    free(record->session_id);
    free(record->tool_call_id);
    free(record->text);
    free(record->error);
    free(record->metadata);
    free(record->artifacts);
    memset(record, 0, sizeof(*record));
}

/**
 * @brief 为一组已复制记录预留三个数组的提交容量。
 * @param[in,out] store 非 NULL 状态，调用者持有其 mutex，数组容量与实际分配一致。
 * @param[in] message_count 本批新增消息项数。
 * @param[in] tool_call_count 本批新增调用项数。
 * @param[in] tool_result_count 本批新增结果项数。
 * @return 成功为 CC_OK，计数或字节容量溢出为 LIMIT_EXCEEDED，realloc 失败为 OUT_OF_MEMORY。
 * @note 失败可保留前面已扩大的容量，三个有效计数与原记录始终不变。
 */
static cc_result_t ensure_batch_capacity(
    cc_memory_session_store_t *store,
    size_t message_count,
    size_t tool_call_count,
    size_t tool_result_count)
{
    size_t required_messages = store->message_count + message_count;
    size_t required_calls = store->tool_call_count + tool_call_count;
    size_t required_results = store->tool_result_count + tool_result_count;
    size_t max_messages = SIZE_MAX / sizeof(*store->messages);
    size_t max_calls = SIZE_MAX / sizeof(*store->tool_calls);
    size_t max_results = SIZE_MAX / sizeof(*store->tool_results);
    if (required_messages < store->message_count || required_messages > max_messages ||
        required_calls < store->tool_call_count || required_calls > max_calls ||
        required_results < store->tool_result_count || required_results > max_results) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Session batch size overflow");
    }

    if (required_messages > store->message_cap) {
        size_t cap = store->message_cap ? store->message_cap : INITIAL_CAPACITY;
        while (cap < required_messages) {
            cap = cap > max_messages / 2 ? max_messages : cap * 2;
        }
        cc_message_t *items = realloc(store->messages, cap * sizeof(*items));
        if (!items) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow messages");
        memset(items + store->message_cap, 0, (cap - store->message_cap) * sizeof(*items));
        store->messages = items;
        store->message_cap = cap;
    }
    if (required_calls > store->tool_call_cap) {
        size_t cap = store->tool_call_cap ? store->tool_call_cap : INITIAL_CAPACITY;
        while (cap < required_calls) {
            cap = cap > max_calls / 2 ? max_calls : cap * 2;
        }
        cc_memory_tool_call_record_t *items = realloc(store->tool_calls, cap * sizeof(*items));
        if (!items) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow tool calls");
        memset(items + store->tool_call_cap, 0, (cap - store->tool_call_cap) * sizeof(*items));
        store->tool_calls = items;
        store->tool_call_cap = cap;
    }
    if (required_results > store->tool_result_cap) {
        size_t cap = store->tool_result_cap ? store->tool_result_cap : INITIAL_CAPACITY;
        while (cap < required_results) {
            cap = cap > max_results / 2 ? max_results : cap * 2;
        }
        cc_memory_tool_result_record_t *items = realloc(store->tool_results, cap * sizeof(*items));
        if (!items) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow tool results");
        memset(items + store->tool_result_cap, 0, (cap - store->tool_result_cap) * sizeof(*items));
        store->tool_results = items;
        store->tool_result_cap = cap;
    }
    return cc_result_ok();
}

/**
 * @brief 持锁确保会话存在，新会话完整复制后才增加计数。
 * @param[in,out] self 非 NULL 存活状态。
 * @param[in] session_id 非 NULL 调用期稳定 ID，空串沿原有内存后端语义接受。
 * @param[in] workspace_dir 可空借用路径，NULL 使用编译期默认工作目录，空串保留。
 * @return 已存在或创建成功为 CC_OK，空参数为 INVALID_ARGUMENT，容量/OOM 返回对应结果。
 * @note 已有会话保持原工作目录，失败不发布半记录，也不创建实际目录。
 */
static cc_result_t memory_create_session(
    void *self,
    const char *session_id,
    const char *workspace_dir
)
{
    cc_memory_session_store_t *store = (cc_memory_session_store_t *)self;
    if (!store || !session_id)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory session create request");
    cc_mutex_lock(store->mutex);
    for (size_t i = 0; i < store->session_count; i++) {
        if (store->sessions[i].id && strcmp(store->sessions[i].id, session_id) == 0) {
            cc_mutex_unlock(store->mutex);
            return cc_result_ok();
        }
    }

    if (store->session_count >= store->session_cap) {
        size_t maximum = SIZE_MAX / sizeof(*store->sessions);
        if (store->session_cap >= maximum) {
            cc_mutex_unlock(store->mutex);
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Session capacity overflow");
        }
        size_t new_cap = store->session_cap > maximum / 2 ? maximum : store->session_cap * 2;
        if (new_cap == 0) new_cap = maximum < INITIAL_CAPACITY ? maximum : INITIAL_CAPACITY;
        cc_session_t *new_sessions = realloc(store->sessions, new_cap * sizeof(cc_session_t));
        if (!new_sessions) {
            cc_mutex_unlock(store->mutex);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow sessions");
        }
        store->sessions = new_sessions;
        store->session_cap = new_cap;
    }

    cc_session_t *s = &store->sessions[store->session_count];
    memset(s, 0, sizeof(cc_session_t));
    s->id = cc_copy_string(session_id);
    s->workspace_dir = workspace_dir ? cc_copy_string(workspace_dir) : cc_copy_string(CC_DEFAULT_WORKSPACE_PATH);
    if (!s->id || !s->workspace_dir) {
        free(s->id);
        free(s->workspace_dir);
        memset(s, 0, sizeof(*s));
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy session fields");
    }
    s->status = CC_SESSION_ACTIVE;
    store->session_count++;

    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/**
 * @brief 在锁内复制指定会话最近的消息，结果保持原追加顺序。
 * @param[in] self 非 NULL 存活状态，调用期间借用。
 * @param[in] session_id 非 NULL 稳定过滤键，按消息自身 session_id 匹配。
 * @param[in] limit 正数保留最近若干项，零或负数不限制。
 * @param[out] out_messages 必填新拥有槽，基础参数有效后先置 NULL，成功由调用者逐项 cleanup 后 free。
 * @param[out] out_count 必填数量槽，基础参数有效后先置零，成功为实际复制项数。
 * @return 参数错误为 INVALID_ARGUMENT，数组或字段 OOM 返回错误且撤销全部输出，空会话成功返回空。
 */
static cc_result_t memory_load_messages(
    void *self,
    const char *session_id,
    int limit,
    cc_message_t **out_messages,
    size_t *out_count
)
{
    if (!self || !session_id || !out_messages || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid session message load request");
    }
    *out_messages = NULL;
    *out_count = 0;
    cc_memory_session_store_t *store = (cc_memory_session_store_t *)self;

    cc_mutex_lock(store->mutex);
    size_t total = 0;
    for (size_t i = 0; i < store->message_count; i++) {
        if (store->messages[i].session_id &&
            strcmp(store->messages[i].session_id, session_id) == 0) {
            total++;
        }
    }
    size_t count = limit > 0 && total > (size_t)limit ? (size_t)limit : total;
    size_t skip = total - count;
    if (count == 0) {
        cc_mutex_unlock(store->mutex);
        return cc_result_ok();
    }
    cc_message_t *result = calloc(count, sizeof(cc_message_t));
    if (!result) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate session messages");
    }

    size_t idx = 0;
    for (size_t i = 0; i < store->message_count && idx < count; i++) {
        if (store->messages[i].session_id &&
            strcmp(store->messages[i].session_id, session_id) == 0) {
            if (skip > 0) {
                skip--;
                continue;
            }
            cc_result_t rc = cc_message_copy(&store->messages[i], &result[idx]);
            if (rc.code != CC_OK) {
                for (size_t j = 0; j < idx; j++) cc_message_cleanup(&result[j]);
                free(result);
                cc_mutex_unlock(store->mutex);
                return rc;
            }
            cc_result_free(&rc);
            idx++;
        }
    }

    *out_messages = result;
    *out_count = idx;
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/**
 * @brief 持锁按创建顺序深拷贝全部会话描述。
 * @param[in] self 非 NULL 存活状态。
 * @param[out] out_sessions 必填新拥有槽，基础参数有效后先置 NULL，成功数组由调用者 session_free_array。
 * @param[out] out_count 必填数量槽，基础参数有效后先置零。
 * @return 成功为 CC_OK，参数或复制错误为对应结果，失败清理已复制会话且保持空输出。
 */
static cc_result_t memory_list_sessions(
    void *self,
    cc_session_t **out_sessions,
    size_t *out_count
)
{
    if (!self || !out_sessions || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid session list request");
    }
    *out_sessions = NULL;
    *out_count = 0;
    cc_memory_session_store_t *store = (cc_memory_session_store_t *)self;

    cc_mutex_lock(store->mutex);
    if (store->session_count == 0) {
        cc_mutex_unlock(store->mutex);
        return cc_result_ok();
    }
    cc_session_t *result = calloc(store->session_count, sizeof(cc_session_t));
    if (!result) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate sessions");
    }

    for (size_t i = 0; i < store->session_count; i++) {
        result[i].id = store->sessions[i].id ? cc_copy_string(store->sessions[i].id) : NULL;
        result[i].name = store->sessions[i].name ? cc_copy_string(store->sessions[i].name) : NULL;
        result[i].workspace_dir = store->sessions[i].workspace_dir ? cc_copy_string(store->sessions[i].workspace_dir) : NULL;
        result[i].model = store->sessions[i].model ? cc_copy_string(store->sessions[i].model) : NULL;
        result[i].created_at = store->sessions[i].created_at ? cc_copy_string(store->sessions[i].created_at) : NULL;
        result[i].updated_at = store->sessions[i].updated_at ? cc_copy_string(store->sessions[i].updated_at) : NULL;
        result[i].status = store->sessions[i].status;
        if ((store->sessions[i].id && !result[i].id) ||
            (store->sessions[i].name && !result[i].name) ||
            (store->sessions[i].workspace_dir && !result[i].workspace_dir) ||
            (store->sessions[i].model && !result[i].model) ||
            (store->sessions[i].created_at && !result[i].created_at) ||
            (store->sessions[i].updated_at && !result[i].updated_at)) {
            cc_session_free_array(result, i + 1U);
            cc_mutex_unlock(store->mutex);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy session list");
        }
    }

    *out_sessions = result;
    *out_count = store->session_count;
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/**
 * @brief 持锁移除指定会话的消息和工具记录，保留会话描述。
 * @param[in,out] self 非 NULL 存活状态。
 * @param[in] session_id 非 NULL 调用期稳定过滤键。
 * @return 参数错误为 INVALID_ARGUMENT，清理完成为 CC_OK，未命中同样成功。
 * @note 原地 MOVE 压缩数组并清零尾槽，保留全部数组容量。
 */
static cc_result_t memory_clear_session(void *self, const char *session_id)
{
    if (!self || !session_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid session clear request");
    }
    cc_memory_session_store_t *store = (cc_memory_session_store_t *)self;
    cc_mutex_lock(store->mutex);

    size_t write = 0;
    for (size_t read = 0; read < store->message_count; read++) {
        cc_message_t *message = &store->messages[read];
        if (message->session_id && strcmp(message->session_id, session_id) == 0) {
            cc_message_cleanup(message);
            continue;
        }
        if (write != read) store->messages[write] = store->messages[read];
        write++;
    }
    for (size_t i = write; i < store->message_count; i++) {
        memset(&store->messages[i], 0, sizeof(store->messages[i]));
    }
    store->message_count = write;

    write = 0;
    for (size_t read = 0; read < store->tool_call_count; read++) {
        cc_memory_tool_call_record_t *record = &store->tool_calls[read];
        if (record->session_id && strcmp(record->session_id, session_id) == 0) {
            free_tool_call_record(record);
            continue;
        }
        if (write != read) store->tool_calls[write] = store->tool_calls[read];
        write++;
    }
    for (size_t i = write; i < store->tool_call_count; i++) {
        memset(&store->tool_calls[i], 0, sizeof(store->tool_calls[i]));
    }
    store->tool_call_count = write;

    write = 0;
    for (size_t read = 0; read < store->tool_result_count; read++) {
        cc_memory_tool_result_record_t *record = &store->tool_results[read];
        if (record->session_id && strcmp(record->session_id, session_id) == 0) {
            free_tool_result_record(record);
            continue;
        }
        if (write != read) store->tool_results[write] = store->tool_results[read];
        write++;
    }
    for (size_t i = write; i < store->tool_result_count; i++) {
        memset(&store->tool_results[i], 0, sizeof(store->tool_results[i]));
    }
    store->tool_result_count = write;

    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/**
 * @brief 消费四类记录、数组、mutex 和后端状态。
 * @param[in,out] self 可空独占状态，owner 已关闭入口并排空全部方法。
 * @post 非空状态释放，端口描述符及其旧别名由调用者停止使用并清空。
 * @note 内部锁保护清理，不为仍准备进入方法的外部调用延长实例寿命。
 */
static void memory_destroy(void *self)
{
    cc_memory_session_store_t *store = (cc_memory_session_store_t *)self;
    if (!store) return;
    cc_mutex_lock(store->mutex);
    for (size_t i = 0; i < store->message_count; i++) {
        cc_message_cleanup(&store->messages[i]);
    }
    free(store->messages);
    for (size_t i = 0; i < store->session_count; i++) {
        free(store->sessions[i].id);
        free(store->sessions[i].name);
        free(store->sessions[i].workspace_dir);
        free(store->sessions[i].model);
        free(store->sessions[i].created_at);
        free(store->sessions[i].updated_at);
    }
    free(store->sessions);
    for (size_t i = 0; i < store->tool_call_count; i++) {
        free_tool_call_record(&store->tool_calls[i]);
    }
    free(store->tool_calls);
    for (size_t i = 0; i < store->tool_result_count; i++) {
        free_tool_result_record(&store->tool_results[i]);
    }
    free(store->tool_results);
    cc_mutex_unlock(store->mutex);
    cc_mutex_destroy(store->mutex);
    free(store);
}

/**
 * @brief 锁外制作完整批次副本，锁内预留容量后一次发布所有记录。
 * @param[in,out] self 非 NULL 存活状态，阶段复制期间也保持实例有效。
 * @param[in] records 稳定借用数组，count 为零时可为 NULL，消息按自身会话字段保存。
 * @param[in] count 实际记录数量，工具记录须有会话键，结果另须关联调用 ID。
 * @return 成功为 CC_OK，参数/类型、容量、复制或序列化失败返回对应拥有结果。
 * @note 失败不发布任何有效项，预留阶段可增容量。工具可空文本归为空串，调用状态保存 completed。
 * 本入口不创建会话描述，跨线程顺序以锁内发布点确定，原输入始终由调用者保管。
 */
static cc_result_t memory_append_records(
    void *self,
    const cc_session_record_t *records,
    size_t count
)
{
    if (!self || (!records && count > 0)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory store batch");
    }
    if (count == 0) return cc_result_ok();
    if (count > SIZE_MAX / sizeof(*records))
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Session record count overflow");

    size_t message_count = 0;
    size_t call_count = 0;
    size_t result_count = 0;
    for (size_t i = 0; i < count; i++) {
        switch (records[i].type) {
        case CC_SESSION_RECORD_MESSAGE: message_count++; break;
        case CC_SESSION_RECORD_TOOL_CALL: call_count++; break;
        case CC_SESSION_RECORD_TOOL_RESULT: result_count++; break;
        default: return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Unknown session record type");
        }
    }

    if (message_count > SIZE_MAX / sizeof(cc_message_t) ||
        call_count > SIZE_MAX / sizeof(cc_memory_tool_call_record_t) ||
        result_count > SIZE_MAX / sizeof(cc_memory_tool_result_record_t))
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Session batch allocation overflow");

    cc_message_t *messages = calloc(message_count ? message_count : 1, sizeof(*messages));
    cc_memory_tool_call_record_t *calls = calloc(call_count ? call_count : 1, sizeof(*calls));
    cc_memory_tool_result_record_t *results = calloc(result_count ? result_count : 1, sizeof(*results));
    if (!messages || !calls || !results) {
        free(messages);
        free(calls);
        free(results);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to prepare session batch");
    }

    size_t mi = 0;
    size_t ci = 0;
    size_t ri = 0;
    cc_result_t rc = cc_result_ok();
    for (size_t i = 0; i < count && rc.code == CC_OK; i++) {
        const cc_session_record_t *record = &records[i];
        if (record->type == CC_SESSION_RECORD_MESSAGE) {
            if (!record->data.message) {
                rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message in session batch");
                break;
            }
            rc = cc_message_copy(record->data.message, &messages[mi++]);
        } else if (record->type == CC_SESSION_RECORD_TOOL_CALL) {
            const cc_tool_call_t *call = record->data.tool_call;
            if (!call || !record->session_id) {
                rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid tool call in session batch");
                break;
            }
            cc_memory_tool_call_record_t *dst = &calls[ci++];
            dst->id = dup_required_string(call->id);
            dst->session_id = dup_required_string(record->session_id);
            dst->name = dup_required_string(call->name);
            dst->arguments_json = dup_required_string(call->arguments_json);
            dst->status = dup_required_string("completed");
            if (!dst->id || !dst->session_id || !dst->name ||
                !dst->arguments_json || !dst->status) {
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to prepare tool call batch");
            }
        } else {
            const cc_tool_result_t *result = record->data.tool_result.result;
            const char *tool_call_id = record->data.tool_result.tool_call_id;
            if (!result || !record->session_id || !tool_call_id) {
                rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid tool result in session batch");
                break;
            }
            cc_memory_tool_result_record_t *dst = &results[ri++];
            dst->id = dup_required_string(tool_call_id);
            dst->session_id = dup_required_string(record->session_id);
            dst->tool_call_id = dup_required_string(tool_call_id);
            dst->ok = result->ok;
            dst->text = dup_required_string(result->text);
            dst->error = dup_required_string(result->error);
            dst->metadata = dup_required_string(result->metadata);
            char *artifacts = NULL;
            rc = cc_media_artifact_list_to_json(&result->artifacts, &artifacts);
            if (rc.code == CC_OK) dst->artifacts = dup_required_string(artifacts ? artifacts : "[]");
            free(artifacts);
            if (rc.code == CC_OK && (!dst->id || !dst->session_id || !dst->tool_call_id ||
                !dst->text || !dst->error || !dst->metadata || !dst->artifacts)) {
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to prepare tool result batch");
            }
        }
    }

    if (rc.code == CC_OK) {
        cc_memory_session_store_t *store = (cc_memory_session_store_t *)self;
        cc_mutex_lock(store->mutex);
        rc = ensure_batch_capacity(store, message_count, call_count, result_count);
        if (rc.code == CC_OK) {
            for (size_t i = 0; i < message_count; i++) {
                store->messages[store->message_count++] = messages[i];
                memset(&messages[i], 0, sizeof(messages[i]));
            }
            for (size_t i = 0; i < call_count; i++) {
                store->tool_calls[store->tool_call_count++] = calls[i];
                memset(&calls[i], 0, sizeof(calls[i]));
            }
            for (size_t i = 0; i < result_count; i++) {
                store->tool_results[store->tool_result_count++] = results[i];
                memset(&results[i], 0, sizeof(results[i]));
            }
        }
        cc_mutex_unlock(store->mutex);
    }

    for (size_t i = 0; i < message_count; i++) cc_message_cleanup(&messages[i]);
    for (size_t i = 0; i < call_count; i++) free_tool_call_record(&calls[i]);
    for (size_t i = 0; i < result_count; i++) free_tool_result_record(&results[i]);
    free(messages);
    free(calls);
    free(results);
    return rc;
}

/** @brief 内存后端方法表，不提供延迟媒体加载扩展，普通消息读取已返回字段副本。 */
static const cc_session_store_vtable_t memory_vtable = {
    .create_session = memory_create_session,
    .append_records = memory_append_records,
    .load_messages = memory_load_messages,
    .list_sessions = memory_list_sessions,
    .clear_session = memory_clear_session,
    .destroy = memory_destroy,
    .load_message_metadata = NULL,
    .hydrate_context_media = NULL
};

/**
 * @brief 创建四个初始数组和一把 mutex，完成后才交付内存会话端口。
 * @param[out] out_store 必填新拥有槽，先清零，成功后由调用者排空并 destroy。
 * @return 成功为 CC_OK，空输出为 INVALID_ARGUMENT，分配或 mutex 创建失败归还全部局部资源并返回原错误。
 * @note 后端不落盘、不保留配置指针，多线程方法通过同一 mutex 保护内部数组。
 */
cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store)
{
    if (!out_store) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null memory session store output");
    }
    memset(out_store, 0, sizeof(*out_store));
    cc_memory_session_store_t *self = calloc(1, sizeof(cc_memory_session_store_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create memory store");

    self->messages = calloc(INITIAL_CAPACITY, sizeof(cc_message_t));
    self->message_cap = INITIAL_CAPACITY;

    self->sessions = calloc(INITIAL_CAPACITY, sizeof(cc_session_t));
    self->session_cap = INITIAL_CAPACITY;
    self->tool_calls = calloc(INITIAL_CAPACITY, sizeof(cc_memory_tool_call_record_t));
    self->tool_call_cap = INITIAL_CAPACITY;
    self->tool_results = calloc(INITIAL_CAPACITY, sizeof(cc_memory_tool_result_record_t));
    self->tool_result_cap = INITIAL_CAPACITY;
    if (!self->messages || !self->sessions || !self->tool_calls || !self->tool_results) {
        free(self->messages);
        free(self->sessions);
        free(self->tool_calls);
        free(self->tool_results);
        free(self);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate memory session store arrays");
    }
    cc_result_t rc = cc_mutex_create(&self->mutex);
    if (rc.code != CC_OK) {
        free(self->messages);
        free(self->sessions);
        free(self->tool_calls);
        free(self->tool_results);
        free(self);
        return rc;
    }

    out_store->self = self;
    out_store->vtable = &memory_vtable;
    return cc_result_ok();
}
