
#ifndef CC_DEFAULT_POLICY_ENGINE_H
#define CC_DEFAULT_POLICY_ENGINE_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_policy_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建只按固定工具名称决定默认审批的不可变引擎。
 * @param[in] shell_requires_approval 非零时 shell_run/shell.run 需要审批，按值保存。
 * @param[out] out_engine 必填空描述符，先清零，成功交付拥有 self 与静态方法表。
 * @return CC_OK、INVALID_ARGUMENT 或 OOM，失败无新状态，拥有诊断由 caller 释放。
 * @note file_delete/file.delete 始终需要审批，其他已命名调用允许，无效调用业务拒绝。
 * 引擎不执行工具或审批回调，executor 处理没有 handler 的拒绝。owner 排空检查后 destroy。
 */
cc_result_t cc_policy_engine_create_default(
    int shell_requires_approval,
    cc_policy_engine_t *out_engine
);

#ifdef __cplusplus
}
#endif

#endif
