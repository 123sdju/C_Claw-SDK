#include "cc/cclaw.h"

#include <string.h>

#ifndef CC_LLM_OPENAI
#define CC_LLM_OPENAI 0
#endif
#ifndef CC_LLM_OLLAMA
#define CC_LLM_OLLAMA 0
#endif
#ifndef CC_LLM_ANTHROPIC
#define CC_LLM_ANTHROPIC 0
#endif
#ifndef CC_TOOL_FILE_READ
#define CC_TOOL_FILE_READ 0
#endif
#ifndef CC_TOOL_FILE_WRITE
#define CC_TOOL_FILE_WRITE 0
#endif
#ifndef CC_TOOL_HTTP_REQUEST
#define CC_TOOL_HTTP_REQUEST 0
#endif
#ifndef CC_HAS_MEMORY
#define CC_HAS_MEMORY 0
#endif

/*
 * 函数 find_provider：实现 cclaw/tests/core/test_default_features.c 中的 find provider 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
static const cc_llm_provider_descriptor_t *find_provider(
    const cc_runtime_feature_set_t *features,
    const char *name
)
{
    for (size_t i = 0; features && i < features->llm_provider_count; i++) {
        if (features->llm_providers[i].name &&
            strcmp(features->llm_providers[i].name, name) == 0) {
            return &features->llm_providers[i];
        }
    }
    return NULL;
}

/*
 * 函数 find_tool：实现 cclaw/tests/core/test_default_features.c 中的 find tool 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
static const cc_tool_descriptor_t *find_tool(
    const cc_runtime_feature_set_t *features,
    const char *name
)
{
    for (size_t i = 0; features && i < features->tool_count; i++) {
        if (features->tools[i].name && strcmp(features->tools[i].name, name) == 0) {
            return &features->tools[i];
        }
    }
    return NULL;
}

/*
 * 函数 main：实现 cclaw/tests/core/test_default_features.c 中的 main 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
int main(void)
{
    const cc_runtime_feature_set_t *features = cc_app_default_features();
    if (!features) return 1;
    if (!features->create_session_store ||
        !features->create_policy_engine) {
        return 1;
    }
    if (CC_HAS_MEMORY && !features->create_memory_store) return 1;
    if (!CC_HAS_MEMORY && features->create_memory_store) return 1;

    const cc_llm_provider_descriptor_t *openai = find_provider(features, "openai");
    const cc_llm_provider_descriptor_t *ollama = find_provider(features, "ollama");
    const cc_llm_provider_descriptor_t *anthropic = find_provider(features, "anthropic");
    if (!openai || !ollama || !anthropic) return 1;
    if (openai->compiled != CC_LLM_OPENAI) return 1;
    if (ollama->compiled != CC_LLM_OLLAMA) return 1;
    if (anthropic->compiled != CC_LLM_ANTHROPIC) return 1;

    const cc_tool_descriptor_t *file_read = find_tool(features, "file_read");
    const cc_tool_descriptor_t *file_write = find_tool(features, "file_write");
    const cc_tool_descriptor_t *http = find_tool(features, "http.request");
    const cc_tool_descriptor_t *memory = find_tool(features, "memory");
    if (!file_read || !file_write || !http || !memory) return 1;
    if (file_read->compiled != CC_TOOL_FILE_READ) return 1;
    if (file_write->compiled != CC_TOOL_FILE_WRITE) return 1;
    if (http->compiled != CC_TOOL_HTTP_REQUEST) return 1;
    if (memory->compiled != CC_HAS_MEMORY) return 1;
    return 0;
}
