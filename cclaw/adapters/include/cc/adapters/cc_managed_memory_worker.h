#ifndef CC_MANAGED_MEMORY_WORKER_H
#define CC_MANAGED_MEMORY_WORKER_H

#include "cc/adapters/cc_json_memory_store.h"
#include "cc/app/cc_memory_extractor.h"
#include "cc/app/cc_memory_pending_queue.h"

typedef struct cc_managed_memory_worker cc_managed_memory_worker_t;

/**
 * @brief 判断本次自动变更是否需要保留重写前快照。
 * @param[in] plan 已生成的变更方案，仅在回调期间借用。
 * @param[in] original_count 生成方案时基线中的受管理记录数。
 * @param[in] user_data options.policy_user_data，生命周期覆盖 worker。
 * @return 零表示无需快照，CC_MEMORY_SNAPSHOT_REWRITE 表示重写快照，其他值使处理失败。
 * @note 在处理锁内调用，禁止重入同一 worker 的 process_one、replan_head、discard_head 或销毁入口。
 */
typedef cc_memory_snapshot_reason_t (*cc_memory_snapshot_decide_fn)(
    const cc_memory_plan_t *plan, size_t original_count, void *user_data);
/**
 * @brief 在确认队首出队前检查外部状态，例如介质是否仍为本次处理的介质。
 * @param[in] user_data options.before_ack_user_data，生命周期覆盖 worker。
 * @return CC_OK 允许确认出队，其他结果保留队首与缓存供重试。
 * @note 在处理锁内调用，禁止重入同一 worker 的 process_one、replan_head、discard_head 或销毁入口。
 */
typedef cc_result_t (*cc_memory_before_ack_fn)(void *user_data);

typedef struct cc_managed_memory_worker_options {
    size_t size; /**< 调用方结构字节数，至少覆盖 policy_user_data，尾部可选字段按此值读取。 */
    size_t capacity; /**< 受管理记忆记录上限，必须大于零。 */
    size_t max_pending_items; /**< FIFO 与未提交预留合计的条数上限，必须大于零。 */
    size_t max_pending_bytes; /**< 对话字符串、答案容量及快照 ID 预留的计费上限，必须大于零。 */
    cc_memory_eviction_select_fn select_eviction; /**< 必需的淘汰策略，在处理锁内调用，遵守同一 worker 的回调重入约束。 */
    cc_memory_snapshot_decide_fn decide_snapshot; /**< 必需的快照策略。 */
    void *policy_user_data; /**< 两项策略共用的借用上下文。 */
    cc_memory_before_ack_fn before_ack; /**< 可选的出队前检查。 */
    void *before_ack_user_data; /**< 出队前检查的借用上下文。 */
} cc_managed_memory_worker_options_t;

typedef struct cc_managed_memory_worker_result {
    int changed; /**< 方案中有新增或已有记录变更时为一，最终执行状态由返回值说明。 */
    size_t added; /**< 方案最终保留的新增记录数。 */
    size_t changed_existing; /**< 方案变更或移除的已有记录数，包含淘汰项。 */
    size_t evicted_existing; /**< 方案淘汰的已有记录数。 */
    size_t evicted_new; /**< 方案中未获保留的新增候选数。 */
    cc_llm_usage_t model_usage; /**< 本次调用实际提取产生的用量，复用提取缓存时保持零初始化值。 */
} cc_managed_memory_worker_result_t;

/**
 * @brief 创建拥有有界待处理队列和处理锁的记忆处理器。
 *
 * 失败清理本次资源并保持输出为空，容量及策略由组合根显式提供。
 *
 * @note 队列入口由队列锁保护，处理、重规划与丢弃由处理锁串行化。外层负责销毁同步及共享 provider 的并发。
 * @param[in] store 借用的 JSON 记忆存储，生命周期覆盖 worker。
 * @param[in] snapshots 借用的历史版本存储，生命周期覆盖 worker。
 * @param[in] options 显式 worker 选项，标量复制，回调上下文继续借用。
 * @param[out] out_worker 成功取得拥有的 worker，先排空处理及回调再销毁。
 * @return 成功为 CC_OK，必需依赖、限制、策略或选项尺寸非法为 CC_ERR_INVALID_ARGUMENT，分配失败为 CC_ERR_OUT_OF_MEMORY，队列与锁创建错误原样返回。
 */
