#include "cc/app/cc_app_features.h"

#include "cc/adapters/cc_builtin_tools.h"
#include "cc/adapters/cc_default_policy_engine.h"
#include "cc/adapters/cc_llm_providers.h"
#include "cc/ports/cc_http_client.h"
#include "cc/ports/cc_memory_tool_factory.h"
#include "cc/ports/cc_storage_factory.h"

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
 * 从 config 创建 OpenAI LLM provider，预先配置 HTTP 客户端（TLS/超时/重试/日志）。
 * 参数: config - SDK 配置, out_provider - 输出 provider 句柄
 * 返回: CC_OK 或 CC_ERR_UNSUPPORTED（编译时未启用）
 */
static cc_result_t create_openai_llm(
    const cc_config_t *config,
    cc_http_client_t *http_client,
    cc_llm_provider_t *out_provider)
{
#if CC_LLM_OPENAI
    return cc_openai_provider_create(
        http_client,
        config ? config->base_url : NULL,
        config ? config->api_key : NULL,
        config ? config->model : NULL,
        out_provider);
#else
    (void)config;
    (void)http_client;
    (void)out_provider;
    return cc_result_error(CC_ERR_UNSUPPORTED, "OpenAI provider is disabled in this build");
#endif
}

/*
 * 从 config 创建 Ollama LLM provider（本地模型）。
 * 参数: config - SDK 配置, out_provider - 输出 provider 句柄
 * 返回: CC_OK 或 CC_ERR_UNSUPPORTED
 */
static cc_result_t create_ollama_llm(
    const cc_config_t *config,
    cc_http_client_t *http_client,
    cc_llm_provider_t *out_provider)
{
#if CC_LLM_OLLAMA
    return cc_ollama_provider_create(
        http_client,
        config ? config->base_url : NULL,
        config ? config->model : NULL,
        out_provider);
#else
    (void)config;
    (void)http_client;
    (void)out_provider;
    return cc_result_error(CC_ERR_UNSUPPORTED, "Ollama provider is disabled in this build");
#endif
}

/*
 * 从 config 创建 Anthropic LLM provider，预先配置 HTTP 客户端。
 * 参数: config - SDK 配置, out_provider - 输出 provider 句柄
 * 返回: CC_OK 或 CC_ERR_UNSUPPORTED
 */
static cc_result_t create_anthropic_llm(
    const cc_config_t *config,
    cc_http_client_t *http_client,
    cc_llm_provider_t *out_provider)
{
#if CC_LLM_ANTHROPIC
    return cc_anthropic_provider_create(
        http_client,
        config ? config->base_url : NULL,
        config ? config->api_key : NULL,
        config ? config->model : NULL,
        out_provider);
#else
    (void)config;
    (void)http_client;
    (void)out_provider;
    return cc_result_error(CC_ERR_UNSUPPORTED, "Anthropic provider is disabled in this build");
#endif
}

/*
 * 委托 storage_factory 根据 storage_type 创建 session store。
 * 参数: config - SDK 配置, out_store - 输出 store 句柄
 */
static cc_result_t create_session_store(const cc_config_t *config, cc_session_store_t *out_store)
{
    return cc_storage_factory_create_store(config, out_store);
}

/*
 * 根据 config 的 memory_backend 创建 memory store（默认 json_file）。
 * 参数: config - SDK 配置, out_store - 输出 store 句柄
 */
#if CC_HAS_MEMORY
static cc_result_t create_memory_store(const cc_config_t *config, cc_memory_store_t *out_store)
{
    const char *backend = (config && config->memory_backend) ? config->memory_backend : "json_file";
    const char *path = config ? config->memory_path : NULL;
    return cc_memory_store_factory_create(out_store, backend, path);
}
#define CC_DEFAULT_CREATE_MEMORY_STORE create_memory_store
#else
#define CC_DEFAULT_CREATE_MEMORY_STORE NULL
#endif

/*
 * 创建默认策略引擎，传入 shell 命令是否需要审批的配置。
 * 参数: config - SDK 配置, out_policy - 输出 policy 句柄
 */
static cc_result_t create_policy_engine(const cc_config_t *config, cc_policy_engine_t *out_policy)
{
    return cc_policy_engine_create_default(
        config ? config->shell_requires_approval : 1,
        out_policy);
}

