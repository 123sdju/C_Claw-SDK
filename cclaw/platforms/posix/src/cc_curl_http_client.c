#include "cc/ports/cc_http_client.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_platform.h"
#include "cc/util/cc_string_builder.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#if CC_HAS_CURL
#include <curl/curl.h>
#endif

typedef struct cc_posix_http_config {
    int tls_verify;
    int connect_timeout_ms;
    int first_byte_timeout_ms;
    int idle_timeout_ms;
    int retry_count;
    int log_level;
    int trace_persist;
} cc_posix_http_config_t;

/* 仅供 observability 关联；POSIX 构建使用 GCC/Clang 的原子内建避免并发请求重复编号。 */
static unsigned g_http_attempt_sequence;

static unsigned next_http_attempt_id(void)
{
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_add_fetch(&g_http_attempt_sequence, 1u, __ATOMIC_RELAXED);
#else
    return ++g_http_attempt_sequence;
#endif
}

typedef struct cc_curl_write_ctx {
    cc_http_response_t *response;
    cc_http_body_callback_fn on_body;
    void *user_data;
    size_t max_response_bytes;
    cc_cancel_token_t *cancel_token;
    cc_result_t callback_error;
    size_t received_bytes;
    int saw_body;
    int saw_stream_body;
    uint64_t start_ms;
    long first_byte_timeout_ms;
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
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *addresses = NULL;
    int gai_rc = getaddrinfo(host, NULL, &hints, &addresses);
    if (gai_rc != 0 || !addresses) {
        if (addresses) freeaddrinfo(addresses);
        return cc_result_error(CC_ERR_NETWORK, "Failed to resolve HTTP host");
    }
    for (struct addrinfo *it = addresses; it; it = it->ai_next) {
        char numeric[NI_MAXHOST];
        if (getnameinfo(it->ai_addr, it->ai_addrlen, numeric, sizeof(numeric),
                NULL, 0, NI_NUMERICHOST) != 0 ||
            !request->validate_address(
                host, numeric, request->validate_address_user_data)) {
            freeaddrinfo(addresses);
            return cc_result_error(CC_ERR_PERMISSION_DENIED,
                "HTTP resolved address rejected by network policy");
        }
    }
    freeaddrinfo(addresses);
    return cc_result_ok();
}

static long choose_long(long request_value, int configured_value, long fallback)
{
    if (request_value > 0) return request_value;
    if (configured_value > 0) return configured_value;
    return fallback;
}

static int choose_int(int request_value, int configured_value)
{
    if (request_value > 0) return request_value;
    return configured_value > 0 ? configured_value : 0;
}

static cc_result_t response_append(
    cc_http_response_t *response,
    const char *data,
    size_t len,
    size_t max_response_bytes
)
{
    if (!response || len == 0) return cc_result_ok();
    if (len > (size_t)-1 - response->body_size - 1) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "HTTP response body is too large");
    }
    if (max_response_bytes > 0 && response->body_size + len > max_response_bytes) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "HTTP response body exceeded limit");
    }

    char *next = realloc(response->body, response->body_size + len + 1);
    if (!next) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow HTTP response body");
    }

    response->body = next;
    memcpy(response->body + response->body_size, data, len);
    response->body_size += len;
    response->body[response->body_size] = '\0';
    return cc_result_ok();
}

/*
 * 流式模式下只保存一段预览 body。
 *
 * on_body 已经把完整数据交给上层；这里的 max_response_bytes 只限制 response->body 预览，
 * 达到上限后停止追加，不把流式请求判为失败。
 */
