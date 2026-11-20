



#include "cc/ports/cc_http_client.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/ports/cc_platform.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_ciphersuites.h"
#include "mbedtls/version.h"
#endif

/** @brief 请求内按值 URL 投影，主机 128 B、路径 256 B，均含 NUL，仅支持 IPv4 主机与域名。 */
typedef struct parsed_url {
    char scheme[6];
    char host[128];
    char path[256];
    uint16_t port;
    int use_tls;
} parsed_url_t;

/** @brief 借用原始头内去除多余前导零的长度数字，避免元数据长度的整数转换上限。 */
typedef struct response_length {
    const char *digits;
    size_t digit_count;
} response_length_t;

/** @brief 单请求独占传输，借用请求与 token，拥有 socket 和已初始化的 TLS 对象。 */
typedef struct transport {
    int fd;
    const cc_http_request_t *request;
    uint64_t deadline_ms;
    uint64_t phase_deadline_ms;
    uint64_t phase_timeout_ms;
    cc_error_code_t io_error;
#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
#if MBEDTLS_VERSION_MAJOR < 4
    uint32_t rng_state;
#endif
    int tls_active;
    int tls_initialized;
#endif
} transport_t;

/** @brief 创建后固定的实例选项，调用互不共享传输状态，owner 排空后释放实例。 */
typedef struct cc_freertos_http_client {
    cc_http_client_options_t options;
} cc_freertos_http_client_t;

/**
 * @brief 查找首个大小写不敏感匹配且字段非空指针的请求头。
 * @param[in] request 非 NULL 稳定请求，headers 与 header_count 匹配。
 * @param[in] name 非 NULL 借用头名。
 * @return 请求内借用值，空字符串也可返回，未找到为 NULL。
 */
static const char *request_header_value(const cc_http_request_t *request, const char *name)
{
    for (size_t i = 0; i < request->header_count; i++) {
        if (request->headers[i].name && request->headers[i].value &&
            strcasecmp(request->headers[i].name, name) == 0) {
            return request->headers[i].value;
        }
    }
    return NULL;
}

/**
 * @brief 深拷贝指定字节并补 NUL，保留正文中的二进制 NUL。
 * @param[in] data 调用期间稳定输入，len 为零时可为 NULL。
 * @param[in] len 要复制的字节数。
 * @return free 可释放的拥有缓冲，参数不合法、加一溢出或分配失败为 NULL。
 */
static char *copy_string_len(const char *data, size_t len)
{
    if (len == SIZE_MAX || (!data && len > 0)) return NULL;
    char *copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    if (len > 0) memcpy(copy, data, len);
    copy[len] = '\0';
    return copy;
}

/**
 * @brief 对容量和预算检查后追加完整原始响应字节。
 * @param[in,out] buf 非 NULL 独占缓冲槽，初始为 NULL，成功可能搬迁。
 * @param[in,out] len 非 NULL 实际字节计数，须与缓冲内容匹配。
 * @param[in] max_len 原始头加正文的字节预算，零表示不设业务上限。
 * @param[in] data 独立稳定片段，不得借自可能被 realloc 搬迁的缓冲。
 * @param[in] data_len 输入字节数。
 * @return 成功为 CC_OK，算术或预算超限为 LIMIT_EXCEEDED，分配失败为 OUT_OF_MEMORY，失败保留旧缓冲和计数。
 */
static cc_result_t append_bytes(char **buf, size_t *len, size_t max_len, const char *data, size_t data_len)
{
    if (*len == SIZE_MAX || data_len > SIZE_MAX - *len - 1 ||
        (max_len > 0 && (*len > max_len || data_len > max_len - *len))) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "HTTP raw response exceeded limit");
    }
    char *next = (char *)realloc(*buf, *len + data_len + 1);
    if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow HTTP raw response");
    memcpy(next + *len, data, data_len);
    *len += data_len;
    next[*len] = '\0';
    *buf = next;
    return cc_result_ok();
}

/**
 * @brief 解析有界 http/https URL，分别保留主机、端口及不含 fragment 的请求路径。
 * @param[in] url 借用完整 NUL 文本，可为 NULL，scheme 忽略大小写。
 * @param[out] out 非 NULL 按值输出，先清零，错误时可能已有部分字段。
 * @return 成功为 CC_OK，坏格式、userinfo、IPv6、超长字段或未编译的 HTTPS 为 INVALID_ARGUMENT。
 * @note 端口只接纳 1..65535 的完整十进制串，无斜杠查询生成 /?query，fragment 不发送。
 */
