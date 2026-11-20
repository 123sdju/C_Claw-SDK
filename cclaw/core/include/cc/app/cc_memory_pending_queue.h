#ifndef CC_MEMORY_PENDING_QUEUE_H
#define CC_MEMORY_PENDING_QUEUE_H

#include "cc/core/cc_result.h"
#include <stddef.h>

typedef struct cc_memory_pending_queue cc_memory_pending_queue_t;
typedef struct cc_memory_pending_reservation cc_memory_pending_reservation_t;
/** @brief 拥有各字符串的对话副本，使用 cc_memory_pending_item_cleanup 释放。 */
typedef struct cc_memory_pending_item {
    char *turn_id; /**< 非空的对话稳定标识。 */
    char *session_id; /**< 非空的原会话标识。 */
    char *user_input; /**< 非空的原用户输入。 */
    char *assistant_text; /**< 答案，可为空字符串，未提交预留的副本中为空字符串。 */
    char *snapshot_id; /**< 已关联的快照 ID，无关联时为 NULL。 */
} cc_memory_pending_item_t;

/**
 * @brief 创建同时限制条数与计费字节的 RAM FIFO。
 *
 * 队列拥有输入副本，释放字符串前清零已使用正文。每项计入 64 字节快照 ID 预留，
 * 未提交项的答案按完整预留容量计费。结构体、固定槽数组、分配器开销与输出副本另占内存。
 * @note 各操作在队列锁下同步。调用者须串行管理同一预留句柄，并在销毁前排空所有调用与句柄使用者。
 * @param[in] max_items FIFO 项与尚未提交预留合计的最大条数，必须大于零。
 * @param[in] max_bytes 对话字符串及 snapshot ID 预留的总计费上限，必须大于零。
 * @param[out] out_queue 成功取得拥有的队列，参数合法后先置 NULL，须覆盖所有预留句柄寿命。
 * @return CC_OK 表示创建成功，非法限制返回 CC_ERR_INVALID_ARGUMENT，分配失败返回 CC_ERR_OUT_OF_MEMORY，锁创建错误原样返回。
 */
cc_result_t cc_memory_pending_queue_create(size_t max_items, size_t max_bytes,
                                            cc_memory_pending_queue_t **out_queue);
/**
 * @brief 外层关闭入口并排空调用/句柄使用者后销毁，清理 FIFO 与尚存 RAM 预留。
 *
 * 销毁后所有旧句柄及队列指针结束寿命。
 * @pre 队列调用已排空，外层已停止继续使用所有预留句柄。
 *
 * @param[in,out] queue 要销毁的队列，可为 NULL。
 * @post 所有 FIFO 项、预留、数组及 mutex 已释放，旧句柄结束寿命。
 */
void cc_memory_pending_queue_destroy(cc_memory_pending_queue_t *queue);
/**
 * @brief 在队列锁内查重、核算容量并深拷贝已完成输入/答案。
 *
 * FIFO 中相同 turn ID 且正文相同返回成功，内容冲突或尚有同 ID 预留报错。
 * 成功新项按尾部提交顺序排列，失败保留既有队列与调用者输入。
 *
 * @param[in,out] queue 接收已完成对话的队列，锁内查重并追加。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @param[in] session_id 原输入所属会话标识，调用期间借用。
 * @param[in] user_input 非空的原始用户输入，调用期间借用并深拷贝。
 * @param[in] assistant_text 完成答复，允许空字符串，调用期间借用并深拷贝。
 * @return 接纳或已有相同内容为 CC_OK，非法参数为 CC_ERR_INVALID_ARGUMENT，ID 冲突为 CC_ERR_INVALID_STATE，容量或长度溢出为 CC_ERR_LIMIT_EXCEEDED，复制失败为 CC_ERR_OUT_OF_MEMORY。
 */
cc_result_t cc_memory_pending_enqueue(cc_memory_pending_queue_t *queue,
                                      const char *turn_id, const char *session_id,
                                      const char *user_input, const char *assistant_text);
