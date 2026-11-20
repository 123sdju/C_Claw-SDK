

#include "cc/app/cc_tool_executor.h"
#include "cc/adapters/cc_default_policy_engine.h"
#include "cc/internal/cc_alloc.h"
#include "cc/app/cc_agent_runtime.h"
#include "cc/internal/cc_alloc.h"
#include "cc/core/cc_observability.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_policy_engine.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/ports/cc_platform.h"
#include "../../core/vendor/cjson/cJSON.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#if defined(CC_TEST_EXECUTOR_DISPATCH_CLOCK)
static int dispatch_clock_enabled;
static uint64_t dispatch_clock_ms;
static int dispatched_timeout_ms;
static void *watched_policy_diagnostic;
static int policy_diagnostics_freed;

/** @brief 声明链接包装器转交的真实单调时钟。 @return 当前端口的毫秒读数。 */
extern uint64_t __real_cc_platform_monotonic_ms(void);
/** @brief 声明链接包装器转交的真实释放。 @param[in,out] allocation 可空拥有分配。 @post 分配已归还。 */
extern void __real_free(void *allocation);

/** @brief 仅在预算回归期间提供手动推进的时钟。 @return 测试毫秒或真实端口读数。 */
uint64_t __wrap_cc_platform_monotonic_ms(void)
{
    return dispatch_clock_enabled ? dispatch_clock_ms : __real_cc_platform_monotonic_ms();
}

/**
 * @brief 观察成功策略诊断的释放，并继续归还全部真实分配。
 * @param[in,out] allocation 可空拥有分配，匹配被观察指针时只累计一次。
 * @post 内存已归还，被观察诊断已清除，后续地址复用不会重复计数。
 */
void __wrap_free(void *allocation)
{
    if (allocation && allocation == watched_policy_diagnostic) {
        watched_policy_diagnostic = NULL;
        policy_diagnostics_freed++;
    }
    __real_free(allocation);
}
#endif


/** @brief 创建测试使用的 RAM 会话端口。 @param[out] out_store 必填空描述符，成功 self 由测试销毁。 @return 拥有结果由 caller 释放，失败无新实例。 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

typedef struct event_capture {
    int approval_required;
    int approval_denied;
    int approval_approved;
    int tool_finish;
    int tool_error;
    int schema_payload_seen;
    cc_agent_runtime_t *runtime;
    int switch_approval;
    int paired_approvals;
} event_capture_t;

/**
 * @brief 使用事件安装的新上下文同意审批，并重入 setter 清除回调以检查锁外调用。
 * @param[in] tool_name 同步借用的工具名，此测试忽略。
 * @param[in] arguments_json 同步借用的参数，此测试忽略。
 * @param[in] reason 同步借用的策略原因，此测试忽略。
 * @param[in,out] user_data 必填的 event_capture_t，来自配对安装的上下文，runtime 在调用期间存活。
 * @return 返回一允许执行，并记录一次配对回调。
 */
static int paired_approval(const char *tool_name, const char *arguments_json,
                            const char *reason, void *user_data)
{
    (void)tool_name; (void)arguments_json; (void)reason;
    event_capture_t *capture = user_data;
    capture->paired_approvals++;
    cc_agent_runtime_set_tool_approval(capture->runtime, NULL, NULL);
    return 1;
}

/**
 * @brief 捕获审批及工具事件，可在 required 事件中安装新审批函数与上下文。
 * @param[in] event_type 同步借用的事件名称。
 * @param[in] event_json 同步借用的事件 JSON，检查统一 schema 标识。
 * @param[in,out] user_data 栈上保管的事件捕获器，持续覆盖订阅寿命。
 * @post 计数已更新；switch_approval 为真且出现 required 时先清标志再调用 setter，当前审批应使用完整新配对。
 */
static void on_event(const char *event_type, const char *event_json, void *user_data)
{
    event_capture_t *capture = (event_capture_t *)user_data;
    if (!capture) return;
    if (strcmp(event_type, CC_OBS_EVENT_APPROVAL_REQUIRED) == 0) {
        capture->approval_required++;
        if (capture->switch_approval) {
            capture->switch_approval = 0;
            cc_agent_runtime_set_tool_approval(capture->runtime, paired_approval, capture);
        }
    }
    if (strcmp(event_type, CC_OBS_EVENT_APPROVAL_DENIED) == 0) capture->approval_denied++;
    if (strcmp(event_type, CC_OBS_EVENT_APPROVAL_APPROVED) == 0) capture->approval_approved++;
    if (strcmp(event_type, CC_OBS_EVENT_TOOL_FINISH) == 0) capture->tool_finish++;
    if (strcmp(event_type, CC_OBS_EVENT_TOOL_ERROR) == 0) capture->tool_error++;
    if (event_json && strstr(event_json, "\"schema_version\":1")) capture->schema_payload_seen++;
}

