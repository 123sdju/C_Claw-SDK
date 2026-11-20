



#include "cc/adapters/cc_builtin_tools.h"
#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_http_client.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/internal/cc_json_fields.h"
#include "cc/internal/cc_json_syntax_check.h"
#include "cc/util/cc_network_policy.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * 工具拥有不可变策略快照，借用 HTTP 描述符指针。HTTP owner 覆盖全部调用和回调，
 * 工具销毁归还允许列表，不销毁 HTTP。缺省无允许项，网络访问由策略拒绝。
 */
typedef struct {
    cc_http_client_t *http_client;
    cc_network_policy_t network_policy;
} cc_http_request_tool_t;

/** @brief 复核每次传输 URL。 @param[in] url 同步借用 URL。 @param[in] user_data 存活工具借用。 @return 策略允许为一，缺工具或不允许为零。 */
static int validate_tool_url(const char *url, void *user_data)
{
    cc_http_request_tool_t *tool = (cc_http_request_tool_t *)user_data;
    return tool && cc_network_policy_url_allowed(&tool->network_policy, url);
}

/** @brief 核对 DNS 候选或已连接地址。 @param[in] host 调用期主机借用。 @param[in] numeric_address 调用期数值地址借用。 @param[in] user_data 存活工具借用。 @return 地址策略允许为一，其他为零。 */
static int validate_tool_address(
    const char *host,
    const char *numeric_address,
    void *user_data)
{
    cc_http_request_tool_t *tool = (cc_http_request_tool_t *)user_data;
    return tool && cc_network_policy_address_allowed(
        &tool->network_policy, host, numeric_address);
}

/** @brief 提供稳定注册名。 @param[in] self 忽略的状态借用，可空。 @return 静态只读名称。 */
static const char *http_req_name(void *self) { (void)self; return "http.request"; }


/** @brief 提供模型说明。 @param[in] self 忽略的状态借用，可空。 @return 静态只读描述。 */
static const char *http_req_description(void *self) { (void)self; return "Make HTTP requests"; }


/** @brief 声明 URL 及可选方法、正文和整数毫秒预算。 @param[in] self 忽略的状态借用，可空。 @return 工具寿命内稳定的 JSON Schema 文本。 */
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

