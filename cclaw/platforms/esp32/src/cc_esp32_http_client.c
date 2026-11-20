#ifdef ESP_PLATFORM
#include "cc/ports/cc_http_client.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_platform.h"

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 响应、完整来源缓存及临时请求字段使用 malloc/free 域。
 * 四个用途池分别保管 IDF handle，池内请求与同步回调串行，重试沿请求/实例配置。 */

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
/* 在短寿命本地服务器的 HTTP 空闲关闭前轮换句柄，TCP keepalive 另由传输层处理。 */
#define CC_HTTP_POOL_IDLE_TTL_MS 4000
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

/** @brief 实例拥有的用途池，mutex 串行保护 handle、完整来源和连接复用状态。 */
typedef struct cc_esp32_http_pool {
    cc_esp32_http_client_state_t *owner;
    esp_http_client_handle_t client;
    SemaphoreHandle_t mutex;
    cc_esp32_http_ctx_t *active_ctx;
    int timeout_ms;
    int healthy;
    unsigned generation;
    char *origin;
    char header_profile[96];
    const char *name;
    int reuse_count;
    int stale_reused_failures;
    int64_t cooldown_until_ms;
    int recover_successes;
    int64_t last_used_ms;
    char method[16];
} cc_esp32_http_pool_t;

/** @brief 仅在当前尝试存活的栈上下文，借用请求与响应，独占尚未交付的 callback_error。 */
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
    /* 保留应用与策略中止原因，后续传输错误不得覆盖。 */
    const char *failure_stage;
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

/** @brief 创建后固定选项的拥有实例，config 锁保护池锁懒建，池锁分别保护传输状态。 */
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

/**
 * @brief 为实例的下一次传输尝试分配诊断序号，计数允许自然回绕。
 * @param[in,out] state 存活实例，非 NULL；GCC/Clang 分支允许不同池同时取号。
 * @return 递增后的无符号序号，仅供观测关联。
 * @note 其他编译器分支由 owner 串行化取号。
 */
static unsigned http_next_attempt_id(cc_esp32_http_client_state_t *state)
{
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_add_fetch(&state->attempt_sequence, 1u, __ATOMIC_RELAXED);
#else
    return ++state->attempt_sequence;
#endif
}

/**
 * @brief 按实例创建时固定的阈值筛选日志。
 * @param[in] state 借用的实例，可为 NULL。
 * @param[in] level 本条日志的 CC_HTTP_LOG_* 级别。
 * @return 实例存在且阈值允许时返回 1，否则返回 0。
 */
static int http_log_enabled(const cc_esp32_http_client_state_t *state, int level)
{
    return state && state->log_level <= level;
}

/**
 * @brief 把端口日志阈值映射到 ESP-IDF 的标签级别。
 * @param[in] level 实例日志阈值，TRACE 与 DEBUG 均映射为 DEBUG。
 * @return 对应 IDF 级别，FATAL 及更高值映射为 NONE。
 */
static esp_log_level_t http_esp_log_level(int level)
{
    if (level <= CC_HTTP_LOG_DEBUG) return ESP_LOG_DEBUG;
    if (level == CC_HTTP_LOG_INFO) return ESP_LOG_INFO;
    if (level == CC_HTTP_LOG_WARN) return ESP_LOG_WARN;
    if (level == CC_HTTP_LOG_ERROR) return ESP_LOG_ERROR;
    return ESP_LOG_NONE;
}

/**
 * @brief 读取本次启动内的 ESP 单调微秒时钟。
 * @return esp_timer_get_time 的微秒值，供期限和阶段计时共用。
 */
static int64_t http_now_us(void)
{
    return esp_timer_get_time();
}

/**
 * @brief 把同一单调时钟换算为完整的六十四位毫秒值。
 * @return 向下取整的毫秒值，用于连接空闲、冷却和绝对期限。
 */
static int64_t http_now_ms(void)
{
    return (int64_t)cc_platform_monotonic_ms();
}

/**
 * @brief 为诊断指标计算毫秒差，并饱和到 int 的上限。
 * @param[in] start_us 起始微秒值，非正值表示尚未观察到。
 * @param[in] end_us 结束微秒值，须不早于起点。
 * @return 有效差值的毫秒整数，过大为 INT_MAX，无效或缺失为 -1。
 */
static int elapsed_ms(int64_t start_us, int64_t end_us)
{
    if (start_us <= 0 || end_us <= 0 || end_us < start_us) {
        return -1;
    }
    int64_t elapsed = (end_us - start_us) / 1000;
    return elapsed > INT_MAX ? INT_MAX : (int)elapsed;
}

/**
 * @brief 选择正的配置值或既有默认值。
 * @param[in] value 首选阶段预算，单位毫秒。
 * @param[in] fallback value 非正时采用的默认值。
 * @return value 为正时返回它，否则返回 fallback。
 */
static int positive_or(int value, int fallback)
{
    return value > 0 ? value : fallback;
}

/**
 * @brief 合并两个可选的阶段预算。
 * @param[in] a 第一项毫秒预算，非正表示缺省。
 * @param[in] b 第二项毫秒预算，非正表示缺省。
 * @return 两项均正时取小值，a 非正时取 b，否则 b 非正时取 a。
 */
static int min_positive(int a, int b)
{
    if (a <= 0) return b;
    if (b <= 0) return a;
    return a < b ? a : b;
}

/**
 * @brief 借用实例创建时已经建立的配置互斥锁。
 * @param[in] state 存活实例，可为 NULL。
 * @return 实例的借用锁句柄，空实例或缺失锁返回 NULL。
 */
static SemaphoreHandle_t http_mutex_get(cc_esp32_http_client_state_t *state)
{
    return state ? state->mutex : NULL;
}

/**
 * @brief 在配置锁保护下懒建一个用途池的互斥锁。
 * @param[in,out] pool 实例内的池，可为 NULL。
 * @return 借用的池锁，空池或分配失败返回 NULL。
 * @pre 调用者持有所属实例的配置锁，后续池操作由返回锁串行化。
 */
static SemaphoreHandle_t http_pool_mutex_get_locked(cc_esp32_http_pool_t *pool)
{
    if (!pool) return NULL;
    if (!pool->mutex) {
        pool->mutex = xSemaphoreCreateMutex();
    }
    return pool->mutex;
}

/**
 * @brief 为连接重置按数组顺序取得所有池锁，失败时归还本次已取得的锁。
 * @param[in,out] state 非 NULL 的存活实例。
 * @return 全部持有时返回 1，任一锁分配失败返回 0。
 * @pre 调用者持有配置锁；池锁使用无限等待，不能从持池锁的回调重入。
 */
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

