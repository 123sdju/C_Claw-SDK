



#include "cc/ports/cc_http_client.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_platform.h"
#include "cc/internal/cc_alloc.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#if CC_HAS_CURL
#include <curl/curl.h>
#endif

typedef struct cc_windows_http_client {
    cc_http_client_options_t options;
} cc_windows_http_client_t;

/*
 * Windows curl 写回调上下文。
 *
 * 与 POSIX curl 版本相同：response 保存 header/body，on_body 支持流式消费，
 * callback_error 把回调内的 SDK 错误带回 curl_easy_perform 之后。
 */
typedef struct cc_curl_write_ctx {
    cc_http_response_t *response;
    cc_http_body_callback_fn on_body;
    void *user_data;
    size_t max_response_bytes;
    size_t received_bytes;
    cc_cancel_token_t *cancel_token;


    cc_result_t callback_error;
    const cc_http_request_t *request;
    char host[256];
    int address_rejected;
} cc_curl_write_ctx_t;

static int extract_url_host(const char *url, char *host, size_t host_size)
{
    if (!url || !host || host_size == 0) return 0;
    const char *authority = strstr(url, "://");
    authority = authority ? authority + 3 : url;
    const char *end = authority + strcspn(authority, "/?#");
    const char *at = NULL;
    for (const char *p = authority; p < end; p++) if (*p == '@') at = p;
    const char *start = at ? at + 1 : authority;
    if (start < end && *start == '[') {
        start++;
        const char *close = memchr(start, ']', (size_t)(end - start));
        if (!close) return 0;
        end = close;
    } else {
        const char *colon = memchr(start, ':', (size_t)(end - start));
        if (colon) end = colon;
    }
    size_t len = (size_t)(end - start);
    if (len == 0 || len >= host_size) return 0;
    memcpy(host, start, len);
    host[len] = '\0';
    return 1;
}

static cc_result_t validate_resolved_candidates(
    const cc_http_request_t *request,
    const char *host)
{
    if (!request || !request->validate_address) return cc_result_ok();
    ADDRINFOA hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    PADDRINFOA addresses = NULL;
    if (GetAddrInfoA(host, NULL, &hints, &addresses) != 0 || !addresses) {
        if (addresses) FreeAddrInfoA(addresses);
        return cc_result_error(CC_ERR_NETWORK, "Failed to resolve HTTP host");
    }
    for (PADDRINFOA it = addresses; it; it = it->ai_next) {
        char numeric[NI_MAXHOST];
        if (GetNameInfoA(it->ai_addr, (int)it->ai_addrlen,
                numeric, sizeof(numeric), NULL, 0, NI_NUMERICHOST) != 0 ||
            !request->validate_address(
                host, numeric, request->validate_address_user_data)) {
            FreeAddrInfoA(addresses);
            return cc_result_error(CC_ERR_PERMISSION_DENIED,
                "HTTP resolved address rejected by network policy");
        }
    }
    FreeAddrInfoA(addresses);
    return cc_result_ok();
}

/* 追加响应 body，并按 max_response_bytes 控制最大缓存。 */
static int response_append(
    cc_http_response_t *response,
    const char *data,
    size_t len,
    size_t max_response_bytes
)
{
    if (!response || len == 0) return 1;
    if (len > SIZE_MAX - response->body_size - 1U) return 0;
    if (max_response_bytes > 0 && response->body_size + len > max_response_bytes) {
        return 0;
    }

    char *next = realloc(response->body, response->body_size + len + 1);
    if (!next) return 0;

    response->body = next;
    memcpy(response->body + response->body_size, data, len);
    response->body_size += len;
    response->body[response->body_size] = '\0';
    return 1;
}

/*
 * 按 max_response_bytes 限制向响应 buffer 追加数据片段。
 *
 * 参数：response - HTTP 响应对象；data - 待追加数据；len - 数据长度；max_response_bytes - 最大响应字节数。
 * 返回：1 表示成功追加。
 */
static int response_append_preview(
    cc_http_response_t *response,
    const char *data,
    size_t len,
    size_t max_response_bytes
)
{
    if (!response || !data || len == 0 || max_response_bytes == 0) return 1;
    if (response->body_size >= max_response_bytes) return 1;

    size_t remaining = max_response_bytes - response->body_size;
    size_t take = len < remaining ? len : remaining;
    return response_append(response, data, take, max_response_bytes);
}

