

#include "cc/app/cc_runtime_builder.h"
#include "cc/app/cc_skill_catalog.h"
#include "cc/app/cc_tool_executor_pool.h"
#include "cc_agent_runtime_internal.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_platform.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"
#include "cc/util/cc_tool_name.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#ifndef CC_ENABLE_ASYNC_OBSERVABILITY
#define CC_ENABLE_ASYNC_OBSERVABILITY 0
#endif

/**
 * @brief 一代拥有的 runtime、provider、工具、技能与扩展状态，引用数由 owner 的代际锁保护。
 * 当前代字段同时由 builder 提供别名，退役代最后一个引用在锁外清理，reclaims_inflight 保活 owner。
 */
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

/**
 * @brief 拥有跨代共享端口及当前/退役代的装配根，features 与恢复存储继续借用外层资源。
 * generation_mutex 保护代际发布、引用、重载与关闭状态，维护回调也在此锁内执行。
 * 工厂及代际析构在锁外调用，owner 在关闭入口后等待引用、重载和锁外回收全部结束。
 */
struct cc_runtime_builder {

    const cc_runtime_feature_set_t *features;

    cc_logger_t *logger;

    cc_event_bus_t *event_bus;

    cc_filesystem_t fs;

    cc_http_client_t http_client;

    cc_session_store_t store;