/**
 * @brief 以逆序归还已经全部取得的池锁。
 * @param[in,out] state 非 NULL 实例，所有已建池锁由本调用者持有。
 * @post 池锁全部归还，配置锁仍由调用者持有。
 */
static void http_unlock_all_pools_locked(cc_esp32_http_client_state_t *state)
{
    for (int i = CC_HTTP_POOL_COUNT - 1; i >= 0; i--) {
        if (state->pools[i].mutex) {
            xSemaphoreGive(state->pools[i].mutex);
        }
    }
}

/**
 * @brief 生成池路由和日志使用的有界来源文本。
 * @param[in] url 调用期间借用的 URL，可为 NULL 或空串。
 * @param[out] out 调用者缓冲，可为 NULL，非空且有容量时先写空串。
 * @param[in] out_size 缓冲字节数，包含 NUL。
 * @post 来源截断至容量减一并补 NUL，完整来源比较另走 http_origins_equal。
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

/**
 * @brief 按 ASCII 大小写折叠比较两个完整字符串。
 * @param[in] left 借用文本，可为 NULL。
 * @param[in] right 借用文本，可为 NULL。
 * @return 两串均非 NULL 且逐字节相等时返回 1，否则返回 0。
 */
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

/**
 * @brief 按完整 scheme 与 authority 字节片段比较两个 URL 的来源。
 * @param[in] left 借用的 URL 或已截取来源，NULL 返回不相同。
 * @param[in] right 借用的 URL 或已截取来源，NULL 返回不相同。
 * @return 两个完整来源忽略 ASCII 大小写相同时返回 1，否则返回 0。
 * @note 默认端口的显式与省略写法仍视为不同来源，避免扩大敏感头保留范围。
 */
static int http_origins_equal(const char *left, const char *right)
{
    if (!left || !right) return 0;
    const char *left_authority = strstr(left, "://");
    const char *right_authority = strstr(right, "://");
    left_authority = left_authority ? left_authority + 3 : left;
    right_authority = right_authority ? right_authority + 3 : right;
    size_t left_length = (size_t)(left_authority - left) + strcspn(left_authority, "/?#");
    size_t right_length = (size_t)(right_authority - right) + strcspn(right_authority, "/?#");
    if (left_length != right_length) return 0;
    for (size_t i = 0; i < left_length; i++) {
        if (tolower((unsigned char)left[i]) != tolower((unsigned char)right[i])) return 0;
    }
    return 1;
}

/**
 * @brief 提取 DNS 与 peer 校验需要的主机，拒绝嵌入凭据的 authority。
 * @param[in] url 非 NULL 的完整 URL，仅接受小写 http/https scheme。
 * @param[out] out_host 主机缓冲，有效参数下先写空串；IPv6 去除方括号。
 * @param[in] out_size 缓冲字节数，须覆盖主机与 NUL。
 * @return 成功为 CC_OK，坏输出/主机为 INVALID_ARGUMENT，不允许的 scheme/user-info 为 PERMISSION_DENIED。
 * @note 只提取主机，端口与完整 URL 的最终解析继续由 IDF 执行。
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

/**
 * @brief 将已解析的 IPv4/IPv6 地址转为数字文本。
 * @param[in] address 非 NULL sockaddr，真实存储须与 sa_family 对应。
 * @param[in] address_len 当前仅为接口配对保留，调用者保证真实结构容量。
 * @param[out] out 数字地址缓冲，失败时内容未保证清零。
 * @param[in] out_size 非零字节容量，交给 inet_ntop 校验。
 * @return 成功为 CC_OK，空参数为 INVALID_ARGUMENT，未知族或转换失败为 NETWORK。
 */
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

/**
 * @brief 提取主机，并在声明策略时逐个核对全部 DNS 候选。
 * @param[in] request 非 NULL 请求，URL 与地址策略在调用期间保持稳定。
 * @param[out] out_host 借用的主机输出缓冲，初始化沿主机提取函数。
 * @param[in] out_host_size 输出字节容量。
 * @return 成功为 CC_OK；任一候选被拒绝为 PERMISSION_DENIED，无可用地址/DNS 失败为 NETWORK。
 * @note DNS 列表本函数释放，策略同步借用主机与数字地址，阻塞解析期限由 lwIP 决定。
 */
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

/**
 * @brief 用真实连接 socket 再核对 peer，避免仅依赖前置 DNS 结果。
 * @param[in] client 当前存活的 IDF handle，仅在声明地址策略时使用。
 * @param[in,out] ctx 本次请求上下文，可为 NULL，无策略时直接成功。
 * @return 策略通过时 CC_OK 并设置 peer_validated，原生查询失败为 NETWORK，策略拒绝为 PERMISSION_DENIED。
 */
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

/**
 * @brief 用既有字符串启发式为本地 API 选择独立连接池。
 * @param[in] origin 有界诊断来源，可为 NULL。
 * @return 匹配列出的私网/回环/.local 子串时为 1，否则为 0。
 * @note 此分类只影响池路由，访问许可沿 URL/DNS/peer 策略处理。
 */
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

/**
 * @brief 查找实例内显式命名的用途池。
 * @param[in,out] state 非 NULL 存活实例。
 * @param[in] name 借用的池名，NULL/空串或未知名表示缺省。
 * @return 匹配池的借用地址，未找到返回 NULL。
 */
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

/**
 * @brief 按显式池、本地来源和流式回调选择固定用途池。
 * @param[in,out] state 非 NULL 存活实例。
 * @param[in] request 借用请求，可为 NULL，显式已知池名优先。
 * @param[in] origin 用于本地路由的来源文本，可为 NULL。
 * @return 实例内的借用池，缺省普通请求使用 llm_sync。
 */
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

/**
 * @brief 取得请求的连接形状标识。
 * @param[in] request 借用请求，可为 NULL。
 * @return 借用 header_profile，未声明时返回静态空串；调用者不得释放。
 */
static const char *request_header_profile(const cc_http_request_t *request)
{
    return request && request->header_profile ? request->header_profile : "";
}

/**
 * @brief 仅在尚未记录时保存本次丢弃连接的原因。
 * @param[in,out] ctx 本次上下文，可为 NULL。
 * @param[in] reason 借用原因，可为 NULL，非空时须覆盖该次指标/日志调用。
 * @post 第一个有效原因保留，后续原因不覆盖。
 */
static void http_pool_record_discard(cc_esp32_http_ctx_t *ctx, const char *reason)
{
    if (ctx && reason && !ctx->connection_discard_reason) {
        ctx->connection_discard_reason = reason;
    }
}

