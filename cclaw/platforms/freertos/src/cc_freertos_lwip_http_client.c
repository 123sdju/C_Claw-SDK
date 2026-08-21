



#include "cc/ports/cc_http_client.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/ports/cc_platform.h"

#include <ctype.h>
#include <errno.h>
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
#endif

/* 解析后的 URL，固定长度数组用于避免 MCU 上动态分配。 */
typedef struct parsed_url {
    char scheme[6];
    char host[128];
    char path[256];
    uint16_t port;
    int use_tls;
} parsed_url_t;

/*
 * lwIP/mbedTLS 传输状态。
 *
 * fd 是 TCP socket；启用 mbedTLS 时附带 SSL context/config。tls_active 用于 close 时决定
 * 是否发送 close_notify 和释放 TLS 对象。
 */
typedef struct transport {
    int fd;
#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    uint32_t rng_state;
    int tls_active;
#endif
} transport_t;

typedef struct cc_freertos_http_client {
    cc_http_client_options_t options;
} cc_freertos_http_client_t;

/* 查找请求头；返回值是 request 内部借用指针。 */
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

/* 复制指定长度的字节为 NUL 结尾字符串。 */
static char *copy_string_len(const char *data, size_t len)
{
    char *copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, data, len);
    copy[len] = '\0';
    return copy;
}

/*
 * 追加原始响应字节。
 *
 * max_len 用于限制响应缓存大小；该 lwIP 实现先缓存完整 raw response，再拆 header/body。
 */
static int append_bytes(char **buf, size_t *len, size_t max_len, const char *data, size_t data_len)
{
    if (max_len > 0 && *len + data_len > max_len) return 0;
    char *next = (char *)realloc(*buf, *len + data_len + 1);
    if (!next) return 0;
    memcpy(next + *len, data, data_len);
    *len += data_len;
    next[*len] = '\0';
    *buf = next;
    return 1;
}

/*
 * 解析 http/https URL。
 *
 * 为 MCU 轻量实现，只支持 http:// 和可选 mbedTLS 的 https://，不支持 userinfo、IPv6 或
 * 超长 host/path。
 */
static cc_result_t parse_url(const char *url, parsed_url_t *out)
{
    if (!url) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL is missing");

    memset(out, 0, sizeof(*out));
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
    const char *slash = strchr(cursor, '/');
    const char *host_end = slash ? slash : cursor + strlen(cursor);
    const char *colon = memchr(cursor, ':', (size_t)(host_end - cursor));

    size_t host_len = (size_t)((colon ? colon : host_end) - cursor);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL host is missing or too long");
    }
    memcpy(out->host, cursor, host_len);
    out->host[host_len] = '\0';

    if (colon) {
        long port = strtol(colon + 1, NULL, 10);
        if (port <= 0 || port > 65535) {
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL port is invalid");
        }
        out->port = (uint16_t)port;
    }

    const char *path = slash ? slash : "/";
    if (strlen(path) >= sizeof(out->path)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL path is too long");
    }
    strcpy(out->path, path);
    return cc_result_ok();
}

/*
 * 解析 IPv4 地址。
 *
 * 先尝试 dotted IPv4，再使用 lwIP DNS；当前实现只返回第一个 AF_INET 结果。
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
        if (request->validate_address &&
            !request->validate_address(host, host, request->validate_address_user_data)) {
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
        return cc_result_error(CC_ERR_NETWORK, "lwIP DNS resolution failed");
    }

    struct sockaddr_in *selected = NULL;
    for (struct addrinfo *it = results; it; it = it->ai_next) {
        if (!it->ai_addr || it->ai_family != AF_INET) continue;
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

/*
 * 测试用轻量伪随机数生成器（xorshift）。
 *
 * 为 mbedTLS 裁剪环境提供基础熵源，生产环境应接入硬件 TRNG。
 * 参数：ctx - 32 位状态指针；out - 输出缓冲区；len - 输出字节数。
 * 返回：0 表示成功。
 */
#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
/*
 * 测试用轻量 RNG。
 *
 * 这是示例级伪随机实现，真实产品应接入硬件 TRNG/mbedTLS entropy。这里保留是为了让
 * 裁剪环境能编译 HTTPS 路径。
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

/* mbedTLS send 回调，底层走 lwIP socket。 */
static int tls_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    int sent = lwip_send(fd, buf, len, 0);
    return sent < 0 ? -1 : sent;
}

/* mbedTLS recv 回调，底层走 lwIP socket。 */
static int tls_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    int got = lwip_recv(fd, buf, len, 0);
    return got < 0 ? -1 : got;
}

