#include "cc/util/cc_network_policy.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct url_parts {
    char scheme[16];
    char host[256];
    int port;
    int has_userinfo;
} url_parts_t;

/**
 * @brief 比较完整 ASCII scheme 或 host，忽略 ASCII 字母大小写。
 * @param[in] a 可选稳定 NUL 结尾文本。
 * @param[in] b 可选稳定 NUL 结尾文本。
 * @return 两个非 NULL 文本完整匹配为一，其他为零，不修改输入且不依赖 locale。
 */
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

/**
 * @brief 读取连续十进制端口并在越过 65535 时停止报错。
 * @param[in] text 可选稳定文本，首字节须为数字。
 * @param[out] end_out 可选借用游标槽，仅成功时指向首个非数字或 NUL。
 * @return 0..65535 为成功端口，空输入、非数字或越界为 -1。
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

/**
 * @brief 从 URL 拆出有界 scheme、host 和显式端口，拒绝 userinfo。
 * @param[in] url 可选稳定 NUL 结尾文本，必须含 scheme 和非空 authority。
 * @param[out] out 必填独占输出，先清零并把未显式指定端口置 -1，失败可能保留部分字段。
 * @return 结构及容量检查通过为一，其他为零，输出按值保存并剥离 IPv6 方括号。
 * @note path、query、fragment 只用于确定 authority 结尾，未补默认端口或执行 DNS。
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

/**
 * @brief 解析带可选 scheme 或单个端口分隔符的 allowlist 条目。
 * @param[in] entry 可选稳定文本，裸 IPv6 可作为 host，带 IPv6 端口使用 scheme://[host]:port。
 * @param[out] out 必填独占按值输出，非空有效输入才开始初始化，失败后不依赖其内容。
 * @return 可解析且容量足够为一，空、端口错误或过长为零，缺省 scheme/port 保留通配含义。
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

/**
 * @brief 对 host 执行完整 ASCII 匹配或星点域名后缀匹配。
 * @param[in] host 可选稳定主机名文本。
 * @param[in] pattern 可选稳定模式，*.domain 要求 host 在点号后缀前另有至少一个字节。
 * @return 匹配为一，NULL、空模式或不匹配为零，星点模式允许多级子域。
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

/**
 * @brief 解析四段完整十进制 IPv4，逐段检查 0..255。
 * @param[in] host 必填稳定 NUL 结尾文本，允许十进制前导零。
 * @param[out] out 至少四字节的独占输出，失败可能写入前几段。
 * @return 四段和全文均消费成功为一，其他为零。
 */
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

/**
 * @brief 将一个 ASCII 十六进制字符转换为数值。
 * @param[in] c 输入字节。
 * @return 合法数字为 0..15，其他为 -1。
 */
static int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/**
 * @brief 解析完整数值 IPv6，支持一次双冒号压缩及末尾 dotted IPv4。
 * @param[in] host 可选稳定文本，接收去方括号的地址，未处理 zone 标记。
 * @param[out] out 可选八个 16 位字的独占输出，有效参数先清零，失败可能保留部分字。
 * @return 合法完整地址为一，空参数、分组、压缩或尾部错误为零。
 */
static int parse_ipv6(const char *host, uint16_t out[8])
{
    if (!host || !out) return 0;
    size_t count = 0;
    size_t compressed_at = SIZE_MAX;
    const char *p = host;
    memset(out, 0, 8U * sizeof(*out));

    if (*p == ':') {
        if (p[1] != ':') return 0;
        compressed_at = 0;
        p += 2;
    }
    while (*p) {
        const char *dot = strchr(p, '.');
        const char *colon = strchr(p, ':');
        if (dot && (!colon || dot < colon)) {
            unsigned char ipv4[4];
            if (count > 6 || !parse_ipv4(p, ipv4)) return 0;
            out[count++] = (uint16_t)(((uint16_t)ipv4[0] << 8) | ipv4[1]);
            out[count++] = (uint16_t)(((uint16_t)ipv4[2] << 8) | ipv4[3]);
            p += strlen(p);
            break;
        }
        if (count >= 8) return 0;
        unsigned value = 0;
        unsigned digits = 0;
        while (*p) {
            int digit = hex_value((unsigned char)*p);
            if (digit < 0) break;
            if (++digits > 4) return 0;
            value = (value << 4) | (unsigned)digit;
            p++;
        }
        if (digits == 0) return 0;
        out[count++] = (uint16_t)value;
        if (*p == ':') {
            p++;
            if (*p == ':') {
                if (compressed_at != SIZE_MAX) return 0;
                compressed_at = count;
                p++;
                if (!*p) break;
            } else if (!*p) {
                return 0;
            }
        } else if (*p) {
            return 0;
        }
    }

    if (compressed_at != SIZE_MAX) {
        if (count >= 8) return 0;
        size_t tail_count = count - compressed_at;
        size_t tail_at = 8U - tail_count;
        memmove(out + tail_at, out + compressed_at, tail_count * sizeof(*out));
        for (size_t i = compressed_at; i < tail_at; i++) out[i] = 0;
        return 1;
    }
    return count == 8;
}

/**
 * @brief 按当前固定网段规则分类 IPv4 本地、私有、共享、测试和组播地址。
 * @param[in] ip 必填四字节稳定地址。
 * @return 命中拒绝网段为一，其他为零，具体范围以函数内字节比较为准。
 */
static int is_private_ipv4(const unsigned char ip[4])
{
    if (ip[0] == 0 || ip[0] == 10 || ip[0] == 127) return 1;
    if (ip[0] == 100 && ip[1] >= 64 && ip[1] <= 127) return 1;
    if (ip[0] == 169 && ip[1] == 254) return 1;
    if (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) return 1;
    if (ip[0] == 192 && ip[1] == 168) return 1;
    if (ip[0] == 198 && (ip[1] == 18 || ip[1] == 19)) return 1;
    if (ip[0] >= 224) return 1;
    return 0;
}