/**
 * @brief 关闭全部已建 handle 并释放各池完整来源缓存。
 * @param[in,out] state 非 NULL 存活实例。
 * @param[in] reason 可空的同步日志原因，不保留指针。
 * @post 已用池的 handle/来源/健康及复用字段归零，池 mutex 和冷却统计继续存活。
 * @pre 重置调用持配置锁及全部池锁；析构调用由 owner 已排空请求和回调。
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
                     pool->origin ? pool->origin : "?",
                     reason ? ", reason=" : "",
                     reason ? reason : "");
        }
        esp_http_client_cleanup(pool->client);
        pool->client = NULL;
        pool->timeout_ms = 0;
        pool->healthy = 0;
        pool->reuse_count = 0;
        pool->last_used_ms = 0;
        free(pool->origin);
        pool->origin = NULL;
        pool->header_profile[0] = '\0';
        pool->method[0] = '\0';
    }
}

/**
 * @brief 关闭一个池的 handle 并释放其完整来源缓存。
 * @param[in,out] pool 已锁定用途池，可为 NULL 或尚未建立 handle。
 * @param[in] reason 可空同步日志原因，调用期间借用。
 * @post 当前连接字段归零，mutex、代号及冷却历史保留供下次请求使用。
 */
static void http_pool_cleanup_locked(cc_esp32_http_pool_t *pool, const char *reason)
{
    if (!pool || !pool->client) return;
    if (http_log_enabled(pool->owner, CC_HTTP_LOG_DEBUG)) {
        ESP_LOGD(TAG, "HTTP persistent handle close pool=%s gen=%u origin=%s%s%s",
                 pool->name ? pool->name : "?",
                 pool->generation,
                 pool->origin ? pool->origin : "?",
                 reason ? ", reason=" : "",
                 reason ? reason : "");
    }
    esp_http_client_cleanup(pool->client);
    pool->client = NULL;
    pool->timeout_ms = 0;
    pool->healthy = 0;
    pool->reuse_count = 0;
    pool->last_used_ms = 0;
    free(pool->origin);
    pool->origin = NULL;
    pool->header_profile[0] = '\0';
    pool->method[0] = '\0';
}

/**
 * @brief 向拥有响应正文追加字节并补 NUL，保留独立 body_size。
 * @param[in,out] response 本次响应，NULL 或零长度沿空操作成功。
 * @param[in] data 长度非零时须指向有效独立输入，不得借用可能被 realloc 搬迁的正文。
 * @param[in] len 输入字节数，可包含 NUL。
 * @param[in] max_response_bytes 正文上限，零表示由 size_t 容量约束。
 * @return 成功为 1，坏输入、容量溢出、超限或 OOM 为 0，失败保留旧正文。
 * @pre 同一响应由当前池锁串行修改。
 */
