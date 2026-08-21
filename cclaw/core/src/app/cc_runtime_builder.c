

#include "cc/app/cc_runtime_builder.h"
#include "cc/app/cc_skill_catalog.h"
#include "cc/app/cc_tool_executor_pool.h"
#include "cc_agent_runtime_internal.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_platform.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifndef CC_ENABLE_ASYNC_OBSERVABILITY
#define CC_ENABLE_ASYNC_OBSERVABILITY 0
#endif

struct cc_runtime_generation {
    struct cc_runtime_builder *owner;
    cc_agent_runtime_t *runtime;
    cc_llm_provider_t llm;
    cc_tool_registry_t *tool_registry;
    cc_tool_executor_pool_t *tool_pool;
    cc_skill_catalog_t *skill_catalog;
    char *system_prompt;
    void *plugin_state;
    void *mcp_state;
    unsigned long id;
    size_t refs;
    int retired;
    struct cc_runtime_generation *next;
};

/*
 * Runtime builder 是应用层装配器。
 *
 * 它拥有 logger/event_bus/provider/store/tool registry 等长期对象，真正的
 * cc_agent_runtime_t 只借用这些端口视图。这个分层是 C 语言里的依赖注入模式：
 * core runtime 不知道具体实现，只依赖 vtable；builder 负责选择 feature set 中的
 * adapter/factory，并在销毁时按反向顺序释放资源。
 *
 * 热重载后的旧 generation 进入 retired_generations 链；最后一个 run 释放引用时立即
 * 从链中摘除并销毁，不把历史资源拖到 builder destroy。
 */
struct cc_runtime_builder {

    const cc_runtime_feature_set_t *features;

    cc_logger_t *logger;

    cc_event_bus_t *event_bus;

    cc_filesystem_t fs;

    cc_http_client_t http_client;

    cc_session_store_t store;

    cc_llm_provider_t llm;

    cc_policy_engine_t policy;

    cc_memory_store_t memory_store;

    cc_sandbox_t sandbox;

    cc_tool_registry_t *tool_registry;

    cc_tool_executor_pool_t *tool_pool;

    cc_run_queue_t *run_queue;

    cc_agent_manager_t *agent_manager;

    cc_skill_catalog_t *skill_catalog;

    cc_agent_runtime_t *runtime;

    char *system_prompt;

    void *plugin_state;

    void *mcp_state;

    cc_runtime_diagnostics_t diagnostics;
    unsigned long reload_generation;
    cc_mutex_t generation_mutex;
    cc_cond_t generation_cond;
    cc_runtime_generation_t *current_generation;
    cc_runtime_generation_t *retired_generations;
    int shutting_down;
    int reload_in_progress;
    size_t reclaims_inflight;
};

static void runtime_generation_destroy(cc_runtime_generation_t *generation)
{
    if (!generation) return;
    cc_runtime_builder_t *builder = generation->owner;
    cc_agent_runtime_destroy(generation->runtime);
    if (generation->llm.vtable && generation->llm.vtable->destroy) {
        generation->llm.vtable->destroy(generation->llm.self);
    }
#if CC_ENABLE_TOOL_POOL
    cc_tool_executor_pool_destroy(generation->tool_pool);
#endif
#if CC_ENABLE_SKILLS
    cc_skill_catalog_destroy(generation->skill_catalog);
#endif
    cc_tool_registry_destroy(generation->tool_registry);
    if (builder && builder->features && builder->features->destroy_plugins) {
        builder->features->destroy_plugins(generation->plugin_state);
    }
    if (builder && builder->features && builder->features->destroy_mcp) {
        builder->features->destroy_mcp(generation->mcp_state);
    }
    free(generation->system_prompt);
    free(generation);
}

cc_result_t cc_runtime_builder_acquire_generation(
    cc_runtime_builder_t *builder,
    cc_runtime_generation_t **out_generation)
{
    if (!builder || !out_generation) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid runtime generation acquire request");
    }
    *out_generation = NULL;
    cc_mutex_lock(builder->generation_mutex);
    cc_runtime_generation_t *generation = builder->current_generation;
    if (builder->shutting_down || !generation) {
        cc_mutex_unlock(builder->generation_mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Runtime builder is draining");
    }
    generation->refs++;
    *out_generation = generation;
    cc_mutex_unlock(builder->generation_mutex);
    return cc_result_ok();
}

void cc_runtime_generation_release(cc_runtime_generation_t *generation)
{
    if (!generation || !generation->owner) return;
    cc_runtime_builder_t *builder = generation->owner;
    int destroy = 0;
    cc_mutex_lock(builder->generation_mutex);
    if (generation->refs > 0) generation->refs--;
    if (generation->refs == 0) {
        cc_cond_broadcast(builder->generation_cond);
        if (generation->retired) {
            cc_runtime_generation_t **link = &builder->retired_generations;
            while (*link && *link != generation) link = &(*link)->next;
            if (*link == generation) *link = generation->next;
            generation->next = NULL;
            builder->reclaims_inflight++;
            destroy = 1;
        }
    }
    cc_mutex_unlock(builder->generation_mutex);
    if (destroy) {
        runtime_generation_destroy(generation);
        cc_mutex_lock(builder->generation_mutex);
        if (builder->reclaims_inflight > 0) builder->reclaims_inflight--;
        cc_cond_broadcast(builder->generation_cond);
        cc_mutex_unlock(builder->generation_mutex);
    }
}

cc_agent_runtime_t *cc_runtime_generation_runtime(cc_runtime_generation_t *generation)
{
    return generation ? generation->runtime : NULL;
}

unsigned long cc_runtime_generation_id(const cc_runtime_generation_t *generation)
{
    return generation ? generation->id : 0;
}

#if CC_ENABLE_MULTI_AGENT && CC_ENABLE_RUN_QUEUE
static cc_result_t builder_agent_runtime_acquire(
    void *context,
    cc_agent_runtime_t **out_runtime,
    void **out_lease)
{
    if (!out_runtime || !out_lease) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid runtime lease output");
    }
    *out_runtime = NULL;
    *out_lease = NULL;
    cc_runtime_generation_t *generation = NULL;
    cc_result_t rc = cc_runtime_builder_acquire_generation(
        (cc_runtime_builder_t *)context, &generation);
    if (rc.code != CC_OK) return rc;
    *out_runtime = cc_runtime_generation_runtime(generation);
    *out_lease = generation;
    return cc_result_ok();
}

