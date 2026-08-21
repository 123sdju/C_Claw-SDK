



#include "cc/adapters/cc_default_policy_engine.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_policy_engine.h"
#include "cc/internal/cc_alloc.h"
#include <stdlib.h>
#include <string.h>

/*
 * 释放策略决策里的动态 reason 字符串。
 *
 * policy engine 把“为什么需要审批/为什么拒绝”返回给调用方；reason 的所有权交给调用方，
 * 因此 runtime/tool executor 在处理完决策后必须调用本函数，避免安全错误路径泄漏内存。
 */
void cc_policy_decision_free(cc_policy_decision_t *decision)
{
    if (!decision) return;
    free(decision->reason);
    decision->reason = NULL;
}

/*
 * 默认策略引擎的私有状态。
 *
 * 当前只包含 shell_requires_approval 开关，后续如果增加网络、文件删除等策略，也应继续
 * 放在 self 中，通过 vtable 隐藏实现细节。这是 C 语言面向对象封装的典型写法。
 */
typedef struct {
    int shell_requires_approval;
} cc_default_policy_engine_t;

/*
 * 检查一次工具调用是否允许执行。
 *
 * 默认策略不直接执行安全动作，只给出 allowed/require_approval/reason，真正的审批回调由
 * tool executor 触发。这样策略、UI 审批和工具执行三者解耦；无审批 handler 时 executor
 * 会把 require_approval 转成可恢复工具错误。
 */
static cc_result_t default_check_tool_call(
    void *self,
    const cc_tool_call_t *call,
    const cc_tool_context_t *ctx,
    cc_policy_decision_t *out_decision
)
{
    (void)ctx;
    cc_default_policy_engine_t *engine = (cc_default_policy_engine_t *)self;

    memset(out_decision, 0, sizeof(cc_policy_decision_t));

    if (!call || !call->name) {
        out_decision->allowed = 0;
        out_decision->reason = cc_copy_string("Invalid tool call");
        return cc_result_ok();
    }

    out_decision->allowed = 1;

    if (strcmp(call->name, "shell_run") == 0 ||
        strcmp(call->name, "shell.run") == 0) {
        if (engine->shell_requires_approval) {
            out_decision->require_approval = 1;
            out_decision->reason = cc_copy_string("Shell execution requires user approval");
        }
    } else if (strcmp(call->name, "file_delete") == 0 ||
               strcmp(call->name, "file.delete") == 0) {
        out_decision->require_approval = 1;
        out_decision->reason = cc_copy_string("File deletion requires user approval");
    }

    return cc_result_ok();
}

/* 销毁默认策略引擎私有状态；端口结构本身由创建者栈上或 builder 内嵌持有。 */
static void default_destroy(void *self)
{
    free(self);
}

/* 默认 policy engine 的 vtable，向 core 暴露“接口”，隐藏 adapter 私有结构。 */
static cc_policy_engine_vtable_t default_vtable = {
    default_check_tool_call,
    default_destroy
};

/*
 * 创建默认策略引擎。
 *
 * out_engine 由调用方提供并接收 self/vtable；成功后 destroy 由 cc_policy_engine_t 的使用方
 * 在 builder/runtime 销毁阶段调用。shell_requires_approval 为真时 shell.run 默认需要审批，
 * 符合 SDK 安全优先的核心契约。
 */
cc_result_t cc_policy_engine_create_default(
    int shell_requires_approval,
    cc_policy_engine_t *out_engine
)
{
    if (!out_engine) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null policy engine output");
    }
    memset(out_engine, 0, sizeof(*out_engine));
    cc_default_policy_engine_t *self = calloc(1, sizeof(cc_default_policy_engine_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create policy engine");

    self->shell_requires_approval = shell_requires_approval;
    out_engine->self = self;
    out_engine->vtable = &default_vtable;
    return cc_result_ok();
}
