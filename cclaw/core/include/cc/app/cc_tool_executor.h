



#ifndef CC_TOOL_EXECUTOR_H
#define CC_TOOL_EXECUTOR_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_tool_call.h"
#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_cancel_token.h"

typedef struct cc_tool_executor_options {
    /* 可选取消 token；executor 会在策略、等待 lane 和工具执行边界检查取消。 */
    cc_cancel_token_t *cancel_token;
    /* 单调毫秒绝对截止时间；0 表示只使用 runtime/tool lane 超时。 */
    uint64_t deadline_ms;
} cc_tool_executor_options_t;

/*
 * 执行一次工具调用。
 *
 * executor 负责：从 registry 查找工具、执行 schema 校验、运行 policy/approval、应用
 * tool pool 限流和 timeout，并把工具结果写入 out_result。工具业务失败应表现为
 * out_result->ok=0；系统级失败才返回非 OK cc_result_t。
 */
cc_result_t cc_tool_executor_execute_with_options(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_call_t *call,
    const cc_tool_executor_options_t *options,
    cc_tool_result_t *out_result
);


/* 不带额外选项的工具执行快捷入口。 */
cc_result_t cc_tool_executor_execute(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_call_t *call,
    cc_tool_result_t *out_result
);

#endif