static void builder_agent_runtime_release(void *context, void *lease)
{
    (void)context;
    cc_runtime_generation_release((cc_runtime_generation_t *)lease);
}
#endif

/* 初始化 reload report，generation 默认保持不变，后续成功或失败路径再填具体状态。 */
static void reload_report_init(
    cc_runtime_reload_report_t *report,
    unsigned long generation
)
{
    if (!report) return;
    memset(report, 0, sizeof(*report));
    report->old_generation = generation;
    report->new_generation = generation;
    cc_runtime_diagnostics_reset(&report->diagnostics);
}

/*
 * 记录 reload 失败的组件名和错误信息。
 *
 * 这里不接管 rc 的所有权，只复制可读 message 到固定缓冲，方便上层在 rc 释放后仍能
 * 查看失败阶段。固定缓冲也避免嵌入式环境里 reload report 额外分配内存。
 */
static void reload_report_fail(
    cc_runtime_reload_report_t *report,
    const char *component,
    const cc_result_t *rc
)
{
    if (!report) return;
    report->rolled_back = 1;
    snprintf(report->failed_component, sizeof(report->failed_component),
        "%s", component ? component : "unknown");
    snprintf(report->message, sizeof(report->message),
        "%s", (rc && rc->message) ? rc->message : "Runtime reload failed");
}

/*
 * 判断配置是否启用某个内置工具。
 *
 * enabled_tools 为空表示“默认全开”；同时支持工具正式 name 和兼容 alias，便于配置文件
 * 使用更短的命名。这个 helper 不拥有任何字符串，只做只读匹配。
 */
static int config_tool_enabled(const cc_config_t *config, const char *name, const char *alias)
{
    if (!config || !config->enabled_tools || config->enabled_tools_count == 0) return 1;
    for (size_t i = 0; i < config->enabled_tools_count; i++) {
        const char *enabled = config->enabled_tools[i];
        if (!enabled) continue;
        if (name && strcmp(enabled, name) == 0) return 1;
        if (alias && strcmp(enabled, alias) == 0) return 1;
    }
    return 0;
}

/*
 * 销毁尚未交给 registry 持有的 tool。
 *
 * 工具创建后如果注册失败，registry 不会接管 self；此时必须调用 tool vtable destroy。
 * 注册成功后 register_created_tool 会清空结构，避免重复销毁。
 */
static void destroy_tool_if_unowned(cc_tool_t *tool)
{
    if (tool && tool->vtable && tool->vtable->destroy && tool->self) {
        tool->vtable->destroy(tool->self);
    }
    if (tool) memset(tool, 0, sizeof(*tool));
}

/*
 * 将 factory 创建出的工具加入 registry。
 *
 * 成功后 registry 深拷贝/接管 tool 端口语义，当前栈上 tool 被清零；失败时销毁未转交的
 * tool self，保证错误路径不泄漏 adapter 私有状态。
 */
static cc_result_t register_created_tool(cc_tool_registry_t *registry, cc_tool_t *tool)
{
    cc_result_t rc = cc_tool_registry_add(registry, *tool);
    if (rc.code != CC_OK) {
        destroy_tool_if_unowned(tool);
        return rc;
    }
    memset(tool, 0, sizeof(*tool));
    return cc_result_ok();
}

/*
 * 销毁 builder 持有的 sandbox 端口。
 *
 * sandbox 是可选能力，只有 self/vtable/destroy 同时存在才调用；随后清零，避免 destroy
 * 阶段因为多个失败路径重复释放。
 */
static void destroy_sandbox_if_owned(cc_sandbox_t *sandbox)
{
    if (sandbox && sandbox->vtable && sandbox->vtable->destroy && sandbox->self) {
        sandbox->vtable->destroy(sandbox->self);
    }
    if (sandbox) memset(sandbox, 0, sizeof(*sandbox));
}

/*
 * 销毁工具构建过程中产生的临时状态，释放资源。
 *
 * 参数:
 *   builder      - 运行时构建器
 *   registry     - 工具注册表（可能为 NULL）
 *   plugin_state - 插件状态（可能为 NULL）
 *   mcp_state    - MCP 状态（可能为 NULL）
 *
 * 安全释放各个组件，对 NULL 参数无操作。
 */
static void destroy_tool_build_state(
    cc_runtime_builder_t *builder,
    cc_tool_registry_t *registry,
    void *plugin_state,
    void *mcp_state
)
{
    cc_tool_registry_destroy(registry);
    if (builder && builder->features && builder->features->destroy_plugins) {
        builder->features->destroy_plugins(plugin_state);
    }
    if (builder && builder->features && builder->features->destroy_mcp) {
        builder->features->destroy_mcp(mcp_state);
    }
}

/*
 * 根据配置 provider 名称从 feature set 创建 LLM provider。
 *
 * 这是典型工厂模式：核心只认识 cc_llm_provider_t 接口，具体 OpenAI/Ollama/Anthropic
 * 等实现由 feature descriptor 决定。未知或编译关闭的 provider 直接返回配置错误。
 */
static cc_result_t create_llm(
    const cc_config_t *config,
    const cc_runtime_feature_set_t *features,
    cc_http_client_t *http_client,
    cc_llm_provider_t *out_llm
)
{
    memset(out_llm, 0, sizeof(*out_llm));
    const char *provider = config->provider ? config->provider : "";
    for (size_t i = 0; i < features->llm_provider_count; i++) {
        const cc_llm_provider_descriptor_t *desc = &features->llm_providers[i];
        if (!desc->compiled || !desc->name || !desc->create) continue;
        if (strcmp(provider, desc->name) == 0) {
            return desc->create(config, http_client, out_llm);
        }
    }
    return cc_result_errf(CC_ERR_INVALID_ARGUMENT, "Unknown or disabled LLM provider: %s",
        provider[0] ? provider : "(none)");
}

/*
 * 为当前配置构建一个冻结的工具注册表。
 *
 * 注册顺序是内置工具 -> plugin 工具 -> MCP 工具。plugin_state/mcp_state 由对应 loader
 * 分配，成功后交给 builder 持有；任一阶段失败都销毁已创建资源。冻结 registry 后，
 * 运行中的 agent 只能查询，不能再修改，这让多线程读工具 schema 更容易推理。
 */
