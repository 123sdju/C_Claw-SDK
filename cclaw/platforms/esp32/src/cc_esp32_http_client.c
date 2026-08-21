#ifdef ESP_PLATFORM
#include "cc/ports/cc_http_client.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/internal/cc_alloc.h"

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * ESP32 HTTP 事件上下文。
 *
 * 这版做了两件事：
 * 1. response/body/header 的动态内存使用标准 malloc/realloc/free 域。
 * 2. 复用同一个 esp_http_client_handle_t，开启 HTTP keep-alive，避免每次 LLM 请求都重新
 *    建 TCP/TLS 连接。若服务器关闭旧连接或网络错误，会清理 handle 并自动重试一次。
 */

#ifndef CC_ESP32_HTTP_BUFFER_SIZE
#define CC_ESP32_HTTP_BUFFER_SIZE 4096
#endif

#ifndef CC_ESP32_HTTP_BUFFER_SIZE_TX
#define CC_ESP32_HTTP_BUFFER_SIZE_TX 4096
#endif

#ifndef CC_ESP32_HTTP_KEEPALIVE_IDLE_SEC
#define CC_ESP32_HTTP_KEEPALIVE_IDLE_SEC 5
#endif

#ifndef CC_ESP32_HTTP_KEEPALIVE_INTERVAL_SEC
#define CC_ESP32_HTTP_KEEPALIVE_INTERVAL_SEC 5
#endif

#ifndef CC_ESP32_HTTP_KEEPALIVE_COUNT
#define CC_ESP32_HTTP_KEEPALIVE_COUNT 3
#endif

#define CC_HTTP_LOG_TRACE 0
#define CC_HTTP_LOG_DEBUG 1
#define CC_HTTP_LOG_INFO 2
#define CC_HTTP_LOG_WARN 3
#define CC_HTTP_LOG_ERROR 4
#define CC_HTTP_LOG_FATAL 5

#define CC_HTTP_DEFAULT_CONNECT_TIMEOUT_MS 15000
#define CC_HTTP_DEFAULT_FIRST_BYTE_TIMEOUT_MS 30000
#define CC_HTTP_DEFAULT_IDLE_TIMEOUT_MS 60000
#define CC_HTTP_DEFAULT_RETRY_COUNT 1
#define CC_HTTP_POOL_IDLE_TTL_MS 45000
#define CC_HTTP_POOL_MAX_REUSE 12
#define CC_HTTP_POOL_STALE_THRESHOLD 2
#define CC_HTTP_POOL_COOLDOWN_MS 60000
#define CC_HTTP_POOL_RECOVER_SUCCESS 3
#define CC_HTTP_MAX_HOST_LENGTH 255
#define CC_HTTP_MAX_REDIRECT_URL 2048
#define CC_HTTP_MAX_RESPONSE_HEADERS 64U
#define CC_HTTP_MAX_RESPONSE_HEADER_BYTES (16U * 1024U)

static const char *TAG = "cc_http_client";

typedef enum cc_esp32_http_pool_id {
    CC_HTTP_POOL_LLM_STREAM = 0,
    CC_HTTP_POOL_LLM_SYNC = 1,
    CC_HTTP_POOL_LOCAL_API = 2,
    CC_HTTP_POOL_DEFAULT = 3,
    CC_HTTP_POOL_COUNT = 4
} cc_esp32_http_pool_id_t;

typedef struct cc_esp32_http_ctx cc_esp32_http_ctx_t;
typedef struct cc_esp32_http_client_state cc_esp32_http_client_state_t;

typedef struct cc_esp32_http_pool {
    cc_esp32_http_client_state_t *owner;
    esp_http_client_handle_t client;
    SemaphoreHandle_t mutex;
    cc_esp32_http_ctx_t *active_ctx;
    int timeout_ms;
    int healthy;
    unsigned generation;
    char origin[160];
    char header_profile[96];
    const char *name;
    int reuse_count;
    int stale_reused_failures;
    int cooldown_until_ms;
    int recover_successes;
    int last_used_ms;
    char method[16];
} cc_esp32_http_pool_t;

struct cc_esp32_http_ctx {
    cc_esp32_http_client_state_t *owner;
    cc_http_response_t *response;
    cc_http_body_callback_fn on_body;
    void *user_data;
    cc_http_attempt_metrics_callback_fn on_attempt_metrics;
    void *attempt_metrics_user_data;
    size_t max_response_bytes;
    size_t received_bytes;
    size_t response_header_bytes;
    cc_cancel_token_t *cancel_token;
    cc_result_t callback_error;
    unsigned attempt_id;
    int attempt_index;
    int total_timeout_ms;
    int connect_timeout_ms;
    int first_byte_timeout_ms;
    int idle_timeout_ms;
    int retry_count;
    int saw_body;
    int stream_request;
    int disconnected;
    int metrics_emitted;
    int retry_allowed;
    int retry_performed;
    int partial_stream;
    int idle_ms_before_reuse;
    int max_reuse_reached;
    int reuse_count;
    const char *connection_discard_reason;
    cc_esp32_http_pool_t *pool;
    char pool_name[24];
    /* 当前请求绑定的持久连接诊断状态，用于日志和 observability。 */
    int reused_connection;
    unsigned connection_generation;
    char origin[160];
    int64_t start_us;
    int64_t connected_us;
    int64_t first_header_us;
    int64_t first_body_us;
    int64_t finish_us;
    cc_http_address_validator_fn validate_address;
    void *validate_address_user_data;
    char request_host[CC_HTTP_MAX_HOST_LENGTH + 1];
    int peer_validated;
};

struct cc_esp32_http_client_state {
    SemaphoreHandle_t mutex;
    int tls_verify;
    int connect_timeout_ms;
    int first_byte_timeout_ms;
    int idle_timeout_ms;
    int retry_count;
    int log_level;
    int trace_persist;
    unsigned attempt_sequence;
    cc_esp32_http_pool_t pools[CC_HTTP_POOL_COUNT];
};

static unsigned http_next_attempt_id(cc_esp32_http_client_state_t *state)
{
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_add_fetch(&state->attempt_sequence, 1u, __ATOMIC_RELAXED);
#else
    return ++state->attempt_sequence;
#endif
}

/*
 * 判断指定日志级别是否启用。
 *
 * 参数：level - 内部日志级别（CC_HTTP_LOG_*）。
 * 返回：1 表示该级别已启用，0 表示被抑制。
 */
static int http_log_enabled(const cc_esp32_http_client_state_t *state, int level)
{
    return state && state->log_level <= level;
}

/*
 * 将内部日志级别映射为 ESP-IDF 日志级别。
 *
 * 参数：level - 内部日志级别（CC_HTTP_LOG_*）。
 * 返回：对应的 esp_log_level_t。
 */
static esp_log_level_t http_esp_log_level(int level)
{
    if (level <= CC_HTTP_LOG_DEBUG) return ESP_LOG_DEBUG;
    if (level == CC_HTTP_LOG_INFO) return ESP_LOG_INFO;
    if (level == CC_HTTP_LOG_WARN) return ESP_LOG_WARN;
    if (level == CC_HTTP_LOG_ERROR) return ESP_LOG_ERROR;
    return ESP_LOG_NONE;
}

/*
 * 获取当前微秒级时间戳，用于 HTTP 请求各阶段耗时测量。
 *
 * 基于 FreeRTOS tick 计数换算，精度受 portTICK_PERIOD_MS 影响。
 * 返回：微秒级时间戳。
 */
static int64_t http_now_us(void)
{
    return (int64_t)xTaskGetTickCount() * (int64_t)portTICK_PERIOD_MS * 1000;
}

static int http_now_ms(void)
{
    return (int)pdTICKS_TO_MS(xTaskGetTickCount());
}

/*
 * 计算两个微秒时间戳之间的毫秒差值。
 *
 * 参数：start_us - 起始微秒时间戳；end_us - 结束微秒时间戳。
 * 返回：毫秒差值，无效参数时返回 -1。
 */
static int elapsed_ms(int64_t start_us, int64_t end_us)
{
    if (start_us <= 0 || end_us <= 0 || end_us < start_us) {
        return -1;
    }
    return (int)((end_us - start_us) / 1000);
}

/*
 * 返回正值或回退默认值，用于超时参数的优雅降级。
 *
 * 参数：value - 首选值；fallback - 回退值。
 * 返回：value 为正时返回 value，否则返回 fallback。
 */
static int positive_or(int value, int fallback)
{
    return value > 0 ? value : fallback;
}

/*
 * 返回两个正整数中的较小值。
 *
 * 任一参数 <= 0 时直接返回另一个，用于超时值的安全比较。
 * 参数：a, b - 待比较的两个超时值（毫秒）。
 * 返回：较小的正值（毫秒）。
 */
static int min_positive(int a, int b)
{
    if (a <= 0) return b;
    if (b <= 0) return a;
    return a < b ? a : b;
}

