#include "cc/adapters/cc_json_segmented_history.h"
#include "cc/internal/cc_alloc.h"
#include "cc_segmented_cursor.h"
#include "cc_segmented_integrity.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 未配对表达到上限时返回错误，限制嵌入式设备扫描孤立结果的内存占用。 */
#define RECOVERY_MAX_UNMATCHED_RESULTS 1024U
#define RECOVERY_PAGE_SIZE 32U
#define RECOVERY_CALL_FILTER_BYTES 8192U

typedef struct result_id_count {
    char *id;
    size_t count;
    cc_message_tool_outcome_t outcome;
} result_id_count_t;

typedef struct result_ids {
    result_id_count_t *items;
    size_t count;
    size_t capacity;
} result_ids_t;

/**
 * @brief 释放本轮反向扫描中拥有的结果/核对 ID 表。
 * @param[in,out] ids 必需的独占表借用，消费全部 ID 和数组分配，返回后禁止再次清理。
 * @return 无返回值。
 * @note 只由会话扫描的统一退出路径调用，调用者负责表结构本身的生命周期。
 */
static void result_ids_cleanup(result_ids_t *ids)
{
    for (size_t i = 0; i < ids->count; i++) free(ids->items[i].id);
    free(ids->items);
}

/**
 * @brief 为反向扫描中遇到的结果 ID 增加待配对计数。
 * @param[in,out] ids 独占的拥有表借用，必要时扩容，失败保留已有项。
 * @param[in] id 1 至 512 字节的 NUL 结尾标识借用，新项持有其独立副本。
 * @return 成功或无效历史 ID、计数溢出、1024 项上限、内存分配错误。
 * @note 调用者释放结果诊断，新项仅在复制 ID 成功后计入 count。
 */
static cc_result_t add_result(result_ids_t *ids, const char *id)
{
    if (!id || !id[0] || strlen(id) > 512U)
        return cc_segmented_integrity_error("Session contains a tool result without a valid id");
    for (size_t i = 0; i < ids->count; i++) {
        if (strcmp(ids->items[i].id, id) == 0) {
            if (ids->items[i].count == SIZE_MAX)
                return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Tool result count overflow");
            ids->items[i].count++;
            return cc_result_ok();
        }
    }
    if (ids->count >= RECOVERY_MAX_UNMATCHED_RESULTS)
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Too many unmatched historical tool results");
    if (ids->count == ids->capacity) {
        size_t next = ids->capacity ? ids->capacity * 2U : 16U;
        result_id_count_t *items = realloc(ids->items, next * sizeof(*items));
        if (!items) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot scan historical tool results");
        ids->items = items;
        ids->capacity = next;
    }
    ids->items[ids->count].id = cc_copy_string(id);
    if (!ids->items[ids->count].id)
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy historical tool result id");
    ids->items[ids->count].count = 1;
    ids->count++;
    return cc_result_ok();
}

/**
 * @brief 消耗一次匹配结果，计数归零时释放 ID 并用末项填补槽位。
 * @param[in,out] ids 本轮扫描独占的待配对表借用。
 * @param[in] id 待匹配的调用 ID 借用，NULL 视为未命中。
 * @return 找到并消耗一次计数时为一，否则为零。
 * @note 表顺序无语义，本函数不分配，不保留输入借用。
 */
static int consume_result(result_ids_t *ids, const char *id)
{
    if (!id) return 0;
    for (size_t i = 0; i < ids->count; i++) {
        if (strcmp(ids->items[i].id, id) != 0) continue;
        if (--ids->items[i].count == 0) {
            free(ids->items[i].id);
            ids->count--;
            if (i != ids->count) ids->items[i] = ids->items[ids->count];
        }
        return 1;
    }
    return 0;
}

/**
 * @brief 记录人工核对结果并拒绝同一调用的相互冲突结论。
 * @param[in,out] ids 本轮扫描独占的核对表借用，新项保存独立 ID 副本。
 * @param[in] message 必需的已解码核对消息借用，调用方已筛选合法核对 outcome。
 * @return 成功、缺失 ID/结论冲突的完整性错误或增项错误，调用者释放诊断。
 * @note 同 ID 同结论的重复持久化视为幂等，不增加待配对计数。
 */