static cc_result_t build_tool_registry_for_config(
    cc_runtime_builder_t *builder,
    const cc_config_t *config,
    cc_tool_registry_t **out_registry,
    void **out_plugin_state,
    void **out_mcp_state,
    cc_runtime_diagnostics_t *diagnostics
)
{
    if (out_registry) *out_registry = NULL;
    if (out_plugin_state) *out_plugin_state = NULL;
    if (out_mcp_state) *out_mcp_state = NULL;

    cc_tool_registry_t *registry = NULL;
    void *plugin_state = NULL;
    void *mcp_state = NULL;
    cc_result_t rc = cc_tool_registry_create(&registry);
    if (rc.code != CC_OK) return rc;

    cc_runtime_tool_factory_ctx_t ctx = {
        .config = config,
        .filesystem = builder->fs,
        .http_client = &builder->http_client,
        .memory_store = builder->memory_store.self ? &builder->memory_store : NULL,
        .create_sandbox = builder->features->create_sandbox
    };

    for (size_t i = 0; i < builder->features->tool_count; i++) {
        const cc_tool_descriptor_t *desc = &builder->features->tools[i];
        if (!desc->compiled || !desc->create) continue;
        if (!config_tool_enabled(config, desc->name, desc->alias)) continue;

        cc_tool_t tool = {0};
        rc = desc->create(&ctx, &tool);
        if (rc.code != CC_OK) {
            destroy_tool_build_state(builder, registry, plugin_state, mcp_state);
            return rc;
        }
        if (!tool.vtable) {
            if (tool.self) {
                destroy_tool_build_state(builder, registry, plugin_state, mcp_state);
                return cc_result_errf(
                    CC_ERR_INVALID_ARGUMENT,
                    "Tool factory returned self without vtable: %s",
                    desc->name ? desc->name : "(unknown)");
            }
            continue;
        }
        rc = register_created_tool(registry, &tool);
        if (rc.code != CC_OK) {
            destroy_tool_build_state(builder, registry, plugin_state, mcp_state);
            return rc;
        }
    }

    if (builder->features->load_plugins) {
        rc = builder->features->load_plugins(
            config, registry, &plugin_state, diagnostics);
        if (rc.code != CC_OK) {
            destroy_tool_build_state(builder, registry, plugin_state, mcp_state);
            return rc;
        }
    }

    if (builder->features->load_mcp) {
        rc = builder->features->load_mcp(
            config, registry, &mcp_state, diagnostics);
        if (rc.code != CC_OK) {
            destroy_tool_build_state(builder, registry, plugin_state, mcp_state);
            return rc;
        }
    }

    rc = cc_tool_registry_freeze(registry);
    if (rc.code != CC_OK) {
        destroy_tool_build_state(builder, registry, plugin_state, mcp_state);
        return rc;
    }

    *out_registry = registry;
    if (out_plugin_state) *out_plugin_state = plugin_state;
    if (out_mcp_state) *out_mcp_state = mcp_state;
    return cc_result_ok();
}

/* 初次启动时构建 builder 当前 generation 的工具 registry 和扩展状态。 */
static cc_result_t build_tools(cc_runtime_builder_t *builder, const cc_config_t *config)
{
    return build_tool_registry_for_config(
        builder,
        config,
        &builder->tool_registry,
        &builder->plugin_state,
        &builder->mcp_state,
        &builder->diagnostics
    );
}

/*
 * 按配置创建工具执行池。
 *
 * tool pool 把不同来源的工具映射到 lane：普通工具使用 tools.policies，plugin/MCP 根据
 * entry/server 生成专属 lane。这样高延迟外部工具不会占满核心执行通道，是嵌入式/边缘
 * 设备里控制并发和超时的关键设计点。
 */
static cc_result_t create_tool_pool_from_config(
    const cc_config_t *config,
    cc_tool_executor_pool_t **out_pool
)
{
    if (!out_pool) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool pool output");
    *out_pool = NULL;
#if CC_ENABLE_TOOL_POOL
    cc_tool_executor_pool_config_t pool_config = cc_tool_executor_pool_default_config();
    pool_config.default_timeout_ms = config->tools.default_timeout_ms > 0 ?
        config->tools.default_timeout_ms : pool_config.default_timeout_ms;

    size_t policy_count = config->tools.policy_count +
        config->plugins.entry_count + config->mcp.server_count;
    cc_tool_executor_pool_policy_t *policies = NULL;
    if (policy_count > 0) {
        policies = calloc(policy_count, sizeof(*policies));
        if (!policies) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate tool pool policies");
    }

    size_t written = 0;
    for (size_t i = 0; i < config->tools.policy_count; i++) {
        policies[written].name = config->tools.policies[i].name;
        policies[written].concurrency = config->tools.policies[i].concurrency;
        policies[written].timeout_ms = config->tools.policies[i].timeout_ms;
        written++;
    }
    for (size_t i = 0; i < config->plugins.entry_count; i++) {
        const cc_config_plugin_entry_t *entry = &config->plugins.entries[i];
        if (!entry->id || !entry->enabled) continue;
        char lane[256];
        snprintf(lane, sizeof(lane), "plugin.%s", entry->id);
        policies[written].name = cc_copy_string(lane);
        policies[written].concurrency = entry->max_in_flight > 0 ?
            entry->max_in_flight : (entry->workers > 0 ? entry->workers : config->queue.plugin_concurrency);
        policies[written].timeout_ms = entry->timeout_ms;
        written++;
    }
    for (size_t i = 0; i < config->mcp.server_count; i++) {
        const cc_config_mcp_server_t *server = &config->mcp.servers[i];
        if (!server->name) continue;
        char lane[256];
        snprintf(lane, sizeof(lane), "mcp.%s", server->name);
        policies[written].name = cc_copy_string(lane);
        policies[written].concurrency = config->queue.mcp_concurrency;
        policies[written].timeout_ms = server->connection_timeout_ms;
        written++;
    }
    pool_config.policies = policies;
    pool_config.policy_count = written;

    cc_result_t rc = cc_tool_executor_pool_create(&pool_config, out_pool);
    for (size_t i = config->tools.policy_count; i < written; i++) {
        free((char *)policies[i].name);
    }
    free(policies);
    return rc;
#else
    (void)config;
    (void)out_pool;
    return cc_result_ok();
#endif
}

/* 初次启动时创建 builder 持有的工具执行池；编译关闭 tool pool 时为 no-op。 */
static cc_result_t build_tool_pool(cc_runtime_builder_t *builder, const cc_config_t *config)
{
    return create_tool_pool_from_config(config, &builder->tool_pool);
}

