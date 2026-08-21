#include "cc/app/cc_agent_runtime.h"
#include "cc/internal/cc_alloc.h"
#include "cc/app/cc_tool_executor.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>
#include <string.h>

/*
 * 函数 cc_memory_session_store_create：声明 cclaw/tests/adapters/test_tool_schema_validation.c 中的 memory session store create 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

/* schema 测试工具名称。 */
static const char *schema_tool_name(void *self)
{
    (void)self;
    return "schema_tool";
}

/* schema 测试工具说明。 */
static const char *schema_tool_description(void *self)
{
    (void)self;
    return "schema validated tool";
}

/*
 * 工具 schema：要求 path/mode，拒绝额外字段，并约束 mode enum。
 */
static const char *schema_tool_schema(void *self)
{
    (void)self;
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

/* 工具真实 call：只有 schema 校验通过时才应被执行。 */
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
    memset(out_result, 0, sizeof(*out_result));
    out_result->ok = 1;
    out_result->text = cc_copy_string("ok");
    return cc_result_ok();
}

/* schema 测试工具 vtable。 */
static cc_tool_vtable_t schema_tool_vtable = {
    schema_tool_name,
    schema_tool_description,
    schema_tool_schema,
    schema_tool_call,
    NULL
};

/* 执行一次工具调用并断言 schema 失败时不会进入 call。 */
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

/*
 * 验证 tool executor 的最小 JSON Schema 校验。
 *
 * 覆盖缺 required、类型错误、enum 错误、additionalProperties=false 和合法参数。
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

    cc_agent_runtime_destroy(runtime);
    if (store.vtable && store.vtable->destroy) store.vtable->destroy(store.self);
    cc_tool_registry_destroy(registry);
    return failed ? 1 : 0;
}
