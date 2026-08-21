



#define _POSIX_C_SOURCE 200809L

#include "cc/app/cc_mcp_runtime_manager.h"
#include "cc/ports/cc_thread.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * 单个 MCP server 的运行时状态。
 *
 * transport 由 server 拥有；mutex 用于串行 transport 或初始化状态保护；next_id 生成
 * JSON-RPC id；idle_ttl_ms 用于空闲后 reset/reinitialize。
 */
typedef struct cc_mcp_server_runtime {
    char *name;
    char *transport_name;
    cc_mcp_transport_t transport;
    cc_mutex_t mutex;
    unsigned long next_id;
    long last_used_ms;
    int idle_ttl_ms;
    int connection_timeout_ms;
    int needs_initialize;
} cc_mcp_server_runtime_t;

/* MCP tool 适配为 SDK cc_tool_t 时的 self 数据。 */
typedef struct cc_mcp_tool {
    char *tool_name;
    char *display_name;
    char *description;
    char *schema_json;
    cc_mcp_server_runtime_t *server;
} cc_mcp_tool_t;

/* MCP runtime manager 保存 transport factory 和已创建 server runtime。 */
struct cc_mcp_runtime_manager {
    cc_mcp_transport_factory_fn factory;
    void *factory_user_data;
    cc_mcp_server_runtime_t **servers;
    size_t server_count;
};

/* 毫秒级 wall clock，用于 idle TTL；当前精度足够做空闲判断。 */
static long wall_time_ms(void)
{
    return (long)time(NULL) * 1000L;
}

/* 判断 transport 是否必须串行访问；缺省按串行处理更安全。 */
static int server_transport_is_serial(cc_mcp_server_runtime_t *server)
{
    if (!server || !server->transport.vtable || !server->transport.vtable->is_serial) return 1;
    return server->transport.vtable->is_serial(server->transport.self);
}

/* 判断 server 是否超过 idle TTL，需要 reset 并重新 initialize。 */
static int server_idle_expired(cc_mcp_server_runtime_t *server, long now_ms)
{
    if (!server || server->idle_ttl_ms <= 0 || server->last_used_ms <= 0) return 0;
    return now_ms - server->last_used_ms >= server->idle_ttl_ms;
}

/* 构造 MCP initialize params。 */
static cc_json_value_t *build_initialize_params(void)
{


    cc_json_value_t *params = cc_json_create_object();
    cc_json_object_set(params, "protocolVersion", cc_json_create_string("2024-11-05"));
    cc_json_object_set(params, "capabilities", cc_json_create_object());
    cc_json_value_t *client = cc_json_create_object();
    cc_json_object_set(client, "name", cc_json_create_string("c-claw"));
    cc_json_object_set(client, "version", cc_json_create_string("1.0"));
    cc_json_object_set(params, "clientInfo", client);
    return params;
}

/*
 * 构造 JSON-RPC 请求字符串。
 *
 * params 所有权转移给 root；构造失败时销毁 params。返回字符串由调用方 free()。
 */
static char *jsonrpc_request(unsigned long id, const char *method, cc_json_value_t *params)
{
    cc_json_value_t *root = cc_json_create_object();
    if (!root) {
        cc_json_destroy(params);
        return NULL;
    }
    cc_json_object_set(root, "jsonrpc", cc_json_create_string("2.0"));
    cc_json_object_set(root, "id", cc_json_create_number((double)id));
    cc_json_object_set(root, "method", cc_json_create_string(method));
    if (params) cc_json_object_set(root, "params", params);
    char *json = cc_json_stringify_unformatted(root);
    cc_json_destroy(root);
    return json;
}

/* 从 JSON-RPC 文本中提取 id 的 JSON 字符串表示，用于 request/response 匹配。 */
static cc_result_t jsonrpc_id_string(const char *json, char **out_id)
{
    *out_id = NULL;
    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse(json ? json : "", &root);
    if (rc.code != CC_OK) return rc;
    cc_json_value_t *id = cc_json_object_get(root, "id");
    if (id) *out_id = cc_json_stringify_unformatted(id);
    cc_json_destroy(root);
    if (id && !*out_id) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy JSON-RPC id");
    return cc_result_ok();
}

