#include "cc/app/cc_runtime_builder.h"
#include "cc/app/cc_tool_executor_pool.h"

#include <stdlib.h>
#include <string.h>

static int g_tool_destroy_count;
static int g_llm_destroy_count;
static int g_store_create_count;

#if defined(CC_TEST_BUILDER_POLICY_OOM) && CC_ENABLE_TOOL_POOL
/** @brief 故障状态仅在当前装配线程使用，避免异步观测线程消耗名称复制故障。 */
static _Thread_local int g_arm_after_freeze;
static _Thread_local int g_fail_next_malloc;
static _Thread_local size_t g_injected_malloc_bytes;

/**
 * @brief 声明链接包装调用的真实分配入口。
 * @param[in] bytes 分配字节数。
 * @return 真实 malloc 结果，成功缓冲由原拥有者释放。
 */
void *__real_malloc(size_t bytes);

/**
 * @brief 在冻结后的下一次 malloc 注入失败，记录实际目标长度。
 * @param[in] bytes 当前线程申请的字节数。
 * @return 故障命中时 NULL，其他情况返回真实 malloc 结果。
 */
void *__wrap_malloc(size_t bytes)
{
    if (g_fail_next_malloc) {
        g_fail_next_malloc = 0;
        g_injected_malloc_bytes = bytes;
        return NULL;
    }
    return __real_malloc(bytes);
}

/**
 * @brief 声明链接包装调用的真实注册表冻结入口。
 * @param[in,out] registry 存活且由 builder 拥有的注册表。
 * @return 真实冻结结果，错误消息沿调用者清理。
 */
cc_result_t __real_cc_tool_registry_freeze(cc_tool_registry_t *registry);

/**
 * @brief 成功冻结后为当前装配线程准备一次 lane 名称分配故障。
 * @param[in,out] registry 借用注册表，转交真实冻结入口。
 * @return 原样交付冻结结果，故障开关只在成功且主动启用时设置。
 * @note builder 随后 calloc 临时策略数组，下一次 malloc 对应 plugin 或 MCP 名称复制。
 */
cc_result_t __wrap_cc_tool_registry_freeze(cc_tool_registry_t *registry)
{
    cc_result_t rc = __real_cc_tool_registry_freeze(registry);
    if (rc.code == CC_OK && g_arm_after_freeze) {
        g_arm_after_freeze = 0;
        g_fail_next_malloc = 1;
    }
    return rc;
}
#endif

/**
 * @brief 提供成功工厂工具的固定名称，供 registry 注册与 schema 构建。
 * @param[in] self 调用期间借用的工具实例，本回调忽略。
 * @return 静态只读名称 test.ok，调用者保持借用。
 */
static const char *test_tool_name(void *self)
{
    (void)self;
    return "test.ok";
}

/**
 * @brief 提供本地测试工具的固定说明。
 * @param[in] self 调用期间借用的工具实例，本回调忽略。
 * @return 静态只读说明，无需释放。
 */
static const char *test_tool_description(void *self)
{
    (void)self;
    return "test tool";
}

/**
 * @brief 声明测试工具接收具有空 properties 的对象参数。
 * @param[in] self 调用期间借用的工具实例，本回调忽略。
 * @return 静态只读 schema JSON，registry 和 executor 同步借用。
 */
static const char *test_tool_schema(void *self)
{
    (void)self;
    return "{\"type\":\"object\",\"properties\":{}}";
}

/**
 * @brief 提供无动作的工具入口，使成功工厂产物具备完整可调用 vtable。
 * @param[in] self 同步借用的工具实例，本回调忽略。
 * @param[in] args_json 同步借用的参数正文，本回调忽略。
 * @param[in] ctx 同步借用的执行上下文，本回调忽略。
 * @param[in,out] out_result 本回归路径不会调用工具，此入口保持传入结果原值。
 * @return 返回 CC_OK，无分配或外部副作用。
 */
static cc_result_t test_tool_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    (void)self;
    (void)args_json;
    (void)ctx;
    (void)out_result;
    return cc_result_ok();
}

/**
 * @brief 释放成功工厂交付的工具实例，并累计销毁次数。
 * @param[in] self 消费工厂 malloc 的一字节实例，free 允许 NULL。
 * @post 该实例已释放，测试计数加一，由 builder 收尾线程调用。
 */
static void test_tool_destroy(void *self)
{
    free(self);
    g_tool_destroy_count++;
}