/** @brief 提供命中默认 shell 规则的名称。 @param[in] self 忽略的状态借用。 @return 静态名称。 */
static const char *fake_name(void *self)
{
    (void)self;
    return "shell_run";
}

/** @brief 提供测试描述。 @param[in] self 忽略的状态借用。 @return 静态描述。 */
static const char *fake_description(void *self)
{
    (void)self;
    return "fake shell tool";
}

/** @brief 提供跳过参数子集校验的空 schema。 @param[in] self 忽略的状态借用。 @return 静态对象文本。 */
static const char *fake_schema(void *self)
{
    (void)self;
    return "{}";
}

/**
 * @brief 记录真实派发并创建成功正文，审批拒绝时须保持调用次数不变。
 * @param[in,out] self 测试栈上调用次数借用，调用时增加。
 * @param[in] args_json 调用期借用参数，当前忽略。
 * @param[in] ctx 调用期借用上下文，时钟回归启用时记录实际派发的毫秒预算。
 * @param[out] out_result 新空结果槽，成功副本交测试 cleanup。
 * @return CC_OK，正文分配是否成功由主测试断言，返回诊断由测试释放。
 */
static cc_result_t fake_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    (void)args_json;
    (void)ctx;
#if defined(CC_TEST_EXECUTOR_DISPATCH_CLOCK)
    if (dispatch_clock_enabled) dispatched_timeout_ms = ctx->timeout_ms;
#endif
    int *called = (int *)self;
    (*called)++;
    memset(out_result, 0, sizeof(*out_result));
    out_result->ok = 1;
    out_result->text = cc_copy_string("called");
    return cc_result_ok();
}

/* 方法表与栈状态覆盖 runtime 和 registry 的测试寿命。 */
static cc_tool_vtable_t fake_vtable = {
    fake_name,
    fake_description,
    fake_schema,
    fake_call,
    NULL
};

/**
 * @brief 记录一次审批并同意。
 * @param[in] tool_name 调用期名称借用，忽略。
 * @param[in] arguments_json 调用期参数借用，忽略。
 * @param[in] reason 调用期原因借用，忽略。
 * @param[in,out] user_data 可空测试次数槽，非空时增加。
 * @return 固定一，不交接输入资源。
 */
static int approval_approve(
    const char *tool_name,
    const char *arguments_json,
    const char *reason,
    void *user_data
)
{
    int *seen = (int *)user_data;
    (void)tool_name;
    (void)arguments_json;
    (void)reason;
    if (seen) (*seen)++;
    return 1;
}

/**
 * @brief 记录一次审批并拒绝。
 * @param[in] tool_name 调用期名称借用，忽略。
 * @param[in] arguments_json 调用期参数借用，忽略。
 * @param[in] reason 调用期原因借用，忽略。
 * @param[in,out] user_data 可空测试次数槽，非空时增加。
 * @return 固定零，不执行工具。
 */
static int approval_deny(
    const char *tool_name,
    const char *arguments_json,
    const char *reason,
    void *user_data
)
{
    int *seen = (int *)user_data;
    (void)tool_name;
    (void)arguments_json;
    (void)reason;
    if (seen) (*seen)++;
    return 0;
}

/** @brief 归还一次工具结果字段。 @param[in,out] result 可空独占初始化结果。 @post 嵌套拥有字段归还且结果清零，容器留归测试。 */
static void clear_tool_result(cc_tool_result_t *result)
{
    cc_tool_result_cleanup(result);
}

static int json_allocation_calls, json_live_allocations;

/**
 * @brief 在第四次 cJSON 分配时失败，命中首个工具事件字段的名称分配。
 * @param[in] size 本次申请字节数，其余分配转交 malloc 并累计活跃数。
 * @return 第四次为 NULL，其余返回拥有的分配，交由配套 release_json_allocation 释放。
 */
static void *fail_json_attribute_key(size_t size)
{
    json_allocation_calls++;
    if (json_allocation_calls == 4) return NULL;
    void *allocation = malloc(size);
    if (allocation) json_live_allocations++;
    return allocation;
}