/*
 * 在已连接 TCP socket 上启动 TLS。
 *
 * 当前配置关闭证书校验以降低移植门槛；生产固件应启用 CA/证书校验，否则 HTTPS 无法防
 * 中间人攻击。
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
    transport->rng_state = 0xc0ffeeu;

    int rc = mbedtls_ssl_config_defaults(&transport->conf,
        MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) return cc_result_error(CC_ERR_NETWORK, "mbedTLS config defaults failed");

    mbedtls_ssl_conf_authmode(&transport->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&transport->conf, test_rng, &transport->rng_state);
    mbedtls_ssl_conf_ciphersuites(&transport->conf, suites);

    rc = mbedtls_ssl_setup(&transport->ssl, &transport->conf);
    if (rc != 0) return cc_result_error(CC_ERR_NETWORK, "mbedTLS ssl setup failed");

    rc = mbedtls_ssl_set_hostname(&transport->ssl, tls_host);
    if (rc != 0) return cc_result_error(CC_ERR_NETWORK, "mbedTLS SNI setup failed");

    mbedtls_ssl_set_bio(&transport->ssl, &transport->fd, tls_send, tls_recv, NULL);
    do {
        rc = mbedtls_ssl_handshake(&transport->ssl);
    } while (0);

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

/* 关闭 transport，按需关闭 TLS，再关闭 lwIP socket。 */
static void transport_close(transport_t *transport)
{
#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
    if (transport->tls_active) {
        (void)mbedtls_ssl_close_notify(&transport->ssl);
    }
    mbedtls_ssl_free(&transport->ssl);
    mbedtls_ssl_config_free(&transport->conf);
#endif
    if (transport->fd >= 0) lwip_close(transport->fd);
    transport->fd = -1;
}

/* 写完整 buffer，处理短写；TLS 和明文 socket 共用这个 helper。 */
static cc_result_t transport_write_all(transport_t *transport, const char *data, size_t len)
{
    while (len > 0) {
        int sent;
#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
        if (transport->tls_active) {
            do {
                sent = mbedtls_ssl_write(&transport->ssl, (const unsigned char *)data, len);
            } while (sent == MBEDTLS_ERR_SSL_WANT_READ || sent == MBEDTLS_ERR_SSL_WANT_WRITE);
        } else
#endif
        {
            sent = lwip_send(transport->fd, data, len, 0);
        }
        if (sent <= 0) return cc_result_error(CC_ERR_NETWORK, "lwIP transport send failed");
        data += sent;
        len -= (size_t)sent;
    }
    return cc_result_ok();
}