static const cc_tool_vtable_t s_test_tool_vtable = {
    test_tool_name,
    test_tool_description,
    test_tool_schema,
    test_tool_call,
    test_tool_destroy
};

/**
 * @brief 创建拥有一字节 self 的工具，检查 builder 对已成功组件的清理。
 * @param[in] ctx 创建期间借用的工厂上下文，本工厂忽略。
 * @param[out] out_tool 必填且无已有资源，成功写入拥有的 self 和静态 vtable，失败保持原值。
 * @return 成功为 CC_OK，空输出为 CC_ERR_INVALID_ARGUMENT，分配失败为 CC_ERR_OUT_OF_MEMORY。
 * @note 成功后由 registry 或装配失败清理路径调用 test_tool_destroy 消费实例。
 */
static cc_result_t create_success_tool(
    const cc_runtime_tool_factory_ctx_t *ctx,
    cc_tool_t *out_tool
)
{
    (void)ctx;
    if (!out_tool) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool output");
    void *self = malloc(1);
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate tool");
    out_tool->self = self;
    out_tool->vtable = &s_test_tool_vtable;
    return cc_result_ok();
}

/**
 * @brief 在成功工具注册后故意中止装配，触发 builder 的部分创建清理。
 * @param[in] ctx 创建期间借用的工厂上下文，本工厂忽略。
 * @param[out] out_tool 可选的空资源输出，非 NULL 时清零，始终无实例交付。
 * @return 固定返回带拥有消息的 CC_ERR_INVALID_ARGUMENT，由调用方释放结果。
 */
static cc_result_t create_failing_tool(
    const cc_runtime_tool_factory_ctx_t *ctx,
    cc_tool_t *out_tool
)
{
    (void)ctx;
    if (out_tool) memset(out_tool, 0, sizeof(*out_tool));
    return cc_result_error(CC_ERR_INVALID_ARGUMENT, "intentional tool factory failure");
}

/**
 * @brief 累计本地 provider 的销毁次数，核对装配失败和完整 builder 的收尾。
 * @param[in] self 工厂提供的非拥有哨兵，本回调忽略。
 * @post provider 销毁计数加一，无堆资源需要归还。
 */
static void fake_llm_destroy(void *self)
{
    (void)self;
    g_llm_destroy_count++;
}

static const cc_llm_provider_vtable_t s_fake_llm_vtable = {
    NULL,
    NULL,
    fake_llm_destroy,
    NULL
};

/**
 * @brief 创建只有销毁回调的本地 provider 哨兵，全部回归均无需模型请求。
 * @param[in] config 同步借用的配置，本工厂忽略。
 * @param[in] http_client 同步借用的客户端，本工厂忽略。
 * @param[out] out_provider 必填且无已有资源，成功写入哨兵及静态 vtable。
 * @return 成功为 CC_OK，空输出为 CC_ERR_INVALID_ARGUMENT；成功后 builder 负责调用 destroy。
 */
static cc_result_t create_fake_llm(
    const cc_config_t *config,
    cc_http_client_t *http_client,
    cc_llm_provider_t *out_provider
)
{
    (void)config;
    (void)http_client;
    if (!out_provider) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null provider output");
    out_provider->self = (void *)1;
    out_provider->vtable = &s_fake_llm_vtable;
    return cc_result_ok();
}

/**
 * @brief 提供可装配的空存储端口，使消息任务在历史扫描入口确定返回存储错误。
 * @param[in] config 同步借用的配置，本工厂忽略。
 * @param[out] out_store 必填且无已有资源，成功清零，无 self 或 vtable 需要销毁。
 * @return 成功为 CC_OK，空输出为 CC_ERR_INVALID_ARGUMENT。
 * @note 每次进入递增存储工厂计数，供基础准入的无工厂调用断言使用。
 */
static cc_result_t create_fake_store(
    const cc_config_t *config,
    cc_session_store_t *out_store
)
{
    g_store_create_count++;
    (void)config;
    if (!out_store) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null store output");
    memset(out_store, 0, sizeof(*out_store));
    return cc_result_ok();
}

/**
 * @brief 提供空策略端口，消息任务在使用策略前已由空存储结束。
 * @param[in] config 同步借用的配置，本工厂忽略。
 * @param[out] out_policy 必填且无已有资源，成功清零，无堆资源交付。
 * @return 成功为 CC_OK，空输出为 CC_ERR_INVALID_ARGUMENT。
 */