/*
 * 获取或创建 HTTP 客户端全局互斥锁，采用延迟初始化策略。
 *
 * 首次调用时创建 FreeRTOS 二值信号量作为互斥锁，后续复用同一句柄。
 * 返回：互斥锁句柄，内存不足时返回 NULL。
 */
static SemaphoreHandle_t http_mutex_get(cc_esp32_http_client_state_t *state)
{
    if (state && !state->mutex) {
        state->mutex = xSemaphoreCreateMutex();
    }
    return state ? state->mutex : NULL;
}

/* 调用前持有全局配置锁；每个池独立串行，避免长 stream 阻塞其它用途的 HTTP。 */
static SemaphoreHandle_t http_pool_mutex_get_locked(cc_esp32_http_pool_t *pool)
{
    if (!pool) return NULL;
    if (!pool->mutex) {
        pool->mutex = xSemaphoreCreateMutex();
    }
    return pool->mutex;
}

/* 配置变更和显式 reset 需要停止所有池；锁顺序固定为全局锁再按数组顺序取池锁。 */
static int http_lock_all_pools_locked(cc_esp32_http_client_state_t *state)
{
    int locked = 0;
    for (int i = 0; i < CC_HTTP_POOL_COUNT; i++) {
        SemaphoreHandle_t mutex = http_pool_mutex_get_locked(&state->pools[i]);
        if (!mutex) {
            while (locked-- > 0) {
                xSemaphoreGive(state->pools[locked].mutex);
            }
            return 0;
        }
        xSemaphoreTake(mutex, portMAX_DELAY);
        locked++;
    }
    return 1;
}

static void http_unlock_all_pools_locked(cc_esp32_http_client_state_t *state)
{
    for (int i = CC_HTTP_POOL_COUNT - 1; i >= 0; i--) {
        if (state->pools[i].mutex) {
            xSemaphoreGive(state->pools[i].mutex);
        }
    }
}

/*
 * 从完整 URL 中提取 origin 部分（scheme://host:port）。
 *
 * 解析 https?:// 前缀后的 host[:port]，不含路径和查询参数。
 * 参数：url - 完整 URL；out - 输出缓冲区；out_size - 输出缓冲区大小（字节）。
 */
static void http_origin_from_url(const char *url, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!url || !*url) {
        return;
    }

    const char *scheme_end = strstr(url, "://");
    const char *end = NULL;
    if (scheme_end) {
        end = scheme_end + 3;
    } else {
        end = url;
    }
    while (*end && *end != '/' && *end != '?' && *end != '#') {
        end++;
    }

    size_t len = (size_t)(end - url);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, url, len);
    out[len] = '\0';
}

static int http_ascii_equal(const char *left, const char *right)
{
    if (!left || !right) return 0;
    while (*left && *right) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) return 0;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

/*
 * Parse only the authority host needed by DNS and peer validation.
 * User-info is rejected because credentials embedded in a redirect URL are both ambiguous and unsafe.
 */
static cc_result_t http_host_from_url(
    const char *url,
    char *out_host,
    size_t out_size)
{
    if (!url || !out_host || out_size == 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid HTTP URL host output");
    }
    out_host[0] = '\0';
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end || (size_t)(scheme_end - url) == 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL requires an explicit scheme");
    }
    size_t scheme_len = (size_t)(scheme_end - url);
    if (!((scheme_len == 4 && strncmp(url, "http", 4) == 0) ||
          (scheme_len == 5 && strncmp(url, "https", 5) == 0))) {
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "HTTP URL scheme is not allowed");
    }

    const char *authority = scheme_end + 3;
    const char *authority_end = authority + strcspn(authority, "/?#");
    if (authority == authority_end || memchr(authority, '@', (size_t)(authority_end - authority))) {
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "HTTP URL user-info is not allowed");
    }

    const char *host_start = authority;
    const char *host_end = authority_end;
    if (*host_start == '[') {
        host_start++;
        host_end = memchr(host_start, ']', (size_t)(authority_end - host_start));
        if (!host_end || host_end == host_start) {
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Malformed IPv6 URL host");
        }
    } else {
        const char *colon = memchr(host_start, ':', (size_t)(authority_end - host_start));
        if (colon) host_end = colon;
    }

    size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len > CC_HTTP_MAX_HOST_LENGTH || host_len >= out_size) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP URL host is invalid");
    }
    memcpy(out_host, host_start, host_len);
    out_host[host_len] = '\0';
    return cc_result_ok();
}

static cc_result_t http_numeric_address(
    const struct sockaddr *address,
    socklen_t address_len,
    char *out,
    size_t out_size)
{
    (void)address_len;
    if (!address || !out || out_size == 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid HTTP address output");
    }

    const void *numeric_address = NULL;
    int address_family = address->sa_family;
    if (address_family == AF_INET) {
        numeric_address = &((const struct sockaddr_in *)address)->sin_addr;
    } else if (address_family == AF_INET6) {
        numeric_address = &((const struct sockaddr_in6 *)address)->sin6_addr;
    } else {
        return cc_result_error(CC_ERR_NETWORK, "Unsupported HTTP address family");
    }

    if (!inet_ntop(address_family, numeric_address, out, out_size) || out[0] == '\0') {
        return cc_result_error(CC_ERR_NETWORK, "Failed to format resolved HTTP address");
    }
    return cc_result_ok();
}

/* Reject the whole request if any resolver candidate violates policy. */
static cc_result_t http_validate_resolved_candidates(
    const cc_http_request_t *request,
    char *out_host,
    size_t out_host_size)
{
    cc_result_t rc = http_host_from_url(request->url, out_host, out_host_size);
    if (rc.code != CC_OK || !request->validate_address) return rc;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *addresses = NULL;
    int gai_rc = getaddrinfo(out_host, NULL, &hints, &addresses);
    if (gai_rc != 0 || !addresses) {
        if (addresses) freeaddrinfo(addresses);
        return cc_result_error(CC_ERR_NETWORK, "HTTP DNS resolution failed");
    }

    size_t candidate_count = 0;
    for (struct addrinfo *it = addresses; it; it = it->ai_next) {
        if (!it->ai_addr ||
            (it->ai_family != AF_INET && it->ai_family != AF_INET6)) {
            continue;
        }
        candidate_count++;
        char numeric[INET6_ADDRSTRLEN + 16];
        numeric[0] = '\0';
        rc = http_numeric_address(
            it->ai_addr, (socklen_t)it->ai_addrlen, numeric, sizeof(numeric));
        if (rc.code != CC_OK ||
            !request->validate_address(
                out_host, numeric, request->validate_address_user_data)) {
            cc_result_free(&rc);
            freeaddrinfo(addresses);
            return cc_result_error(
                CC_ERR_PERMISSION_DENIED,
                "HTTP resolved address rejected by network policy");
        }
        cc_result_free(&rc);
    }
    freeaddrinfo(addresses);
    if (candidate_count == 0) {
        return cc_result_error(CC_ERR_NETWORK, "HTTP DNS returned no usable address");
    }
    return cc_result_ok();
}

static cc_result_t http_validate_connected_peer(
    esp_http_client_handle_t client,
    cc_esp32_http_ctx_t *ctx)
{
    if (!ctx || !ctx->validate_address) return cc_result_ok();
    int sock = esp_http_client_get_socket(client);
    if (sock < 0) {
        return cc_result_error(CC_ERR_NETWORK, "HTTP connected socket is unavailable");
    }
    struct sockaddr_storage peer;
    socklen_t peer_len = sizeof(peer);
    memset(&peer, 0, sizeof(peer));
    if (getpeername(sock, (struct sockaddr *)&peer, &peer_len) != 0) {
        return cc_result_error(CC_ERR_NETWORK, "HTTP connected peer lookup failed");
    }
    char numeric[INET6_ADDRSTRLEN + 16];
    numeric[0] = '\0';
    cc_result_t rc = http_numeric_address(
        (const struct sockaddr *)&peer, peer_len, numeric, sizeof(numeric));
    if (rc.code != CC_OK) return rc;
    if (!ctx->validate_address(
            ctx->request_host, numeric, ctx->validate_address_user_data)) {
        return cc_result_error(
            CC_ERR_PERMISSION_DENIED,
            "HTTP connected peer rejected by network policy");
    }
    ctx->peer_validated = 1;
    return cc_result_ok();
}

static int origin_is_local_api(const char *origin)
{
    if (!origin) return 0;
    return strstr(origin, "://127.0.0.1") != NULL ||
           strstr(origin, "://localhost") != NULL ||
           strstr(origin, "://192.168.") != NULL ||
           strstr(origin, "://10.") != NULL ||
           strstr(origin, "://172.16.") != NULL ||
           strstr(origin, "://172.17.") != NULL ||
           strstr(origin, "://172.18.") != NULL ||
           strstr(origin, "://172.19.") != NULL ||
           strstr(origin, "://172.2") != NULL ||
           strstr(origin, "://172.30.") != NULL ||
           strstr(origin, "://172.31.") != NULL ||
           strstr(origin, ".local") != NULL;
}

