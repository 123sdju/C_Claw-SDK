#include "cc/ports/cc_http_client.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_platform.h"
#include "cc/util/cc_string_builder.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

/* 严格 POSIX 特性宏可能隐藏 NI_MAXHOST，预留包含区域标识的 IPv6 文本空间。 */
#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif

#if CC_HAS_CURL
#include <curl/curl.h>

/* 全局初始化由进程期锁串行化。客户端销毁不能清理其他请求仍使用的 curl 全局状态。 */
static pthread_mutex_t g_curl_init_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_curl_initialized;

/**
 * @brief 在发布客户端之前初始化进程共享的 curl 状态，失败后允许再次创建时重试。
 * @return 成功返回 CC_OK，锁失败返回 CC_ERR_PLATFORM，curl 初始化失败返回对应错误。
 * @note 成功状态保持到进程退出，不在单个客户端销毁时调用 curl_global_cleanup。
 */
static cc_result_t ensure_curl_initialized(void)
{
    if (pthread_mutex_lock(&g_curl_init_mutex) != 0) {
        return cc_result_error(CC_ERR_PLATFORM, "Failed to lock curl initialization");
    }
    CURLcode code = CURLE_OK;
    if (!g_curl_initialized) {
        code = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (code == CURLE_OK) g_curl_initialized = 1;
    }
    pthread_mutex_unlock(&g_curl_init_mutex);
    return code == CURLE_OK ? cc_result_ok() : cc_result_error(
        code == CURLE_OUT_OF_MEMORY ? CC_ERR_OUT_OF_MEMORY : CC_ERR_PLATFORM,
        "Failed to initialize curl globally");
}
#endif

/** @brief 创建后保持只读的客户端默认值，各次请求分别拥有 easy handle 和响应。 */
typedef struct cc_posix_http_config {
    int tls_verify;             /**< 是否校验证书及主机名。 */
    int connect_timeout_ms;     /**< 连接阶段默认预算，单位毫秒。 */
    int first_byte_timeout_ms;  /**< 首个非空正文片段默认预算，单位毫秒。 */
    int idle_timeout_ms;        /**< 正文片段之间默认空闲预算，单位毫秒。 */
    int retry_count;            /**< 未交付流片段时允许追加的尝试次数。 */
    int log_level;              /**< 保存调用方配置，当前实现未据此输出日志。 */
    int trace_persist;          /**< 保存调用方配置，当前实现未持久化 trace。 */
} cc_posix_http_config_t;

/* 仅供 observability 关联；POSIX 构建使用 GCC/Clang 的原子内建避免并发请求重复编号。 */
static unsigned g_http_attempt_sequence;

/**
 * @brief 为观测事件取得进程内尝试序号，无符号溢出时按 C 规则回绕。
 * @return 当前递增后的序号，GCC/Clang 使用原子递增，其他 curl 构建以全局锁保护，锁失败返回 0。
 */
static unsigned next_http_attempt_id(void)
{
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_add_fetch(&g_http_attempt_sequence, 1u, __ATOMIC_RELAXED);
#elif CC_HAS_CURL
    if (pthread_mutex_lock(&g_curl_init_mutex) != 0) return 0;
    unsigned next = ++g_http_attempt_sequence;
    pthread_mutex_unlock(&g_curl_init_mutex);
    return next;
#else
    return ++g_http_attempt_sequence;
#endif
}

/** @brief 一次同步 easy_perform 期间借用的回调状态，首个错误的拥有权交回 perform。 */
typedef struct cc_curl_write_ctx {
    cc_http_response_t *response;      /**< 当前尝试独占的响应，栈状态只借用该槽。 */
    cc_http_body_callback_fn on_body;  /**< 同步正文回调，可为空。 */
    void *user_data;                   /**< 随正文回调借出的调用方上下文。 */
    size_t max_response_bytes;         /**< 累计正文硬上限，0 表示不限且流式不保存预览。 */
    cc_cancel_token_t *cancel_token;    /**< 调用期借用的取消令牌，可为空。 */
    cc_result_t callback_error;        /**< 首个拥有型错误，退出时交给 perform。 */
    size_t received_bytes;             /**< 当前尝试接收的正文字节总数。 */
    int saw_body;                      /**< 是否收到过非空正文片段。 */
    int saw_stream_body;               /**< 是否向 on_body 交付过非空片段，禁止此后重放。 */
    uint64_t start_ms;                 /**< 整个请求入口的单调毫秒值。 */
    long first_byte_timeout_ms;        /**< 首非空正文预算，毫秒，非正数关闭。 */
    uint64_t deadline_ms;              /**< 全部尝试共享的绝对单调毫秒期限。 */
    uint64_t last_body_ms;             /**< 最近非空正文片段到达的单调毫秒值。 */
    long idle_timeout_ms;              /**< 正文片段间空闲预算，毫秒，非正数关闭。 */
    const cc_http_request_t *request;  /**< perform 调用期有效的请求借用。 */
    char host[256];                    /**< 供实际连接地址校验使用的主机文本副本。 */
    int address_rejected;             /**< 实际连接候选被拒绝或无法转换为数字地址。 */
} cc_curl_write_ctx_t;