/* 校验 response id 是否匹配 request id，防止串行 transport 读到错位响应。 */
cc_result_t cc_mcp_jsonrpc_response_matches_request(
    const char *request_json,
    const char *response_json,
    int *out_matches
)
{
    if (!request_json || !response_json || !out_matches) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid JSON-RPC match arguments");
    }
    *out_matches = 0;
    char *request_id = NULL;
    char *response_id = NULL;
    cc_result_t rc = jsonrpc_id_string(request_json, &request_id);
    if (rc.code == CC_OK) rc = jsonrpc_id_string(response_json, &response_id);
    if (rc.code == CC_OK && request_id && response_id) {
        *out_matches = strcmp(request_id, response_id) == 0;
    }
    free(request_id);
    free(response_id);
    return rc;
}

/*
 * 发送请求并处理串行/并行 transport 锁策略。
 *
 * 串行 transport 在持锁状态下发送；并行 transport 会先释放锁，避免阻塞同 server 的其他
 * 并行请求。
 */
static cc_result_t transport_send_locked_or_parallel(
    cc_mcp_server_runtime_t *server,
    char *request,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    char **out_response_json,
    int lock_is_held
)
{
    if (!server->transport.vtable || !server->transport.vtable->send_json) {
        if (lock_is_held) cc_mutex_unlock(server->mutex);
        return cc_result_error(CC_ERR_PLATFORM, "MCP transport has no send_json implementation");
    }

    int serial = server_transport_is_serial(server);
    if (!serial && lock_is_held) cc_mutex_unlock(server->mutex);
    cc_result_t rc = server->transport.vtable->send_json(
        server->transport.self,
        request,
        timeout_ms > 0 ? timeout_ms :
            (server->connection_timeout_ms > 0 ? server->connection_timeout_ms : 30000),
        cancel_token,
        out_response_json);
    if (serial && lock_is_held) cc_mutex_unlock(server->mutex);
    return rc;
}

/* 发送 JSON-RPC 请求、校验 id 并解析响应 JSON。 */
static cc_result_t send_request_and_parse(
    cc_mcp_server_runtime_t *server,
    char *request,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    cc_json_value_t **out_response,
    int lock_is_held
)
{
    char *response_json = NULL;
    cc_result_t rc = transport_send_locked_or_parallel(
        server, request, timeout_ms, cancel_token, &response_json, lock_is_held);
    if (rc.code != CC_OK) {
        free(request);
        return rc;
    }

    int matches = 0;
    rc = cc_mcp_jsonrpc_response_matches_request(request, response_json, &matches);
    free(request);
    if (rc.code != CC_OK) {
        free(response_json);
        return rc;
    }
    if (!matches) {
        free(response_json);
        return cc_result_error(CC_ERR_JSON, "MCP transport returned a mismatched JSON-RPC response id");
    }

    rc = cc_json_parse(response_json, out_response);
    free(response_json);
    return rc;
}

/*
 * 在保持 server 锁时发送请求。
 *
 * initialize 阶段必须串行执行，避免多个线程同时初始化同一个 server。
 */
static cc_result_t send_request_and_parse_while_locked(
    cc_mcp_server_runtime_t *server,
    char *request,
    cc_json_value_t **out_response
)
{
    if (!server->transport.vtable || !server->transport.vtable->send_json) {
        free(request);
        return cc_result_error(CC_ERR_PLATFORM, "MCP transport has no send_json implementation");
    }
    char *response_json = NULL;
    cc_result_t rc = server->transport.vtable->send_json(
        server->transport.self,
        request,
        server->connection_timeout_ms > 0 ? server->connection_timeout_ms : 30000,
        NULL,
        &response_json);
    if (rc.code != CC_OK) {
        free(request);
        return rc;
    }
    int matches = 0;
    rc = cc_mcp_jsonrpc_response_matches_request(request, response_json, &matches);
    free(request);
    if (rc.code != CC_OK) {
        free(response_json);
        return rc;
    }
    if (!matches) {
        free(response_json);
        return cc_result_error(CC_ERR_JSON, "MCP transport returned a mismatched JSON-RPC response id");
    }
    rc = cc_json_parse(response_json, out_response);
    free(response_json);
    return rc;
}

