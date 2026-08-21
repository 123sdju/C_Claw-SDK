



#ifndef CC_MEMORY_TOOL_FACTORY_H
#define CC_MEMORY_TOOL_FACTORY_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_memory_store.h"

/*
 * 创建 memory 工具。
 *
 * store 由调用方提供并在工具生命周期内保持有效；out_tool 成功后交给 registry 管理。
 * 该工具把 memory store 暴露为 LLM 可调用能力，但不绑定具体向量库或业务知识库。
 */
cc_result_t cc_memory_tool_create(cc_memory_store_t *store, cc_tool_t *out_tool);

/*
 * 创建 memory store adapter。
 *
 * backend/path 只在调用期间借用；out_store 成功后持有具体 adapter self。该 factory 用于
 * profile/配置选择 in-memory、JSON file、SQLite 等实现。
 */
cc_result_t cc_memory_store_factory_create(
    cc_memory_store_t *out_store,
    const char *backend,
    const char *path
);

#endif
