



#include "cc/adapters/cc_builtin_tools.h"
#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_http_client.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_network_policy.h"
#include <stdlib.h>
#include <string.h>

/*
 * http.request 工具私有对象。
 *
 * network_policy 是深拷贝后的策略快照，工具销毁时释放 allowlist。默认策略是 deny，
 * 因此没有显式 allowlist 时不会访问外网，符合核心 SDK “安全优先、业务 gateway 外置”
 * 的边界。
 */
typedef struct {
    cc_http_client_t *http_client;
    cc_network_policy_t network_policy;
} cc_http_request_tool_t;

static int validate_tool_url(const char *url, void *user_data)
{
    cc_http_request_tool_t *tool = (cc_http_request_tool_t *)user_data;
    return tool && cc_network_policy_url_allowed(&tool->network_policy, url);
}

static int validate_tool_address(
    const char *host,
    const char *numeric_address,
    void *user_data)
{
    cc_http_request_tool_t *tool = (cc_http_request_tool_t *)user_data;
    return tool && cc_network_policy_address_allowed(
        &tool->network_policy, host, numeric_address);
}

/* 返回工具名；使用带点号的命名空间，方便未来和其它 HTTP-capable 工具区分。 */
static const char *http_req_name(void *self) { (void)self; return "http.request"; }


/* 返回工具说明；静态字符串只读使用，不需要调用方释放。 */
static const char *http_req_description(void *self) { (void)self; return "Make HTTP requests"; }


/*
 * 返回 http.request 的参数 schema。
 *
 * url 必填，method/body/timeout_ms 可选；tool executor 的 schema 校验能先挡住缺少 url
 * 这类基础错误，具体 URL 安全仍由 network policy 在执行期检查。
 */
static const char *http_req_schema_json(void *self)
{
    (void)self;
    return "{"
        "\"type\":\"object\","
        "\"properties\":{"
            "\"url\":{\"type\":\"string\",\"description\":\"URL to request\"},"
            "\"method\":{\"type\":\"string\",\"description\":\"HTTP method\"},"
            "\"body\":{\"type\":\"string\",\"description\":\"Optional request body\"},"
            "\"timeout_ms\":{\"type\":\"integer\",\"description\":\"Request timeout in milliseconds\"}"
        "},"
        "\"required\":[\"url\"]"
    "}";
}

/* 释放 allowlist 深拷贝数组；每个 entry 字符串由工具对象拥有。 */
static void free_allowlist(cc_config_string_list_t *list)
{
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

/*
 * 深拷贝 allowlist。
 *
 * 创建工具时不能直接借用 config 中的 allowlist，因为 config 可能在 reload 后销毁；
 * 工具持有自己的策略快照，保证运行中的 tool call 不受外部配置内存生命周期影响。
 */
static int copy_allowlist(
    const cc_config_string_list_t *src,
    cc_config_string_list_t *dst
)
{
    memset(dst, 0, sizeof(*dst));
    if (!src || src->count == 0) return 1;
    dst->items = calloc(src->count, sizeof(char *));
    if (!dst->items) return 0;
    dst->count = src->count;
    for (size_t i = 0; i < src->count; i++) {
        dst->items[i] = src->items[i] ? cc_copy_string(src->items[i]) : NULL;
        if (src->items[i] && !dst->items[i]) {
            free_allowlist(dst);
            return 0;
        }
    }
    return 1;
}

/*
 * 公开的 URL allowlist 检查 helper。
 *
 * 测试和下游工具可以复用同一套匹配逻辑；返回 1 表示允许，0 表示拒绝。默认空列表拒绝，
 * localhost/private IP 也会由网络策略层按规则拦截。
 */
int cc_http_request_url_allowed(
    const cc_config_string_list_t *network_allowlist,
    const char *url
)
{
    return cc_network_allowlist_url_allowed(network_allowlist, url);
}

/*
 * 执行 HTTP 请求工具。
 *
 * 函数把 JSON 参数转换成 cc_http_request_t，然后交给统一 HTTP client 端口。网络访问
 * 在构造 request 前必须先通过 cc_network_policy_url_allowed；HTTP 错误和非 2xx 状态
 * 都作为可恢复 tool error 返回给模型，SDK 不在这里做 retry/backoff。
 */
static cc_result_t http_req_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    cc_http_request_tool_t *tool = (cc_http_request_tool_t *)self;
    (void)ctx;
    memset(out_result, 0, sizeof(cc_tool_result_t));

    cc_json_value_t *args = NULL;
    cc_result_t rc = cc_json_parse(args_json, &args);
    if (rc.code != CC_OK || !args) {
        out_result->ok = 0;
        out_result->error = cc_copy_string("Failed to parse arguments JSON");
        cc_result_free(&rc);
        return cc_result_ok();
    }

    const char *url = cc_json_string_value(cc_json_object_get(args, "url"));
    const char *method = cc_json_string_value(cc_json_object_get(args, "method"));
    const char *body = cc_json_string_value(cc_json_object_get(args, "body"));
    int timeout_ms = cc_json_int_value(cc_json_object_get(args, "timeout_ms"));

    if (!url || strlen(url) == 0) {
        out_result->ok = 0;
        out_result->error = cc_copy_string("Missing required parameter: url");
        cc_json_destroy(args);
        return cc_result_ok();
    }

    if (!cc_network_policy_url_allowed(&tool->network_policy, url)) {
        out_result->ok = 0;
        out_result->error = cc_copy_string("Network access denied by http.request allowlist");
        cc_json_destroy(args);
        return cc_result_ok();
    }

    cc_http_header_t headers[1] = {
        {"Content-Type", "application/json"}
    };

    cc_http_request_t request;
    memset(&request, 0, sizeof(request));
    request.method = method ? method : (body ? "POST" : "GET");
    request.url = url;
    request.headers = body ? headers : NULL;
    request.header_count = body ? 1 : 0;
    request.body = body;
    request.timeout_ms = timeout_ms > 0 ? timeout_ms :
        (ctx && ctx->timeout_ms > 0 ? ctx->timeout_ms : 30000);
    request.cancel_token = ctx ? ctx->cancel_token : NULL;
    /* 通用工具不能挤占 LLM 连接池，也不能因 POST/自定义方法重复执行而自动重试。 */
    request.connection_pool = "default";
    request.header_profile = body ? "tool-json-body" : "tool-empty";
    request.retry_count = 0;
    request.max_response_bytes = 16U * 1024U;
    request.max_redirects = 5;
    request.validate_url = validate_tool_url;
    request.validate_url_user_data = tool;
    request.validate_address = validate_tool_address;
    request.validate_address_user_data = tool;

    cc_http_response_t response;
    rc = cc_http_client_perform(tool->http_client, &request, &response);
    cc_json_destroy(args);

    if (rc.code != CC_OK) {
        out_result->ok = 0;
        out_result->error = cc_copy_string(rc.message ? rc.message : "HTTP request failed");
        cc_result_free(&rc);
        return cc_result_ok();
    }

    cc_json_value_t *result = cc_json_create_object();
    cc_json_object_set(result, "status", cc_json_create_number((double)response.status_code));
    cc_json_object_set(result, "body", cc_json_create_string(response.body ? response.body : ""));
    out_result->text = cc_json_stringify_unformatted(result);
    cc_json_destroy(result);

    out_result->ok = (response.status_code >= 200 && response.status_code < 300) ? 1 : 0;
    if (!out_result->ok) {
        out_result->error = cc_copy_string("HTTP status outside 2xx");
    }
    cc_http_response_free(&response);
    return cc_result_ok();
}

