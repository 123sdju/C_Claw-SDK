#include "cc/app/cc_app_features.h"

#include "cc/adapters/cc_builtin_tools.h"
#include "cc/adapters/cc_default_policy_engine.h"
#include "cc/adapters/cc_llm_providers.h"
#include "cc/ports/cc_http_client.h"
#include "cc/ports/cc_memory_tool_factory.h"
#include "cc/ports/cc_storage_factory.h"

/* 构建未提供的能力宏按关闭处理，描述符仍保留名称，compiled 与对应工厂分支保持一致。 */
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
 * @brief 将配置中的地址、凭据与模型转交 OpenAI-compatible 构造器。
 * @param[in] config 调用期间借用配置，NULL 时让适配器选择默认地址与模型。
 * @param[in,out] http_client 借用已有 HTTP 端口，开启能力时须有效并覆盖 provider 使用期。
 * @param[out] out_provider 有效空槽，成功取得拥有端口，调用者经 vtable destroy 释放。
 * @return 开启时原样返回适配器结果，关闭时为 UNSUPPORTED 且保持输出；结果消息由调用者释放。
 * @note 本层只传参，HTTP 的 TLS、超时、重试与日志配置由端口创建者管理。
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

/**
 * @brief 将地址与模型配置转交 Ollama 构造器。
 * @param[in] config 调用期间借用配置，NULL 时使用适配器默认值。
 * @param[in,out] http_client 借用有效 HTTP 端口，覆盖 provider 使用期。
 * @param[out] out_provider 有效空槽，成功接收拥有端口，由调用者 destroy。
 * @return 开启时返回适配器结果，关闭时为 UNSUPPORTED 并保持输出，诊断由调用者释放。
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

/**
 * @brief 将地址、凭据、模型及 extra_body 缓存设置转交 Anthropic 构造器。
 * @param[in] config 调用期间借用配置，NULL 使用适配器默认地址、模型和缓存模式。
 * @param[in,out] http_client 借用有效 HTTP 端口，覆盖 provider 使用期。
 * @param[out] out_provider 有效空槽，成功接收拥有端口，由调用者 destroy。
 * @return 开启时返回缓存解析或适配器创建结果，关闭时为 UNSUPPORTED，拥有诊断由调用者释放。
 * @note 本层沿适配器支持范围解析 extra_body 的缓存配置，输出失败行为沿具体构造器契约。
 */
static cc_result_t create_anthropic_llm(
    const cc_config_t *config,
    cc_http_client_t *http_client,
    cc_llm_provider_t *out_provider)
{
#if CC_LLM_ANTHROPIC
    return cc_anthropic_provider_create_configured(
        http_client,
        config ? config->base_url : NULL,
        config ? config->api_key : NULL,
        config ? config->model : NULL,
        config ? config->model_extra_body_json : NULL,
        out_provider);
#else
    (void)config;
    (void)http_client;
    (void)out_provider;
    return cc_result_error(CC_ERR_UNSUPPORTED, "Anthropic provider is disabled in this build");
#endif
}

/**
 * @brief 按 storage_type 委托存储工厂创建会话后端。
 * @param[in] config 有效借用配置，路径和预算在本次调用期间稳定。
 * @param[out] out_store 有效空槽，成功接收后端所有权，由调用者通过端口 destroy。
 * @return 原样交付参数、后端创建或编译裁剪结果，拥有诊断由调用者释放。
 */
static cc_result_t create_session_store(const cc_config_t *config, cc_session_store_t *out_store)
{
    return cc_storage_factory_create_store(config, out_store);
}

#if CC_HAS_MEMORY
/**
 * @brief 按 memory_backend 创建长期记忆后端，缺失后端使用 json_file。
 * @param[in] config 可选借用配置，NULL 使用默认后端及后端默认路径。
 * @param[out] out_store 有效空槽，成功移交拥有记忆端口，由调用者销毁。
 * @return 原样返回记忆工厂结果；失败沿所选后端输出契约，诊断由调用者释放。
 */
static cc_result_t create_memory_store(const cc_config_t *config, cc_memory_store_t *out_store)
{
    const char *backend = (config && config->memory_backend) ? config->memory_backend : "json_file";
    const char *path = config ? config->memory_path : NULL;
    return cc_memory_store_factory_create(out_store, backend, path);
}
/** @brief 记忆编入时提供默认工厂回调。 */
#define CC_DEFAULT_CREATE_MEMORY_STORE create_memory_store
#else
/** @brief 记忆裁剪时以 NULL 回调表达此能力缺失。 */
#define CC_DEFAULT_CREATE_MEMORY_STORE NULL
#endif

/**
 * @brief 创建默认策略引擎并传入 shell 审批开关。
 * @param[in] config 可选借用配置，NULL 时采用需要审批。
 * @param[out] out_policy 有效空槽，成功取得拥有策略端口，由调用者 destroy。
 * @return 原样交付默认策略创建结果，诊断由调用者释放。
 */