static int response_append(
    cc_http_response_t *response,
    const char *data,
    size_t len,
    size_t max_response_bytes
)
{
    if (!response || len == 0) return 1;
    if (!data || response->body_size == SIZE_MAX ||
        len > SIZE_MAX - response->body_size - 1 ||
        (max_response_bytes > 0 &&
         (response->body_size > max_response_bytes ||
          len > max_response_bytes - response->body_size))) {
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

/**
 * @brief 为流式请求保留预算内的正文预览前缀。
 * @param[in,out] response 当前响应，可为 NULL。
 * @param[in] data 同步借用片段，可为 NULL。
 * @param[in] len 输入字节数。
 * @param[in] max_response_bytes 预览上限，零表示不保留预览。
 * @return 无需追加或追加成功为 1，底层追加失败为 0，已满时直接成功。
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

/**
 * @brief 深拷贝一个响应头到本次响应拥有的数组。
 * @param[in,out] response 独占响应，可为 NULL。
 * @param[in] name 调用期间借用的头名，可为 NULL，空参数跳过。
 * @param[in] value 调用期间借用的头值，可为 NULL。
 * @return 成功或空参数为 CC_OK，扩容/字段复制失败为 OUT_OF_MEMORY。
 * @note 当前槽两字段完成后才增 count，失败清理当前字段，旧有效项保留；事件入口已限制总数和字节。
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

/**
 * @brief 记录中止原因并主动关闭传输，以弥补 IDF 忽略相关回调返回值的路径。
 * @param[in] evt 借用事件，可为 NULL，非空 client 时同步 close。
 * @param[in,out] ctx 当前上下文，可为 NULL，标记连接不健康并保留丢弃原因。
 * @param[in] reason 可空借用原因，非空时覆盖本次指标与日志。
 * @return 始终 ESP_FAIL；调用者此前已经安装具体 callback_error。
 * @note close 可同步触发 DISCONNECTED，parser 仍可能派发当前缓冲的后续事件，入口继续检查首个错误。
 */
static esp_err_t http_abort_from_event(
    esp_http_client_event_t *evt,
    cc_esp32_http_ctx_t *ctx,
    const char *reason)
{
    if (ctx && reason) {
        ctx->failure_stage = reason;
    }
    http_pool_record_discard(ctx, reason);
    if (ctx && ctx->pool) ctx->pool->healthy = 0;
    if (evt && evt->client) {
        (void)esp_http_client_close(evt->client);
    }
    return ESP_FAIL;
}

/**
 * @brief 把下一次 IDF 阻塞读取限制到阶段预算与本次尝试剩余期限。
 * @param[in] evt 非 NULL 同步事件，client 可为 NULL。
 * @param[in,out] ctx 非 NULL 当前尝试，起点与总预算已经初始化。
 * @param[in] phase_timeout_ms 阶段毫秒预算，非正时只采用总剩余量。
 * @return 有余量为 ESP_OK，期限耗尽安装 TIMEOUT 并关闭传输后返回 ESP_FAIL。
 */
static esp_err_t http_apply_remaining_timeout(esp_http_client_event_t *evt,
                                             cc_esp32_http_ctx_t *ctx,
                                             int phase_timeout_ms)
{
    int64_t remaining_us = (int64_t)ctx->total_timeout_ms * 1000 -
                           (http_now_us() - ctx->start_us);
    if (remaining_us <= 0) {
        if (ctx->callback_error.code == CC_OK)
            ctx->callback_error = cc_result_error(CC_ERR_TIMEOUT, "HTTP request deadline expired");
        return http_abort_from_event(evt, ctx, "total_timeout");
    }
    int remaining_ms = (int)((remaining_us + 999) / 1000);
    int timeout = phase_timeout_ms > 0 && phase_timeout_ms < remaining_ms
        ? phase_timeout_ms : remaining_ms;
    if (evt->client) {
        (void)esp_http_client_set_timeout_ms(evt->client, timeout);
        if (ctx->pool) ctx->pool->timeout_ms = timeout;
    }
    return ESP_OK;
}

/**
 * @brief 同步消费当前池的 IDF 事件，采集头/正文并执行地址、取消及预算检查。
 * @param[in] evt 借用事件，可为 NULL，user_data 指向存活池，data/header 字段只在调用内使用。
 * @return 成功或无活动请求为 ESP_OK，首次错误及后续被抑制事件为 ESP_FAIL。
 * @note perform 持该池锁，active_ctx 指向栈上尝试且覆盖全部回调。用户 body 回调返回结果由端口消费。
 * @note 回调不得重入同池 perform、全池 reset 或销毁。重定向正文计入预算并跳过业务交付。
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    cc_esp32_http_pool_t *pool = evt ? (cc_esp32_http_pool_t *)evt->user_data : NULL;
    cc_esp32_http_ctx_t *ctx = pool ? pool->active_ctx : NULL;
    if (!evt) return ESP_OK;
    if (ctx && ctx->callback_error.code != CC_OK &&
        evt->event_id != HTTP_EVENT_DISCONNECTED) {
        return ESP_FAIL;
    }
    if (ctx && (evt->event_id == HTTP_EVENT_ON_CONNECTED ||
                evt->event_id == HTTP_EVENT_ON_HEADER ||
                evt->event_id == HTTP_EVENT_ON_DATA)) {
        if (cc_cancel_token_is_cancelled(ctx->cancel_token)) {
            ctx->callback_error = cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled");
            return http_abort_from_event(evt, ctx, "request_cancelled");
        }
        int phase_timeout = (ctx->saw_body || evt->event_id == HTTP_EVENT_ON_DATA)
            ? ctx->idle_timeout_ms : ctx->first_byte_timeout_ms;
        if (http_apply_remaining_timeout(evt, ctx, phase_timeout) != ESP_OK) return ESP_FAIL;
    }

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
        }
        if (http_log_enabled(pool ? pool->owner : NULL, CC_HTTP_LOG_DEBUG)) {
            ESP_LOGD(TAG, "HTTP connected pool=%s gen=%u attempt=%u origin=%s elapsed_ms=%d",
                     ctx && ctx->pool ? ctx->pool->name : "?",
                     ctx && ctx->pool ? ctx->pool->generation : 0,
                     ctx ? ctx->attempt_id : 0,
                     ctx && ctx->pool && ctx->pool->origin ? ctx->pool->origin : "?",
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
                     ctx && ctx->pool && ctx->pool->origin ? ctx->pool->origin : "?");
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
        if (ctx->callback_error.code == CC_OK) cc_result_free(&ctx->callback_error);
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

    /* 重定向正文仍计入本跳预算，业务回调与正文缓冲跳过该片段。 */
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
        cc_result_free(&ctx->callback_error);
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

    return http_apply_remaining_timeout(evt, ctx, ctx->idle_timeout_ms);
}

/**
 * @brief 计算请求默认预算与绝对期限共同约束的本次尝试预算。
 * @param[in] state 为现有接口保留，本函数不读取。
 * @param[in] request 可空请求，非正 timeout 使用 120000 毫秒。
 * @return 正预算饱和到 INT_MAX 后按 deadline 再取小，绝对期限已到返回 -1。
 */
static int request_total_timeout_ms(
    const cc_esp32_http_client_state_t *state,
    const cc_http_request_t *request)
{
    (void)state;
    int total = request && request->timeout_ms > 0 ?
        (request->timeout_ms > INT_MAX ? INT_MAX : (int)request->timeout_ms) : 120000;
    if (request && request->deadline_ms > 0) {
        uint64_t now_ms = (uint64_t)http_now_ms();
        if (now_ms >= request->deadline_ms) return -1;
        uint64_t remaining = request->deadline_ms - now_ms;
        if (remaining < (uint64_t)total) total = (int)remaining;
    }
    return total;
}

/**
 * @brief 选择请求覆盖或实例默认的连接预算。
 * @param[in] state 非 NULL 的存活实例，选项创建后固定。
 * @param[in] request 可空请求，正数覆盖默认值。
 * @return 毫秒预算，请求超出 int 范围时饱和至 INT_MAX。
 */
static int request_connect_timeout_ms(
    const cc_esp32_http_client_state_t *state,
    const cc_http_request_t *request)
{
    return request && request->connect_timeout_ms > 0 ?
        (request->connect_timeout_ms > INT_MAX ? INT_MAX : (int)request->connect_timeout_ms) :
        state->connect_timeout_ms;
}

/**
 * @brief 选择请求覆盖或实例默认的首响应阶段预算。
 * @param[in] state 非 NULL 实例。
 * @param[in] request 可空请求，正数覆盖默认值。
 * @return 饱和到 INT_MAX 的毫秒值，覆盖连接之后至首次正文的阶段检查。
 */
static int request_first_byte_timeout_ms(
    const cc_esp32_http_client_state_t *state,
    const cc_http_request_t *request)
{
    return request && request->first_byte_timeout_ms > 0 ?
        (request->first_byte_timeout_ms > INT_MAX ? INT_MAX : (int)request->first_byte_timeout_ms) :
        state->first_byte_timeout_ms;
}

/**
 * @brief 选择正文片段之间的读取空闲预算。
 * @param[in] state 非 NULL 实例。
 * @param[in] request 可空请求，正数覆盖实例默认值。
 * @return 饱和到 INT_MAX 的毫秒值，总剩余期限会进一步收紧实际读取。
 */
static int request_idle_timeout_ms(
    const cc_esp32_http_client_state_t *state,
    const cc_http_request_t *request)
{
    return request && request->idle_timeout_ms > 0 ?
        (request->idle_timeout_ms > INT_MAX ? INT_MAX : (int)request->idle_timeout_ms) :
        state->idle_timeout_ms;
}

/**
 * @brief 选择失败后追加尝试次数。
 * @param[in] state 非 NULL 实例，创建选项可将默认次数设为零。
 * @param[in] request 可空请求，负 retry_count 禁止重试，零继承实例默认，正值显式覆盖。
 * @return 有效追加尝试次数，实际重试还受正文、取消和总期限约束。
 */
static int request_retry_count(
    const cc_esp32_http_client_state_t *state,
    const cc_http_request_t *request)
{
    if (request && request->retry_count < 0) return 0;
    return request && request->retry_count > 0 ? request->retry_count : state->retry_count;
}

/**
 * @brief 选择新连接或复用连接的初始 IDF 阻塞预算。
 * @param[in] state 非 NULL 实例。
 * @param[in] request 借用请求，可为 NULL。
 * @param[in] reusing_connection 非零选择首响应预算，否则选择连接预算。
 * @return 与总预算取小后的毫秒值，调用者同时负责检查绝对期限。
 */
static int request_initial_timeout_ms(
    const cc_esp32_http_client_state_t *state,
    const cc_http_request_t *request,
    int reusing_connection)
{
    int total = request_total_timeout_ms(state, request);
    if (total < 0) return total;
    int connect = positive_or(request_connect_timeout_ms(state, request),
                              CC_HTTP_DEFAULT_CONNECT_TIMEOUT_MS);
    int first_byte = positive_or(request_first_byte_timeout_ms(state, request),
                                 CC_HTTP_DEFAULT_FIRST_BYTE_TIMEOUT_MS);
    int initial = reusing_connection ? first_byte : connect;
    return min_positive(initial, total);
}

/**
 * @brief 按来源、形状、空闲与复用策略保留或重建用途池 handle。
 * @param[in] request 非 NULL 请求，URL 与形状在调用期间稳定。
 * @param[in,out] ctx 本次上下文，须有 owner，记录池与本次诊断。
 * @return 成功为 CC_OK，缺实例为 INVALID_STATE，来源分配失败为 OUT_OF_MEMORY，IDF 初始化失败为 NETWORK。
 * @pre 调用者持所选用途池锁，实例选项固定。
 * @note 完整来源缓存每池含 NUL 最多 2048 B，过长来源只执行当前请求，退出时关闭 handle。
 * 连接代际在一至 INT_MAX 范围循环，仅用于诊断，避免转入公共 int 字段时产生负值。
 */
static cc_result_t http_client_ensure_pool_locked(const cc_http_request_t *request, cc_esp32_http_ctx_t *ctx)
{
    cc_esp32_http_client_state_t *state = ctx ? ctx->owner : NULL;
    if (!state) return cc_result_error(CC_ERR_INVALID_STATE, "HTTP client state is unavailable");
    char origin[160];
    http_origin_from_url(request->url, origin, sizeof(origin));
    cc_esp32_http_pool_t *pool = http_pool_for_request(state, request, origin);
    const char *profile = request_header_profile(request);
    const char *method = request->method ? request->method : "GET";
    int64_t now_ms = http_now_ms();
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
        int64_t idle_ms = now_ms - pool->last_used_ms;
        if (idle_ms < 0) idle_ms = 0;
        if (ctx) ctx->idle_ms_before_reuse = idle_ms > INT_MAX ? INT_MAX : (int)idle_ms;
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
    if (pool->client && !http_origins_equal(pool->origin, request->url)) {
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

    timeout_ms = request_initial_timeout_ms(state, request, 0);
    if (timeout_ms < 0) return cc_result_error(CC_ERR_TIMEOUT, "HTTP request deadline expired");
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

    const char *authority = strstr(request->url, "://");
    authority = authority ? authority + 3 : request->url;
    size_t origin_length = (size_t)(authority - request->url) + strcspn(authority, "/?#");
    char *owned_origin = NULL;
    if (origin_length < CC_HTTP_MAX_REDIRECT_URL) {
        owned_origin = malloc(origin_length + 1);
        if (!owned_origin) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy HTTP origin");
        }
        memcpy(owned_origin, request->url, origin_length);
        owned_origin[origin_length] = '\0';
    }
    pool->client = esp_http_client_init(&config);
    if (!pool->client) {
        free(owned_origin);
        return cc_result_error(CC_ERR_NETWORK, "Failed to initialize esp_http_client");
    }
    pool->timeout_ms = timeout_ms;
    pool->healthy = 0;
    pool->reuse_count = 0;
    pool->last_used_ms = 0;
    pool->origin = owned_origin;
    snprintf(pool->header_profile, sizeof(pool->header_profile), "%s", profile);
    snprintf(pool->method, sizeof(pool->method), "%s", method);
    pool->generation = pool->generation >= (unsigned)INT_MAX
        ? 1U : pool->generation + 1U;
    if (ctx) {
        ctx->reused_connection = 0;
        ctx->connection_generation = pool->generation;
        snprintf(ctx->origin, sizeof(ctx->origin), "%s", origin);
    }
    if (!state->tls_verify && http_log_enabled(state, CC_HTTP_LOG_WARN)) {
        ESP_LOGW(TAG, "HTTP TLS verify disabled for pool=%s origin=%s",
                 pool->name ? pool->name : "?",
                 pool->origin ? pool->origin : "?");
    }
    return cc_result_ok();
}

/**
 * @brief 清除 handle 旧请求头，再安装 URL、Host、默认 User-Agent、方法、当前头与借用正文。
 * @param[in,out] client 已锁定池的存活 IDF handle。
 * @param[in] request 非 NULL 请求，头数组匹配 count，正文 NUL 结尾并覆盖 perform。
 * @return 成功为 CC_OK，临时 Host 分配失败为 OUT_OF_MEMORY，正文超过 IDF int 长度为 LIMIT_EXCEEDED，IDF 设置失败为 NETWORK。
 * @note GET/POST 使用原生方法，其余方法沿既有 POST 加 override 语义；失败可能已修改 handle，由外层清理。
 */
static cc_result_t http_client_apply_request_locked(
    esp_http_client_handle_t client,
    const cc_http_request_t *request
)
{
    esp_err_t err = esp_http_client_delete_all_headers(client);
    if (err != ESP_OK) {
        return cc_result_error(CC_ERR_NETWORK, esp_err_to_name(err));
    }
    err = esp_http_client_set_url(client, request->url);
    if (err != ESP_OK) {
        return cc_result_error(CC_ERR_NETWORK, esp_err_to_name(err));
    }
    const char *authority = strstr(request->url, "://");
    authority = authority ? authority + 3 : request->url;
    size_t authority_length = strcspn(authority, "/?#");
    char *host_header = malloc(authority_length + 1);
    if (!host_header) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy HTTP Host header");
    }
    memcpy(host_header, authority, authority_length);
    host_header[authority_length] = '\0';
    err = esp_http_client_set_header(client, "Host", host_header);
    free(host_header);
    if (err != ESP_OK) {
        return cc_result_error(CC_ERR_NETWORK, esp_err_to_name(err));
    }
    /* 与当前 IDF init 的缺省 User-Agent 保持一致，随后允许 caller 字段覆盖。 */
    err = esp_http_client_set_header(client, "User-Agent", "ESP32 HTTP Client/1.0");
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
     * IDF header 属于 handle，每次已经完整清理并重装 Host，当前数组只交接本次字段。
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
     * 请求显式声明 keep-alive，服务器仍可关闭连接；失败重试沿实例与请求的有效策略。
     */
    err = esp_http_client_set_header(client, "Connection", "keep-alive");
    if (err != ESP_OK) {
        return cc_result_error(CC_ERR_NETWORK, esp_err_to_name(err));
    }

    if (request->body) {
        size_t body_length = strlen(request->body);
        if (body_length > INT_MAX) {
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "HTTP request body exceeds IDF length range");
        }
        err = esp_http_client_set_post_field(client, request->body, (int)body_length);
    } else {
        err = esp_http_client_set_post_field(client, "", 0);
    }
    if (err != ESP_OK) {
        return cc_result_error(CC_ERR_NETWORK, esp_err_to_name(err));
    }

    return cc_result_ok();
}

