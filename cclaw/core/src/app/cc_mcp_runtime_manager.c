



#define _POSIX_C_SOURCE 200809L

#include "cc/app/cc_mcp_runtime_manager.h"
#include "cc/ports/cc_thread.h"
#include "cc/ports/cc_platform.h"
#include "cc/core/cc_version.h"
#include "cc/internal/cc_json_syntax_check.h"
#include "cc/util/cc_json.h"
#include "cc/util/cc_tool_name.h"
#include "cc/internal/cc_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <stdint.h>

#define MCP_PROTOCOL_VERSION "2024-11-05"
#define MCP_MAX_NUMERIC_REQUEST_ID UINT64_C(9007199254740991)

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
    cc_cond_t cond;
    uint64_t next_id;
    uint64_t last_used_ms;
    size_t active_calls;
    int idle_ttl_ms;
    int connection_timeout_ms;
    int needs_initialize;
    int needs_reset;
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

/**
 * @brief 查询传输是否要求单请求串行访问。
 * @param[in] server 借用的 server，缺失实例或回调时采用串行策略。
 * @return 非零表示串行，零表示允许并发请求。
 */
static int server_transport_is_serial(cc_mcp_server_runtime_t *server)
{
    if (!server || !server->transport.vtable || !server->transport.vtable->is_serial) return 1;
    return server->transport.vtable->is_serial(server->transport.self);
}

/**
 * @brief 在没有在途调用时判断单调空闲时间是否达到配置期限。
 * @param[in] server 锁内借用的 server 状态，允许 NULL。
 * @param[in] now_ms 当前单调毫秒时间。
 * @return 已初始化、无在途请求且达到正 TTL 为一，其他为零。
 */
static int server_idle_expired(cc_mcp_server_runtime_t *server, uint64_t now_ms)
{
    if (!server || server->idle_ttl_ms <= 0 || server->needs_initialize ||
        server->active_calls || now_ms < server->last_used_ms) return 0;
    return now_ms - server->last_used_ms >= (uint64_t)server->idle_ttl_ms;
}

/**
 * @brief 构造当前支持版本的 initialize 参数，检查每次字段拥有权交接。
 * @return 成功返回拥有的对象树，调用者消费或销毁，任一分配失败清理并返回 NULL。
 */
static cc_json_value_t *build_initialize_params(void)
{


    cc_json_value_t *params = cc_json_create_object();
    if (!params) return NULL;
    if (!cc_json_object_set_owned(params, "protocolVersion", cc_json_create_string(MCP_PROTOCOL_VERSION)) ||
        !cc_json_object_set_owned(params, "capabilities", cc_json_create_object())) {
        cc_json_destroy(params);
        return NULL;
    }
    cc_json_value_t *client = cc_json_create_object();
    if (!client ||
        !cc_json_object_set_owned(client, "name", cc_json_create_string("c-claw")) ||
        !cc_json_object_set_owned(client, "version", cc_json_create_string(cc_claw_version_string()))) {
        cc_json_destroy(client);
        cc_json_destroy(params);
        return NULL;
    }
    if (!cc_json_object_set_owned(params, "clientInfo", client)) {
        cc_json_destroy(params);
        return NULL;
    }
    return params;
}

/**
 * @brief 构造 JSON-RPC 请求或无 id 通知，消费可选参数树。
 * @param[in] id 请求数字标识，调用者保证可被 double 精确表示，通知忽略该值。
 * @param[in] method 借用的非空方法名。
 * @param[in,out] params 消费的可选独占参数树，失败也销毁。
 * @param[in] include_id 非零写入请求 id，零构造通知。
 * @return 拥有的紧凑字符串，由调用者 free，分配失败返回 NULL 并回收本次节点。
 */
static char *jsonrpc_request(uint64_t id, const char *method, cc_json_value_t *params, int include_id)
{
    cc_json_value_t *root = cc_json_create_object();
    if (!root) {
        cc_json_destroy(params);
        return NULL;
    }
    if (!cc_json_object_set_owned(root, "jsonrpc", cc_json_create_string("2.0")) ||
        (include_id && !cc_json_object_set_owned(root, "id", cc_json_create_number((double)id))) ||
        !cc_json_object_set_owned(root, "method", cc_json_create_string(method))) {
        cc_json_destroy(params);
        cc_json_destroy(root);
        return NULL;
    }
    if (params && !cc_json_object_set_owned(root, "params", params)) {
        cc_json_destroy(root);
        return NULL;
    }
    char *json = cc_json_stringify_unformatted(root);
    cc_json_destroy(root);
    return json;
}

/**
 * @brief 完整解析可无损表示为 C 字符串的 JSON 对象。
 * @param[in] json 借用的必填完整 JSON 文本。
 * @param[out] out_root 必填拥有树输出，入口置 NULL，失败无树交付。
 * @return 返回解析、对象类型或 NUL 转义检查结果，消息由调用者释放。
 */