/*
 * 创建 run queue。
 *
 * run queue 只在 multi-agent 和 run-queue profile 同时启用时存在；小型 MCU profile
 * 可以完全裁剪掉这层，runtime 仍保持同步执行能力。
 */
static cc_result_t build_run_queue(cc_runtime_builder_t *builder, const cc_config_t *config)
{
#if CC_ENABLE_MULTI_AGENT && CC_ENABLE_RUN_QUEUE
    cc_run_queue_config_t queue_config = cc_run_queue_default_config();
    queue_config.main_concurrency = config->queue.main_concurrency;
    queue_config.subagent_concurrency = config->queue.subagent_concurrency;
    queue_config.plugin_concurrency = config->queue.plugin_concurrency;
    queue_config.mcp_concurrency = config->queue.mcp_concurrency;
    queue_config.per_session_concurrency = config->queue.per_session_concurrency;
    queue_config.max_pending_per_session = config->queue.max_pending_per_session;
    queue_config.max_total_tasks = config->queue.max_total_tasks;
    queue_config.completed_ttl_ms = config->queue.completed_ttl_ms;
    return cc_run_queue_create(&queue_config, &builder->run_queue);
#else
    (void)builder;
    (void)config;
    return cc_result_ok();
#endif
}

/*
 * 将配置字符串映射为 run queue 行为枚举。
 *
 * 参数:
 *   mode - 行为模式字符串 ("steer"|"followup"|"parallel")
 * 返回: 对应的 cc_run_queue_action_t 枚举值，未知值默认返回 CC_RUN_QUEUE_ACTION_STEER。
 */
#if CC_ENABLE_MULTI_AGENT && CC_ENABLE_RUN_QUEUE
/* 将配置字符串映射成 run queue 行为枚举，未知值按 steer 处理保证默认可用。 */
static cc_run_queue_action_t queue_action_from_config(const char *mode)
{
    if (!mode || strcmp(mode, "steer") == 0) return CC_RUN_QUEUE_ACTION_STEER;
    if (strcmp(mode, "followup") == 0) return CC_RUN_QUEUE_ACTION_FOLLOWUP;
    if (strcmp(mode, "collect") == 0) return CC_RUN_QUEUE_ACTION_COLLECT;
    if (strcmp(mode, "interrupt") == 0) return CC_RUN_QUEUE_ACTION_INTERRUPT;
    return CC_RUN_QUEUE_ACTION_STEER;
}
#endif

/*
 * 构建 system prompt 和 skill catalog 快照。
 *
 * system_prompt 是传给 runtime 的配置快照，builder 和 runtime 各自持有自己的指针；
 * skills 启用时会把 allowlist 内技能拼接进 prompt。失败路径释放 catalog/prompt，
 * 避免 reload 中构建失败污染旧 generation。
 */
static cc_result_t build_system_prompt_snapshot(
    cc_runtime_builder_t *builder,
    const cc_config_t *config,
    char **out_system_prompt,
    cc_skill_catalog_t **out_skill_catalog
)
{
    if (!out_system_prompt || !out_skill_catalog) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid system prompt snapshot output");
    }
    *out_system_prompt = NULL;
    *out_skill_catalog = NULL;

    char *system_prompt = NULL;
    cc_result_t rc = cc_config_build_system_prompt(config, &system_prompt);
    if (rc.code != CC_OK || !system_prompt) {
        cc_result_free(&rc);
        system_prompt = cc_copy_string("You are a helpful AI assistant. Use tools to help the user.");
        if (!system_prompt) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create default system prompt");
        }
    }

#if CC_ENABLE_SKILLS
    cc_skill_catalog_t *catalog = NULL;
    rc = cc_skill_catalog_create(&catalog);
    if (rc.code != CC_OK) {
        free(system_prompt);
        return rc;
    }
    rc = cc_skill_catalog_load_from_config(catalog, &builder->fs, config);
    if (rc.code != CC_OK) {
        free(system_prompt);
        cc_skill_catalog_destroy(catalog);
        return rc;
    }

    const cc_config_string_list_t *allowlist = &config->agents.defaults.skills;
    char *skill_prompt = NULL;
    rc = cc_skill_catalog_build_prompt(catalog, allowlist, &skill_prompt);
    if (rc.code != CC_OK) {
        free(system_prompt);
        cc_skill_catalog_destroy(catalog);
        return rc;
    }
    if (skill_prompt && skill_prompt[0]) {
        cc_string_builder_t sb;
        cc_string_builder_init(&sb);
        cc_string_builder_append(&sb, system_prompt);
        cc_string_builder_append(&sb, skill_prompt);
        char *joined = cc_string_builder_take(&sb);
        if (!joined) {
            free(skill_prompt);
            free(system_prompt);
            cc_skill_catalog_destroy(catalog);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to append skills to system prompt");
        }
        free(system_prompt);
        system_prompt = joined;
    }
    free(skill_prompt);
    *out_skill_catalog = catalog;
#endif

    *out_system_prompt = system_prompt;
    return cc_result_ok();
}

/*
 * 创建完整 runtime builder。
 *
 * 该入口把配置、feature set 和端口工厂装配成一个可运行 SDK 实例。成功后 out_builder
 * 拥有返回对象；失败路径跳到统一 destroy，释放已创建组件。面试里可以把它解释为
 * “组合根”：所有依赖都在这里创建，业务核心通过接口使用它们。
 */
cc_result_t cc_runtime_builder_create(
    const cc_config_t *config,
    const cc_runtime_feature_set_t *features,
    cc_runtime_builder_t **out_builder
)
{
    if (!config || !features || !out_builder) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null runtime builder argument");
    }
    if (!features->create_session_store || !features->create_policy_engine) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Runtime feature set is incomplete");
    }

    cc_platform_init();

    *out_builder = NULL;
    cc_runtime_builder_t *builder = calloc(1, sizeof(*builder));
    if (!builder) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create runtime builder");
    builder->features = features;
    builder->reload_generation = 1;
    cc_runtime_diagnostics_reset(&builder->diagnostics);

    cc_result_t rc = cc_mutex_create(&builder->generation_mutex);
    if (rc.code != CC_OK) {
        free(builder);
        return rc;
    }
    rc = cc_cond_create(&builder->generation_cond);
    if (rc.code != CC_OK) {
        cc_mutex_destroy(builder->generation_mutex);
        free(builder);
        return rc;
    }

    rc = cc_logger_create("c-claw", CC_LOG_INFO, &builder->logger);
    if (rc.code != CC_OK) goto fail;
    cc_logger_log(builder->logger, CC_LOG_INFO, "c-claw starting...");

    cc_event_bus_config_t event_bus_config = cc_event_bus_default_config();