/**
 * @brief 从 URL authority 提取用于地址校验的主机文本，去除用户信息、端口和 IPv6 方括号。
 * @param[in] url 借用的 NUL 结尾 URL，NULL 返回失败。
 * @param[out] host 调用方缓冲区，成功时写入 NUL 结尾主机，失败时内容未定义。
 * @param[in] host_size 缓冲区字节数，必须容纳主机及末尾 NUL。
 * @return 成功返回 1，参数无效、主机为空或空间不足返回 0。
 * @note 这里只拆 authority，URL 是否允许由 validate_url 决定。
 */
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

/**
 * @brief 同步解析并逐个校验候选地址，所有解析结果均在返回前释放。
 * @param[in] request 借用请求，NULL 或未提供 validate_address 时直接成功。
 * @param[in] host 需要解析的 NUL 结尾主机，启用校验时必须有效。
 * @return 全部候选通过返回 CC_OK，解析失败返回 CC_ERR_NETWORK，任一拒绝返回 CC_ERR_PERMISSION_DENIED。
 * @note 系统 getaddrinfo 阻塞期间无法被此端口的取消令牌或期限打断，返回后再次检查预算。
 */
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

/**
 * @brief 按请求、客户端、内置值的顺序选择正的阶段预算。
 * @param[in] request_value 请求预算，非正数表示继承，单位毫秒。
 * @param[in] configured_value 客户端预算，非正数表示使用 fallback，单位毫秒。
 * @param[in] fallback 最终默认预算，单位毫秒，可为 0 表示关闭该阶段检查。
 * @return 选中的毫秒值。
 */
static long choose_long(long request_value, int configured_value, long fallback)
{
    if (request_value > 0) return request_value;
    if (configured_value > 0) return configured_value;
    return fallback;
}

/**
 * @brief 选择追加尝试次数，负请求值禁用，零值继承客户端配置。
 * @param[in] request_value 请求重试次数，正数覆盖默认值，0 继承，负数禁用。
 * @param[in] configured_value 客户端重试次数，非正数归一为 0。
 * @return 非负重试次数。
 */
static int choose_int(int request_value, int configured_value)
{
    if (request_value < 0) return 0;
    if (request_value > 0) return request_value;
    return configured_value > 0 ? configured_value : 0;
}

/**
 * @brief 事务式扩容并追加正文二进制片段，另补一个便于文本调用者使用的末尾 NUL。
 * @param[in,out] response 本次请求独占的响应，NULL 或 len 为 0 时直接成功。
 * @param[in] data 借用 len 字节，len 非零时必须可读，无须 NUL 结尾。
 * @param[in] len 正文字节数。
 * @param[in] max_response_bytes 缓存上限，0 表示不设上限。
 * @return 成功返回 CC_OK，大小溢出或超限返回 CC_ERR_LIMIT_EXCEEDED，扩容失败返回 CC_ERR_OUT_OF_MEMORY。
 * @note 失败保留原响应，最终由 perform 统一清理。
 */