/**
 * @brief 模型运行前在队列锁内预分配输入副本及 max_answer_bytes+1 答案缓冲。
 *
 * 预留同时占用项数与字节上限，并登记到独立链表。
 * 分配失败清理本次内容。
 * 成功句柄交给调用者，直到 commit/cancel 消费前需串行管理。
 *
 * @note 队列已有相同 turn_id 时返回 CC_ERR_INVALID_STATE，即使内容一致也拒绝再次预留。
 * 预留在 commit 成功后进入可 peek 的 FIFO，FIFO 按提交顺序排列。
 * @param[in,out] queue 登记预留并承担其完整容量计费的队列。
 * @param[in] turn_id 非空的对话稳定标识，调用期间借用并复制。
 * @param[in] session_id 非空的原会话标识，调用期间借用并复制。
 * @param[in] user_input 非空的原用户输入，调用期间借用并复制。
 * @param[in] max_answer_bytes 答案正文容量，可为零，须小于 SIZE_MAX，额外分配一个 NUL 字节。
 * @param[out] out_reservation 非 NULL 时先置空，成功取得句柄，调用者串行使用，commit/cancel 成功后消费。
 * @return 成功为 CC_OK，非法参数为 CC_ERR_INVALID_ARGUMENT，重复 ID 为 CC_ERR_INVALID_STATE，容量或长度溢出为 CC_ERR_LIMIT_EXCEEDED，分配失败为 CC_ERR_OUT_OF_MEMORY。
 */
cc_result_t cc_memory_pending_reserve(cc_memory_pending_queue_t *queue,
    const char *turn_id, const char *session_id, const char *user_input,
    size_t max_answer_bytes, cc_memory_pending_reservation_t **out_reservation);
/**
 * @brief 核对预留归属，在字节预算内 realloc 答案缓冲并同步增加计费。
 *
 * 请求容量未增大直接成功，失败保留原缓冲、容量与预留身份。
 *
 * @param[in,out] queue 原预留所属队列，扩容增加其计费字节。
 * @param[in,out] reservation 仍属于该队列的预留句柄。
 * @param[in] max_answer_bytes 请求的答案正文容量，须小于 SIZE_MAX，实际缓冲额外保留一个 NUL 字节。
 * @return 容量足够或扩容成功为 CC_OK，非法参数为 CC_ERR_INVALID_ARGUMENT，句柄不属于队列为 CC_ERR_INVALID_STATE，容量不足为 CC_ERR_LIMIT_EXCEEDED，分配失败为 CC_ERR_OUT_OF_MEMORY。失败保留原预留及计费。
 */
cc_result_t cc_memory_pending_expand(cc_memory_pending_queue_t *queue,
    cc_memory_pending_reservation_t *reservation, size_t max_answer_bytes);
/**
 * @brief 锁内核对预留身份及原 turn/session/输入，参数调用期借用。
 *
 * @param[in] queue 预留应当归属的队列，内部加锁核对。
 * @param[in] reservation 仍属于该队列的预留句柄。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @param[in] session_id 原输入所属会话标识，调用期间借用。
 * @param[in] user_input 借用的原用户输入，仅用于逐字节核对。
 * @return 全部匹配为 CC_OK，NULL 参数为 CC_ERR_INVALID_ARGUMENT，句柄归属或任一字符串不匹配为 CC_ERR_INVALID_STATE。
 */
cc_result_t cc_memory_pending_verify(cc_memory_pending_queue_t *queue,
    cc_memory_pending_reservation_t *reservation, const char *turn_id,
    const char *session_id, const char *user_input);
/**
 * @brief 锁内复制仍登记的预留内容，成功拥有副本由调用者 cleanup。
 *
 * @note out_item 须无待释放资源。参数合法后先清零，复制失败时仍为空结构。
 * @param[in] queue 预留所属队列，内部加锁复制。
 * @param[in] reservation 仍属于该队列的预留句柄。
 * @param[out] out_item ID、会话及原输入的拥有副本，答案为空字符串，使用 cc_memory_pending_item_cleanup 清理。
 * @return 成功为 CC_OK，NULL 参数为 CC_ERR_INVALID_ARGUMENT，句柄不属于队列为 CC_ERR_INVALID_STATE，复制失败为 CC_ERR_OUT_OF_MEMORY。
 */
