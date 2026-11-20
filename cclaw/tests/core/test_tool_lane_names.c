#include "cc/app/cc_runtime_builder.h"
#include "cc/app/cc_tool_executor.h"
#include "cc/app/cc_tool_executor_pool.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_tool_name.h"
#include "cc/ports/cc_platform.h"

#include <stdlib.h>
#include <string.h>

/** @brief 测试拥有名称和预期 lane，回调只在本次同步执行期间借用。 */
typedef struct lane_tool {
    char *name;
    char *expected_lane;
    int expected_timeout;
    int policy_calls;
    int tool_calls;
    int mismatch;
} lane_tool_t;

static lane_tool_t tools[6];
static size_t next_tool;

#ifdef CC_TEST_TOOL_LANE_ALLOCATION
/** @brief 仅当前测试线程触发一次 malloc 失败，其他 SDK 线程继续使用真实分配器。 */
static _Thread_local int fail_next_malloc;

/**
 * @brief 声明链接包装调用的真实分配入口。
 * @param[in] bytes 分配字节数。
 * @return 真实 malloc 返回的拥有缓冲或 NULL。
 */
void *__real_malloc(size_t bytes);

/**
 * @brief 只在故障开关启用时拒绝当前线程的下一次分配。
 * @param[in] bytes 分配字节数，正常分支转交真实入口。
 * @return 故障为 NULL 并关闭开关，其余真实 malloc 结果。
 */
void *__wrap_malloc(size_t bytes)
{
    if (fail_next_malloc) {
        fail_next_malloc = 0;
        return NULL;
    }
    return __real_malloc(bytes);
}
#endif

/**
 * @brief 核对结果码并释放拥有诊断。
 * @param[in] result 按值消费的结果，原消息由本函数释放。
 * @param[in] expected 预期错误码。
 * @return 匹配为一，其余为零。
 */
static int result_is(cc_result_t result, cc_error_code_t expected)
{
    int matches = result.code == expected;
    cc_result_free(&result);
    return matches;
}

/**
 * @brief 返回当前工具的完整已编码名称。
 * @param[in] self 借用 lane_tool_t，名称在 builder 销毁后才释放。
 * @return 测试拥有的名称借用。
 */
static const char *tool_name(void *self)
{
    return ((lane_tool_t *)self)->name;
}

/**
 * @brief 提供固定工具说明。
 * @param[in] self 借用工具状态，本回调忽略。
 * @return 静态只读说明。
 */
static const char *tool_description(void *self)
{
    (void)self;
    return "lane identity test";
}

/**
 * @brief 提供允许空对象参数的 schema。
 * @param[in] self 借用工具状态，本回调忽略。
 * @return 静态只读空对象 schema。
 */
static const char *tool_schema(void *self)
{
    (void)self;
    return "{}";
}

/**
 * @brief 在实际工具入口核对完整 lane 和 builder 配置的超时。
 * @param[in,out] self 借用工具状态，记录调用和身份不匹配。
 * @param[in] arguments 借用参数正文，本回调忽略。
 * @param[in] context 借用同步上下文，lane 仅在本次调用期间读取。
 * @param[out] out_result 空资源输出，成功交付拥有的 ok 正文。
 * @return 正文分配成功为 CC_OK，失败为 OUT_OF_MEMORY，身份错误由测试状态记录。
 */
static cc_result_t tool_call(void *self, const char *arguments,
    const cc_tool_context_t *context, cc_tool_result_t *out_result)
{
    (void)arguments;
    lane_tool_t *tool = self;
    tool->tool_calls++;
    if (!context->lane_name || strcmp(context->lane_name, tool->expected_lane) != 0 ||
        context->timeout_ms != tool->expected_timeout) tool->mismatch = 1;
    out_result->ok = 1;
    out_result->text = cc_copy_string("ok");
    return out_result->text ? cc_result_ok() :
        cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy lane test output");
}

static const cc_tool_vtable_t tool_vtable = {
    tool_name, tool_description, tool_schema, tool_call, NULL
};

/**
 * @brief 按描述符顺序交付测试状态的借用工具视图。
 * @param[in] context 借用工厂上下文，本工厂忽略。
 * @param[out] out_tool 空资源输出，成功写静态 vtable 和由测试保活的 self。
 * @return 状态充足为 CC_OK，描述符数量异常为 INVALID_STATE。
 */