static cc_result_t response_append(
    cc_http_response_t *response,
    const char *data,
    size_t len,
    size_t max_response_bytes
)
{
    if (!response || len == 0) return cc_result_ok();
    if (response->body_size == SIZE_MAX || len > SIZE_MAX - response->body_size - 1) {
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

/**
 * @brief 在流式交付后缓存受限的正文预览。
 * @param[in,out] response 本次请求独占的响应，NULL 时直接成功。
 * @param[in] data 借用 len 字节，NULL 时直接成功。
 * @param[in] len 当前片段字节数。
 * @param[in] max_response_bytes 预览容量，0 表示不保存预览。
 * @return 成功或无需追加返回 CC_OK，实际追加失败返回 response_append 的错误。
 * @note 总交付字节上限另由正文回调检查，此函数截取剩余预览空间。
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

/**
 * @brief 复制有限长响应头字段并去掉两端空格、制表符和换行。
 * @param[in] data 借用 len 字节，无须 NUL 结尾。
 * @param[in] len 可读字节数。
 * @return 返回调用方用 free 释放的 NUL 结尾副本，大小溢出或分配失败返回 NULL。
 */
static char *copy_trimmed_header_piece(const char *data, size_t len)
{
    if (len == SIZE_MAX) return NULL;
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

/**
 * @brief 按长度处理状态行或响应头，新状态行开始一个独立响应块。
 * @param[in,out] response 本次请求独占的响应，NULL 时直接成功。
 * @param[in] line 借用 len 字节的完整头行，NULL 时直接成功，无须 NUL 结尾。
 * @param[in] len 行字节数，包含 curl 交付的行结束符。
 * @return 成功或忽略非字段行返回 CC_OK，计数溢出返回 CC_ERR_LIMIT_EXCEEDED，分配失败返回 CC_ERR_OUT_OF_MEMORY。
 * @note 新 HTTP 状态行先清理旧响应块，最终保存最终响应头及其 trailer。
 */
static cc_result_t response_header_append(
    cc_http_response_t *response,
    const char *line,
    size_t len
)
{
    if (!response || !line || len == 0) return cc_result_ok();

    if (len >= 5 && memcmp(line, "HTTP/", 5) == 0) {
        const char *space = memchr(line, ' ', len);
        cc_http_response_free(response);
        if (space && (size_t)(line + len - space) >= 4 &&
            space[1] >= '0' && space[1] <= '9' &&
            space[2] >= '0' && space[2] <= '9' &&
            space[3] >= '0' && space[3] <= '9') {
            response->status_code = (space[1] - '0') * 100L +
                (space[2] - '0') * 10L + space[3] - '0';
        }
        return cc_result_ok();
    }

    const char *colon = memchr(line, ':', len);
    if (!colon) return cc_result_ok();
    if (response->header_count >= SIZE_MAX / sizeof(*response->headers)) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Too many HTTP response headers");
    }

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
/**
 * @brief 检查取消、共享总期限、首片段和正文空闲预算，并保存首个错误。
 * @param[in,out] ctx 当前回调状态，NULL 时视为无检查。
 * @return 需要中止返回 1，其余返回 0。
 * @note 由 curl 回调触发检查，空闲时的响应精度受 curl 调度和同步 DNS 阻塞影响。
 */
static int transfer_should_abort(cc_curl_write_ctx_t *ctx)
{
    if (!ctx) return 0;
    if (ctx->callback_error.code != CC_OK) return 1;
    if (cc_cancel_token_is_cancelled(ctx->cancel_token)) {
        ctx->callback_error = cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled");
        return 1;
    }
    uint64_t now = cc_platform_monotonic_ms();
    if (now >= ctx->deadline_ms ||
        (!ctx->saw_body && ctx->first_byte_timeout_ms > 0 && now >= ctx->start_ms &&
         now - ctx->start_ms >= (uint64_t)ctx->first_byte_timeout_ms) ||
        (ctx->saw_body && ctx->idle_timeout_ms > 0 && now >= ctx->last_body_ms &&
         now - ctx->last_body_ms >= (uint64_t)ctx->idle_timeout_ms)) {
        ctx->callback_error = cc_result_error(CC_ERR_TIMEOUT, "HTTP request budget expired");
        return 1;
    }
    return 0;
}

/**
 * @brief 在当前线程暂时屏蔽 SIGPIPE 后调用 curl，保持进程全局信号处置不变。
 * @param[in,out] curl 本次尝试独占且配置成功的 easy handle。
 * @param[in,out] ctx 本次回调状态，屏蔽、检查或恢复失败时保存首个平台错误。
 * @return curl 的返回码，无法安全进入或退出保护区时返回 CURLE_ABORTED_BY_CALLBACK。
 * @note 原先已屏蔽或已有 pending 的 SIGPIPE 保留。只有原未屏蔽且无旧 pending 时，
 *       消费实际 easy_perform 调用期间新产生的 pending SIGPIPE，再恢复线程原 mask。
 */
static CURLcode perform_with_sigpipe_guard(CURL *curl, cc_curl_write_ctx_t *ctx)
{
    sigset_t blocked, original, pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &blocked, &original) != 0) {
        ctx->callback_error = cc_result_error(CC_ERR_PLATFORM, "Failed to block HTTP SIGPIPE");
        return CURLE_ABORTED_BY_CALLBACK;
    }
    if (sigpending(&pending) != 0) {
        pthread_sigmask(SIG_SETMASK, &original, NULL);
        ctx->callback_error = cc_result_error(CC_ERR_PLATFORM, "Failed to inspect HTTP SIGPIPE");
        return CURLE_ABORTED_BY_CALLBACK;
    }
    int was_blocked = sigismember(&original, SIGPIPE);
    int had_pending = sigismember(&pending, SIGPIPE);
    CURLcode code = curl_easy_perform(curl);
    int signal_error = 0;
    if (!was_blocked && !had_pending) {
        if (sigpending(&pending) != 0) signal_error = 1;
        else if (sigismember(&pending, SIGPIPE)) {
            int received;
            signal_error = sigwait(&blocked, &received) != 0;
        }
    }
    int restored = pthread_sigmask(SIG_SETMASK, &original, NULL) == 0;
    if (signal_error || !restored) {
        if (ctx->callback_error.code == CC_OK) {
            ctx->callback_error = cc_result_error(CC_ERR_PLATFORM, "Failed to restore HTTP SIGPIPE state");
        }
        return CURLE_ABORTED_BY_CALLBACK;
    }
    return code;
}

/**
 * @brief 在 curl 打开套接字前校验实际连接的数字地址。
 * @param[in,out] clientp 借用当前请求回调状态，失败时记录 address_rejected。
 * @param[in] purpose curl 给出的套接字用途，当前实现不按用途区分。
 * @param[in] address curl 提供的地址、地址族和套接字参数，仅本次调用借用。
 * @return 成功返回交由 curl 关闭的套接字，参数无效、策略拒绝或创建失败返回 CURL_SOCKET_BAD。
 */
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

/**
 * @brief 检查预算和累计正文上限，同步交付流片段并按模式缓存正文。
 * @param[in] contents 借用 size*nmemb 字节，无须 NUL 结尾。
 * @param[in] size 单个元素字节数。
 * @param[in] nmemb 元素数，乘法溢出时中止。
 * @param[in,out] userp 当前请求独占的回调状态。
 * @return 接受的字节数，失败返回 0 并保存首个拥有型错误。
 * @note on_body 返回结果的拥有权在调用后移交本层，成功结果也立即释放。
 */
static size_t cc_curl_write_body(void *contents, size_t size, size_t nmemb, void *userp)
{
    cc_curl_write_ctx_t *ctx = (cc_curl_write_ctx_t *)userp;
    if (!ctx) return 0;
    if (transfer_should_abort(ctx)) return 0;
    if (size != 0 && nmemb > SIZE_MAX / size) {
        ctx->callback_error = cc_result_error(
            CC_ERR_LIMIT_EXCEEDED, "HTTP body size overflow");
        return 0;
    }
    size_t real_size = size * nmemb;

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

    if (real_size > 0) {
        ctx->saw_body = 1;
        ctx->last_body_ms = cc_platform_monotonic_ms();
    }

    if (ctx->on_body) {
        if (real_size > 0) ctx->saw_stream_body = 1;
        cc_result_t rc = ctx->on_body((const char *)contents, real_size, ctx->user_data);
        if (rc.code != CC_OK) {
            if (ctx->callback_error.code == CC_OK) ctx->callback_error = rc;
            else cc_result_free(&rc);
            return 0;
        }
        cc_result_free(&rc);
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
    cc_result_free(&rc);

    return real_size;
}

/**
 * @brief 保存有限长状态行和头字段，预算或分配失败时中止传输。
 * @param[in] contents 借用 size*nmemb 字节的头行，无须 NUL 结尾。
 * @param[in] size 元素字节数。
 * @param[in] nmemb 元素数，乘法溢出时中止。
 * @param[in,out] userp 当前请求独占的回调状态。
 * @return 接受的字节数，失败返回 0，首个错误由 perform 取走。
 */
static size_t cc_curl_write_header(void *contents, size_t size, size_t nmemb, void *userp)
{
    cc_curl_write_ctx_t *ctx = (cc_curl_write_ctx_t *)userp;
    if (!ctx) return 0;
    if (transfer_should_abort(ctx)) return 0;
    if (size != 0 && nmemb > SIZE_MAX / size) {
        ctx->callback_error = cc_result_error(
            CC_ERR_LIMIT_EXCEEDED, "HTTP header size overflow");
        return 0;
    }
    size_t real_size = size * nmemb;

    cc_result_t rc = response_header_append(ctx->response, (const char *)contents, real_size);
    if (rc.code != CC_OK) {
        if (ctx->callback_error.code == CC_OK) ctx->callback_error = rc;
        else cc_result_free(&rc);
        return 0;
    }
    cc_result_free(&rc);
    return real_size;
}

/**
 * @brief 在暂无正文回调时继续检查取消和各阶段预算。
 * @param[in,out] clientp 当前请求回调状态。
 * @param[in] dltotal curl 预计下载字节数，当前检查不使用。
 * @param[in] dlnow curl 已下载字节数，当前检查不使用。
 * @param[in] ultotal curl 预计上传字节数，当前检查不使用。
 * @param[in] ulnow curl 已上传字节数，当前检查不使用。
 * @return 需要中止返回 1，继续返回 0。
 */
static int cc_curl_progress(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                            curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;

    return transfer_should_abort((cc_curl_write_ctx_t *)clientp);
}

/**
 * @brief 将一对借用的请求头复制为 curl_slist 节点。
 * @param[in,out] headers 当前请求独占的链表槽，必须非 NULL，失败保留已有节点。
 * @param[in] name NUL 结尾字段名，NULL 时跳过该项。
 * @param[in] value NUL 结尾字段值，NULL 时跳过该项。
 * @return 成功返回 CC_OK，构造或复制失败返回拥有型错误，链表最终由 perform 释放。
 */
static cc_result_t append_header(struct curl_slist **headers, const char *name, const char *value)
{
    if (!name || !value) return cc_result_ok();

    cc_string_builder_t sb;
    cc_result_t rc = cc_string_builder_init(&sb);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);

    rc = cc_string_builder_appendf(&sb, "%s: %s", name, value);
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&sb);
        return rc;
    }
    cc_result_free(&rc);

    char *line = cc_string_builder_take(&sb);
    if (!line) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build HTTP header");
    struct curl_slist *next = curl_slist_append(*headers, line);
    free(line);

    if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to append HTTP header");
    *headers = next;
    return cc_result_ok();
}

