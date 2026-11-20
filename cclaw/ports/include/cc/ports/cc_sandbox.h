



#ifndef CC_SANDBOX_H
#define CC_SANDBOX_H

#include "cc/core/cc_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 一次同步执行的借用输入，具体后端定义命令、工作目录和环境格式及默认。
 * 结构和指向内容由 caller 保持至 run 返回，策略与审批由组合调用方组织。
 */
typedef struct cc_sandbox_command {
    char *command; /**< 命令文本借用，后端解释其语法。 */

    char *working_dir; /**< 可选工作目录借用，路径边界由具体实现实施。 */

    char **env; /**< 可选环境数组及字符串借用，终止与覆盖规则沿后端。 */

    int timeout_ms; /**< 执行预算毫秒，非正值缺省沿后端。 */

} cc_sandbox_command_t;

/*
 * 执行后的拥有输出，stdout/stderr 各自为独立兼容 free 的分配。
 * caller 清理成功或失败时已发布的字段，具体结果有效性沿 run 的返回契约。
 */
typedef struct cc_sandbox_result {
    int exit_code; /**< 后端观察到的进程退出值。 */

    char *stdout_text; /**< 可选拥有标准输出。 */

    char *stderr_text; /**< 可选拥有标准错误。 */

    int timed_out; /**< 后端是否按超时路径结束执行。 */

} cc_sandbox_result_t;


/* sandbox vtable 前置声明。 */
typedef struct cc_sandbox_vtable cc_sandbox_vtable_t;

/* sandbox 接口对象前置声明。 */
typedef struct cc_sandbox cc_sandbox_t;

/*
 * 沙箱描述符，创建者拥有 self，复制描述符产生借用。
 * 当前仓库声明执行端口并提供通用结果清理，具体 run 后端由接入方提供。
 */
struct cc_sandbox {
    void *self; /**< 实现状态，寿命覆盖全部 run。 */

    const cc_sandbox_vtable_t *vtable; /**< 稳定借用方法表。 */
};


/* 沙箱方法表。 */
struct cc_sandbox_vtable {


    /**
     * @brief 按后端约定同步执行命令并交付拥有输出。
     * @param[in,out] self 调用期存活的状态借用，并发支持沿实现。
     * @param[in] command 稳定命令及嵌套借用，格式、NULL 和预算规则沿后端。
     * @param[out] out_result 必填新对象或已清理槽，全部已发布字符串交 caller result_free，包括错误部分结果。
     * @return 拥有诊断交 caller cc_result_free，进程退出码和超时另读有效输出字段。
     */
    cc_result_t (*run)(
        void *self,
        const cc_sandbox_command_t *command,
        cc_sandbox_result_t *out_result
    );



    /** @brief 消费实现状态。 @param[in,out] self 独占状态，NULL 支持沿实现。 @pre owner 已关闭入口并排空 run 与借用。 @post 实现状态及旧别名失效。 */
    void (*destroy)(void *self);
};

/** @brief 消费两个输出字符串并清零结果。 @param[in,out] result 可空独占初始化记录，两字符串独立且兼容 free。 @post 字段释放、结构清零、容器保留，空记录可重复清理。 */
void cc_sandbox_result_free(cc_sandbox_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
