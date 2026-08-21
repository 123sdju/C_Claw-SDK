

#ifndef CC_CANCEL_TOKEN_H
#define CC_CANCEL_TOKEN_H

#include "cc/core/cc_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 取消源拥有取消状态和 token。
 *
 * 调用方持有 source，用它发出取消；被调用模块只拿 token 查询状态。这个拆分类似
 * C# CancellationTokenSource/CancellationToken，能避免下游模块随意触发取消。
 */
typedef struct cc_cancel_source cc_cancel_source_t;

/* 只读取消 token；由 source 拥有，不能单独 destroy。 */
typedef struct cc_cancel_token cc_cancel_token_t;

/* 创建取消源；成功后调用方用 cc_cancel_source_destroy() 释放。 */
cc_result_t cc_cancel_source_create(cc_cancel_source_t **out_source);

/* 销毁取消源和内部 token；调用前应保证没有线程继续使用 token。 */
void cc_cancel_source_destroy(cc_cancel_source_t *source);

/* 标记取消；该操作线程安全，重复调用保持取消状态。 */
void cc_cancel_source_cancel(cc_cancel_source_t *source);

/* 获取 source 拥有的 token；返回指针生命周期不超过 source。 */
cc_cancel_token_t *cc_cancel_source_token(cc_cancel_source_t *source);

/* 查询 token 是否已取消；NULL token 视为未取消，便于可选取消参数。 */
int cc_cancel_token_is_cancelled(cc_cancel_token_t *token);

#ifdef __cplusplus
}
#endif

#endif