cc_result_t cc_memory_pending_inspect_reservation(cc_memory_pending_queue_t *queue,
    cc_memory_pending_reservation_t *reservation, cc_memory_pending_item_t *out_item);
/**
 * @brief 答案放入既有缓冲后，把项的拥有字段从预留移到 FIFO 尾部，无新增分配。
 *
 * 成功清空调用者句柄并释放预留壳，计费仍保留原容量，ack 时统一归还。
 * 超出答案容量或归属/计费错误时保留预留，输入答案为调用期间借用的字符串。
 *
 * @param[in,out] queue 预留所属队列，成功时从预留链表移入 FIFO。
 * @param[in,out] reservation 仍属于该队列的预留句柄，成功消费后置空。
 * @param[in] assistant_text 完成答复，允许空字符串，调用期间借用并复制到预留缓冲。
 * @return 成功为 CC_OK，非法参数为 CC_ERR_INVALID_ARGUMENT，答复超长为 CC_ERR_LIMIT_EXCEEDED，句柄归属或计费异常为 CC_ERR_INVALID_STATE。失败保留预留。
 */
cc_result_t cc_memory_pending_commit(cc_memory_pending_queue_t *queue,
    cc_memory_pending_reservation_t **reservation, const char *assistant_text);
/**
 * @brief 取消仍登记的预留，锁内摘链并归还计费，锁外清理拥有字符串及壳对象。
 *
 * 成功清空句柄，已消费的空句柄再次调用按参数错误处理。
 *
 * @param[in,out] queue 预留所属队列，成功时摘除预留并归还容量。
 * @param[in,out] reservation 仍属于该队列的预留句柄，成功消费后置空。
 * @return 成功为 CC_OK，queue、reservation 或 *reservation 为 NULL 时为 CC_ERR_INVALID_ARGUMENT，归属或计费异常为 CC_ERR_INVALID_STATE，失败保留预留。
 */
cc_result_t cc_memory_pending_cancel(cc_memory_pending_queue_t *queue,
    cc_memory_pending_reservation_t **reservation);
/**
 * @brief 锁内深拷贝 FIFO 队首，成功副本由调用者 cleanup，空 FIFO 返回 NOT_FOUND。
 *
 * 副本独立于队列寿命，其瞬时分配另计峰值 RAM。
 *
 * @note out_item 须无待释放资源。参数合法后先清零，空 FIFO 或复制失败时输出为空结构。
 * @param[in] queue 待读取的队列，内部加锁复制队首。
 * @param[out] out_item 成功取得拥有的独立项目副本，使用 cc_memory_pending_item_cleanup 清理。
 * @return 成功为 CC_OK，FIFO 无已提交项为 CC_ERR_NOT_FOUND，NULL 参数为 CC_ERR_INVALID_ARGUMENT，复制失败为 CC_ERR_OUT_OF_MEMORY。
 */
cc_result_t cc_memory_pending_peek(cc_memory_pending_queue_t *queue, cc_memory_pending_item_t *out_item);
/**
 * @brief 清理一个拥有项的全部字符串并清零结构，适用于副本与已移出的队列项。
 * @param[in,out] item 待清理的拥有项目，可为 NULL，字符串须分别可由 free 释放。
 * @post 拥有字符串的正文已清零并释放，item 结构已清零。
 */
void cc_memory_pending_item_cleanup(cc_memory_pending_item_t *item);
/**
 * @brief 先复制借用 snapshot ID，再调用 take 关联到已提交项，归还未转移的副本。
 *
 * 同项同 ID 可重复确认，实际 ID 长度在 take 中检查。
 *
 * @param[in,out] queue 包含目标已提交项的队列，内部加锁关联快照。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @param[in] snapshot_id 要复制关联的快照 ID，调用期间借用。
 * @return 同 ID 或新关联为 CC_OK，非法参数为 CC_ERR_INVALID_ARGUMENT，复制失败为 CC_ERR_OUT_OF_MEMORY，目标缺失为 CC_ERR_NOT_FOUND，冲突为 CC_ERR_INVALID_STATE，超长为 CC_ERR_LIMIT_EXCEEDED。
 */