static cc_result_t add_reconciliation(result_ids_t *ids, const cc_message_t *message)
{
    const char *id = message->tool_call_id;
    if (!id || !id[0]) return cc_segmented_integrity_error("Reconciliation has no tool call id");
    for (size_t i = 0; i < ids->count; i++) {
        if (strcmp(ids->items[i].id, id) != 0) continue;
        if (ids->items[i].outcome != message->tool_outcome)
            return cc_segmented_integrity_error("Conflicting tool reconciliations");
        return cc_result_ok(); /* 相同核对结论重试持久化。 */
    }
    cc_result_t rc = add_result(ids, id);
    if (rc.code == CC_OK) ids->items[ids->count - 1U].outcome = message->tool_outcome;
    return rc;
}

/**
 * @brief 在固定空间的四位过滤器中记录 ID，判断是否需要精确重扫历史。
 * @param[in,out] filter 本次扫描独占、初始清零的 8192 字节位数组借用。
 * @param[in] id 已验证非空的 NUL 结尾调用 ID 借用。
 * @return 本次设置前四个位均已存在时为一，否则为零。
 * @note 哈希碰撞会增加精确扫描次数，重复 ID 总设置相同位，因此不会漏掉重复项。
 *       调用者在返回一时执行全历史唯一性核对，函数不分配和保留借用。
 */
static int id_filter_seen_or_add(unsigned char *filter, const char *id)
{
    uint64_t first = UINT64_C(1469598103934665603);
    for (const unsigned char *p = (const unsigned char *)id; *p; p++) {
        first ^= *p;
        first *= UINT64_C(1099511628211);
    }
    uint64_t second = first ^ (first >> 33);
    second *= UINT64_C(0xff51afd7ed558ccd);
    second ^= second >> 33;
    second |= 1U;
    int seen = 1;
    for (unsigned i = 0; i < 4; i++) {
        size_t bit = (size_t)((first + (uint64_t)i * second) %
                              (RECOVERY_CALL_FILTER_BYTES * 8U));
        unsigned char mask = (unsigned char)(1U << (bit % 8U));
        if (!(filter[bit / 8U] & mask)) seen = 0;
        filter[bit / 8U] |= mask;
    }
    return seen;
}

/**
 * @brief 全历史精确统计一个工具意图或结果 ID，验证过滤器命中的候选项。
 * @param[in] store 存活至游标销毁的分段存储借用。
 * @param[in] session_id 非空会话标识借用。
 * @param[in] id 待核对的非空调用标识借用。
 * @param[in] intent 非零统计助手消息中的工具调用，为零统计工具结果消息。
 * @return 恰好一次匹配时成功，其他情况返回完整性错误，游标/读取错误原样传播。
 * @note 最多每页解码 32 条消息，发现第二次匹配即可结束。回收全部页面及游标。
 *       同会话并发写入使游标失效，调用者释放结果诊断并禁止并发销毁存储。
 */
static cc_result_t check_unique_historical_reference(cc_session_store_t *store,
                                                     const char *session_id, const char *id,
                                                     int intent)
{
    cc_segmented_cursor_t *cursor = NULL;
    cc_result_t rc = cc_segmented_cursor_open(store, session_id, &cursor);
    size_t count = RECOVERY_PAGE_SIZE;
    size_t matches = 0;
    while (rc.code == CC_OK && count && matches < 2U) {
        cc_message_t *messages = NULL;
        count = 0;
        rc = cc_segmented_cursor_next(cursor, RECOVERY_PAGE_SIZE, &messages, &count);
        for (size_t i = 0; rc.code == CC_OK && i < count && matches < 2U; i++) {
            if (intent && messages[i].role == CC_ROLE_ASSISTANT) {
                for (size_t j = 0; j < messages[i].tool_calls.count; j++) {
                    const char *candidate = messages[i].tool_calls.items[j].id;
                    if (candidate && strcmp(candidate, id) == 0 && ++matches >= 2U) break;
                }
            } else if (!intent && messages[i].role == CC_ROLE_TOOL && messages[i].tool_call_id &&
                       strcmp(messages[i].tool_call_id, id) == 0) {
                matches++;
            }
        }
        for (size_t i = 0; i < count; i++) cc_message_cleanup(&messages[i]);
        free(messages);
    }
    cc_segmented_cursor_destroy(cursor);
    if (rc.code == CC_OK && matches != 1U)
        return cc_segmented_integrity_error("Historical tool call or result id is ambiguous");
    return rc;
}

