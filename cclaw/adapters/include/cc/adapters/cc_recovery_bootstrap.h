#ifndef CC_RECOVERY_BOOTSTRAP_H
#define CC_RECOVERY_BOOTSTRAP_H

#include "cc/app/cc_runtime_builder.h"
#include "cc/ports/cc_filesystem.h"

/** 拥有操作账本和运行检查点，成功附加后由 builder 及其 generation 借用。 */
typedef struct cc_recovery_bootstrap cc_recovery_bootstrap_t;

typedef enum cc_recovery_bootstrap_failure {
    CC_RECOVERY_BOOTSTRAP_FAILURE_OTHER = 0, /**< I/O、资源、能力或其他未确定完整性损坏的失败。 */
    CC_RECOVERY_BOOTSTRAP_FAILURE_HISTORY_INTEGRITY, /**< 已确认的分段历史完整性损坏。 */
    CC_RECOVERY_BOOTSTRAP_FAILURE_JOURNAL_INTEGRITY /**< 已确认的操作账本完整性损坏。 */
} cc_recovery_bootstrap_failure_t;

/**
 * @brief 为新 builder 打开账本及检查点、扫描中断历史并附加完整恢复装配。
 * @pre 产品已核对介质绑定、文件系统身份和路径隔离，owner 排空所有输入及手动工具入口并独占目录。
 * @param[in,out] builder 借用的全新 builder，先置为恢复必需，成功后持续借用返回装配。
 * @param[in] fs 借用的文件系统，寿命覆盖 builder 及返回装配。
 * @param[in] operation_directory 已存在的操作账本目录，路径在调用期间借用并由账本复制。
 * @param[in] checkpoint_directory 已存在的运行检查点目录，路径在调用期间借用并由存储复制。
 * @param[in] max_pending 普通操作准入阈值，零使用账本默认值。
 * @param[out] out_recovery 必填且无待释放内容，参数有效后置 NULL，成功取得拥有装配，先销毁 builder 再销毁装配。
 * @param[out] out_unresolved 必填，参数有效后置零，成功输出历史导入的未决调用数，失败归零。
 * @return 成功为 CC_OK，参数、资源、存储、历史扫描或附加错误向上传递，已要求恢复后的失败继续保持模型及手动工具派发关闭。
 */
cc_result_t cc_recovery_bootstrap_open(
    cc_runtime_builder_t *builder, cc_filesystem_t *fs,
    const char *operation_directory, const char *checkpoint_directory, size_t max_pending,
    cc_recovery_bootstrap_t **out_recovery, size_t *out_unresolved);
/**
 * @brief 执行恢复装配并区分已确认的历史损坏和账本损坏。
 * @pre builder 尚无恢复装配，输入和手动入口已排空，目录由 owner 独占。
 * @param[in,out] builder 借用的全新 builder，先要求恢复再构造完整 bundle。
 * @param[in] fs 借用至 builder 及装配销毁的文件系统。
 * @param[in] operation_directory 调用期间借用的既有账本目录路径。
 * @param[in] checkpoint_directory 调用期间借用的既有检查点目录路径。
 * @param[in] max_pending 普通操作条数阈值，零采用账本默认值。
 * @param[out] out_recovery 必填，参数有效后置 NULL，成功取得拥有装配，失败清理本次资源。
 * @param[out] out_unresolved 必填，参数有效后置零，成功取得导入数，失败归零。
 * @param[out] out_failure 可选，先置 OTHER，仅明确完整性证据时改为 HISTORY_INTEGRITY 或 JOURNAL_INTEGRITY。
 * @return 沿用 open 的结果，普通 I/O、资源及未确定解析失败保持 OTHER，完整性分类供产品选择纯对话限制模式。
 */
cc_result_t cc_recovery_bootstrap_open_detailed(
    cc_runtime_builder_t *builder, cc_filesystem_t *fs,
    const char *operation_directory, const char *checkpoint_directory, size_t max_pending,
    cc_recovery_bootstrap_t **out_recovery, size_t *out_unresolved,
    cc_recovery_bootstrap_failure_t *out_failure);
/**
 * @brief 释放拥有的检查点、账本及恢复装配。
 * @pre 已附加的 builder 及 generation 已销毁，相关调用全部退出，fs 仍存活。
 * @param[in,out] recovery 待释放装配，允许 NULL，支持部分构造失败清理。
 * @post 装配寿命结束，其账本和检查点借用指针失效，磁盘文件继续保留。
 */
void cc_recovery_bootstrap_destroy(cc_recovery_bootstrap_t *recovery);
/**
 * @brief 借出装配拥有的操作账本。
 * @param[in] recovery 借用的恢复装配，允许 NULL。
 * @return 账本借用指针，有效期至装配销毁，空装配返回 NULL。
 */
cc_operation_journal_t *cc_recovery_bootstrap_journal(cc_recovery_bootstrap_t *recovery);
/**
 * @brief 借出装配拥有的运行检查点存储。
 * @param[in] recovery 借用的恢复装配，允许 NULL。
 * @return 检查点借用指针，有效期至装配销毁，空装配返回 NULL。
 */
cc_run_checkpoint_store_t *cc_recovery_bootstrap_checkpoints(cc_recovery_bootstrap_t *recovery);

#endif