/*
 * 确保 server 已完成 initialize。
 *
 * 调用时持有 server->mutex；如果无需初始化则释放锁返回。初始化成功后更新状态并释放锁。
 */
static cc_result_t ensure_initialized_locked(cc_mcp_server_runtime_t *server)
{
    if (!server->needs_initialize) {
        cc_mutex_unlock(server->mutex);
        return cc_result_ok();
    }

    unsigned long id = ++server->next_id;
    char *request = jsonrpc_request(id, "initialize", build_initialize_params());
    if (!request) {
        cc_mutex_unlock(server->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build MCP initialize request");
    }

    cc_json_value_t *response = NULL;


    cc_result_t rc = send_request_and_parse_while_locked(server, request, &response);
    cc_json_destroy(response);
    if (rc.code == CC_OK) {
        server->needs_initialize = 0;
        server->last_used_ms = wall_time_ms();
    }
    cc_mutex_unlock(server->mutex);
    return rc;
}

/*
 * 发起一次 MCP JSON-RPC 调用。
 *
 * 负责取消检查、空闲 reset、按需 initialize、构造请求、发送并解析响应。params 所有权
 * 在函数内消费，失败路径也会销毁。
 */
static cc_result_t mcp_call_raw(
    cc_mcp_server_runtime_t *server,
    const char *method,
    cc_json_value_t *params,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    cc_json_value_t **out_response
)
{
    if (!server || !method || !out_response) {
        cc_json_destroy(params);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid MCP call");
    }
    *out_response = NULL;
    if (cc_cancel_token_is_cancelled(cancel_token)) {
        cc_json_destroy(params);
        return cc_result_error(CC_ERR_CANCELLED, "MCP call cancelled before send");
    }

    cc_mutex_lock(server->mutex);
    long now_ms = wall_time_ms();
    if (server_idle_expired(server, now_ms)) {
        if (server->transport.vtable && server->transport.vtable->reset) {
            cc_result_t reset_rc = server->transport.vtable->reset(server->transport.self);
            if (reset_rc.code != CC_OK) {
                cc_json_destroy(params);
                cc_mutex_unlock(server->mutex);
                return reset_rc;
            }
        }
        server->needs_initialize = 1;
    }

    if (strcmp(method, "initialize") != 0) {
        cc_result_t init_rc = ensure_initialized_locked(server);
        if (init_rc.code != CC_OK) {
            cc_json_destroy(params);
            return init_rc;
        }


        cc_mutex_lock(server->mutex);
    }

    unsigned long id = ++server->next_id;
    server->last_used_ms = wall_time_ms();
    char *request = jsonrpc_request(id, method, params);
    if (!request) {
        cc_mutex_unlock(server->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build MCP request");
    }

    cc_result_t rc = send_request_and_parse(
        server, request, timeout_ms, cancel_token, out_response, 1);
    if (rc.code == CC_OK && strcmp(method, "initialize") == 0) {
        cc_mutex_lock(server->mutex);
        server->needs_initialize = 0;
        server->last_used_ms = wall_time_ms();
        cc_mutex_unlock(server->mutex);
    }
    return rc;
}

/* 销毁 MCP server runtime、transport、mutex 和字符串字段。 */
static void server_runtime_destroy(cc_mcp_server_runtime_t *server)
{
    if (!server) return;
    if (server->transport.vtable && server->transport.vtable->destroy) {
        server->transport.vtable->destroy(server->transport.self);
    }
    if (server->mutex) cc_mutex_destroy(server->mutex);
    free(server->name);
    free(server->transport_name);
    free(server);
}

/* 创建 MCP runtime manager。 */
cc_result_t cc_mcp_runtime_manager_create(
    cc_mcp_transport_factory_fn factory,
    void *factory_user_data,
    cc_mcp_runtime_manager_t **out_manager
)
{
    if (!factory || !out_manager) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid MCP runtime manager arguments");
    }
    cc_mcp_runtime_manager_t *manager = calloc(1, sizeof(*manager));
    if (!manager) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create MCP runtime manager");
    manager->factory = factory;
    manager->factory_user_data = factory_user_data;
    *out_manager = manager;
    return cc_result_ok();
}

/* 销毁 manager 和所有 server runtime。 */
void cc_mcp_runtime_manager_destroy(cc_mcp_runtime_manager_t *manager)
{
    if (!manager) return;
    for (size_t i = 0; i < manager->server_count; i++) {
        server_runtime_destroy(manager->servers[i]);
    }
    free(manager->servers);
    free(manager);
}

/* MCP tool vtable: 返回展示给 registry/provider 的工具名。 */
static const char *mcp_tool_name(void *self)
{
    return ((cc_mcp_tool_t *)self)->display_name;
}

/* MCP tool vtable: 返回工具描述。 */
static const char *mcp_tool_description(void *self)
{
    return ((cc_mcp_tool_t *)self)->description;
}

/* MCP tool vtable: 返回参数 schema JSON。 */
static const char *mcp_tool_schema_json(void *self)
{
    return ((cc_mcp_tool_t *)self)->schema_json;
}

/*
 * MCP tool vtable: 执行 tools/call。
 *
 * args_json 解析失败时退化为空对象；MCP error 作为可恢复 tool result 返回，避免整个 run
 * 崩溃。
 */
static cc_result_t mcp_tool_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    cc_mcp_tool_t *tool = (cc_mcp_tool_t *)self;
    memset(out_result, 0, sizeof(*out_result));

    cc_json_value_t *params = cc_json_create_object();
    cc_json_object_set(params, "name", cc_json_create_string(tool->tool_name));
    cc_json_value_t *arguments = NULL;
    cc_result_t parse_rc = cc_json_parse(args_json ? args_json : "{}", &arguments);
    if (parse_rc.code != CC_OK || !arguments) {
        cc_result_free(&parse_rc);
        arguments = cc_json_create_object();
    }
    cc_json_object_set(params, "arguments", arguments);

    cc_json_value_t *response = NULL;
    cc_cancel_token_t *token = ctx ? ctx->cancel_token : NULL;
    int timeout_ms = ctx && ctx->timeout_ms > 0 ? ctx->timeout_ms : 0;
    cc_result_t rc = mcp_call_raw(
        tool->server, "tools/call", params, timeout_ms, token, &response);
    if (rc.code != CC_OK) {
        out_result->ok = 0;
        out_result->error = cc_copy_string(rc.message ? rc.message : "MCP tool call failed");
        cc_result_free(&rc);
        return cc_result_ok();
    }

    cc_json_value_t *error = cc_json_object_get(response, "error");
    if (error) {
        char *err = cc_json_stringify_unformatted(error);
        out_result->ok = 0;
        out_result->error = err ? err : cc_copy_string("MCP server returned an error");
        cc_json_destroy(response);
        return cc_result_ok();
    }

    cc_json_value_t *result = cc_json_object_get(response, "result");
    out_result->ok = 1;
    out_result->text = result ? cc_json_stringify_unformatted(result) : cc_copy_string("{}");
    cc_json_destroy(response);
    return cc_result_ok();
}

