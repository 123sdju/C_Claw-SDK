#include "cc/adapters/cc_recovery_bootstrap.h"
#include "cc/adapters/cc_json_segmented_history.h"
#include "cc_segmented_integrity.h"
#include <stdlib.h>

struct cc_recovery_bootstrap {
    cc_operation_journal_t *journal;       /* 装配拥有，成功后供 builder 借用。 */
    cc_run_checkpoint_store_t *checkpoints; /* 装配拥有，销毁时先释放。 */
};

/**
 * @brief 先销毁检查点，再销毁账本并释放装配外壳。
 * @pre 已附加的 builder 及其使用者均已退出，借用 fs 仍存活。
 * @param[in,out] recovery 拥有的完整或部分装配，允许 NULL。
 * @post 装配及其借出对象指针失效，恢复文件保留。
 */
void cc_recovery_bootstrap_destroy(cc_recovery_bootstrap_t *recovery)
{
    if (!recovery) return;
    cc_run_checkpoint_store_destroy(recovery->checkpoints);
    cc_operation_journal_destroy(recovery->journal);
    free(recovery);
}

/**
 * @brief 读取装配的账本指针。
 * @param[in] recovery 借用的装配，允许 NULL。
 * @return 装配寿命内有效的账本借用指针，空装配返回 NULL。
 */
cc_operation_journal_t *cc_recovery_bootstrap_journal(cc_recovery_bootstrap_t *recovery)
{
    return recovery ? recovery->journal : NULL;
}

/**
 * @brief 读取装配的检查点存储指针。
 * @param[in] recovery 借用的装配，允许 NULL。
 * @return 装配寿命内有效的检查点借用指针，空装配返回 NULL。
 */
cc_run_checkpoint_store_t *cc_recovery_bootstrap_checkpoints(cc_recovery_bootstrap_t *recovery)
{
    return recovery ? recovery->checkpoints : NULL;
}

/**
 * @brief 先锁定恢复要求，再打开两种存储、扫描历史并原子附加恢复 bundle。
 *
 * journal 和历史扫描各自提供明确完整性分类。任何中途失败清理本次存储，
 * 已设置的 builder 恢复要求继续有效，使所有工具入口保持关闭。
 * @pre 新 builder 由 owner 串行装配，所有输入及手动入口已关闭，目录由同一 owner 独占。
 * @param[in,out] builder 借用的 builder，成功后持续借用返回对象中的存储和历史访问器。
 * @param[in] fs 借用至 builder 及装配销毁的文件系统。
 * @param[in] operation_directory 非空既有账本目录路径，同步借用。
 * @param[in] checkpoint_directory 非空既有检查点目录路径，同步借用。
 * @param[in] max_pending 账本普通新操作条数阈值，零使用默认值。
 * @param[out] out_recovery 必填，参数有效后置 NULL，成功输出拥有装配，先销毁 builder 后释放。
 * @param[out] out_unresolved 必填，参数有效后置零，成功得到导入数，后续失败归零。
 * @param[out] out_failure 可选，先置 OTHER，明确账本或历史损坏时填写对应分类。
 * @return 成功为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，非分段会话后端为 CC_ERR_UNSUPPORTED，其他资源、读取、扫描及附加错误向上传递。
 */
cc_result_t cc_recovery_bootstrap_open_detailed(
    cc_runtime_builder_t *builder, cc_filesystem_t *fs,
    const char *operation_directory, const char *checkpoint_directory, size_t max_pending,
    cc_recovery_bootstrap_t **out_recovery, size_t *out_unresolved,
    cc_recovery_bootstrap_failure_t *out_failure)
{
    if (out_failure) *out_failure = CC_RECOVERY_BOOTSTRAP_FAILURE_OTHER;
    if (!builder || !fs || !operation_directory || !operation_directory[0] ||
        !checkpoint_directory || !checkpoint_directory[0] || !out_recovery || !out_unresolved)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid recovery bootstrap request");
    *out_recovery = NULL; *out_unresolved = 0;
    cc_result_t rc = cc_runtime_builder_require_recovery(builder);
    if (rc.code != CC_OK) return rc;
    cc_recovery_bootstrap_t *recovery = calloc(1, sizeof(*recovery));
    if (!recovery) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate recovery bootstrap");
    cc_operation_journal_failure_t journal_failure = CC_OPERATION_JOURNAL_FAILURE_OTHER;
    rc = cc_operation_journal_open_detailed(fs, operation_directory, max_pending,
                                           &recovery->journal, &journal_failure);
    if (out_failure && journal_failure == CC_OPERATION_JOURNAL_FAILURE_INTEGRITY)
        *out_failure = CC_RECOVERY_BOOTSTRAP_FAILURE_JOURNAL_INTEGRITY;
    if (rc.code == CC_OK)
        rc = cc_run_checkpoint_store_open(fs, checkpoint_directory, &recovery->checkpoints);
    cc_runtime_generation_t *generation = NULL;
    if (rc.code == CC_OK) rc = cc_runtime_builder_acquire_generation(builder, &generation);
    if (rc.code == CC_OK) {
        cc_agent_runtime_t *runtime = cc_runtime_generation_runtime(generation);
        cc_session_store_t *store = cc_agent_runtime_session_store(runtime);
        if (!cc_json_segmented_store_is_instance(store))
            rc = cc_result_error(CC_ERR_UNSUPPORTED, "Recovery requires segmented JSON session storage");
        else {
            rc = cc_json_segmented_store_import_interrupted_calls(store, recovery->journal, out_unresolved);
            if (out_failure && cc_segmented_is_integrity_error(&rc))
                *out_failure = CC_RECOVERY_BOOTSTRAP_FAILURE_HISTORY_INTEGRITY;
        }
    }
    if (generation) cc_runtime_generation_release(generation);
    if (rc.code == CC_OK)
        rc = cc_runtime_builder_set_recovery_bundle_with_visitor(builder, recovery->journal,
            recovery->checkpoints, cc_json_segmented_store_find_message,
            cc_json_segmented_store_visit_messages);
    if (rc.code != CC_OK) { *out_unresolved = 0; cc_recovery_bootstrap_destroy(recovery); return rc; }
    *out_recovery = recovery;
    return cc_result_ok();
}

/**
 * @brief 通过详细入口执行恢复装配，省略失败分类输出。
 * @param[in,out] builder 输入和工具入口已排空的全新 builder，成功借用装配至销毁。
 * @param[in] fs 借用至 builder 及装配销毁的文件系统。
 * @param[in] operation_directory 调用期间借用的既有账本目录。
 * @param[in] checkpoint_directory 调用期间借用的既有检查点目录。
 * @param[in] max_pending 普通操作条数阈值，零用默认值。
 * @param[out] out_recovery 必填，参数有效后置 NULL，成功得到拥有装配。
 * @param[out] out_unresolved 必填，参数有效后置零，成功得到历史导入数。
 * @return 原样返回详细入口结果，失败继续保持已设置的恢复要求。
 */
cc_result_t cc_recovery_bootstrap_open(
    cc_runtime_builder_t *builder, cc_filesystem_t *fs,
    const char *operation_directory, const char *checkpoint_directory, size_t max_pending,
    cc_recovery_bootstrap_t **out_recovery, size_t *out_unresolved)
{
    return cc_recovery_bootstrap_open_detailed(builder, fs, operation_directory,
        checkpoint_directory, max_pending, out_recovery, out_unresolved, NULL);
}