static cc_result_t parse_url(const char *url, parsed_url_t *out)
{
    if (!out) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL output is missing");

    memset(out, 0, sizeof(*out));
    if (!url) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL is missing");
    const char *prefix_end = strstr(url, "://");
    if (!prefix_end) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL scheme is missing");
    }

    size_t scheme_len = (size_t)(prefix_end - url);
    if (scheme_len >= sizeof(out->scheme)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL scheme is too long");
    }
    for (size_t i = 0; i < scheme_len; i++) {
        out->scheme[i] = (char)tolower((unsigned char)url[i]);
    }
    out->scheme[scheme_len] = '\0';

    if (strcmp(out->scheme, "http") == 0) {
        out->port = 80;
        out->use_tls = 0;
    } else if (strcmp(out->scheme, "https") == 0) {
#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
        out->port = 443;
        out->use_tls = 1;
#else
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "FreeRTOS lwIP HTTP client was built without HTTPS support");
#endif
    } else {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "FreeRTOS lwIP HTTP client only supports http:// and https:// URLs");
    }

    const char *cursor = prefix_end + 3;
    const char *host_end = cursor + strcspn(cursor, "/?#");
    if (memchr(cursor, '@', (size_t)(host_end - cursor)) || *cursor == '[') {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL user-info and IPv6 are unsupported");
    }
    const char *colon = memchr(cursor, ':', (size_t)(host_end - cursor));

    size_t host_len = (size_t)((colon ? colon : host_end) - cursor);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL host is missing or too long");
    }
    memcpy(out->host, cursor, host_len);
    out->host[host_len] = '\0';

    if (colon) {
        uint32_t port = 0;
        const char *digit = colon + 1;
        if (digit == host_end) {
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL port is invalid");
        }
        for (; digit < host_end; digit++) {
            if (*digit < '0' || *digit > '9' || port > (65535U - (unsigned)(*digit - '0')) / 10U) {
                return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL port is invalid");
            }
            port = port * 10U + (unsigned)(*digit - '0');
        }
        if (port == 0) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL port is invalid");
        out->port = (uint16_t)port;
    }

    const char *path = host_end;
    size_t path_length = strcspn(path, "#");
    size_t prefix_length = *path == '/' ? 0U : 1U;
    if (path_length + prefix_length >= sizeof(out->path)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL path is too long");
    }
    if (prefix_length) out->path[0] = '/';
    memcpy(out->path + prefix_length, path, path_length);
    out->path[path_length + prefix_length] = '\0';
    return cc_result_ok();
}

/**
 * @brief 解析 IPv4 并核对全部 IPv4 候选，选首个可用地址。
 * @param[in] request 非 NULL 请求，地址策略与 user_data 在同步回调期间稳定。
 * @param[in] host 非 NULL 域名或数字 IPv4，借用覆盖 DNS 调用。
 * @param[in] port 目标端口，按网络字节序写入输出。
 * @param[out] out 非 NULL sockaddr，先清零并填地址族/端口，失败可能保留部分字段。
 * @return 成功为 CC_OK，策略拒绝为 PERMISSION_DENIED，DNS 或无可用 IPv4 为 NETWORK。
 * @note 本函数释放所有 DNS 列表，原生 DNS 可阻塞，外层在返回后重查取消与期限。
 */
static cc_result_t resolve_ipv4(
    const cc_http_request_t *request,
    const char *host,
    uint16_t port,
    struct sockaddr_in *out)
{
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = PP_HTONS(port);