static cc_esp32_http_pool_t *http_pool_by_name(
    cc_esp32_http_client_state_t *state,
    const char *name)
{
    if (!name || !*name) return NULL;
    for (int i = 0; i < CC_HTTP_POOL_COUNT; i++) {
        if (state->pools[i].name && strcmp(state->pools[i].name, name) == 0) {
            return &state->pools[i];
        }
    }
    return NULL;
}

static cc_esp32_http_pool_t *http_pool_for_request(
    cc_esp32_http_client_state_t *state,
    const cc_http_request_t *request,
    const char *origin)
{
    cc_esp32_http_pool_t *explicit_pool = http_pool_by_name(
        state, request ? request->connection_pool : NULL);
    if (explicit_pool) return explicit_pool;
    if (origin_is_local_api(origin)) return &state->pools[CC_HTTP_POOL_LOCAL_API];
    if (request && request->on_body) return &state->pools[CC_HTTP_POOL_LLM_STREAM];
    return &state->pools[CC_HTTP_POOL_LLM_SYNC];
}

static const char *request_header_profile(const cc_http_request_t *request)
{
    return request && request->header_profile ? request->header_profile : "";
}

static void http_pool_record_discard(cc_esp32_http_ctx_t *ctx, const char *reason)
{
    if (ctx && reason && !ctx->connection_discard_reason) {
        ctx->connection_discard_reason = reason;
    }
}

/*
 * 清理并关闭持久 HTTP 客户端句柄（调用前需持有互斥锁）。
 *
 * 通过 esp_http_client_cleanup 释放 handle，重置全局连接状态。
 * 参数：reason - 清理原因描述（用于调试日志）。
 */
static void http_client_cleanup_locked(
    cc_esp32_http_client_state_t *state,
    const char *reason)
{
    for (int i = 0; i < CC_HTTP_POOL_COUNT; i++) {
        cc_esp32_http_pool_t *pool = &state->pools[i];
        if (!pool->client) continue;
        if (http_log_enabled(state, CC_HTTP_LOG_DEBUG)) {
            ESP_LOGD(TAG, "HTTP persistent handle close pool=%s gen=%u origin=%s%s%s",
                     pool->name ? pool->name : "?",
                     pool->generation,
                     pool->origin[0] ? pool->origin : "?",
                     reason ? ", reason=" : "",
                     reason ? reason : "");
        }
        esp_http_client_cleanup(pool->client);
        pool->client = NULL;
        pool->timeout_ms = 0;
        pool->healthy = 0;
        pool->reuse_count = 0;
        pool->last_used_ms = 0;
        pool->origin[0] = '\0';
        pool->header_profile[0] = '\0';
        pool->method[0] = '\0';
    }
}

static void http_pool_cleanup_locked(cc_esp32_http_pool_t *pool, const char *reason)
{
    if (!pool || !pool->client) return;
    if (http_log_enabled(pool->owner, CC_HTTP_LOG_DEBUG)) {
        ESP_LOGD(TAG, "HTTP persistent handle close pool=%s gen=%u origin=%s%s%s",
                 pool->name ? pool->name : "?",
                 pool->generation,
                 pool->origin[0] ? pool->origin : "?",
                 reason ? ", reason=" : "",
                 reason ? reason : "");
    }
    esp_http_client_cleanup(pool->client);
    pool->client = NULL;
    pool->timeout_ms = 0;
    pool->healthy = 0;
    pool->reuse_count = 0;
    pool->last_used_ms = 0;
    pool->origin[0] = '\0';
    pool->header_profile[0] = '\0';
    pool->method[0] = '\0';
}

/*
 * 配置 ESP32 HTTP 客户端的全局参数。
 *
 * 设置 TLS 证书验证、连接/首字节/空闲超时以及重试次数，默认值见宏定义。
 * 参数：options - HTTP 客户端配置选项，可为 NULL 以使用默认值。
 */
/*
 * 追加响应 body。
 *
 * max_response_bytes 用于 MCU RAM 保护；超过限制或 realloc 失败返回 0，事件回调会中止请求。
 */
static int response_append(
    cc_http_response_t *response,
    const char *data,
    size_t len,
    size_t max_response_bytes
)
{
    if (!response || len == 0) return 1;
    if (max_response_bytes > 0 && response->body_size + len > max_response_bytes) {
        return 0;
    }

    char *next = (char *)realloc(response->body, response->body_size + len + 1);
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

/*
 * 保存一个响应头。
 *
 * header name/value 都深拷贝到 response，供 provider 错误分类读取 Retry-After 等字段。
 */
static cc_result_t response_header_append(
    cc_http_response_t *response,
    const char *name,
    const char *value
)
{
    if (!response || !name || !value) return cc_result_ok();

    cc_http_header_t *next = (cc_http_header_t *)realloc(
        response->headers,
        (response->header_count + 1) * sizeof(*response->headers));
    if (!next) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow HTTP response headers");
    }

    response->headers = next;
    response->headers[response->header_count].name = cc_copy_string(name);
    response->headers[response->header_count].value = cc_copy_string(value);
    if (!response->headers[response->header_count].name ||
        !response->headers[response->header_count].value) {
        free((char *)response->headers[response->header_count].name);
        free((char *)response->headers[response->header_count].value);
        response->headers[response->header_count].name = NULL;
        response->headers[response->header_count].value = NULL;
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy HTTP response header");
    }

    response->header_count++;
    return cc_result_ok();
}

/*
 * ESP-IDF ignores event-handler return values for CONNECTED/HEADER/DATA. Close the transport
 * inside the callback so peer rejection, cancellation, limits and body callback errors stop I/O.
 */
static esp_err_t http_abort_from_event(
    esp_http_client_event_t *evt,
    cc_esp32_http_ctx_t *ctx,
    const char *reason)
{
    http_pool_record_discard(ctx, reason);
    if (ctx && ctx->pool) ctx->pool->healthy = 0;
    if (evt && evt->client) {
        (void)esp_http_client_close(evt->client);
    }
    return ESP_FAIL;
}