/**
 * @brief 用反向物理游标逐页访问全会话消息并回收每页。
 * @param[in] store 必需的分段 JSON 后端借用，调用期间保持存活。
 * @param[in] session_id 非空会话标识借用。
 * @param[in] visitor 必需的同步访问回调，返回失败时停止扫描。
 * @param[in,out] context 原样借给回调的上下文，允许 NULL。
 * @return 成功或首个参数、游标、读取、访问者错误，调用者释放诊断。
 * @note 回调仅在调用期间借用消息且在存储锁外执行，异步保留须深复制。
 *       同会话写入使后续页读取失败，所有退出路径释放当前页和游标。
 */
cc_result_t cc_json_segmented_store_visit_messages(
    cc_session_store_t *store, const char *session_id,
    cc_run_history_message_visitor_fn visitor, void *context)
{
    if (!cc_json_segmented_store_is_instance(store) || !session_id || !session_id[0] || !visitor)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid recovery history traversal");
    cc_segmented_cursor_t *cursor = NULL;
    cc_result_t rc = cc_segmented_cursor_open(store, session_id, &cursor);
    size_t count = RECOVERY_PAGE_SIZE;
    while (count && rc.code == CC_OK) {
        cc_message_t *messages = NULL;
        count = 0;
        rc = cc_segmented_cursor_next(cursor, RECOVERY_PAGE_SIZE, &messages, &count);
        for (size_t i = 0; rc.code == CC_OK && i < count; i++)
            rc = visitor(&messages[i], context);
        for (size_t i = 0; i < count; i++) cc_message_cleanup(&messages[i]);
        free(messages);
    }
    cc_segmented_cursor_destroy(cursor);
    return rc;
}

/**
 * @brief 扫描全会话并转移首个有效匹配消息，同时核对后续重复和冲突。
 * @param[in] store 存活至扫描退出的分段 JSON 存储借用。
 * @param[in] session_id 非空会话标识借用。
 * @param[in] match 消息 ID、工具意图或已确定工具结果的查找模式。
 * @param[in] id 非空消息或工具调用标识借用。
 * @param[out] out_message 必需的新/已清理槽，合法参数入口清零，成功命中后拥有消息。
 * @param[out] out_found 必需的命中槽，合法入口和失败退出置零，成功命中为一。
 * @return 完整扫描成功或参数、读取、重复 ID/结果、核对冲突错误，调用者释放诊断。
 * @note 未知工具占位不作为确定结果，人工核对可命中。先从页中移出消息，再清理页面。
 *       任何后续错误回收已移出的输出，成功输出由调用者 cc_message_cleanup。
 *       同会话写入使游标失效，调用者禁止并发销毁存储。
 */