/**
 * @brief 在所选池锁内执行一次 IDF perform，并收集其同步事件与原生结果。
 * @param[in] request 非 NULL 稳定请求，body、回调及 token 借用覆盖本次调用。
 * @param[in,out] ctx 非 NULL 已清零上下文，owner/response/回调已设置，函数初始化阶段时点与预算。
 * @param[in,out] out_response 非 NULL 当前响应，事件可能形成拥有的部分头/正文，最终清理由外层执行。
 * @param[out] out_err 可为 NULL，执行到 perform 收尾时写原生错误，前置失败时保留原槽值。
 * @return 配置/取消/前置期限失败返回相应错误，IDF perform 的实际结果由 out_err 和 ctx.callback_error 交付。
 * @note active_ctx 只覆盖同步 perform，返回前全部直接事件回调已退出，之后清除栈借用。
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
    if (cc_cancel_token_is_cancelled(request->cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled before attempt");
    }
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
    if (ctx->callback_error.code == CC_OK &&
        http_now_us() - ctx->start_us >= (int64_t)ctx->total_timeout_ms * 1000) {
        ctx->callback_error = cc_result_error(CC_ERR_TIMEOUT, "HTTP request deadline expired");
        ctx->failure_stage = "total_timeout";
        err = ESP_ERR_TIMEOUT;
    }
    if (ctx->finish_us == 0) {
        ctx->finish_us = http_now_us();
    }

    /* 复用 handle 未收到本次响应时，其状态码仍可能属于前一次响应。 */
    out_response->status_code = (ctx->first_header_us || ctx->saw_body || err == ESP_OK)
        ? esp_http_client_get_status_code(client) : 0;
    if (err == ESP_ERR_HTTP_READ_TIMEOUT && !ctx->failure_stage) {
        ctx->failure_stage = ctx->first_header_us == 0 ? "header_timeout" :
            ctx->first_body_us == 0 ? "first_body_timeout" :
            ctx->stream_request ? "stream_idle_timeout" : "body_timeout";
    }
    if (err != ESP_OK && !ctx->failure_stage &&
        err != ESP_ERR_TIMEOUT && err != ESP_ERR_HTTP_EAGAIN) {
        ctx->failure_stage = ctx->connected_us == 0 ? "connect_error" :
            ctx->first_header_us == 0 ? "header_io_error" : "body_io_error";
    }
    if (err == ESP_OK && !esp_http_client_is_complete_data_received(client)) {
        err = ESP_ERR_HTTP_EAGAIN;
        ctx->failure_stage = ctx->stream_request ? "stream_incomplete" : "incomplete_data";
        http_pool_record_discard(ctx, ctx->stream_request ? "stream_incomplete" : "incomplete_data");
        if (ctx->stream_request && ctx->saw_body) ctx->partial_stream = 1;
    }
    if (out_err) {
        *out_err = err;
    }

    return cc_result_ok();
}

