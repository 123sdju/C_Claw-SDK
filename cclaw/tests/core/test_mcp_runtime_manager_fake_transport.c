#include "cc/app/cc_mcp_runtime_manager.h"
#include "cc/internal/cc_alloc.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_config.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* fake MCP transport 状态，记录 initialize/list/call/reset 调用次数。 */
typedef struct fake_transport {
    int initialize_count;
    int list_count;
    int call_count;
    int reset_count;
} fake_transport_t;

/* 构造带 result 的 JSON-RPC 响应，result 所有权转移给 root。 */
static char *response_with_result(double id, cc_json_value_t *result)
{
    cc_json_value_t *root = cc_json_create_object();
    cc_json_object_set(root, "jsonrpc", cc_json_create_string("2.0"));
    cc_json_object_set(root, "id", cc_json_create_number(id));
    cc_json_object_set(root, "result", result);
    char *json = cc_json_stringify_unformatted(root);
    cc_json_destroy(root);
    return json;
}

/* 构造 tools/list 响应中的单个 echo 工具描述。 */
static cc_json_value_t *tool_list_result(void)
{
    cc_json_value_t *result = cc_json_create_object();
    cc_json_value_t *tools = cc_json_create_array();
    cc_json_value_t *tool = cc_json_create_object();
    cc_json_object_set(tool, "name", cc_json_create_string("echo"));
    cc_json_object_set(tool, "description", cc_json_create_string("Echo through fake MCP"));
    cc_json_value_t *schema = cc_json_create_object();
    cc_json_object_set(schema, "type", cc_json_create_string("object"));
    cc_json_object_set(tool, "inputSchema", schema);
    cc_json_array_append(tools, tool);
    cc_json_object_set(result, "tools", tools);
    return result;
}

/*
 * fake transport 的 send_json。
 *
 * 根据 method 返回 initialize、tools/list 或 tools/call 响应，用于测试 manager 不依赖真实
 * 子进程/MCP server。
 */
static cc_result_t fake_send_json(
    void *self,
    const char *request_json,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    char **out_response_json
)
{
    (void)timeout_ms;
    (void)cancel_token;
    fake_transport_t *transport = (fake_transport_t *)self;
    *out_response_json = NULL;

    cc_json_value_t *request = NULL;
    cc_result_t rc = cc_json_parse(request_json, &request);
    if (rc.code != CC_OK) return rc;
    double id = cc_json_number_value(cc_json_object_get(request, "id"));
    const char *method = cc_json_string_value(cc_json_object_get(request, "method"));

    if (strcmp(method, "initialize") == 0) {
        transport->initialize_count++;
        cc_json_value_t *result = cc_json_create_object();
        cc_json_object_set(result, "protocolVersion", cc_json_create_string("2024-11-05"));
        *out_response_json = response_with_result(id, result);
    } else if (strcmp(method, "tools/list") == 0) {
        transport->list_count++;
        *out_response_json = response_with_result(id, tool_list_result());
    } else if (strcmp(method, "tools/call") == 0) {
        transport->call_count++;
        cc_json_value_t *result = cc_json_create_object();
        cc_json_value_t *content = cc_json_create_array();
        cc_json_value_t *item = cc_json_create_object();
        cc_json_object_set(item, "type", cc_json_create_string("text"));
        cc_json_object_set(item, "text", cc_json_create_string("fake-ok"));
        cc_json_array_append(content, item);
        cc_json_object_set(result, "content", content);
        *out_response_json = response_with_result(id, result);
    }

    cc_json_destroy(request);
    if (!*out_response_json) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Fake MCP transport failed to build response");
    }
    return cc_result_ok();
}

/* fake reset：记录 TTL 过期后 runtime manager 是否重置 transport。 */
static cc_result_t fake_reset(void *self)
{
    fake_transport_t *transport = (fake_transport_t *)self;
    transport->reset_count++;
    return cc_result_ok();
}

/* fake transport 非串行，允许 manager 按普通并发语义处理。 */
static int fake_is_serial(void *self)
{
    (void)self;
    return 0;
}

/* 销毁 fake transport 私有状态。 */
static void fake_destroy(void *self)
{
    free(self);
}

/* fake MCP transport vtable。 */
static const cc_mcp_transport_vtable_t fake_vtable = {
    fake_send_json,
    fake_reset,
    fake_is_serial,
    fake_destroy
};

/* 等到 wall clock 秒变化，确保 1ms TTL 相关逻辑能稳定触发。 */
static void wait_for_wall_clock_tick(void)
{
    time_t start = time(NULL);
    while (time(NULL) == start) {

    }
}

/* 成功 factory：创建 fake transport，并把指针回传给测试断言。 */
static cc_result_t fake_factory(
    const cc_config_mcp_server_t *server_config,
    cc_mcp_transport_t *out_transport,
    void *user_data
)
{
    (void)server_config;
    fake_transport_t **out_seen = (fake_transport_t **)user_data;
    fake_transport_t *transport = calloc(1, sizeof(*transport));
    if (!transport) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "fake transport alloc failed");
    *out_seen = transport;
    out_transport->self = transport;
    out_transport->vtable = &fake_vtable;
    return cc_result_ok();
}