static cc_result_t create_fake_policy(
    const cc_config_t *config,
    cc_policy_engine_t *out_policy
)
{
    (void)config;
    if (!out_policy) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null policy output");
    memset(out_policy, 0, sizeof(*out_policy));
    return cc_result_ok();
}

#if CC_ENABLE_MULTI_AGENT && CC_ENABLE_RUN_QUEUE
/**
 * @brief 保留一个已完成未收集的管理器任务，再以最大 uint32_t 预算销毁其 builder。
 * @param[in] config 调用期间借用的基础配置，本函数按值复制并启用同会话串行 FOLLOWUP。
 * @param[in] features 调用期间借用的工厂集合，只使用首个成功工具，所有借用覆盖 builder 销毁。
 * @return 顺序销毁成功且工具/provider 各销毁一次为零，任一检查失败为一。
 * @note 第二任务的同步收集构成同会话 FIFO 屏障，首任务始终留给 manager_destroy 收集。
 * 空存储让两个任务均以 CC_ERR_STORAGE 完成。空闲事件总线应立即收尾，最大预算不会产生长时间等待。
 */
static int test_completed_manager_task_cleanup(
    const cc_config_t *config,
    const cc_runtime_feature_set_t *features
)
{
    cc_config_t queued_config = *config;
    queued_config.queue.main_concurrency = 1;
    queued_config.queue.per_session_concurrency = 1;
    queued_config.queue.mode = "followup";
    cc_runtime_feature_set_t queued_features = *features;
    queued_features.tool_count = 1;
    int tools_before = g_tool_destroy_count;
    int providers_before = g_llm_destroy_count;
    cc_runtime_builder_t *builder = NULL;
    cc_result_t rc = cc_runtime_builder_create(&queued_config, &queued_features, &builder);
    if (rc.code != CC_OK || !builder) {
        cc_result_free(&rc);
        cc_result_t cleanup = cc_runtime_builder_destroy(builder, 1000);
        cc_result_free(&cleanup);
        return 1;
    }
    cc_result_free(&rc);

    int failed = 0;
    cc_agent_manager_t *manager = cc_runtime_builder_agent_manager(builder);
    if (!manager) failed = 1;
    if (manager) {
        cc_run_id_t first_run_id = 0;
        rc = cc_agent_manager_submit(manager, NULL, "cleanup-session", "first", &first_run_id);
        if (rc.code != CC_OK || first_run_id == 0) failed = 1;
        int submitted = rc.code == CC_OK && first_run_id != 0;
        cc_result_free(&rc);
        if (submitted) {
            char *response = NULL;
            rc = cc_agent_manager_handle_message(manager, NULL, "cleanup-session", "barrier", &response);
            if (rc.code != CC_ERR_STORAGE || response) failed = 1;
            cc_result_free(&rc);
            free(response);
        }
    }

    rc = cc_runtime_builder_destroy(builder, UINT32_MAX);
    if (rc.code != CC_OK) {
        failed = 1;
        cc_result_free(&rc);
        rc = cc_runtime_builder_destroy(builder, 1000);
    }
    cc_result_free(&rc);
    if (g_tool_destroy_count != tools_before + 1 ||
        g_llm_destroy_count != providers_before + 1) failed = 1;
    return failed;
}
#endif

#if CC_ENABLE_TOOL_POOL
/**
 * @brief 通过公共 builder 工厂核对策略构建错误及已创建组件的配对清理。
 * @param[in] config 借用异常策略配置，预期在运行时创建前失败。
 * @param[in] features 借用原工厂集合，局部副本只启用首个工具并关闭插件/MCP loader。
 * @param[in] expected 预期错误码。
 * @param[in] needle 借用的预期错误消息片段。
 * @return 错误、空 builder 和工具/provider 各销毁一次均匹配为零，其余为一。
 * @note 配置中的伪造大计数在数组遍历前被拒绝，所有字段继续借用，函数不 destroy 配置。
 */