static cc_result_t response_append_preview(
    cc_http_response_t *response,
    const char *data,
    size_t len,
    size_t max_response_bytes
)
{
    if (!response || !data || len == 0 || max_response_bytes == 0) return cc_result_ok();
    if (response->body_size >= max_response_bytes) return cc_result_ok();

    size_t remaining = max_response_bytes - response->body_size;
    size_t take = len < remaining ? len : remaining;
    return response_append(response, data, take, max_response_bytes);
}

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
 * 解析并保存一行 HTTP 响应头。
 *
 * curl 会把状态行也交给 header callback；没有冒号的行直接忽略。
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
    if (getnameinfo(&address->addr, address->addrlen, numeric, sizeof(numeric),
            NULL, 0, NI_NUMERICHOST) != 0 ||
        (ctx->request && ctx->request->validate_address &&
         !ctx->request->validate_address(
             ctx->host, numeric, ctx->request->validate_address_user_data))) {
        ctx->address_rejected = 1;
        return CURL_SOCKET_BAD;
    }
    return socket(address->family, address->socktype, address->protocol);
}

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

    ctx->saw_body = 1;

    if (ctx->on_body) {
        if (real_size > 0) ctx->saw_stream_body = 1;
        cc_result_t rc = ctx->on_body((const char *)contents, real_size, ctx->user_data);
        if (rc.code != CC_OK) {
            if (ctx->callback_error.code == CC_OK) ctx->callback_error = rc;
            else cc_result_free(&rc);
            return 0;
        }
    }

    cc_result_t rc = ctx->on_body
        ? response_append_preview(ctx->response, (const char *)contents, real_size,
                                  ctx->max_response_bytes)
        : response_append(ctx->response, (const char *)contents, real_size,
                          ctx->max_response_bytes);
    if (rc.code != CC_OK) {
        if (ctx->callback_error.code == CC_OK) ctx->callback_error = rc;
        else cc_result_free(&rc);
        return 0;
    }

    return real_size;
}

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

/*
 * 进度回调负责取消和 first-byte 超时。
 *
 * libcurl 没有直接的 HTTP first body byte timeout 选项；这里在没有收到 body 前用
 * monotonic clock 做平台层检查。
 */
static int cc_curl_progress(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                            curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;

    cc_curl_write_ctx_t *ctx = (cc_curl_write_ctx_t *)clientp;
    if (!ctx) return 0;

    if (cc_cancel_token_is_cancelled(ctx->cancel_token)) {
        if (ctx->callback_error.code == CC_OK) {
            ctx->callback_error = cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled");
        }
        return 1;
    }

    if (!ctx->saw_body && ctx->first_byte_timeout_ms > 0) {
        uint64_t now = cc_platform_monotonic_ms();
        if (now >= ctx->start_ms &&
            now - ctx->start_ms >= (uint64_t)ctx->first_byte_timeout_ms) {
            if (ctx->callback_error.code == CC_OK) {
                ctx->callback_error = cc_result_error(
                    CC_ERR_TIMEOUT,
                    "Timed out waiting for first HTTP response body byte");
            }
            return 1;
        }
    }

    return 0;
}

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

static cc_error_code_t map_curl_error(CURLcode res)
{
    switch (res) {
    case CURLE_OK:
        return CC_OK;
    case CURLE_OPERATION_TIMEDOUT:
        return CC_ERR_TIMEOUT;
    case CURLE_ABORTED_BY_CALLBACK:
        return CC_ERR_CANCELLED;
    case CURLE_OUT_OF_MEMORY:
        return CC_ERR_OUT_OF_MEMORY;
    default:
        return CC_ERR_NETWORK;
    }
}

static int should_retry(CURLcode res, const cc_curl_write_ctx_t *ctx, int attempt_index, int retry_count)
{
    if (attempt_index >= retry_count) return 0;
    if (ctx && (ctx->callback_error.code != CC_OK || ctx->saw_stream_body)) return 0;
    return res == CURLE_COULDNT_RESOLVE_HOST ||
           res == CURLE_COULDNT_CONNECT ||
           res == CURLE_SEND_ERROR ||
           res == CURLE_RECV_ERROR ||
           res == CURLE_OPERATION_TIMEDOUT;
}