/**
 * @brief 释放故障钩子取得的 cJSON 内存并归还活跃计数。
 * @param[in,out] allocation 此次钩子区间内拥有的分配，允许 NULL。
 * @post 非空内存已释放，活跃计数减一，供回归核对未挂接子节点的清理。
 */
static void release_json_allocation(void *allocation)
{
    if (allocation) json_live_allocations--;
    free(allocation);
}

#if defined(CC_TEST_EXECUTOR_DISPATCH_CLOCK)
typedef struct dispatch_timing {
    uint64_t approval_ms; /**< 同步审批消耗的模拟毫秒。 */
    uint64_t start_ms; /**< 同步开始事件消耗的模拟毫秒。 */
} dispatch_timing_t;

/**
 * @brief 允许工具但要求审批，并返回需由执行器释放的成功诊断。
 * @param[in,out] self 忽略的测试状态借用。
 * @param[in] call 调用期工具记录借用，此策略不读取。
 * @param[in] ctx 调用期上下文借用，此策略不读取。
 * @param[out] decision 必填空决策，发布允许及审批标志，无拥有原因。
 * @return 拥有的 CC_OK 诊断交执行器，测试观察其在后续步骤前被释放。
 */
static cc_result_t timed_policy(void *self, const cc_tool_call_t *call,
    const cc_tool_context_t *ctx, cc_policy_decision_t *decision)
{
    (void)self; (void)call; (void)ctx;
    memset(decision, 0, sizeof(*decision));
    decision->allowed = 1;
    decision->require_approval = 1;
    cc_result_t rc = cc_result_error(CC_OK, "owned policy success");
    assert(rc.message);
    watched_policy_diagnostic = rc.message;
    return rc;
}

/**
 * @brief 同意审批并推进确定性时钟。
 * @param[in] name 调用期借用工具名，忽略。
 * @param[in] arguments 调用期借用参数，忽略。
 * @param[in] reason 调用期借用审批原因，忽略。
 * @param[in,out] user_data 必填计时记录，覆盖同步审批寿命。
 * @return 固定一，后续派发须扣除 approval_ms。
 */
static int timed_approval(const char *name, const char *arguments,
    const char *reason, void *user_data)
{
    (void)name; (void)arguments; (void)reason;
    dispatch_timing_t *timing = user_data;
    dispatch_clock_ms += timing->approval_ms;
    return 1;
}

/**
 * @brief 在同步工具开始事件中模拟耗时。
 * @param[in] type 事件名借用，仅 TOOL_START 推进时钟。
 * @param[in] json 事件正文借用，忽略。
 * @param[in,out] user_data 必填计时记录，覆盖订阅寿命。
 * @post 开始事件返回时已计入 start_ms，其他事件不推进时钟。
 */
static void timed_start_event(const char *type, const char *json, void *user_data)
{
    (void)json;
    dispatch_timing_t *timing = user_data;
    if (strcmp(type, CC_OBS_EVENT_TOOL_START) == 0)
        dispatch_clock_ms += timing->start_ms;
}

/**
 * @brief 验证审批及开始事件耗时收紧派发预算，耗尽时不进入工具，并消费成功策略诊断。
 * @post 所有断言通过后关闭假时钟，销毁独立 runtime、总线、store 和 registry，栈借用全部退出。
 * @note 此同步 fixture 使用 100 毫秒截止窗口，分别消耗 30+20 和 90+20 毫秒，无墙钟等待。
 */
