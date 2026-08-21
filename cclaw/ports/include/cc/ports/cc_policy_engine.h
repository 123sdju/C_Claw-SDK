



#ifndef CC_POLICY_ENGINE_H
#define CC_POLICY_ENGINE_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_tool_call.h"
#include "cc/ports/cc_tool.h"

/*
 * 工具调用风险等级。
 *
 * SAFE 可直接执行，MEDIUM 通常需要结合上下文判断，DANGEROUS 默认应要求审批或拒绝。
 * 风险等级是策略输入，不是最终允许结果。
 */
typedef enum cc_risk_level {
    CC_RISK_SAFE,

    CC_RISK_MEDIUM,

    CC_RISK_DANGEROUS

} cc_risk_level_t;

/*
 * policy engine 的决策结果。
 *
 * allowed 表示是否允许继续；require_approval 表示还需要用户/应用审批；reason 由决策
 * 拥有，调用方用 cc_policy_decision_free() 释放。无 approval handler 时应把需要审批
 * 的操作转为 deny。
 */
typedef struct cc_policy_decision {
    int allowed;

    int require_approval;

    char *reason;

} cc_policy_decision_t;


/* policy engine vtable 前置声明。 */
typedef struct cc_policy_engine_vtable cc_policy_engine_vtable_t;

/* policy engine 接口对象前置声明。 */
typedef struct cc_policy_engine cc_policy_engine_t;

/*
 * policy engine 接口对象。
 *
 * self 指向具体策略实现，vtable 提供检查和销毁函数。核心 tool executor 只依赖该 port，
 * 因此可以替换默认策略或接入产品侧审批系统。
 */
struct cc_policy_engine {
    void *self;

    const cc_policy_engine_vtable_t *vtable;
};


/* policy engine vtable。 */
struct cc_policy_engine_vtable {


    /*
     * 检查一次工具调用。
     *
     * call/ctx 均为借用指针；out_decision 由调用方提供，函数成功后 reason 由调用方释放。
     * 实现应把 workspace、tool 名称、参数、网络/进程风险纳入判断。
     */
    cc_result_t (*check_tool_call)(
        void *self,
        const cc_tool_call_t *call,
        const cc_tool_context_t *ctx,
        cc_policy_decision_t *out_decision
    );



    /* 销毁策略实现 self。 */
    void (*destroy)(void *self);
};

/* 释放 policy decision 中的 reason 字符串并清零结构。 */
void cc_policy_decision_free(cc_policy_decision_t *decision);

#endif