    cc_operation_journal_t *operation_journal;
    int require_operation_journal;
    cc_run_checkpoint_store_t *run_checkpoints;
    cc_run_history_find_fn run_history_find;
    cc_run_history_visit_fn run_history_visit;

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

/**
 * @brief 按依赖顺序清理一代拥有资源，最后释放代容器。
 * @pre 已无该代借用，调用方位于代际锁外，并通过重载标记或 reclaims_inflight 保持 owner 存活。
 * @param[in] generation 消费已摘链或尚未发布的拥有代，允许 NULL。
 * @post runtime、provider、pool、skills、registry、扩展状态与提示副本均释放，共享端口保留。
 * @note 插件和 MCP 销毁回调也可能收到 NULL 状态，features 及回调上下文须仍有效。
 */
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

/**
 * @brief 在代际锁内取得当前代的一份引用，关闭后拒绝新租约。
 * @param[in,out] builder 存活借用对象，本次 API 期间外层排除释放。
 * @param[out] out_generation 必填，参数有效后置 NULL，成功写须配对 release 的引用。
 * @return CC_OK、参数错误或关闭/无当前代时的 CC_ERR_INVALID_STATE。
 */
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

/**
 * @brief 无代际使用者时更新 builder 与当前 runtime 的借用操作账本配置。
 * @param[in,out] builder 存活装配根，须无重载、退役代、引用或锁外回收。
 * @param[in] journal 可选借用账本，外层保证覆盖 builder 和全部代的使用期。
 * @param[in] required 非零要求工具派发存在账本，NULL 账本时工具关闭，普通模型仍可用。
 * @return CC_OK 或参数/状态错误，已有检查点时拒绝移除账本或取消 required，失败保持原配置。
 */
cc_result_t cc_runtime_builder_set_operation_journal(
    cc_runtime_builder_t *builder, cc_operation_journal_t *journal, int required)
{
    if (!builder) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid runtime builder");
    cc_mutex_lock(builder->generation_mutex);
    cc_runtime_generation_t *generation = builder->current_generation;
    if (builder->shutting_down || builder->reload_in_progress || !generation ||
        generation->refs != 0 || builder->retired_generations || builder->reclaims_inflight) {
        cc_mutex_unlock(builder->generation_mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Cannot change operation journal while runtime is in use");
    }
    if (builder->run_checkpoints && (!journal || !required)) {
        cc_mutex_unlock(builder->generation_mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Run recovery requires a device-wide journal");
    }
    builder->operation_journal = journal;
    builder->require_operation_journal = required != 0;
    generation->runtime->operation_journal = journal;
    generation->runtime->require_operation_journal = required != 0;
    cc_mutex_unlock(builder->generation_mutex);
    return cc_result_ok();
}

/**
 * @brief 在尚未附加恢复组件的空闲实例上关闭工具派发，等待完整恢复装配。
 * @param[in,out] builder 存活借用装配根，不可已有账本、检查点或活动代际操作。
 * @return CC_OK 或参数/状态错误，成功把 builder 和当前 runtime 的 required 置一。
 */
cc_result_t cc_runtime_builder_require_recovery(cc_runtime_builder_t *builder)
{
    if (!builder) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid runtime builder");
    cc_mutex_lock(builder->generation_mutex);
    cc_runtime_generation_t *generation = builder->current_generation;
    if (builder->shutting_down || builder->reload_in_progress || !generation || generation->refs != 0 ||
        builder->retired_generations || builder->reclaims_inflight || builder->operation_journal ||
        builder->run_checkpoints) {
        cc_mutex_unlock(builder->generation_mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Cannot require recovery while runtime is in use");
    }
    builder->require_operation_journal = 1;
    generation->runtime->require_operation_journal = 1;
    cc_mutex_unlock(builder->generation_mutex);
    return cc_result_ok();
}

/**
 * @brief 在代际静止时配置或清除检查点与历史精确查询，并清除旧 visitor。
 * @param[in,out] builder 存活装配根，须无重载、引用、退役代或锁外回收。
 * @param[in] checkpoints 借用检查点存储，与 history_find 同时为 NULL 时清除运行恢复。
 * @param[in] history_find 借用查询回调，配置检查点时必填，覆盖各代使用期。
 * @return CC_OK 或参数/状态错误，配置非空检查点前须已有 required 账本，失败保持原配置。
 */
cc_result_t cc_runtime_builder_set_run_recovery(
    cc_runtime_builder_t *builder, cc_run_checkpoint_store_t *checkpoints,
    cc_run_history_find_fn history_find)
{
    if (!builder || (!!checkpoints != !!history_find))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run recovery configuration");
    cc_mutex_lock(builder->generation_mutex);
    cc_runtime_generation_t *generation = builder->current_generation;
    if (builder->shutting_down || builder->reload_in_progress || !generation ||
        generation->refs != 0 || builder->retired_generations || builder->reclaims_inflight ||
        (checkpoints && (!builder->operation_journal || !builder->require_operation_journal))) {
        cc_mutex_unlock(builder->generation_mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Run recovery cannot be configured while in use or without a journal");
    }
    builder->run_checkpoints = checkpoints;
    builder->run_history_find = history_find;
    builder->run_history_visit = NULL;
    generation->runtime->run_checkpoints = checkpoints;
    generation->runtime->run_history_find = history_find;
    generation->runtime->run_history_visit = NULL;
    cc_mutex_unlock(builder->generation_mutex);
    return cc_result_ok();
}

/**
 * @brief 以无 visitor 模式原子附加必需账本、检查点及历史精确查询。
 * @param[in,out] builder 存活且尚未附加恢复资源的装配根。
 * @param[in] journal 非空借用账本，覆盖 builder 及全部代使用期。
 * @param[in] checkpoints 非空借用检查点存储，同样由外层保活。
 * @param[in] history_find 非空借用查询回调。
 * @return 与带 visitor 的附加入口相同，失败保留全部原配置。
 */
cc_result_t cc_runtime_builder_set_recovery_bundle(
    cc_runtime_builder_t *builder, cc_operation_journal_t *journal,
    cc_run_checkpoint_store_t *checkpoints, cc_run_history_find_fn history_find)
{
    return cc_runtime_builder_set_recovery_bundle_with_visitor(
        builder, journal, checkpoints, history_find, NULL);
}

/**
 * @brief 在代际锁内一次附加恢复 bundle，成功后工具要求该账本。
 * @param[in,out] builder 存活装配根，当前无租约、重载、退役代、回收或既有恢复资源。
 * @param[in] journal 非空外部账本，借用至 builder 及全部代退出。
 * @param[in] checkpoints 非空外部检查点存储，借用寿命相同。
 * @param[in] history_find 非空精确查询回调，外层保活对应实现及其存储。
 * @param[in] history_visit 可选完整历史访问回调，以有界内存扫描接口供 runtime 借用。
 * @return CC_OK 或参数/状态错误，所有检查通过后才同时修改 builder 和当前 runtime。
 */
cc_result_t cc_runtime_builder_set_recovery_bundle_with_visitor(
    cc_runtime_builder_t *builder, cc_operation_journal_t *journal,
    cc_run_checkpoint_store_t *checkpoints, cc_run_history_find_fn history_find,
    cc_run_history_visit_fn history_visit)
{
    if (!builder || !journal || !checkpoints || !history_find)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid recovery bundle");
    cc_mutex_lock(builder->generation_mutex);
    cc_runtime_generation_t *generation = builder->current_generation;
    if (builder->shutting_down || builder->reload_in_progress || !generation || generation->refs != 0 ||
        builder->retired_generations || builder->reclaims_inflight || builder->operation_journal ||
        builder->run_checkpoints) {
        cc_mutex_unlock(builder->generation_mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Cannot attach recovery while runtime is in use");
    }
    builder->operation_journal = journal;
    builder->require_operation_journal = 1;
    builder->run_checkpoints = checkpoints;
    builder->run_history_find = history_find;
    builder->run_history_visit = history_visit;
    generation->runtime->operation_journal = journal;
    generation->runtime->require_operation_journal = 1;
    generation->runtime->run_checkpoints = checkpoints;
    generation->runtime->run_history_find = history_find;
    generation->runtime->run_history_visit = history_visit;
    cc_mutex_unlock(builder->generation_mutex);
    return cc_result_ok();
}

/**
 * @brief 归还一次代际引用，退役代归零时摘链并在锁外销毁。
 * @pre 非空代由调用者持有尚未归还的引用，owner 存活至释放及锁外回收结束。
 * @param[in] generation 消费一份引用，允许 NULL。
 * @post 该份借用结束，退役代可能已销毁，调用者停止使用原引用及其端口借用。
 * @note 回收期间通过 reclaims_inflight 阻止 owner 过早销毁，释放后广播代际条件变量。
 */
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

/**
 * @brief 由有效代际引用取得 runtime 借用。
 * @param[in] generation 尚未归还的引用，允许 NULL。
 * @return 非空代的 runtime，NULL 代返回 NULL，借用止于该引用 release。
 */
cc_agent_runtime_t *cc_runtime_generation_runtime(cc_runtime_generation_t *generation)
{
    return generation ? generation->runtime : NULL;
}

/**
 * @brief 借用该代嵌入的 provider 端口，不增加额外引用。
 * @param[in] generation 当前持有的代际引用，允许 NULL。
 * @return 代及 provider vtable 存在时返回借用端口，其他情况 NULL，使用在 release 前结束。
 * @note provider 共享调用由外层按其并发能力同步，调用者不销毁端口。
 */
cc_llm_provider_t *cc_runtime_generation_borrow_provider(cc_runtime_generation_t *generation)
{ return generation && generation->llm.vtable ? &generation->llm : NULL; }

/**
 * @brief 借用 builder 持有且跨重载保持的记忆端口。
 * @param[in] builder 存活借用装配根，允许 NULL。
 * @return self 和 vtable 均存在时返回端口，否则 NULL，直接使用者在 builder 销毁前退出。
 */
cc_memory_store_t *cc_runtime_builder_borrow_memory_store(cc_runtime_builder_t *builder)
{ return builder && builder->memory_store.self && builder->memory_store.vtable ? &builder->memory_store : NULL; }

/**
 * @brief 排除代际引用、重载和回收后，在代际锁内同步执行记忆维护回调。
 * @pre 外层已排空所有直接记忆借用者，回调不得重入 builder API 或保存传入端口。
 * @param[in,out] builder 存活装配根，不能处于关闭状态。
 * @param[in] callback 必填同步维护函数，端口缺失时仍以 NULL 调用。
 * @param[in,out] user_data 回调借用上下文，允许 NULL，覆盖本次同步调用。
 * @return 参数/状态错误或回调拥有的结果，回调副作用按其自身契约保留。
 */
cc_result_t cc_runtime_builder_maintain_memory_quiescent(
    cc_runtime_builder_t *builder, cc_runtime_memory_maintenance_fn callback, void *user_data)
{
    if (!builder || !callback)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory maintenance request");
    cc_mutex_lock(builder->generation_mutex);
    cc_runtime_generation_t *generation=builder->current_generation;
    if (builder->shutting_down || builder->reload_in_progress || !generation || generation->refs ||
        builder->retired_generations || builder->reclaims_inflight) {
        cc_mutex_unlock(builder->generation_mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Cannot maintain memory while runtime is in use");
    }
    cc_memory_store_t *memory=builder->memory_store.self && builder->memory_store.vtable ?
        &builder->memory_store : NULL;
    cc_result_t rc=callback(memory,user_data);
    cc_mutex_unlock(builder->generation_mutex);
    return rc;
}

/**
 * @brief 从有效代际引用读取创建时固定的版本号。
 * @param[in] generation 借用的尚未释放代，允许 NULL。
 * @return 非空代的 id，NULL 返回零。
 */
unsigned long cc_runtime_generation_id(const cc_runtime_generation_t *generation)
{
    return generation ? generation->id : 0;
}

#if CC_ENABLE_MULTI_AGENT && CC_ENABLE_RUN_QUEUE
/**
 * @brief 为 manager 获取当前代引用，并借出该代 runtime。
 * @param[in,out] context 借用的 builder，覆盖 acquire 和后续 release。
 * @param[out] out_runtime 必填，输出齐全后先置 NULL，成功写租约期内借用 runtime。
 * @param[out] out_lease 必填，输出齐全后先置 NULL，成功移交须 release 的代引用。
 * @return 参数错误或代际取得结果，失败保持两个有效输出为空。
 */
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

/**
 * @brief 归还 manager 已结束使用的代际租约。
 * @param[in] context 原注册 builder 上下文，本函数忽略，owner 仍须覆盖租约清理。
 * @param[in] lease 消费 acquire 返回的引用，允许 NULL。
 * @post manager 对该代 runtime 的使用结束，退役代可能在此清理。
 */
static void builder_agent_runtime_release(void *context, void *lease)
{
    (void)context;
    cc_runtime_generation_release((cc_runtime_generation_t *)lease);
}
#endif

/**
 * @brief 清零重载报告并把新旧代号初始化为当前代号。
 * @param[out] report 可选借用输出，NULL 直接返回，其他内容全部覆盖。
 * @param[in] generation 已在代际锁内快照的当前版本。
 * @post 非空报告的诊断为空，新旧版本相同，其余标志为零。
 */
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

/**
 * @brief 标记代际准备失败并将组件名、结果消息复制到固定报告缓冲。
 * @param[in,out] report 可选借用报告，NULL 直接返回。
 * @param[in] component 借用组件名，NULL 使用 unknown，过长按缓冲截断。
 * @param[in] rc 可选借用结果，只复制消息，不接管或释放诊断。
 * @post rolled_back 为一，旧代保留，错误说明在原结果释放后仍可读取。
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

/**
 * @brief 检查启用列表是否按完整名称或别名选择该内置工具。
 * @param[in] config 借用配置，NULL、无列表或零条时默认启用。
 * @param[in] name 可选借用正式名，按字节比较。
 * @param[in] alias 可选借用兼容别名，按字节比较。
 * @return 默认启用或任一匹配返回一，列表存在但未匹配返回零，跳过 NULL 列表项。
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

/**
 * @brief 消费尚未转交 registry 的工具端口，调用可用的 self 析构后清零。
 * @param[in,out] tool 拥有且无并发借用的端口槽，允许 NULL。
 * @post 非空槽已清零，self/vtable/destroy 齐备时拥有实例已销毁。
 */
static void destroy_tool_if_unowned(cc_tool_t *tool)
{
    if (tool && tool->vtable && tool->vtable->destroy && tool->self) {
        tool->vtable->destroy(tool->self);
    }
    if (tool) memset(tool, 0, sizeof(*tool));
}

/**
 * @brief 向 registry 转交工厂工具及可信 capabilities，失败时清理未转交端口。
 * @param[in,out] registry 有效且尚未冻结的借用注册表。
 * @param[in,out] tool 必填拥有端口槽，成功和注册失败后均清零。
 * @param[in] capabilities 本次可信工厂输出的能力位。
 * @return 注册结果，成功由 registry 接管端口 self，失败已调用本地可用析构。
 */
static cc_result_t register_created_tool(cc_tool_registry_t *registry, cc_tool_t *tool, unsigned capabilities)
{
    cc_result_t rc = cc_tool_registry_add_with_capabilities(registry, *tool, capabilities);
    if (rc.code != CC_OK) {
        destroy_tool_if_unowned(tool);
        return rc;
    }
    memset(tool, 0, sizeof(*tool));
    return cc_result_ok();
}

/**
 * @brief 消费可选 sandbox 端口并清零其槽。
 * @param[in,out] sandbox 拥有端口且无在途使用，允许 NULL。
 * @post self/vtable/destroy 齐备时已析构，非空槽在所有路径清零。
 */
static void destroy_sandbox_if_owned(cc_sandbox_t *sandbox)
{
    if (sandbox && sandbox->vtable && sandbox->vtable->destroy && sandbox->self) {
        sandbox->vtable->destroy(sandbox->self);
    }
    if (sandbox) memset(sandbox, 0, sizeof(*sandbox));
}

/**
 * @brief 在装配失败时先销毁 registry 工具，再归还插件和 MCP loader 状态。
 * @param[in] builder 借用装配根，提供存活的 features 析构回调，允许 NULL。
 * @param[in] registry 消费拥有注册表，允许 NULL。
 * @param[in] plugin_state 消费插件状态，允许 NULL，有析构回调时仍传入 NULL。
 * @param[in] mcp_state 消费 MCP 状态，允许 NULL，规则同插件。
 * @post 能经所提供回调释放的本次资源均已归还，调用方不再使用这些拥有句柄。
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

/**
 * @brief 清空输出后按 provider 名称查找首个已编译且有工厂的描述符。
 * @param[in] config 非空借用配置，provider 为 NULL 时按空名称查找。
 * @param[in] features 有效借用描述符集合，非零数量须有对应数组。
 * @param[in,out] http_client 工厂同步借用的共享客户端，其寿命覆盖所创建 provider。
 * @param[out] out_llm 必填空资源槽，先清零，匹配后交给工厂填写拥有端口。
 * @return 工厂原结果，未找到启用描述符返回 CC_ERR_INVALID_ARGUMENT。
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

/**
 * @brief 按本地字符串规则筛选可自动用作摘要缓存命名空间的 HTTP(S) 地址。
 * @param[in] base_url 借用 C 字符串，允许 NULL。
 * @return 接受时借出原地址，否则 NULL，调用者在原配置失效前复制所需内容。
 * @note 仅接受小写 http:// 或 https://，非空 authority，不含 @、?、#，路径为空或单个尾斜杠。
 * 该筛选按字符形式限制常见凭据/路径输入，完整 URL 合法性与命名空间保密性由配置者负责。
 */
static const char *summary_cache_origin(const char *base_url)
{
    if (!base_url) return NULL;
    const char *authority = NULL;
    if (strncmp(base_url, "https://", 8) == 0) authority = base_url + 8;
    else if (strncmp(base_url, "http://", 7) == 0) authority = base_url + 7;
    if (!authority || !authority[0] || authority[0] == '/' || strpbrk(authority, "@?#")) return NULL;
    const char *path = strchr(authority, '/');
    if (path && path[1] != '\0') return NULL;
    return base_url;
}

/**
 * @brief 顺序创建内置工具、插件和 MCP 工具，冻结成功后交付新 registry 及扩展状态。
 * @param[in] builder 有效借用装配根，features、共享端口在工厂及后续工具使用期间存活。
 * @param[in] config 调用期间稳定的借用配置，工厂自行复制后续所需内容。
 * @param[out] out_registry 必填独立空槽，入口置 NULL，成功移交拥有注册表。
 * @param[out] out_plugin_state 可选状态槽，入口置 NULL，loader 存在时调用方须提供以接管状态。
 * @param[out] out_mcp_state 可选状态槽，入口置 NULL，规则同插件。
 * @param[in,out] diagnostics 可选借用诊断，loader 可追加非致命说明，失败也可能已有新增项。
 * @return CC_OK 或 registry、工厂、loader/冻结错误，失败清理已成功创建和转交的资源。
 * @note 工厂失败时自行清理尚未成功交付的工具。self 非空而 vtable 为空无法析构，视为工厂契约错误。
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
        unsigned capabilities = 0;
        ctx.out_capabilities = &capabilities;
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
        rc = register_created_tool(registry, &tool, capabilities);
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

/**
 * @brief 把初始工具 registry、插件/MCP 状态及诊断写入 builder 的空资源槽。
 * @param[in,out] builder 初始装配中的有效对象，相关输出槽尚无拥有资源。
 * @param[in] config 借用创建配置。
 * @return 完整工具构建结果，失败已清理本次工具状态。
 */
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

/**
 * @brief 合并普通策略与 plugin/MCP lane 配置，复制到新工具池后释放临时数组和名称。
 * @param[in] config 非空借用配置，非零数量须有数组，内容在本次调用期间稳定。
 * @param[out] out_pool 必填空资源槽，入口置 NULL，成功取得拥有池，编译关闭时保持 NULL。
 * @return CC_OK 或参数、计数/尺寸溢出、分配及池创建错误，失败归还本次临时资源。
 * @note plugin/MCP lane 按来源组件编码并动态分配全长名称，普通策略名称继续借用到池创建完成。
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

    if (config->plugins.entry_count > SIZE_MAX - config->tools.policy_count ||
        config->mcp.server_count > SIZE_MAX - config->tools.policy_count - config->plugins.entry_count) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Tool pool policy count overflow");
    }
    size_t policy_count = config->tools.policy_count +
        config->plugins.entry_count + config->mcp.server_count;
    if (policy_count > SIZE_MAX / sizeof(cc_tool_executor_pool_policy_t)) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Tool pool policy size overflow");
    }
    if ((config->tools.policy_count && !config->tools.policies) ||
        (config->plugins.entry_count && !config->plugins.entries) ||
        (config->mcp.server_count && !config->mcp.servers)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Missing tool pool policy configuration");
    }
    cc_tool_executor_pool_policy_t *policies = NULL;
    if (policy_count > 0) {
        policies = calloc(policy_count, sizeof(*policies));
        if (!policies) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate tool pool policies");
    }

    cc_result_t rc = cc_result_ok();
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
        char *lane = NULL;
        rc = cc_tool_name_build("plugin", entry->id, NULL, &lane);
        if (rc.code != CC_OK) {
            cc_error_code_t code = rc.code;
            cc_result_free(&rc);
            rc = cc_result_error(code, "Failed to build plugin tool lane");
            goto cleanup_policies;
        }
        policies[written].name = lane;
        policies[written].concurrency = entry->max_in_flight > 0 ?
            entry->max_in_flight : (entry->workers > 0 ? entry->workers : config->queue.plugin_concurrency);
        policies[written].timeout_ms = entry->timeout_ms;
        written++;
    }
    for (size_t i = 0; i < config->mcp.server_count; i++) {
        const cc_config_mcp_server_t *server = &config->mcp.servers[i];
        if (!server->name) continue;
        char *lane = NULL;
        rc = cc_tool_name_build("mcp", server->name, NULL, &lane);
        if (rc.code != CC_OK) {
            cc_error_code_t code = rc.code;
            cc_result_free(&rc);
            rc = cc_result_error(code, "Failed to build MCP tool lane");
            goto cleanup_policies;
        }
        policies[written].name = lane;
        policies[written].concurrency = config->queue.mcp_concurrency;
        policies[written].timeout_ms = server->connection_timeout_ms;
        written++;
    }
    pool_config.policies = policies;
    pool_config.policy_count = written;

    rc = cc_tool_executor_pool_create(&pool_config, out_pool);
cleanup_policies:
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

/**
 * @brief 为初始 builder 空槽创建工具池，编译关闭时保持空池。
 * @param[in,out] builder 有效装配根，tool_pool 尚无拥有资源。
 * @param[in] config 调用期间借用的配置。
 * @return 工具池构建结果，成功对象由 builder 持有。
 */
static cc_result_t build_tool_pool(cc_runtime_builder_t *builder, const cc_config_t *config)
{
    return create_tool_pool_from_config(config, &builder->tool_pool);
}

/**
 * @brief 在 multi-agent 与 run-queue 均启用时按配置创建共享 worker 队列。
 * @param[in,out] builder 初始装配根，run_queue 空槽成功取得拥有队列。
 * @param[in] config 借用队列预算配置，交给队列按值复制并归一化。
 * @return 创建结果，任一编译开关关闭时 CC_OK 且不创建队列。
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

#if CC_ENABLE_MULTI_AGENT && CC_ENABLE_RUN_QUEUE
/**
 * @brief 按配置字符串选择队列 action，缺失或未知值使用 STEER。
 * @param[in] mode 可选借用字符串，识别 steer、followup、collect、interrupt。
 * @return 对应 action，比较区分大小写。
 */
static cc_run_queue_action_t queue_action_from_config(const char *mode)
{
    if (!mode || strcmp(mode, "steer") == 0) return CC_RUN_QUEUE_ACTION_STEER;
    if (strcmp(mode, "followup") == 0) return CC_RUN_QUEUE_ACTION_FOLLOWUP;
    if (strcmp(mode, "collect") == 0) return CC_RUN_QUEUE_ACTION_COLLECT;
    if (strcmp(mode, "interrupt") == 0) return CC_RUN_QUEUE_ACTION_INTERRUPT;
    return CC_RUN_QUEUE_ACTION_STEER;
}
#endif

/**
 * @brief 构建提示副本和可选技能目录，按 allowlist 追加技能提示。
 * @param[in] builder 借用装配根，技能开启时借用其 filesystem 加载目录。
 * @param[in] config 调用期间稳定的非空配置。
 * @param[out] out_system_prompt 必填空资源槽，输出齐全后置 NULL，成功移交需 free 的提示副本。
 * @param[out] out_skill_catalog 必填空资源槽，输出齐全后置 NULL，技能开启时成功取得拥有目录。
 * @return CC_OK 或目录/技能构建、分配错误，失败释放本次目录和提示。
 * @note 基础提示构建出错或无正文时释放诊断并回退默认提示，runtime 创建后另行复制提示内容。
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

/**
 * @brief 顺序创建共享端口、初始代资源与可选队列/manager，再发布拥有的 builder。
 * @param[in] config 非空借用配置，覆盖本次调用，工厂及 runtime 复制需保留的字段。
 * @param[in] features 非空借用工厂集合，保存原指针，覆盖 builder 和所有代的最终清理。
 * @param[out] out_builder 必填，基础参数、必需工厂和描述符数组检查通过后置 NULL，成功移交拥有对象。
 * @return CC_OK 或参数、工厂、端口/分配错误，失败统一收尾尚未发布资源。
 * @note 记忆工厂失败转入固定诊断并继续无记忆装配，其他已创建目录或工厂外部副作用按其自身契约处理。
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
    if ((features->llm_provider_count && !features->llm_providers) ||
        (features->tool_count && !features->tools)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Runtime feature descriptor array is missing");
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
#if CC_HAS_HTTP_CLIENT
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
#endif
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
            cc_runtime_diagnostics_add(&builder->diagnostics, "memory",
                config->memory_backend ? config->memory_backend : "default",
                rc.message ? rc.message : cc_error_string(rc.code));
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
    deps.size = sizeof(deps);
    deps.llm = builder->llm;
    deps.tool_registry = builder->tool_registry;
    deps.store = builder->store;
    deps.policy = builder->policy;
    deps.sandbox = builder->sandbox;
    deps.event_bus = builder->event_bus;
    deps.logger = builder->logger;
    deps.memory_store = builder->memory_store.self ? &builder->memory_store : NULL;
    deps.tool_pool = builder->tool_pool;
    deps.operation_journal = builder->operation_journal;
    deps.require_operation_journal = builder->require_operation_journal;
    deps.run_checkpoints = builder->run_checkpoints;
    deps.run_history_find = builder->run_history_find;
    deps.run_history_visit = builder->run_history_visit;

    cc_agent_runtime_options_t options = {0};
    options.size = sizeof(options);
    options.config = runtime_config;
    options.thinking_mode = config->thinking_mode;
    options.provider_name = config->provider;
    options.summary_cache_namespace = summary_cache_origin(config->base_url);
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

/**
 * @brief 借出可选多 agent 管理器，编译裁剪或无对象时返回空。
 * @param[in] builder 存活借用装配根，允许 NULL，外层排除并发销毁。
 * @return builder 持有的 manager 或 NULL，使用期在 manager_destroy 前结束。
 */
cc_agent_manager_t *cc_runtime_builder_agent_manager(cc_runtime_builder_t *builder)
{
    return builder ? builder->agent_manager : NULL;
}

/**
 * @brief 直接借出初始或最近一次成功重载发布的固定诊断集合。
 * @param[in] builder 存活对象，允许 NULL，读取期间外层排除重载写入和销毁。
 * @return 只读借用诊断或 NULL，调用者不修改、释放或跨销毁保存。
 */
const cc_runtime_diagnostics_t *cc_runtime_builder_diagnostics(cc_runtime_builder_t *builder)
{
    return builder ? &builder->diagnostics : NULL;
}

/**
 * @brief 以无报告模式转交完整代际重载。
 * @param[in,out] builder 存活借用装配根，关闭或已有准备中重载时拒绝。
 * @param[in] config 非空配置，调用期间稳定。
 * @return 完整重载结果，失败保留旧当前代，拥有诊断由调用者清理。
 */
cc_result_t cc_runtime_builder_reload(
    cc_runtime_builder_t *builder,
    const cc_config_t *config
)
{
    return cc_runtime_builder_reload_with_report(builder, config, NULL);
}

/**
 * @brief 锁外构建新 provider、工具、技能、池与 runtime，锁内原子切代并填写成功报告。
 * @param[in,out] builder 存活装配根，内部标记排除同时准备的第二次重载，原代租约继续可用。
 * @param[in] config 非空调用期配置，复用共享端口和原 features，工厂复制所需长期内容。
 * @param[out] out_report 可选独立报告，接纳后初始化，早期参数/状态拒绝保留原值。
 * @return CC_OK 或状态、参数、工厂及分配错误，失败清理新代，旧当前代继续保留。
 * @note 内部持有旧代引用覆盖构建，提交后的新代字段仅在锁内用于报告，避免并发下一重载回收它。
 * 报告的 rolled_back 描述代际未发布，构建工厂造成的文件及外部副作用由对应实现处理。
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
    deps.size = sizeof(deps);
    deps.llm = new_generation->llm;
    deps.tool_registry = new_generation->tool_registry;
    deps.store = builder->store;
    deps.policy = builder->policy;
    deps.sandbox = builder->sandbox;
    deps.event_bus = builder->event_bus;
    deps.logger = builder->logger;
    deps.memory_store = builder->memory_store.self ? &builder->memory_store : NULL;
    deps.tool_pool = new_generation->tool_pool;
    deps.operation_journal = builder->operation_journal;
    deps.require_operation_journal = builder->require_operation_journal;
    deps.run_checkpoints = builder->run_checkpoints;
    deps.run_history_find = builder->run_history_find;
    deps.run_history_visit = builder->run_history_visit;
    cc_agent_runtime_options_t options = {0};
    options.size = sizeof(options);
    options.config = runtime_config;
    options.thinking_mode = config->thinking_mode;
    options.provider_name = config->provider;
    options.summary_cache_namespace = summary_cache_origin(config->base_url);
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
    builder->reload_in_progress = 0;
    cc_cond_broadcast(builder->generation_cond);
    cc_mutex_unlock(builder->generation_mutex);
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

/**
 * @brief 在代际锁内发布关闭状态并唤醒等待者，随后尝试记录日志。
 * @pre 非空 builder 的同步资源仍有效，拥有者保持对象覆盖本次调用。
 * @param[in,out] builder 借用的 builder，允许 NULL，此时直接返回。
 * @post 非空对象保持 shutting_down，后续代际准入据此拒绝，排空和释放由 destroy 继续。
 * @note 该调用只发布关闭，队列取消、任务退出及事件排空各有后续步骤。
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

/**
 * @brief 借出 builder 拥有的 logger，日志同步及脱敏由 logger 实现。
 * @param[in] builder 存活对象，允许 NULL，外层排除销毁。
 * @return logger 借用或 NULL，调用者在 builder 释放前结束使用。
 */
cc_logger_t *cc_runtime_builder_logger(cc_runtime_builder_t *builder)
{
    return builder ? builder->logger : NULL;
}

/**
 * @brief 将连接池重置请求转交本 builder 的共享 HTTP 客户端。
 * @param[in,out] builder 非空存活装配根，HTTP 与在途使用者由外层按端口约定协调。
 * @return 空对象为参数错误，其他原样返回 HTTP 端口结果。
 */
cc_result_t cc_runtime_builder_reset_http_connections(cc_runtime_builder_t *builder)
{
    if (!builder) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null runtime builder");
    return cc_http_client_reset_connections(&builder->http_client);
}

/**
 * @brief 关闭运行准入并按依赖次序排空、释放 builder 拥有的组件。
 * @pre owner 已关闭外部入口，调用期间 builder 存活，调用线程不在本 builder 待退出的任务或回调内。
 * @param[in,out] builder 消费拥有的 builder，NULL 直接成功，失败时仍由调用者保管并可重试。
 * @param[in] timeout_ms 队列及代际/事件屏障的毫秒预算，锁等待与具体端口收尾还需按各自契约解释。
 * @return 完成释放为 CC_OK，队列、活动代际或事件排空失败向上传递，失败保留尚未释放的依赖。
 * @note 先关闭队列、销毁仍借用队列的 manager，再销毁队列。成功释放后原句柄失效。
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
    }
    if (builder->agent_manager) {
        cc_agent_manager_destroy(builder->agent_manager);
        builder->agent_manager = NULL;
    }
    if (builder->run_queue) {
        cc_run_queue_destroy(builder->run_queue);
        builder->run_queue = NULL;
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

    /* 事件回调可能仍借用代际对象，先排空再摘除链表。超时保留这些依赖，已释放的上层字段保持空值供重试。 */
    if (builder->event_bus) {
        uint64_t elapsed_ms = cc_platform_monotonic_ms() - destroy_started_ms;
        uint64_t remaining_budget = elapsed_ms >= timeout_ms ? 0 : (uint64_t)timeout_ms - elapsed_ms;
        int remaining_ms = remaining_budget > INT_MAX ? INT_MAX : (int)remaining_budget;
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