/*
 * ESP-IDF HTTP event handler。
 *
 * ON_HEADER 保存 header，ON_DATA 可走 stream 回调或 body 缓冲；每次事件都检查 cancel token，
 * 让长请求可以被 runtime 取消。
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    cc_esp32_http_pool_t *pool = evt ? (cc_esp32_http_pool_t *)evt->user_data : NULL;
    cc_esp32_http_ctx_t *ctx = pool ? pool->active_ctx : NULL;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
        if (ctx && evt->client && ctx->validate_address) {
            cc_result_free(&ctx->callback_error);
            ctx->callback_error = http_validate_connected_peer(evt->client, ctx);
            if (ctx->callback_error.code != CC_OK) {
                return http_abort_from_event(evt, ctx, "peer_validation_failed");
            }
        }
        if (ctx && ctx->connected_us == 0) {
            ctx->connected_us = http_now_us();
            if (ctx->first_byte_timeout_ms > 0 && evt->client) {
                (void)esp_http_client_set_timeout_ms(evt->client, ctx->first_byte_timeout_ms);
                if (ctx->pool) ctx->pool->timeout_ms = ctx->first_byte_timeout_ms;
            }
        }
        if (http_log_enabled(pool ? pool->owner : NULL, CC_HTTP_LOG_DEBUG)) {
            ESP_LOGD(TAG, "HTTP connected pool=%s gen=%u attempt=%u origin=%s elapsed_ms=%d",
                     ctx && ctx->pool ? ctx->pool->name : "?",
                     ctx && ctx->pool ? ctx->pool->generation : 0,
                     ctx ? ctx->attempt_id : 0,
                     ctx && ctx->pool && ctx->pool->origin[0] ? ctx->pool->origin : "?",
                     ctx ? elapsed_ms(ctx->start_us, ctx->connected_us) : -1);
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        if (ctx) {
            ctx->finish_us = http_now_us();
        }
        if (http_log_enabled(pool ? pool->owner : NULL, CC_HTTP_LOG_DEBUG)) {
            ESP_LOGD(TAG, "HTTP response finished pool=%s gen=%u attempt=%u status=%d complete=%d total_ms=%d",
                     ctx && ctx->pool ? ctx->pool->name : "?",
                     ctx && ctx->pool ? ctx->pool->generation : 0,
                     ctx ? ctx->attempt_id : 0,
                     evt->client ? esp_http_client_get_status_code(evt->client) : 0,
                     evt->client ? (int)esp_http_client_is_complete_data_received(evt->client) : 0,
                     ctx ? elapsed_ms(ctx->start_us, ctx->finish_us) : -1);
        }
        break;
    case HTTP_EVENT_DISCONNECTED:
        if (ctx) {
            ctx->disconnected = 1;
            http_pool_record_discard(ctx, "peer_closed");
            if (ctx->pool) ctx->pool->healthy = 0;
        }
        if (http_log_enabled(pool ? pool->owner : NULL, CC_HTTP_LOG_DEBUG)) {
            ESP_LOGD(TAG, "HTTP transport disconnected pool=%s gen=%u attempt=%u origin=%s",
                     ctx && ctx->pool ? ctx->pool->name : "?",
                     ctx && ctx->pool ? ctx->pool->generation : 0,
                     ctx ? ctx->attempt_id : 0,
                     ctx && ctx->pool && ctx->pool->origin[0] ? ctx->pool->origin : "?");
        }
        break;
    default:
        break;
    }

    if (!ctx) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (ctx->first_header_us == 0) {
            ctx->first_header_us = http_now_us();
        }
        if (cc_cancel_token_is_cancelled(ctx->cancel_token)) {
            ctx->callback_error = cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled");
            return http_abort_from_event(evt, ctx, "header_cancelled");
        }
        size_t name_len = evt->header_key ? strlen(evt->header_key) : 0;
        size_t value_len = evt->header_value ? strlen(evt->header_value) : 0;
        if (ctx->response->header_count >= CC_HTTP_MAX_RESPONSE_HEADERS ||
            name_len > SIZE_MAX - value_len ||
            name_len + value_len > SIZE_MAX - ctx->response_header_bytes ||
            ctx->response_header_bytes + name_len + value_len >
                CC_HTTP_MAX_RESPONSE_HEADER_BYTES) {
            ctx->callback_error = cc_result_error(
                CC_ERR_LIMIT_EXCEEDED, "HTTP response headers exceeded limit");
            return http_abort_from_event(evt, ctx, "header_limit_exceeded");
        }
        ctx->response_header_bytes += name_len + value_len;
        ctx->callback_error = response_header_append(ctx->response, evt->header_key, evt->header_value);
        return ctx->callback_error.code == CC_OK
            ? ESP_OK : http_abort_from_event(evt, ctx, "header_callback_error");
    }

    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }

    const char *data = (const char *)evt->data;
    size_t len = (size_t)evt->data_len;
    if (ctx->first_body_us == 0) {
        ctx->first_body_us = http_now_us();
        ctx->saw_body = 1;
        if (ctx->idle_timeout_ms > 0 && evt->client) {
            (void)esp_http_client_set_timeout_ms(evt->client, ctx->idle_timeout_ms);
            if (ctx->pool) ctx->pool->timeout_ms = ctx->idle_timeout_ms;
        }
        if (http_log_enabled(ctx->owner, CC_HTTP_LOG_DEBUG)) {
            ESP_LOGD(TAG, "HTTP first body pool=%s gen=%u attempt=%u first_body_ms=%d idle_timeout_ms=%d",
                     ctx->pool ? ctx->pool->name : "?",
                     ctx->pool ? ctx->pool->generation : 0,
                     ctx->attempt_id,
                     elapsed_ms(ctx->start_us, ctx->first_body_us),
                     ctx->idle_timeout_ms);
        }
    }
    if (cc_cancel_token_is_cancelled(ctx->cancel_token)) {
        ctx->callback_error = cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled");
        return http_abort_from_event(evt, ctx, "body_cancelled");
    }
    if (len > (size_t)-1 - ctx->received_bytes ||
        (ctx->max_response_bytes > 0 &&
         ctx->received_bytes + len > ctx->max_response_bytes)) {
        ctx->callback_error = cc_result_error(
            CC_ERR_LIMIT_EXCEEDED, "HTTP response body exceeded limit");
        return http_abort_from_event(evt, ctx, "body_limit_exceeded");
    }
    ctx->received_bytes += len;

    /* Redirect bodies are transport details, but still consume the request response budget. */
    int status_code = evt->client ? esp_http_client_get_status_code(evt->client) : 0;
    if (status_code == 301 || status_code == 302 || status_code == 303 ||
        status_code == 307 || status_code == 308) {
        return ESP_OK;
    }

    if (ctx->on_body) {
        ctx->callback_error = ctx->on_body(data, len, ctx->user_data);
        if (ctx->callback_error.code != CC_OK) {
            return http_abort_from_event(evt, ctx, "body_callback_error");
        }
    }

    if (ctx->on_body && ctx->max_response_bytes > 0) {
        if (!response_append_preview(ctx->response, data, len, ctx->max_response_bytes)) {
            ctx->callback_error = cc_result_error(CC_ERR_OUT_OF_MEMORY, "HTTP response preview allocation failed");
            return http_abort_from_event(evt, ctx, "body_preview_error");
        }
    } else if (!ctx->on_body) {
        if (!response_append(ctx->response, data, len, ctx->max_response_bytes)) {
            ctx->callback_error = cc_result_error(CC_ERR_OUT_OF_MEMORY, "HTTP response buffer full");
            return http_abort_from_event(evt, ctx, "body_buffer_error");
        }
    }

    return ESP_OK;
}

/*
 * 获取请求级别的总超时时间（毫秒）。
 *
 * 参数：request - HTTP 请求对象。
 * 返回：超时毫秒数，0 表示无限制。
 */
static int request_total_timeout_ms(
    const cc_esp32_http_client_state_t *state,
    const cc_http_request_t *request)
{
    (void)state;
    int total = request && request->timeout_ms > 0 ? (int)request->timeout_ms : 120000;
    if (request && request->deadline_ms > 0) {
        uint64_t now_ms = (uint64_t)http_now_ms();
        if (now_ms >= request->deadline_ms) return -1;
        uint64_t remaining = request->deadline_ms - now_ms;
        if (remaining < (uint64_t)total) total = (int)remaining;
    }
    return total;
}

/*
 * 获取请求级别的连接超时时间（毫秒），请求未指定时使用全局默认值。
 *
 * 参数：request - HTTP 请求对象。
 * 返回：连接超时毫秒数。
 */
static int request_connect_timeout_ms(
    const cc_esp32_http_client_state_t *state,
    const cc_http_request_t *request)
{
    return request && request->connect_timeout_ms > 0 ?
        (int)request->connect_timeout_ms : state->connect_timeout_ms;
}

/*
 * 获取请求级别的首字节超时时间（毫秒），请求未指定时使用全局默认值。
 *
 * 参数：request - HTTP 请求对象。
 * 返回：首字节超时毫秒数。
 */
static int request_first_byte_timeout_ms(
    const cc_esp32_http_client_state_t *state,
    const cc_http_request_t *request)
{
    return request && request->first_byte_timeout_ms > 0 ?
        (int)request->first_byte_timeout_ms : state->first_byte_timeout_ms;
}

/*
 * 获取请求级别的空闲超时时间（毫秒），请求未指定时使用全局默认值。
 *
 * 参数：request - HTTP 请求对象。
 * 返回：空闲超时毫秒数。
 */
static int request_idle_timeout_ms(
    const cc_esp32_http_client_state_t *state,
    const cc_http_request_t *request)
{
    return request && request->idle_timeout_ms > 0 ?
        (int)request->idle_timeout_ms : state->idle_timeout_ms;
}

/*
 * 获取请求级别的重试次数，请求未指定时使用全局默认值。
 *
 * 参数：request - HTTP 请求对象。
 * 返回：重试次数。
 */
static int request_retry_count(
    const cc_esp32_http_client_state_t *state,
    const cc_http_request_t *request)
{
    return request && request->retry_count > 0 ? request->retry_count : state->retry_count;
}

/*
 * 根据连接复用状态计算初始超时时间。
 *
 * 复用已有连接时使用首字节超时，新建连接时使用连接超时，最终不超过总超时。
 * 参数：request - HTTP 请求对象；reusing_connection - 是否复用已有持久连接。
 * 返回：初始超时毫秒数。
 */
static int request_initial_timeout_ms(
    const cc_esp32_http_client_state_t *state,
    const cc_http_request_t *request,
    int reusing_connection)
{
    int total = request_total_timeout_ms(state, request);
    int connect = positive_or(request_connect_timeout_ms(state, request),
                              CC_HTTP_DEFAULT_CONNECT_TIMEOUT_MS);
    int first_byte = positive_or(request_first_byte_timeout_ms(state, request),
                                 CC_HTTP_DEFAULT_FIRST_BYTE_TIMEOUT_MS);
    int initial = reusing_connection ? first_byte : connect;
    return min_positive(initial, total);
}