/* 复制并裁剪 header name/value 两端空白。 */
static char *copy_trimmed_header_piece(const char *data, size_t len)
{
    while (len > 0 && (*data == ' ' || *data == '\t' || *data == '\r' || *data == '\n')) {
        data++;
        len--;
    }
    while (len > 0) {
        char ch = data[len - 1];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
        len--;
    }

    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, data, len);
    copy[len] = '\0';
    return copy;
}

/*
 * 解析并保存一行响应 header。
 *
 * 没有冒号的状态行会被忽略；保存的 header 字符串由 cc_http_response_free 释放。
 */
static cc_result_t response_header_append(
    cc_http_response_t *response,
    const char *line,
    size_t len
)
{
    if (!response || !line || len == 0) return cc_result_ok();

    const char *colon = memchr(line, ':', len);
    if (!colon) return cc_result_ok();

    size_t name_len = (size_t)(colon - line);
    size_t value_len = len - name_len - 1;
    char *name = copy_trimmed_header_piece(line, name_len);
    char *value = copy_trimmed_header_piece(colon + 1, value_len);
    if (!name || !value) {
        free(name);
        free(value);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy HTTP response header");
    }

    cc_http_header_t *next = realloc(
        response->headers,
        (response->header_count + 1) * sizeof(*response->headers));
    if (!next) {
        free(name);
        free(value);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow HTTP response headers");
    }

    response->headers = next;
    response->headers[response->header_count].name = name;
    response->headers[response->header_count].value = value;
    response->header_count++;
    return cc_result_ok();
}

#if CC_HAS_CURL
static curl_socket_t cc_curl_open_socket(
    void *clientp,
    curlsocktype purpose,
    struct curl_sockaddr *address)
{
    (void)purpose;
    cc_curl_write_ctx_t *ctx = (cc_curl_write_ctx_t *)clientp;
    if (!ctx || !address) return CURL_SOCKET_BAD;
    char numeric[NI_MAXHOST];
    if (GetNameInfoA(&address->addr, (int)address->addrlen,
            numeric, sizeof(numeric), NULL, 0, NI_NUMERICHOST) != 0 ||
        (ctx->request && ctx->request->validate_address &&
         !ctx->request->validate_address(
             ctx->host, numeric, ctx->request->validate_address_user_data))) {
        ctx->address_rejected = 1;
        return CURL_SOCKET_BAD;
    }
    return socket(address->family, address->socktype, address->protocol);
}

/*
 * curl body 回调。
 *
 * 检查取消、调用可选 on_body、必要时缓冲 body；返回 0 表示中止 curl 传输。
 */
static size_t cc_curl_write_body(void *contents, size_t size, size_t nmemb, void *userp)
{
    cc_curl_write_ctx_t *ctx = (cc_curl_write_ctx_t *)userp;
    if (!ctx) return 0;
    if (size != 0 && nmemb > SIZE_MAX / size) {
        ctx->callback_error = cc_result_error(
            CC_ERR_LIMIT_EXCEEDED, "HTTP body size overflow");
        return 0;
    }
    size_t real_size = size * nmemb;
    if (cc_cancel_token_is_cancelled(ctx->cancel_token)) {
        if (ctx->callback_error.code == CC_OK) {
            ctx->callback_error = cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled");
        }
        return 0;
    }
    if (real_size > (size_t)-1 - ctx->received_bytes ||
        (ctx->max_response_bytes > 0 &&
         ctx->received_bytes + real_size > ctx->max_response_bytes)) {
        if (ctx->callback_error.code == CC_OK) {
            ctx->callback_error = cc_result_error(
                CC_ERR_LIMIT_EXCEEDED, "HTTP response body exceeded limit");
        }
        return 0;
    }
    ctx->received_bytes += real_size;

    if (ctx->on_body) {
        cc_result_t rc = ctx->on_body((const char *)contents, real_size, ctx->user_data);
        if (rc.code != CC_OK) {
            if (ctx->callback_error.code == CC_OK) ctx->callback_error = rc;
            else cc_result_free(&rc);
            return 0;
        }
    }

    if (ctx->on_body && ctx->max_response_bytes > 0) {
        if (!response_append_preview(ctx->response, (const char *)contents, real_size,
                                     ctx->max_response_bytes)) {
            if (ctx->callback_error.code == CC_OK) {
                ctx->callback_error = cc_result_error(
                    CC_ERR_OUT_OF_MEMORY,
                    "HTTP response preview could not be buffered");
            }
            return 0;
        }
    } else if (!ctx->on_body) {
        if (!response_append(ctx->response, (const char *)contents, real_size,
                             ctx->max_response_bytes)) {
            if (ctx->callback_error.code == CC_OK) {
                ctx->callback_error = cc_result_error(
                    ctx->max_response_bytes > 0 ? CC_ERR_INVALID_ARGUMENT : CC_ERR_OUT_OF_MEMORY,
                    "HTTP response body could not be buffered");
            }
            return 0;
        }
    }

    return real_size;
}