static cc_result_t parse_json_object(const char *json, cc_json_value_t **out_root)
{
    *out_root = NULL;
    if (!json || cc_storage_json_has_nul_escape(json))
        return cc_result_error(CC_ERR_JSON, "MCP JSON contains an unrepresentable string");
    cc_result_t rc = cc_json_parse_strict(json, out_root);
    if (rc.code != CC_OK) return rc;
    if (!cc_json_is_object(*out_root)) {
        cc_json_destroy(*out_root);
        *out_root = NULL;
        return cc_result_error(CC_ERR_JSON, "MCP JSON value must be an object");
    }
    return cc_result_ok();
}

/**
 * @brief 解析 JSON-RPC 对象并拒绝重复的协议控制字段。
 * @param[in] json 借用的完整 JSON-RPC 文本。
 * @param[out] out_root 必填拥有树输出，失败保持 NULL，成功由调用者销毁。
 * @return 返回完整解析或协议字段歧义结果，普通未知字段继续由对应方法解释。
 */
static cc_result_t parse_rpc_object(const char *json, cc_json_value_t **out_root)
{
    cc_result_t rc = parse_json_object(json, out_root);
    if (rc.code != CC_OK) return rc;
    const char *fields[] = {"jsonrpc", "id", "method", "params", "result", "error"};
    unsigned seen = 0;
    int count = cc_json_object_size(*out_root);
    for (int i = 0; i < count; i++) {
        const char *key = cc_json_object_key_at(*out_root, i);
        for (size_t j = 0; j < sizeof(fields) / sizeof(fields[0]); j++) {
            if (!key || strcmp(key, fields[j]) != 0) continue;
            unsigned bit = 1U << j;
            if (seen & bit) {
                cc_json_destroy(*out_root);
                *out_root = NULL;
                return cc_result_error(CC_ERR_JSON, "MCP JSON-RPC message has a duplicate field");
            }
            seen |= bit;
        }
    }
    return cc_result_ok();
}

/**
 * @brief 提取合法 JSON-RPC id 的紧凑文本，用于保留类型的匹配。
 * @param[in] json 借用的完整对象文本。
 * @param[out] out_id 必填，入口置 NULL，存在有效 id 时交付需 free 的字符串。
 * @return 缺少 id 返回 CC_OK 和 NULL，有效类型为字符串、有限数字或 null，其他返回 JSON 错误。
 */
static cc_result_t jsonrpc_id_string(const char *json, char **out_id)
{
    *out_id = NULL;
    cc_json_value_t *root = NULL;
    cc_result_t rc = parse_rpc_object(json, &root);
    if (rc.code != CC_OK) return rc;
    cc_json_value_t *id = cc_json_object_get_case_sensitive(root, "id");
    if (id && !cc_json_is_string(id) && !cc_json_is_null(id) &&
        !(cc_json_is_number(id) && isfinite(cc_json_number_value(id)))) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "Invalid JSON-RPC id type");
    }
    int has_id = id != NULL;
    if (id) *out_id = cc_json_stringify_unformatted(id);
    cc_json_destroy(root);
    if (has_id && !*out_id) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy JSON-RPC id");
    return cc_result_ok();
}

/**
 * @brief 完整解析两个 JSON-RPC 对象，并按 id 的类型和值匹配响应归属。
 * @param[in] request_json 调用期间借用的完整请求文本。
 * @param[in] response_json 调用期间借用的完整响应文本。
 * @param[out] out_matches 必填，参数有效后先置零，两个 id 存在且相等时置一。
 * @return 返回参数、JSON 或复制失败结果，拥有诊断由调用者释放，缺失或不等 id 返回 CC_OK 和零。
 */
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

/**
 * @brief 为一次调用建立单调绝对截止时间。
 * @param[in] server 借用配置，提供缺省连接超时。
 * @param[in] timeout_ms 调用超时，正值优先，其他使用 server 或 30000 毫秒。
 * @return 饱和加法得到的绝对单调毫秒截止时间。
 */
static uint64_t call_deadline_ms(const cc_mcp_server_runtime_t *server, int timeout_ms)
{
    int budget = timeout_ms > 0 ? timeout_ms :
        (server->connection_timeout_ms > 0 ? server->connection_timeout_ms : 30000);
    uint64_t now = cc_platform_monotonic_ms();
    return UINT64_MAX - now < (uint64_t)budget ? UINT64_MAX : now + (uint64_t)budget;
}

/**
 * @brief 检查取消与截止时间，并返回本次调用的剩余整数毫秒预算。
 * @param[in] deadline_ms 共享的绝对单调截止时间。
 * @param[in] cancel_token 可选借用令牌。
 * @param[out] out_ms 必填剩余值，成功为一至 INT_MAX，失败保持原值。
 * @return CC_OK、CC_ERR_CANCELLED 或 CC_ERR_TIMEOUT，消息由调用者释放。
 */