#if CC_ENABLE_ASYNC_OBSERVABILITY
    event_bus_config.mode = CC_EVENT_BUS_MODE_ASYNC;
#endif
    rc = cc_event_bus_create_with_config(&event_bus_config, &builder->event_bus);
    if (rc.code != CC_OK) goto fail;
    rc = cc_filesystem_get_default(&builder->fs);
    if (rc.code != CC_OK) goto fail;
    cc_http_client_options_t http_options = {
        .size = sizeof(http_options),
        .tls_verify = config->tls_verify,
        .connect_timeout_ms = config->http_connect_timeout_ms,
        .first_byte_timeout_ms = config->http_first_byte_timeout_ms,
        .idle_timeout_ms = config->http_stream_idle_timeout_ms,
        .retry_count = config->http_retry_count,
        .log_level = config->http_log_level,
        .trace_persist = config->http_trace_persist,
    };
    rc = cc_http_client_create_default(&http_options, &builder->http_client);
    if (rc.code != CC_OK) goto fail;
    if (config->data_dir && config->data_dir[0]) {
        rc = builder->fs.vtable->make_dir(builder->fs.self, config->data_dir);
        if (rc.code != CC_OK) goto fail;
    }
    if (config->workspace_path && config->workspace_path[0]) {
        rc = builder->fs.vtable->make_dir(builder->fs.self, config->workspace_path);
        if (rc.code != CC_OK) goto fail;
    }

    rc = features->create_session_store(config, &builder->store);
    if (rc.code != CC_OK) goto fail;
    rc = create_llm(config, features, &builder->http_client, &builder->llm);
    if (rc.code != CC_OK) goto fail;
    rc = features->create_policy_engine(config, &builder->policy);
    if (rc.code != CC_OK) goto fail;
    if (features->create_sandbox) {
        rc = features->create_sandbox(config, &builder->sandbox);
        if (rc.code != CC_OK) goto fail;
    }
    if (features->create_memory_store) {
        rc = features->create_memory_store(config, &builder->memory_store);
        if (rc.code != CC_OK) {
            cc_result_free(&rc);
            memset(&builder->memory_store, 0, sizeof(builder->memory_store));
        }
    }

    rc = build_tools(builder, config);
    if (rc.code != CC_OK) goto fail;
    rc = build_tool_pool(builder, config);
    if (rc.code != CC_OK) goto fail;
    rc = build_system_prompt_snapshot(
        builder, config, &builder->system_prompt, &builder->skill_catalog);
    if (rc.code != CC_OK) goto fail;

    cc_agent_runtime_config_t runtime_config = {0};
    runtime_config.max_steps = config->max_steps ? config->max_steps : 4;
    runtime_config.context_window_tokens = config->context_window_tokens;
    runtime_config.context_compress_threshold = config->context_compress_threshold;
    runtime_config.context_keep_recent = config->context_keep_recent;
    runtime_config.max_tokens = config->max_tokens;
    runtime_config.temperature = config->temperature;
    runtime_config.summary_max_tokens = config->summary_max_tokens;
    runtime_config.summary_temperature = config->summary_temperature;
    runtime_config.multimodal = config->multimodal;
    runtime_config.active_memory_enabled = config->active_memory_enabled;
    runtime_config.active_memory_write_summary = config->active_memory_write_summary;
    runtime_config.active_memory_max_value_chars = config->active_memory_max_value_chars;
    runtime_config.active_memory_category = config->active_memory_category;
    runtime_config.system_prompt = builder->system_prompt;
    runtime_config.workspace_dir = config->workspace_path;
    runtime_config.model = config->model;
    runtime_config.model_extra_body_json = config->model_extra_body_json;
    runtime_config.limits.size = sizeof(runtime_config.limits);
    runtime_config.limits.run_timeout_ms = config->run_timeout_ms > 0 ? config->run_timeout_ms : 180000;
    runtime_config.limits.provider_timeout_ms = config->provider_timeout_ms > 0 ? config->provider_timeout_ms : 120000;
    runtime_config.limits.tool_timeout_ms = config->tool_timeout_ms > 0 ? config->tool_timeout_ms : 10000;
    runtime_config.limits.max_input_bytes = config->max_input_bytes > 0 ? (size_t)config->max_input_bytes : 1024U;
    runtime_config.limits.max_output_bytes = config->max_output_bytes > 0 ? (size_t)config->max_output_bytes : 32U * 1024U;
    runtime_config.limits.max_tool_result_bytes = config->max_tool_result_bytes > 0 ? (size_t)config->max_tool_result_bytes : 16U * 1024U;
    runtime_config.limits.max_stream_bytes = config->max_stream_bytes > 0 ? (size_t)config->max_stream_bytes : 32U * 1024U;
    runtime_config.limits.max_steps = runtime_config.max_steps;
    runtime_config.limits.max_concurrency = config->max_concurrency > 0 ? config->max_concurrency : 1;

    cc_agent_runtime_deps_t deps = {0};
    deps.llm = builder->llm;
    deps.tool_registry = builder->tool_registry;
    deps.store = builder->store;
    deps.policy = builder->policy;
    deps.sandbox = builder->sandbox;
    deps.event_bus = builder->event_bus;
    deps.logger = builder->logger;
    deps.memory_store = builder->memory_store.self ? &builder->memory_store : NULL;
    deps.tool_pool = builder->tool_pool;

    cc_agent_runtime_options_t options = {0};
    options.config = runtime_config;
    options.thinking_mode = config->thinking_mode;
    rc = cc_agent_runtime_create(&deps, &options, &builder->runtime);
    if (rc.code != CC_OK) goto fail;

    rc = build_run_queue(builder, config);
    if (rc.code != CC_OK) goto fail;