/*
 * 从运行时上下文创建 file_read 工具，需要 filesystem 可用。
 * 参数: ctx - 运行时工具工厂上下文, out_tool - 输出 tool 句柄
 */
static cc_result_t create_file_read_tool(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
#if CC_TOOL_FILE_READ
    if (!ctx || !ctx->filesystem.vtable) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Filesystem is unavailable");
    }
    return cc_file_read_tool_create(ctx->filesystem, out_tool);
#else
    (void)ctx;
    (void)out_tool;
    return cc_result_error(CC_ERR_UNSUPPORTED, "file_read tool is disabled in this build");
#endif
}

/*
 * 从运行时上下文创建 file_write 工具，需要 filesystem 可用。
 * 参数: ctx - 运行时工具工厂上下文, out_tool - 输出 tool 句柄
 */
static cc_result_t create_file_write_tool(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
#if CC_TOOL_FILE_WRITE
    if (!ctx || !ctx->filesystem.vtable) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Filesystem is unavailable");
    }
    return cc_file_write_tool_create(ctx->filesystem, out_tool);
#else
    (void)ctx;
    (void)out_tool;
    return cc_result_error(CC_ERR_UNSUPPORTED, "file_write tool is disabled in this build");
#endif
}

/*
 * 从运行时上下文创建 http.request 工具，支持网络白名单。
 * 参数: ctx - 运行时工具工厂上下文, out_tool - 输出 tool 句柄
 */
static cc_result_t create_http_request_tool(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
#if CC_TOOL_HTTP_REQUEST
    if (ctx && ctx->config) {
        return cc_http_request_tool_create_with_allowlist(
            ctx->http_client,
            &ctx->config->tools.network_allowlist,
            out_tool);
    }
    return cc_http_request_tool_create(ctx ? ctx->http_client : NULL, out_tool);
#else
    (void)ctx;
    (void)out_tool;
    return cc_result_error(CC_ERR_UNSUPPORTED, "http.request tool is disabled in this build");
#endif
}

/*
 * 从运行时上下文创建 memory 工具，需要 memory_store 可用。
 * 参数: ctx - 运行时工具工厂上下文, out_tool - 输出 tool 句柄
 */
static cc_result_t create_memory_tool(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
#if CC_HAS_MEMORY
    if (!ctx || !ctx->memory_store || !ctx->memory_store->vtable) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Memory store is unavailable");
    }
    return cc_memory_tool_create(ctx->memory_store, out_tool);
#else
    (void)ctx;
    (void)out_tool;
    return cc_result_error(CC_ERR_UNSUPPORTED, "memory tool is disabled in this build");
#endif
}

static const cc_llm_provider_descriptor_t s_llm_providers[] = {
    {"openai", CC_LLM_OPENAI, create_openai_llm},
    {"ollama", CC_LLM_OLLAMA, create_ollama_llm},
    {"anthropic", CC_LLM_ANTHROPIC, create_anthropic_llm},
};

static const cc_tool_descriptor_t s_tools[] = {
    {"file_read", NULL, CC_TOOL_FILE_READ, create_file_read_tool},
    {"file_write", NULL, CC_TOOL_FILE_WRITE, create_file_write_tool},
    {"http.request", "http", CC_TOOL_HTTP_REQUEST, create_http_request_tool},
    {"memory", NULL, CC_HAS_MEMORY, create_memory_tool},
};

static const cc_runtime_feature_set_t s_features = {
    .llm_providers = s_llm_providers,
    .llm_provider_count = sizeof(s_llm_providers) / sizeof(s_llm_providers[0]),
    .tools = s_tools,
    .tool_count = sizeof(s_tools) / sizeof(s_tools[0]),
    .create_session_store = create_session_store,
    .create_memory_store = CC_DEFAULT_CREATE_MEMORY_STORE,
    .create_policy_engine = create_policy_engine,
};

/*
 * 返回编译期注册的默认 feature set（provider/storage/tool/policy/sandbox 工厂集合）。
 * 返回: 指向静态 feature_set 的指针
 */
const cc_runtime_feature_set_t *cc_app_default_features(void)
{
    return &s_features;
}
