#ifndef CC_RUN_HISTORY_H
#define CC_RUN_HISTORY_H

#include "cc/ports/cc_session_store.h"

/* 恢复查询的匹配目标。实现须扫描完整历史并传播损坏，避免重复执行外部工具。
 * TOOL_INTENT 匹配助手消息内工具调用的 ID，TOOL_RESULT 跳过结果未知的占位消息，
 * 可以返回 tool_call_id 匹配且 tool_outcome 明确的用户人工核对消息。 */
typedef enum cc_run_history_match {
    CC_RUN_HISTORY_MESSAGE_ID = 1,
    CC_RUN_HISTORY_TOOL_INTENT = 2,
    CC_RUN_HISTORY_TOOL_RESULT = 3
} cc_run_history_match_t;

/**
 * @brief 扫描一个会话的完整历史并返回匹配消息的独立拥有副本。
 * @param[in] store 调用期间借用的存储接口，具体实现负责其分页和一致性约束。
 * @param[in] session_id 必需的非空会话标识借用。
 * @param[in] match 消息 ID、工具意图或已确定工具结果的匹配模式。
 * @param[in] id 必需的非空消息或工具调用 ID 借用。
 * @param[out] out_message 调用者提供的新/已清理槽，命中副本最终由 cc_message_cleanup 释放。
 * @param[out] out_found 必需的命中标志，成功无匹配为零，命中为一。
 * @return 完整扫描成功或存储、完整性、复制错误，结果诊断由调用者释放。
 * @note 查找范围包含全部页。具体后端在合法参数入口初始化输出，错误后的输出按其契约
 *       清理。调用者禁止并发销毁存储，实现须明确扫描与追加/清除的同步关系。
 */
typedef cc_result_t (*cc_run_history_find_fn)(
    cc_session_store_t *store, const char *session_id, cc_run_history_match_t match,
    const char *id, cc_message_t *out_message, int *out_found);

/**
 * @brief 同步访问完整历史扫描中的一条消息。
 * @param[in] message 仅本次回调期间有效的只读借用消息。
 * @param[in,out] context 调用者拥有的借用上下文。
 * @return 成功继续扫描，其他结果停止扫描并由扫描调用者消费诊断。
 * @note 异步保存必须深复制，回调须遵守后端扫描期间的重入约束。
 */
typedef cc_result_t (*cc_run_history_message_visitor_fn)(const cc_message_t *message, void *context);
/**
 * @brief 通过有界分页同步访问完整会话，逐条借出消息。
 * @param[in] store 调用期间存活的存储接口借用。
 * @param[in] session_id 非空会话标识借用。
 * @param[in] visitor 每条消息调用的同步访问者。
 * @param[in,out] context 原样借给访问者的上下文，允许 NULL。
 * @return 完整扫描成功或存储/访问者错误，调用者释放结果诊断。
 * @note 可选恢复端口不承诺访问顺序，支持分段后端的有界内存核对。
 *       拥有者保证存储在扫描和所有访问回调退出前继续存活。
 */
typedef cc_result_t (*cc_run_history_visit_fn)(cc_session_store_t *store, const char *session_id,
    cc_run_history_message_visitor_fn visitor, void *context);

#endif