static cc_result_t http_client_ensure_pool_locked(const cc_http_request_t *request, cc_esp32_http_ctx_t *ctx)
{
    cc_esp32_http_client_state_t *state = ctx ? ctx->owner : NULL;
    if (!state) return cc_result_error(CC_ERR_INVALID_STATE, "HTTP client state is unavailable");
    char origin[160];
    http_origin_from_url(request->url, origin, sizeof(origin));
    cc_esp32_http_pool_t *pool = http_pool_for_request(state, request, origin);
    const char *profile = request_header_profile(request);
    const char *method = request->method ? request->method : "GET";
    int now_ms = http_now_ms();
    int timeout_ms = request_initial_timeout_ms(state, request, pool->client && pool->healthy);
    if (timeout_ms < 0) return cc_result_error(CC_ERR_TIMEOUT, "HTTP request deadline expired");

    if (ctx) {
        ctx->pool = pool;
        snprintf(ctx->pool_name, sizeof(ctx->pool_name), "%s", pool->name ? pool->name : "default");
    }

    if (pool->client && !pool->healthy) {
        http_pool_record_discard(ctx, "unhealthy_handle");
        http_pool_cleanup_locked(pool, "unhealthy_handle");
    }
    if (pool->client && pool->cooldown_until_ms > now_ms) {
        http_pool_record_discard(ctx, "pool_cooldown");
        http_pool_cleanup_locked(pool, "pool_cooldown");
    }
    if (pool->client && pool->last_used_ms > 0) {
        int idle_ms = now_ms - pool->last_used_ms;
        if (idle_ms < 0) idle_ms = 0;
        if (ctx) ctx->idle_ms_before_reuse = idle_ms;
        if (idle_ms > CC_HTTP_POOL_IDLE_TTL_MS) {
            http_pool_record_discard(ctx, "idle_ttl_expired");
            http_pool_cleanup_locked(pool, "idle_ttl_expired");
        }
    }
    if (pool->client && pool->reuse_count >= CC_HTTP_POOL_MAX_REUSE) {
        if (ctx) ctx->max_reuse_reached = 1;
        http_pool_record_discard(ctx, "max_reuse_reached");
        http_pool_cleanup_locked(pool, "max_reuse_reached");
    }
    if (pool->client && strcmp(pool->origin, origin) != 0) {
        http_pool_record_discard(ctx, "origin_changed");
        http_pool_cleanup_locked(pool, "origin_changed");
    }
    if (pool->client && strcmp(pool->header_profile, profile) != 0) {
        http_pool_record_discard(ctx, "header_profile_changed");
        http_pool_cleanup_locked(pool, "header_profile_changed");
    }
    if (pool->client && strcmp(pool->method, method) != 0) {
        http_pool_record_discard(ctx, "method_changed");
        http_pool_cleanup_locked(pool, "method_changed");
    }
    if (pool->client && pool->timeout_ms != timeout_ms) {
        esp_err_t timeout_err = esp_http_client_set_timeout_ms(pool->client, timeout_ms);
        if (timeout_err == ESP_OK) {
            pool->timeout_ms = timeout_ms;
        } else {
            http_pool_record_discard(ctx, "timeout_update_failed");
            http_pool_cleanup_locked(pool, "timeout_update_failed");
        }
    }

    if (pool->client) {
        if (ctx) {
            ctx->reused_connection = 1;
            ctx->connection_generation = pool->generation;
            ctx->reuse_count = pool->reuse_count;
            snprintf(ctx->origin, sizeof(ctx->origin), "%s", pool->origin);
        }
        return cc_result_ok();
    }

    esp_http_client_config_t config = {
        .url = request->url,
        .timeout_ms = timeout_ms,
        .event_handler = http_event_handler,
        .user_data = pool,
        .crt_bundle_attach = state->tls_verify ? esp_crt_bundle_attach : NULL,
        .skip_cert_common_name_check = state->tls_verify ? false : true,
        .disable_auto_redirect = true,
        .buffer_size = CC_ESP32_HTTP_BUFFER_SIZE,
        .buffer_size_tx = CC_ESP32_HTTP_BUFFER_SIZE_TX,
        .keep_alive_enable = true,
        .keep_alive_idle = CC_ESP32_HTTP_KEEPALIVE_IDLE_SEC,
        .keep_alive_interval = CC_ESP32_HTTP_KEEPALIVE_INTERVAL_SEC,
        .keep_alive_count = CC_ESP32_HTTP_KEEPALIVE_COUNT,
    };

    pool->client = esp_http_client_init(&config);
    if (!pool->client) {
        return cc_result_error(CC_ERR_NETWORK, "Failed to initialize esp_http_client");
    }
    pool->timeout_ms = timeout_ms;
    pool->healthy = 0;
    pool->reuse_count = 0;
    pool->last_used_ms = 0;
    snprintf(pool->origin, sizeof(pool->origin), "%s", origin);
    snprintf(pool->header_profile, sizeof(pool->header_profile), "%s", profile);
    snprintf(pool->method, sizeof(pool->method), "%s", method);
    pool->generation++;
    if (ctx) {
        ctx->reused_connection = 0;
        ctx->connection_generation = pool->generation;
        snprintf(ctx->origin, sizeof(ctx->origin), "%s", pool->origin);
    }
    if (!state->tls_verify && http_log_enabled(state, CC_HTTP_LOG_WARN)) {
        ESP_LOGW(TAG, "HTTP TLS verify disabled for pool=%s origin=%s",
                 pool->name ? pool->name : "?",
                 pool->origin[0] ? pool->origin : "?");
    }
    return cc_result_ok();
}

static cc_result_t http_client_apply_request_locked(
    esp_http_client_handle_t client,
    const cc_http_request_t *request
)
{
    esp_err_t err = esp_http_client_set_url(client, request->url);
    if (err != ESP_OK) {
        return cc_result_error(CC_ERR_NETWORK, esp_err_to_name(err));
    }

    const char *method = request->method ? request->method : "GET";
    /* 自定义 method 通过 override 表达时不能残留到下一次 GET/POST。 */
    (void)esp_http_client_delete_header(client, "X-HTTP-Method-Override");
    if (strcmp(method, "POST") == 0) {
        err = esp_http_client_set_method(client, HTTP_METHOD_POST);
    } else if (strcmp(method, "GET") == 0) {
        err = esp_http_client_set_method(client, HTTP_METHOD_GET);
    } else {
        err = esp_http_client_set_method(client, HTTP_METHOD_POST);
        if (err == ESP_OK) {
            err = esp_http_client_set_header(client, "X-HTTP-Method-Override", method);
        }
    }
    if (err != ESP_OK) {
        return cc_result_error(CC_ERR_NETWORK, esp_err_to_name(err));
    }

    /*
     * esp_http_client 的 header 是 handle 级状态。复用前已比较 method 与不含 value 的
     * header_profile；任一集合变化都会重建 handle，避免旧 header 泄漏到不同请求形状。
     */
    for (size_t i = 0; i < request->header_count; i++) {
        if (request->headers[i].name && request->headers[i].value) {
            err = esp_http_client_set_header(client, request->headers[i].name, request->headers[i].value);
            if (err != ESP_OK) {
                return cc_result_error(CC_ERR_NETWORK, esp_err_to_name(err));
            }
        }
    }

    /*
     * 明确要求 keep-alive。服务器仍可能主动 close；这种情况下 perform 会失败或下一次请求
     * 失败，我们会清理 handle 并自动重试一次。
     */
    esp_http_client_set_header(client, "Connection", "keep-alive");

    if (request->body) {
        err = esp_http_client_set_post_field(client, request->body, (int)strlen(request->body));
    } else {
        err = esp_http_client_set_post_field(client, "", 0);
    }
    if (err != ESP_OK) {
        return cc_result_error(CC_ERR_NETWORK, esp_err_to_name(err));
    }

    return cc_result_ok();
}

/*
 * 执行单次 HTTP 请求的完整生命周期（调用前需持有互斥锁）。
 *
 * 包含 handle 初始化、请求配置、事件驱动执行、超时监控和取消令牌检查。
 * 参数：request - HTTP 请求对象；ctx - 执行上下文；out_response - 输出响应；out_err - 输出 ESP 底层错误码。
 * 返回：cc_result_t 结果。
 */