/* 从 transport 读取一次数据；TLS 模式下处理 WANT_READ/WANT_WRITE。 */
static int transport_read(transport_t *transport, char *buf, size_t len)
{
#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
    if (transport->tls_active) {
        int got;
        do {
            got = mbedtls_ssl_read(&transport->ssl, (unsigned char *)buf, len);
        } while (got == MBEDTLS_ERR_SSL_WANT_READ || got == MBEDTLS_ERR_SSL_WANT_WRITE);
        if (got == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return got;
    }
#endif
    return lwip_recv(transport->fd, buf, len, 0);
}

/*
 * 执行 FreeRTOS/lwIP HTTP 请求。
 *
 * 该实现手写 HTTP/1.0 请求并缓存完整 raw response，再拆出 body；适合资源受限 profile。
 * 当前不解析响应头数组，stream on_body 在 body 完整缓存后回调，不是逐包流式。
 */
static cc_result_t freertos_http_perform(
    void *self,
    const cc_http_request_t *request,
    cc_http_response_t *out_response)
{
    cc_freertos_http_client_t *client = (cc_freertos_http_client_t *)self;
    if (!client || !request || !request->url || !out_response) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid HTTP request");
    }
    if (request->validate_url &&
        !request->validate_url(request->url, request->validate_url_user_data)) {
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "HTTP URL rejected by network policy");
    }
    if (cc_cancel_token_is_cancelled(request->cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled before start");
    }
    memset(out_response, 0, sizeof(*out_response));

    parsed_url_t url;
    cc_result_t rc = parse_url(request->url, &url);
    if (rc.code != CC_OK) return rc;
    const char *host_header = request_header_value(request, "Host");
    if (!host_header) host_header = url.host;

    transport_t transport;
    memset(&transport, 0, sizeof(transport));
    transport.fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (transport.fd < 0) return cc_result_error(CC_ERR_NETWORK, "lwIP socket creation failed");

    long timeout_ms = request->timeout_ms > 0 ? request->timeout_ms : 120000;
    if (request->deadline_ms > 0) {
        uint64_t now_ms = cc_platform_monotonic_ms();
        if (now_ms >= request->deadline_ms) {
            transport_close(&transport);
            return cc_result_error(CC_ERR_TIMEOUT, "HTTP request deadline expired");
        }
        uint64_t remaining = request->deadline_ms - now_ms;
        if (remaining < (uint64_t)timeout_ms) timeout_ms = (long)remaining;
    }
    if (timeout_ms > 0) {
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        (void)lwip_setsockopt(transport.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)lwip_setsockopt(transport.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    struct sockaddr_in addr;
    rc = resolve_ipv4(request, url.host, url.port, &addr);
    if (rc.code != CC_OK) {
        transport_close(&transport);
        return rc;
    }

    if (lwip_connect(transport.fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        transport_close(&transport);
        return cc_result_error(CC_ERR_NETWORK, "lwIP connect failed");
    }

#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
    if (url.use_tls) {
        if (client->options.tls_verify) {
            transport_close(&transport);
            return cc_result_error(CC_ERR_UNSUPPORTED,
                "FreeRTOS HTTPS requires a configured CA verification backend");
        }
        rc = start_tls(&transport, host_header);
        if (rc.code != CC_OK) {
            transport_close(&transport);
            return rc;
        }
    }
#endif

    const char *method = request->method ? request->method : "GET";
    char header[768];
    int n = snprintf(header, sizeof(header),
        "%s %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n",
        method, url.path, host_header);
    if (n < 0 || (size_t)n >= sizeof(header)) {
        transport_close(&transport);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP request line too long");
    }
    rc = transport_write_all(&transport, header, (size_t)n);
    if (rc.code != CC_OK) {
        transport_close(&transport);
        return rc;
    }

    for (size_t i = 0; i < request->header_count; i++) {
        if (!request->headers[i].name || !request->headers[i].value) continue;
        if (strcasecmp(request->headers[i].name, "Host") == 0) continue;
        n = snprintf(header, sizeof(header), "%s: %s\r\n",
            request->headers[i].name, request->headers[i].value);
        if (n < 0 || (size_t)n >= sizeof(header)) {
            transport_close(&transport);
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP header too long");
        }
        rc = transport_write_all(&transport, header, (size_t)n);
        if (rc.code != CC_OK) {
            transport_close(&transport);
            return rc;
        }
    }

    size_t body_len = request->body ? strlen(request->body) : 0;
    if (body_len > 0) {
        n = snprintf(header, sizeof(header), "Content-Length: %lu\r\n\r\n", (unsigned long)body_len);
        if (n < 0 || (size_t)n >= sizeof(header)) {
            transport_close(&transport);
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP content header too long");
        }
        rc = transport_write_all(&transport, header, (size_t)n);
        if (rc.code == CC_OK) rc = transport_write_all(&transport, request->body, body_len);
    } else {
        rc = transport_write_all(&transport, "\r\n", 2);
    }
    if (rc.code != CC_OK) {
        transport_close(&transport);
        return rc;
    }

    char *raw = NULL;
    size_t raw_len = 0;
    char chunk[512];
    for (;;) {
        if (cc_cancel_token_is_cancelled(request->cancel_token)) {
            transport_close(&transport);
            free(raw);
            return cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled");
        }
        int got = transport_read(&transport, chunk, sizeof(chunk));
        if (got < 0) {
            transport_close(&transport);
            free(raw);
            return cc_result_error(CC_ERR_NETWORK, "lwIP transport receive failed");
        }
        if (got == 0) break;
        if (!append_bytes(&raw, &raw_len, request->max_response_bytes, chunk, (size_t)got)) {
            transport_close(&transport);
            free(raw);
            return cc_result_error(
                request->max_response_bytes > 0 ? CC_ERR_LIMIT_EXCEEDED : CC_ERR_OUT_OF_MEMORY,
                "HTTP response body could not be buffered");
        }
    }
    transport_close(&transport);

    if (!raw) raw = copy_string_len("", 0);
    if (!raw) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate empty HTTP response");

    char *body = strstr(raw, "\r\n\r\n");
    if (strncmp(raw, "HTTP/", 5) == 0) {
        char *status = strchr(raw, ' ');
        if (status) out_response->status_code = strtol(status + 1, NULL, 10);
    }
    if (body) {
        body += 4;
        out_response->body_size = raw_len - (size_t)(body - raw);
        out_response->body = copy_string_len(body, out_response->body_size);
        free(raw);
        if (!out_response->body) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy HTTP body");
    } else {
        out_response->body = raw;
        out_response->body_size = raw_len;
    }

    if (request->on_body && out_response->body_size > 0) {
        return request->on_body(out_response->body, out_response->body_size, request->user_data);
    }
    return cc_result_ok();
}

/*
 * 重置 FreeRTOS lwIP 所有 HTTP 持久连接。
 *
 * 调用后下次请求将重建 TCP/TLS 连接。当前实现为空，lwIP socket 按请求创建和关闭。
 */
static cc_result_t freertos_http_reset_connections(void *self)
{
    (void)self;
    return cc_result_ok();
}

/*
 * 配置 FreeRTOS lwIP HTTP 客户端参数。
 *
 * 参数：options - TLS 验证、超时等配置项。
 * 当前实现使用默认值，options 参数被忽略。
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

/* 释放 lwIP HTTP response 的 headers/body。 */
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