static int expect_policy_build_failure(
    const cc_config_t *config,
    const cc_runtime_feature_set_t *features,
    cc_error_code_t expected,
    const char *needle)
{
    cc_runtime_feature_set_t local_features = *features;
    local_features.tool_count = 1;
    local_features.load_plugins = NULL;
    local_features.load_mcp = NULL;
    int tools_before = g_tool_destroy_count;
    int providers_before = g_llm_destroy_count;
    cc_runtime_builder_t *builder = NULL;
    cc_result_t rc = cc_runtime_builder_create(config, &local_features, &builder);
#if defined(CC_TEST_BUILDER_POLICY_OOM)
    g_arm_after_freeze = 0;
    g_fail_next_malloc = 0;
#endif
    int failed = rc.code != expected || builder != NULL ||
        !rc.message || !strstr(rc.message, needle);
    cc_result_free(&rc);
    cc_result_t cleanup = cc_runtime_builder_destroy(builder, 1000);
    if (cleanup.code != CC_OK) failed = 1;
    cc_result_free(&cleanup);
    if (g_tool_destroy_count != tools_before + 1 ||
        g_llm_destroy_count != providers_before + 1) failed = 1;
    return failed;
}

/**
 * @brief 检查三类策略计数相加、数组字节溢出和非零计数缺失数组。
 * @param[in] config 借用无扩展集合的基础配置，按值复制后只改变目标计数。
 * @param[in] features 借用工厂集合，所有借用覆盖每次创建与销毁。
 * @return 六个边界均在遍历前按预期失败且清理完成为零，其余为一。
 */
static int test_policy_configuration_boundaries(
    const cc_config_t *config,
    const cc_runtime_feature_set_t *features)
{
    cc_config_t invalid = *config;
    invalid.tools.policy_count = SIZE_MAX;
    invalid.plugins.entry_count = 1;
    if (expect_policy_build_failure(&invalid, features, CC_ERR_LIMIT_EXCEEDED, "count overflow")) return 1;
    invalid = *config;
    invalid.tools.policy_count = SIZE_MAX - 1U;
    invalid.plugins.entry_count = 1;
    invalid.mcp.server_count = 1;
    if (expect_policy_build_failure(&invalid, features, CC_ERR_LIMIT_EXCEEDED, "count overflow")) return 1;
    invalid = *config;
    invalid.tools.policy_count = SIZE_MAX / sizeof(cc_tool_executor_pool_policy_t) + 1U;
    if (expect_policy_build_failure(&invalid, features, CC_ERR_LIMIT_EXCEEDED, "size overflow")) return 1;
    invalid = *config;
    invalid.tools.policy_count = 1;
    if (expect_policy_build_failure(&invalid, features, CC_ERR_INVALID_ARGUMENT, "Missing tool pool")) return 1;
    invalid = *config;
    invalid.plugins.entry_count = 1;
    if (expect_policy_build_failure(&invalid, features, CC_ERR_INVALID_ARGUMENT, "Missing tool pool")) return 1;
    invalid = *config;
    invalid.mcp.server_count = 1;
    return expect_policy_build_failure(&invalid, features, CC_ERR_INVALID_ARGUMENT, "Missing tool pool");
}

#if defined(CC_TEST_BUILDER_POLICY_OOM)
/**
 * @brief 分别注入 plugin 与 MCP lane 名称复制失败，核对 OOM 向上传递。
 * @param[in] config 借用基础配置，局部集合及字符串保持到创建/清理返回。
 * @param[in] features 借用工厂集合，真实扩展 loader 在测试辅助函数中关闭。
 * @return 两条故障均命中精确长度、返回 OOM 并清理，正常双 lane 对照创建和销毁成功为零，其余为一。
 */