cc_result_t cc_json_segmented_store_find_message(
    cc_session_store_t *store, const char *session_id, cc_run_history_match_t match,
    const char *id, cc_message_t *out_message, int *out_found)
{
    if (!store || !session_id || !session_id[0] || !id || !id[0] ||
        !out_message || !out_found ||
        (match != CC_RUN_HISTORY_MESSAGE_ID && match != CC_RUN_HISTORY_TOOL_INTENT &&
         match != CC_RUN_HISTORY_TOOL_RESULT))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid recovery history lookup");
    memset(out_message, 0, sizeof(*out_message));
    *out_found = 0;
    cc_segmented_cursor_t *cursor = NULL;
    cc_result_t rc = cc_segmented_cursor_open(store, session_id, &cursor);
    size_t count = RECOVERY_PAGE_SIZE;
    size_t intent_count = 0, tool_result_count = 0;
    int note_seen = 0;
    cc_message_tool_outcome_t note_outcome = CC_MESSAGE_TOOL_OUTCOME_RECORDED;
    while (count && rc.code == CC_OK) {
        cc_message_t *messages = NULL;
        count = 0;
        rc = cc_segmented_cursor_next(cursor, RECOVERY_PAGE_SIZE, &messages, &count);
        for (size_t i = 0; rc.code == CC_OK && i < count; i++) {
            cc_message_t *message = &messages[i];
            int found = match == CC_RUN_HISTORY_MESSAGE_ID && message->id &&
                strcmp(message->id, id) == 0;
            if (match == CC_RUN_HISTORY_TOOL_RESULT && message->tool_call_id &&
                strcmp(message->tool_call_id, id) == 0) {
                if (message->role == CC_ROLE_TOOL) {
                    if (++tool_result_count > 1U) {
                        rc = cc_result_error(CC_ERR_STORAGE, "Repeated historical tool result id");
                        break;
                    }
                    found = !cc_message_tool_outcome_is_unknown(message);
                } else if (message->role == CC_ROLE_USER &&
                           (message->tool_outcome == CC_MESSAGE_TOOL_OUTCOME_RECONCILED_EXECUTED ||
                            message->tool_outcome == CC_MESSAGE_TOOL_OUTCOME_RECONCILED_NOT_EXECUTED)) {
                    if (note_seen && note_outcome != message->tool_outcome) {
                        rc = cc_result_error(CC_ERR_STORAGE, "Conflicting tool reconciliations");
                        break;
                    }
                    note_seen = 1;
                    note_outcome = message->tool_outcome;
                    found = 1;
                }
            }
            if (match == CC_RUN_HISTORY_TOOL_INTENT && message->role == CC_ROLE_ASSISTANT) {
                for (size_t j = 0; j < message->tool_calls.count; j++) {
                    if (message->tool_calls.items[j].id &&
                        strcmp(message->tool_calls.items[j].id, id) == 0) {
                        if (++intent_count > 1U) {
                            rc = cc_result_error(CC_ERR_STORAGE, "Repeated historical tool call id");
                            break;
                        }
                        found = 1;
                    }
                }
            }
            if (rc.code != CC_OK) break;
            if (found) {
                if (*out_found && match == CC_RUN_HISTORY_MESSAGE_ID) {
                    rc = cc_result_error(CC_ERR_STORAGE, "Repeated historical message id");
                    break;
                }
                if (!*out_found) {
                    *out_message = *message;
                    memset(message, 0, sizeof(*message));
                    *out_found = 1;
                }
            }
        }
        for (size_t i = 0; i < count; i++) cc_message_cleanup(&messages[i]);
        free(messages);
    }
    cc_segmented_cursor_destroy(cursor);
    if (rc.code != CC_OK) {
        cc_message_cleanup(out_message);
        *out_found = 0;
    }
    return rc;
}

/**
 * @brief 反向扫描每个会话，配对工具结果与意图并导入未知调用。
 * @param[in] store 必需的分段 JSON 存储借用，应在接纳新工具调用前执行扫描。
 * @param[in,out] journal 存活的操作账本借用，按调用 ID 幂等持久化未知记录。
 * @param[out] out_unresolved_calls 必需的计数槽，合法入口清零，包含遇到的已导入项。
 * @return 全部扫描成功或首个参数、分配、读取、完整性、账本导入错误，调用者释放诊断。
 * @note 每会话释放游标、两个 8 KiB 过滤器及两张至多 1024 项的 ID 表。
 *       过滤器可能命中时执行精确重扫，结果未知占位保留对应意图的核对需求。
 *       失败可能已导入部分记录，计数仅累计此前成功项。拥有者协调清除/人工核对并禁止销毁。
 */