static void emit_attempt_metrics(
    const cc_http_request_t *request,
    const cc_http_response_t *response,
    const cc_curl_write_ctx_t *ctx,
    CURLcode res,
    int attempt_index,
    int retry_performed,
    unsigned attempt_id,
    uint64_t start_ms,
    uint64_t end_ms
)
{
    if (!request->on_attempt_metrics) return;

    cc_http_attempt_metrics_t metrics;
    memset(&metrics, 0, sizeof(metrics));
    metrics.size = sizeof(metrics);
    metrics.stage = res == CURLE_OK ? "complete" : "perform";
    metrics.err = res == CURLE_OK ? NULL : curl_easy_strerror(res);
    metrics.status = response ? response->status_code : 0;
    metrics.attempt_id = (int)attempt_id;
    metrics.attempt_index = attempt_index;
    metrics.final_attempt = !retry_performed;
    metrics.total_ms = end_ms >= start_ms ? (int)(end_ms - start_ms) : 0;
    metrics.connect_ms = 0;
    metrics.header_ms = 0;
    metrics.first_body_ms = 0;
    metrics.saw_body = ctx ? ctx->saw_body : 0;
    metrics.reused_connection = 0;
    metrics.connection_generation = attempt_index + 1;
    metrics.origin = request->url;
    metrics.connection_pool = request->connection_pool ? request->connection_pool : "posix-default";
    metrics.reuse_count = 0;
    metrics.idle_ms_before_reuse = 0;
    metrics.max_reuse_reached = 0;
    metrics.connection_discard_reason = "per-request-easy-handle";
    metrics.retry_allowed = retry_performed;
    metrics.retry_performed = retry_performed;
    metrics.partial_stream = ctx && res != CURLE_OK && ctx->saw_stream_body;
    metrics.saw_done = 0;

    request->on_attempt_metrics(&metrics, request->attempt_metrics_user_data);
}

static cc_result_t configure_curl_handle(
    CURL *curl,
    const cc_posix_http_config_t *config,
    const cc_http_request_t *request,
    struct curl_slist *headers,
    cc_curl_write_ctx_t *write_ctx,
    long timeout_ms,
    long connect_timeout_ms,
    long idle_timeout_ms
)
{
    const char *method = request->method ? request->method : "GET";

    curl_easy_setopt(curl, CURLOPT_URL, request->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, config->tls_verify ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, config->tls_verify ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    if (idle_timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)((idle_timeout_ms + 999) / 1000));
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

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cc_curl_write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, write_ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, cc_curl_write_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, write_ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cc_curl_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, write_ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (request->validate_address) {
        curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, cc_curl_open_socket);
        curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, write_ctx);
    }

    /*
     * connection_pool/header_profile 是跨平台 request 契约字段。POSIX 当前使用一次性 easy
     * handle，没有连接池和 header profile 分流，因此字段只参与 metrics，不改变行为。
     */
    (void)request->connection_pool;
    (void)request->header_profile;

    return cc_result_ok();
}
#endif