/* 销毁 MCP tool self。 */
static void mcp_tool_destroy(void *self)
{
    cc_mcp_tool_t *tool = (cc_mcp_tool_t *)self;
    if (!tool) return;
    free(tool->tool_name);
    free(tool->display_name);
    free(tool->description);
    free(tool->schema_json);
    free(tool);
}

/* MCP tool 的 vtable 实例。 */
static const cc_tool_vtable_t mcp_tool_vtable = {
    mcp_tool_name,
    mcp_tool_description,
    mcp_tool_schema_json,
    mcp_tool_call,
    mcp_tool_destroy
};

/*
 * 把 MCP tools/list 中的一项注册为 SDK tool。
 *
 * 对外工具名加上 mcp.<server>.<tool> 前缀，避免不同 server 的 tool 名冲突。
 */
static cc_result_t register_mcp_tool(
    cc_tool_registry_t *registry,
    cc_mcp_server_runtime_t *server,
    cc_json_value_t *tool_json
)
{
    const char *name = cc_json_string_value(cc_json_object_get(tool_json, "name"));
    if (!name || !name[0]) return cc_result_ok();
    const char *description = cc_json_string_value(cc_json_object_get(tool_json, "description"));
    cc_json_value_t *schema = cc_json_object_get(tool_json, "inputSchema");
    if (!schema) schema = cc_json_object_get(tool_json, "parameters");

    cc_mcp_tool_t *tool_self = calloc(1, sizeof(*tool_self));
    if (!tool_self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create MCP tool");
    tool_self->tool_name = cc_copy_string(name);
    tool_self->description = cc_copy_string(description ? description : "MCP tool");
    tool_self->schema_json = schema ? cc_json_stringify_unformatted(schema) :
        cc_copy_string("{\"type\":\"object\",\"properties\":{}}");
    size_t display_len = strlen("mcp..") + strlen(server->name) + strlen(name) + 1;
    tool_self->display_name = malloc(display_len);
    if (tool_self->display_name) {
        snprintf(tool_self->display_name, display_len, "mcp.%s.%s", server->name, name);
    }
    tool_self->server = server;

    if (!tool_self->tool_name || !tool_self->display_name ||
        !tool_self->description || !tool_self->schema_json) {
        mcp_tool_destroy(tool_self);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy MCP tool metadata");
    }

    cc_tool_t tool = {0};
    tool.self = tool_self;
    tool.vtable = &mcp_tool_vtable;
    cc_result_t rc = cc_tool_registry_add(registry, tool);
    if (rc.code != CC_OK) {
        mcp_tool_destroy(tool_self);
    }
    return rc;
}

/* 调用 server 的 tools/list 并逐项注册工具。 */
static cc_result_t list_server_tools(
    cc_mcp_server_runtime_t *server,
    cc_tool_registry_t *registry
)
{
    cc_json_value_t *response = NULL;
    cc_result_t rc = mcp_call_raw(server, "tools/list", cc_json_create_object(), 0, NULL, &response);
    if (rc.code != CC_OK) return rc;
    cc_json_value_t *error = cc_json_object_get(response, "error");
    if (error) {
        char *err = cc_json_stringify_unformatted(error);
        rc = cc_result_error(CC_ERR_TOOL, err ? err : "MCP tools/list returned an error");
        free(err);
        cc_json_destroy(response);
        return rc;
    }
    cc_json_value_t *result = cc_json_object_get(response, "result");
    cc_json_value_t *tools = result ? cc_json_object_get(result, "tools") : NULL;
    if (!tools || !cc_json_is_array(tools)) {
        cc_json_destroy(response);
        return cc_result_ok();
    }
    int count = cc_json_array_size(tools);
    for (int i = 0; i < count; i++) {
        cc_result_t trc = register_mcp_tool(registry, server, cc_json_array_get(tools, i));
        if (trc.code != CC_OK) {
            cc_json_destroy(response);
            return trc;
        }
    }
    cc_json_destroy(response);
    return cc_result_ok();
}

/* 把 server runtime 加入 manager 动态数组，manager 接管 server 所有权。 */
static cc_result_t manager_add_server(
    cc_mcp_runtime_manager_t *manager,
    cc_mcp_server_runtime_t *server
)
{
    cc_mcp_server_runtime_t **next = realloc(
        manager->servers, (manager->server_count + 1) * sizeof(*next));
    if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow MCP server table");
    manager->servers = next;
    manager->servers[manager->server_count++] = server;
    return cc_result_ok();
}

/*
 * 根据配置创建单个 server runtime。
 *
 * transport 由 factory 创建；server 复制 name/transport_name 并创建 mutex。失败时销毁
 * transport 和 server 半成品。
 */
static cc_result_t server_runtime_create(
    cc_mcp_runtime_manager_t *manager,
    const cc_config_mcp_server_t *config,
    int idle_ttl_ms,
    cc_mcp_server_runtime_t **out_server
)
{
    *out_server = NULL;
    if (!config->name || !config->name[0]) return cc_result_ok();
    cc_mcp_transport_t transport;
    memset(&transport, 0, sizeof(transport));
    cc_result_t rc = manager->factory(config, &transport, manager->factory_user_data);
    if (rc.code != CC_OK) return rc;
    if (!transport.vtable) return cc_result_ok();

    cc_mcp_server_runtime_t *server = calloc(1, sizeof(*server));
    if (!server) {
        if (transport.vtable->destroy) transport.vtable->destroy(transport.self);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create MCP server runtime");
    }
    server->name = cc_copy_string(config->name);
    server->transport_name = cc_copy_string(config->transport ? config->transport : "stdio");
    server->transport = transport;
    server->idle_ttl_ms = idle_ttl_ms;
    server->connection_timeout_ms = config->connection_timeout_ms;
    server->needs_initialize = 1;
    rc = cc_mutex_create(&server->mutex);
    if (rc.code == CC_OK && (!server->name || !server->transport_name)) {
        rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy MCP server config");
    }
    if (rc.code != CC_OK) {
        server_runtime_destroy(server);
        return rc;
    }
    *out_server = server;
    return cc_result_ok();
}

/*
 * 加载 MCP server 并注册工具。
 *
 * 每个 server 创建失败、initialize 失败或 tools/list 失败都会写入 diagnostics 并继续下
 * 一个 server，保证单个 MCP server 不影响整个 runtime 启动。
 */
cc_result_t cc_mcp_runtime_manager_load_tools(
    cc_mcp_runtime_manager_t *manager,
    const cc_config_t *config,
    cc_tool_registry_t *registry,
    cc_runtime_diagnostics_t *diagnostics
)
{
    if (!manager || !config || !registry || !config->mcp.enabled) return cc_result_ok();
    for (size_t i = 0; i < config->mcp.server_count; i++) {
        const cc_config_mcp_server_t *server_cfg = &config->mcp.servers[i];
        cc_mcp_server_runtime_t *server = NULL;
        cc_result_t rc = server_runtime_create(
            manager, server_cfg, config->mcp.session_idle_ttl_ms, &server);
        if (rc.code != CC_OK || !server) {
            char message[192];
            snprintf(message, sizeof(message), "failed to create transport: %s",
                rc.message ? rc.message : "unsupported or disabled transport");
            cc_runtime_diagnostics_add(diagnostics, "mcp",
                server_cfg->name ? server_cfg->name : "(unnamed)",
                message);
            cc_result_free(&rc);
            continue;
        }

        cc_json_value_t *init_response = NULL;
        rc = mcp_call_raw(server, "initialize", build_initialize_params(), 0, NULL, &init_response);
        cc_json_destroy(init_response);
        if (rc.code != CC_OK) {
            cc_runtime_diagnostics_add(diagnostics, "mcp",
                server->name ? server->name : "(unnamed)",
                rc.message ? rc.message : "failed to initialize MCP server");
            cc_result_free(&rc);
            server_runtime_destroy(server);
            continue;
        }

        rc = manager_add_server(manager, server);
        if (rc.code != CC_OK) {
            server_runtime_destroy(server);
            return rc;
        }

        rc = list_server_tools(server, registry);
        if (rc.code != CC_OK) {
            cc_runtime_diagnostics_add(diagnostics, "mcp",
                server->name ? server->name : "(unnamed)",
                rc.message ? rc.message : "failed to list/register MCP tools");
            cc_result_free(&rc);
        }
    }
    return cc_result_ok();
}
