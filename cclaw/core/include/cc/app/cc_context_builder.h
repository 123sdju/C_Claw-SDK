#ifndef CC_CONTEXT_BUILDER_H
#define CC_CONTEXT_BUILDER_H

#include "cc/core/cc_result.h"
#include "cc/app/cc_agent_runtime.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 读取会话历史，组合规则和记忆，再按窗口预算压缩、裁剪及加载媒体。
 * @param[in,out] runtime 已创建的运行时，借用其配置、存储、provider 与摘要缓存。
 * @param[in] session_id 非 NULL 会话标识，同步调用期间保持有效。
 * @param[in] system_prompt 可为 NULL 的系统提示，同步调用期间借用，NULL 按空文本。
 * @param[in] cancel_token 可为 NULL 的借用取消令牌，非 NULL 时须覆盖本次调用寿命。
 * @param[out] out_messages 非 NULL 数组输出，参数有效后先置 NULL。成功取得独立拥有
 * 的消息及字段，调用者逐项 cc_message_cleanup 后 free 数组。
 * @param[out] out_count 非 NULL 数量输出，参数有效后先置 0，成功表示数组元素数。
 * @return 成功返回 CC_OK。必需的历史读取、记忆收集、估算、分类或媒体加载失败
 * 返回对应错误，当前轮次无法装入窗口返回 CC_ERR_LIMIT_EXCEEDED，观察到取消返回
 * CC_ERR_CANCELLED。失败清理已构造输出。摘要模型失败沿裁剪路径继续，派生缓存
 * 读写及观察事件按尽力完成处理。
 * @pre owner 保证运行时及其借用依赖存活，并串行化同一会话的历史修改。不同会话
 * 可并行构建，运行时互斥锁只保护共享摘要缓存，外部回调均在该锁之外执行。
 * @note 此调用可能进行摘要模型请求和缓存写入，随后取消或失败仍可留下已提交缓存。
 * 媒体限制由 CC_CONTEXT_MEDIA_MAX_BYTES 构建值传给存储，0 表示此层不设总量限制。
 */
cc_result_t cc_context_builder_build_messages(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *system_prompt,
    cc_cancel_token_t *cancel_token,
    cc_message_t **out_messages,
    size_t *out_count
);

#ifdef __cplusplus
}
#endif

#endif