/** @brief 消费允许列表及各字符串。 @param[in,out] list 可空独占列表，非零 count 须有对应拥有数组。 @post 字段归还且列表清零，无锁，caller 排空借用。 */
static void free_allowlist(cc_config_string_list_t *list)
{
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

/**
 * @brief 在未发布目标中建立独立允许列表，保留 NULL 项。
 * @param[in] src 可空稳定列表借用，外层已检查 count/items 与乘法容量。
 * @param[out] dst 必填新槽，先清零，成功拥有数组和字符串。
 * @return 成功为一，分配失败清理部分副本并返回零，源始终由 caller 保管。
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

/** @brief 委托允许列表纯检查。 @param[in] network_allowlist 稳定列表借用，NULL/空拒绝。 @param[in] url 调用期 URL 借用。 @return 命中为一，其他为零，旧允许列表入口保留显式列入私网的语义。 */
int cc_http_request_url_allowed(
    const cc_config_string_list_t *network_allowlist,
    const char *url
)
{
    return cc_network_allowlist_url_allowed(network_allowlist, url);
}

/**
 * @brief 严格校验参数并通过网络策略派发一次禁止自动重试的同步 HTTP 请求。
 * @param[in] self 必填存活工具借用，HTTP 及不可变策略覆盖 perform 和全部回调。
 * @param[in] args_json 调用期完整对象，url 必填非空字符串，method/body 可选字符串，timeout_ms 可选可表示整数。
 * @param[in] ctx 可选调用期上下文，传递取消令牌，正预算限制请求参数所选超时。
 * @param[out] out_result 必填新对象或已 cleanup 槽，先清零，成功及业务失败拥有字段均交 caller cleanup。
 * @return 基础地址错误为 INVALID_ARGUMENT，结果构造 OOM 返回 OOM，参数/策略/传输/正文表示或非 2xx 通过 CC_OK 与 ok=0 反馈，诊断交 caller。
 * @note 参数树保持至 perform 返回，body_size 为正文权威长度，正值精确复制并拒绝 raw NUL，零值输出空正文且不读取 body。
 * 传输或结果失败可能已有远端副作用，工具不重试，owner 关闭入口并排空调用后销毁。
 */
static cc_result_t http_req_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    if (!out_result) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null HTTP tool result");
    memset(out_result, 0, sizeof(cc_tool_result_t));
    if (!self) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null HTTP tool");
    cc_http_request_tool_t *tool = (cc_http_request_tool_t *)self;
    const char *error = "Failed to parse arguments JSON";
    const char *oom_message = "Failed to allocate HTTP tool result";
    cc_json_value_t *args = NULL;
    cc_json_value_t *result = NULL;
    char *body_copy = NULL;
    cc_http_response_t response = {0};
    cc_result_t rc = cc_result_ok();
    if (!args_json || cc_storage_json_has_nul_escape(args_json)) goto error;
    rc = cc_json_parse_strict(args_json, &args);
    if (rc.code != CC_OK) goto error;
    const char *fields[] = {"url", "method", "body", "timeout_ms"};
    if (!cc_json_is_object(args) || cc_json_duplicate_known_field(args, fields, 4)) {
        error = "HTTP arguments must be an object without duplicate known fields";
        goto error;
    }
    const char *url = cc_json_string_value(cc_json_object_get(args, "url"));
    cc_json_value_t *method_value = cc_json_object_get(args, "method");
    cc_json_value_t *body_value = cc_json_object_get(args, "body");
    cc_json_value_t *timeout_value = cc_json_object_get(args, "timeout_ms");
    const char *method = cc_json_string_value(method_value);
    const char *body = cc_json_string_value(body_value);
    long timeout_ms = 0;
    if (!url || !url[0] || (method_value && !method) || (body_value && !body)) {
        error = "url must be a nonempty string and method/body must be strings when present";
        goto error;
    }
    if (timeout_value) {
        double number = cc_json_number_value(timeout_value);
        if (!cc_json_is_number(timeout_value) || !(number >= (double)LONG_MIN) ||
            number >= -(double)LONG_MIN) {
            error = "timeout_ms must be an integer representable as long";
            goto error;
        }
        timeout_ms = (long)number;
        if ((double)timeout_ms != number) {
            error = "timeout_ms must be an integer representable as long";
            goto error;
        }
    }
    if (!cc_network_policy_url_allowed(&tool->network_policy, url)) {
        error = "Network access denied by http.request allowlist";
        goto error;
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
    if (ctx && ctx->timeout_ms > 0 && request.timeout_ms > ctx->timeout_ms)
        request.timeout_ms = ctx->timeout_ms;
    request.cancel_token = ctx ? ctx->cancel_token : NULL;
    /* 通用工具不能挤占 LLM 连接池，也不能因 POST/自定义方法重复执行而自动重试。 */
    request.connection_pool = "default";
    request.header_profile = body ? "tool-json-body" : "tool-empty";
    request.retry_count = -1;
    request.max_response_bytes = 16U * 1024U;
    request.max_redirects = 5;
    request.validate_url = validate_tool_url;
    request.validate_url_user_data = tool;
    request.validate_address = validate_tool_address;
    request.validate_address_user_data = tool;

    cc_result_free(&rc);
    oom_message = "HTTP request may have completed, but result allocation failed";
    rc = cc_http_client_perform(tool->http_client, &request, &response);
    if (rc.code != CC_OK) {
        error = rc.message ? rc.message : "HTTP request failed";
        goto error;
    }
    if (response.body_size) {
        if (!response.body || response.body_size == SIZE_MAX) {
            error = "HTTP response body has an invalid size";
            goto error;
        }
        if (memchr(response.body, '\0', response.body_size)) {
            error = "HTTP response contains an embedded NUL and cannot be represented as text; action may have completed";
            goto error;
        }
        body_copy = malloc(response.body_size + 1);
        if (!body_copy) goto oom;
        memcpy(body_copy, response.body, response.body_size);
        body_copy[response.body_size] = '\0';
    }
    result = cc_json_create_object();
    if (!result || !cc_json_object_set_owned(result, "status", cc_json_create_number((double)response.status_code)) ||
        !cc_json_object_set_owned(result, "body", cc_json_create_string(body_copy ? body_copy : "")))
        goto oom;
    out_result->text = cc_json_stringify_unformatted(result);
    if (!out_result->text) goto oom;
    out_result->ok = (response.status_code >= 200 && response.status_code < 300) ? 1 : 0;
    if (!out_result->ok) {
        out_result->error = cc_copy_string("HTTP status outside 2xx");
        if (!out_result->error) goto oom;
    }
    cc_result_free(&rc);
    rc = cc_result_ok();
    goto done;

error:
    out_result->error = cc_copy_string(error);
    if (!out_result->error) goto oom;
    cc_result_free(&rc);
    rc = cc_result_ok();
    goto done;

oom:
    cc_result_free(&rc);
    cc_tool_result_cleanup(out_result);
    rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, oom_message);

done:
    cc_json_destroy(args);
    cc_json_destroy(result);
    free(body_copy);
    cc_http_response_free(&response);
    return rc;
}

