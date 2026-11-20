



#include "cc/adapters/cc_default_policy_engine.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_policy_engine.h"
#include <stdlib.h>
#include <string.h>

/**
 * @brief 消费决策原因并清零全部决策字段。
 * @param[in,out] decision 可空、已初始化且独占的决策，reason 须兼容 free。
 * @post 原因归还、结构清零，容器归 caller，空决策可重复清理。
 * @note 调用前排空该决策借用，不销毁策略实例或撤回已经执行的工具。
 */
void cc_policy_decision_free(cc_policy_decision_t *decision)
{
    if (!decision) return;
    free(decision->reason);
    memset(decision, 0, sizeof(*decision));
}

/*
 * 创建后不可变的 shell 审批开关，实例可在 owner 保持存活时并发检查。
 */
typedef struct {
    int shell_requires_approval;
} cc_default_policy_engine_t;

/**
 * @brief 按固定工具名称生成默认允许与审批决策，不执行工具或调用审批。
 * @param[in] self 必填存活的不可变引擎借用。
 * @param[in] call 调用期稳定记录，可空或缺名称时输出业务拒绝。
 * @param[in] ctx 调用期可选上下文，当前规则不读取。
 * @param[out] out_decision 必填新对象或已 free 槽，先清零，成功原因交 caller decision_free。
 * @return 基础状态/输出错误为 INVALID_ARGUMENT，原因副本 OOM 清空决策并返回 OOM，其余为 CC_OK，诊断交 caller。
 * @note shell_run/shell.run 沿实例开关审批，file_delete/file.delete 始终审批，其他名称允许。
 * 检查无内部修改，owner 排空调用后才销毁实例，审批缺失的后续拒绝由 executor 处理。
 */
static cc_result_t default_check_tool_call(
    void *self,
    const cc_tool_call_t *call,
    const cc_tool_context_t *ctx,
    cc_policy_decision_t *out_decision
)
{
    (void)ctx;
    if (!out_decision) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null policy decision output");
    memset(out_decision, 0, sizeof(cc_policy_decision_t));
    if (!self) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null default policy engine");
    cc_default_policy_engine_t *engine = (cc_default_policy_engine_t *)self;
    const char *reason = NULL;

    if (!call || !call->name) {
        reason = "Invalid tool call";
    } else {
        out_decision->allowed = 1;
        if (strcmp(call->name, "shell_run") == 0 ||
            strcmp(call->name, "shell.run") == 0) {
            if (engine->shell_requires_approval) {
                out_decision->require_approval = 1;
                reason = "Shell execution requires user approval";
            }
        } else if (strcmp(call->name, "file_delete") == 0 ||
                   strcmp(call->name, "file.delete") == 0) {
            out_decision->require_approval = 1;
            reason = "File deletion requires user approval";
        }
    }
    if (reason) {
        out_decision->reason = cc_copy_string(reason);
        if (!out_decision->reason) {
            memset(out_decision, 0, sizeof(*out_decision));
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy policy decision reason");
        }
    }

    return cc_result_ok();
}

/** @brief 消费默认引擎状态。 @param[in,out] self 独占堆状态，可空。 @pre owner 关闭入口并排空检查调用。 @post 状态释放，外部端口容器归 caller。 */
static void default_destroy(void *self)
{
    free(self);
}

/* 静态方法表覆盖实例生命周期。 */
static cc_policy_engine_vtable_t default_vtable = {
    default_check_tool_call,
    default_destroy
};

/**
 * @brief 创建固定名称规则的默认策略实例。
 * @param[in] shell_requires_approval 非零时两种 shell 名称需要审批，按值保存。
 * @param[out] out_engine 必填空描述符，先清零，成功交付拥有 self 与静态 vtable。
 * @return 参数、OOM 或 CC_OK，失败无新资源，诊断交 caller，成功实例由 owner 排空后 destroy。
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
