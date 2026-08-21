

#include "cc/app/cc_tool_executor.h"
#include "cc/internal/cc_alloc.h"
#include "cc/app/cc_agent_runtime.h"
#include "cc/internal/cc_alloc.h"
#include "cc/core/cc_observability.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_policy_engine.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>
#include <string.h>


/*
 * 函数 cc_policy_engine_create_default：声明 cclaw/tests/adapters/test_tool_executor_policy_approval.c 中的 policy engine create default 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
extern cc_result_t cc_policy_engine_create_default(
    int shell_requires_approval,
    cc_policy_engine_t *out_engine
);

/*
 * 函数 cc_memory_session_store_create：声明 cclaw/tests/adapters/test_tool_executor_policy_approval.c 中的 memory session store create 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

typedef struct event_capture {
    int approval_required;
    int approval_denied;
    int approval_approved;
    int tool_finish;
    int tool_error;
    int schema_payload_seen;
} event_capture_t;

/* 捕获审批和工具事件，确认 tool executor 已经迁移到统一 observability schema。 */
static void on_event(const char *event_type, const char *event_json, void *user_data)
{
    event_capture_t *capture = (event_capture_t *)user_data;
    if (!capture) return;
    if (strcmp(event_type, CC_OBS_EVENT_APPROVAL_REQUIRED) == 0) capture->approval_required++;
    if (strcmp(event_type, CC_OBS_EVENT_APPROVAL_DENIED) == 0) capture->approval_denied++;
    if (strcmp(event_type, CC_OBS_EVENT_APPROVAL_APPROVED) == 0) capture->approval_approved++;
    if (strcmp(event_type, CC_OBS_EVENT_TOOL_FINISH) == 0) capture->tool_finish++;
    if (strcmp(event_type, CC_OBS_EVENT_TOOL_ERROR) == 0) capture->tool_error++;
    if (event_json && strstr(event_json, "\"schema_version\":1")) capture->schema_payload_seen++;
}

/* fake shell 工具名，触发默认 policy 的 shell 审批规则。 */
static const char *fake_name(void *self)
{
    (void)self;
    return "shell_run";
}

/* fake 工具说明。 */
static const char *fake_description(void *self)
{
    (void)self;
    return "fake shell tool";
}

/* fake 工具 schema，此测试关注 approval，不关注参数校验。 */
static const char *fake_schema(void *self)
{
    (void)self;
    return "{}";
}

/* fake 工具调用：只有审批通过时才应增加 called 并返回 called 文本。 */
static cc_result_t fake_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    (void)args_json;
    (void)ctx;
    int *called = (int *)self;
    (*called)++;
    memset(out_result, 0, sizeof(*out_result));
    out_result->ok = 1;
    out_result->text = cc_copy_string("called");
    return cc_result_ok();
}

/* fake shell 工具 vtable。 */
static cc_tool_vtable_t fake_vtable = {
    fake_name,
    fake_description,
    fake_schema,
    fake_call,
    NULL
};

/* 审批回调：记录调用次数并允许执行。 */
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

/* 审批回调：记录调用次数并拒绝执行。 */
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

/* 清理工具结果，封装成 helper 让测试主流程更清晰。 */
static void clear_tool_result(cc_tool_result_t *result)
{
    cc_tool_result_cleanup(result);
}

/*
 * 验证 tool executor 的 policy/approval 契约。
 *
 * 覆盖无 approval handler 默认拒绝、handler deny、handler approve、缺失工具错误，以及
 * approval/tool observability 事件 schema。
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

    cc_agent_runtime_destroy(runtime);
    clear_tool_result(&result);
    cc_event_bus_destroy(bus);
    store.vtable->destroy(store.self);
    if (policy.vtable && policy.vtable->destroy) policy.vtable->destroy(policy.self);
    cc_tool_registry_destroy(registry);
    return failed ? 1 : 0;
}