/**
 * @brief 将 curl 传输错误映射为 SDK 错误类别。
 * @param[in] res curl 返回码，回调内的具体错误由上层优先处理。
 * @return 对应成功、超时、取消、内存不足或网络错误码。
 */
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

/**
 * @brief 仅对未交付流片段且无本地回调错误的临时网络失败决定追加尝试。
 * @param[in] res 当前 curl 返回码。
 * @param[in] ctx 当前尝试状态，可为 NULL。
 * @param[in] attempt_index 从 0 开始的当前尝试序号。
 * @param[in] retry_count 允许追加的尝试次数。
 * @return 可以重试返回 1，否则返回 0，剩余总预算由 perform 再检查。
 */
static int should_retry(CURLcode res, const cc_curl_write_ctx_t *ctx, int attempt_index, int retry_count)
{
    if (attempt_index >= retry_count) return 0;
    if (ctx && (ctx->callback_error.code != CC_OK || ctx->saw_stream_body || ctx->address_rejected)) return 0;
    return res == CURLE_COULDNT_RESOLVE_HOST ||
           res == CURLE_COULDNT_CONNECT ||
           res == CURLE_SEND_ERROR ||
           res == CURLE_RECV_ERROR ||
           res == CURLE_OPERATION_TIMEDOUT;
}