cc_result_t cc_json_segmented_store_import_interrupted_calls(
    cc_session_store_t *store, cc_operation_journal_t *journal, size_t *out_unresolved_calls)
{
    if (!cc_json_segmented_store_is_instance(store) || !store->vtable->list_sessions || !journal ||
        !out_unresolved_calls)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid historical recovery scan");
    *out_unresolved_calls = 0;
    cc_session_t *sessions = NULL;
    size_t session_count = 0;
    cc_result_t rc = store->vtable->list_sessions(store->self, &sessions, &session_count);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    for (size_t si = 0; si < session_count && rc.code == CC_OK; si++) {
        result_ids_t results = {0}, reconciliations = {0};
        unsigned char *call_filter = calloc(2U, RECOVERY_CALL_FILTER_BYTES);
        if (!call_filter) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate historical tool-id filters");
            break;
        }
        unsigned char *result_filter = call_filter + RECOVERY_CALL_FILTER_BYTES;
        cc_segmented_cursor_t *cursor = NULL;
        rc = cc_segmented_cursor_open(store, sessions[si].id, &cursor);
        size_t count = RECOVERY_PAGE_SIZE;
        while (count && rc.code == CC_OK) {
            cc_message_t *messages = NULL;
            count = 0;
            rc = cc_segmented_cursor_next(cursor, RECOVERY_PAGE_SIZE, &messages, &count);
            for (size_t mi = 0; rc.code == CC_OK && mi < count; mi++) {
                const cc_message_t *message = &messages[mi];
                if (message->role == CC_ROLE_TOOL) {
                    const char *id = message->tool_call_id;
                    if (!id || !id[0] || strlen(id) > 512U) {
                        rc = cc_segmented_integrity_error("Historical tool result has an invalid id");
                        break;
                    }
                    if (id_filter_seen_or_add(result_filter, id)) {
                        rc = check_unique_historical_reference(store, sessions[si].id, id, 0);
                        if (rc.code != CC_OK) break;
                    }
                }
                if (message->role == CC_ROLE_USER &&
                    (message->tool_outcome == CC_MESSAGE_TOOL_OUTCOME_RECONCILED_EXECUTED ||
                     message->tool_outcome == CC_MESSAGE_TOOL_OUTCOME_RECONCILED_NOT_EXECUTED)) {
                    rc = add_reconciliation(&reconciliations, message);
                } else if (message->role == CC_ROLE_TOOL && cc_message_tool_outcome_is_unknown(message)) {
                    if (!message->tool_call_id || !message->tool_call_id[0])
                        rc = cc_segmented_integrity_error("Unknown tool result has no matching call id");
                } else if (message->role == CC_ROLE_TOOL) {
                    /* 账本移除失败时，已持久化结果可能同时带有人工核对消息。
                     * 先消费核对项，让同一调用只通过结果表配对一次。 */
                    (void)consume_result(&reconciliations, message->tool_call_id);
                    rc = add_result(&results, message->tool_call_id);
                } else if (message->role == CC_ROLE_ASSISTANT) {
                    for (size_t ti = message->tool_calls.count; rc.code == CC_OK && ti > 0; ti--) {
                        const cc_tool_call_t *call = &message->tool_calls.items[ti - 1U];
                        if (!call->id || !call->id[0] || !call->name || !call->name[0] ||
                            !call->arguments_json || strlen(call->id) > 512U) {
                            rc = cc_segmented_integrity_error("Historical tool call has invalid fields");
                            break;
                        }
                        if (id_filter_seen_or_add(call_filter, call->id)) {
                            rc = check_unique_historical_reference(store, sessions[si].id, call->id, 1);
                            if (rc.code != CC_OK) break;
                        }
                        if (consume_result(&results, call->id) || consume_result(&reconciliations, call->id)) continue;
                        char *operation_id = NULL;
                        rc = cc_operation_journal_import_unknown(journal, sessions[si].id,
                                                                 call, &operation_id);
                        free(operation_id);
                        if (rc.code == CC_OK) (*out_unresolved_calls)++;
                    }
                }
            }
            for (size_t mi = 0; mi < count; mi++) cc_message_cleanup(&messages[mi]);
            free(messages);
        }
        if (rc.code == CC_OK && (results.count != 0 || reconciliations.count != 0)) {
            rc = cc_segmented_integrity_error("Historical tool result has no matching call");
        }
        cc_segmented_cursor_destroy(cursor);
        free(call_filter);
        result_ids_cleanup(&results);
        result_ids_cleanup(&reconciliations);
    }
    cc_session_free_array(sessions, session_count);
    return rc;
}