cc_result_t cc_managed_memory_worker_create(
    cc_memory_store_t *store, cc_memory_snapshot_store_t *snapshots,
    const cc_managed_memory_worker_options_t *options, cc_managed_memory_worker_t **out_worker);
/**
 * @brief 调用者先关闭入口并排空处理/队列使用者，再释放缓存、队列和锁。
 *
 * 队列拥有的 RAM 项及预留随销毁清理，借用 store/snapshots 由外层管理。
 * @pre 处理、策略回调及队列使用者已退出，外层已关闭入口。
 *
 * @param[in,out] worker 要销毁的处理器，可为 NULL，成功创建的对象在此结束寿命。
 * @post pending queue、提取缓存与处理锁已释放，worker 寿命结束。
 */
void cc_managed_memory_worker_destroy(cc_managed_memory_worker_t *worker);
/**
 * @brief 复制一轮输入/答案到有界 FIFO，重复 turn ID 按队列内内容一致性检查。
 *
 * @note ID、会话、原输入与答案均深拷贝。相同 ID 的会话、原输入或答案不同，以及该 ID 尚有预留时，返回 CC_ERR_INVALID_STATE。
 * @param[in,out] worker 接收对话的处理器，内部通过队列锁追加。
 * @param[in] turn_id 非空的对话稳定标识，调用期间借用。
 * @param[in] session_id 非空的原会话标识，调用期间借用。
 * @param[in] user_input 非空的原用户输入，调用期间借用并深拷贝。
 * @param[in] assistant_text 完成答复，可为空字符串，调用期间借用并深拷贝。
 * @return 接纳或已有相同内容为 CC_OK，非法参数为 CC_ERR_INVALID_ARGUMENT，ID 冲突为 CC_ERR_INVALID_STATE，容量不足为 CC_ERR_LIMIT_EXCEEDED，分配失败为 CC_ERR_OUT_OF_MEMORY。
 */
cc_result_t cc_managed_memory_worker_enqueue(
    cc_managed_memory_worker_t *worker, const char *turn_id, const char *session_id,
    const char *user_input, const char *assistant_text);
/**
 * @brief 创建计入总项数及字节预算的 RAM 预留，成功句柄归提交者持有。
 *
 * @note 复制 ID、会话和原输入，预分配答案缓冲。FIFO 按 commit 顺序排列，队列中的重复 ID 一律拒绝。
 * @param[in,out] worker 登记预留并承担计费的处理器。
 * @param[in] turn_id 非空的对话稳定标识，调用期间借用并复制。
 * @param[in] session_id 非空的原会话标识，调用期间借用并复制。
 * @param[in] user_input 非空的原用户输入，调用期间借用并复制。
 * @param[in] max_answer_bytes 答案正文容量，可为零，须小于 SIZE_MAX，额外分配一个 NUL 字节。
 * @param[out] out_reservation 成功取得句柄，worker 有效且输出指针非 NULL 时先置空，调用者串行使用并通过提交或取消消费。
 * @return 成功为 CC_OK，非法参数为 CC_ERR_INVALID_ARGUMENT，重复 ID 为 CC_ERR_INVALID_STATE，容量或长度溢出为 CC_ERR_LIMIT_EXCEEDED，分配失败为 CC_ERR_OUT_OF_MEMORY。
 */
cc_result_t cc_managed_memory_worker_reserve(cc_managed_memory_worker_t *worker,
    const char *turn_id, const char *session_id, const char *user_input,
    size_t max_answer_bytes, cc_memory_pending_reservation_t **out_reservation);
/**
 * @brief 在同一队列预算内扩大原答案缓冲，失败保留既有容量及句柄。
 *
 * @param[in,out] worker 原预留所属处理器，扩容增加其队列计费。
 * @param[in,out] reservation 仍属于该队列的预留句柄。
 * @param[in] max_answer_bytes 请求的答案正文容量，须小于 SIZE_MAX，未超过原容量时直接成功。
 * @return 容量足够或扩容成功为 CC_OK，非法参数为 CC_ERR_INVALID_ARGUMENT，归属不符为 CC_ERR_INVALID_STATE，容量不足为 CC_ERR_LIMIT_EXCEEDED，分配失败为 CC_ERR_OUT_OF_MEMORY。失败保留原预留及计费。
 */
