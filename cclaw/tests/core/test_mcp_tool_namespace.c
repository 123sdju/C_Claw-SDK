#include "cc/app/cc_mcp_runtime_manager.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_json.h"
#include "cc/util/cc_tool_name.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "check failed at %d\n", __LINE__); return 1; } } while (0)

/** @brief 单个 fake server 的原始命名、预期公开名和调用计数，字符串由 main 保活。 */
typedef struct namespace_case {
    const char *server_name;
    const char *raw_name;
    const char *public_name;
    int call_count;
    int raw_name_mismatch;
    int destroy_count;
    int initialize_count;
    int notify_count;
} namespace_case_t;

/** @brief factory 调用期及全部 transport 使用期间借用的 server 对照数组。 */
typedef struct namespace_factory_context {
    namespace_case_t *cases;
    size_t count;
} namespace_factory_context_t;

/**
 * @brief 消费拥有结果并返回成功状态。
 * @param[in] rc 按值移交的结果，原副本的消息随后失效。
 * @return CC_OK 为一，其他为零。
 */
static int ok(cc_result_t rc)
{
    int success = rc.code == CC_OK;
    cc_result_free(&rc);
    return success;
}

/**
 * @brief 生成与请求 ID 对应的 JSON-RPC result envelope。
 * @param[in] id 请求中的数值 ID，本测试仅用小整数。
 * @param[in] result 消费拥有的结果树，创建 envelope 失败时也释放。
 * @return 需 free 的拥有 JSON 文本，分配失败返回 NULL。
 */
static char *response_with_result(double id, cc_json_value_t *result)
{
    cc_json_value_t *root = cc_json_create_object();
    if (!root) {
        cc_json_destroy(result);
        return NULL;
    }
    cc_json_object_set(root, "jsonrpc", cc_json_create_string("2.0"));
    cc_json_object_set(root, "id", cc_json_create_number(id));
    cc_json_object_set(root, "result", result);
    char *text = cc_json_stringify_unformatted(root);
    cc_json_destroy(root);
    return text;
}

/**
 * @brief 生成只含当前 server 原始工具名的 tools/list 结果。
 * @param[in] raw_name 调用期借用原名，复制到 JSON 树。
 * @return 拥有结果树，交 response_with_result 消费。
 */
static cc_json_value_t *tool_list_result(const char *raw_name)
{
    cc_json_value_t *result = cc_json_create_object();
    cc_json_value_t *tools = cc_json_create_array();
    cc_json_value_t *tool = cc_json_create_object();
    cc_json_value_t *schema = cc_json_create_object();
    cc_json_object_set(schema, "type", cc_json_create_string("object"));
    cc_json_object_set(tool, "name", cc_json_create_string(raw_name));
    cc_json_object_set(tool, "description", cc_json_create_string("namespace fixture"));
    cc_json_object_set(tool, "inputSchema", schema);
    cc_json_array_append(tools, tool);
    cc_json_object_set(result, "tools", tools);
    return result;
}

/**
 * @brief 提供初始化、工具发现和执行响应，并核对 tools/call 仍使用原始远端名。
 * @param[in,out] self main 借用的 namespace_case_t，覆盖 transport 寿命。
 * @param[in] request_json 借用 manager 生成的请求文本。
 * @param[in] timeout_ms 毫秒预算，本 fake 同步完成，忽略。
 * @param[in] cancel_token 借用取消 token，本场景不发取消，忽略。
 * @param[out] out_response_json 必填，先置 NULL，成功交付需 free 的拥有响应。
 * @return CC_OK 或请求解析、参数、响应分配错误，拥有请求树在返回前释放。
 */
