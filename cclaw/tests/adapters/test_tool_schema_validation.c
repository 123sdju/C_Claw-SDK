#include "cc/app/cc_agent_runtime.h"
#include "cc/internal/cc_alloc.h"
#include "cc/app/cc_tool_executor.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/ports/cc_platform.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>
#include <string.h>

/** @brief 创建测试 RAM 会话端口。 @param[out] out_store 必填空描述符，成功交付测试须销毁的实例。 @return 拥有结果由 caller 释放，失败无新实例。 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

/** @brief 提供参数校验工具名。 @param[in] self 忽略的状态借用。 @return 静态名称。 */
static const char *schema_tool_name(void *self)
{
    (void)self;
    return "schema_tool";
}

/** @brief 提供参数校验说明。 @param[in] self 忽略的状态借用。 @return 静态描述。 */
static const char *schema_tool_description(void *self)
{
    (void)self;
    return "schema validated tool";
}

static const char *schema_override;
static uint64_t error_after_deadline_ms;

/**
 * @brief 返回可注入损坏正文的测试 schema，默认要求 path/mode 并声明整数 count。
 * @param[in] self 测试工具上下文，此回调忽略。
 * @return 借用的覆盖正文或静态 schema，覆盖指针由主测试保持存活。
 */
static const char *schema_tool_schema(void *self)
{
    (void)self;
    if (schema_override) return schema_override;
    return "{"
        "\"type\":\"object\","
        "\"additionalProperties\":false,"
        "\"properties\":{"
            "\"path\":{\"type\":\"string\"},"
            "\"count\":{\"type\":\"integer\"},"
            "\"mode\":{\"type\":\"string\",\"enum\":[\"read\",\"write\"]}"
        "},"
        "\"required\":[\"path\",\"mode\"]"
    "}";
}

/**
 * @brief 记录实际工具派发，或等待指定截止时间后返回带拥有消息的系统错误。
 * @param[in,out] self 指向主测试保管的调用计数，调用时递增。
 * @param[in] args_json 本次借用参数，测试工具忽略正文。
 * @param[in] ctx 本次借用上下文，测试工具忽略。
 * @param[out] out_result 预先为空的结果，普通分支输出拥有的 ok 文本，调用者 cleanup。
 * @return 普通分支为 CC_OK，截止故障分支为 CC_ERR_IO，故障消息用于 sanitizer 核对超时覆盖时的释放。
 */
static cc_result_t schema_tool_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    int *called = (int *)self;
    (void)args_json;
    (void)ctx;
    (*called)++;
    if (error_after_deadline_ms) {
        while (cc_platform_monotonic_ms() < error_after_deadline_ms) { }
        return cc_result_error(CC_ERR_IO, "Tool error returned after its deadline");
    }
    memset(out_result, 0, sizeof(*out_result));
    out_result->ok = 1;
    out_result->text = cc_copy_string("ok");
    return cc_result_ok();
}

/* 主测试同步更改 call 以模拟实现缺失，整个测试期间没有并发读取。 */
static cc_tool_vtable_t schema_tool_vtable = {
    schema_tool_name,
    schema_tool_description,
    schema_tool_schema,
    schema_tool_call,
    NULL
};

/**
 * @brief 无审批地允许测试工具，使场景集中到 schema 与截止检查。
 * @param[in] self 未使用的状态借用，可空。
 * @param[in] call 调用记录借用，忽略。
 * @param[in] ctx 上下文借用，忽略。
 * @param[out] out 新空决策槽，清零后置 allowed，原因保持 NULL。
 * @return CC_OK，决策由执行器清理。
 */
static cc_result_t allow_tool(void *self, const cc_tool_call_t *call,
    const cc_tool_context_t *ctx, cc_policy_decision_t *out)
{
    (void)self; (void)call; (void)ctx;
    memset(out, 0, sizeof(*out));
    out->allowed = 1;
    return cc_result_ok();
}

static const cc_policy_engine_vtable_t allow_policy = { allow_tool, NULL };

/**
 * @brief 经真实 executor 调用一次并检查业务结果及派发次数。
 * @param[in,out] runtime 存活 runtime 借用，同步测试期间不销毁。
 * @param[in] arguments_json 调用期稳定参数文本，构造调用仅浅借用。
 * @param[in] expect_ok 预期业务成功标志。
 * @param[in] called 主测试持有的调用计数，失败场景须仍为零。
 * @return 符合预期为零，任何错误为一，结果与诊断返回前归还。
 */
static int run_call(
    cc_agent_runtime_t *runtime,
    const char *arguments_json,
    int expect_ok,
    int *called
)
{
    cc_tool_call_t call = {
        .id = "call_schema",
        .name = "schema_tool",
        .arguments_json = (char *)arguments_json
    };
    cc_tool_result_t result;
    memset(&result, 0, sizeof(result));
    cc_result_t rc = cc_tool_executor_execute(runtime, "schema_session", &call, &result);
    int failed = 0;
    if (rc.code != CC_OK) failed = 1;
    if (result.ok != expect_ok) failed = 1;
    if (!expect_ok && !result.error) failed = 1;
    if (expect_ok && (!result.text || strcmp(result.text, "ok") != 0)) failed = 1;
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    if (!expect_ok && *called != 0) failed = 1;
    return failed;
}

