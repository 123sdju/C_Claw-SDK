

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

/** @brief 诊断集合的固定条数上限，超限只标记 truncated，保持已有条目。 */
#define CC_RUNTIME_DIAGNOSTICS_MAX 32

/** @brief 按值拥有的构建诊断，各字段按字节截断并保证 NUL 结尾。 */
typedef struct cc_runtime_diagnostic {
    char kind[24]; /**< 组件分类，包含结尾 NUL 的容量为 24 字节。 */
    char id[80]; /**< 组件标识，包含结尾 NUL 的容量为 80 字节。 */
    char message[192]; /**< 诊断文本，包含结尾 NUL 的容量为 192 字节。 */
} cc_runtime_diagnostic_t;

/** @brief 拥有固定条目数组的诊断集合，读写由当前拥有者串行化。 */
typedef struct cc_runtime_diagnostics {
    cc_runtime_diagnostic_t items[CC_RUNTIME_DIAGNOSTICS_MAX]; /**< 按值条目，无动态资源。 */
    size_t count; /**< 已填写条数，正常初始化后不超过固定容量。 */
    int truncated; /**< 容量用尽后仍尝试追加时置一，字段文本本身的截断不设置此标志。 */
} cc_runtime_diagnostics_t;

/**
 * @brief 清空全部诊断条目、计数和集合截断标志。
 * @param[out] diagnostics 可为 NULL 的目标集合。
 * @post 非 NULL 目标完全归零，旧条目借用结束；调用者排除并发访问。
 */
static inline void cc_runtime_diagnostics_reset(cc_runtime_diagnostics_t *diagnostics)
{
    if (!diagnostics) return;
    memset(diagnostics, 0, sizeof(*diagnostics));
}

/**
 * @brief 在固定数组追加一条文本诊断，集合满时标记截断。
 * @param[in,out] diagnostics 已初始化的独占集合，可为 NULL 时跳过。
 * @param[in] kind 调用期借用组件类别，NULL 使用 tool。
 * @param[in] id 调用期借用标识，NULL 使用 (unknown)。
 * @param[in] message 调用期借用诊断，NULL 使用 unavailable。
 * @post 未满时复制文本并增加 count，已满时保持条目并置 truncated；复制超长字段按字节截断。
 * @note 无动态分配，调用者负责串行化；输入文本须在各自复制期间可读且与本次写入槽不重叠。
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


/**
 * @brief 按配置创建一个由 builder generation 拥有的模型端口。
 * @param[in] config 调用期间借用的配置，长期使用的字段由工厂复制。
 * @param[in,out] http_client 借用 HTTP 端口及其接收者，覆盖 provider 的全部使用期。
 * @param[out] out_provider 必填空槽，成功交付 self 和有效 vtable，归 generation 销毁。
 * @return 工厂结果及拥有诊断；失败由工厂清理尚未交接的部分资源，调用者释放结果消息。
 * @note 配置创建期间由调用者保持稳定，后续 provider 并发按其端口能力协调。
 */
typedef cc_result_t (*cc_runtime_llm_create_fn)(
    const cc_config_t *config,
    cc_http_client_t *http_client,
    cc_llm_provider_t *out_provider
);

/**
 * @brief 创建跨 generation 共享的会话存储端口。
 * @param[in] config 调用期间借用配置，工厂复制需长期使用的路径等字段。
 * @param[out] out_store 必填空槽，成功交付拥有端口，由 builder 在全部使用者退出后销毁。
 * @return 创建结果及拥有诊断；失败清理未交接资源，调用者释放结果消息。
 */
typedef cc_result_t (*cc_runtime_session_store_create_fn)(
    const cc_config_t *config,
    cc_session_store_t *out_store
);

/**
 * @brief 创建跨 generation 共享的长期记忆端口。
 * @param[in] config 调用期间借用配置，需保留的内容由工厂复制。
 * @param[out] out_store 必填空槽，成功交付拥有端口，builder 最终销毁。
 * @return 创建结果及拥有诊断；失败须清理本次资源并保持空输出，builder 可记录诊断后继续无记忆装配。
 */
typedef cc_result_t (*cc_runtime_memory_store_create_fn)(
    const cc_config_t *config,
    cc_memory_store_t *out_store
);