static cc_result_t remaining_call_ms(uint64_t deadline_ms, cc_cancel_token_t *cancel_token,
                                     int *out_ms)
{
    if (cc_cancel_token_is_cancelled(cancel_token))
        return cc_result_error(CC_ERR_CANCELLED, "MCP call cancelled");
    uint64_t now = cc_platform_monotonic_ms();
    if (now >= deadline_ms)
        return cc_result_error(CC_ERR_TIMEOUT, "MCP call deadline exceeded");
    uint64_t remaining = deadline_ms - now;
    *out_ms = remaining > INT_MAX ? INT_MAX : (int)remaining;
    return cc_result_ok();
}

/**
 * @brief 在 server 锁内分配可精确表示的新数字 id，并消费参数构造请求。
 * @param[in,out] server 锁内借用的 server，id 单调增加且禁止精度耗尽后复用。
 * @param[in] method 调用期间借用的方法名。
 * @param[in,out] params 消费的可选独占参数树。
 * @param[out] out_id 必填，成功接收精确请求 id。
 * @param[out] out_request 必填，先置 NULL，成功接收需 free 的请求文本。
 * @return CC_OK、id 耗尽时的 LIMIT_EXCEEDED 或构造 OOM，失败也消费 params。
 */
static cc_result_t next_request_locked(cc_mcp_server_runtime_t *server, const char *method,
                                       cc_json_value_t *params, uint64_t *out_id,
                                       char **out_request)
{
    *out_request = NULL;
    if (server->next_id >= MCP_MAX_NUMERIC_REQUEST_ID) {
        cc_json_destroy(params);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "MCP request id range exhausted");
    }
    *out_id = ++server->next_id;
    *out_request = jsonrpc_request(*out_id, method, params, 1);
    return *out_request ? cc_result_ok() :
        cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build MCP request");
}

/**
 * @brief 同步发送拥有请求，严格核对响应信封与数字 id，回收传输临时正文。
 * @param[in,out] server 存活 server，调用方根据传输能力维护锁与在途计数。
 * @param[in] request_id 本地精确数字请求 id。
 * @param[in,out] request 消费的完整请求字符串，所有路径 free。
 * @param[in] timeout_ms 本次传输正毫秒预算。
 * @param[in] cancel_token 可选借用取消令牌。
 * @param[out] out_response 必填，入口置 NULL，成功交付由调用者销毁的响应对象。
 * @return 传输、解析或协议校验结果，合法 JSON-RPC error 保留在成功交付的响应中。
 */
static cc_result_t send_request_and_parse(
    cc_mcp_server_runtime_t *server,
    uint64_t request_id,
    char *request,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    cc_json_value_t **out_response
)
{
    *out_response = NULL;
    char *response_json = NULL;
    if (!server->transport.vtable || !server->transport.vtable->send_json) {
        free(request);
        return cc_result_error(CC_ERR_PLATFORM, "MCP transport has no send_json implementation");
    }
    cc_result_t rc = server->transport.vtable->send_json(server->transport.self,
        request, timeout_ms, cancel_token, &response_json);
    free(request);
    if (rc.code != CC_OK) {
        free(response_json);
        return rc;
    }
    cc_result_free(&rc);
    rc = parse_rpc_object(response_json, out_response);
    free(response_json);
    if (rc.code != CC_OK) return rc;
    cc_json_value_t *root = *out_response;
    const char *version = cc_json_string_value(cc_json_object_get_case_sensitive(root, "jsonrpc"));
    cc_json_value_t *id = cc_json_object_get_case_sensitive(root, "id");
    cc_json_value_t *result = cc_json_object_get_case_sensitive(root, "result");
    cc_json_value_t *error = cc_json_object_get_case_sensitive(root, "error");
    int valid = version && strcmp(version, "2.0") == 0 && cc_json_is_number(id) &&
        isfinite(cc_json_number_value(id)) && cc_json_number_value(id) == (double)request_id &&
        ((result != NULL) != (error != NULL));
    if (valid && error) {
        cc_json_value_t *code = cc_json_object_get_case_sensitive(error, "code");
        cc_json_value_t *message = cc_json_object_get_case_sensitive(error, "message");
        double number = cc_json_number_value(code);
        double integer_part = 0.0;
        valid = cc_json_is_object(error) && cc_json_is_number(code) && isfinite(number) &&
            modf(number, &integer_part) == 0.0 && cc_json_is_string(message);
    }
    if (valid) return cc_result_ok();
    cc_json_destroy(root);
    *out_response = NULL;
    return cc_result_error(CC_ERR_JSON, "MCP transport returned an invalid JSON-RPC response");
}