/**
 * @brief 在本次 easy handle 清理后同步发布一次尝试的观测快照。
 * @param[in] request 借用请求，未配置观测回调时直接返回。
 * @param[in] response 借用当前响应，NULL 时状态码按 0 记录。
 * @param[in] ctx 借用尝试状态，NULL 时正文标记按 0 记录。
 * @param[in] res curl 返回码。
 * @param[in] attempt_index 从 0 开始的尝试序号。
 * @param[in] retry_performed 是否已决定进入下一次尝试。
 * @param[in] attempt_id 进程尝试序号，向公共 int 字段交付时限制在 INT_MAX 内。
 * @param[in] start_ms 本次传输开始的单调毫秒值。
 * @param[in] end_ms 本次传输结束的单调毫秒值。
 * @post 回调已返回，回调不得继续借用栈上的 metrics 或其请求字符串。
 */
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
    metrics.stage = res == CURLE_OK ? "ok" : "perform";
    metrics.err = res == CURLE_OK ? NULL : curl_easy_strerror(res);
    metrics.status = response ? response->status_code : 0;
    metrics.attempt_id = attempt_id > INT_MAX ? INT_MAX : (int)attempt_id;
    metrics.attempt_index = attempt_index;
    metrics.final_attempt = !retry_performed;
    uint64_t elapsed = end_ms >= start_ms ? end_ms - start_ms : 0;
    metrics.total_ms = elapsed > INT_MAX ? INT_MAX : (int)elapsed;
    metrics.connect_ms = 0;
    metrics.header_ms = 0;
    metrics.first_body_ms = 0;
    metrics.saw_body = ctx ? ctx->saw_body : 0;
    metrics.reused_connection = 0;
    metrics.connection_generation = attempt_index < INT_MAX ? attempt_index + 1 : INT_MAX;
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