cc_result_t cc_managed_memory_worker_expand_reservation(cc_managed_memory_worker_t *worker,
    cc_memory_pending_reservation_t *reservation, size_t max_answer_bytes);
/**
 * @brief 核对预留身份与 turn/session/输入，调用中借用参数。
 *
 * @param[in] worker 借用的处理器，队列内容只读，内部通过队列锁同步。
 * @param[in] reservation 仍属于该队列的预留句柄。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @param[in] session_id 原输入所属会话标识，调用期间借用。
 * @param[in] user_input 用于逐字节核对的原用户输入，调用期间借用。
 * @return 全部匹配为 CC_OK，NULL 参数为 CC_ERR_INVALID_ARGUMENT，句柄归属或任一字符串不匹配为 CC_ERR_INVALID_STATE。
 */
cc_result_t cc_managed_memory_worker_verify_reservation(cc_managed_memory_worker_t *worker,
    cc_memory_pending_reservation_t *reservation, const char *turn_id,
    const char *session_id, const char *user_input);
/**
 * @brief 返回独立预留副本，调用者用 cc_memory_pending_item_cleanup 清理。
 *
 * @note 输出须为无拥有资源的结构。有效 worker 和参数经检查后输出清零，副本中的答案为空字符串。
 * @param[in] worker 借用的处理器，内部通过队列锁复制预留。
 * @param[in] reservation 仍属于该队列的预留句柄。
 * @param[out] out_item 成功取得拥有的独立项目副本，使用 cc_memory_pending_item_cleanup 清理。
 * @return 成功为 CC_OK，NULL 参数为 CC_ERR_INVALID_ARGUMENT，归属不符为 CC_ERR_INVALID_STATE，复制失败为 CC_ERR_OUT_OF_MEMORY。
 */
cc_result_t cc_managed_memory_worker_inspect_reservation(cc_managed_memory_worker_t *worker,
    cc_memory_pending_reservation_t *reservation, cc_memory_pending_item_t *out_item);
/**
 * @brief 使用预分配答案缓冲交接到 FIFO，成功消费预留句柄，失败保留。
 *
 * @note 答案必须适配已有容量，交接无需新分配。全部预留计费保留至该项出队。
 * @param[in,out] worker 接收本预留提交的处理器，内部通过队列锁交接。
 * @param[in,out] reservation 仍属于该队列的预留句柄，成功消费后置空。
 * @param[in] assistant_text 完成答复，可为空字符串，调用期间借用并复制到预留缓冲。
 * @return 成功为 CC_OK，非法参数为 CC_ERR_INVALID_ARGUMENT，答复超长为 CC_ERR_LIMIT_EXCEEDED，归属或计费异常为 CC_ERR_INVALID_STATE。失败保留预留。
 */
cc_result_t cc_managed_memory_worker_commit(cc_managed_memory_worker_t *worker,
    cc_memory_pending_reservation_t **reservation, const char *assistant_text);
/**
 * @brief 取消尚未提交的预留并归还预算，成功后调用者句柄清空。
 *
 * @param[in,out] worker 原预留所属处理器，取消后归还其队列容量。
 * @param[in,out] reservation 仍属于该队列的预留句柄，成功消费后置空。
 * @return 成功为 CC_OK，NULL worker、reservation 或 *reservation 为 CC_ERR_INVALID_ARGUMENT，归属或计费异常为 CC_ERR_INVALID_STATE。失败保留预留。
 */
cc_result_t cc_managed_memory_worker_cancel_reservation(cc_managed_memory_worker_t *worker,
    cc_memory_pending_reservation_t **reservation);