static cc_result_t namespace_send_json(void *self, const char *request_json, int timeout_ms,
                                      cc_cancel_token_t *cancel_token, char **out_response_json)
{
    (void)timeout_ms;
    (void)cancel_token;
    namespace_case_t *state = self;
    *out_response_json = NULL;
    cc_json_value_t *request = NULL;
    cc_result_t rc = cc_json_parse(request_json, &request);
    if (rc.code != CC_OK) return rc;
    const char *method = cc_json_string_value(cc_json_object_get(request, "method"));
    double id = cc_json_number_value(cc_json_object_get(request, "id"));
    if (method && strcmp(method, "initialize") == 0) {
        state->initialize_count++;
        cc_json_value_t *result = cc_json_create_object();
        cc_json_object_set(result, "protocolVersion", cc_json_create_string("2024-11-05"));
        cc_json_value_t *capabilities = cc_json_create_object();
        cc_json_object_set(capabilities, "tools", cc_json_create_object());
        cc_json_object_set(result, "capabilities", capabilities);
        cc_json_value_t *info = cc_json_create_object();
        cc_json_object_set(info, "name", cc_json_create_string("namespace"));
        cc_json_object_set(info, "version", cc_json_create_string("1"));
        cc_json_object_set(result, "serverInfo", info);
        *out_response_json = response_with_result(id, result);
    } else if (method && strcmp(method, "tools/list") == 0) {
        if (state->notify_count != state->initialize_count) {
            cc_json_destroy(request);
            return cc_result_error(CC_ERR_INVALID_STATE, "Discovery preceded initialized notification");
        }
        *out_response_json = response_with_result(id, tool_list_result(state->raw_name));
    } else if (method && strcmp(method, "tools/call") == 0) {
        cc_json_value_t *params = cc_json_object_get(request, "params");
        const char *raw_name = cc_json_string_value(cc_json_object_get(params, "name"));
        state->call_count++;
        if (!raw_name || strcmp(raw_name, state->raw_name) != 0) state->raw_name_mismatch = 1;
        cc_json_value_t *result = cc_json_create_object();
        cc_json_value_t *content = cc_json_create_array();
        cc_json_value_t *item = cc_json_create_object();
        cc_json_object_set(item, "type", cc_json_create_string("text"));
        cc_json_object_set(item, "text", cc_json_create_string("called"));
        cc_json_array_append(content, item);
        cc_json_object_set(result, "content", content);
        *out_response_json = response_with_result(id, result);
    }
    cc_json_destroy(request);
    if (!*out_response_json) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot create fake MCP response");
    return cc_result_ok();
}

/**
 * @brief 验证初始化通知没有 id，并记录每个 namespace server 的就绪交接。
 * @param[in,out] self 借用 main 保管的 case。
 * @param[in] notification_json 本次借用完整通知，解析树在返回前销毁。
 * @param[in] timeout_ms 正毫秒预算，本同步 fixture 不等待。
 * @param[in] cancel_token 可选借用令牌，本场景忽略。
 * @return 形状和顺序正确为 CC_OK，否则为解析或状态错误。
 */
static cc_result_t namespace_notify(void *self, const char *notification_json, int timeout_ms,
                                    cc_cancel_token_t *cancel_token)
{
    (void)timeout_ms;
    (void)cancel_token;
    namespace_case_t *state = self;
    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse_strict(notification_json, &root);
    if (rc.code != CC_OK) return rc;
    const char *method = cc_json_string_value(cc_json_object_get(root, "method"));
    int valid = method && strcmp(method, "notifications/initialized") == 0 &&
        !cc_json_object_get(root, "id") && state->notify_count + 1 == state->initialize_count;
    cc_json_destroy(root);
    if (!valid) return cc_result_error(CC_ERR_INVALID_STATE, "Invalid initialized notification");
    state->notify_count++;
    return cc_result_ok();
}

/**
 * @brief 提供无动作的 fake reset 入口。
 * @param[in] self 借用 case 状态，本场景忽略。
 * @return 始终 CC_OK。
 */
static cc_result_t namespace_reset(void *self)
{
    (void)self;
    return cc_result_ok();
}

/**
 * @brief 声明 fake transport 可按 manager 的非串行路径使用。
 * @param[in] self 借用 case 状态，忽略。
 * @return 零，本回归顺序调用每个工具。
 */
static int namespace_is_serial(void *self)
{
    (void)self;
    return 0;
}

/**
 * @brief 结束 transport 对 case 的借用，并记录唯一销毁。
 * @param[in,out] self main 拥有的 case，本回调不释放其栈空间或字符串。
 * @post destroy_count 增加，manager 不再使用该 case 的 transport。
 */
static void namespace_destroy(void *self)
{
    namespace_case_t *state = self;
    state->destroy_count++;
}

static const cc_mcp_transport_vtable_t namespace_vtable = {
    namespace_send_json, namespace_reset, namespace_is_serial, namespace_destroy, namespace_notify
};

/**
 * @brief 按完整 server 名选择对应 fake transport，保持来源与原工具名一一对应。
 * @param[in] server_config 调用期间借用配置，按 name 查找 case。
 * @param[out] out_transport 必填空资源槽，成功交付 case self 与静态虚表。
 * @param[in,out] user_data 借用 namespace_factory_context_t，覆盖 manager 和全部 transport 寿命。
 * @return 找到为 CC_OK，未知 server 为 CC_ERR_NOT_FOUND。
 */