#if CC_ENABLE_MULTI_AGENT && CC_ENABLE_RUN_QUEUE
    cc_agent_manager_options_t manager_options = {0};
    manager_options.queue = builder->run_queue;
    manager_options.owns_queue = 0;
    manager_options.default_action = queue_action_from_config(config->queue.mode);
    manager_options.default_agent_id =
        (config->agents.defaults.id && strcmp(config->agents.defaults.id, "defaults") != 0) ?
            config->agents.defaults.id : "default";
    rc = cc_agent_manager_create(&manager_options, &builder->agent_manager);
    if (rc.code != CC_OK) goto fail;
#endif

    cc_runtime_generation_t *initial_generation = calloc(1, sizeof(*initial_generation));
    if (!initial_generation) {
        rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate initial runtime generation");
        goto fail;
    }
    initial_generation->owner = builder;
    initial_generation->runtime = builder->runtime;
    initial_generation->llm = builder->llm;
    initial_generation->tool_registry = builder->tool_registry;
    initial_generation->tool_pool = builder->tool_pool;
    initial_generation->skill_catalog = builder->skill_catalog;
    initial_generation->system_prompt = builder->system_prompt;
    initial_generation->plugin_state = builder->plugin_state;
    initial_generation->mcp_state = builder->mcp_state;
    initial_generation->id = builder->reload_generation;
    builder->current_generation = initial_generation;

#if CC_ENABLE_MULTI_AGENT && CC_ENABLE_RUN_QUEUE
    const char *default_agent_id =
        (config->agents.defaults.id && strcmp(config->agents.defaults.id, "defaults") != 0) ?
            config->agents.defaults.id : "default";
    rc = cc_agent_manager_add_agent_lease(
        builder->agent_manager,
        default_agent_id,
        builder,
        builder_agent_runtime_acquire,
        builder_agent_runtime_release);
    if (rc.code != CC_OK) goto fail;
    rc = cc_agent_manager_set_current_agent(builder->agent_manager, default_agent_id);
    if (rc.code != CC_OK) goto fail;
#endif

    *out_builder = builder;
    return cc_result_ok();

fail:
    {
        cc_result_t destroy_rc = cc_runtime_builder_destroy(builder, 0);
        cc_result_free(&destroy_rc);
    }
    return rc;
}

/*
 * 返回 agent manager 的借用指针。
 *
 * 只有 multi-agent/run-queue profile 才会创建 manager；裁剪 profile 返回 NULL，调用方
 * 需要按能力判断降级到单 runtime 执行。
 */
cc_agent_manager_t *cc_runtime_builder_agent_manager(cc_runtime_builder_t *builder)
{
    return builder ? builder->agent_manager : NULL;
}

/*
 * 返回最近一次启动或 reload 的诊断信息借用指针。
 *
 * diagnostics 由 builder 持有，不需要释放；plugin/MCP loader 可把非致命问题写入这里，
 * 让应用展示“部分能力不可用”而不是直接启动失败。
 */
const cc_runtime_diagnostics_t *cc_runtime_builder_diagnostics(cc_runtime_builder_t *builder)
{
    return builder ? &builder->diagnostics : NULL;
}

/* 简化 reload 入口：不需要详细 report 时调用，内部仍走同一套事务式 reload 实现。 */
cc_result_t cc_runtime_builder_reload(
    cc_runtime_builder_t *builder,
    const cc_config_t *config
)
{
    return cc_runtime_builder_reload_with_report(builder, config, NULL);
}

/*
 * 事务式热重载工具、技能和执行池。
 *
 * 新资源全部创建成功后才切换到 runtime；任何阶段失败都会销毁新资源并保留旧 generation。
 * 切换时把旧资源 retire，保证正在执行的 run 不会读到已释放的 registry/prompt。这种
 * “先构建、后提交、失败回滚”的思路在嵌入式配置热更新中很常见。
 */