/* 销毁 http.request 工具对象，并释放其私有 allowlist 深拷贝。 */
static void http_req_destroy(void *self)
{
    cc_http_request_tool_t *tool = (cc_http_request_tool_t *)self;
    if (!tool) return;
    free_allowlist(&tool->network_policy.allowlist);
    free(tool);
}

/* http.request 的工具 vtable，供 registry 和 tool executor 以统一接口调用。 */
static cc_tool_vtable_t http_req_vtable = {
    http_req_name,
    http_req_description,
    http_req_schema_json,
    http_req_call,
    http_req_destroy
};

/*
 * 创建默认 http.request 工具。
 *
 * 默认不传 allowlist，因此网络策略为 deny；应用如果确实需要开放网络，必须使用
 * allowlist/policy 版本显式配置。
 */
cc_result_t cc_http_request_tool_create(
    cc_http_client_t *http_client,
    cc_tool_t *out_tool)
{
    return cc_http_request_tool_create_with_allowlist(http_client, NULL, out_tool);
}

/*
 * 用字符串 allowlist 创建 http.request 工具。
 *
 * 该接口是便捷 wrapper，会把 allowlist 包装成 cc_network_policy_t；allowlist entry
 * 支持 host、host:port、*.domain、scheme://host 等格式，匹配规则由 util 层实现。
 */
cc_result_t cc_http_request_tool_create_with_allowlist(
    cc_http_client_t *http_client,
    const cc_config_string_list_t *network_allowlist,
    cc_tool_t *out_tool
)
{
    cc_network_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    policy.size = sizeof(policy);
    if (network_allowlist) policy.allowlist = *network_allowlist;
    return cc_http_request_tool_create_with_policy(http_client, &policy, out_tool);
}

/*
 * 用完整 network policy 创建 http.request 工具。
 *
 * 成功后 out_tool 持有工具 self/vtable，销毁时释放 policy 深拷贝。policy 只在创建期间
 * 借用，调用返回后调用方可以释放自己的配置对象。
 */
cc_result_t cc_http_request_tool_create_with_policy(
    cc_http_client_t *http_client,
    const cc_network_policy_t *network_policy,
    cc_tool_t *out_tool
)
{
    if (!http_client || !http_client->vtable || !out_tool) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null http request tool output");
    }
    if ((http_client->capabilities & CC_HTTP_CAP_RESOLVED_ADDRESS_VALIDATION) == 0) {
        return cc_result_error(CC_ERR_UNSUPPORTED,
            "HTTP request tool requires connected-address validation");
    }
    memset(out_tool, 0, sizeof(*out_tool));
    cc_http_request_tool_t *self = calloc(1, sizeof(cc_http_request_tool_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create http request tool");
    self->http_client = http_client;
    self->network_policy.size = sizeof(self->network_policy);
    if (network_policy) {
        self->network_policy.allow_private_networks = network_policy->allow_private_networks;
    }
    if (!copy_allowlist(network_policy ? &network_policy->allowlist : NULL,
            &self->network_policy.allowlist)) {
        free(self);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy http request allowlist");
    }
    out_tool->self = self;
    out_tool->vtable = &http_req_vtable;
    return cc_result_ok();
}
