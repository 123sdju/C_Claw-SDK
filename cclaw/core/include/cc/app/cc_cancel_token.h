

#ifndef CC_CANCEL_TOKEN_H
#define CC_CANCEL_TOKEN_H

#include "cc/core/cc_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 拥有取消状态、配套 token 及互斥锁的取消源。
 *
 * 拥有者通过 source 发出取消，被调用模块借用 token 查询。状态由内部 mutex 保护，
 * 初始未取消，取消后持续为真。取消与查询可并发，销毁同步和借用期由外层拥有者保证。
 * 每次独立操作可创建自己的 source，操作结束并排空所有 token 使用者后再销毁。
 */
typedef struct cc_cancel_source cc_cancel_source_t;

/** source 拥有的只读查询句柄，随 source 销毁而失效。 */
typedef struct cc_cancel_token cc_cancel_token_t;

/**
 * @brief 创建未取消的 source 及其唯一配套 token。
 * @param[out] out_source 必填输出槽，成功取得拥有的 source，失败保持原槽值，槽中须无待释放对象。
 * @return 成功为 CC_OK，空输出为 CC_ERR_INVALID_ARGUMENT，分配失败为 CC_ERR_OUT_OF_MEMORY，mutex 创建错误原样返回，失败清理本次资源。
 */
cc_result_t cc_cancel_source_create(cc_cancel_source_t **out_source);

/**
 * @brief 释放取消源的 mutex、配套 token 及 source。
 * @pre 外层已排空 cancel、token 获取、取消查询及全部持有借用 token 的 provider/tool 调用。
 * @param[in,out] source 拥有的取消源，允许 NULL。
 * @post 非空 source 和 token 的寿命结束，全部借用指针失效。
 */
void cc_cancel_source_destroy(cc_cancel_source_t *source);

/**
 * @brief 锁内将取消状态置一，重复调用保持该状态。
 * @param[in,out] source 存活的取消源，允许 NULL，外层保证与销毁互斥。
 * @post 非空源的后续同步查询观察到取消状态，下游操作的退出由其取消检查与排空协议决定。
 */
void cc_cancel_source_cancel(cc_cancel_source_t *source);

/**
 * @brief 借出 source 创建时固定的 token。
 * @param[in] source 存活的取消源，允许 NULL。
 * @return 配套 token 的借用指针，寿命至 source 销毁，NULL source 返回 NULL。
 */
cc_cancel_token_t *cc_cancel_source_token(cc_cancel_source_t *source);

/**
 * @brief 通过 token 在源 mutex 内读取取消状态。
 * @param[in] token 本次调用借用的 token，允许 NULL，拥有 source 在查询期间保持存活。
 * @return 已取消为一，未取消或 NULL token 为零，查询只返回当前状态快照。
 */
int cc_cancel_token_is_cancelled(cc_cancel_token_t *token);

#ifdef __cplusplus
}
#endif

#endif