/**
 * @brief 检查 initialize 响应并发送 initialized 通知，完成当前版本工具能力握手。
 * @param[in,out] server 调用前后均由本线程持有 mutex，无在途请求，初始化状态在锁内更新。
 * @param[in] deadline_ms 初始化及后续普通请求共用的单调截止时间。
 * @param[in] cancel_token 可选借用取消令牌，向请求和通知传递。
 * @return 已就绪或完成握手为 CC_OK，版本/能力/通知接口缺失为 UNSUPPORTED，其他为实际错误。
 * @note 发送初始化请求后即保留 needs_reset，只有通知成功才清除，失败后重试先重置连接。
 */
static cc_result_t ensure_initialized_locked(cc_mcp_server_runtime_t *server,
                                             uint64_t deadline_ms,
                                             cc_cancel_token_t *cancel_token)
{
    if (!server->needs_initialize) return cc_result_ok();
    if (!server->transport.vtable || !server->transport.vtable->send_notification)
        return cc_result_error(CC_ERR_UNSUPPORTED, "MCP transport requires notification support");
    int remaining = 0;
    cc_result_t rc = remaining_call_ms(deadline_ms, cancel_token, &remaining);
    if (rc.code != CC_OK) return rc;
    cc_json_value_t *params = build_initialize_params();
    if (!params)
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build MCP initialize params");
    uint64_t id = 0;
    char *request = NULL;
    rc = next_request_locked(server, "initialize", params, &id, &request);
    if (rc.code != CC_OK) return rc;
    rc = remaining_call_ms(deadline_ms, cancel_token, &remaining);
    if (rc.code != CC_OK) {
        free(request);
        return rc;
    }
    server->needs_reset = 1;
    cc_json_value_t *response = NULL;
    rc = send_request_and_parse(server, id, request, remaining, cancel_token, &response);
    if (rc.code != CC_OK) return rc;
    cc_json_value_t *error = cc_json_object_get_case_sensitive(response, "error");
    if (error) {
        char *message = cc_json_stringify_unformatted(error);
        rc = cc_result_error(CC_ERR_TOOL, message ? message : "MCP initialize returned an error");
        free(message);
        cc_json_destroy(response);
        return rc;
    }
    cc_json_value_t *result = cc_json_object_get_case_sensitive(response, "result");
    const char *version = cc_json_string_value(cc_json_object_get_case_sensitive(result, "protocolVersion"));
    cc_json_value_t *capabilities = cc_json_object_get_case_sensitive(result, "capabilities");
    cc_json_value_t *tools = cc_json_object_get_case_sensitive(capabilities, "tools");
    cc_json_value_t *info = cc_json_object_get_case_sensitive(result, "serverInfo");
    int valid = cc_json_is_object(result) && version && strcmp(version, MCP_PROTOCOL_VERSION) == 0 &&
        cc_json_is_object(capabilities) && cc_json_is_object(tools) && cc_json_is_object(info) &&
        cc_json_is_string(cc_json_object_get_case_sensitive(info, "name")) &&
        cc_json_is_string(cc_json_object_get_case_sensitive(info, "version"));
    cc_json_destroy(response);
    if (!valid)
        return cc_result_error(CC_ERR_UNSUPPORTED, "MCP initialize has an unsupported version or tool capability");
    char *notification = jsonrpc_request(0, "notifications/initialized", NULL, 0);
    if (!notification)
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build MCP initialized notification");
    rc = remaining_call_ms(deadline_ms, cancel_token, &remaining);
    if (rc.code == CC_OK) {
        rc = server->transport.vtable->send_notification(server->transport.self,
            notification, remaining, cancel_token);
    }
    free(notification);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    rc = remaining_call_ms(deadline_ms, cancel_token, &remaining);
    if (rc.code != CC_OK) return rc;
    server->needs_initialize = 0;
    server->needs_reset = 0;
    server->last_used_ms = cc_platform_monotonic_ms();
    return cc_result_ok();
}

/**
 * @brief 按剩余预算等待故障请求排空、重建握手，再按传输能力执行普通 MCP 请求。
 * @param[in,out] server 借用存活 server，其 manager 由外层调用寿命屏障保活。
 * @param[in] method 借用的普通方法名，调用点使用 tools/list 或 tools/call。
 * @param[in,out] params 消费的可选独占参数树，失败也销毁。
 * @param[in] deadline_ms 调用者从操作入口建立的绝对单调毫秒截止时间。
 * @param[in] cancel_token 可选借用令牌，握手和普通请求均检查并传递。
 * @param[out] out_response 必填拥有响应输出，基础参数有效后置 NULL，成功后由调用者销毁。
 * @return 实际参数、取消、超时、连接、协议或分配结果，合法远端 error 随响应交付。
 * @note 串行传输持锁完成发送及解析，并行传输登记在途后解锁，最后归锁收尾。
 *       空闲期从最后完成时刻起算，在途请求阻止 reset。锁获取及 reset 回调自身的耗时
 *       取决于端口，返回后再次检查预算再决定是否发送下一条消息。
 */