/* curl header 回调，保存响应头并支持取消。 */
static size_t cc_curl_write_header(void *contents, size_t size, size_t nmemb, void *userp)
{
    cc_curl_write_ctx_t *ctx = (cc_curl_write_ctx_t *)userp;
    if (!ctx) return 0;
    if (size != 0 && nmemb > SIZE_MAX / size) {
        ctx->callback_error = cc_result_error(
            CC_ERR_LIMIT_EXCEEDED, "HTTP header size overflow");
        return 0;
    }
    size_t real_size = size * nmemb;
    if (cc_cancel_token_is_cancelled(ctx->cancel_token)) {
        if (ctx->callback_error.code == CC_OK) {
            ctx->callback_error = cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled");
        }
        return 0;
    }
    cc_result_t rc = response_header_append(ctx->response, (const char *)contents, real_size);
    if (rc.code != CC_OK) {
        if (ctx->callback_error.code == CC_OK) ctx->callback_error = rc;
        else cc_result_free(&rc);
        return 0;
    }
    return real_size;
}

/* curl progress 回调，用于没有 body 数据时也能及时响应 cancel token。 */
static int cc_curl_progress(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                            curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    cc_curl_write_ctx_t *ctx = (cc_curl_write_ctx_t *)clientp;
    if (cc_cancel_token_is_cancelled(ctx ? ctx->cancel_token : NULL)) {
        if (ctx && ctx->callback_error.code == CC_OK) {
            ctx->callback_error = cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled");
        }
        return 1;
    }
    return 0;
}

/* 把 SDK header 转成 curl_slist 行，curl 会复制临时字符串内容。 */
static cc_result_t append_header(struct curl_slist **headers, const char *name, const char *value)
{
    if (!name || !value) return cc_result_ok();

    cc_string_builder_t sb;
    cc_result_t rc = cc_string_builder_init(&sb);
    if (rc.code != CC_OK) return rc;

    rc = cc_string_builder_appendf(&sb, "%s: %s", name, value);
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&sb);
        return rc;
    }

    char *line = cc_string_builder_take(&sb);
    struct curl_slist *next = curl_slist_append(*headers, line);
    free(line);

    if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to append HTTP header");
    *headers = next;
    return cc_result_ok();
}
#endif

/*
 * 执行 Windows HTTP 请求。
 *
 * 当前 Windows profile 使用 libcurl；没有 curl 时返回 CC_ERR_PLATFORM。支持普通缓冲响应、
 * stream on_body、timeout、cancel 和 response header 保存。
 */
static cc_result_t windows_http_perform(
    void *self,
    const cc_http_request_t *request,
    cc_http_response_t *out_response
)
{
    cc_windows_http_client_t *client = (cc_windows_http_client_t *)self;
    if (!client || !request || !request->url || !out_response) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid HTTP request");
    }
    if (request->validate_url &&
        !request->validate_url(request->url, request->validate_url_user_data)) {
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "HTTP URL rejected by network policy");
    }

    char request_host[256];
    if (request->validate_address) {
        if (!extract_url_host(request->url, request_host, sizeof(request_host))) {
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid HTTP URL host");
        }
        cc_result_t address_rc = validate_resolved_candidates(request, request_host);
        if (address_rc.code != CC_OK) return address_rc;
    } else {
        request_host[0] = '\0';
    }

    memset(out_response, 0, sizeof(*out_response));

#if !CC_HAS_CURL
    (void)request;
    return cc_result_error(CC_ERR_PLATFORM, "No HTTP client adapter available in this build");