/**
 * @brief 创建供各代工具执行共同借用的策略端口。
 * @param[in] config 调用期间借用配置，需保留的策略内容由工厂复制。
 * @param[out] out_policy 必填空槽，成功交付由 builder 拥有并最终销毁的端口。
 * @return 创建结果及拥有诊断，失败清理未交接资源，调用者释放结果消息。
 */
typedef cc_result_t (*cc_runtime_policy_create_fn)(
    const cc_config_t *config,
    cc_policy_engine_t *out_policy
);

/**
 * @brief 创建由本次调用者保管的沙箱端口。
 * @param[in] config 调用期间借用配置，需保留的字段由工厂复制。
 * @param[out] out_sandbox 必填空槽，成功接收拥有沙箱，由接收者在使用者退出后销毁。
 * @return 创建结果及拥有诊断，可返回 UNSUPPORTED；失败由工厂清理未交接资源。
 * @note builder 可直接创建共享沙箱，也把此回调借给工具工厂，具体调用者承担成功资源的销毁。
 */
typedef cc_result_t (*cc_runtime_sandbox_create_fn)(
    const cc_config_t *config,
    cc_sandbox_t *out_sandbox
);

/**
 * @brief builder 在栈上提供的同步工具工厂上下文。
 * @note ctx 地址及 out_capabilities 槽只覆盖 create 调用，长期字段由工厂复制。
 * 工具可以按端口契约保存 filesystem 的按值借用及 HTTP/记忆借用，builder 保持这些底层资源到工具销毁。
 */
typedef struct cc_runtime_tool_factory_ctx {

    const cc_config_t *config; /**< 同步配置借用，工具长期需要的字段须复制。 */

    cc_filesystem_t filesystem; /**< 按值借用文件系统端口，工具不接管底层 self。 */

    cc_http_client_t *http_client; /**< 可选 HTTP 端口借用，需客户端的工厂自行校验能力。 */

    cc_memory_store_t *memory_store; /**< 可选记忆端口借用，NULL 表示当前未装配。 */

    cc_runtime_sandbox_create_fn create_sandbox; /**< 可选沙箱创建回调，成功资源由实际接收者销毁。 */

    /* 本次调用可选能力输出，builder 初值为零；只有受信任工厂可声明，借用止于 create 返回。 */
    unsigned *out_capabilities;
} cc_runtime_tool_factory_ctx_t;

/**
 * @brief 从同步上下文创建供 registry 接管的独占工具。
 * @param[in] ctx 调用期间借用的上下文，builder 提供有效地址，具体依赖可为空。
 * @param[out] out_tool 必填空槽，成功移交 self/vtable；成功注册后 registry 拥有工具。
 * @return 工厂结果及拥有诊断；失败须清理未发布工具并保持空输出。
 * @note builder 对成功但空端口的结果跳过注册，self 非空而 vtable 为空会报告工厂契约错误。
 * 成功工具注册失败时由 builder 清理，端口依赖继续借用至工具销毁。
 */
typedef cc_result_t (*cc_runtime_tool_create_fn)(
    const cc_runtime_tool_factory_ctx_t *ctx,
    cc_tool_t *out_tool
);

/**
 * @brief 同步加载插件并把成功工具注册到未冻结 registry。
 * @param[in] config 调用期间借用配置，运行态需保留内容由 loader 复制。
 * @param[in,out] registry 借用注册表，注册成功后接管工具，工具可借用 loader 状态。
 * @param[out] out_state 必填空槽，可交付完整或失败后的部分拥有状态，后续交 destroy_plugins。
 * @param[in,out] diagnostics 可选独占诊断集合，loader 可记录组件问题。
 * @return 结果与拥有诊断，错误时 builder 先销毁已注册工具，再调用对应状态析构。
 * @note 非空状态须配对可用析构；编解码插件名称应复用 cc_tool_name_build，保持来源组件可逆与 lane 一致。
 */
typedef cc_result_t (*cc_runtime_plugin_load_fn)(
    const cc_config_t *config,
    cc_tool_registry_t *registry,
    void **out_state,
    cc_runtime_diagnostics_t *diagnostics
);

/**
 * @brief 消费插件 loader 交付的完整或部分状态。
 * @param[in,out] state 拥有状态，允许 NULL，调用前借用该状态的工具已销毁、调用已退出。
 * @post 状态及其拥有资源释放，原借用失效；builder 在各代清理中可用 NULL 调用。
 */
