#ifndef CC_SEGMENTED_CURSOR_H
#define CC_SEGMENTED_CURSOR_H

#include "cc/ports/cc_session_store.h"

/* 内部恢复游标，从新到旧遍历物理位置，避免依赖可能重复的消息 ID。
 * 游标拥有一个分段的文本快照和当前解码批次，借用的存储必须活到游标销毁。
 * 同会话写入使游标失效，其他会话可以继续操作。 */
typedef struct cc_segmented_cursor cc_segmented_cursor_t;

/**
 * @brief 创建从会话最新分段开始的反向恢复游标。
 * @param[in] store 必须是分段 JSON 后端的存储借用，存活至游标销毁。
 * @param[in] session_id 非空会话标识借用，成功时游标持有其独立副本。
 * @param[out] out_cursor 必需的新输出槽，先置 NULL，成功后由调用者销毁。
 * @return 成功或参数、分配、目录、清单、分段读取错误，调用者释放诊断。
 * @note 建立快照和注册游标受存储互斥量保护。失败回收部分创建的资源。
 */
cc_result_t cc_segmented_cursor_open(cc_session_store_t *store, const char *session_id,
                                     cc_segmented_cursor_t **out_cursor);
/**
 * @brief 从游标当前位置取出至多 limit 条消息，页内按时间从新到旧排列。
 * @param[in,out] cursor 调用者拥有的有效游标，推进位置，读取失败后永久失效。
 * @param[in] limit 消息条数上限，范围为 1 至 256。
 * @param[out] out_messages 必需的新数组槽，合法参数入口置 NULL，成功后转移数组所有权。
 * @param[out] out_count 必需的条数槽，合法参数入口置零，成功为零表示已遍历完毕。
 * @return 成功或参数、游标失效、分配、读取、完整性错误，失败回收本页。
 * @note 读取受存储互斥量保护。返回数组由调用者逐项 cc_message_cleanup 后 free。
 *       同一游标的 next 与 destroy 必须由调用者串行化，存储禁止并发销毁。
 */
cc_result_t cc_segmented_cursor_next(cc_segmented_cursor_t *cursor, size_t limit,
                                     cc_message_t **out_messages, size_t *out_count);
/**
 * @brief 注销并释放游标及其尚未交付的分段和消息快照。
 * @param[in] cursor 待消费的拥有指针，允许 NULL，返回后禁止再次使用。
 * @note 注销受存储互斥量保护，存储必须仍存活，调用者等待该游标的 next 退出。
 * @return 无返回值。
 */
void cc_segmented_cursor_destroy(cc_segmented_cursor_t *cursor);

#endif