cc_result_t cc_memory_pending_set_snapshot(cc_memory_pending_queue_t *queue,
                                            const char *turn_id, const char *snapshot_id);
/**
 * @brief 锁内给指定 FIFO 项关联拥有 ID，新关联成功时移交指针并清空输入槽。
 *
 * 已有关联且正文相同时也返回成功，此时输入仍归调用者清理。
 * 不同 ID 报错。
 * 所有失败保留输入拥有关系，长度含 NUL 最多 64 字节。
 *
 * @param[in,out] queue 接收快照 ID 所有权的目标队列，内部加锁查找已提交项。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @param[in,out] snapshot_id 指向独立分配、可由 free 释放的非空 ID。新关联时置 NULL，同 ID 已关联时保留原指针。
 * @return 新关联或同 ID 已关联为 CC_OK，目标缺失为 CC_ERR_NOT_FOUND，冲突为 CC_ERR_INVALID_STATE，超长为 CC_ERR_LIMIT_EXCEEDED，非法参数为 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t cc_memory_pending_take_snapshot(cc_memory_pending_queue_t *queue,
                                             const char *turn_id, char **snapshot_id);
/**
 * @brief 仅当当前关联仍等于期望 ID 时解除并释放该字符串，历史文件由快照存储管理。
 *
 * @param[in,out] queue 包含目标已提交项的队列，内部加锁解除关联。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @param[in] expected_snapshot_id 调用者预期解除的当前快照 ID，匹配后才清关联。
 * @return 成功为 CC_OK，非法参数为 CC_ERR_INVALID_ARGUMENT，目标缺失为 CC_ERR_NOT_FOUND，无关联或关联不符为 CC_ERR_INVALID_STATE。
 */
cc_result_t cc_memory_pending_clear_snapshot(cc_memory_pending_queue_t *queue,
                                              const char *turn_id, const char *expected_snapshot_id);
/**
 * @brief 只确认匹配 ID 的当前 FIFO 队首，清理字符串并归还其完整计费容量。
 *
 * 乱序或计费异常返回错误，已完成写入与核对由调用者在 ack 前保证。
 *
 * @param[in,out] queue 待确认队列，成功时移除当前队首并归还计费。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @return 成功为 CC_OK，非法参数为 CC_ERR_INVALID_ARGUMENT，FIFO 空、ID 不匹配或计费异常为 CC_ERR_INVALID_STATE。
 */
cc_result_t cc_memory_pending_ack(cc_memory_pending_queue_t *queue, const char *turn_id);
/**
 * @brief 锁内取得 FIFO 与预留的总项数及计费字节，queue 为 NULL 时输出零。
 *
 * @param[in] queue 待查询队列，可为 NULL。
 * @param[out] out_count 可选输出，FIFO 项与预留合计数量。
 * @param[out] out_bytes 可选输出，包含完整答复预留的计费字节数。
 * @post 非 NULL 输出指针已填写同一次队列锁下的快照，queue 为 NULL 时填写零。
 */
void cc_memory_pending_status(cc_memory_pending_queue_t *queue,
                              size_t *out_count, size_t *out_bytes);
/**
 * @brief 同一队列锁下取得 FIFO 项、预留项与字节快照，输出指针可以分别省略。
 *
 * @param[in] queue 待查询队列，可为 NULL。
 * @param[out] out_ready 可选输出，FIFO 中已提交项数。
 * @param[out] out_reserved 可选输出，尚未提交的预留数。
 * @param[out] out_bytes 可选输出，包含完整答复预留的计费字节数。
 * @post 非 NULL 输出指针已填写同一次队列锁下的快照，queue 为 NULL 时填写零。
 */
void cc_memory_pending_status_detail(cc_memory_pending_queue_t *queue,
    size_t *out_ready, size_t *out_reserved, size_t *out_bytes);

#endif