static cc_result_t mcp_call_raw(
    cc_mcp_server_runtime_t *server,
    const char *method,
    cc_json_value_t *params,
    uint64_t deadline_ms,
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
    int remaining = 0;
    cc_result_t rc = remaining_call_ms(deadline_ms, cancel_token, &remaining);
    while (rc.code == CC_OK && server->needs_reset && server->active_calls > 0) {
        cc_cond_timedwait(server->cond, server->mutex, remaining > 50 ? 50 : remaining);
        rc = remaining_call_ms(deadline_ms, cancel_token, &remaining);
    }
    if (rc.code == CC_OK && server_idle_expired(server, cc_platform_monotonic_ms()))
        server->needs_reset = 1;
    if (rc.code == CC_OK && server->needs_reset) {
        if (!server->transport.vtable || !server->transport.vtable->reset) {
            rc = cc_result_error(CC_ERR_UNSUPPORTED, "MCP transport cannot reset this connection");
        } else {
            rc = server->transport.vtable->reset(server->transport.self);
            if (rc.code == CC_OK) {
                cc_result_free(&rc);
                server->needs_initialize = 1;
                server->needs_reset = 0;
            }
        }
    }
    if (rc.code == CC_OK) rc = ensure_initialized_locked(server, deadline_ms, cancel_token);
    if (rc.code == CC_OK) rc = remaining_call_ms(deadline_ms, cancel_token, &remaining);
    if (rc.code != CC_OK) {
        cc_json_destroy(params);
        cc_mutex_unlock(server->mutex);
        return rc;
    }
    uint64_t id = 0;
    char *request = NULL;
    rc = next_request_locked(server, method, params, &id, &request);
    if (rc.code != CC_OK) {
        cc_mutex_unlock(server->mutex);
        return rc;
    }
    rc = remaining_call_ms(deadline_ms, cancel_token, &remaining);
    if (rc.code != CC_OK) {
        free(request);
        cc_mutex_unlock(server->mutex);
        return rc;
    }
    int serial = server_transport_is_serial(server);
    server->active_calls++;
    if (!serial) cc_mutex_unlock(server->mutex);
    rc = send_request_and_parse(server, id, request, remaining, cancel_token, out_response);
    if (rc.code == CC_OK) {
        rc = remaining_call_ms(deadline_ms, cancel_token, &remaining);
        if (rc.code != CC_OK) {
            cc_json_destroy(*out_response);
            *out_response = NULL;
        }
    }
    if (!serial) cc_mutex_lock(server->mutex);
    server->active_calls--;
    server->last_used_ms = cc_platform_monotonic_ms();
    if (rc.code != CC_OK) server->needs_reset = 1;
    cc_cond_broadcast(server->cond);
    cc_mutex_unlock(server->mutex);
    return rc;
}

/**
 * @brief 释放 server 拥有的传输、同步资源和名称副本。
 * @param[in,out] server 消费的独占 server，允许 NULL。
 * @pre 所有工具调用、初始化和条件等待者已退出，外层按 registry 后 manager 的顺序清理。
 * @post 非空 server 及其拥有资源释放，相关工具对 server 的借用结束。
 */
static void server_runtime_destroy(cc_mcp_server_runtime_t *server)
{
    if (!server) return;
    if (server->transport.vtable && server->transport.vtable->destroy) {
        server->transport.vtable->destroy(server->transport.self);
    }
    if (server->cond) cc_cond_destroy(server->cond);
    if (server->mutex) cc_mutex_destroy(server->mutex);
    free(server->name);
    free(server->transport_name);
    free(server);
}

/**
 * @brief 创建持有传输工厂借用及 server 表的管理器。
 * @param[in] factory 必填同步工厂，后续加载时调用。
 * @param[in] factory_user_data 工厂上下文借用，允许 NULL，覆盖管理器加载调用期。
 * @param[out] out_manager 必填，成功交付需 destroy 的拥有对象，失败保持原值。
 * @return CC_OK、参数错误或分配失败，消息由调用者释放。
 */
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

/**
 * @brief 销毁管理器拥有的全部 server 及传输实例。
 * @param[in,out] manager 消费独占管理器，允许 NULL。
 * @pre owner 已关闭新调用并等待已接纳使用者退出，先销毁借用 server 的 registry 工具。
 * @post 管理器、server 表和全部传输资源释放，原句柄失效。
 */
void cc_mcp_runtime_manager_destroy(cc_mcp_runtime_manager_t *manager)
{
    if (!manager) return;
    for (size_t i = 0; i < manager->server_count; i++) {
        server_runtime_destroy(manager->servers[i]);
    }
    free(manager->servers);
    free(manager);
}

/**
 * @brief 返回向模型和 registry 导出的编码工具名。
 * @param[in] self 借用有效 MCP 工具实例。
 * @return 工具拥有的只读字符串借用，寿命至工具销毁。
 */