static cc_result_t create_policy_engine(const cc_config_t *config, cc_policy_engine_t *out_policy)
{
    return cc_policy_engine_create_default(
        config ? config->shell_requires_approval : 1,
        out_policy);
}

/**
 * @brief 从上下文借用文件系统创建读取工具，成功声明只读能力。
 * @param[in] ctx 开启能力时须为有效借用上下文，filesystem 须有 vtable，out_capabilities 可为空。
 * @param[out] out_tool 有效空槽，成功交付拥有工具，注册后由 registry 析构。
 * @return 开启时返回参数或工具创建结果，关闭为 UNSUPPORTED，拥有诊断由调用者释放。
 * @note 工具按值借用 filesystem，其底层接收者覆盖工具全部使用期；仅成功时写 READ_ONLY 能力输出。
 */
static cc_result_t create_file_read_tool(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
#if CC_TOOL_FILE_READ
    if (!ctx || !ctx->filesystem.vtable) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Filesystem is unavailable");
    }
    cc_result_t rc = cc_file_read_tool_create(ctx->filesystem, out_tool);
    if (rc.code == CC_OK && ctx->out_capabilities)
        *ctx->out_capabilities = CC_TOOL_CAPABILITY_READ_ONLY;
    return rc;
#else
    (void)ctx;
    (void)out_tool;
    return cc_result_error(CC_ERR_UNSUPPORTED, "file_read tool is disabled in this build");
#endif
}

/**
 * @brief 从上下文借用文件系统创建写入工具。
 * @param[in] ctx 开启能力时为有效借用上下文，filesystem 须有 vtable。
 * @param[out] out_tool 有效空槽，成功取得拥有工具，注册后由 registry 析构。
 * @return 开启时交付参数或构造结果，关闭为 UNSUPPORTED，诊断由调用者释放。
 * @note 底层 filesystem 覆盖工具寿命，本工厂保持能力输出原值。
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

/**
 * @brief 用上下文 HTTP 端口和可选配置白名单创建请求工具。
 * @param[in] ctx 借用上下文，可为 NULL 时交适配器返回依赖错误；有效客户端须覆盖工具寿命。
 * @param[out] out_tool 有效空槽，成功取得拥有工具，由注册表或未注册接收者析构。
 * @return 返回参数、客户端能力、白名单复制或工具创建结果，关闭为 UNSUPPORTED，诊断由调用者释放。
 * @note 白名单由工具创建器深拷贝，HTTP 客户端继续借用；本工厂保持能力输出原值。
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

/**
 * @brief 借用有效记忆端口创建普通记忆工具。
 * @param[in] ctx 开启能力时须有 memory_store 和 vtable 的有效借用上下文。
 * @param[out] out_tool 有效空槽，成功取得拥有工具，由注册表或接收者析构。
 * @return 开启时交付参数或创建结果，关闭为 UNSUPPORTED，拥有诊断由调用者释放。
 * @note 记忆端口覆盖工具使用期，本工厂保持能力输出原值。
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

/* 静态名称和回调始终保留，compiled 单独记录对应实现是否编入。 */
static const cc_llm_provider_descriptor_t s_llm_providers[] = {
    {"openai", CC_LLM_OPENAI, create_openai_llm},
    {"ollama", CC_LLM_OLLAMA, create_ollama_llm},
    {"anthropic", CC_LLM_ANTHROPIC, create_anthropic_llm},
};

/* 默认四类工具，http 是 http.request 的配置别名。 */
static const cc_tool_descriptor_t s_tools[] = {
    {"file_read", NULL, CC_TOOL_FILE_READ, create_file_read_tool},
    {"file_write", NULL, CC_TOOL_FILE_WRITE, create_file_write_tool},
    {"http.request", "http", CC_TOOL_HTTP_REQUEST, create_http_request_tool},
    {"memory", NULL, CC_HAS_MEMORY, create_memory_tool},
};

/* 静态能力集合由本模块保管；沙箱、plugin 与 MCP 装配回调缺省为空，产品可提供自己的集合。 */
static const cc_runtime_feature_set_t s_features = {
    .llm_providers = s_llm_providers,
    .llm_provider_count = sizeof(s_llm_providers) / sizeof(s_llm_providers[0]),
    .tools = s_tools,
    .tool_count = sizeof(s_tools) / sizeof(s_tools[0]),
    .create_session_store = create_session_store,
    .create_memory_store = CC_DEFAULT_CREATE_MEMORY_STORE,
    .create_policy_engine = create_policy_engine,
};

/**
 * @brief 借用当前编译开关对应的静态默认能力集合。
 * @return 始终返回 s_features 的只读地址，覆盖程序存活期，模块保管其所有权。
 * @note 无动态初始化，可并发读取；各工厂的端口和配置并发沿对应创建接口契约协调。
 */
const cc_runtime_feature_set_t *cc_app_default_features(void)
{
    return &s_features;
}
