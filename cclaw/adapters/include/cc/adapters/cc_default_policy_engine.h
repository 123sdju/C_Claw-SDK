
#ifndef CC_DEFAULT_POLICY_ENGINE_H
#define CC_DEFAULT_POLICY_ENGINE_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_policy_engine.h"

/*
 * 创建默认 policy engine。
 *
 * shell_requires_approval 为非 0 时 shell 类高风险工具需要审批；无审批 handler 时应 deny。
 * out_engine 成功后持有 self/vtable，调用方通过 vtable destroy 释放。
 */
cc_result_t cc_policy_engine_create_default(
    int shell_requires_approval,
    cc_policy_engine_t *out_engine
);

#endif