/**
 * @brief 优先采用明确中止原因，否则按已观察到的阶段生成诊断。
 * @param[in] ctx 借用尝试上下文，可为 NULL。
 * @return 借用原因或静态阶段字符串，覆盖调用方生成指标/错误的使用期。
 */
static const char *http_failure_stage(const cc_esp32_http_ctx_t *ctx)
{
    if (ctx && ctx->failure_stage && ctx->failure_stage[0]) {
        return ctx->failure_stage;
    }
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

/**
 * @brief 为一次尝试至多同步发布一条借用指标。
 * @param[in,out] ctx 当前上下文，可为 NULL，回调缺失或已发布时跳过。
 * @param[in] err 本次原生错误，ESP_OK 对应 ok 阶段。
 * @param[in] status_code 已确认的当前响应码。
 * @param[in] final_attempt 非零表示本请求在此停止尝试。
 * @param[in] total_ms 总耗时，毫秒，缺失为 -1。
 * @param[in] connect_ms 连接阶段耗时，毫秒。
 * @param[in] header_ms 首头耗时，毫秒。
 * @param[in] first_body_ms 首正文耗时，毫秒。
 * @post 指标与字符串只在同步回调内有效，发布前置 metrics_emitted 防重复。
 * @note 仍持当前池锁，回调不得重入同池请求、reset 或销毁。
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

/**
 * @brief 计算尝试耗时、发布指标并按实例阈值记录成功或失败摘要。
 * @param[in,out] ctx 当前上下文，可为 NULL，指标发布状态可能更新。
 * @param[in] err 原生结果。
 * @param[in] status_code 本次响应码。
 * @param[in] final_attempt 非零表示最后尝试，失败最终摘要可按 WARN 输出。
 * @post 临时耗时只在本次使用，回调同步结束后返回，调用者仍持池锁。
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

/**
 * @brief 将传输失败阶段与原生名称构造为拥有诊断。
 * @param[in] ctx 借用上下文，可为 NULL。
 * @param[in] err 原生错误码。
 * @return 原生超时码或阶段包含 timeout 时为 TIMEOUT，否则为 NETWORK；结果动态字段由接收者释放。
 */
static cc_result_t http_error_result(const cc_esp32_http_ctx_t *ctx, esp_err_t err)
{
    const char *stage = http_failure_stage(ctx);
    cc_error_code_t code =
        err == ESP_ERR_TIMEOUT || err == ESP_ERR_HTTP_READ_TIMEOUT ||
        strstr(stage, "timeout") != NULL ? CC_ERR_TIMEOUT : CC_ERR_NETWORK;
    return cc_result_errf(code, "HTTP %s: %s", stage, esp_err_to_name(err));
}

/**
 * @brief 记录失败连接的轮换原因与陈旧复用冷却统计，并释放 handle。
 * @param[in,out] ctx 当前上下文，可为 NULL，pool 缺失时跳过。
 * @param[in] err ESP_OK 时跳过，其他错误进入失败统计。
 * @post 陈旧复用失败按阈值饱和，达到阈值后进入冷却，完整来源与 handle 已清理，首个丢弃原因保留。
 * @pre 调用者持当前池锁。
 */
static void http_pool_note_failure_locked(cc_esp32_http_ctx_t *ctx, esp_err_t err)
{
    if (!ctx || !ctx->pool || err == ESP_OK) return;
    cc_esp32_http_pool_t *pool = ctx->pool;
    const char *reason = ctx->connection_discard_reason;
    if (!reason) {
        if (ctx->reused_connection && ctx->first_header_us == 0 && !ctx->saw_body) {
            reason = "stale_reused_connection";
        } else if (ctx->stream_request && ctx->saw_body) {
            reason = "stream_incomplete";
        } else if (ctx->disconnected) {
            reason = "peer_closed";
        } else {
            reason = "transport_failure";
        }
        http_pool_record_discard(ctx, reason);
    }

    if (ctx->reused_connection && ctx->first_header_us == 0 && !ctx->saw_body) {
        if (pool->stale_reused_failures < CC_HTTP_POOL_STALE_THRESHOLD) {
            pool->stale_reused_failures++;
        }
        if (pool->stale_reused_failures >= CC_HTTP_POOL_STALE_THRESHOLD) {
            pool->cooldown_until_ms = http_now_ms() + CC_HTTP_POOL_COOLDOWN_MS;
            pool->recover_successes = 0;
        }
    }
    http_pool_cleanup_locked(pool, reason);
}

/**
 * @brief 更新成功连接的空闲起点、复用数与冷却恢复统计。
 * @param[in,out] ctx 本次上下文，可为 NULL 或无池。
 * @post 对端已关闭的连接保持不健康，新连接成功计数按阈值饱和，达到阈值后清除冷却。
 * @pre 调用者持当前池锁。
 */
static void http_pool_note_success_locked(cc_esp32_http_ctx_t *ctx)
{
    if (!ctx || !ctx->pool) return;
    cc_esp32_http_pool_t *pool = ctx->pool;
    pool->healthy = ctx->disconnected ? 0 : 1;
    pool->last_used_ms = http_now_ms();
    pool->reuse_count++;
    if (!ctx->reused_connection) {
        if (pool->recover_successes < CC_HTTP_POOL_RECOVER_SUCCESS) {
            pool->recover_successes++;
        }
        if (pool->recover_successes >= CC_HTTP_POOL_RECOVER_SUCCESS) {
            pool->stale_reused_failures = 0;
            pool->cooldown_until_ms = 0;
        }
    } else {
        pool->stale_reused_failures = 0;
    }
}

/**
 * @brief 对一个 URL 执行带策略检查、池内串行和受限重试的完整传输。
 * @param[in,out] self 非 NULL 实例，owner 保持至请求及全部同步回调退出。
 * @param[in] request 非 NULL 稳定请求，URL 必填，非零头数量须有数组。
 * @param[out] out_response 非 NULL 的新/已清理响应，有效参数后先清零，返回后按拥有字段清理。
 * @return 成功为 CC_OK；策略、取消、期限、传输、分配及回调失败返回对应拥有结果。
 * @note config→pool 顺序取得锁后释放 config，请求、重试退避与回调均持池锁。
 * @note 同池串行，其他已取得池锁的请求可并行。DNS 与锁等待有原生阻塞边界，取得后再核对绝对期限。
 * @note 只有未观察到正文且有效重试策略允许时才重复传输，服务端副作用仍可能已经发生。
 */
static cc_result_t esp32_http_perform_single(
    void *self,
    const cc_http_request_t *request,
    cc_http_response_t *out_response
)
{
    cc_esp32_http_client_state_t *state = (cc_esp32_http_client_state_t *)self;
    if (!state || !request || !request->url || !out_response ||
        (request->header_count > 0 && !request->headers)) {
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
    unsigned max_attempts = (unsigned)(retry_count > 0 ? retry_count : 0) + 1U;

    for (unsigned attempt = 0; attempt < max_attempts; attempt++) {
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
        ctx.attempt_index = (int)attempt;
        ctx.validate_address = request->validate_address;
        ctx.validate_address_user_data = request->validate_address_user_data;
        snprintf(ctx.request_host, sizeof(ctx.request_host), "%s", request_host);

        err = ESP_OK;
        rc = http_perform_once_locked(request, &ctx, out_response, &err);
        if (rc.code != CC_OK || ctx.callback_error.code != CC_OK || err == ESP_OK) {
            break;
        }

        int can_retry = attempt + 1 < max_attempts && !ctx.saw_body &&
            !cc_cancel_token_is_cancelled(request->cancel_token) &&
            request_total_timeout_ms(state, request) >= 0;
        ctx.retry_allowed = can_retry;
        if (!can_retry) {
            break;
        }

        cc_result_free(&ctx.callback_error);
        ctx.retry_performed = 1;
        http_log_attempt_summary(&ctx, err, out_response->status_code, 0);
        cc_http_response_free(out_response);
        memset(out_response, 0, sizeof(*out_response));
        http_pool_note_failure_locked(&ctx, err);

        if (http_log_enabled(state, CC_HTTP_LOG_DEBUG)) {
            ESP_LOGD(TAG, "HTTP retry scheduled after %s attempt=%u next_index=%u",
                     http_failure_stage(&ctx),
                     ctx.attempt_id,
                     attempt + 2);
        }
        vTaskDelay(pdMS_TO_TICKS(attempt == 0 ? 250 : 750));
    }

    /* 已观察到正文即停止重试，保留上层已经接收的输出与副作用边界。 */
    if (ctx.callback_error.code != CC_OK) {
        if (ctx.stream_request && ctx.saw_body) ctx.partial_stream = 1;
        http_pool_record_discard(&ctx, "callback_error");
        http_pool_note_failure_locked(&ctx, ESP_FAIL);
        http_log_attempt_summary(&ctx, ESP_FAIL, out_response->status_code, 1);
        cc_http_response_free(out_response);
        cc_result_free(&rc);
        rc = ctx.callback_error;
        memset(&ctx.callback_error, 0, sizeof(ctx.callback_error));
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

    cc_result_free(&ctx.callback_error);
    if (pool->client && !pool->origin) http_pool_cleanup_locked(pool, "origin_not_cached");
    xSemaphoreGive(pool_mutex);
    return rc;
}

/**
 * @brief 判断端口显式支持的五种重定向响应码。
 * @param[in] status HTTP 响应码。
 * @return 301/302/303/307/308 返回 1，其余返回 0。
 */
static int http_is_redirect_status(long status)
{
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

/**
 * @brief 查找响应中首个忽略 ASCII 大小写匹配的头值。
 * @param[in] response 稳定响应，可为 NULL，数组须与 count 匹配。
 * @param[in] name 借用头名，可为 NULL。
 * @return 借用头值，未找到或空输入返回 NULL，寿命至响应清理。
 */
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

/**
 * @brief 把 Location 的绝对、协议相对或路径相对形式组合为下一跳 URL。
 * @param[in] base_url 稳定基准 URL，须有 scheme，NULL 失败。
 * @param[in] location 稳定响应头，NULL/空串失败，裁去首尾空白及 CR/LF 后内容。
 * @return free 可释放的完整字符串，长度超过既有 2048 B 界限、输入无效或 OOM 返回 NULL。
 * @note 点路径继续交给 IDF URL 解析，返回值由调用方重新执行 URL 和地址策略。
 */
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
    const char *path = authority + strcspn(authority, "/?#");
    const char *base_end = base_url + strlen(base_url);
    size_t prefix_len = 0;

    if (location_len >= 2 && location[0] == '/' && location[1] == '/') {
        prefix_len = (size_t)(scheme_end - base_url) + 1;
    } else if (location[0] == '/') {
        prefix_len = (size_t)(path - base_url);
    } else if (location[0] == '?') {
        prefix_len = strcspn(base_url, "?#");
    } else if (location[0] == '#') {
        prefix_len = strcspn(base_url, "#");
    } else {
        if (path == base_end || *path != '/') {
            size_t base_len = (size_t)(path - base_url);
            if (location_len > CC_HTTP_MAX_REDIRECT_URL - 2U) return NULL;
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

/**
 * @brief 按既有重定向语义决定是否切换 GET 并移除正文。
 * @param[in] status 重定向响应码。
 * @param[in] method 可空原方法，比较忽略 ASCII 大小写。
 * @return 303 且非 HEAD，或 301/302 的 POST 返回 1，其他返回 0。
 */
static int http_redirect_drops_body(long status, const char *method)
{
    if (status == 303) return !method || !http_ascii_equal(method, "HEAD");
    return (status == 301 || status == 302) && method && http_ascii_equal(method, "POST");
}

/**
 * @brief 判断跨来源时需要移除的三类凭据头。
 * @param[in] name 借用头名，可为 NULL。
 * @return Authorization、Proxy-Authorization、Cookie 返回 1，其他为 0。
 */
static int http_sensitive_redirect_header(const char *name)
{
    return http_ascii_equal(name, "Authorization") ||
           http_ascii_equal(name, "Proxy-Authorization") ||
           http_ascii_equal(name, "Cookie");
}

/**
 * @brief 判断丢弃正文后需要同步移除的实体头。
 * @param[in] name 借用头名，可为 NULL。
 * @return Content-Length、Content-Type、Transfer-Encoding 返回 1，其他为 0。
 */
static int http_entity_header(const char *name)
{
    return http_ascii_equal(name, "Content-Length") ||
           http_ascii_equal(name, "Content-Type") ||
           http_ascii_equal(name, "Transfer-Encoding");
}

/**
 * @brief 为下一跳分配头数组并筛掉跨来源凭据或已无正文的实体字段。
 * @param[in] request 非 NULL 当前跳请求，头数组与 count 匹配。
 * @param[in] same_origin 非零保留凭据字段。
 * @param[in] drops_body 非零移除实体头。
 * @param[out] out_headers 非 NULL 输出，先置 NULL，成功交出仅数组拥有的浅拷贝。
 * @param[out] out_count 非 NULL 输出，先置零，成功填写保留项数。
 * @return 成功为 CC_OK，分配失败为 OUT_OF_MEMORY，头字符串仍借自原始请求。
 */
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

/**
 * @brief 在统一绝对期限内逐跳传输并执行重定向过滤。
 * @param[in,out] self 存活实例，传给单跳实现，owner 排空全部调用后销毁。
 * @param[in] request 非 NULL 请求，字符串、数组、token 和回调覆盖完整重定向过程。
 * @param[out] out_response 非 NULL 新/已清理响应，最终或停止跟随后交付拥有字段，错误也须清理。
 * @return 传输结果或重定向限额/Location/分配错误，动态诊断交给接收者释放。
 * @note max_redirects 为零时返回首跳响应；跨来源剥离凭据，每跳重查 URL/DNS/peer，临时 URL/数组统一释放。
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
    uint64_t now = (uint64_t)http_now_ms();
    uint64_t budget = request->timeout_ms > 0
        ? (uint64_t)request->timeout_ms : 120000U;
    uint64_t timeout_deadline = budget > UINT64_MAX - now ? UINT64_MAX : now + budget;
    if (hop.deadline_ms == 0 || timeout_deadline < hop.deadline_ms)
        hop.deadline_ms = timeout_deadline;
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

        int same_origin = http_origins_equal(hop.url, next_url);
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

/**
 * @brief 等待当前池请求退出并关闭全部持久连接，保留实例与锁供后续使用。
 * @param[in,out] self 存活实例，调用期间由 owner 保持。
 * @return 成功为 CC_OK，缺配置锁或懒建池锁失败为 OUT_OF_MEMORY，已取得池锁在失败时归还。
 * @note config→按数组池锁无限等待，不能从本实例持池锁的同步回调重入。
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

/**
 * @brief 在 owner 排空后无分配地释放全部 handle、来源缓存、已建锁和实例。
 * @param[in,out] self 唯一拥有实例，可为 NULL。
 * @post 非空实例与其全部借用指针失效，vtable 包装由上层统一清空。
 * @pre owner 已关闭入口，借助 builder/generation 或等价屏障排空请求、reset 与全部回调。
 */
static void esp32_http_destroy(void *self)
{
    cc_esp32_http_client_state_t *state = (cc_esp32_http_client_state_t *)self;
    if (!state) return;
    http_client_cleanup_locked(state, "destroy");
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

/**
 * @brief 创建独立 ESP32 HTTP 实例及配置锁，池与连接按请求懒建。
 * @param[in] options 可空配置，仅调用期间借用并复制标量，NULL 采用安全默认 TLS 校验。
 * @param[out] out_client 非 NULL 新/已销毁包装，先清零，成功接管实例并公布能力位。
 * @return 成功为 CC_OK，空输出为 INVALID_ARGUMENT，实例或锁分配失败为 OUT_OF_MEMORY，失败输出保持空。
 * @note 各实例状态独立，ESP 日志标签阈值是 IDF 标签级设置；trace_persist 当前仅保存配置，无持久化实现。
 */
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

/**
 * @brief 消费响应拥有的头数组、各头字段和正文并清零结构。
 * @param[in,out] response 可空响应，其 count/数组与正文须为本端口交付的独占拥有状态。
 * @post 动态字段全部释放，调用者的结构存储继续有效，清理前结束全部字段借用。
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

#endif
