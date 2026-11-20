#include "cc/adapters/cc_builtin_tools.h"

#include <string.h>
#include <assert.h>

/** @brief 真实工具入口传给 HTTP 端口的重试约定捕获槽。 */
typedef struct retry_capture {
    int calls;       /**< 端口 perform 被调用次数。 */
    int retry_count; /**< 工具构造的请求级重试值。 */
} retry_capture_t;

/**
 * @brief 捕获真实 http.request 工具构造的 POST 请求，随后注入网络错误。
 * @param[in,out] self 借用栈上的 retry_capture_t。
 * @param[in] request 仅调用期借用的工具请求，验证其方法、正文和策略回调。
 * @param[out] out_response 清零的空响应，不交付任何动态资源。
 * @return 返回拥有型 CC_ERR_NETWORK，真实工具入口负责归还错误消息。
 */
static cc_result_t capture_http_request(void *self, const cc_http_request_t *request,
    cc_http_response_t *out_response)
{
    retry_capture_t *capture = self;
    assert(request && !strcmp(request->method, "POST") && !strcmp(request->body, "payload"));
    assert(request->validate_url && request->validate_address);
    capture->calls++;
    capture->retry_count = request->retry_count;
    memset(out_response, 0, sizeof(*out_response));
    return cc_result_error(CC_ERR_NETWORK, "injected transport failure");
}

/**
 * @brief 通过真实工具创建和 call 入口验证显式禁用端口重试的负值约定。
 * @post 工具、工具结果及拥有型返回结果均已清理，借用的假端口没有动态资源。
 * @note 网络策略只对 URL 作真实判断，perform 替身不连接网络，客户端默认重试继承另由 curl 模型覆盖。
 */
static void test_tool_disables_retry(void)
{
    retry_capture_t capture = {0};
    static const cc_http_client_vtable_t vtable = {.perform = capture_http_request};
    cc_http_client_t http = {
        .self = &capture, .vtable = &vtable, .size = sizeof(http),
        .capabilities = CC_HTTP_CAP_RESOLVED_ADDRESS_VALIDATION
    };
    char *items[] = {"api.example.com"};
    cc_config_string_list_t allowlist = {items, 1};
    cc_tool_t tool = {0};
    cc_result_t rc = cc_http_request_tool_create_with_allowlist(&http, &allowlist, &tool);
    assert(rc.code == CC_OK);
    cc_result_free(&rc);
    cc_tool_result_t result = {0};
    rc = tool.vtable->call(tool.self,
        "{\"url\":\"https://api.example.com/v1\",\"method\":\"POST\",\"body\":\"payload\"}",
        NULL, &result);
    assert(rc.code == CC_OK && !result.ok && result.error);
    assert(capture.calls == 1 && capture.retry_count < 0);
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result);
    tool.vtable->destroy(tool.self);
}

/**
 * @brief 验证 HTTP 工具 URL allowlist 的主机、端口、协议和通配匹配，以及拒绝分支。
 * @return 全部判断符合预期返回 0，任一断言不符返回 1。
 * @note 列表和策略均借用栈对象，本测试只调用纯 URL 判断，不解析 DNS 或发起请求。
 *       旧入口允许显式列入的 localhost 和私网地址，策略入口另外覆盖未列入主机的拒绝。
 */
int main(void)
{
    test_tool_disables_retry();
    char *items[] = {
        "api.example.com",
        "api.example.com:8443",
        "*.trusted.local",
        "https://secure.example.com",
        "https://ports.example.com:9443",
        "localhost",
        "10.0.0.5"
    };
    cc_config_string_list_t allowlist;
    allowlist.items = items;
    allowlist.count = sizeof(items) / sizeof(items[0]);

    int failed = 0;
    if (cc_http_request_url_allowed(NULL, "https://api.example.com/v1")) failed = 1;
    if (!cc_http_request_url_allowed(&allowlist, "https://api.example.com/v1")) failed = 1;
    if (!cc_http_request_url_allowed(&allowlist, "https://api.example.com:8443/v1")) failed = 1;
    if (!cc_http_request_url_allowed(&allowlist, "http://svc.trusted.local/x")) failed = 1;
    if (!cc_http_request_url_allowed(&allowlist, "https://secure.example.com/x")) failed = 1;
    if (cc_http_request_url_allowed(&allowlist, "http://secure.example.com/x")) failed = 1;
    if (!cc_http_request_url_allowed(&allowlist, "https://ports.example.com:9443/x")) failed = 1;
    if (cc_http_request_url_allowed(&allowlist, "https://ports.example.com/x")) failed = 1;
    if (cc_http_request_url_allowed(&allowlist, "http://ports.example.com:9443/x")) failed = 1;
    if (cc_http_request_url_allowed(&allowlist, "ftp://api.example.com/x")) failed = 1;
    if (cc_http_request_url_allowed(&allowlist, "https://evil.example.com/x")) failed = 1;
    if (cc_http_request_url_allowed(&allowlist, "https://user:pass@api.example.com/x")) failed = 1;
    if (!cc_http_request_url_allowed(&allowlist, "http://localhost/status")) failed = 1;
    if (!cc_http_request_url_allowed(&allowlist, "http://10.0.0.5/status")) failed = 1;

    char *public_only_items[] = { "example.com" };
    cc_config_string_list_t public_only = { public_only_items, 1 };
    cc_network_policy_t policy = { 0 };
    policy.size = sizeof(policy);
    policy.allowlist = public_only;
    if (cc_network_policy_url_allowed(&policy, "http://localhost/status")) failed = 1;
    if (cc_network_policy_url_allowed(&policy, "http://10.0.0.5/status")) failed = 1;
    return failed ? 1 : 0;
}
