#ifndef CC_MANAGED_MEMORY_EDITOR_H
#define CC_MANAGED_MEMORY_EDITOR_H

#include "cc/adapters/cc_json_memory_store.h"

typedef struct cc_managed_memory_editor cc_managed_memory_editor_t;

/**
 * @file cc_managed_memory_editor.h
 * @brief 受管理记忆的同步编辑接口。
 *
 * editor 借用 store 和 snapshots，拥有实例 epoch。调用者保证依赖覆盖实例寿命，
 * 并协调实例同步、销毁、存储恢复和队列处理。JSON store 在读取和提交时加锁，
 * 提交再次检查 revision，两个调用之间的列表结果可能失效。
 * 修改接口的 out_installed 为一表示存储已观察到新内容安装，随后清理失败仍可返回错误。
 * 零值同时涵盖无变更、提交前失败以及安装结果未知。写入结果未知时存储会阻断后续写入，
 * 调用者须先恢复存储并重新列出记录核对结果。
 */

/**
 * @brief 分配编辑器并生成独立 epoch。
 * @param[in] store 整个实例寿命内借用的 JSON 记忆存储，后续操作验证其实现类型。
 * @param[in] snapshots 整个实例寿命内借用的快照存储，重置时验证其源文件。
 * @param[out] out_editor 必填，成功返回由调用者销毁的实例，失败置为 NULL。
 * @return CC_OK 表示创建成功，参数无效返回 CC_ERR_INVALID_ARGUMENT，分配或 UUID 生成错误向上传递。
 */
cc_result_t cc_managed_memory_editor_create(cc_memory_store_t *store,
    cc_memory_snapshot_store_t *snapshots, cc_managed_memory_editor_t **out_editor);
/**
 * @brief 释放实例拥有的 epoch 和编辑器。
 * @pre 编辑调用已退出，借用的 store/snapshots 仍存活。
 * @param[in,out] editor 待销毁实例，允许 NULL。
 * @post 非空实例寿命结束，调用者持有的 editor 和 epoch 指针失效。
 */
void cc_managed_memory_editor_destroy(cc_managed_memory_editor_t *editor);
/**
 * @brief 借出实例 epoch 至编辑器销毁前，用于识别跨实例的陈旧编辑请求。
 * @param[in] editor 借用的编辑器，允许 NULL。
 * @return 本实例 epoch 的借用字符串，editor 为 NULL 时返回 NULL。
 */
const char *cc_managed_memory_editor_epoch(const cc_managed_memory_editor_t *editor);
/**
 * @brief 只核对借用 store 的对象身份，进一步可写性在修改路径检查。
 * @param[in] editor 借用的编辑器，允许 NULL。
 * @param[in] store 要比较的存储指针，允许 NULL。
 * @return editor 借用同一 store 对象时为一，缺失或不匹配时为零。
 */
int cc_managed_memory_editor_uses_store(const cc_managed_memory_editor_t *editor, const cc_memory_store_t *store);
/**
 * @brief 在同一次存储加锁期间复制受管理集合并读取 revision。
 * @param[in] editor 借用的编辑器。
 * @param[out] out_state 必填且须无待释放内容，成功取得拥有字符串的计划，用 cc_memory_plan_cleanup 清理。
 * @param[out] out_revision 必填，成功取得后续编辑须带回的列表版本。
 * @return CC_OK 表示已取得集合与版本，参数、分配或记录元数据错误向上传递。
 * @note 存储写入被阻断时仍允许读取当前内存集合，恢复后应重新读取。
 */
cc_result_t cc_managed_memory_editor_list(cc_managed_memory_editor_t *editor,
    cc_memory_plan_t *out_state, uint64_t *out_revision);

/**
 * @brief 按当前列表版本更新一条既有记忆，正文/类别改变时解除旧证据来源。
 *
 * 相同记录返回成功且 out_installed 为零。正文或类别改变时清除 evidence 和 source_session，
 * 仅修改重要度或时间保留原来源。此次编辑使用无快照提交。
 * @param[in,out] editor 借用的编辑器及其存储。
 * @param[in] epoch 读取列表时所属 editor 实例的身份字符串。
 * @param[in] revision 读取列表时取得的受管理集合版本。
 * @param[in] id 目标记忆的稳定 ID，调用期间借用。
 * @param[in] text 更新后的非空正文，同步调用期间借用。
 * @param[in] category 更新后的非空类别，同步调用期间借用，提交拒绝 context_summary。
 * @param[in] importance 更新后的重要度，范围为 0 到 100。
 * @param[in] updated_at 更新后的非负时间戳，提交时还须满足 JSON store 的时间范围。
 * @param[out] out_installed 必填，进入时置零，安装状态遵循本文件的修改契约。
 * @return CC_OK 表示更新或无变更，陈旧 epoch/revision 返回 CC_ERR_INVALID_STATE，目标缺失返回 CC_ERR_NOT_FOUND，其余校验、分配和存储错误向上传递。
 */
cc_result_t cc_managed_memory_editor_update(cc_managed_memory_editor_t *editor,
    const char *epoch, uint64_t revision, const char *id, const char *text,
    const char *category, unsigned importance, int64_t updated_at, int *out_installed);
/**
 * @brief 按实例 epoch 和列表 revision 删除指定受管理条目，使用无快照提交。
 * @param[in,out] editor 借用的编辑器及其存储。
 * @param[in] epoch 读取列表时所属 editor 实例的身份字符串。
 * @param[in] revision 读取列表时取得的受管理集合版本。
 * @param[in] id 目标记忆的稳定 ID，调用期间借用。
 * @param[out] out_installed 必填，进入时置零，安装状态遵循本文件的修改契约。
 * @return CC_OK 表示删除成功，陈旧 epoch/revision 返回 CC_ERR_INVALID_STATE，目标缺失返回 CC_ERR_NOT_FOUND，其余校验、分配和存储错误向上传递。
 */
cc_result_t cc_managed_memory_editor_delete(cc_managed_memory_editor_t *editor,
    const char *epoch, uint64_t revision, const char *id, int *out_installed);

/**
 * @brief 当前受管理集合非空时，先建立/复用 RESET 快照，再提交空受管理集合。
 *
 * 快照保存整个记忆主文件，提交仅清除受管理集合。空集合返回成功且两个输出均为空或零。
 * 待处理队列由调用者负责排空，或协调其与重置操作的执行顺序。
 * @param[in,out] editor 借用的编辑器及其存储。
 * @param[in] epoch 读取列表时所属 editor 实例的身份字符串。
 * @param[in] revision 读取列表时取得的受管理集合版本。
 * @param[in] prior_snapshot_id 可选的既有 reset- 快照 ID，同步借用，供同一重置重试时确认并复用。
 * @param[out] out_snapshot_id 必填，参数检查通过后置 NULL，可能在失败时仍返回分配的 ID，调用者用 free 释放并保留重试关联。
 * @param[out] out_installed 必填，参数检查通过后置零，安装状态遵循本文件的修改契约。
 * @return CC_OK 表示重置或集合已空，陈旧实例/版本、快照确认、分配与存储错误向上传递。
 */
cc_result_t cc_managed_memory_editor_reset(cc_managed_memory_editor_t *editor,
    const char *epoch, uint64_t revision, const char *prior_snapshot_id,
    char **out_snapshot_id, int *out_installed);

#endif