static cc_result_t create_tool(const cc_runtime_tool_factory_ctx_t *context, cc_tool_t *out_tool)
{
    (void)context;
    if (next_tool >= sizeof(tools) / sizeof(tools[0]))
        return cc_result_error(CC_ERR_INVALID_STATE, "Too many lane test tools");
    out_tool->self = &tools[next_tool++];
    out_tool->vtable = &tool_vtable;
    return cc_result_ok();
}

/**
 * @brief 在策略阶段核对工具名对应的完整 lane 和超时。
 * @param[in] self 借用策略上下文，本回调忽略。
 * @param[in] call 借用调用，完整名称用于定位测试状态。
 * @param[in] context 借用同步上下文，lane 在函数返回后不保留。
 * @param[out] decision 空资源输出，匹配工具时允许执行，无拥有 reason。
 * @return 找到工具为 CC_OK，未找到为 TOOL 错误。
 */
static cc_result_t check_policy(void *self, const cc_tool_call_t *call,
    const cc_tool_context_t *context, cc_policy_decision_t *decision)
{
    (void)self;
    memset(decision, 0, sizeof(*decision));
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        if (strcmp(call->name, tools[i].name) != 0) continue;
        tools[i].policy_calls++;
        if (!context->lane_name || strcmp(context->lane_name, tools[i].expected_lane) != 0 ||
            context->timeout_ms != tools[i].expected_timeout) tools[i].mismatch = 1;
        decision->allowed = 1;
        return cc_result_ok();
    }
    return cc_result_error(CC_ERR_TOOL, "Unknown lane test tool");
}

static const cc_policy_engine_vtable_t policy_vtable = {check_policy, NULL};
static const cc_llm_provider_vtable_t provider_vtable = {0};

/**
 * @brief 提供无需模型请求的 provider 哨兵。
 * @param[in] config 借用创建配置，本工厂忽略。
 * @param[in] http 借用共享客户端，本工厂忽略。
 * @param[out] out_provider 空资源输出，交付无动态 self 的端口。
 * @return CC_OK，回归只执行工具。
 */
static cc_result_t create_provider(const cc_config_t *config, cc_http_client_t *http,
    cc_llm_provider_t *out_provider)
{
    (void)config;
    (void)http;
    out_provider->self = NULL;
    out_provider->vtable = &provider_vtable;
    return cc_result_ok();
}

/**
 * @brief 提供无资源会话端口，直接工具入口无需读写会话。
 * @param[in] config 借用配置，本工厂忽略。
 * @param[out] out_store 空资源输出，清零。
 * @return CC_OK。
 */
static cc_result_t create_store(const cc_config_t *config, cc_session_store_t *out_store)
{
    (void)config;
    memset(out_store, 0, sizeof(*out_store));
    return cc_result_ok();
}

/**
 * @brief 安装核对 lane 的静态策略端口。
 * @param[in] config 借用配置，本工厂忽略。
 * @param[out] out_policy 空资源输出，交付无 self 的静态 vtable。
 * @return CC_OK。
 */
static cc_result_t create_policy(const cc_config_t *config, cc_policy_engine_t *out_policy)
{
    (void)config;
    out_policy->self = NULL;
    out_policy->vtable = &policy_vtable;
    return cc_result_ok();
}

/**
 * @brief 为一个扩展工具创建独立拥有的编码名称与来源 lane。
 * @param[in] index 有效工具槽索引。
 * @param[in] namespace_name 借用 plugin 或 mcp 名称空间。
 * @param[in] source 借用原始来源名，点和百分号由 helper 编码。
 * @param[in] name 借用原始工具名。
 * @return 两个输出均成功为一，失败为零，部分拥有字段由主测试统一释放。
 */
static int prepare_tool(size_t index, const char *namespace_name,
    const char *source, const char *name)
{
    return result_is(cc_tool_name_build(namespace_name, source, name, &tools[index].name), CC_OK) &&
        result_is(cc_tool_name_build(namespace_name, source, NULL, &tools[index].expected_lane), CC_OK);
}

/**
 * @brief 执行六个工具并检查 policy、实际调用和池指标使用同一完整身份。
 * @param[in,out] runtime 借用 builder 当前代 runtime，整个函数由 generation lease 保活。
 * @return 全部身份、超时、调用和许可计数符合预期为零，失败为一。
 */