#else
    CURL *curl = curl_easy_init();
    if (!curl) return cc_result_error(CC_ERR_NETWORK, "Failed to initialize curl");

    struct curl_slist *headers = NULL;
    for (size_t i = 0; i < request->header_count; i++) {
        cc_result_t rc = append_header(&headers, request->headers[i].name, request->headers[i].value);
        if (rc.code != CC_OK) {
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return rc;
        }
    }

    const char *method = request->method ? request->method : "GET";
    curl_easy_setopt(curl, CURLOPT_URL, request->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    long timeout_ms = request->timeout_ms > 0 ? request->timeout_ms : 120000L;
    if (request->deadline_ms > 0) {
        uint64_t now_ms = cc_platform_monotonic_ms();
        if (now_ms >= request->deadline_ms) {
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return cc_result_error(CC_ERR_TIMEOUT, "HTTP request deadline expired");
        }
        uint64_t remaining = request->deadline_ms - now_ms;
        if (remaining < (uint64_t)timeout_ms) timeout_ms = (long)remaining;
    }
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    long connect_timeout_ms = request->connect_timeout_ms > 0 ? request->connect_timeout_ms :
        client->options.connect_timeout_ms;
    if (connect_timeout_ms > 0) curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, client->options.tls_verify ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, client->options.tls_verify ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    if (request->idle_timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                         (long)((request->idle_timeout_ms + 999) / 1000));
    }

    if (strcmp(method, "GET") == 0) {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->body ? request->body : "");
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
        if (request->body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->body);
        }
    }

    cc_curl_write_ctx_t write_ctx;
    memset(&write_ctx, 0, sizeof(write_ctx));
    write_ctx.response = out_response;
    write_ctx.on_body = request->on_body;
    write_ctx.user_data = request->user_data;
    write_ctx.max_response_bytes = request->max_response_bytes;
    write_ctx.cancel_token = request->cancel_token;
    write_ctx.request = request;
    memcpy(write_ctx.host, request_host, sizeof(write_ctx.host));

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cc_curl_write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, cc_curl_write_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &write_ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cc_curl_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &write_ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (request->validate_address) {
        curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, cc_curl_open_socket);
        curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, &write_ctx);
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out_response->status_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (write_ctx.callback_error.code != CC_OK) {
        cc_result_t rc = write_ctx.callback_error;
        memset(&write_ctx.callback_error, 0, sizeof(write_ctx.callback_error));
        cc_http_response_free(out_response);
        return rc;
    }

    if (write_ctx.address_rejected) {
        cc_http_response_free(out_response);
        return cc_result_error(CC_ERR_PERMISSION_DENIED,
            "HTTP connected address rejected by network policy");
    }

    if (res != CURLE_OK) {
        cc_http_response_free(out_response);
        return cc_result_error(CC_ERR_NETWORK, curl_easy_strerror(res));
    }

    if (!out_response->body) {
        out_response->body = cc_copy_string("");
        if (!out_response->body) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate empty HTTP body");
        }
    }

    return cc_result_ok();
#endif
}

/*
 * 重置 Windows curl 所有 HTTP 持久连接。
 *
 * 调用后下次请求将重建 TCP/TLS 连接。当前实现为空，curl 多句柄自动管理连接生命周期。
 */
static cc_result_t windows_http_reset_connections(void *self)
{
    (void)self;
    return cc_result_ok();
}

/*
 * 配置 Windows curl HTTP 客户端参数。
 *
 * 参数：options - TLS 验证、超时等配置项。
 * 当前实现使用默认值，options 参数被忽略。
 */
static void windows_http_destroy(void *self)
{
    free(self);
}

static const cc_http_client_vtable_t s_windows_http_vtable = {
    .perform = windows_http_perform,
    .reset_connections = windows_http_reset_connections,
    .destroy = windows_http_destroy,
};

cc_result_t cc_http_client_create_default(
    const cc_http_client_options_t *options,
    cc_http_client_t *out_client)
{
    if (!out_client) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null HTTP client output");
    memset(out_client, 0, sizeof(*out_client));
    cc_windows_http_client_t *client = calloc(1, sizeof(*client));
    if (!client) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate HTTP client");
    client->options.size = sizeof(client->options);
    client->options.tls_verify = options ? options->tls_verify != 0 : 1;
    client->options.connect_timeout_ms = options ? options->connect_timeout_ms : 30000;
    client->options.first_byte_timeout_ms = options ? options->first_byte_timeout_ms : 0;
    client->options.idle_timeout_ms = options ? options->idle_timeout_ms : 0;
    client->options.retry_count = options ? options->retry_count : 0;
    client->options.log_level = options ? options->log_level : 0;
    client->options.trace_persist = options ? options->trace_persist : 0;
    out_client->self = client;
    out_client->vtable = &s_windows_http_vtable;
    out_client->size = sizeof(*out_client);
    out_client->capabilities = CC_HTTP_CAP_STREAM_BODY | CC_HTTP_CAP_CANCEL |
        CC_HTTP_CAP_RESOLVED_ADDRESS_VALIDATION;
    return cc_result_ok();
}

/* 释放 Windows curl response 的 headers/body。 */
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