static cc_result_t http_perform_once_locked(
    const cc_http_request_t *request,
    cc_esp32_http_ctx_t *ctx,
    cc_http_response_t *out_response,
    esp_err_t *out_err
)
{
    ctx->start_us = http_now_us();
    ctx->connected_us = 0;
    ctx->first_header_us = 0;
    ctx->first_body_us = 0;
    ctx->finish_us = 0;
    ctx->saw_body = 0;
    ctx->stream_request = request->on_body != NULL;
    ctx->total_timeout_ms = request_total_timeout_ms(ctx->owner, request);
    if (ctx->total_timeout_ms < 0) {
        return cc_result_error(CC_ERR_TIMEOUT, "HTTP request deadline expired");
    }
    ctx->connect_timeout_ms = positive_or(request_connect_timeout_ms(ctx->owner, request),
                                          CC_HTTP_DEFAULT_CONNECT_TIMEOUT_MS);
    ctx->first_byte_timeout_ms = positive_or(request_first_byte_timeout_ms(ctx->owner, request),
                                             CC_HTTP_DEFAULT_FIRST_BYTE_TIMEOUT_MS);
    ctx->idle_timeout_ms = positive_or(request_idle_timeout_ms(ctx->owner, request),
                                       CC_HTTP_DEFAULT_IDLE_TIMEOUT_MS);
    ctx->retry_count = request_retry_count(ctx->owner, request);

    cc_result_t rc = http_client_ensure_pool_locked(request, ctx);
    if (rc.code != CC_OK) {
        return rc;
    }
    /*
     * 复用连接不会产生 HTTP_EVENT_ON_CONNECTED。把 connected_us 固定为请求起点，
     * 使 connect_ms=0，避免复用请求被误判为 connect_timeout。
     */
    if (ctx->reused_connection && ctx->connected_us == 0) {
        ctx->connected_us = ctx->start_us;
    }

    esp_http_client_handle_t client = ctx->pool ? ctx->pool->client : NULL;
    if (!client) {
        return cc_result_error(CC_ERR_NETWORK, "HTTP pool has no active client");
    }
    if (ctx->reused_connection && ctx->validate_address) {
        rc = http_validate_connected_peer(client, ctx);
        if (rc.code != CC_OK) {
            http_pool_record_discard(ctx, "reused_peer_validation_failed");
            http_pool_cleanup_locked(ctx->pool, "reused_peer_validation_failed");
            return rc;
        }
    }

    rc = http_client_apply_request_locked(client, request);
    if (rc.code != CC_OK) {
        return rc;
    }

    ctx->pool->active_ctx = ctx;
    esp_err_t err = esp_http_client_perform(client);
    ctx->pool->active_ctx = NULL;
    if (ctx->finish_us == 0) {
        ctx->finish_us = http_now_us();
    }

    out_response->status_code = esp_http_client_get_status_code(client);
    if (err == ESP_OK && !esp_http_client_is_complete_data_received(client)) {
        err = ESP_ERR_HTTP_EAGAIN;
        http_pool_record_discard(ctx, ctx->stream_request ? "stream_incomplete" : "incomplete_data");
        if (ctx->stream_request && ctx->saw_body) ctx->partial_stream = 1;
    }
    if (out_err) {
        *out_err = err;
    }

    return cc_result_ok();
}

/*
 * 判断 HTTP 请求失败的阶段，用于错误诊断。
 *
 * 参数：ctx - 请求执行上下文。
 * 返回：失败阶段字符串（"connect_timeout"/"first_byte_timeout"/"idle_timeout"）。
 */
static const char *http_failure_stage(const cc_esp32_http_ctx_t *ctx)
{
    if (!ctx || ctx->connected_us == 0) {
        return "connect_timeout";
    }
    if (ctx->first_header_us == 0) {
        return "header_timeout";
    }
    if (ctx->stream_request && ctx->first_body_us > 0) {
        return "stream_idle_timeout";
    }
    if (ctx->first_body_us == 0) {
        return "first_body_timeout";
    }
    return "network_error";
}

/*
 * 发送单次请求尝试的指标数据，通过回调通知上层。
 *
 * 汇总各阶段耗时（total/connect/header/first_body）并调用 on_attempt_metrics 回调。
 * 参数：ctx - 执行上下文；err - ESP 错误码；status_code - HTTP 状态码；final_attempt - 是否最后一次尝试；
 *       total_ms/connect_ms/header_ms/first_body_ms - 各阶段耗时（毫秒）。
 */
static void http_emit_attempt_metrics(cc_esp32_http_ctx_t *ctx,
                                      esp_err_t err,
                                      long status_code,
                                      int final_attempt,
                                      int total_ms,
                                      int connect_ms,
                                      int header_ms,
                                      int first_body_ms)
{
    if (!ctx || !ctx->on_attempt_metrics || ctx->metrics_emitted) {
        return;
    }
    ctx->metrics_emitted = 1;
    cc_http_attempt_metrics_t metrics;
    memset(&metrics, 0, sizeof(metrics));
    metrics.size = sizeof(metrics);
    metrics.stage = err == ESP_OK ? "ok" : http_failure_stage(ctx);
    metrics.err = err == ESP_OK ? "ESP_OK" : esp_err_to_name(err);
    metrics.status = status_code;
    metrics.attempt_id = (int)ctx->attempt_id;
    metrics.attempt_index = ctx->attempt_index;
    metrics.final_attempt = final_attempt;
    metrics.total_ms = total_ms;
    metrics.connect_ms = connect_ms;
    metrics.header_ms = header_ms;
    metrics.first_body_ms = first_body_ms;
    metrics.saw_body = ctx->saw_body;
    metrics.reused_connection = ctx->reused_connection;
    metrics.connection_generation = (int)ctx->connection_generation;
    metrics.origin = ctx->origin[0] ? ctx->origin : NULL;
    metrics.connection_pool = ctx->pool_name[0] ? ctx->pool_name : NULL;
    metrics.reuse_count = ctx->reuse_count;
    metrics.idle_ms_before_reuse = ctx->idle_ms_before_reuse;
    metrics.max_reuse_reached = ctx->max_reuse_reached;
    metrics.connection_discard_reason = ctx->connection_discard_reason;
    metrics.retry_allowed = ctx->retry_allowed;
    metrics.retry_performed = ctx->retry_performed;
    metrics.partial_stream = ctx->partial_stream;
    metrics.saw_done = 0;
    ctx->on_attempt_metrics(&metrics, ctx->attempt_metrics_user_data);
}

/*
 * 记录单次请求尝试的摘要日志并发送耗时指标。
 *
 * 成功时在 DEBUG 级别输出，失败时在 WARN 级别输出。
 * 参数：ctx - 执行上下文；err - ESP 错误码；status_code - HTTP 状态码；final_attempt - 是否最后一次尝试。
 */
static void http_log_attempt_summary(cc_esp32_http_ctx_t *ctx,
                                     esp_err_t err,
                                     long status_code,
                                     int final_attempt)
{
    if (!ctx) return;
    int total_ms = elapsed_ms(ctx->start_us, ctx->finish_us);
    int connect_ms = elapsed_ms(ctx->start_us, ctx->connected_us);
    int header_ms = elapsed_ms(ctx->start_us, ctx->first_header_us);
    int first_body_ms = elapsed_ms(ctx->start_us, ctx->first_body_us);

    http_emit_attempt_metrics(ctx, err, status_code, final_attempt,
                              total_ms, connect_ms, header_ms, first_body_ms);

    if (err == ESP_OK && http_log_enabled(ctx->owner, CC_HTTP_LOG_DEBUG)) {
        ESP_LOGD(TAG,
                 "HTTP attempt summary attempt=%u index=%d status=%ld total_ms=%d connect_ms=%d header_ms=%d first_body_ms=%d body=%d reused=%d gen=%u origin=%s",
                 ctx->attempt_id,
                 ctx->attempt_index,
                 status_code,
                 total_ms,
                 connect_ms,
                 header_ms,
                 first_body_ms,
                 ctx->saw_body,
                 ctx->reused_connection,
                 ctx->connection_generation,
                 ctx->origin[0] ? ctx->origin : "?");
        return;
    }

    if (err != ESP_OK && (final_attempt || http_log_enabled(ctx->owner, CC_HTTP_LOG_DEBUG)) &&
        http_log_enabled(ctx->owner, CC_HTTP_LOG_WARN)) {
        ESP_LOGW(TAG,
                 "HTTP attempt failed attempt=%u index=%d stage=%s err=%s status=%ld total_ms=%d connect_ms=%d header_ms=%d first_body_ms=%d body=%d reused=%d gen=%u origin=%s",
                 ctx->attempt_id,
                 ctx->attempt_index,
                 http_failure_stage(ctx),
                 esp_err_to_name(err),
                 status_code,
                 total_ms,
                 connect_ms,
                 header_ms,
                 first_body_ms,
                 ctx->saw_body,
                 ctx->reused_connection,
                 ctx->connection_generation,
                 ctx->origin[0] ? ctx->origin : "?");
    }
}

/*
 * 将 ESP HTTP 错误转换为统一的 cc_result_t 结果。
 *
 * 根据 ctx 的失败阶段判定超时或网络错误。
 * 参数：ctx - 请求执行上下文；err - ESP 底层错误码。
 * 返回：cc_result_t 错误结果。
 */
static cc_result_t http_error_result(const cc_esp32_http_ctx_t *ctx, esp_err_t err)
{
    const char *stage = http_failure_stage(ctx);
    cc_error_code_t code =
        strstr(stage, "timeout") != NULL ? CC_ERR_TIMEOUT : CC_ERR_NETWORK;
    return cc_result_errf(code, "HTTP %s: %s", stage, esp_err_to_name(err));
}

/*
 * 执行 ESP32 HTTP 请求。
 *
 * 使用 esp_http_client 和证书 bundle；POST/GET 原生支持，其它 method 用
 * X-HTTP-Method-Override 退化。
 *
 * 本实现按用途复用 esp_http_client_handle_t；同一池内串行，不同池可并行执行。
 * 连接错误会重建对应 handle，并仅在尚未收到 body 时按请求重试策略尝试恢复。
 */