/**
 * @brief 对 localhost、数值 IPv4/IPv6 和 IPv4 嵌入地址应用本地范围分类。
 * @param[in] host 可选稳定去括号文本，NULL 保守返回一。
 * @return 本地、私有或固定拒绝范围为一，其他域名和无法解析的地址为零。
 * @note 此处不执行 DNS，IPv4 mapped 和旧式 compatible 地址按最后四字节分类。
 */
static int is_private_host(const char *host)
{
    if (!host) return 1;
    if (ascii_case_equal(host, "localhost") ||
        ascii_case_equal(host, "::1") ||
        ascii_case_equal(host, "0:0:0:0:0:0:0:1")) {
        return 1;
    }
    unsigned char ipv4[4];
    if (parse_ipv4(host, ipv4)) return is_private_ipv4(ipv4);

    uint16_t words[8];
    if (!parse_ipv6(host, words)) return 0;
    unsigned char ipv6[16];
    for (size_t i = 0; i < 8; i++) {
        ipv6[i * 2] = (unsigned char)(words[i] >> 8);
        ipv6[i * 2 + 1] = (unsigned char)words[i];
    }
    int all_zero = 1;
    for (size_t i = 0; i < sizeof(ipv6); i++) all_zero &= ipv6[i] == 0;
    if (all_zero) return 1; /* 未指定地址。 */
    if (ipv6[0] == 0xff) return 1; /* 组播地址。 */
    if ((ipv6[0] & 0xfeU) == 0xfcU) return 1; /* 唯一本地地址。 */
    if (ipv6[0] == 0xfeU && (ipv6[1] & 0xc0U) == 0x80U) return 1; /* 链路本地地址。 */
    if (ipv6[0] == 0xfeU && (ipv6[1] & 0xc0U) == 0xc0U) return 1; /* 已废弃的站点本地地址。 */

    int prefix_zero = 1;
    for (size_t i = 0; i < 10; i++) prefix_zero &= ipv6[i] == 0;
    if (prefix_zero && ipv6[10] == 0xffU && ipv6[11] == 0xffU)
        return is_private_ipv4(ipv6 + 12);
    if (prefix_zero && ipv6[10] == 0 && ipv6[11] == 0)
        return is_private_ipv4(ipv6 + 12); /* 旧式 IPv4 兼容地址沿尾部 IPv4 分类。 */
    return 0;
}

/**
 * @brief 在连接层对已取得的数值地址执行当前私网开关检查。
 * @param[in] policy 可选稳定策略，NULL 为拒绝，size 当前未用于版本分支。
 * @param[in] host 保留的主机身份借用，当前实现不读取该参数且不重复匹配 allowlist。
 * @param[in] numeric_address 可选稳定数值地址文本，空值为拒绝。
 * @return allow_private_networks 非零直接允许非空地址，否则拒绝已识别私网及固定 IPv6 前缀。
 * @note 调用者先完成 URL allowlist 校验并提供真实连接地址，此入口不执行 DNS 或完整地址有效性认证。
 */
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

/**
 * @brief 将已解析 allowlist 的可选 scheme/port 和 host 模式与 URL 比较。
 * @param[in] entry 必填稳定解析条目，空 scheme 和负端口为通配。
 * @param[in] url 必填稳定解析 URL，未显式指定的端口为 -1。
 * @return 全部已指定字段匹配为一，其他为零，未归一化默认端口。
 */
static int entry_matches(const url_parts_t *entry, const url_parts_t *url)
{
    if (entry->scheme[0] && !ascii_case_equal(entry->scheme, url->scheme)) return 0;
    if (entry->port >= 0 && entry->port != url->port) return 0;
    return host_matches_pattern(url->host, entry->host);
}

/**
 * @brief 顺序扫描 allowlist 并接受首个可解析且匹配的条目。
 * @param[in] allowlist 可选稳定列表，非零 count 必须有覆盖 count 项的数组。
 * @param[in] url 必填稳定解析 URL。
 * @return 命中为一，空列表、缺数组或全部无效/不匹配为零，NULL 项按无效条目跳过。
 */
static int explicit_private_allow(const cc_config_string_list_t *allowlist, const url_parts_t *url)
{
    if (!allowlist || (allowlist->count && !allowlist->items)) return 0;
    for (size_t i = 0; i < allowlist->count; i++) {
        url_parts_t entry;
        if (!parse_allow_entry(allowlist->items[i], &entry)) continue;
        if (entry_matches(&entry, url)) return 1;
    }
    return 0;
}

/**
 * @brief 检查 HTTP(S) URL 的端点结构和显式 allowlist 匹配。
 * @param[in] policy 可选稳定策略及列表借用，NULL 为拒绝，读取期间 owner 排除配置修改。
 * @param[in] url 可选稳定 URL 文本，userinfo、非 HTTP(S)、空 authority 或容量超限为拒绝。
 * @return 命中有效 allowlist 为一，其他为零，当前显式 allowlist 可提供私网例外。
 * @note allow_private_networks 不跳过 allowlist，域名 DNS 解析和重定向后校验由传输调用者负责。
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

/**
 * @brief 构造禁止默认私网的临时策略并复用 URL allowlist 判断。
 * @param[in] allowlist 可选稳定列表，结构按值复制而数组与字符串仅在调用期间借用。
 * @param[in] url 可选稳定 URL 文本。
 * @return cc_network_policy_url_allowed 的布尔结果，空列表不会命中。
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
