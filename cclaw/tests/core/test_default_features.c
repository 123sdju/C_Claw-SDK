#include "cc/cclaw.h"

#include <string.h>

/* 测试与默认集合使用相同的能力宏缺省，未传入的开关按关闭核对。 */
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

/**
 * @brief 从默认描述符数组按精确名称查找 provider。
 * @param[in] features 借用能力集合，可为 NULL 时返回 NULL，非零计数须匹配有效数组。
 * @param[in] name 有效借用目标名称。
 * @return 匹配描述符的借用地址，缺失返回 NULL，寿命受集合约束。
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

/**
 * @brief 从默认工具数组按精确规范名称查找描述符。
 * @param[in] features 借用能力集合，可为 NULL，非零计数须匹配有效数组。
 * @param[in] name 有效借用工具名称，此 helper 按规范名匹配。
 * @return 匹配描述符的借用地址，缺失返回 NULL，寿命受集合约束。
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

/**
 * @brief 检查诊断集合的默认文本、字段截断、容量上限和完整清零。
 * @return 全部检查通过返回零，失败返回一；仅使用按值对象和静态输入。
 */
static int test_diagnostics(void)
{
    cc_runtime_diagnostics_t diagnostics;
    cc_runtime_diagnostics_reset(NULL);
    cc_runtime_diagnostics_add(NULL, NULL, NULL, NULL);
    cc_runtime_diagnostics_reset(&diagnostics);
    cc_runtime_diagnostics_add(&diagnostics, NULL, NULL, NULL);
    if (diagnostics.count != 1 || diagnostics.truncated ||
        strcmp(diagnostics.items[0].kind, "tool") != 0 ||
        strcmp(diagnostics.items[0].id, "(unknown)") != 0 ||
        strcmp(diagnostics.items[0].message, "unavailable") != 0) return 1;
    char long_text[256];
    memset(long_text, 'x', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    cc_runtime_diagnostics_add(&diagnostics, long_text, long_text, long_text);
    if (diagnostics.count != 2 || diagnostics.truncated ||
        strlen(diagnostics.items[1].kind) != sizeof(diagnostics.items[1].kind) - 1 ||
        strlen(diagnostics.items[1].id) != sizeof(diagnostics.items[1].id) - 1 ||
        strlen(diagnostics.items[1].message) != sizeof(diagnostics.items[1].message) - 1) return 1;
    while (diagnostics.count < CC_RUNTIME_DIAGNOSTICS_MAX)
        cc_runtime_diagnostics_add(&diagnostics, "tool", "kept", "kept");
    cc_runtime_diagnostics_add(&diagnostics, "discarded", "discarded", "discarded");
    if (diagnostics.count != CC_RUNTIME_DIAGNOSTICS_MAX || !diagnostics.truncated ||
        strcmp(diagnostics.items[0].id, "(unknown)") != 0 ||
        strcmp(diagnostics.items[CC_RUNTIME_DIAGNOSTICS_MAX - 1].id, "kept") != 0) return 1;
    cc_runtime_diagnostics_reset(&diagnostics);
    const unsigned char *bytes = (const unsigned char *)&diagnostics;
    for (size_t i = 0; i < sizeof(diagnostics); i++) if (bytes[i]) return 1;
    return 0;
}

/**
 * @brief 验证被裁剪描述符保留工厂，直接调用明确返回不支持且输出为空。
 * @param[in] features 有效静态默认集合，调用期间借用。
 * @return 通过返回零，失败返回一；每次拥有诊断均释放，无端口资源交付。
 */
static int test_disabled_factories(const cc_runtime_feature_set_t *features)
{
    for (size_t i = 0; i < features->llm_provider_count; i++) {
        const cc_llm_provider_descriptor_t *provider = &features->llm_providers[i];
        if (!provider->create) return 1;
        if (provider->compiled) continue;
        cc_llm_provider_t output = {0};
        cc_result_t rc = provider->create(NULL, NULL, &output);
        int failed = rc.code != CC_ERR_UNSUPPORTED || output.self || output.vtable;
        cc_result_free(&rc);
        if (failed) return 1;
    }
    for (size_t i = 0; i < features->tool_count; i++) {
        const cc_tool_descriptor_t *tool = &features->tools[i];
        if (!tool->create) return 1;
        if (tool->compiled) continue;
        cc_tool_t output = {0};
        cc_result_t rc = tool->create(NULL, &output);
        int failed = rc.code != CC_ERR_UNSUPPORTED || output.self || output.vtable;
        cc_result_free(&rc);
        if (failed) return 1;
    }
    return 0;
}

/**
 * @brief 核对默认集合的静态寿命、编译开关、基础工厂和诊断边界。
 * @return 全部检查通过返回零，失败返回一。
 */
int main(void)
{
    const cc_runtime_feature_set_t *features = cc_app_default_features();
    if (!features) return 1;
    if (features != cc_app_default_features() || features->llm_provider_count != 3 ||
        features->tool_count != 4 || features->create_sandbox || features->load_plugins ||
        features->destroy_plugins || features->load_mcp || features->destroy_mcp) return 1;
    if (test_diagnostics() || test_disabled_factories(features)) return 1;
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