static void test_dispatch_budget(void)
{
    cc_tool_registry_t *registry = NULL;
    cc_session_store_t store = {0};
    cc_event_bus_t *bus = NULL;
    cc_agent_runtime_t *runtime = NULL;
    int called = 0;
    dispatch_timing_t timing = {30, 20};
    cc_policy_engine_vtable_t policy_vtable = {timed_policy, NULL};
    cc_result_t rc = cc_tool_registry_create(&registry);
    assert(rc.code == CC_OK); cc_result_free(&rc);
    cc_tool_t tool = {&called, &fake_vtable};
    rc = cc_tool_registry_add(registry, tool);
    assert(rc.code == CC_OK); cc_result_free(&rc);
    cc_tool_registry_freeze(registry);
    rc = cc_memory_session_store_create(&store);
    assert(rc.code == CC_OK); cc_result_free(&rc);
    rc = cc_event_bus_create(&bus);
    assert(rc.code == CC_OK); cc_result_free(&rc);
    cc_event_subscription_t subscription = {0};
    rc = cc_event_bus_subscribe(bus, NULL, timed_start_event, &timing, NULL, &subscription);
    assert(rc.code == CC_OK); cc_result_free(&rc);
    cc_agent_runtime_deps_t deps = {0};
    deps.tool_registry = registry;
    deps.store = store;
    deps.event_bus = bus;
    deps.policy.self = &timing;
    deps.policy.vtable = &policy_vtable;
    cc_agent_runtime_options_t options = {0};
    options.config.max_steps = 1;
    options.config.workspace_dir = ".";
    options.config.limits.tool_timeout_ms = 1000;
    rc = cc_agent_runtime_create(&deps, &options, &runtime);
    assert(rc.code == CC_OK); cc_result_free(&rc);
    cc_agent_runtime_set_tool_approval(runtime, timed_approval, &timing);
    cc_tool_call_t call = {"call_budget", "shell_run", "{}"};
    cc_tool_result_t result = {0};
    cc_tool_executor_options_t execution = {0};
    policy_diagnostics_freed = 0;
    dispatch_clock_enabled = 1;
    dispatch_clock_ms = 1000;
    execution.deadline_ms = 1100;
    rc = cc_tool_executor_execute_with_options(runtime, "ses_budget", &call, &execution, &result);
    assert(rc.code == CC_OK && result.ok && called == 1 && dispatched_timeout_ms == 50);
    assert(policy_diagnostics_freed == 1 && !watched_policy_diagnostic);
    cc_result_free(&rc); cc_tool_result_cleanup(&result);
    timing.approval_ms = 90;
    dispatch_clock_ms = 2000;
    execution.deadline_ms = 2100;
    rc = cc_tool_executor_execute_with_options(runtime, "ses_budget", &call, &execution, &result);
    assert(rc.code == CC_ERR_TIMEOUT && called == 1 && !result.ok && !result.text);
    assert(policy_diagnostics_freed == 2 && !watched_policy_diagnostic);
    cc_result_free(&rc); cc_tool_result_cleanup(&result);
    dispatch_clock_enabled = 0;
    cc_agent_runtime_destroy(runtime);
    cc_event_bus_destroy(bus);
    store.vtable->destroy(store.self);
    cc_tool_registry_destroy(registry);
}
#endif

/**
 * @brief 构造带事件总线的同步 runtime，核对审批拒绝/允许、事件切换配对及属性挂接失败清理。
 * @return 全部断言通过为零，创建失败或任一断言失败为一。
 * @note 栈上工具计数与审批上下文覆盖 runtime 寿命。cJSON 全局分配钩子只在最后一次同步调用
 * 期间安装，并在断言前恢复；正常收尾清理结果，依次销毁 runtime、总线、store、policy 和 registry。
 */
