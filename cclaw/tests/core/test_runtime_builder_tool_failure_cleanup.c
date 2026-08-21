#include "cc/app/cc_runtime_builder.h"

#include <stdlib.h>
#include <string.h>

static int g_tool_destroy_count;
static int g_llm_destroy_count;

/*
 * 函数 test_tool_name：实现 cclaw/tests/core/test_runtime_builder_tool_failure_cleanup.c 中的 test tool name 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
static const char *test_tool_name(void *self)
{
    (void)self;
    return "test.ok";
}

/*
 * 函数 test_tool_description：实现 cclaw/tests/core/test_runtime_builder_tool_failure_cleanup.c 中的 test tool description 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
static const char *test_tool_description(void *self)
{
    (void)self;
    return "test tool";
}

/*
 * 函数 test_tool_schema：实现 cclaw/tests/core/test_runtime_builder_tool_failure_cleanup.c 中的 test tool schema 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
static const char *test_tool_schema(void *self)
{
    (void)self;
    return "{\"type\":\"object\",\"properties\":{}}";
}

/*
 * 函数 test_tool_call：实现 cclaw/tests/core/test_runtime_builder_tool_failure_cleanup.c 中的 test tool call 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
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

/*
 * 函数 test_tool_destroy：实现 cclaw/tests/core/test_runtime_builder_tool_failure_cleanup.c 中的 test tool destroy 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
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

/*
 * 函数 create_success_tool：实现 cclaw/tests/core/test_runtime_builder_tool_failure_cleanup.c 中的 create success tool 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
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

/*
 * 函数 create_failing_tool：实现 cclaw/tests/core/test_runtime_builder_tool_failure_cleanup.c 中的 create failing tool 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
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

/*
 * 函数 fake_llm_destroy：实现 cclaw/tests/core/test_runtime_builder_tool_failure_cleanup.c 中的 fake llm destroy 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
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

/*
 * 函数 create_fake_llm：实现 cclaw/tests/core/test_runtime_builder_tool_failure_cleanup.c 中的 create fake llm 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
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

/*
 * 函数 create_fake_store：实现 cclaw/tests/core/test_runtime_builder_tool_failure_cleanup.c 中的 create fake store 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
static cc_result_t create_fake_store(
    const cc_config_t *config,
    cc_session_store_t *out_store
)
{
    (void)config;
    if (!out_store) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null store output");
    memset(out_store, 0, sizeof(*out_store));
    return cc_result_ok();
}

/*
 * 函数 create_fake_policy：实现 cclaw/tests/core/test_runtime_builder_tool_failure_cleanup.c 中的 create fake policy 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
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

/*
 * 函数 main：实现 cclaw/tests/core/test_runtime_builder_tool_failure_cleanup.c 中的 main 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
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

    cc_runtime_builder_t *builder = NULL;
    cc_result_t rc = cc_runtime_builder_create(&config, &features, &builder);
    int failed = 0;
    if (rc.code == CC_OK || builder != NULL) failed = 1;
    if (g_tool_destroy_count != 1) failed = 1;
    if (g_llm_destroy_count != 1) failed = 1;
    cc_result_free(&rc);
    cc_result_t destroy_rc = cc_runtime_builder_destroy(builder, 1000);
    cc_result_free(&destroy_rc);
    return failed ? 1 : 0;
}
