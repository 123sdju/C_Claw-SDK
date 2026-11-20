



#ifndef CC_POLICY_ENGINE_H
#define CC_POLICY_ENGINE_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_tool_call.h"
#include "cc/ports/cc_tool.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 供策略实现表达风险的分类，最终允许和审批由决策字段表达。
 * 当前默认实现只按名称匹配规则，不读取此枚举。
 */
typedef enum cc_risk_level {
    CC_RISK_SAFE, /**< 低风险分类。 */

    CC_RISK_MEDIUM, /**< 需要结合业务上下文的分类。 */

    CC_RISK_DANGEROUS /**< 高风险分类，采用何种规则由策略实现决定。 */

} cc_risk_level_t;

/*
 * 一次检查输出的拥有决策，caller 对成功及失败部分输出都执行 decision_free。
 * executor 仅在 allowed 为真时继续，require_approval 再触发审批，缺 handler 时拒绝。
 */
typedef struct cc_policy_decision {
    int allowed; /**< 是否允许进入后续准入。 */

    int require_approval; /**< 是否仍须同步审批。 */

    char *reason; /**< 可选独立拥有原因字符串，兼容 free。 */

} cc_policy_decision_t;


/* policy engine vtable 前置声明。 */
typedef struct cc_policy_engine_vtable cc_policy_engine_vtable_t;

/* policy engine 接口对象前置声明。 */
typedef struct cc_policy_engine cc_policy_engine_t;

/*
 * 策略实例描述符，创建者持有唯一销毁责任。按值复制仅借用 self/vtable，
 * owner 维持借用调用寿命，方法表本身不提供同步或自动租约。
 */
struct cc_policy_engine {
    void *self; /**< 实现拥有的状态，是否允许 NULL 沿该实现。 */

    const cc_policy_engine_vtable_t *vtable; /**< 覆盖全部调用及销毁的稳定方法表。 */
};


/* 策略方法表。 */
struct cc_policy_engine_vtable {


    /**
     * @brief 同步生成工具准入和审批决策。
     * @param[in,out] self 调用期存活状态借用，并发及 NULL 支持沿实现。
     * @param[in] call 稳定调用记录借用，无效输入处理沿实现。
     * @param[in] ctx 调用期可选上下文借用，外部服务寿命覆盖检查。
     * @param[out] out_decision 必填新对象或已 free 槽，已发布原因交 caller decision_free，包括错误部分输出。
     * @return 拥有结果交 caller 释放，非 OK 表示检查失败，CC_OK 再读 allowed/require_approval。
     * @note 不接管输入或执行工具，审批回调由 executor 在检查成功后组织。
     */
    cc_result_t (*check_tool_call)(
        void *self,
        const cc_tool_call_t *call,
        const cc_tool_context_t *ctx,
        cc_policy_decision_t *out_decision
    );



    /** @brief 消费实现状态。 @param[in,out] self 独占拥有状态，NULL 支持沿实现。 @pre owner 已关闭入口并等待检查及借用退出。 @post 旧状态别名失效，外部描述符仍归 caller。 */
    void (*destroy)(void *self);
};

/** @brief 消费原因并清零决策。 @param[in,out] decision 可空、独占且已初始化记录，原因兼容 free。 @post 字段归还且结构清零，容器保留，空记录可重复清理。 */
void cc_policy_decision_free(cc_policy_decision_t *decision);

#ifdef __cplusplus
}
#endif

#endif