static const char *mcp_tool_name(void *self)
{
    return ((cc_mcp_tool_t *)self)->display_name;
}

/**
 * @brief 返回远端工具的描述副本。
 * @param[in] self 借用有效 MCP 工具实例。
 * @return 工具拥有的只读描述借用，寿命至工具销毁。
 */
static const char *mcp_tool_description(void *self)
{
    return ((cc_mcp_tool_t *)self)->description;
}

/**
 * @brief 返回参数 schema 的紧凑 JSON 副本。
 * @param[in] self 借用有效 MCP 工具实例。
 * @return 工具拥有的只读 schema 借用，寿命至工具销毁。
 */
static const char *mcp_tool_schema_json(void *self)
{
    return ((cc_mcp_tool_t *)self)->schema_json;
}

/**
 * @brief 完整验证参数对象，调用远端原始工具名，并转换两类 MCP 错误。
 * @param[in,out] self 借用有效工具及 server，外层保持 registry 和 manager 覆盖整个调用。
 * @param[in] args_json 借用完整参数对象文本，NULL 使用空对象。
 * @param[in] ctx 可选借用上下文，其取消令牌和超时用于握手及调用。
 * @param[out] out_result 必填且无待释放资源，基础参数有效后清零，拥有正文/错误由调用者 cleanup。
 * @return 参数或分配错误返回非 OK，传输和远端错误通常转换为 CC_OK 加 ok=0，仍检查完整输出。
 * @note isError=true 的 result 转为业务失败并保留序列化正文。实际请求发出后的任何失败
 *       由外层账本继续保管副作用状态，内部重连只服务后续请求。
 */
static cc_result_t mcp_tool_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    if (!self || !out_result)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid MCP tool invocation");
    cc_mcp_tool_t *tool = (cc_mcp_tool_t *)self;
    memset(out_result, 0, sizeof(*out_result));
    cc_cancel_token_t *token = ctx ? ctx->cancel_token : NULL;
    int timeout_ms = ctx && ctx->timeout_ms > 0 ? ctx->timeout_ms : 0;
    uint64_t deadline_ms = call_deadline_ms(tool->server, timeout_ms);
    cc_json_value_t *arguments = NULL;
    cc_result_t rc = parse_json_object(args_json ? args_json : "{}", &arguments);
    if (rc.code != CC_OK) return rc;
    cc_json_value_t *params = cc_json_create_object();
    if (!params || !cc_json_object_set_owned(params, "name", cc_json_create_string(tool->tool_name))) {
        cc_json_destroy(arguments);
        cc_json_destroy(params);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build MCP tool params");
    }
    if (!cc_json_object_set_owned(params, "arguments", arguments)) {
        cc_json_destroy(params);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to attach MCP tool arguments");
    }

    cc_json_value_t *response = NULL;
    rc = mcp_call_raw(
        tool->server, "tools/call", params, deadline_ms, token, &response);
    if (rc.code != CC_OK) {
        out_result->ok = 0;
        out_result->error = cc_copy_string(rc.message ? rc.message : "MCP tool call failed");
        cc_result_free(&rc);
        return out_result->error ? cc_result_ok() :
            cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy MCP tool error");
    }

    cc_json_value_t *error = cc_json_object_get_case_sensitive(response, "error");
    if (error) {
        char *err = cc_json_stringify_unformatted(error);
        out_result->ok = 0;
        out_result->error = err ? err : cc_copy_string("MCP server returned an error");
        cc_json_destroy(response);
        return out_result->error ? cc_result_ok() :
            cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy MCP protocol error");
    }

    cc_json_value_t *result = cc_json_object_get_case_sensitive(response, "result");
    cc_json_value_t *is_error = cc_json_object_get_case_sensitive(result, "isError");
    cc_json_value_t *content = cc_json_object_get_case_sensitive(result, "content");
    if (!cc_json_is_object(result) || !cc_json_is_array(content) ||
        (is_error && !cc_json_is_bool(is_error))) {
        cc_json_destroy(response);
        return cc_result_error(CC_ERR_JSON, "MCP tool result has an invalid shape");
    }
    int failed = cc_json_bool_value(is_error);
    char *body = cc_json_stringify_unformatted(result);
    cc_json_destroy(response);
    if (!body) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy MCP tool result");
    out_result->ok = !failed;
    if (out_result->ok) out_result->text = body;
    else out_result->error = body;
    return cc_result_ok();
}

/**
 * @brief 释放 MCP 工具的名称、描述、schema 与实例。
 * @param[in,out] self 消费独占工具，允许 NULL。
 * @post 实例拥有字段释放，server 的资源继续由 manager 持有。
 */
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