/**
 * @brief 将本次请求及借用回调状态安装到新建的独占 easy handle。
 * @param[in,out] curl 本次尝试拥有的 easy handle，失败后仍由调用方清理。
 * @param[in] config 创建后只读的默认配置。
 * @param[in] request perform 期间有效的请求，正文按 NUL 结尾字符串交付 curl。
 * @param[in] headers 借用的 curl 头链表，可为 NULL，存活至传输结束。
 * @param[in,out] write_ctx 传输期间有效的栈状态。
 * @param[in] timeout_ms 本次尝试剩余总预算，单位毫秒，必须为正。
 * @param[in] connect_timeout_ms 连接阶段预算，单位毫秒。
 * @return 全部选项安装成功返回 CC_OK，任一选项失败返回拥有型错误，禁止随后执行该 handle。
 * @note 自动重定向关闭，逐请求创建 handle，不提供持久连接池或 header_profile 分流。
 */
static cc_result_t configure_curl_handle(
    CURL *curl,
    const cc_posix_http_config_t *config,
    const cc_http_request_t *request,
    struct curl_slist *headers,
    cc_curl_write_ctx_t *write_ctx,
    long timeout_ms,
    long connect_timeout_ms
)
{
    const char *method = request->method ? request->method : "GET";
    CURLcode code = CURLE_OK;
    /* 保留 curl 的选项参数类型检查，并在首个失败后停止安装选项。 */
#define CC_CURL_SETOPT(option, value) do { \
    if (code == CURLE_OK) code = curl_easy_setopt(curl, option, value); \
} while (0)
    CC_CURL_SETOPT(CURLOPT_URL, request->url);
    CC_CURL_SETOPT(CURLOPT_HTTPHEADER, headers);
    CC_CURL_SETOPT(CURLOPT_TIMEOUT_MS, timeout_ms);
    CC_CURL_SETOPT(CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
    CC_CURL_SETOPT(CURLOPT_NOSIGNAL, 1L);
    CC_CURL_SETOPT(CURLOPT_SSL_VERIFYPEER, config->tls_verify ? 1L : 0L);
    CC_CURL_SETOPT(CURLOPT_SSL_VERIFYHOST, config->tls_verify ? 2L : 0L);
    CC_CURL_SETOPT(CURLOPT_FOLLOWLOCATION, 0L);

    if (strcmp(method, "GET") == 0) {
        CC_CURL_SETOPT(CURLOPT_HTTPGET, 1L);
    } else if (strcmp(method, "POST") == 0) {
        CC_CURL_SETOPT(CURLOPT_POST, 1L);
        CC_CURL_SETOPT(CURLOPT_POSTFIELDS, request->body ? request->body : "");
    } else {
        CC_CURL_SETOPT(CURLOPT_CUSTOMREQUEST, method);
        if (request->body) {
            CC_CURL_SETOPT(CURLOPT_POSTFIELDS, request->body);
        }
    }

    CC_CURL_SETOPT(CURLOPT_WRITEFUNCTION, cc_curl_write_body);
    CC_CURL_SETOPT(CURLOPT_WRITEDATA, write_ctx);
    CC_CURL_SETOPT(CURLOPT_HEADERFUNCTION, cc_curl_write_header);
    CC_CURL_SETOPT(CURLOPT_HEADERDATA, write_ctx);
    CC_CURL_SETOPT(CURLOPT_XFERINFOFUNCTION, cc_curl_progress);
    CC_CURL_SETOPT(CURLOPT_XFERINFODATA, write_ctx);
    CC_CURL_SETOPT(CURLOPT_NOPROGRESS, 0L);
    if (request->validate_address) {
        CC_CURL_SETOPT(CURLOPT_OPENSOCKETFUNCTION, cc_curl_open_socket);
        CC_CURL_SETOPT(CURLOPT_OPENSOCKETDATA, write_ctx);
    }

    /*
     * connection_pool/header_profile 是跨平台 request 契约字段。POSIX 当前使用一次性 easy
     * handle，没有连接池和 header profile 分流，因此字段只参与 metrics，不改变行为。
     */
    (void)request->connection_pool;
    (void)request->header_profile;
#undef CC_CURL_SETOPT
    return code == CURLE_OK ? cc_result_ok() :
        cc_result_error(map_curl_error(code), curl_easy_strerror(code));
}
#endif