static void http_pool_note_failure_locked(cc_esp32_http_ctx_t *ctx, esp_err_t err)
{
    if (!ctx || !ctx->pool || err == ESP_OK) return;
    cc_esp32_http_pool_t *pool = ctx->pool;
    const char *stage = http_failure_stage(ctx);
    const char *reason = ctx->connection_discard_reason;
    if (!reason) {
        if (ctx->reused_connection && strcmp(stage, "header_timeout") == 0) {
            reason = "stale_reused_header_timeout";
        } else if (ctx->stream_request && ctx->saw_body) {
            reason = "stream_incomplete";
        } else if (ctx->disconnected) {
            reason = "peer_closed";
        } else {
            reason = "transport_failure";
        }
        http_pool_record_discard(ctx, reason);
    }

    if (ctx->reused_connection && strcmp(stage, "header_timeout") == 0) {
        pool->stale_reused_failures++;
        if (pool->stale_reused_failures >= CC_HTTP_POOL_STALE_THRESHOLD) {
            pool->cooldown_until_ms = http_now_ms() + CC_HTTP_POOL_COOLDOWN_MS;
            pool->recover_successes = 0;
        }
    }
    http_pool_cleanup_locked(pool, reason);
}

static void http_pool_note_success_locked(cc_esp32_http_ctx_t *ctx)
{
    if (!ctx || !ctx->pool) return;
    cc_esp32_http_pool_t *pool = ctx->pool;
    pool->healthy = ctx->disconnected ? 0 : 1;
    pool->last_used_ms = http_now_ms();
    pool->reuse_count++;
    if (!ctx->reused_connection) {
        pool->recover_successes++;
        if (pool->recover_successes >= CC_HTTP_POOL_RECOVER_SUCCESS) {
            pool->stale_reused_failures = 0;
            pool->cooldown_until_ms = 0;
        }
    } else {
        pool->stale_reused_failures = 0;
    }
}

static cc_result_t esp32_http_perform_single(
    void *self,
    const cc_http_request_t *request,
    cc_http_response_t *out_response
)
{
    cc_esp32_http_client_state_t *state = (cc_esp32_http_client_state_t *)self;
    if (!state || !request || !request->url || !out_response) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid HTTP request");
    }
    memset(out_response, 0, sizeof(*out_response));
    if (request->validate_url &&
        !request->validate_url(request->url, request->validate_url_user_data)) {
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "HTTP URL rejected by network policy");
    }
    char request_host[CC_HTTP_MAX_HOST_LENGTH + 1];
    cc_result_t address_rc = http_validate_resolved_candidates(
        request, request_host, sizeof(request_host));
    if (address_rc.code != CC_OK) return address_rc;

    if (cc_cancel_token_is_cancelled(request->cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled before start");
    }

    SemaphoreHandle_t config_mutex = http_mutex_get(state);
    if (!config_mutex) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create HTTP mutex");
    }

    char origin[160];
    http_origin_from_url(request->url, origin, sizeof(origin));
    xSemaphoreTake(config_mutex, portMAX_DELAY);
    cc_esp32_http_pool_t *pool = http_pool_for_request(state, request, origin);
    SemaphoreHandle_t pool_mutex = http_pool_mutex_get_locked(pool);
    if (!pool_mutex) {
        xSemaphoreGive(config_mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create HTTP pool mutex");
    }
    xSemaphoreTake(pool_mutex, portMAX_DELAY);
    xSemaphoreGive(config_mutex);

    cc_esp32_http_ctx_t ctx;
    esp_err_t err = ESP_OK;
    cc_result_t rc = cc_result_ok();
    int retry_count = request_retry_count(state, request);
    int max_attempts = retry_count + 1;

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        memset(&ctx, 0, sizeof(ctx));
        ctx.owner = state;
        ctx.response = out_response;
        ctx.on_body = request->on_body;
        ctx.user_data = request->user_data;
        ctx.on_attempt_metrics = request->on_attempt_metrics;
        ctx.attempt_metrics_user_data = request->attempt_metrics_user_data;
        ctx.max_response_bytes = request->max_response_bytes;
        ctx.cancel_token = request->cancel_token;
        ctx.attempt_id = http_next_attempt_id(state);
        ctx.attempt_index = attempt;
        ctx.validate_address = request->validate_address;
        ctx.validate_address_user_data = request->validate_address_user_data;
        snprintf(ctx.request_host, sizeof(ctx.request_host), "%s", request_host);

        err = ESP_OK;
        rc = http_perform_once_locked(request, &ctx, out_response, &err);
        if (rc.code != CC_OK || ctx.callback_error.code != CC_OK || err == ESP_OK) {
            break;
        }

        int can_retry = attempt + 1 < max_attempts && !ctx.saw_body;
        ctx.retry_allowed = can_retry;
        if (!can_retry) {
            break;
        }

        ctx.retry_performed = 1;
        http_log_attempt_summary(&ctx, err, out_response->status_code, 0);
        cc_http_response_free(out_response);
        memset(out_response, 0, sizeof(*out_response));
        http_pool_note_failure_locked(&ctx, err);

        if (http_log_enabled(state, CC_HTTP_LOG_DEBUG)) {
            ESP_LOGD(TAG, "HTTP retry scheduled after %s attempt=%u next_index=%d",
                     http_failure_stage(&ctx),
                     ctx.attempt_id,
                     attempt + 2);
        }
        vTaskDelay(pdMS_TO_TICKS(attempt == 0 ? 250 : 750));
    }

    /*
     * Retry only before any response body has been delivered. Once streaming
     * output starts, retrying would duplicate user-visible content.
    */
    if (ctx.callback_error.code != CC_OK) {
        if (ctx.stream_request && ctx.saw_body) ctx.partial_stream = 1;
        http_pool_record_discard(&ctx, "callback_error");
        http_pool_note_failure_locked(&ctx, ESP_FAIL);
        http_log_attempt_summary(&ctx, ESP_FAIL, out_response->status_code, 1);
        cc_http_response_free(out_response);
        rc = ctx.callback_error;
    } else if (rc.code != CC_OK) {
        http_pool_record_discard(&ctx, "request_setup_failed");
        http_pool_note_failure_locked(&ctx, ESP_FAIL);
        http_log_attempt_summary(&ctx, ESP_FAIL, out_response->status_code, 1);
    } else if (rc.code == CC_OK && err != ESP_OK) {
        if (ctx.stream_request && ctx.saw_body) ctx.partial_stream = 1;
        http_pool_note_failure_locked(&ctx, err);
        http_log_attempt_summary(&ctx, err, out_response->status_code, 1);
        cc_http_response_free(out_response);
        rc = http_error_result(&ctx, err);
    } else if (rc.code == CC_OK) {
        http_pool_note_success_locked(&ctx);
        http_log_attempt_summary(&ctx, ESP_OK, out_response->status_code, 1);
    }

    if (rc.code == CC_OK && !out_response->body && !request->on_body) {
        out_response->body = cc_copy_string("");
        if (!out_response->body) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate empty HTTP body");
        }
    }

    xSemaphoreGive(pool_mutex);
    return rc;
}

static int http_is_redirect_status(long status)
{
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

static const char *http_response_header(
    const cc_http_response_t *response,
    const char *name)
{
    if (!response || !name) return NULL;
    for (size_t i = 0; i < response->header_count; i++) {
        if (http_ascii_equal(response->headers[i].name, name)) {
            return response->headers[i].value;
        }
    }
    return NULL;
}

static char *http_redirect_url(const char *base_url, const char *location)
{
    if (!base_url || !location || !location[0]) return NULL;
    while (*location && isspace((unsigned char)*location)) location++;
    size_t location_len = strcspn(location, "\r\n");
    while (location_len > 0 &&
           isspace((unsigned char)location[location_len - 1])) {
        location_len--;
    }
    if (location_len == 0 || location_len >= CC_HTTP_MAX_REDIRECT_URL) return NULL;

    if ((location_len >= 7 && strncmp(location, "http://", 7) == 0) ||
        (location_len >= 8 && strncmp(location, "https://", 8) == 0)) {
        char *absolute = malloc(location_len + 1);
        if (!absolute) return NULL;
        memcpy(absolute, location, location_len);
        absolute[location_len] = '\0';
        return absolute;
    }

    const char *scheme_end = strstr(base_url, "://");
    if (!scheme_end) return NULL;
    const char *authority = scheme_end + 3;
    const char *path = strchr(authority, '/');
    const char *base_end = base_url + strlen(base_url);
    if (!path) path = base_end;
    size_t prefix_len = 0;

    if (location_len >= 2 && location[0] == '/' && location[1] == '/') {
        prefix_len = (size_t)(scheme_end - base_url) + 1;
    } else if (location[0] == '/') {
        prefix_len = (size_t)(path - base_url);
    } else {
        if (path == base_end) {
            size_t base_len = strlen(base_url);
            if (base_len > CC_HTTP_MAX_REDIRECT_URL - location_len - 2) return NULL;
            char *absolute = malloc(base_len + location_len + 2);
            if (!absolute) return NULL;
            memcpy(absolute, base_url, base_len);
            absolute[base_len] = '/';
            memcpy(absolute + base_len + 1, location, location_len);
            absolute[base_len + location_len + 1] = '\0';
            return absolute;
        }
        const char *query = strchr(path, '?');
        const char *fragment = strchr(path, '#');
        const char *path_end = base_end;
        if (query && query < path_end) path_end = query;
        if (fragment && fragment < path_end) path_end = fragment;
        const char *slash = path_end;
        while (slash > path && slash[-1] != '/') slash--;
        prefix_len = slash > path ? (size_t)(slash - base_url) : (size_t)(path - base_url) + 1;
    }

    if (prefix_len > CC_HTTP_MAX_REDIRECT_URL - location_len - 1) return NULL;
    char *absolute = malloc(prefix_len + location_len + 1);
    if (!absolute) return NULL;
    memcpy(absolute, base_url, prefix_len);
    memcpy(absolute + prefix_len, location, location_len);
    absolute[prefix_len + location_len] = '\0';
    return absolute;
}

static int http_redirect_drops_body(long status, const char *method)
{
    if (status == 303) return !method || !http_ascii_equal(method, "HEAD");
    return (status == 301 || status == 302) && method && http_ascii_equal(method, "POST");
}

static int http_sensitive_redirect_header(const char *name)
{
    return http_ascii_equal(name, "Authorization") ||
           http_ascii_equal(name, "Proxy-Authorization") ||
           http_ascii_equal(name, "Cookie");
}

static int http_entity_header(const char *name)
{
    return http_ascii_equal(name, "Content-Length") ||
           http_ascii_equal(name, "Content-Type") ||
           http_ascii_equal(name, "Transfer-Encoding");
}

static cc_result_t http_redirect_headers(
    const cc_http_request_t *request,
    int same_origin,
    int drops_body,
    cc_http_header_t **out_headers,
    size_t *out_count)
{
    *out_headers = NULL;
    *out_count = 0;
    if (!request->headers || request->header_count == 0) return cc_result_ok();
    cc_http_header_t *headers = calloc(request->header_count, sizeof(*headers));
    if (!headers) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate redirect headers");
    }
    size_t count = 0;
    for (size_t i = 0; i < request->header_count; i++) {
        const char *name = request->headers[i].name;
        if (!name) continue;
        if ((!same_origin && http_sensitive_redirect_header(name)) ||
            (drops_body && http_entity_header(name))) {
            continue;
        }
        headers[count++] = request->headers[i];
    }
    *out_headers = headers;
    *out_count = count;
    return cc_result_ok();
}