static int test_policy_name_copy_failure(
    const cc_config_t *config,
    const cc_runtime_feature_set_t *features)
{
    cc_config_plugin_entry_t plugin = {0};
    plugin.id = "copy-failure";
    plugin.enabled = 1;
    cc_config_t invalid = *config;
    invalid.plugins.entries = &plugin;
    invalid.plugins.entry_count = 1;
    g_injected_malloc_bytes = 0;
    g_arm_after_freeze = 1;
    int failed = expect_policy_build_failure(
        &invalid, features, CC_ERR_OUT_OF_MEMORY, "plugin tool lane");
    if (g_injected_malloc_bytes != sizeof("plugin.copy-failure")) failed = 1;

    cc_config_mcp_server_t server = {0};
    server.name = "copy-failure";
    invalid = *config;
    invalid.mcp.servers = &server;
    invalid.mcp.server_count = 1;
    g_injected_malloc_bytes = 0;
    g_arm_after_freeze = 1;
    failed |= expect_policy_build_failure(
        &invalid, features, CC_ERR_OUT_OF_MEMORY, "MCP tool lane");
    if (g_injected_malloc_bytes != sizeof("mcp.copy-failure")) failed = 1;

    invalid.plugins.entries = &plugin;
    invalid.plugins.entry_count = 1;
    cc_runtime_feature_set_t local_features = *features;
    local_features.tool_count = 1;
    local_features.load_plugins = NULL;
    local_features.load_mcp = NULL;
    int tools_before = g_tool_destroy_count;
    int providers_before = g_llm_destroy_count;
    cc_runtime_builder_t *builder = NULL;
    cc_result_t rc = cc_runtime_builder_create(&invalid, &local_features, &builder);
    if (rc.code != CC_OK || !builder) failed = 1;
    cc_result_free(&rc);
    rc = cc_runtime_builder_destroy(builder, 1000);
    if (rc.code != CC_OK) failed = 1;
    cc_result_free(&rc);
    if (g_tool_destroy_count != tools_before + 1 ||
        g_llm_destroy_count != providers_before + 1) failed = 1;
    return failed;
}
#endif
#endif

/**
 * @brief 检查描述符准入、工厂失败、策略边界和可选队列组合根对未收集完成任务的清理。
 * @return 全部检查通过为零，失败为一，各次拥有的错误结果均在主线程释放。
 * @note 工厂描述符及配置字符串覆盖 builder 的整个使用期，普通失败清理保留 1000 毫秒预算。
 */
int main(void)
{
    cc_config_t config;
    memset(&config, 0, sizeof(config));
    config.provider = "fake";
    config.model = "fake-model";
    config.data_dir = ".";
    config.workspace_path = ".";

    const cc_llm_provider_descriptor_t providers[] = {
        {"fake", 1, create_fake_llm},
    };
    const cc_tool_descriptor_t tools[] = {
        {"test.ok", NULL, 1, create_success_tool},
        {"test.fail", NULL, 1, create_failing_tool},
    };
    cc_runtime_feature_set_t features;
    memset(&features, 0, sizeof(features));
    features.llm_providers = providers;
    features.llm_provider_count = sizeof(providers) / sizeof(providers[0]);
    features.tools = tools;
    features.tool_count = sizeof(tools) / sizeof(tools[0]);
    features.create_session_store = create_fake_store;
    features.create_policy_engine = create_fake_policy;

    cc_runtime_feature_set_t invalid_features = features;
    cc_runtime_builder_t *sentinel = (cc_runtime_builder_t *)&features;
    cc_runtime_builder_t *rejected = sentinel;
    int stores_before = g_store_create_count;
    invalid_features.llm_providers = NULL;
    cc_result_t early = cc_runtime_builder_create(&config, &invalid_features, &rejected);
    int early_failed = early.code != CC_ERR_INVALID_ARGUMENT || rejected != sentinel;
    cc_result_free(&early);
    invalid_features = features;
    invalid_features.tools = NULL;
    rejected = sentinel;
    early = cc_runtime_builder_create(&config, &invalid_features, &rejected);
    early_failed |= early.code != CC_ERR_INVALID_ARGUMENT || rejected != sentinel;
    cc_result_free(&early);
    if (early_failed || g_store_create_count != stores_before ||
        g_tool_destroy_count != 0 || g_llm_destroy_count != 0) return 1;

    cc_runtime_builder_t *builder = NULL;
    cc_result_t rc = cc_runtime_builder_create(&config, &features, &builder);
    int failed = 0;
    if (rc.code == CC_OK || builder != NULL) failed = 1;
    if (g_tool_destroy_count != 1) failed = 1;
    if (g_llm_destroy_count != 1) failed = 1;
    cc_result_free(&rc);
    cc_result_t destroy_rc = cc_runtime_builder_destroy(builder, 1000);
    cc_result_free(&destroy_rc);
#if CC_ENABLE_MULTI_AGENT && CC_ENABLE_RUN_QUEUE
    failed |= test_completed_manager_task_cleanup(&config, &features);
#endif
#if CC_ENABLE_TOOL_POOL
    failed |= test_policy_configuration_boundaries(&config, &features);
#if defined(CC_TEST_BUILDER_POLICY_OOM)
    failed |= test_policy_name_copy_failure(&config, &features);
#endif
#endif
    return failed ? 1 : 0;
}
