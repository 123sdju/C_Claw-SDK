

#ifndef CC_RUNTIME_FEATURES_H
#define CC_RUNTIME_FEATURES_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_http_client.h"
#include "cc/ports/cc_llm_provider.h"
#include "cc/ports/cc_memory_store.h"
#include "cc/ports/cc_policy_engine.h"
#include "cc/ports/cc_sandbox.h"
#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/util/cc_config.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* runtime builder 最多记录的诊断条数；超过后设置 truncated，避免动态扩容。 */
#define CC_RUNTIME_DIAGNOSTICS_MAX 32

/*
 * 单条 runtime 构建诊断。
 *
 * 使用固定长度数组是为了让配置加载失败时不再依赖额外堆内存；kind/id/message 用于
 * 说明哪个 provider/tool/plugin 缺失或不可用。
 */
typedef struct cc_runtime_diagnostic {
    char kind[24];
    char id[80];
    char message[192];
} cc_runtime_diagnostic_t;

/* runtime 构建诊断集合；由 builder 填充，调用方只读。 */
typedef struct cc_runtime_diagnostics {
    cc_runtime_diagnostic_t items[CC_RUNTIME_DIAGNOSTICS_MAX];
    size_t count;
    int truncated;
} cc_runtime_diagnostics_t;

/* 清空诊断集合；inline 是为了 public header 中轻量复用。 */
static inline void cc_runtime_diagnostics_reset(cc_runtime_diagnostics_t *diagnostics)
{
    if (!diagnostics) return;
    memset(diagnostics, 0, sizeof(*diagnostics));
}

/*
 * 添加一条诊断。
 *
 * 超出固定容量时只设置 truncated，不分配更多内存。这是嵌入式友好的失败报告方式：
 * 保留前几条关键信息，同时不让错误报告本身造成 OOM。
 */
static inline void cc_runtime_diagnostics_add(
    cc_runtime_diagnostics_t *diagnostics,
    const char *kind,
    const char *id,
    const char *message
)
{
    if (!diagnostics) return;
    if (diagnostics->count >= CC_RUNTIME_DIAGNOSTICS_MAX) {
        diagnostics->truncated = 1;
        return;
    }
    cc_runtime_diagnostic_t *item = &diagnostics->items[diagnostics->count++];
    snprintf(item->kind, sizeof(item->kind), "%s", kind ? kind : "tool");
    snprintf(item->id, sizeof(item->id), "%s", id ? id : "(unknown)");
    snprintf(item->message, sizeof(item->message), "%s", message ? message : "unavailable");
}


/* LLM provider factory；config 借用，out_provider 成功后由 runtime 管理。 */
typedef cc_result_t (*cc_runtime_llm_create_fn)(
    const cc_config_t *config,
    cc_http_client_t *http_client,
    cc_llm_provider_t *out_provider
);

/* session store factory；根据配置选择 JSON/SQLite/memory 等后端。 */
typedef cc_result_t (*cc_runtime_session_store_create_fn)(
    const cc_config_t *config,
    cc_session_store_t *out_store
);

/* memory store factory；可由下游替换为向量库/嵌入式 KV 存储。 */
typedef cc_result_t (*cc_runtime_memory_store_create_fn)(
    const cc_config_t *config,
    cc_memory_store_t *out_store
);

/* policy engine factory；默认策略之外可以接入产品审批系统。 */
typedef cc_result_t (*cc_runtime_policy_create_fn)(
    const cc_config_t *config,
    cc_policy_engine_t *out_policy
);

/* sandbox factory；在支持进程的平台创建本地 sandbox，不支持时可返回 unsupported。 */
typedef cc_result_t (*cc_runtime_sandbox_create_fn)(
    const cc_config_t *config,
    cc_sandbox_t *out_sandbox
);

/*
 * tool factory 上下文。
 *
 * config/filesystem/memory_store/create_sandbox 都由 runtime builder 提供；工具 factory
 * 只在创建期间借用它们，不能把 ctx 指针保存到工具实例外部。
 */
typedef struct cc_runtime_tool_factory_ctx {

    const cc_config_t *config;

    cc_filesystem_t filesystem;

    cc_http_client_t *http_client;

    cc_memory_store_t *memory_store;

    cc_runtime_sandbox_create_fn create_sandbox;
} cc_runtime_tool_factory_ctx_t;

/* tool factory；成功后 out_tool 通常注册到 registry 并由 registry 管理 self。 */
typedef cc_result_t (*cc_runtime_tool_create_fn)(
    const cc_runtime_tool_factory_ctx_t *ctx,
    cc_tool_t *out_tool
);

/*
 * plugin load 扩展点。
 *
 * SDK 核心不内置业务 plugin，只提供加载扩展点。out_state 由 loader 返回，runtime 销毁
 * 时交给 destroy_plugins；diagnostics 用于记录加载失败但可继续运行的原因。
 */
typedef cc_result_t (*cc_runtime_plugin_load_fn)(
    const cc_config_t *config,
    cc_tool_registry_t *registry,
    void **out_state,
    cc_runtime_diagnostics_t *diagnostics
);

/* 销毁 plugin loader 返回的 state。 */
typedef void (*cc_runtime_plugin_destroy_fn)(void *state);

/* MCP load 扩展点；语义同 plugin，但面向 MCP server/tool 注册。 */
typedef cc_result_t (*cc_runtime_mcp_load_fn)(
    const cc_config_t *config,
    cc_tool_registry_t *registry,
    void **out_state,
    cc_runtime_diagnostics_t *diagnostics
);

/* 销毁 MCP loader 返回的 state。 */
typedef void (*cc_runtime_mcp_destroy_fn)(void *state);

/*
 * provider 描述符。
 *
 * compiled 标识该 provider 是否进入当前 profile；create 为 NULL 或 compiled=0 时 builder
 * 应给出诊断而不是运行时崩溃。
 */
typedef struct cc_llm_provider_descriptor {

    const char *name;

    int compiled;

    cc_runtime_llm_create_fn create;
} cc_llm_provider_descriptor_t;

/*
 * tool 描述符。
 *
 * name 是配置中使用的稳定名，alias 允许兼容简写；compiled 标识当前 profile 是否包含
 * 该工具实现。
 */
typedef struct cc_tool_descriptor {

    const char *name;

    const char *alias;

    int compiled;

    cc_runtime_tool_create_fn create;
} cc_tool_descriptor_t;

/*
 * runtime feature set。
 *
 * 该结构把“当前构建包含哪些 provider/tool/storage/policy/plugin/MCP 扩展点”集中交给
 * runtime builder。它是 extension-only 边界：SDK 提供 core ports 和可选 adapter，
 * 不把产品 gateway 或业务工具塞进核心。
 */
typedef struct cc_runtime_feature_set {

    const cc_llm_provider_descriptor_t *llm_providers;

    size_t llm_provider_count;


    const cc_tool_descriptor_t *tools;

    size_t tool_count;


    cc_runtime_session_store_create_fn create_session_store;

    cc_runtime_memory_store_create_fn create_memory_store;

    cc_runtime_policy_create_fn create_policy_engine;

    cc_runtime_sandbox_create_fn create_sandbox;


    cc_runtime_plugin_load_fn load_plugins;

    cc_runtime_plugin_destroy_fn destroy_plugins;

    cc_runtime_mcp_load_fn load_mcp;

    cc_runtime_mcp_destroy_fn destroy_mcp;
} cc_runtime_feature_set_t;

#endif