/**
 * @brief 将 tools/list 项适配为 SDK 工具，公开名分别编码 server 与原工具名的点号和百分号。
 * @param[in,out] registry 借用未冻结注册表，成功接管本次工具 self。
 * @param[in] server 借用有效 server，复制其名称，server 存活至 registry 工具使用结束。
 * @param[in] tool_json 借用远端工具描述，缺失或空名称时跳过，不保留 JSON 树指针。
 * @return CC_OK 或编码/分配/注册错误，失败释放本次已创建的工具字段及 self。
 * @note 公开名按 mcp.编码来源.编码工具组成，tools/call 仍发送单独保存的原始 tool_name。
 */
static cc_result_t register_mcp_tool(
    cc_tool_registry_t *registry,
    cc_mcp_server_runtime_t *server,
    cc_json_value_t *tool_json
)
{
    const char *name = cc_json_string_value(cc_json_object_get_case_sensitive(tool_json, "name"));
    if (!name || !name[0]) return cc_result_ok();
    const char *description = cc_json_string_value(cc_json_object_get_case_sensitive(tool_json, "description"));
    cc_json_value_t *schema = cc_json_object_get_case_sensitive(tool_json, "inputSchema");
    if (!schema) schema = cc_json_object_get_case_sensitive(tool_json, "parameters");
    if (schema && !cc_json_is_object(schema))
        return cc_result_error(CC_ERR_JSON, "MCP tool schema must be an object");

    cc_mcp_tool_t *tool_self = calloc(1, sizeof(*tool_self));
    if (!tool_self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create MCP tool");
    tool_self->tool_name = cc_copy_string(name);
    tool_self->description = cc_copy_string(description ? description : "MCP tool");
    tool_self->schema_json = schema ? cc_json_stringify_unformatted(schema) :
        cc_copy_string("{\"type\":\"object\",\"properties\":{}}");
    cc_result_t rc = cc_tool_name_build("mcp", server->name, name, &tool_self->display_name);
    if (rc.code != CC_OK) {
        mcp_tool_destroy(tool_self);
        return rc;
    }
    tool_self->server = server;

    if (!tool_self->tool_name || !tool_self->display_name ||
        !tool_self->description || !tool_self->schema_json) {
        mcp_tool_destroy(tool_self);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy MCP tool metadata");
    }

    cc_tool_t existing = {0};
    rc = cc_tool_registry_find(registry, tool_self->display_name, &existing);
    if (rc.code == CC_OK) {
        mcp_tool_destroy(tool_self);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Duplicate MCP tool name");
    }
    if (rc.code != CC_ERR_NOT_FOUND) {
        mcp_tool_destroy(tool_self);
        return rc;
    }
    cc_result_free(&rc);
    cc_tool_t tool = {0};
    tool.self = tool_self;
    tool.vtable = &mcp_tool_vtable;
    rc = cc_tool_registry_add(registry, tool);
    if (rc.code != CC_OK) {
        mcp_tool_destroy(tool_self);
    }
    return rc;
}

/**
 * @brief 在统一连接预算内分页读取 tools/list，并按编码名称移交工具。
 * @param[in,out] server 借用存活 server，按普通调用状态机完成重连及请求。
 * @param[in,out] registry 借用独占装配的未冻结表，成功工具由其拥有。
 * @return CC_OK 或请求、结构、重复 cursor、注册及分配错误，已注册工具继续由 registry 持有。
 * @note 保存已使用的 opaque cursor 以识别循环，循环与分配失败均停止本 server 列举。
 *       所有页共用一次截止时间，临时响应、cursor 副本及表在返回前释放。
 */
static cc_result_t list_server_tools(
    cc_mcp_server_runtime_t *server,
    cc_tool_registry_t *registry
)
{
    char **cursors = NULL;
    size_t cursor_count = 0;
    cc_json_value_t *response = NULL;
    uint64_t deadline_ms = call_deadline_ms(server, 0);
    cc_result_t rc = cc_result_ok();
    for (;;) {
        int remaining = 0;
        rc = remaining_call_ms(deadline_ms, NULL, &remaining);
        if (rc.code != CC_OK) break;
        cc_json_value_t *params = cc_json_create_object();
        if (!params) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build MCP tools/list params");
            break;
        }
        if (cursor_count > 0 &&
            !cc_json_object_set_owned(params, "cursor", cc_json_create_string(cursors[cursor_count - 1]))) {
            cc_json_destroy(params);
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy MCP tools/list cursor");
            break;
        }
        rc = mcp_call_raw(server, "tools/list", params, deadline_ms, NULL, &response);
        if (rc.code != CC_OK) break;
        cc_json_value_t *error = cc_json_object_get_case_sensitive(response, "error");
        if (error) {
            char *message = cc_json_stringify_unformatted(error);
            rc = cc_result_error(CC_ERR_TOOL, message ? message : "MCP tools/list returned an error");
            free(message);
            break;
        }
        cc_json_value_t *result = cc_json_object_get_case_sensitive(response, "result");
        cc_json_value_t *tools = cc_json_object_get_case_sensitive(result, "tools");
        if (!cc_json_is_object(result) || !cc_json_is_array(tools)) {
            rc = cc_result_error(CC_ERR_JSON, "MCP tools/list result must contain a tools array");
            break;
        }
        int count = cc_json_array_size(tools);
        for (int i = 0; i < count; i++) {
            rc = register_mcp_tool(registry, server, cc_json_array_get(tools, i));
            if (rc.code != CC_OK) break;
        }
        if (rc.code != CC_OK) break;
        cc_json_value_t *next = cc_json_object_get_case_sensitive(result, "nextCursor");
        if (!next) break;
        const char *cursor = cc_json_string_value(next);
        if (!cursor) {
            rc = cc_result_error(CC_ERR_JSON, "MCP tools/list cursor must be a string");
            break;
        }
        for (size_t i = 0; i < cursor_count; i++) {
            if (strcmp(cursor, cursors[i]) == 0) {
                rc = cc_result_error(CC_ERR_JSON, "MCP tools/list cursor repeated");
                break;
            }
        }
        if (rc.code != CC_OK) break;
        if (cursor_count >= SIZE_MAX / sizeof(*cursors)) {
            rc = cc_result_error(CC_ERR_LIMIT_EXCEEDED, "MCP cursor table size overflow");
            break;
        }
        char **grown = realloc(cursors, (cursor_count + 1) * sizeof(*cursors));
        if (!grown) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow MCP cursor table");
            break;
        }
        cursors = grown;
        cursors[cursor_count] = cc_copy_string(cursor);
        if (!cursors[cursor_count]) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to save MCP tools/list cursor");
            break;
        }
        cursor_count++;
        cc_json_destroy(response);
        response = NULL;
    }
    cc_json_destroy(response);
    for (size_t i = 0; i < cursor_count; i++) free(cursors[i]);
    free(cursors);
    return rc;
}