/**
 * @brief 同步执行请求，以一个总期限约束 DNS 后的传输和全部追加尝试。
 * @param[in] self 借用创建后只读的客户端配置，必须非 NULL。
 * @param[in] request 借用请求及其字符串、头数组、令牌和回调对象，须覆盖整个调用。
 * @param[out] out_response 必须非 NULL 且不持有待释放资源，入口清零，成功获得头和正文的拥有权。
 * @return 成功返回 CC_OK，包括非 2xx HTTP 状态；错误返回拥有型结果，响应恢复为空。
 * @note 不同调用各自拥有 easy handle 和输出，可并发使用同一客户端。销毁须等待调用及同步回调全部退出。
 */
static cc_result_t posix_http_perform(
    void *self,
    const cc_http_request_t *request,
    cc_http_response_t *out_response
)
{
    cc_posix_http_config_t *config = (cc_posix_http_config_t *)self;
    if (out_response) memset(out_response, 0, sizeof(*out_response));
    if (!config || !request || !request->url || !out_response) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid HTTP request");
    }
    if (request->header_count && !request->headers) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Missing HTTP request headers");
    }
#if !CC_HAS_CURL
    return cc_result_error(CC_ERR_PLATFORM, "No HTTP client adapter available in this build");
#else
    if (cc_cancel_token_is_cancelled(request->cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled");
    }
    uint64_t now_ms = cc_platform_monotonic_ms();
    uint64_t request_start_ms = now_ms;
    uint64_t total_ms = request->timeout_ms > 0 ? (uint64_t)request->timeout_ms : 120000u;
    uint64_t deadline_ms = total_ms > UINT64_MAX - now_ms ? UINT64_MAX : now_ms + total_ms;
    if (request->deadline_ms && request->deadline_ms < deadline_ms) deadline_ms = request->deadline_ms;
    if (now_ms >= deadline_ms) {
        return cc_result_error(CC_ERR_TIMEOUT, "HTTP request deadline expired");
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
        if (cc_cancel_token_is_cancelled(request->cancel_token)) {
            cc_result_free(&address_rc);
            return cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled");
        }
        if (cc_platform_monotonic_ms() >= deadline_ms) {
            cc_result_free(&address_rc);
            return cc_result_error(CC_ERR_TIMEOUT, "HTTP request deadline expired");
        }
        if (address_rc.code != CC_OK) return address_rc;
        cc_result_free(&address_rc);
    } else {
        request_host[0] = '\0';
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
        cc_result_free(&rc);
    }

    cc_result_t final_error = cc_result_ok();
    for (int attempt = 0;;) {
        cc_http_response_free(out_response);
        now_ms = cc_platform_monotonic_ms();
        if (cc_cancel_token_is_cancelled(request->cancel_token)) {
            final_error = cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled");
            break;
        }
        if (now_ms >= deadline_ms) {
            final_error = cc_result_error(CC_ERR_TIMEOUT, "HTTP request deadline expired");
            break;
        }
        uint64_t remaining = deadline_ms - now_ms;
        long timeout_ms = remaining > LONG_MAX ? LONG_MAX : (long)remaining;

        CURL *curl = curl_easy_init();
        if (!curl) {
            final_error = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to initialize curl");
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
        write_ctx.start_ms = request_start_ms;
        write_ctx.first_byte_timeout_ms = first_byte_timeout_ms;
        write_ctx.deadline_ms = deadline_ms;
        write_ctx.idle_timeout_ms = idle_timeout_ms;
        write_ctx.request = request;
        strcpy(write_ctx.host, request_host);
        if (transfer_should_abort(&write_ctx)) {
            curl_easy_cleanup(curl);
            final_error = write_ctx.callback_error;
            break;
        }

        cc_result_t rc = configure_curl_handle(
            curl,
            config,
            request,
            headers,
            &write_ctx,
            timeout_ms,
            connect_timeout_ms);
        if (rc.code != CC_OK) {
            curl_easy_cleanup(curl);
            final_error = rc;
            break;
        }
        cc_result_free(&rc);

        uint64_t attempt_start = cc_platform_monotonic_ms();
        CURLcode res = perform_with_sigpipe_guard(curl, &write_ctx);
        if (transfer_should_abort(&write_ctx)) res = CURLE_ABORTED_BY_CALLBACK;
        uint64_t attempt_end = cc_platform_monotonic_ms();
        CURLcode info_res = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out_response->status_code);
        if (res == CURLE_OK && info_res != CURLE_OK) res = info_res;
        curl_easy_cleanup(curl);

        int retry_performed = should_retry(res, &write_ctx, attempt, retry_count) &&
            !cc_cancel_token_is_cancelled(request->cancel_token) && attempt_end < deadline_ms;
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
            attempt++;
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
            cc_http_response_free(out_response);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate empty HTTP body");
        }
    }

    return cc_result_ok();