cc_result_t cc_runtime_builder_reload_with_report(
    cc_runtime_builder_t *builder,
    const cc_config_t *config,
    cc_runtime_reload_report_t *out_report
)
{
    if (!builder || !config) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid runtime reload request");
    }
    cc_mutex_lock(builder->generation_mutex);
    if (builder->shutting_down) {
        cc_mutex_unlock(builder->generation_mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Runtime builder is draining");
    }
    if (builder->reload_in_progress) {
        cc_mutex_unlock(builder->generation_mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Runtime reload already in progress");
    }
    builder->reload_in_progress = 1;
    cc_runtime_generation_t *old_generation = builder->current_generation;
    if (old_generation) old_generation->refs++;
    unsigned long old_id = old_generation ? old_generation->id : builder->reload_generation;
    cc_mutex_unlock(builder->generation_mutex);
    reload_report_init(out_report, old_id);

    cc_runtime_generation_t *new_generation = calloc(1, sizeof(*new_generation));
    if (!new_generation) {
        cc_result_t oom = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate runtime generation");
        reload_report_fail(out_report, "generation", &oom);
        if (old_generation) cc_runtime_generation_release(old_generation);
        cc_mutex_lock(builder->generation_mutex);
        builder->reload_in_progress = 0;
        cc_cond_broadcast(builder->generation_cond);
        cc_mutex_unlock(builder->generation_mutex);
        return oom;
    }
    new_generation->owner = builder;
    new_generation->id = old_id + 1;

    cc_runtime_diagnostics_t new_diagnostics;
    cc_runtime_diagnostics_reset(&new_diagnostics);
    cc_result_t rc = create_llm(
        config, builder->features, &builder->http_client, &new_generation->llm);
    if (rc.code != CC_OK) {
        reload_report_fail(out_report, "provider", &rc);
        goto reload_fail;
    }
    rc = build_tool_registry_for_config(
        builder, config, &new_generation->tool_registry,
        &new_generation->plugin_state, &new_generation->mcp_state,
        &new_diagnostics);
    if (rc.code != CC_OK) {
        reload_report_fail(out_report, "tools", &rc);
        goto reload_fail;
    }
    rc = create_tool_pool_from_config(config, &new_generation->tool_pool);
    if (rc.code != CC_OK) {
        reload_report_fail(out_report, "tool_pool", &rc);
        goto reload_fail;
    }
    rc = build_system_prompt_snapshot(
        builder, config, &new_generation->system_prompt,
        &new_generation->skill_catalog);
    if (rc.code != CC_OK) {
        reload_report_fail(out_report, "skills", &rc);
        goto reload_fail;
    }

    cc_agent_runtime_config_t runtime_config = old_generation && old_generation->runtime ?
        old_generation->runtime->config : (cc_agent_runtime_config_t){0};
    runtime_config.max_steps = config->max_steps ? config->max_steps : 4;
    runtime_config.context_window_tokens = config->context_window_tokens;
    runtime_config.context_compress_threshold = config->context_compress_threshold;
    runtime_config.context_keep_recent = config->context_keep_recent;
    runtime_config.max_tokens = config->max_tokens;
    runtime_config.temperature = config->temperature;
    runtime_config.summary_max_tokens = config->summary_max_tokens;
    runtime_config.summary_temperature = config->summary_temperature;
    runtime_config.multimodal = config->multimodal;
    runtime_config.active_memory_enabled = config->active_memory_enabled;
    runtime_config.active_memory_write_summary = config->active_memory_write_summary;
    runtime_config.active_memory_max_value_chars = config->active_memory_max_value_chars;
    runtime_config.active_memory_category = config->active_memory_category;
    runtime_config.system_prompt = new_generation->system_prompt;
    runtime_config.workspace_dir = config->workspace_path;
    runtime_config.model = config->model;
    runtime_config.model_extra_body_json = config->model_extra_body_json;
    runtime_config.limits.size = sizeof(runtime_config.limits);
    runtime_config.limits.run_timeout_ms = config->run_timeout_ms > 0 ? config->run_timeout_ms : 180000;
    runtime_config.limits.provider_timeout_ms = config->provider_timeout_ms > 0 ? config->provider_timeout_ms : 120000;
    runtime_config.limits.tool_timeout_ms = config->tool_timeout_ms > 0 ? config->tool_timeout_ms : 10000;
    runtime_config.limits.max_input_bytes = config->max_input_bytes > 0 ? (size_t)config->max_input_bytes : 1024U;
    runtime_config.limits.max_output_bytes = config->max_output_bytes > 0 ? (size_t)config->max_output_bytes : 32U * 1024U;
    runtime_config.limits.max_stream_bytes = config->max_stream_bytes > 0 ? (size_t)config->max_stream_bytes : 32U * 1024U;
    runtime_config.limits.max_tool_result_bytes = config->max_tool_result_bytes > 0 ? (size_t)config->max_tool_result_bytes : 16U * 1024U;
    runtime_config.limits.max_steps = runtime_config.max_steps;
    runtime_config.limits.max_concurrency = config->max_concurrency > 0 ? config->max_concurrency : 1;

    cc_agent_runtime_deps_t deps = {0};
    deps.llm = new_generation->llm;
    deps.tool_registry = new_generation->tool_registry;
    deps.store = builder->store;
    deps.policy = builder->policy;
    deps.sandbox = builder->sandbox;
    deps.event_bus = builder->event_bus;
    deps.logger = builder->logger;
    deps.memory_store = builder->memory_store.self ? &builder->memory_store : NULL;
    deps.tool_pool = new_generation->tool_pool;
    cc_agent_runtime_options_t options = {0};
    options.config = runtime_config;
    options.thinking_mode = config->thinking_mode;
    rc = cc_agent_runtime_create(&deps, &options, &new_generation->runtime);
    if (rc.code != CC_OK) {
        reload_report_fail(out_report, "runtime", &rc);
        goto reload_fail;
    }

    cc_mutex_lock(builder->generation_mutex);
    if (builder->shutting_down || builder->current_generation != old_generation) {
        cc_mutex_unlock(builder->generation_mutex);
        cc_result_free(&rc);
        rc = cc_result_error(CC_ERR_INVALID_STATE, "Runtime generation changed during reload");
        reload_report_fail(out_report, "commit", &rc);
        goto reload_fail;
    }
    builder->current_generation = new_generation;
    builder->runtime = new_generation->runtime;
    builder->llm = new_generation->llm;
    builder->tool_registry = new_generation->tool_registry;
    builder->tool_pool = new_generation->tool_pool;
    builder->skill_catalog = new_generation->skill_catalog;
    builder->system_prompt = new_generation->system_prompt;
    builder->plugin_state = new_generation->plugin_state;
    builder->mcp_state = new_generation->mcp_state;
    builder->diagnostics = new_diagnostics;
    builder->reload_generation = new_generation->id;
    old_generation->retired = 1;
    old_generation->next = builder->retired_generations;
    builder->retired_generations = old_generation;
    builder->reload_in_progress = 0;
    cc_cond_broadcast(builder->generation_cond);
    cc_mutex_unlock(builder->generation_mutex);

    if (out_report) {
        out_report->tools_reloaded = 1;
        out_report->plugins_reloaded = new_generation->plugin_state != NULL;
        out_report->mcp_reloaded = new_generation->mcp_state != NULL;
        out_report->skills_reloaded = new_generation->skill_catalog != NULL;
        out_report->tool_pool_reloaded = new_generation->tool_pool != NULL;
        out_report->old_generation = old_id;
        out_report->new_generation = new_generation->id;
        out_report->diagnostics = new_diagnostics;
    }
    cc_runtime_generation_release(old_generation);
    if (builder->logger) cc_logger_log(builder->logger, CC_LOG_INFO, "Runtime reload completed");
    return cc_result_ok();

reload_fail:
    runtime_generation_destroy(new_generation);
    if (old_generation) cc_runtime_generation_release(old_generation);
    cc_mutex_lock(builder->generation_mutex);
    builder->reload_in_progress = 0;
    cc_cond_broadcast(builder->generation_cond);
    cc_mutex_unlock(builder->generation_mutex);
    return rc;
}

/*
 * 请求关闭 runtime。
 *
 * 当前实现只记录日志；异步 run queue 的实际 worker 销毁发生在 destroy 中。保留这个
 * API 是为了未来接入更细的 stop/drain 流程时不改变上层调用点。
 */
void cc_runtime_builder_request_shutdown(cc_runtime_builder_t *builder)
{
    if (!builder) return;
    cc_mutex_lock(builder->generation_mutex);
    builder->shutting_down = 1;
    cc_cond_broadcast(builder->generation_cond);
    cc_mutex_unlock(builder->generation_mutex);
    if (builder->logger) {
        cc_logger_log(builder->logger, CC_LOG_INFO, "Runtime shutdown requested");
    }
}

/*
 * 返回 logger 的借用指针。
 *
 * 调用方可以临时写日志，但不能销毁；logger 的脱敏策略和线程安全由 logger 模块内部
 * 处理，builder destroy 时统一释放。
 */
cc_logger_t *cc_runtime_builder_logger(cc_runtime_builder_t *builder)
{
    return builder ? builder->logger : NULL;
}

cc_result_t cc_runtime_builder_reset_http_connections(cc_runtime_builder_t *builder)
{
    if (!builder) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null runtime builder");
    return cc_http_client_reset_connections(&builder->http_client);
}

/*
 * 销毁 builder 及其拥有的所有组件。
 *
 * 释放顺序按依赖关系反向执行：先停止队列/manager，再销毁 runtime，然后销毁 tool
 * pool、skills、registry、plugin/MCP、store/provider/policy/sandbox、event bus/logger。
 * 这种顺序能避免后销毁对象在析构期间访问已经失效的底层端口。
 */
cc_result_t cc_runtime_builder_destroy(cc_runtime_builder_t *builder, uint32_t timeout_ms)
{
    if (!builder) return cc_result_ok();
    if (builder->logger) cc_logger_log(builder->logger, CC_LOG_INFO, "Shutting down...");
    cc_runtime_builder_request_shutdown(builder);
    uint64_t destroy_started_ms = cc_platform_monotonic_ms();
#if CC_ENABLE_MULTI_AGENT && CC_ENABLE_RUN_QUEUE
    if (builder->run_queue) {
        cc_result_t queue_shutdown = cc_run_queue_shutdown(builder->run_queue, timeout_ms);
        if (queue_shutdown.code != CC_OK) return queue_shutdown;
        cc_result_free(&queue_shutdown);
        cc_run_queue_destroy(builder->run_queue);
        builder->run_queue = NULL;
    }
    if (builder->agent_manager) {
        cc_agent_manager_destroy(builder->agent_manager);
        builder->agent_manager = NULL;
    }
#endif

    if (builder->generation_mutex) {
        cc_mutex_lock(builder->generation_mutex);
        for (;;) {
            size_t refs = builder->current_generation ? builder->current_generation->refs : 0;
            for (cc_runtime_generation_t *it = builder->retired_generations; it; it = it->next) {
                refs += it->refs;
            }
            if (refs == 0 && !builder->reload_in_progress &&
                builder->reclaims_inflight == 0) {
                break;
            }
            uint64_t elapsed_ms = cc_platform_monotonic_ms() - destroy_started_ms;
            if (elapsed_ms >= timeout_ms) break;
            uint64_t remaining_ms = (uint64_t)timeout_ms - elapsed_ms;
            int slice = remaining_ms > 250U ? 250 : (int)remaining_ms;
            (void)cc_cond_timedwait(builder->generation_cond, builder->generation_mutex, slice);
        }
        size_t remaining_refs = builder->current_generation ? builder->current_generation->refs : 0;
        for (cc_runtime_generation_t *it = builder->retired_generations; it; it = it->next) {
            remaining_refs += it->refs;
        }
        if (remaining_refs > 0 || builder->reload_in_progress ||
            builder->reclaims_inflight > 0) {
            cc_mutex_unlock(builder->generation_mutex);
            if (builder->logger) {
                cc_logger_log(builder->logger, CC_LOG_ERROR,
                    "Runtime builder destroy timed out with active generation references");
            }
            return cc_result_error(CC_ERR_TIMEOUT,
                                   "Runtime builder destroy timed out with active references");
        }
        cc_mutex_unlock(builder->generation_mutex);
    }

    /*
     * Event callbacks may still hold borrowed objects owned by the generation.
     * Drain the bus before detaching the generation list.  In particular, a
     * timeout must leave the builder structurally intact so destroy can be
     * retried safely.
     */
    if (builder->event_bus) {
        uint64_t elapsed_ms = cc_platform_monotonic_ms() - destroy_started_ms;
        int remaining_ms = elapsed_ms >= timeout_ms ? 0 : (int)((uint64_t)timeout_ms - elapsed_ms);
        cc_result_t event_shutdown = cc_event_bus_shutdown(builder->event_bus, remaining_ms);
        if (event_shutdown.code != CC_OK) return event_shutdown;
        cc_result_free(&event_shutdown);
    }

    cc_runtime_generation_t *current = NULL;
    cc_runtime_generation_t *retired = NULL;
    if (builder->generation_mutex) {
        cc_mutex_lock(builder->generation_mutex);
        current = builder->current_generation;
        retired = builder->retired_generations;
        builder->current_generation = NULL;
        builder->retired_generations = NULL;
        cc_mutex_unlock(builder->generation_mutex);
    }

    if (current) {
        current->next = NULL;
        runtime_generation_destroy(current);
    } else {
        /* 构建失败发生在 initial generation 发布之前时，字段仍由 builder 直接持有。 */
        cc_agent_runtime_destroy(builder->runtime);
        if (builder->llm.vtable && builder->llm.vtable->destroy) {
            builder->llm.vtable->destroy(builder->llm.self);
        }
#if CC_ENABLE_TOOL_POOL
        cc_tool_executor_pool_destroy(builder->tool_pool);
#endif
#if CC_ENABLE_SKILLS
        cc_skill_catalog_destroy(builder->skill_catalog);
#endif
        cc_tool_registry_destroy(builder->tool_registry);
        if (builder->features && builder->features->destroy_plugins) {
            builder->features->destroy_plugins(builder->plugin_state);
        }
        if (builder->features && builder->features->destroy_mcp) {
            builder->features->destroy_mcp(builder->mcp_state);
        }
        free(builder->system_prompt);
    }
    while (retired) {
        cc_runtime_generation_t *next = retired->next;
        retired->next = NULL;
        runtime_generation_destroy(retired);
        retired = next;
    }
    if (builder->memory_store.self) cc_memory_store_destroy(&builder->memory_store);
    if (builder->store.vtable && builder->store.vtable->destroy) {
        builder->store.vtable->destroy(builder->store.self);
    }
    cc_http_client_destroy(&builder->http_client);
    if (builder->policy.vtable && builder->policy.vtable->destroy) {
        builder->policy.vtable->destroy(builder->policy.self);
    }
    destroy_sandbox_if_owned(&builder->sandbox);
    if (builder->fs.vtable && builder->fs.vtable->destroy) {
        builder->fs.vtable->destroy(builder->fs.self);
    }
    cc_event_bus_destroy(builder->event_bus);
    cc_logger_destroy(builder->logger);
    cc_cond_destroy(builder->generation_cond);
    cc_mutex_destroy(builder->generation_mutex);
    free(builder);
    return cc_result_ok();
}