/**
 * @brief 扩展 server 表并在成功后接管一个 server。
 * @param[in,out] manager 独占加载阶段的管理器。
 * @param[in,out] server 成功时移交的独占实例，失败仍归调用者。
 * @return CC_OK、尺寸溢出的 LIMIT_EXCEEDED 或 realloc 的 OOM，失败保持原表及拥有关系。
 */
static cc_result_t manager_add_server(
    cc_mcp_runtime_manager_t *manager,
    cc_mcp_server_runtime_t *server
)
{
    if (manager->server_count >= SIZE_MAX / sizeof(*manager->servers))
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "MCP server table size overflow");
    cc_mcp_server_runtime_t **next = realloc(
        manager->servers, (manager->server_count + 1) * sizeof(*next));
    if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow MCP server table");
    manager->servers = next;
    manager->servers[manager->server_count++] = server;
    return cc_result_ok();
}

/**
 * @brief 调用传输工厂，复制名称并创建 server 同步资源。
 * @param[in] manager 借用管理器及工厂上下文。
 * @param[in] config 调用期间借用的 server 配置，名称缺失或为空时跳过。
 * @param[in] idle_ttl_ms 空闲连接 TTL，非正值停用自动空闲重置。
 * @param[out] out_server 必填，先置 NULL，成功移交需 server_runtime_destroy 的实例。
 * @return 返回工厂、分配或同步资源结果，跳过时 CC_OK 和 NULL，失败清理已交付的资源。
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
    cc_result_free(&rc);
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
    if (rc.code == CC_OK) rc = cc_cond_create(&server->cond);
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

/**
 * @brief 独占装配阶段连接各 server，完成握手并把工具移交 registry。
 * @param[in,out] manager 借用管理器，NULL 时跳过，成功接管 server。
 * @param[in] config 借用调用期配置，NULL 或 MCP 关闭时跳过。
 * @param[in,out] registry 借用未冻结表，NULL 时跳过，成功接管工具。
 * @param[in,out] diagnostics 可选诊断，单 server 的连接、握手或工具列举错误追加后继续。
 * @return CC_OK 或 server 表增长错误，部分资源可能已装配，调用者按 registry 后 manager 清理。
 * @pre owner 独占 manager 加载与 registry 装配，工具调用在完整装配后开始。
 */
cc_result_t cc_mcp_runtime_manager_load_tools(
    cc_mcp_runtime_manager_t *manager,
    const cc_config_t *config,
    cc_tool_registry_t *registry,
    cc_runtime_diagnostics_t *diagnostics
)
{
    if (!manager || !config || !registry || !config->mcp.enabled) return cc_result_ok();
    if (config->mcp.server_count && !config->mcp.servers)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "MCP server count requires an array");
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

        cc_mutex_lock(server->mutex);
        rc = ensure_initialized_locked(server, call_deadline_ms(server, 0), NULL);
        cc_mutex_unlock(server->mutex);
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
