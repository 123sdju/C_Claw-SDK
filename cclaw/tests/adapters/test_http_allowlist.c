#include "cc/adapters/cc_builtin_tools.h"

#include <string.h>

/*
 * 验证 http.request 工具暴露出来的 allowlist 判断逻辑。
 *
 * 这个测试不真正发起 HTTP 请求，而是把网络安全策略当作纯函数测试：
 * 1. 未配置 allowlist 时默认拒绝，符合 SDK “显式允许才联网”的安全默认值。
 * 2. host、host:port、通配子域、scheme://host 和 scheme://host:port 都要按契约匹配。
 * 3. userinfo、未知 scheme、端口/scheme 不匹配都必须拒绝，防止 URL 解析绕过。
 * 4. localhost/private IP 默认拒绝，但在旧兼容 wrapper 的显式 allowlist 中仍可放行；
 *    新的 cc_network_policy_t 会继续执行私网默认拒绝，更适合实际嵌入式网关场景。
 *
 * 嵌入式面试中可以把这里讲成“安全策略与传输实现解耦”：MCU/RTOS 上即使 HTTP
 * client 换成 lwIP 或 esp_http_client，URL 访问边界仍由同一个策略函数控制。
 */
int main(void)
{
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