/**
 * @brief 创建同步测试 runtime，核对 schema 子集、整数表示、完整尾部、缺失实现及执行后超时。
 * @return 全部断言通过为零，创建失败或任一断言失败为一。
 * @note 故障注入只在本进程同步执行期间启用。正常收尾销毁 runtime、store 和 registry，
 * 各次工具结果与返回消息单独清理；截止回归等待约一秒以覆盖工具错误被超时结果替换的释放路径。
 */
int main(void)
{
    cc_tool_registry_t *registry = NULL;
    cc_session_store_t store = {0};
    cc_agent_runtime_t *runtime = NULL;
    int called = 0;
    int failed = 0;

    if (cc_tool_registry_create(&registry).code != CC_OK) return 1;
    cc_tool_t tool = { &called, &schema_tool_vtable };
    if (cc_tool_registry_add(registry, tool).code != CC_OK) return 1;
    cc_tool_registry_freeze(registry);
    if (cc_memory_session_store_create(&store).code != CC_OK) return 1;

    cc_agent_runtime_deps_t deps = {0};
    deps.tool_registry = registry;
    deps.store = store;
    deps.policy.vtable = &allow_policy;
    cc_agent_runtime_options_t options = {0};
    options.config.max_steps = 1;
    options.config.workspace_dir = ".";
    if (cc_agent_runtime_create(&deps, &options, &runtime).code != CC_OK) return 1;

    called = 0;
    failed |= run_call(runtime, "{\"mode\":\"read\"}", 0, &called);
    called = 0;
    failed |= run_call(runtime, "{\"path\":42,\"mode\":\"read\"}", 0, &called);
    called = 0;
    failed |= run_call(runtime, "{\"path\":\"a\",\"mode\":\"delete\"}", 0, &called);
    called = 0;
    failed |= run_call(runtime, "{\"path\":\"a\",\"mode\":\"read\",\"extra\":true}", 0, &called);
    called = 0;
    failed |= run_call(runtime, "{\"path\":\"a\",\"mode\":\"write\",\"count\":2}", 1, &called);
    if (called != 1) failed = 1;

    const char *rejected_integers[] = {
        "{\"path\":\"a\",\"mode\":\"write\",\"count\":2.5}",
        "{\"path\":\"a\",\"mode\":\"write\",\"count\":-2.5}",
        "{\"path\":\"a\",\"mode\":\"write\",\"count\":25e-1}",
        "{\"path\":\"a\",\"mode\":\"write\",\"count\":1e999}"
    };
    for (size_t i = 0; i < sizeof(rejected_integers) / sizeof(rejected_integers[0]); i++) {
        called = 0;
        failed |= run_call(runtime, rejected_integers[i], 0, &called);
    }
    const char *accepted_integers[] = {
        "{\"path\":\"a\",\"mode\":\"write\",\"count\":-2}",
        "{\"path\":\"a\",\"mode\":\"write\",\"count\":2e1}",
        "{\"path\":\"a\",\"mode\":\"write\",\"count\":2147483648}",
        "{\"path\":\"a\",\"mode\":\"write\",\"count\":1e100}"
    };
    for (size_t i = 0; i < sizeof(accepted_integers) / sizeof(accepted_integers[0]); i++) {
        called = 0;
        failed |= run_call(runtime, accepted_integers[i], 1, &called);
        if (called != 1) failed = 1;
    }
    called = 0;
    failed |= run_call(runtime, "{\"path\":\"a\",\"mode\":\"read\"} trailing", 0, &called);
    schema_override = "{";
    failed |= run_call(runtime, "{}", 0, &called);
    schema_override = "{\"type\":\"object\"} trailing";
    failed |= run_call(runtime, "{}", 0, &called);
    schema_override = NULL;

    schema_tool_vtable.call = NULL;
    cc_tool_call_t unavailable = {"missing-call", "schema_tool", "{}"};
    cc_tool_result_t unavailable_result = {0};
    cc_result_t unavailable_rc = cc_tool_executor_execute(runtime, "schema_session", &unavailable, &unavailable_result);
    if (unavailable_rc.code != CC_ERR_TOOL || called != 0) failed = 1;
    cc_result_free(&unavailable_rc); cc_tool_result_cleanup(&unavailable_result);
    schema_tool_vtable.call = schema_tool_call;

    error_after_deadline_ms = cc_platform_monotonic_ms() + 1000U;
    cc_tool_executor_options_t deadline_options = {0}; deadline_options.deadline_ms = error_after_deadline_ms;
    cc_tool_call_t late = {"late-error", "schema_tool", "{\"path\":\"a\",\"mode\":\"read\"}"};
    cc_tool_result_t late_result = {0};
    cc_result_t late_rc = cc_tool_executor_execute_with_options(runtime, "schema_session", &late,
                                                                &deadline_options, &late_result);
    if (late_rc.code != CC_ERR_TIMEOUT || called != 1) failed = 1;
    cc_result_free(&late_rc); cc_tool_result_cleanup(&late_result); error_after_deadline_ms = 0;

    cc_agent_runtime_destroy(runtime);
    if (store.vtable && store.vtable->destroy) store.vtable->destroy(store.self);
    cc_tool_registry_destroy(registry);
    return failed ? 1 : 0;
}