static cc_result_t posix_http_perform(
    void *self,
    const cc_http_request_t *request,
    cc_http_response_t *out_response
)
{
    cc_posix_http_config_t *config = (cc_posix_http_config_t *)self;
    if (!config || !request || !request->url || !out_response) {
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
    long timeout_ms = request->timeout_ms > 0 ? request->timeout_ms : 120000L;
    if (request->deadline_ms > 0) {
        uint64_t now_ms = cc_platform_monotonic_ms();
        if (now_ms >= request->deadline_ms) {
            return cc_result_error(CC_ERR_TIMEOUT, "HTTP request deadline expired");
        }
        uint64_t remaining = request->deadline_ms - now_ms;
        if (remaining < (uint64_t)timeout_ms) timeout_ms = (long)remaining;
    }
    long connect_timeout_ms = choose_long(request->connect_timeout_ms,
                                          config->connect_timeout_ms,
                                          30000L);
    long first_byte_timeout_ms = choose_long(request->first_byte_timeout_ms,
                                             config->first_byte_timeout_ms,
                                             0L);
    long idle_timeout_ms = choose_long(request->idle_timeout_ms,
                                       config->idle_timeout_ms,
                                       0L);
    int retry_count = choose_int(request->retry_count, config->retry_count);

    struct curl_slist *headers = NULL;
    for (size_t i = 0; i < request->header_count; i++) {
        cc_result_t rc = append_header(&headers, request->headers[i].name, request->headers[i].value);
        if (rc.code != CC_OK) {
            curl_slist_free_all(headers);
            return rc;
        }
    }

    cc_result_t final_error = cc_result_ok();
    for (int attempt = 0; attempt <= retry_count; attempt++) {
        cc_http_response_free(out_response);

        CURL *curl = curl_easy_init();
        if (!curl) {
            final_error = cc_result_error(CC_ERR_NETWORK, "Failed to initialize curl");
            break;
        }
        unsigned attempt_id = next_http_attempt_id();

        cc_curl_write_ctx_t write_ctx;
        memset(&write_ctx, 0, sizeof(write_ctx));
        write_ctx.response = out_response;
        write_ctx.on_body = request->on_body;
        write_ctx.user_data = request->user_data;
        write_ctx.max_response_bytes = request->max_response_bytes;
        write_ctx.cancel_token = request->cancel_token;
        write_ctx.start_ms = cc_platform_monotonic_ms();
        write_ctx.first_byte_timeout_ms = first_byte_timeout_ms;
        write_ctx.request = request;
        memcpy(write_ctx.host, request_host, sizeof(write_ctx.host));

        cc_result_t rc = configure_curl_handle(
            curl,
            config,
            request,
            headers,
            &write_ctx,
            timeout_ms,
            connect_timeout_ms,
            idle_timeout_ms);
        if (rc.code != CC_OK) {
            curl_easy_cleanup(curl);
            final_error = rc;
            break;
        }

        uint64_t attempt_start = cc_platform_monotonic_ms();
        CURLcode res = curl_easy_perform(curl);
        uint64_t attempt_end = cc_platform_monotonic_ms();
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out_response->status_code);
        curl_easy_cleanup(curl);

        int retry_performed = should_retry(res, &write_ctx, attempt, retry_count);
        emit_attempt_metrics(
            request,
            out_response,
            &write_ctx,
            res,
            attempt,
            retry_performed,
            attempt_id,
            attempt_start,
            attempt_end);

        if (write_ctx.callback_error.code != CC_OK) {
            final_error = write_ctx.callback_error;
            memset(&write_ctx.callback_error, 0, sizeof(write_ctx.callback_error));
            break;
        }
        if (write_ctx.address_rejected) {
            final_error = cc_result_error(CC_ERR_PERMISSION_DENIED,
                "HTTP connected address rejected by network policy");
            break;
        }

        if (res == CURLE_OK) {
            final_error = cc_result_ok();
            break;
        }

        if (retry_performed) {
            continue;
        }

        cc_error_code_t code = map_curl_error(res);
        final_error = cc_result_error(code, curl_easy_strerror(res));
        break;
    }

    curl_slist_free_all(headers);

    if (final_error.code != CC_OK) {
        cc_http_response_free(out_response);
        return final_error;
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
 * 重置 POSIX HTTP 连接。
 *
 * 当前 POSIX 实现每次请求创建并清理一个 curl easy handle，没有 SDK 级持久连接池可重置；
 * 因此该函数是明确的 no-op。若后续引入 share/multi handle 池，应在这里关闭并重建池。
 */
static cc_result_t posix_http_reset_connections(void *self)
{
    (void)self;
    return cc_result_ok();
}

/*
 * 配置 POSIX curl HTTP 客户端默认参数。
 *
 * NULL 恢复默认值；非 NULL 时复制 options 中的纯值字段。request 字段仍然拥有最高优先级。
 */
static void posix_http_destroy(void *self)
{
    free(self);
}

static const cc_http_client_vtable_t s_posix_http_vtable = {
    .perform = posix_http_perform,
    .reset_connections = posix_http_reset_connections,
    .destroy = posix_http_destroy,
};

cc_result_t cc_http_client_create_default(
    const cc_http_client_options_t *options,
    cc_http_client_t *out_client)
{
    if (!out_client) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null HTTP client output");
    memset(out_client, 0, sizeof(*out_client));
    cc_posix_http_config_t *config = calloc(1, sizeof(*config));
    if (!config) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate HTTP client");
    config->tls_verify = options ? options->tls_verify != 0 : 1;
    config->connect_timeout_ms = options ? options->connect_timeout_ms : 30000;
    config->first_byte_timeout_ms = options ? options->first_byte_timeout_ms : 0;
    config->idle_timeout_ms = options ? options->idle_timeout_ms : 0;
    config->retry_count = options ? options->retry_count : 0;
    config->log_level = options ? options->log_level : 0;
    config->trace_persist = options ? options->trace_persist : 0;
    out_client->self = config;
    out_client->vtable = &s_posix_http_vtable;
    out_client->size = sizeof(*out_client);
    out_client->capabilities = CC_HTTP_CAP_STREAM_BODY | CC_HTTP_CAP_CANCEL |
        CC_HTTP_CAP_RESOLVED_ADDRESS_VALIDATION;
    return cc_result_ok();
}

/*
 * 释放 HTTP 响应。
 *
 * headers/body 都由 HTTP client 分配；provider/tool 处理完响应后必须调用本函数。
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