    out->sin_addr.s_addr = inet_addr(host);
    if (out->sin_addr.s_addr != IPADDR_NONE) {
        char numeric[INET_ADDRSTRLEN];
        const char *rendered = inet_ntop(AF_INET, &out->sin_addr, numeric, sizeof(numeric));
        if (request->validate_address &&
            (!rendered || !request->validate_address(host, numeric, request->validate_address_user_data))) {
            return cc_result_error(CC_ERR_PERMISSION_DENIED, "HTTP address rejected by network policy");
        }
        return cc_result_ok();
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *results = NULL;
    int rc = lwip_getaddrinfo(host, NULL, &hints, &results);
    if (rc != 0 || !results) {
        if (results) lwip_freeaddrinfo(results);
        return cc_result_error(CC_ERR_NETWORK, "lwIP DNS resolution failed");
    }

    struct sockaddr_in *selected = NULL;
    for (struct addrinfo *it = results; it; it = it->ai_next) {
        if (it->ai_family != AF_INET) continue;
        if (!it->ai_addr || it->ai_addrlen < (socklen_t)sizeof(struct sockaddr_in) ||
            it->ai_addr->sa_family != AF_INET) {
            lwip_freeaddrinfo(results);
            return cc_result_error(CC_ERR_NETWORK, "lwIP DNS returned an invalid IPv4 address");
        }
        struct sockaddr_in *candidate = (struct sockaddr_in *)it->ai_addr;
        char numeric[INET_ADDRSTRLEN] = {0};
        const char *rendered = inet_ntop(AF_INET, &candidate->sin_addr, numeric, sizeof(numeric));
        if (request->validate_address &&
            (!rendered || !request->validate_address(
                host, numeric, request->validate_address_user_data))) {
            lwip_freeaddrinfo(results);
            return cc_result_error(CC_ERR_PERMISSION_DENIED,
                "HTTP resolved address rejected by network policy");
        }
        if (!selected) selected = candidate;
    }
    if (!selected) {
        lwip_freeaddrinfo(results);
        return cc_result_error(CC_ERR_NETWORK, "lwIP DNS returned no IPv4 address");
    }
    out->sin_addr = selected->sin_addr;
    lwip_freeaddrinfo(results);
    return cc_result_ok();
}

/**
 * @brief 在原生阻塞前后核对取消和完整请求期限，锁存首次失败分类。
 * @param[in,out] transport 当前请求独占传输，request 和 deadline 已初始化。
 * @return 可以继续时返回 1，取消或期限耗尽时返回 0 并设置 io_error。
 */
static int transport_check(transport_t *transport)
{
    if (transport->io_error != CC_OK) return 0;
    if (cc_cancel_token_is_cancelled(transport->request->cancel_token)) {
        transport->io_error = CC_ERR_CANCELLED;
        return 0;
    }
    uint64_t now = cc_platform_monotonic_ms();
    if (now >= transport->deadline_ms ||
        (transport->phase_deadline_ms > 0 && now >= transport->phase_deadline_ms)) {
        transport->io_error = CC_ERR_TIMEOUT;
        return 0;
    }
    return 1;
}

/**
 * @brief 将下一次 socket 收发等待限制到阶段预算和同一总期限的剩余量。
 * @param[in,out] transport 当前独占传输，fd 有效，request 借用覆盖调用。
 * @return 两个原生 timeout 设置均成功返回 1，取消/超时/设置失败返回 0 并锁存分类。
 * @note setsockopt 和之后的原生调用仍可能有平台调度延迟，返回后继续核对期限。
 */
static int transport_prepare_io(transport_t *transport)
{
    if (!transport_check(transport)) return 0;
    uint64_t now = cc_platform_monotonic_ms();
    if (now >= transport->deadline_ms) {
        transport->io_error = CC_ERR_TIMEOUT;
        return 0;
    }
    uint64_t remaining = transport->deadline_ms - now;
    if (transport->phase_deadline_ms > 0) {
        if (now >= transport->phase_deadline_ms) {
            transport->io_error = CC_ERR_TIMEOUT;
            return 0;
        }
        uint64_t phase_remaining = transport->phase_deadline_ms - now;
        if (phase_remaining < remaining) remaining = phase_remaining;
    }
    if (transport->phase_timeout_ms > 0 && transport->phase_timeout_ms < remaining)
        remaining = transport->phase_timeout_ms;
    if (remaining > INT_MAX) remaining = INT_MAX;
#if defined(LWIP_SO_SNDRCVTIMEO_NONSTANDARD) && LWIP_SO_SNDRCVTIMEO_NONSTANDARD
    int timeout = (int)remaining;
#else
    struct timeval timeout;
    timeout.tv_sec = (long)(remaining / 1000U);
    timeout.tv_usec = (long)((remaining % 1000U) * 1000U);
#endif
    if (lwip_setsockopt(transport->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        lwip_setsockopt(transport->fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        transport->io_error = CC_ERR_NETWORK;
        return 0;
    }
    return 1;
}

/**
 * @brief 根据刚返回的 socket 错误登记超时或网络失败。
 * @param[in,out] transport 当前独占传输，已有取消/期限分类保留。
 * @post 首个错误保留，原生 EAGAIN/EWOULDBLOCK/ETIMEDOUT 归为 TIMEOUT。
 */
static void transport_note_io_error(transport_t *transport)
{
    if (transport->io_error != CC_OK) return;
    transport->io_error = errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT
        ? CC_ERR_TIMEOUT : CC_ERR_NETWORK;
}

#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
#if MBEDTLS_VERSION_MAJOR < 4
/**
 * @brief 为当前示例 TLS 分支产生确定性的 xorshift 字节。
 * @param[in,out] ctx 非 NULL 的当前传输 uint32 状态，每请求独占。
 * @param[out] out 至少 len 字节的输出缓冲。
 * @param[in] len 输出字节数。
 * @return 始终返回零。
 * @note 当前保留示例 RNG，真实产品的熵源接线仍由该平台移植负责。
 */
static int test_rng(void *ctx, unsigned char *out, size_t len)
{
    uint32_t *state = (uint32_t *)ctx;
    for (size_t i = 0; i < len; i++) {
        uint32_t x = *state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        *state = x;
        out[i] = (unsigned char)(x & 0xffu);
    }
    return 0;
}
#endif

/**
 * @brief 在期限/取消保护下向当前 socket 写一个有界片段。
 * @param[in,out] ctx 非 NULL 当前 transport，mbedTLS 同步借用。
 * @param[in] buf 至少 len 字节的稳定输入。
 * @param[in] len 可写字节数，本次最多 INT_MAX。
 * @return 原生写入计数，失败或阻塞返回后已取消/超时为 -1，详细分类保留在 io_error。
 */
static int tls_send(void *ctx, const unsigned char *buf, size_t len)
{
    transport_t *transport = ctx;
    if (!transport_prepare_io(transport)) return -1;
    size_t count = len > INT_MAX ? INT_MAX : len;
    int sent = lwip_send(transport->fd, buf, count, 0);
    if (!transport_check(transport)) return -1;
    if (sent < 0) transport_note_io_error(transport);
    return sent < 0 ? -1 : sent;
}

/**
 * @brief 在期限/取消保护下从当前 socket 读取一个有界片段。
 * @param[in,out] ctx 非 NULL 当前 transport。
 * @param[out] buf 至少 len 字节的接收缓冲。
 * @param[in] len 可读容量，本次最多 INT_MAX。
 * @return 正值为字节数，零为 EOF，失败或检查中止为 -1，分类保存在 io_error。
 */
static int tls_recv(void *ctx, unsigned char *buf, size_t len)
{
    transport_t *transport = ctx;
    if (!transport_prepare_io(transport)) return -1;
    size_t count = len > INT_MAX ? INT_MAX : len;
    int got = lwip_recv(transport->fd, buf, count, 0);
    if (!transport_check(transport)) return -1;
    if (got < 0) transport_note_io_error(transport);
    return got < 0 ? -1 : got;
}

/**
 * @brief 初始化当前 TLS 对象并在同一请求期限内完成握手。
 * @param[in,out] transport 非 NULL 已连接传输，request/期限已设置，调用独占。
 * @param[in] tls_host URL 提取的纯主机，借用覆盖配置调用，与 HTTP Host 覆盖字段独立。
 * @return 成功为 CC_OK 并设置 tls_active，取消/期限返回相应错误，其他 TLS 失败为 NETWORK。
 * @note 初始化标志保证半失败对象由外层 close 释放。WANT 循环逐次检查取消和期限。
 * @note 保留 VERIFY_NONE，旧版使用示例 RNG，4.x 使用库级 PSA RNG；要求校验证书时外层返回 UNSUPPORTED。
 * @pre Mbed TLS 4.x 的 BSP/应用须在开放 HTTP 调用前完成 PSA 初始化，并保持其寿命覆盖所有调用。
 */
static cc_result_t start_tls(transport_t *transport, const char *tls_host)
{
    static const int suites[] = {
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        0
    };

    mbedtls_ssl_init(&transport->ssl);
    mbedtls_ssl_config_init(&transport->conf);
    transport->tls_initialized = 1;
#if MBEDTLS_VERSION_MAJOR < 4
    transport->rng_state = 0xc0ffeeu;
#endif

    int rc = mbedtls_ssl_config_defaults(&transport->conf,
        MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) return cc_result_error(CC_ERR_NETWORK, "mbedTLS config defaults failed");

    mbedtls_ssl_conf_authmode(&transport->conf, MBEDTLS_SSL_VERIFY_NONE);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ssl_conf_rng(&transport->conf, test_rng, &transport->rng_state);
#endif
    mbedtls_ssl_conf_ciphersuites(&transport->conf, suites);

    rc = mbedtls_ssl_setup(&transport->ssl, &transport->conf);
    if (rc != 0) return cc_result_error(CC_ERR_NETWORK, "mbedTLS ssl setup failed");

    rc = mbedtls_ssl_set_hostname(&transport->ssl, tls_host);
    if (rc != 0) return cc_result_error(CC_ERR_NETWORK, "mbedTLS SNI setup failed");

    mbedtls_ssl_set_bio(&transport->ssl, transport, tls_send, tls_recv, NULL);
    do {
        if (!transport_prepare_io(transport)) {
            return cc_result_error(transport->io_error, "HTTP TLS handshake interrupted");
        }
        rc = mbedtls_ssl_handshake(&transport->ssl);
        if (!transport_check(transport)) {
            return cc_result_error(transport->io_error, "HTTP TLS handshake interrupted");
        }
    } while (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);

    if (rc != 0) {
        char msg[96];
        char err[64];
        mbedtls_strerror(rc, err, sizeof(err));
        snprintf(msg, sizeof(msg), "mbedTLS handshake failed: %s", err);
        return cc_result_error(CC_ERR_NETWORK, msg);
    }

    transport->tls_active = 1;
    return cc_result_ok();
}
#endif

/**
 * @brief 幂等归还本次已初始化 TLS 对象与 socket。
 * @param[in,out] transport 非 NULL 当前传输，fd 初始须为 -1，TLS 标志须反映实际初始化。
 * @post TLS 标志清零、fd 置 -1；close_notify 尽力发送，其错误不覆盖既有传输分类。
 * @pre 当前传输全部同步读写和回调已返回，实例无共享 socket。
 */
static void transport_close(transport_t *transport)
{
#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
    if (transport->tls_active) {
        cc_error_code_t saved_error = transport->io_error;
        (void)mbedtls_ssl_close_notify(&transport->ssl);
        transport->io_error = saved_error;
    }
    if (transport->tls_initialized) {
        mbedtls_ssl_free(&transport->ssl);
        mbedtls_ssl_config_free(&transport->conf);
        transport->tls_initialized = 0;
        transport->tls_active = 0;
    }
#endif
    if (transport->fd >= 0) lwip_close(transport->fd);
    transport->fd = -1;
}

/**
 * @brief 分段写完借用正文，处理短写并约束 TLS WANT 重试。
 * @param[in,out] transport 非 NULL 独占传输，fd 有效，request/期限已设置。
 * @param[in] data 至少 len 字节的稳定文本或字节输入。
 * @param[in] len 字节数，每次原生写最多 INT_MAX。
 * @return 全部写出为 CC_OK，取消/超时/原生失败返回对应拥有结果，失败可能已发送前缀。
 */
static cc_result_t transport_write_all(transport_t *transport, const char *data, size_t len)
{
    while (len > 0) {
        if (!transport_prepare_io(transport))
            return cc_result_error(transport->io_error, "HTTP transport write interrupted");
        size_t count = len > INT_MAX ? INT_MAX : len;
        int sent;
#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
        if (transport->tls_active) {
            do {
                sent = mbedtls_ssl_write(&transport->ssl, (const unsigned char *)data, count);
                if (!transport_check(transport))
                    return cc_result_error(transport->io_error, "HTTP TLS write interrupted");
            } while (sent == MBEDTLS_ERR_SSL_WANT_READ || sent == MBEDTLS_ERR_SSL_WANT_WRITE);
            if (sent < 0 && transport->io_error == CC_OK) transport->io_error = CC_ERR_NETWORK;
        } else
#endif
        {
            sent = lwip_send(transport->fd, data, count, 0);
        }
        if (!transport_check(transport))
            return cc_result_error(transport->io_error, "HTTP transport write interrupted");
        if (sent <= 0) {
            if (sent == 0) transport->io_error = CC_ERR_NETWORK;
            else transport_note_io_error(transport);
            return cc_result_error(transport->io_error, "lwIP transport send failed");
        }
        data += sent;
        len -= (size_t)sent;
    }
    return cc_result_ok();
}

/**
 * @brief 读取一个受预算保护的片段，并在 TLS WANT 期间持续检查停止条件。
 * @param[in,out] transport 非 NULL 当前传输，独占 socket 与 TLS 对象。
 * @param[out] buf 至少 len 字节的接收缓冲。
 * @param[in] len 容量，本次最多 INT_MAX。
 * @return 正数为收到字节，零为干净 EOF，负数为失败，具体分类保存在 io_error。
 */
static int transport_read(transport_t *transport, char *buf, size_t len)
{
    if (!transport_prepare_io(transport)) return -1;
    size_t count = len > INT_MAX ? INT_MAX : len;
#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
    if (transport->tls_active) {
        int got;
        do {
            got = mbedtls_ssl_read(&transport->ssl, (unsigned char *)buf, count);
            if (!transport_check(transport)) return -1;
        } while (got == MBEDTLS_ERR_SSL_WANT_READ || got == MBEDTLS_ERR_SSL_WANT_WRITE);
        if (got == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        if (got < 0 && transport->io_error == CC_OK) transport->io_error = CC_ERR_NETWORK;
        return got;
    }
#endif
    int got = lwip_recv(transport->fd, buf, count, 0);
    if (!transport_check(transport)) return -1;
    if (got < 0) transport_note_io_error(transport);
    return got;
}

/**
 * @brief 合并一行 Content-Length 的十进制值，接受全部相同的重复值。
 * @param[in] begin 借用字段值起点。
 * @param[in] end 同一缓冲的排他末尾，须不早于 begin。
 * @param[in,out] length 非 NULL 数字视图，初始清零，首值成功后借用原始头的规范化数字片段。
 * @return 完整语法且所有值相同为 CC_OK，空值、尾垃圾或冲突为 NETWORK。
 * @note 不转换元数据为整数，HEAD/304 可以描述超出本机 size_t 的实体长度。
 */
static cc_result_t response_content_length(
    const char *begin, const char *end, response_length_t *length)
{
    const char *cursor = begin;
    for (;;) {
        while (cursor < end && (*cursor == ' ' || *cursor == '\t')) cursor++;
        if (cursor == end || *cursor < '0' || *cursor > '9')
            return cc_result_error(CC_ERR_NETWORK, "Invalid HTTP Content-Length");
        const char *digits = cursor;
        while (cursor < end && *cursor >= '0' && *cursor <= '9') cursor++;
        const char *digits_end = cursor;
        while (digits + 1 < digits_end && *digits == '0') digits++;
        size_t digit_count = (size_t)(digits_end - digits);
        while (cursor < end && (*cursor == ' ' || *cursor == '\t')) cursor++;
        if ((length->digits && (digit_count != length->digit_count ||
             memcmp(digits, length->digits, digit_count) != 0)) ||
            (cursor < end && *cursor != ','))
            return cc_result_error(CC_ERR_NETWORK, "Conflicting or invalid HTTP Content-Length");
        length->digits = digits;
        length->digit_count = digit_count;
        if (cursor == end) return cc_result_ok();
        cursor++;
    }
}

/**
 * @brief 核对完整状态行、字段与正文长度，跳过前置 informational 响应。
 * @param[in] request 稳定请求，HEAD 的长度字段只描述对应实体。
 * @param[in] raw NUL 结尾原始响应，拥有缓冲由外层保管，头部不得含 NUL。
 * @param[in] raw_len 实际字节数，正文可含 NUL。
 * @param[out] out_status 非 NULL，成功时填写最终状态，失败保留原槽。
 * @param[out] out_offset 非 NULL，成功时填写最终正文在 raw 中的字节偏移。
 * @return 完整响应为 CC_OK，状态/头/长度错误为 NETWORK，需解码的传输编码为 UNSUPPORTED。
 * @note HEAD、1xx、204、304 要求实际无正文，允许 HEAD/304 携带描述实体的长度或传输编码。
 */
static cc_result_t response_validate(
    const cc_http_request_t *request, const char *raw, size_t raw_len,
    long *out_status, size_t *out_offset)
{
    size_t offset = 0;
    for (;;) {
        const char *start = raw ? raw + offset : NULL;
        const char *header_end = start ? strstr(start, "\r\n\r\n") : NULL;
        const char *status_end = start ? strstr(start, "\r\n") : NULL;
        if (!header_end || !status_end || raw_len - offset < 12 ||
            memcmp(start, "HTTP/", 5) != 0 || !isdigit((unsigned char)start[5]) ||
            start[6] != '.' || !isdigit((unsigned char)start[7]) || start[8] != ' ' ||
            start[9] < '1' || start[9] > '5' || !isdigit((unsigned char)start[10]) ||
            !isdigit((unsigned char)start[11]) ||
            (start[12] != ' ' && start[12] != '\r') || status_end > header_end) {
            return cc_result_error(CC_ERR_NETWORK, "HTTP response header is incomplete or invalid");
        }
        long status = (start[9] - '0') * 100 + (start[10] - '0') * 10 + start[11] - '0';
        size_t body_offset = (size_t)(header_end + 4 - raw);
        int has_encoding = 0;
        response_length_t length = {0};
        const char *line = status_end + 2;
        while (line < header_end) {
            const char *line_end = strstr(line, "\r\n");
            const char *colon = line_end ? memchr(line, ':', (size_t)(line_end - line)) : NULL;
            if (!line_end || line_end > header_end || !colon || colon == line)
                return cc_result_error(CC_ERR_NETWORK, "Invalid HTTP response field");
            for (const char *name = line; name < colon; name++) {
                if ((unsigned char)*name <= ' ' || (unsigned char)*name >= 127)
                    return cc_result_error(CC_ERR_NETWORK, "Invalid HTTP response field name");
            }
            size_t name_length = (size_t)(colon - line);
            if (name_length == 14 && strncasecmp(line, "Content-Length", name_length) == 0) {
                cc_result_t rc = response_content_length(colon + 1, line_end, &length);
                if (rc.code != CC_OK) return rc;
                cc_result_free(&rc);
            } else if (name_length == 17 && strncasecmp(line, "Transfer-Encoding", name_length) == 0) {
                const char *value = colon + 1;
                while (value < line_end && (*value == ' ' || *value == '\t')) value++;
                if (value == line_end)
                    return cc_result_error(CC_ERR_NETWORK, "Empty HTTP Transfer-Encoding");
                has_encoding = 1;
            }
            line = line_end + 2;
        }
        if (status >= 100 && status < 200 && status != 101 && body_offset < raw_len &&
            raw_len - body_offset >= 5 && memcmp(raw + body_offset, "HTTP/", 5) == 0) {
            offset = body_offset;
            continue;
        }
        int head = request->method && strcasecmp(request->method, "HEAD") == 0;
        int no_body = head || (status >= 100 && status < 200) || status == 204 || status == 304;
        size_t body_length = raw_len - body_offset;
        if (no_body && body_length != 0)
            return cc_result_error(CC_ERR_NETWORK, "HTTP response body length mismatch");
        if (has_encoding && !head && status != 304)
            return cc_result_error(CC_ERR_UNSUPPORTED, "FreeRTOS HTTP response transfer encoding is unsupported");
        if (!no_body && length.digits) {
            char actual_length[3 * sizeof(size_t) + 1];
            int count = snprintf(actual_length, sizeof(actual_length), "%zu", body_length);
            if (count < 0 || (size_t)count != length.digit_count ||
                memcmp(actual_length, length.digits, length.digit_count) != 0)
                return cc_result_error(CC_ERR_NETWORK, "HTTP response body length mismatch");
        }
        *out_status = status;
        *out_offset = body_offset;
        return cc_result_ok();
    }
}

/**
 * @brief 在独立 socket 上发送 HTTP/1.0 长度正文并有界缓存完整响应后交付。
 * @param[in] self 非 NULL 存活实例，选项创建后固定，owner 保持至请求和回调退出。
 * @param[in] request 非 NULL 稳定请求，URL 必填，数组/count 匹配，正文为 NUL 文本。
 * @param[out] out_response 非 NULL 新/已清理响应，有效参数后先清零，返回后由调用者清理拥有字段。
 * @return 成功为 CC_OK；参数/策略、取消、总期限、原生 I/O、预算或分配失败返回对应拥有结果。
 * @note 每调用独占传输，可并行使用固定实例选项。DNS/connect/原生等待返回后重查同一绝对期限。
 * @note max_response_bytes 计原始头加正文，on_body 仅完整缓存后调用一次，末次回调结果可继续交付 perform 接收者。
 * @note 无内部重试和重定向，响应头数组保持空；请求非空 Transfer-Encoding 为当前长度编码器的 UNSUPPORTED 边界。
 */
static cc_result_t freertos_http_perform(
    void *self,
    const cc_http_request_t *request,
    cc_http_response_t *out_response)
{
    cc_freertos_http_client_t *client = (cc_freertos_http_client_t *)self;
    if (!client || !request || !request->url || !out_response ||
        (request->header_count > 0 && !request->headers)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid HTTP request");
    }
    memset(out_response, 0, sizeof(*out_response));
    if (request->validate_url &&
        !request->validate_url(request->url, request->validate_url_user_data)) {
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "HTTP URL rejected by network policy");
    }
    if (cc_cancel_token_is_cancelled(request->cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled before start");
    }
    parsed_url_t url;
    cc_result_t rc = parse_url(request->url, &url);
    if (rc.code != CC_OK) return rc;
    for (size_t i = 0; i < request->header_count; i++) {
        const cc_http_header_t *field = &request->headers[i];
        if (field->name && field->value && *field->value &&
            strcasecmp(field->name, "Transfer-Encoding") == 0) {
            return cc_result_error(CC_ERR_UNSUPPORTED, "FreeRTOS HTTP request encoding requires a length body");
        }
    }
    char default_host[sizeof(url.host) + 8];
    const char *host_header = request_header_value(request, "Host");
    if (!host_header) {
        int default_port = url.use_tls ? 443 : 80;
        if (url.port == default_port) {
            host_header = url.host;
        } else {
            snprintf(default_host, sizeof(default_host), "%s:%u", url.host, (unsigned)url.port);
            host_header = default_host;
        }
    }

    transport_t transport;
    memset(&transport, 0, sizeof(transport));
    transport.fd = -1;
    transport.request = request;
    uint64_t started_ms = cc_platform_monotonic_ms();
    uint64_t budget_ms = request->timeout_ms > 0 ? (uint64_t)request->timeout_ms : 120000U;
    transport.deadline_ms = budget_ms > UINT64_MAX - started_ms ? UINT64_MAX : started_ms + budget_ms;
    if (request->deadline_ms > 0 && request->deadline_ms < transport.deadline_ms)
        transport.deadline_ms = request->deadline_ms;
    transport.phase_timeout_ms = request->connect_timeout_ms > 0 ? (uint64_t)request->connect_timeout_ms :
        client->options.connect_timeout_ms > 0 ? (uint64_t)client->options.connect_timeout_ms : 15000U;
    if (!transport_check(&transport))
        return cc_result_error(transport.io_error, "HTTP request interrupted before DNS");
    transport.phase_deadline_ms = transport.phase_timeout_ms < transport.deadline_ms - started_ms
        ? started_ms + transport.phase_timeout_ms : transport.deadline_ms;

    struct sockaddr_in addr;
    cc_result_free(&rc);
    rc = resolve_ipv4(request, url.host, url.port, &addr);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    if (!transport_check(&transport))
        return cc_result_error(transport.io_error, "HTTP request interrupted after DNS");
    transport.fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (transport.fd < 0) return cc_result_error(CC_ERR_NETWORK, "lwIP socket creation failed");
    char *raw = NULL;
    size_t raw_len = 0;
    if (!transport_prepare_io(&transport)) {
        rc = cc_result_error(transport.io_error, "Failed to prepare HTTP connect");
        goto cleanup;
    }

    int connected = lwip_connect(transport.fd, (struct sockaddr *)&addr, sizeof(addr));
    if (!transport_check(&transport)) {
        rc = cc_result_error(transport.io_error, "HTTP request interrupted after connect");
        goto cleanup;
    }
    if (connected != 0) {
        transport_note_io_error(&transport);
        rc = cc_result_error(transport.io_error, "lwIP connect failed");
        goto cleanup;
    }

#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
    if (url.use_tls) {
        if (client->options.tls_verify) {
            rc = cc_result_error(CC_ERR_UNSUPPORTED,
                "FreeRTOS HTTPS requires a configured CA verification backend");
            goto cleanup;
        }
        rc = start_tls(&transport, url.host);
        if (rc.code != CC_OK) goto cleanup;
        cc_result_free(&rc);
    }
#endif

    transport.phase_deadline_ms = 0;
    const char *method = request->method ? request->method : "GET";
    char header[768];
    int n = snprintf(header, sizeof(header),
        "%s %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n",
        method, url.path, host_header);
    if (n < 0 || (size_t)n >= sizeof(header)) {
        rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP request line too long");
        goto cleanup;
    }
    rc = transport_write_all(&transport, header, (size_t)n);
    if (rc.code != CC_OK) goto cleanup;
    cc_result_free(&rc);

    for (size_t i = 0; i < request->header_count; i++) {
        if (!request->headers[i].name || !request->headers[i].value) continue;
        if (strcasecmp(request->headers[i].name, "Host") == 0 ||
            strcasecmp(request->headers[i].name, "Content-Length") == 0 ||
            strcasecmp(request->headers[i].name, "Transfer-Encoding") == 0) continue;
        n = snprintf(header, sizeof(header), "%s: %s\r\n",
            request->headers[i].name, request->headers[i].value);
        if (n < 0 || (size_t)n >= sizeof(header)) {
            rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP header too long");
            goto cleanup;
        }
        rc = transport_write_all(&transport, header, (size_t)n);
        if (rc.code != CC_OK) goto cleanup;
        cc_result_free(&rc);
    }

    size_t body_len = request->body ? strlen(request->body) : 0;
    n = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", body_len);
    if (n < 0 || (size_t)n >= sizeof(header)) {
        rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP content header too long");
        goto cleanup;
    }
    rc = transport_write_all(&transport, header, (size_t)n);
    if (rc.code != CC_OK) goto cleanup;
    cc_result_free(&rc);
    if (body_len > 0) {
        rc = transport_write_all(&transport, request->body, body_len);
        if (rc.code != CC_OK) goto cleanup;
        cc_result_free(&rc);
    }

    transport.phase_timeout_ms = request->first_byte_timeout_ms > 0 ? (uint64_t)request->first_byte_timeout_ms :
        client->options.first_byte_timeout_ms > 0 ? (uint64_t)client->options.first_byte_timeout_ms : 30000U;
    uint64_t first_read_ms = cc_platform_monotonic_ms();
    transport.phase_deadline_ms = first_read_ms < transport.deadline_ms &&
        transport.phase_timeout_ms < transport.deadline_ms - first_read_ms
        ? first_read_ms + transport.phase_timeout_ms : transport.deadline_ms;
    char chunk[512];
    for (;;) {
        int got = transport_read(&transport, chunk, sizeof(chunk));
        if (got < 0) {
            rc = cc_result_error(transport.io_error, "lwIP transport receive failed");
            goto cleanup;
        }
        if (got == 0) break;
        rc = append_bytes(&raw, &raw_len, request->max_response_bytes, chunk, (size_t)got);
        if (rc.code != CC_OK) goto cleanup;
        cc_result_free(&rc);
        transport.phase_deadline_ms = 0;
        transport.phase_timeout_ms = request->idle_timeout_ms > 0 ? (uint64_t)request->idle_timeout_ms :
            client->options.idle_timeout_ms > 0 ? (uint64_t)client->options.idle_timeout_ms : 60000U;
    }
    transport_close(&transport);
    if (!transport_check(&transport)) {
        rc = cc_result_error(transport.io_error, "HTTP request interrupted before response delivery");
        goto cleanup;
    }

    long response_status = 0;
    size_t body_offset = 0;
    rc = response_validate(request, raw, raw_len, &response_status, &body_offset);
    if (rc.code != CC_OK) goto cleanup;
    cc_result_free(&rc);
    char *body = raw + body_offset;
    size_t response_length = raw_len - body_offset;
    char *response_body = copy_string_len(body, response_length);
    if (!response_body) {
        rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy HTTP body");
        goto cleanup;
    }
    out_response->status_code = response_status;
    out_response->body = response_body;
    out_response->body_size = response_length;

    rc = cc_result_ok();
    if (request->on_body && out_response->body_size > 0) {
        rc = request->on_body(out_response->body, out_response->body_size, request->user_data);
        if (rc.code != CC_OK) goto cleanup;
    }
    if (!transport_check(&transport)) {
        cc_result_free(&rc);
        rc = cc_result_error(transport.io_error, "HTTP request interrupted after response callback");
    }
cleanup:
    transport_close(&transport);
    free(raw);
    return rc;
}

/**
 * @brief 为没有持久连接的端口提供 reset 兼容入口。
 * @param[in] self 当前不读取，可为 NULL。
 * @return 始终 CC_OK，每请求 socket 仍由其独占调用自行关闭。
 */
static cc_result_t freertos_http_reset_connections(void *self)
{
    (void)self;
    return cc_result_ok();
}

/**
 * @brief 消费保存固定选项的实例包装。
 * @param[in,out] self 唯一拥有实例，可为 NULL。
 * @post 实例存储释放，借用指针失效，外层清空 cc_http_client_t 包装。
 * @pre owner 已关闭入口并排空所有借用请求和同步回调。
 */
static void freertos_http_destroy(void *self)
{
    free(self);
}

static const cc_http_client_vtable_t s_freertos_http_vtable = {
    .perform = freertos_http_perform,
    .reset_connections = freertos_http_reset_connections,
    .destroy = freertos_http_destroy,
};

/**
 * @brief 创建只保管固定选项的 FreeRTOS/lwIP 客户端实例。
 * @param[in] options 可空选项，调用期间借用，复制 TLS/阶段预算/retry 标量；retry 当前无读取执行点。
 * @param[out] out_client 非 NULL 新/已销毁输出，先清零，成功交付唯一拥有实例与能力位。
 * @return 成功为 CC_OK，空输出为 INVALID_ARGUMENT，实例分配失败为 OUT_OF_MEMORY。
 * @note 实例没有共享 socket，log_level/trace_persist 当前无实现读取，TLS 默认要求校验而当前后端未配置 CA。
 */
cc_result_t cc_http_client_create_default(
    const cc_http_client_options_t *options,
    cc_http_client_t *out_client)
{
    if (!out_client) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null HTTP client output");
    memset(out_client, 0, sizeof(*out_client));
    cc_freertos_http_client_t *client = calloc(1, sizeof(*client));
    if (!client) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate HTTP client");
    client->options.size = sizeof(client->options);
    client->options.tls_verify = options ? options->tls_verify != 0 : 1;
    client->options.connect_timeout_ms = options ? options->connect_timeout_ms : 15000;
    client->options.first_byte_timeout_ms = options ? options->first_byte_timeout_ms : 30000;
    client->options.idle_timeout_ms = options ? options->idle_timeout_ms : 60000;
    client->options.retry_count = options ? options->retry_count : 0;
    out_client->self = client;
    out_client->vtable = &s_freertos_http_vtable;
    out_client->size = sizeof(*out_client);
    out_client->capabilities = CC_HTTP_CAP_STREAM_BODY | CC_HTTP_CAP_CANCEL |
        CC_HTTP_CAP_RESOLVED_ADDRESS_VALIDATION;
    return cc_result_ok();
}

/**
 * @brief 归还响应拥有的头字段、头数组和正文并清零结构。
 * @param[in,out] response 可空响应，字段为唯一拥有分配，count 与数组匹配。
 * @post 动态字段已消费，结构存储继续由调用者保管，清理前结束所有字段借用。
 */
void cc_http_response_free(cc_http_response_t *response)
{
    if (!response) return;
    for (size_t i = 0; i < response->header_count; i++) {
        free((char *)response->headers[i].name);
        free((char *)response->headers[i].value);
    }
    free(response->headers);
    free(response->body);
    memset(response, 0, sizeof(*response));
}