/*
 * Redirect orchestration stays above the single-hop transport so URL and address policy are
 * re-applied before every connection. The original absolute deadline is preserved across hops.
 */
static cc_result_t esp32_http_perform(
    void *self,
    const cc_http_request_t *request,
    cc_http_response_t *out_response)
{
    if (!request || !out_response) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid HTTP redirect request");
    }
    cc_http_request_t hop = *request;
    if (hop.deadline_ms == 0) {
        uint64_t now = (uint64_t)http_now_ms();
        uint64_t budget = request->timeout_ms > 0
            ? (uint64_t)request->timeout_ms : 120000U;
        hop.deadline_ms = budget > UINT64_MAX - now ? UINT64_MAX : now + budget;
    }
    char *owned_url = NULL;
    cc_http_header_t *owned_headers = NULL;
    size_t redirects = 0;

    for (;;) {
        cc_result_t rc = esp32_http_perform_single(self, &hop, out_response);
        if (rc.code != CC_OK || !http_is_redirect_status(out_response->status_code)) {
            free(owned_headers);
            free(owned_url);
            return rc;
        }
        if (request->max_redirects == 0) {
            free(owned_headers);
            free(owned_url);
            return rc;
        }
        if (redirects >= request->max_redirects) {
            cc_http_response_free(out_response);
            free(owned_headers);
            free(owned_url);
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "HTTP redirect limit exceeded");
        }
        const char *location = http_response_header(out_response, "Location");
        char *next_url = http_redirect_url(hop.url, location);
        if (!next_url) {
            cc_http_response_free(out_response);
            free(owned_headers);
            free(owned_url);
            return cc_result_error(CC_ERR_NETWORK, "HTTP redirect has an invalid Location");
        }

        char current_origin[160];
        char next_origin[160];
        http_origin_from_url(hop.url, current_origin, sizeof(current_origin));
        http_origin_from_url(next_url, next_origin, sizeof(next_origin));
        int same_origin = http_ascii_equal(current_origin, next_origin);
        int drops_body = http_redirect_drops_body(out_response->status_code, hop.method);
        cc_http_header_t *next_headers = NULL;
        size_t next_header_count = 0;
        rc = http_redirect_headers(
            &hop, same_origin, drops_body, &next_headers, &next_header_count);
        if (rc.code != CC_OK) {
            cc_http_response_free(out_response);
            free(next_url);
            free(owned_headers);
            free(owned_url);
            return rc;
        }

        cc_http_response_free(out_response);
        free(owned_headers);
        free(owned_url);
        owned_url = next_url;
        owned_headers = next_headers;
        hop.url = owned_url;
        hop.headers = owned_headers;
        hop.header_count = next_header_count;
        hop.header_profile = NULL;
        if (drops_body) {
            hop.method = "GET";
            hop.body = NULL;
        }
        redirects++;
    }
}

/*
 * 重置 ESP32 所有 HTTP 持久连接。
 *
 * 获取互斥锁后清理全局持久 esp_http_client 句柄，下次请求将重新建连（含 TCP/TLS 握手）。
 */
static cc_result_t esp32_http_reset_connections(void *self)
{
    cc_esp32_http_client_state_t *state = (cc_esp32_http_client_state_t *)self;
    SemaphoreHandle_t mutex = http_mutex_get(state);
    if (!mutex) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create HTTP mutex");
    }

    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!http_lock_all_pools_locked(state)) {
        xSemaphoreGive(mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate HTTP pool mutex");
    }
    http_client_cleanup_locked(state, "reset_requested");
    http_unlock_all_pools_locked(state);
    xSemaphoreGive(mutex);
    return cc_result_ok();
}

static void esp32_http_destroy(void *self)
{
    cc_esp32_http_client_state_t *state = (cc_esp32_http_client_state_t *)self;
    if (!state) return;
    cc_result_t rc = esp32_http_reset_connections(state);
    cc_result_free(&rc);
    for (int i = 0; i < CC_HTTP_POOL_COUNT; i++) {
        if (state->pools[i].mutex) vSemaphoreDelete(state->pools[i].mutex);
    }
    if (state->mutex) vSemaphoreDelete(state->mutex);
    free(state);
}

static const cc_http_client_vtable_t s_esp32_http_vtable = {
    .perform = esp32_http_perform,
    .reset_connections = esp32_http_reset_connections,
    .destroy = esp32_http_destroy,
};

cc_result_t cc_http_client_create_default(
    const cc_http_client_options_t *options,
    cc_http_client_t *out_client)
{
    if (!out_client) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null HTTP client output");
    memset(out_client, 0, sizeof(*out_client));
    cc_esp32_http_client_state_t *state = calloc(1, sizeof(*state));
    if (!state) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate HTTP client");
    state->tls_verify = options ? options->tls_verify != 0 : 1;
    state->connect_timeout_ms = options && options->connect_timeout_ms > 0 ?
        options->connect_timeout_ms : CC_HTTP_DEFAULT_CONNECT_TIMEOUT_MS;
    state->first_byte_timeout_ms = options && options->first_byte_timeout_ms > 0 ?
        options->first_byte_timeout_ms : CC_HTTP_DEFAULT_FIRST_BYTE_TIMEOUT_MS;
    state->idle_timeout_ms = options && options->idle_timeout_ms > 0 ?
        options->idle_timeout_ms : CC_HTTP_DEFAULT_IDLE_TIMEOUT_MS;
    state->retry_count = options ? (options->retry_count > 0 ? options->retry_count : 0) :
        CC_HTTP_DEFAULT_RETRY_COUNT;
    state->log_level = options ? options->log_level : CC_HTTP_LOG_WARN;
    state->trace_persist = options ? options->trace_persist != 0 : 0;
    const char *names[CC_HTTP_POOL_COUNT] = {"llm_stream", "llm_sync", "local_api", "default"};
    for (int i = 0; i < CC_HTTP_POOL_COUNT; i++) {
        state->pools[i].owner = state;
        state->pools[i].name = names[i];
    }
    state->mutex = xSemaphoreCreateMutex();
    if (!state->mutex) {
        free(state);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate HTTP mutex");
    }
    esp_log_level_set(TAG, http_esp_log_level(state->log_level));
    out_client->self = state;
    out_client->vtable = &s_esp32_http_vtable;
    out_client->size = sizeof(*out_client);
    out_client->capabilities = CC_HTTP_CAP_STREAM_BODY | CC_HTTP_CAP_CANCEL |
        CC_HTTP_CAP_CONNECTION_POOL | CC_HTTP_CAP_REDIRECT_VALIDATION |
        CC_HTTP_CAP_RESOLVED_ADDRESS_VALIDATION;
    return cc_result_ok();
}

/* 释放 ESP32 HTTP response 的 headers/body。 */
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

#endif
