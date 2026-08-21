#include "cc/util/cc_network_policy.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct url_parts {
    char scheme[16];
    char host[256];
    int port;
    int has_userinfo;
} url_parts_t;

/* ASCII 忽略大小写比较；URL scheme/host 不依赖 locale。 */
static int ascii_case_equal(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char cb = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/*
 * 解析端口号。
 *
 * 只接受 0..65535 的十进制端口，并通过 end_out 告诉调用方解析停止位置，便于检查
 * host:port 后面是否还有非法字符。
 */
static int parse_port(const char *text, const char **end_out)
{
    int port = -1;
    if (!text || !isdigit((unsigned char)*text)) return -1;
    port = 0;
    while (*text && isdigit((unsigned char)*text)) {
        port = port * 10 + (*text - '0');
        if (port > 65535) return -1;
        text++;
    }
    if (end_out) *end_out = text;
    return port;
}

/*
 * 解析 URL 的 scheme/host/port。
 *
 * 只接受包含 scheme://authority 的 URL，拒绝 userinfo，支持 IPv6 方括号和显式端口。
 * 该函数不解析 path/query，因为 allowlist 只按网络端点做安全决策。
 */
static int parse_url_parts(const char *url, url_parts_t *out)
{
    memset(out, 0, sizeof(*out));
    out->port = -1;
    const char *sep = strstr(url ? url : "", "://");
    if (!sep || sep == url) return 0;
    size_t scheme_len = (size_t)(sep - url);
    if (scheme_len == 0 || scheme_len >= sizeof(out->scheme)) return 0;
    memcpy(out->scheme, url, scheme_len);
    out->scheme[scheme_len] = '\0';

    const char *authority = sep + 3;
    const char *authority_end = authority;
    while (*authority_end && *authority_end != '/' &&
           *authority_end != '?' && *authority_end != '#') {
        authority_end++;
    }
    if (authority_end == authority) return 0;
    for (const char *p = authority; p < authority_end; p++) {
        if (*p == '@') {
            out->has_userinfo = 1;
            return 0;
        }
    }

    const char *host_start = authority;
    const char *host_end = authority_end;
    if (*host_start == '[') {
        host_start++;
        host_end = host_start;
        while (host_end < authority_end && *host_end != ']') host_end++;
        if (host_end >= authority_end) return 0;
        if (host_end + 1 < authority_end && host_end[1] == ':') {
            const char *port_end = NULL;
            out->port = parse_port(host_end + 2, &port_end);
            if (out->port < 0 || port_end != authority_end) return 0;
        } else if (host_end + 1 != authority_end) {
            return 0;
        }
    } else {
        const char *colon = NULL;
        for (const char *p = authority; p < authority_end; p++) {
            if (*p == ':') colon = p;
        }
        if (colon) {
            host_end = colon;
            const char *port_end = NULL;
            out->port = parse_port(colon + 1, &port_end);
            if (out->port < 0 || port_end != authority_end) return 0;
        }
    }

    size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host)) return 0;
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';
    return 1;
}

/*
 * 解析 allowlist 条目。
 *
 * 条目可以是 host、host:port、*.domain、scheme://host 或 scheme://host:port；没有 scheme
 * 时只限制 host/port。无效条目直接忽略，避免配置拼写错误变成放行。
 */
static int parse_allow_entry(const char *entry, url_parts_t *out)
{
    if (!entry || !*entry) return 0;
    if (strstr(entry, "://")) return parse_url_parts(entry, out);
    memset(out, 0, sizeof(*out));
    out->port = -1;
    const char *host_start = entry;
    const char *host_end = entry + strlen(entry);
    const char *colon = strrchr(entry, ':');
    if (colon && strchr(entry, ':') == colon) {
        host_end = colon;
        const char *port_end = NULL;
        out->port = parse_port(colon + 1, &port_end);
        if (out->port < 0 || *port_end != '\0') return 0;
    }
    size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host)) return 0;
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';
    return 1;
}

/*
 * 判断 host 是否匹配模式。
 *
 * "*.domain" 只匹配子域，不匹配 domain 自身；普通模式做忽略大小写精确匹配。
 */
static int host_matches_pattern(const char *host, const char *pattern)
{
    if (!host || !pattern || !*pattern) return 0;
    if (strncmp(pattern, "*.", 2) == 0) {
        const char *suffix = pattern + 1;
        size_t host_len = strlen(host);
        size_t suffix_len = strlen(suffix);
        return host_len > suffix_len &&
            ascii_case_equal(host + host_len - suffix_len, suffix);
    }
    return ascii_case_equal(host, pattern);
}