int main(void)
{
    cc_tool_registry_t *registry = NULL;
    cc_policy_engine_t policy = {0};
    cc_session_store_t store = {0};
    cc_event_bus_t *bus = NULL;
    cc_agent_runtime_t *runtime = NULL;
    cc_tool_result_t result;
    int called = 0;
    int approvals = 0;
    int failed = 0;
    event_capture_t events = {0};

    memset(&result, 0, sizeof(result));

    if (cc_tool_registry_create(&registry).code != CC_OK) return 1;

    cc_tool_t tool = { &called, &fake_vtable };
    if (cc_tool_registry_add(registry, tool).code != CC_OK) {
        cc_tool_registry_destroy(registry);
        return 1;
    }
    cc_tool_registry_freeze(registry);

    if (cc_policy_engine_create_default(1, &policy).code != CC_OK) {
        cc_tool_registry_destroy(registry);
        return 1;
    }
    if (cc_memory_session_store_create(&store).code != CC_OK) {
        if (policy.vtable && policy.vtable->destroy) policy.vtable->destroy(policy.self);
        cc_tool_registry_destroy(registry);
        return 1;
    }
    if (cc_event_bus_create(&bus).code != CC_OK) {
        store.vtable->destroy(store.self);
        if (policy.vtable && policy.vtable->destroy) policy.vtable->destroy(policy.self);
        cc_tool_registry_destroy(registry);
        return 1;
    }
    cc_event_subscription_t event_subscription = {0};
    cc_event_bus_subscribe(bus, NULL, on_event, &events, NULL, &event_subscription);

    cc_agent_runtime_deps_t deps;
    memset(&deps, 0, sizeof(deps));
    deps.tool_registry = registry;
    deps.policy = policy;
    deps.store = store;
    deps.event_bus = bus;
    cc_agent_runtime_options_t options;
    memset(&options, 0, sizeof(options));
    options.config.max_steps = 1;
    options.config.workspace_dir = ".";
    if (cc_agent_runtime_create(&deps, &options, &runtime).code != CC_OK) {
        cc_event_bus_destroy(bus);
        store.vtable->destroy(store.self);
        if (policy.vtable && policy.vtable->destroy) policy.vtable->destroy(policy.self);
        cc_tool_registry_destroy(registry);
        return 1;
    }
    events.runtime = runtime;

    cc_tool_call_t call = {
        .id = "call_approval",
        .name = "shell_run",
        .arguments_json = "{\"command\":\"echo should-not-run\"}"
    };

    cc_result_t rc = cc_tool_executor_execute(runtime, "ses_approval", &call, &result);
    if (rc.code != CC_OK) failed = 1;
    if (result.ok != 0) failed = 1;
    if (!result.error || !strstr(result.error, "approval")) failed = 1;
    if (called != 0) failed = 1;
    if (events.approval_required != 1 || events.approval_denied != 1) failed = 1;
    cc_result_free(&rc);
    clear_tool_result(&result);

    cc_agent_runtime_set_tool_approval(runtime, approval_deny, &approvals);
    rc = cc_tool_executor_execute(runtime, "ses_approval", &call, &result);
    if (rc.code != CC_OK) failed = 1;
    if (result.ok != 0) failed = 1;
    if (!result.error || !strstr(result.error, "denied")) failed = 1;
    if (called != 0) failed = 1;
    if (approvals != 1) failed = 1;
    if (events.approval_required != 2 || events.approval_denied != 2) failed = 1;
    cc_result_free(&rc);
    clear_tool_result(&result);

    cc_agent_runtime_set_tool_approval(runtime, approval_approve, &approvals);
    rc = cc_tool_executor_execute(runtime, "ses_approval", &call, &result);
    if (rc.code != CC_OK) failed = 1;
    if (result.ok != 1) failed = 1;
    if (!result.text || strcmp(result.text, "called") != 0) failed = 1;
    if (called != 1) failed = 1;
    if (approvals != 2) failed = 1;
    if (events.approval_required != 3 || events.approval_approved != 1 ||
        events.tool_finish != 1) failed = 1;
    cc_result_free(&rc);
    clear_tool_result(&result);

    cc_tool_call_t missing_call = {
        .id = "call_missing",
        .name = "missing_tool",
        .arguments_json = "{}"
    };
    rc = cc_tool_executor_execute(runtime, "ses_approval", &missing_call, &result);
    if (rc.code != CC_OK) failed = 1;
    if (result.ok != 0) failed = 1;
    if (!result.error || !strstr(result.error, "Tool not found: missing_tool")) failed = 1;
    if (called != 1) failed = 1;
    if (events.tool_error < 1 || events.schema_payload_seen < 1) failed = 1;
    cc_result_free(&rc);

    clear_tool_result(&result);
    cc_agent_runtime_set_tool_approval(runtime, approval_deny, &approvals);
    events.switch_approval = 1;
    rc = cc_tool_executor_execute(runtime, "ses_approval", &call, &result);
    if (rc.code != CC_OK || !result.ok || called != 2 || events.switch_approval ||
        events.paired_approvals != 1 || approvals != 2) failed = 1;
    cc_result_free(&rc); clear_tool_result(&result);

    json_allocation_calls = 0; json_live_allocations = 0;
    cJSON_Hooks hooks = {fail_json_attribute_key, release_json_allocation};
    cJSON_InitHooks(&hooks);
    rc = cc_tool_executor_execute(runtime, "ses_approval", &missing_call, &result);
    cJSON_InitHooks(NULL);
    if (rc.code != CC_OK || result.ok || !result.error || json_allocation_calls != 4 ||
        json_live_allocations != 0 || called != 2) failed = 1;
    cc_result_free(&rc);

    cc_agent_runtime_destroy(runtime);
    clear_tool_result(&result);
    cc_event_bus_destroy(bus);
    store.vtable->destroy(store.self);
    if (policy.vtable && policy.vtable->destroy) policy.vtable->destroy(policy.self);
    cc_tool_registry_destroy(registry);
#if defined(CC_TEST_EXECUTOR_DISPATCH_CLOCK)
    test_dispatch_budget();
#endif
    return failed ? 1 : 0;
}
