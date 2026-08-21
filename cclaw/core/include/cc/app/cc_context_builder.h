



#ifndef CC_CONTEXT_BUILDER_H
#define CC_CONTEXT_BUILDER_H

#include "cc/core/cc_result.h"
#include "cc/app/cc_agent_runtime.h"
#include <stddef.h>

/*
 * 为一次 LLM 请求构建上下文消息数组。
 *
 * 函数会从 runtime/session store 中读取历史消息，合并 system_prompt，并按 runtime
 * 配置执行窗口裁剪、摘要或 active memory 注入。out_messages 成功后由调用方逐项
 * cc_message_cleanup() 再 free；out_count 返回数组长度。
 */
cc_result_t cc_context_builder_build_messages(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *system_prompt,
    cc_cancel_token_t *cancel_token,
    cc_message_t **out_messages,
    size_t *out_count
);

#endif