/**
 * @brief 同步处理最旧的已提交对话，完成提取、规划、可选快照、写入和出队确认。
 *
 * 成功提取后缓存决策、基线和 updated_at。重试时，UPDATE 目标被编辑或删除返回 CC_ERR_INVALID_STATE，
 * 其余基线变化可沿用决策重新规划。已观察到安装，或失败后无法确认存储可写时，保留安装核对状态。
 * 存储恢复后，当前集合等于方案则直接确认，等于基线则允许重试，其他集合返回冲突。
 * @note 处理锁覆盖模型、策略和 before_ack 回调，回调禁止重入同一 worker 的处理、重规划、丢弃或销毁入口。
 * 队列字节预算只约束对话及预留，提取响应、基线、方案和临时副本另占内存。
 * @param[in,out] worker 待处理队首的处理器，处理锁串行保护执行和缓存。
 * @param[in] provider 本次同步借用的模型 provider，跨调用并发由外层协调。
 * @param[in] options 本次提取选项，调用期间借用。
 * @param[in] updated_at 应用提供的非负更新时间，worker 首次成功提取后固定该值。
 * @param[out] out_result 必需输出，参数合法后先清零。方案计数与本次模型用量可能在后续失败前已填写。
 * @return CC_OK 表示队首已确认出队，空队列为 CC_ERR_NOT_FOUND，其他错误保留队首和已有提取缓存。
 */
cc_result_t cc_managed_memory_worker_process_one(
    cc_managed_memory_worker_t *worker, cc_llm_provider_t *provider,
    const cc_memory_extraction_options_t *options, int64_t updated_at,
    cc_managed_memory_worker_result_t *out_result);
/**
 * @brief 显式重规划当前队首，保留输入并清除可重新评估的缓存。
 *
 * @note 在处理锁内核对 turn_id 和存储可写性。存在待安装核对状态时返回 CC_ERR_INVALID_STATE，
 * 应先恢复存储并通过 process_one 核对。成功时解除快照关联、释放提取缓存，下一次处理重新提取。
 * 历史文件仍由快照存储管理。
 * @param[in,out] worker 待重规划队首所属的处理器，内部获取处理锁。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @return 成功为 CC_OK，非法参数为 CC_ERR_INVALID_ARGUMENT，FIFO 空为 CC_ERR_NOT_FOUND，身份冲突或待安装核对为 CC_ERR_INVALID_STATE，存储不可写为 CC_ERR_STORAGE。复制及存储查询错误原样返回。
 */
cc_result_t cc_managed_memory_worker_replan_head(cc_managed_memory_worker_t *worker, const char *turn_id);
/**
 * @brief 显式丢弃当前队首，按内部核对规则确认后释放队列项。
 * @note 与 replan_head 使用相同的身份、安装核对和存储可写性检查，在处理锁内执行。成功后清理缓存。
 * @param[in,out] worker 待丢弃队首所属的处理器，内部获取处理锁。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @return 成功为 CC_OK，非法参数为 CC_ERR_INVALID_ARGUMENT，FIFO 空为 CC_ERR_NOT_FOUND，身份冲突、待核对安装或计费异常为 CC_ERR_INVALID_STATE，存储不可写为 CC_ERR_STORAGE。复制及存储查询错误原样返回。
 */
cc_result_t cc_managed_memory_worker_discard_head(cc_managed_memory_worker_t *worker, const char *turn_id);
/**
 * @brief 读取 FIFO 和未提交预留的总项数及计费字节，空 worker 返回零。
 *
 * @param[in] worker 待查询处理器，可为 NULL。
 * @param[out] out_pending 可选输出，FIFO 项与预留合计数量。
 * @param[out] out_bytes 可选输出，包含完整答复预留的计费字节数。
 * @post 可选输出已按同一次队列锁快照填写，空 worker 或 queue 输出零。
 */
void cc_managed_memory_worker_status(cc_managed_memory_worker_t *worker,
                                     size_t *out_pending, size_t *out_bytes);
/**
 * @brief 分别读取 FIFO 项、预留和总计费字节，队列自身锁保护各字段快照。
 *
 * @param[in] worker 待查询处理器，可为 NULL。
 * @param[out] out_ready 可选输出，FIFO 中已提交项数。
 * @param[out] out_reserved 可选输出，尚未提交的预留数。
 * @param[out] out_bytes 可选输出，包含完整答复预留的计费字节数。
 * @post 可选输出已按同一次队列锁快照填写，空 worker 或 queue 输出零。
 */
void cc_managed_memory_worker_status_detail(cc_managed_memory_worker_t *worker,
    size_t *out_ready, size_t *out_reserved, size_t *out_bytes);

#endif