static cc_result_t namespace_factory(const cc_config_mcp_server_t *server_config,
                                    cc_mcp_transport_t *out_transport, void *user_data)
{
    namespace_factory_context_t *context = user_data;
    for (size_t i = 0; i < context->count; i++) {
        if (strcmp(context->cases[i].server_name, server_config->name) == 0) {
            out_transport->self = &context->cases[i];
            out_transport->vtable = &namespace_vtable;
            return cc_result_ok();
        }
    }
    return cc_result_error(CC_ERR_NOT_FOUND, "Unknown namespace fixture server");
}

/**
 * @brief 注册并调用冲突来源、原百分号序列、长组件和普通名称，逐个核对远端原名。
 * @return 全部工具可独立查找调用且 transport 各销毁一次为零，任一检查失败一。
 * @note registry 先释放工具，再销毁 manager，最后释放配置及预期字符串，覆盖全部借用寿命。
 */
int main(void)
{
    char long_source[301];
    char long_tool[401];
    memset(long_source, 's', sizeof(long_source) - 1U);
    memset(long_tool, 't', sizeof(long_tool) - 1U);
    long_source[149] = '.';
    long_source[200] = '%';
    long_tool[149] = '.';
    long_tool[200] = '%';
    long_source[sizeof(long_source) - 1U] = '\0';
    long_tool[sizeof(long_tool) - 1U] = '\0';
    char *long_public_name = NULL;
    CHECK(ok(cc_tool_name_build("mcp", long_source, long_tool, &long_public_name)));
    CHECK(strlen(long_public_name) > 700U);
    namespace_case_t cases[] = {
        { .server_name = "a", .raw_name = "b.c", .public_name = "mcp.a.b%2Ec" },
        { .server_name = "a.b", .raw_name = "c", .public_name = "mcp.a%2Eb.c" },
        { .server_name = "a%2Eb", .raw_name = "c", .public_name = "mcp.a%252Eb.c" },
        { .server_name = "plain", .raw_name = "echo", .public_name = "mcp.plain.echo" },
        { .server_name = "raw-percent", .raw_name = "%2E", .public_name = "mcp.raw-percent.%252E" },
        { .server_name = "raw-dot", .raw_name = ".", .public_name = "mcp.raw-dot.%2E" },
        { .server_name = long_source, .raw_name = long_tool, .public_name = long_public_name },
    };
    namespace_factory_context_t context = { cases, sizeof(cases) / sizeof(cases[0]) };
    cc_config_t config = {0};
    config.mcp.enabled = 1;
    config.mcp.server_count = context.count;
    config.mcp.servers = calloc(context.count, sizeof(*config.mcp.servers));
    CHECK(config.mcp.servers);
    for (size_t i = 0; i < context.count; i++) {
        config.mcp.servers[i].name = cc_copy_string(cases[i].server_name);
        config.mcp.servers[i].transport = cc_copy_string("stdio");
        config.mcp.servers[i].command = cc_copy_string("fake");
        CHECK(config.mcp.servers[i].name && config.mcp.servers[i].transport && config.mcp.servers[i].command);
    }
    cc_tool_registry_t *registry = NULL;
    cc_mcp_runtime_manager_t *manager = NULL;
    cc_runtime_diagnostics_t diagnostics;
    cc_runtime_diagnostics_reset(&diagnostics);
    CHECK(ok(cc_tool_registry_create(&registry)));
    CHECK(ok(cc_mcp_runtime_manager_create(namespace_factory, &context, &manager)));
    CHECK(ok(cc_mcp_runtime_manager_load_tools(manager, &config, registry, &diagnostics)));
    CHECK(diagnostics.count == 0 && cc_tool_registry_count(registry) == context.count);
    for (size_t i = 0; i < context.count; i++) {
        CHECK(cases[i].initialize_count == 1 && cases[i].notify_count == 1);
        cc_tool_t tool;
        CHECK(ok(cc_tool_registry_find(registry, cases[i].public_name, &tool)));
        CHECK(strcmp(tool.vtable->name(tool.self), cases[i].public_name) == 0);
        cc_tool_result_t result = {0};
        cc_tool_context_t tool_context = {0};
        CHECK(ok(tool.vtable->call(tool.self, "{}", &tool_context, &result)));
        CHECK(result.ok && result.text && cases[i].call_count == 1 && !cases[i].raw_name_mismatch);
        cc_tool_result_cleanup(&result);
    }
    cc_tool_registry_destroy(registry);
    cc_mcp_runtime_manager_destroy(manager);
    for (size_t i = 0; i < context.count; i++) CHECK(cases[i].destroy_count == 1);
    cc_config_destroy(&config);
    free(long_public_name);
    return 0;
}