/* 解析 dotted IPv4；成功写入 4 个字节。 */
static int parse_ipv4(const char *host, unsigned char out[4])
{
    const char *p = host;
    for (int i = 0; i < 4; i++) {
        if (!isdigit((unsigned char)*p)) return 0;
        int value = 0;
        while (isdigit((unsigned char)*p)) {
            value = value * 10 + (*p - '0');
            if (value > 255) return 0;
            p++;
        }
        out[i] = (unsigned char)value;
        if (i < 3) {
            if (*p != '.') return 0;
            p++;
        }
    }
    return *p == '\0';
}

/*
 * 判断 host 是否属于默认拒绝的本地/私有网络。
 *
 * 包含 localhost、IPv6 loopback、IPv4 loopback、RFC1918、link-local。域名不会做 DNS
 * 解析，因此这里无法发现解析后落到私网的情况，HTTP adapter 仍应在连接层保持警惕。
 */
static int is_private_host(const char *host)
{
    if (!host) return 1;
    if (ascii_case_equal(host, "localhost") ||
        ascii_case_equal(host, "::1") ||
        ascii_case_equal(host, "0:0:0:0:0:0:0:1")) {
        return 1;
    }
    unsigned char ip[4];
    if (!parse_ipv4(host, ip)) return 0;
    if (ip[0] == 10) return 1;
    if (ip[0] == 127) return 1;
    if (ip[0] == 169 && ip[1] == 254) return 1;
    if (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) return 1;
    if (ip[0] == 192 && ip[1] == 168) return 1;
    return 0;
}

int cc_network_policy_address_allowed(
    const cc_network_policy_t *policy,
    const char *host,
    const char *numeric_address)
{
    (void)host;
    if (!policy || !numeric_address || !numeric_address[0]) return 0;
    if (policy->allow_private_networks) return 1;
    if (is_private_host(numeric_address)) return 0;
    if ((numeric_address[0] == 'f' || numeric_address[0] == 'F') &&
        (numeric_address[1] == 'c' || numeric_address[1] == 'C' ||
         numeric_address[1] == 'd' || numeric_address[1] == 'D')) return 0;
    if ((numeric_address[0] == 'f' || numeric_address[0] == 'F') &&
        (numeric_address[1] == 'e' || numeric_address[1] == 'E') &&
        numeric_address[2] == '8' && numeric_address[3] == '0') return 0;
    return 1;
}

/*
 * 判断 allowlist entry 是否匹配 URL。
 *
 * entry 指定 scheme/port 时必须完全匹配；未指定时只按 host 模式匹配。
 */
static int entry_matches(const url_parts_t *entry, const url_parts_t *url)
{
    if (entry->scheme[0] && !ascii_case_equal(entry->scheme, url->scheme)) return 0;
    if (entry->port >= 0 && entry->port != url->port) return 0;
    return host_matches_pattern(url->host, entry->host);
}

/*
 * 检查 allowlist 是否显式允许该 URL。
 *
 * 该函数同时用于普通 allow 和 private network 例外；只有匹配到有效条目才返回 1。
 */
static int explicit_private_allow(const cc_config_string_list_t *allowlist, const url_parts_t *url)
{
    if (!allowlist) return 0;
    for (size_t i = 0; i < allowlist->count; i++) {
        url_parts_t entry;
        if (!parse_allow_entry(allowlist->items[i], &entry)) continue;
        if (entry_matches(&entry, url)) return 1;
    }
    return 0;
}

/*
 * 判断 URL 是否满足网络策略。
 *
 * 默认拒绝无 policy、非法 URL、非 http/https、userinfo 和私有网络。即便 allow_private_networks
 * 为真，URL 仍需命中 allowlist；这样保持“未显式允许即拒绝”的 SDK 安全默认值。
 */
int cc_network_policy_url_allowed(
    const cc_network_policy_t *policy,
    const char *url
)
{
    if (!policy) return 0;
    url_parts_t parsed;
    if (!parse_url_parts(url, &parsed)) return 0;
    if (!ascii_case_equal(parsed.scheme, "http") &&
        !ascii_case_equal(parsed.scheme, "https")) return 0;
    if (parsed.has_userinfo) return 0;
    if (is_private_host(parsed.host) &&
        !policy->allow_private_networks &&
        !explicit_private_allow(&policy->allowlist, &parsed)) {
        return 0;
    }
    return explicit_private_allow(&policy->allowlist, &parsed);
}

/*
 * 基于简单 allowlist 的兼容入口。
 *
 * 构造默认 policy 后复用主校验逻辑，确保所有调用路径对 private network、scheme、userinfo
 * 的处理一致。
 */
int cc_network_allowlist_url_allowed(
    const cc_config_string_list_t *allowlist,
    const char *url
)
{
    cc_network_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    policy.size = sizeof(policy);
    if (allowlist) policy.allowlist = *allowlist;
    return cc_network_policy_url_allowed(&policy, url);
}