typedef void (*cc_runtime_plugin_destroy_fn)(void *state);

/**
 * @brief 同步装配 MCP server 并向未冻结 registry 注册适配工具。
 * @param[in] config 调用期间借用配置，运行态内容由 loader 复制。
 * @param[in,out] registry 借用注册表，接管成功注册工具，工具可借用 MCP 状态。
 * @param[out] out_state 必填空槽，可交付完整或部分拥有状态，后续交 destroy_mcp。
 * @param[in,out] diagnostics 可选独占诊断集合，供追加 server 失败原因。
 * @return 装配结果与拥有诊断；错误时 builder 先销毁已注册工具，再析构交付状态。
 * @note 非空状态须有配对析构，成功后的并发调用遵循各传输与工具端口契约。
 */
typedef cc_result_t (*cc_runtime_mcp_load_fn)(
    const cc_config_t *config,
    cc_tool_registry_t *registry,
    void **out_state,
    cc_runtime_diagnostics_t *diagnostics
);

/**
 * @brief 消费 MCP loader 交付的完整或部分连接状态。
 * @param[in,out] state 拥有状态，允许 NULL，工具已销毁且所有在途请求已退出。
 * @post 状态与拥有连接资源释放，原借用结束。
 */
typedef void (*cc_runtime_mcp_destroy_fn)(void *state);

/** @brief 只读 provider 描述符，名称、回调和数组均由 feature owner 保活。 */
typedef struct cc_llm_provider_descriptor {

    const char *name; /**< 配置使用的稳定名称，builder 跳过 NULL 名称。 */

    int compiled; /**< 非零表示当前构建提供此能力，零时 builder 跳过。 */

    cc_runtime_llm_create_fn create; /**< 可选创建回调，NULL 时 builder 跳过。 */
} cc_llm_provider_descriptor_t;

/** @brief 只读工具描述符，供配置启用筛选及同步工厂装配。 */
typedef struct cc_tool_descriptor {

    const char *name; /**< 配置使用的稳定工具名，存活期覆盖 builder 和全部 generation。 */

    const char *alias; /**< 可选配置简写名称，NULL 表示无别名。 */

    int compiled; /**< 非零表示工厂实现已进入本次构建。 */

    cc_runtime_tool_create_fn create; /**< 可选创建回调，compiled 为零或回调为空时跳过。 */
} cc_tool_descriptor_t;

/**
 * @brief 由外层拥有、builder 长期借用的端口与扩展工厂集合。
 * @note builder 保存原指针，集合、数组、名称和回调须覆盖全部代创建、退役回收及最终销毁。
 * builder 销毁失败保留资源供重试时借用仍持续，owner 先关闭入口、排空使用者再完成销毁。
 * 构建和重载期间配置需保持稳定，集合本身在被借用期间保持只读。
 */
typedef struct cc_runtime_feature_set {

    const cc_llm_provider_descriptor_t *llm_providers; /**< provider 描述符借用，非零数量时须有有效数组。 */

    size_t llm_provider_count; /**< provider 数量，非零缺数组在创建准入时返回参数错误。 */


    const cc_tool_descriptor_t *tools; /**< 工具描述符借用，非零数量时须有有效数组。 */

    size_t tool_count; /**< 工具数量，零表示本集合没有内置工具。 */


    cc_runtime_session_store_create_fn create_session_store; /**< builder 创建必需的共享会话工厂。 */

    cc_runtime_memory_store_create_fn create_memory_store; /**< 可选记忆工厂，创建错误转为诊断并保持无记忆。 */

    cc_runtime_policy_create_fn create_policy_engine; /**< builder 创建必需的共享策略工厂。 */

    cc_runtime_sandbox_create_fn create_sandbox; /**< 可选沙箱工厂，同时提供给工具工厂上下文。 */


    cc_runtime_plugin_load_fn load_plugins; /**< 可选插件装配回调，按 generation 调用。 */

    cc_runtime_plugin_destroy_fn destroy_plugins; /**< 可选状态析构，非空插件状态须配对。 */

    cc_runtime_mcp_load_fn load_mcp; /**< 可选 MCP 装配回调，按 generation 调用。 */

    cc_runtime_mcp_destroy_fn destroy_mcp; /**< 可选 MCP 状态析构，非空状态须配对。 */
} cc_runtime_feature_set_t;

#endif