static int verify_lane_execution(cc_agent_runtime_t *runtime)
{
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        cc_tool_call_t call = {"lane-call", tools[i].name, "{}"};
        cc_tool_result_t result = {0};
        cc_result_t rc = cc_tool_executor_execute(runtime, "lane-session", &call, &result);
        int failed = rc.code != CC_OK || !result.ok || !result.text ||
            strcmp(result.text, "ok") != 0 || tools[i].mismatch ||
            tools[i].policy_calls != 1 || tools[i].tool_calls != 1;
        cc_result_free(&rc);
        cc_tool_result_cleanup(&result);
        if (failed) return 1;
    }
    cc_tool_executor_pool_lane_metrics_t lanes[6];
    cc_tool_executor_pool_metrics_t metrics;
    if (!result_is(cc_tool_executor_pool_get_metrics(cc_agent_runtime_tool_pool(runtime),
            &metrics, lanes, sizeof(lanes) / sizeof(lanes[0])), CC_OK)) return 1;
    if (metrics.lane_count != 6 || metrics.reported_lane_count != 6 ||
        metrics.acquired != 6 || metrics.released != 6 || metrics.total_in_flight != 0) return 1;
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        int found = 0;
        for (size_t j = 0; j < metrics.reported_lane_count; j++) {
            if (strcmp(lanes[j].name, tools[i].expected_lane) != 0) continue;
            found = lanes[j].timeout_ms == tools[i].expected_timeout &&
                lanes[j].acquired == 1 && lanes[j].released == 1;
        }
        if (!found) return 1;
    }
    return 0;
}

#ifdef CC_TEST_TOOL_LANE_ALLOCATION
/**
 * @brief 核对新增 lane 分配 OOM 以及非法参数、取消和截止的既有优先级。
 * @param[in,out] runtime 借用已执行完成功对照的 runtime，本次错误不得再调用策略或工具。
 * @return 所有错误码、输出初始化和调用计数匹配为零，其余为一，取消源及结果均清理。
 */
static int verify_allocation_and_prechecks(cc_agent_runtime_t *runtime)
{
    cc_tool_call_t call = {"lane-call", tools[0].name, "{}"};
    cc_tool_result_t result = {0};
    fail_next_malloc = 1;
    cc_result_t rc = cc_tool_executor_execute(runtime, "lane-session", &call, &result);
    int failed = rc.code != CC_ERR_OUT_OF_MEMORY || result.ok || result.text || result.error ||
        fail_next_malloc || tools[0].policy_calls != 1 || tools[0].tool_calls != 1;
    fail_next_malloc = 0;
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);

    result.ok = 77;
    fail_next_malloc = 1;
    rc = cc_tool_executor_execute(runtime, "lane-session", NULL, &result);
    failed |= rc.code != CC_ERR_INVALID_ARGUMENT || result.ok != 77;
    fail_next_malloc = 0;
    cc_result_free(&rc);
    memset(&result, 0, sizeof(result));

    cc_cancel_source_t *source = NULL;
    if (!result_is(cc_cancel_source_create(&source), CC_OK)) return 1;
    cc_cancel_source_cancel(source);
    cc_tool_executor_options_t options = {0};
    options.cancel_token = cc_cancel_source_token(source);
    fail_next_malloc = 1;
    rc = cc_tool_executor_execute_with_options(runtime, "lane-session", &call, &options, &result);
    failed |= rc.code != CC_ERR_CANCELLED || result.ok || result.text || result.error;
    fail_next_malloc = 0;
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    cc_cancel_source_destroy(source);

    options.cancel_token = NULL;
    options.deadline_ms = cc_platform_monotonic_ms();
    if (!options.deadline_ms) options.deadline_ms = 1;
    while (cc_platform_monotonic_ms() < options.deadline_ms) { }
    fail_next_malloc = 1;
    rc = cc_tool_executor_execute_with_options(runtime, "lane-session", &call, &options, &result);
    failed |= rc.code != CC_ERR_TIMEOUT || result.ok || result.text || result.error ||
        tools[0].policy_calls != 1 || tools[0].tool_calls != 1;
    fail_next_malloc = 0;
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    return failed;
}
#endif

/**
 * @brief 核对长来源、原始点百分号、同显示组合和普通长工具的全链 lane。
 * @return 全部通过为零，失败为一，先释放代际和 builder，再释放测试拥有名称。
 */
