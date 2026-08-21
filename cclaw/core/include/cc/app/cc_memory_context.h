



#ifndef CC_MEMORY_CONTEXT_H
#define CC_MEMORY_CONTEXT_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_memory_store.h"

/*
 * 从 memory store 检索并生成可注入 prompt 的 memory block。
 *
 * session_text 是当前会话摘要或用户输入文本，store 用它做检索；out_memory_block 成功后
 * 由调用方 free()。该函数只生成上下文文本，不把 embedding/vector adapter 固化进核心。
 */
cc_result_t cc_memory_context_inject(
    cc_memory_store_t *store,
    const char *session_text,
    char **out_memory_block
);

#endif