/* 失败 factory：用于验证 MCP 加载失败会写 diagnostics 而不是整体崩溃。 */
static cc_result_t failing_factory(
    const cc_config_mcp_server_t *server_config,
    cc_mcp_transport_t *out_transport,
    void *user_data
)
{
    (void)server_config;
    (void)out_transport;
    (void)user_data;
    return cc_result_error(CC_ERR_PLATFORM, "factory boom");
}

/*
 * 验证 MCP runtime manager。
 *
 * 覆盖 initialize/tools-list 注册、tool call、cancel 前置拦截、TTL 后 reset/reinitialize，以及
 * factory 失败写入 diagnostics 的降级语义。
 */
int main(void)
{
    cc_config_t config;
    if (cc_config_load_default(&config).code != CC_OK) return 1;
    config.mcp.enabled = 1;
    config.mcp.session_idle_ttl_ms = 1;
    config.mcp.server_count = 1;
    config.mcp.servers = calloc(1, sizeof(cc_config_mcp_server_t));
    if (!config.mcp.servers) return 1;
    config.mcp.servers[0].name = cc_copy_string("fake");
    config.mcp.servers[0].transport = cc_copy_string("fake");

    cc_tool_registry_t *registry = NULL;
    cc_mcp_runtime_manager_t *manager = NULL;
    fake_transport_t *transport = NULL;
    cc_runtime_diagnostics_t diagnostics;
    cc_runtime_diagnostics_reset(&diagnostics);
    int ok = cc_tool_registry_create(&registry).code == CC_OK &&
        cc_mcp_runtime_manager_create(fake_factory, &transport, &manager).code == CC_OK &&
        cc_mcp_runtime_manager_load_tools(manager, &config, registry, &diagnostics).code == CC_OK &&
        transport && transport->initialize_count == 1 && transport->list_count == 1;

    cc_tool_t tool;
    if (ok) ok = cc_tool_registry_find(registry, "mcp.fake.echo", &tool).code == CC_OK;
    if (ok) {
        cc_tool_result_t result;
        cc_tool_context_t ctx;
        memset(&result, 0, sizeof(result));
        memset(&ctx, 0, sizeof(ctx));
        ok = tool.vtable->call(tool.self, "{\"text\":\"hello\"}", &ctx, &result).code == CC_OK &&
            result.ok &&
            result.text &&
            strstr(result.text, "fake-ok") != NULL &&
            transport->call_count == 1;
        cc_tool_result_cleanup(&result);
    }

    if (ok) {
        cc_cancel_source_t *source = NULL;
        cc_tool_result_t result;
        cc_tool_context_t ctx;
        memset(&result, 0, sizeof(result));
        memset(&ctx, 0, sizeof(ctx));
        ok = cc_cancel_source_create(&source).code == CC_OK;
        if (ok) {
            int calls_before_cancel = transport->call_count;
            cc_cancel_source_cancel(source);
            ctx.cancel_token = cc_cancel_source_token(source);
            ok = tool.vtable->call(tool.self, "{\"text\":\"cancel\"}", &ctx, &result).code == CC_OK &&
                !result.ok &&
                result.error &&
                strstr(result.error, "cancel") != NULL &&
                transport->call_count == calls_before_cancel;
        }
        cc_tool_result_cleanup(&result);
        cc_cancel_source_destroy(source);
    }

    if (ok) {
        cc_tool_result_t result;
        cc_tool_context_t ctx;
        memset(&result, 0, sizeof(result));
        memset(&ctx, 0, sizeof(ctx));
        wait_for_wall_clock_tick();
        ok = tool.vtable->call(tool.self, "{\"text\":\"after-ttl\"}", &ctx, &result).code == CC_OK &&
            result.ok &&
            transport->reset_count == 1 &&
            transport->initialize_count == 2 &&
            transport->call_count == 2;
        cc_tool_result_cleanup(&result);
    }

    if (ok) {
        cc_tool_registry_t *diag_registry = NULL;
        cc_mcp_runtime_manager_t *diag_manager = NULL;
        cc_runtime_diagnostics_t diag;
        cc_runtime_diagnostics_reset(&diag);
        ok = cc_tool_registry_create(&diag_registry).code == CC_OK &&
            cc_mcp_runtime_manager_create(failing_factory, NULL, &diag_manager).code == CC_OK &&
            cc_mcp_runtime_manager_load_tools(diag_manager, &config, diag_registry, &diag).code == CC_OK &&
            diag.count == 1 &&
            strcmp(diag.items[0].kind, "mcp") == 0 &&
            strstr(diag.items[0].message, "factory boom") != NULL;
        cc_tool_registry_destroy(diag_registry);
        cc_mcp_runtime_manager_destroy(diag_manager);
    }

    cc_tool_registry_destroy(registry);
    cc_mcp_runtime_manager_destroy(manager);
    cc_config_destroy(&config);
    return ok ? 0 : 1;
}