#endif
}

/**
 * @brief 完成当前无持久连接池实现的重置调用。
 * @param[in] self 客户端配置，当前实现不读取，可为 NULL。
 * @return 始终返回 CC_OK，每次请求自行销毁 easy handle。
 */
static cc_result_t posix_http_reset_connections(void *self)
{
    (void)self;
    return cc_result_ok();
}

/**
 * @brief 释放客户端拥有的默认配置。
 * @param[in] self 创建函数分配的配置，可为 NULL。
 * @pre 调用方已关闭入口并等待所有 perform、reset 及其回调退出。
 * @post 配置已释放，进程共享的 curl 初始化状态继续有效。
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

/**
 * @brief 复制标量配置并创建 POSIX curl 客户端，成功发布前完成 curl 全局初始化。
 * @param[in] options 借用创建期配置，NULL 使用 TLS 校验开启及默认阶段预算，size 当前不作版本检查。
 * @param[out] out_client 非 NULL 的空客户端槽，入口清零，成功后由调用方调用 cc_http_client_destroy。
 * @return 成功返回 CC_OK，参数无效、分配或全局初始化失败返回错误，失败输出保持为空。
 * @note 未编入 curl 时仍创建端口对象，能力位为 0，perform 基本参数校验后直接返回 CC_ERR_PLATFORM。
 */
cc_result_t cc_http_client_create_default(
    const cc_http_client_options_t *options,
    cc_http_client_t *out_client)
{
    if (!out_client) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null HTTP client output");
    memset(out_client, 0, sizeof(*out_client));
#if CC_HAS_CURL
    cc_result_t init_rc = ensure_curl_initialized();
    if (init_rc.code != CC_OK) return init_rc;
    cc_result_free(&init_rc);
#endif
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
#if CC_HAS_CURL
    out_client->capabilities = CC_HTTP_CAP_STREAM_BODY | CC_HTTP_CAP_CANCEL |
        CC_HTTP_CAP_RESOLVED_ADDRESS_VALIDATION;
#endif
    return cc_result_ok();
}

/**
 * @brief 释放响应拥有的所有头字段、头数组和正文，并清零对象。
 * @param[in,out] response 此端口交付的响应或零初始化对象，NULL 时无操作。
 * @post 所有资源已释放，可重复调用或重新作为空输出槽使用。
 * @pre 调用方保证独占访问，正文或头字段的借用者已经退出。
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