int main(void)
{
    int failed = 1;
    cc_runtime_builder_t *builder = NULL;
    cc_runtime_generation_t *generation = NULL;
    char long_a[330], long_b[330], long_mcp[330], ordinary[330];
    memset(long_a, 'a', 320);
    memcpy(long_a + 320, ".one%", 6);
    memcpy(long_b, long_a, 326);
    memcpy(long_b + 320, ".two%", 6);
    memset(long_mcp, 'm', 320);
    memcpy(long_mcp + 320, ".srv%", 6);
    memset(ordinary, 'n', 320);
    ordinary[20] = '.';
    ordinary[320] = '\0';
    if (!prepare_tool(0, "plugin", long_a, "do.work%") ||
        !prepare_tool(1, "plugin", long_b, "do.work%") ||
        !prepare_tool(2, "plugin", "a.b", "c") ||
        !prepare_tool(3, "plugin", "a", "b.c") ||
        !prepare_tool(4, "mcp", long_mcp, "read.file")) goto cleanup;
    tools[5].name = cc_copy_string(ordinary);
    if (!tools[5].name || !result_is(cc_tool_name_lane(ordinary, &tools[5].expected_lane), CC_OK)) goto cleanup;
    if (strcmp(tools[2].name, "plugin.a%2Eb.c") != 0 ||
        strcmp(tools[3].name, "plugin.a.b%2Ec") != 0 ||
        strcmp(tools[2].name, tools[3].name) == 0 ||
        strlen(tools[0].expected_lane) <= 256 ||
        strcmp(tools[0].expected_lane, tools[1].expected_lane) == 0 ||
        strncmp(tools[5].expected_lane, "tool.", 5) != 0 ||
        strcmp(tools[5].expected_lane + 5, ordinary) != 0) goto cleanup;

    cc_config_plugin_entry_t plugins[4] = {0};
    const char *sources[] = {long_a, long_b, "a.b", "a"};
    for (size_t i = 0; i < 4; i++) {
        plugins[i].id = (char *)sources[i];
        plugins[i].enabled = 1;
        plugins[i].max_in_flight = 1;
        plugins[i].timeout_ms = 1801 + (int)i;
        tools[i].expected_timeout = plugins[i].timeout_ms;
    }
    cc_config_mcp_server_t server = {0};
    server.name = long_mcp;
    server.connection_timeout_ms = tools[4].expected_timeout = 1805;
    tools[5].expected_timeout = 1806;
    cc_config_tool_policy_t ordinary_policy = {tools[5].expected_lane, 1, 1806};
    cc_config_t config = {0};
    config.provider = "fake";
    config.model = "fake";
    config.data_dir = ".";
    config.workspace_path = ".";
    config.plugins.entries = plugins;
    config.plugins.entry_count = 4;
    config.mcp.servers = &server;
    config.mcp.server_count = 1;
    config.tools.policies = &ordinary_policy;
    config.tools.policy_count = 1;
    cc_tool_descriptor_t descriptors[6] = {0};
    for (size_t i = 0; i < sizeof(descriptors) / sizeof(descriptors[0]); i++) {
        descriptors[i].name = tools[i].name;
        descriptors[i].compiled = 1;
        descriptors[i].create = create_tool;
    }
    cc_llm_provider_descriptor_t provider = {"fake", 1, create_provider};
    cc_runtime_feature_set_t features = {0};
    features.llm_providers = &provider;
    features.llm_provider_count = 1;
    features.tools = descriptors;
    features.tool_count = 6;
    features.create_session_store = create_store;
    features.create_policy_engine = create_policy;
    if (!result_is(cc_runtime_builder_create(&config, &features, &builder), CC_OK)) goto cleanup;
    if (!result_is(cc_runtime_builder_acquire_generation(builder, &generation), CC_OK)) goto cleanup;
    failed = verify_lane_execution(cc_runtime_generation_runtime(generation));
#ifdef CC_TEST_TOOL_LANE_ALLOCATION
    if (!failed) failed = verify_allocation_and_prechecks(cc_runtime_generation_runtime(generation));
#endif

cleanup:
    cc_runtime_generation_release(generation);
    if (!result_is(cc_runtime_builder_destroy(builder, 1000), CC_OK)) failed = 1;
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        free(tools[i].name);
        free(tools[i].expected_lane);
    }
    return failed;
}