/** @brief 消费工具及策略副本。 @param[in,out] self 独占状态，可空。 @pre owner 已关闭入口并排空 perform 与回调。 @post 状态及允许列表归还，HTTP 描述符继续归其 owner。 */
static void http_req_destroy(void *self)
{
    cc_http_request_tool_t *tool = (cc_http_request_tool_t *)self;
    if (!tool) return;
    free_allowlist(&tool->network_policy.allowlist);
    free(tool);
}

/* 静态方法表覆盖工具整个生命周期。 */
static cc_tool_vtable_t http_req_vtable = {
    http_req_name,
    http_req_description,
    http_req_schema_json,
    http_req_call,
    http_req_destroy
};

/**
 * @brief 用空允许列表创建缺省拒绝网络的 HTTP 工具。
 * @param[in] http_client 存活 HTTP 描述符借用，须支持已连接地址验证并覆盖工具寿命。
 * @param[out] out_tool 必填空描述符，初始化与拥有规则沿完整策略工厂。
 * @return 完整策略创建结果，诊断由 caller 释放，工具不接管 HTTP 销毁。
 */
cc_result_t cc_http_request_tool_create(
    cc_http_client_t *http_client,
    cc_tool_t *out_tool)
{
    return cc_http_request_tool_create_with_allowlist(http_client, NULL, out_tool);
}

/**
 * @brief 将可选允许列表包装为策略，再创建独立快照工具。
 * @param[in] http_client 覆盖工具寿命的 HTTP 描述符借用。
 * @param[in] network_allowlist 创建期稳定列表借用，可空，非零 count 须有对应 items。
 * @param[out] out_tool 必填空描述符，早期失败输出规则沿完整策略工厂。
 * @return 参数、能力、容量或分配结果，诊断交 caller，配置副本成功后不再借用原列表。
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

/**
 * @brief 验证端口及允许列表形状，复制策略后发布拥有工具。
 * @param[in] http_client 必填存活端口借用，须有方法表和已连接地址校验能力，使用期由 owner 保持。
 * @param[in] network_policy 可空创建期借用，列表非零 count 须有 items，NULL 项原样保留，计数须可分配。
 * @param[out] out_tool 必填空描述符，早期参数/能力/容量失败保留原值，其后清零，成功 self 须 destroy 或注册交接。
 * @return 参数、UNSUPPORTED、LIMIT_EXCEEDED 或 OOM，失败归还本次副本，诊断交 caller。
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
    if (network_policy && network_policy->allowlist.count && !network_policy->allowlist.items)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP allowlist has no items");
    if (network_policy && network_policy->allowlist.count > SIZE_MAX / sizeof(char *))
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "HTTP allowlist size exceeds addressable capacity");
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
