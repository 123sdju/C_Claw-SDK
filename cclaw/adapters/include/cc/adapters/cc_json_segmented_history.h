#ifndef CC_JSON_SEGMENTED_HISTORY_H
#define CC_JSON_SEGMENTED_HISTORY_H

#include "cc/ports/cc_session_store.h"
#include "cc/app/cc_operation_journal.h"
#include "cc/app/cc_run_history.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 根据端口实例和固定虚表确认是否为分段 JSON 后端。
 * @param[in] store 只读存储端口借用，允许 NULL。
 * @return self 有效且虚表身份匹配时为一，否则为零，空库也可识别。
 * @note 不访问磁盘，调用者保证端口及实例存活，禁止并发销毁或改装端口。
 */
int cc_json_segmented_store_is_instance(const cc_session_store_t *store);
/**
 * @brief 比较分段后端创建时保存的根路径原文。
 * @param[in] store 分段存储端口只读借用，允许 NULL 或其他后端。
 * @param[in] path 待比较的 NUL 结尾路径借用，允许 NULL。
 * @return 后端类型及完整路径字节均匹配时为一，否则为零。
 * @note 采用区分大小写的精确比较，调用者保证端口和路径在调用期间存活。
 */
int cc_json_segmented_store_matches_path(const cc_session_store_t *store, const char *path);

/**
 * @brief 读取锚点之前的一页历史，返回消息按时间从旧到新排列。
 * @param[in] store 必须为分段 JSON 后端的存储借用，存活至调用退出。
 * @param[in] session_id 非空会话标识借用。
 * @param[in] before_message_id 排除于结果外的非空消息 ID，NULL 从最新消息开始。
 * @param[in] limit 本页消息上限，范围为 1 至 256。
 * @param[out] out_messages 必需的新数组槽，合法参数入口置 NULL，成功转移数组所有权。
 * @param[out] out_count 必需的条数槽，合法参数入口置零。
 * @param[out] out_has_more 必需的标志槽，合法参数入口置零，更早仍有消息时为一。
 * @return 成功或参数、锚点缺失/重复、清单/分段损坏、分配/读取错误，调用者释放诊断。
 * @note 下一页传本页首条消息的 ID。调用者逐项清理消息再 free 数组。
 *       指定锚点时扫描全会话核对唯一性。同会话写入使读取失效，禁止并发销毁存储。
 */
cc_result_t cc_json_segmented_store_load_page(
    cc_session_store_t *store, const char *session_id, const char *before_message_id,
    size_t limit, cc_message_t **out_messages, size_t *out_count, int *out_has_more);

/**
 * @brief 扫描全会话并查找唯一消息、工具意图或已确定的工具结果。
 * @param[in] store 分段 JSON 存储端口借用，存活至完整扫描退出。
 * @param[in] session_id 非空会话标识借用。
 * @param[in] match 消息 ID、工具调用意图或工具结果匹配模式。
 * @param[in] id 非空消息或调用标识借用。
 * @param[out] out_message 必需的新/已清理消息槽，合法入口清零，命中后拥有消息。
 * @param[out] out_found 必需的命中标志槽，合法入口置零，成功命中为一。
 * @return 完整扫描成功，或参数、重复匹配 ID/工具结果、核对冲突、存储错误。
 * @note 未知工具结果占位不算已确定结果，人工核对消息可作为结果。
 *       失败清理输出，调用者对成功命中调用 cc_message_cleanup，并释放结果诊断。
 *       分页间同会话写入会使扫描失败，调用者禁止并发销毁存储。
 */
cc_result_t cc_json_segmented_store_find_message(
    cc_session_store_t *store, const char *session_id, cc_run_history_match_t match,
    const char *id, cc_message_t *out_message, int *out_found);

/**
 * @brief 从新到旧访问全会话消息，按页借出解码后的消息。
 * @param[in] store 必须为分段 JSON 后端的存储借用。
 * @param[in] session_id 非空会话标识借用。
 * @param[in] visitor 必需的同步访问回调，消息只在本次回调内有效，异步保留须深复制。
 * @param[in,out] context 原样传给访问者的借用上下文，允许 NULL。
 * @return 完整扫描成功或首个访问者/存储错误，调用者释放诊断。
 * @note 存储须活到所有回调退出。回调在存储锁外运行，同会话写入会使后续读取失效。
 *       游标内存包含一个分段、当前解码批次和返回页，调用者禁止并发销毁存储。
 */
cc_result_t cc_json_segmented_store_visit_messages(
    cc_session_store_t *store, const char *session_id,
    cc_run_history_message_visitor_fn visitor, void *context);

/**
 * @brief 扫描现有全量历史，把缺少确定结果的助手调用导入待核对操作账本。
 * @param[in] store 必须为分段 JSON 后端且提供会话枚举的存储借用。
 * @param[in,out] journal 借用的操作账本，导入的未知记录由账本持久保存。
 * @param[out] out_unresolved_calls 必需的计数槽，合法入口清零，包含扫描遇到的已导入项。
 * @return 全部扫描成功或参数、分配、读取、完整性、账本错误，调用者释放诊断。
 * @note 调用应在接纳新工具调用前执行，拥有者协调会话清除和账本核对操作。
 *       失败可能已经导入部分未知项，重试按账本导入契约去重，计数保留已成功扫描的部分。
 *       每会话使用两个 8 KiB 位过滤器和最多各 1024 项的未配对结果/核对表。
 */
cc_result_t cc_json_segmented_store_import_interrupted_calls(
    cc_session_store_t *store, cc_operation_journal_t *journal, size_t *out_unresolved_calls);

#ifdef __cplusplus
}
#endif

#endif
