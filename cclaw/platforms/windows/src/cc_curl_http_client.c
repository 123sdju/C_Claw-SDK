#include "cc/ports/cc_http_client.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_platform.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/util/cc_string_builder.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#if CC_HAS_CURL
#include <curl/curl.h>

/* 进程期锁保护 curl 全局状态，首次客户端发布前也完成 Winsock 初始化。 */
static SRWLOCK g_curl_init_lock = SRWLOCK_INIT;
static int g_curl_initialized;

/**
 * @brief 串行初始化进程共享的 curl 和 Winsock 状态，失败时允许后续创建重试。
 * @return 成功返回 CC_OK，curl 初始化失败返回内存不足或平台错误。
 * @note 单个客户端销毁不执行全局清理，避免影响其他客户端或在途请求。
 */
static cc_result_t ensure_curl_initialized(void)
{
    AcquireSRWLockExclusive(&g_curl_init_lock);
    CURLcode code = CURLE_OK;
    if (!g_curl_initialized) {
        code = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (code == CURLE_OK) g_curl_initialized = 1;
    }
    ReleaseSRWLockExclusive(&g_curl_init_lock);
    return code == CURLE_OK ? cc_result_ok() : cc_result_error(
        code == CURLE_OUT_OF_MEMORY ? CC_ERR_OUT_OF_MEMORY : CC_ERR_PLATFORM,
        "Failed to initialize curl globally");
}
#endif

/** @brief 创建时复制、之后只读的客户端配置，各请求分别拥有传输资源。 */
typedef struct cc_windows_http_client {
    cc_http_client_options_t options; /**< 标量默认值副本，size 当前不作版本检查。 */
} cc_windows_http_client_t;

static volatile LONG g_http_attempt_sequence;

/**
 * @brief 原子生成仅供观测关联的尝试序号。
 * @return InterlockedIncrement 的结果按无符号值交付，达到原生整数边界时回绕。
 */
static unsigned next_http_attempt_id(void)
{
    return (unsigned)InterlockedIncrement(&g_http_attempt_sequence);
}

/** @brief 单次传输借用的回调状态，首个回调错误由 perform 接管。 */
typedef struct cc_curl_write_ctx {
    cc_http_response_t *response;      /**< 当前尝试响应槽的借用。 */
    cc_http_body_callback_fn on_body;  /**< 同步正文回调，可为空。 */
    void *user_data;                   /**< 交给正文回调的调用方上下文。 */
    size_t max_response_bytes;         /**< 总正文上限，0 表示不限且流式不缓存预览。 */
    size_t received_bytes;             /**< 当前尝试累计接收的正文字节数。 */
    cc_cancel_token_t *cancel_token;    /**< 调用期间借用的取消令牌，可为空。 */
    cc_result_t callback_error;        /**< 本层拥有的首个错误，退出后交给 perform。 */
    int saw_body;                      /**< 已收到非空正文片段。 */
    int saw_stream_body;               /**< 已交付非空流片段，禁止后续重试。 */
    uint64_t start_ms;                 /**< 整个请求入口的单调毫秒值。 */
    long first_byte_timeout_ms;        /**< 首非空正文预算，毫秒，非正数关闭。 */
    uint64_t deadline_ms;              /**< 所有尝试共享的单调毫秒期限。 */
    uint64_t last_body_ms;             /**< 最近非空片段的到达毫秒值。 */
    long idle_timeout_ms;              /**< 正文片段间空闲预算，毫秒，非正数关闭。 */
    const cc_http_request_t *request;  /**< 覆盖整个传输期的请求借用。 */
    char host[256];                    /**< 实际连接地址校验所需的主机副本。 */
    int address_rejected;             /**< 实际候选被拒绝或数字地址转换失败。 */
} cc_curl_write_ctx_t;

/**
 * @brief 从 URL authority 提取校验主机，去掉用户信息、端口和 IPv6 方括号。
 * @param[in] url 借用的 NUL 结尾 URL，NULL 返回失败。
 * @param[out] host 调用方缓冲区，成功时写 NUL 结尾主机，失败内容未定义。
 * @param[in] host_size 缓冲区容量，单位字节，须包含末尾 NUL。
 * @return 成功返回 1，主机为空、参数无效或空间不足返回 0。
 * @note 本函数只拆 authority，URL 策略由 validate_url 决定。
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
 * @brief 通过 Winsock 同步解析主机并逐个校验候选数字地址。
 * @param[in] request 借用请求，NULL 或未安装地址校验器时直接成功。
 * @param[in] host 借用的 NUL 结尾主机，启用校验时必须有效。
 * @return 全部地址允许返回 CC_OK，解析失败返回 CC_ERR_NETWORK，任一拒绝返回 CC_ERR_PERMISSION_DENIED。
 * @note 所有解析节点在返回前释放，阻塞解析无法被令牌打断，返回后再次核验总期限。
 */
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

/**
 * @brief 事务式扩容并追加二进制正文，另补一个末尾 NUL。
 * @param[in,out] response 独占响应，NULL 或 len 为 0 时直接成功。
 * @param[in] data 借用 len 字节，len 非零时必须可读，无须 NUL 结尾。
 * @param[in] len 片段字节数。
 * @param[in] max_response_bytes 缓存上限，0 表示不设上限。
 * @return 成功返回 CC_OK，大小超限或溢出返回 CC_ERR_LIMIT_EXCEEDED，扩容失败返回 CC_ERR_OUT_OF_MEMORY。
 * @note 失败保留原响应供 perform 统一清理。
 */
static cc_result_t response_append(
    cc_http_response_t *response,
    const char *data,
    size_t len,
    size_t max_response_bytes
)
{
    if (!response || len == 0) return cc_result_ok();
    if (response->body_size == SIZE_MAX || len > SIZE_MAX - response->body_size - 1U) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "HTTP response body is too large");
    }
    if (max_response_bytes > 0 && response->body_size + len > max_response_bytes) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "HTTP response body exceeded limit");
    }

    char *next = realloc(response->body, response->body_size + len + 1);
    if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow HTTP response body");

    response->body = next;
    memcpy(response->body + response->body_size, data, len);
    response->body_size += len;
    response->body[response->body_size] = '\0';
    return cc_result_ok();
}

/**
 * @brief 流式交付后保存指定容量的正文预览。
 * @param[in,out] response 独占响应，NULL 时直接成功。
 * @param[in] data 借用片段，NULL 时直接成功。
 * @param[in] len 片段字节数。
 * @param[in] max_response_bytes 预览容量，0 表示不保存预览。
 * @return 无需追加或追加成功返回 CC_OK，实际扩容失败返回 response_append 的错误。
 * @note 正文回调另行执行总交付字节上限检查。
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
 * @brief 复制有限长头字段并裁掉两端空格、制表符与行结束符。
 * @param[in] data 借用 len 字节，无须 NUL 结尾。
 * @param[in] len 可读字节数。
 * @return 返回需用 free 释放的 NUL 结尾副本，大小溢出或分配失败返回 NULL。
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
 * @brief 处理有限长状态行与头字段，以状态行清理上一个响应块。
 * @param[in,out] response 本次请求独占响应，NULL 时直接成功。
 * @param[in] line 借用完整头行，NULL 时直接成功，无须 NUL 结尾。
 * @param[in] len 行字节数，包含行结束符。
 * @return 成功或忽略非字段行返回 CC_OK，计数溢出返回 CC_ERR_LIMIT_EXCEEDED，分配失败返回 CC_ERR_OUT_OF_MEMORY。
 * @note 最终响应头和随后 trailer 由响应对象持有，交给 cc_http_response_free 释放。
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
 * @brief 检查取消、总期限、首正文片段和片段间空闲预算，保存首个错误。
 * @param[in,out] ctx 当前尝试状态，NULL 时不执行检查。
 * @return 需要中止返回 1，否则返回 0。
 * @note 检查依赖 curl 回调调度，系统同步解析期间无法强制打断。
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
 * @brief 校验实际连接的数字地址，再创建由 curl 接管的 Winsock 套接字。
 * @param[in,out] clientp 借用当前请求状态，策略拒绝时设置 address_rejected。
 * @param[in] purpose curl 的套接字用途，当前不据此分流。
 * @param[in] address 借用 curl 地址族、类型和地址，仅本次回调有效。
 * @return 成功返回归 curl 关闭的套接字，参数无效、拒绝或创建失败返回 CURL_SOCKET_BAD。
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

/**
 * @brief 检查预算与累计正文上限，同步交付流片段并缓存正文或预览。
 * @param[in] contents 借用 size*nmemb 字节，无须 NUL 结尾。
 * @param[in] size 元素字节数。
 * @param[in] nmemb 元素数，乘法溢出时中止。
 * @param[in,out] userp 当前请求独占的回调状态。
 * @return 接受字节数，失败返回 0 并保存首个拥有型错误。
 * @note on_body 返回值的拥有权移交本层，成功结果也在交付后释放。
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
 * @brief 在预算内保存状态行和有限长响应头，记录首个失败原因。
 * @param[in] contents 借用 size*nmemb 字节，无须 NUL 结尾。
 * @param[in] size 元素字节数。
 * @param[in] nmemb 元素数，乘法溢出时中止。
 * @param[in,out] userp 当前请求独占的回调状态。
 * @return 接受字节数，失败返回 0，错误由 perform 接管。
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
 * @brief 在正文回调间隙检查取消和请求预算。
 * @param[in,out] clientp 当前尝试状态。
 * @param[in] dltotal curl 预计下载字节数，当前未使用。
 * @param[in] dlnow curl 已下载字节数，当前未使用。
 * @param[in] ultotal curl 预计上传字节数，当前未使用。
 * @param[in] ulnow curl 已上传字节数，当前未使用。
 * @return 需要中止返回 1，否则返回 0。
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
 * @brief 将借用的请求头复制到本次请求的 curl_slist。
 * @param[in,out] headers 非 NULL 的链表槽，失败保留此前节点。
 * @param[in] name NUL 结尾名称，NULL 时跳过该项。
 * @param[in] value NUL 结尾值，NULL 时跳过该项。
 * @return 成功返回 CC_OK，构造或复制失败返回拥有型错误，最终链表由 perform 释放。
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
 * @brief 将 curl 返回码归为 SDK 成功、超时、取消、内存或网络错误。
 * @param[in] res curl 返回码，具体回调错误由 perform 优先交付。
 * @return 对应 SDK 错误码。
 */
static cc_error_code_t map_curl_error(CURLcode res)
{
    switch (res) {
    case CURLE_OK: return CC_OK;
    case CURLE_OPERATION_TIMEDOUT: return CC_ERR_TIMEOUT;
    case CURLE_ABORTED_BY_CALLBACK: return CC_ERR_CANCELLED;
    case CURLE_OUT_OF_MEMORY: return CC_ERR_OUT_OF_MEMORY;
    default: return CC_ERR_NETWORK;
    }
}

/**
 * @brief 仅在未交付流片段且无本地错误时重试临时网络失败。
 * @param[in] res 当前 curl 返回码。
 * @param[in] ctx 当前尝试状态，可为 NULL。
 * @param[in] attempt_index 从 0 开始的尝试序号。
 * @param[in] retry_count 允许追加的尝试次数。
 * @return 可重试返回 1，达到次数或不适合重放返回 0。
 */
static int should_retry(CURLcode res, const cc_curl_write_ctx_t *ctx, int attempt_index, int retry_count)
{
    if (attempt_index >= retry_count) return 0;
    if (ctx && (ctx->callback_error.code != CC_OK || ctx->saw_stream_body || ctx->address_rejected)) return 0;
    return res == CURLE_COULDNT_RESOLVE_HOST || res == CURLE_COULDNT_CONNECT ||
        res == CURLE_SEND_ERROR || res == CURLE_RECV_ERROR || res == CURLE_OPERATION_TIMEDOUT;
}

/**
 * @brief 同步发布本次尝试的栈上观测快照，公共 int 时长字段饱和到 INT_MAX。
 * @param[in] request 借用请求，无观测回调时直接返回。
 * @param[in] response 借用响应，NULL 时状态码记录为 0。
 * @param[in] ctx 借用尝试状态，NULL 时正文标记记录为 0。
 * @param[in] res curl 返回码。
 * @param[in] attempt_index 从 0 开始的尝试序号。
 * @param[in] retry_performed 是否决定进入下一次尝试。
 * @param[in] attempt_id 进程尝试序号，交付 int 时限制在 INT_MAX 内。
 * @param[in] start_ms 传输开始的单调毫秒值。
 * @param[in] end_ms 传输结束的单调毫秒值。
 * @post 回调已返回，栈快照和其请求字符串借用结束。
 */
static void emit_attempt_metrics(const cc_http_request_t *request,
    const cc_http_response_t *response, const cc_curl_write_ctx_t *ctx, CURLcode res,
    int attempt_index, int retry_performed, unsigned attempt_id,
    uint64_t start_ms, uint64_t end_ms)
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
    metrics.saw_body = ctx ? ctx->saw_body : 0;
    metrics.connection_generation = attempt_index < INT_MAX ? attempt_index + 1 : INT_MAX;
    metrics.origin = request->url;
    metrics.connection_pool = request->connection_pool ? request->connection_pool : "windows-default";
    metrics.connection_discard_reason = "per-request-easy-handle";
    metrics.retry_allowed = retry_performed;
    metrics.retry_performed = retry_performed;
    metrics.partial_stream = ctx && res != CURLE_OK && ctx->saw_stream_body;
    request->on_attempt_metrics(&metrics, request->attempt_metrics_user_data);
}

/**
 * @brief 安装本次传输选项，保留 curl 参数类型检查并在首个错误后停止配置。
 * @param[in,out] curl 本次尝试独占的 handle，失败仍由调用方清理。
 * @param[in] client 只读客户端默认值。
 * @param[in] request 借用请求，body 按 NUL 结尾字符串交付 curl。
 * @param[in] headers 借用本次请求的头链表，可为 NULL。
 * @param[in,out] write_ctx 覆盖整个传输期的栈状态。
 * @param[in] timeout_ms 剩余总预算，单位毫秒，必须为正。
 * @param[in] connect_timeout_ms 连接阶段预算，单位毫秒。
 * @return 全部选项成功返回 CC_OK，失败返回拥有型错误，禁止执行配置失败的 handle。
 * @note 关闭自动重定向，connection_pool 只用于观测，header_profile 当前不改变传输。
 */
static cc_result_t configure_curl_handle(CURL *curl, const cc_windows_http_client_t *client,
    const cc_http_request_t *request, struct curl_slist *headers,
    cc_curl_write_ctx_t *write_ctx, long timeout_ms, long connect_timeout_ms)
{
    const char *method = request->method ? request->method : "GET";
    CURLcode code = CURLE_OK;
#define CC_CURL_SETOPT(option, value) do { \
    if (code == CURLE_OK) code = curl_easy_setopt(curl, option, value); \
} while (0)
    CC_CURL_SETOPT(CURLOPT_URL, request->url);
    CC_CURL_SETOPT(CURLOPT_HTTPHEADER, headers);
    CC_CURL_SETOPT(CURLOPT_TIMEOUT_MS, timeout_ms);
    CC_CURL_SETOPT(CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
    CC_CURL_SETOPT(CURLOPT_NOSIGNAL, 1L);
    CC_CURL_SETOPT(CURLOPT_SSL_VERIFYPEER, client->options.tls_verify ? 1L : 0L);
    CC_CURL_SETOPT(CURLOPT_SSL_VERIFYHOST, client->options.tls_verify ? 2L : 0L);
    CC_CURL_SETOPT(CURLOPT_FOLLOWLOCATION, 0L);
    if (strcmp(method, "GET") == 0) {
        CC_CURL_SETOPT(CURLOPT_HTTPGET, 1L);
    } else if (strcmp(method, "POST") == 0) {
        CC_CURL_SETOPT(CURLOPT_POST, 1L);
        CC_CURL_SETOPT(CURLOPT_POSTFIELDS, request->body ? request->body : "");
    } else {
        CC_CURL_SETOPT(CURLOPT_CUSTOMREQUEST, method);
        if (request->body) CC_CURL_SETOPT(CURLOPT_POSTFIELDS, request->body);
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
#undef CC_CURL_SETOPT
    return code == CURLE_OK ? cc_result_ok() :
        cc_result_error(map_curl_error(code), curl_easy_strerror(code));
}
#endif

/**
 * @brief 按请求值、客户端值和默认值选择阶段预算。
 * @param[in] request_value 请求毫秒值，非正数表示继承。
 * @param[in] configured_value 客户端毫秒值，非正数表示使用 fallback。
 * @param[in] fallback 内置毫秒值，可为 0 关闭该阶段检查。
 * @return 选中的预算。
 */
static long choose_long(long request_value, int configured_value, long fallback)
{
    if (request_value > 0) return request_value;
    if (configured_value > 0) return configured_value;
    return fallback;
}

/**
 * @brief 选择非负追加尝试次数，负请求值禁用，零值继承客户端默认值。
 * @param[in] request_value 请求重试次数，正值覆盖，0 继承，负值禁用。
 * @param[in] configured_value 客户端默认次数，非正数归一为 0。
 * @return 非负重试次数。
 */
static int choose_int(int request_value, int configured_value)
{
    if (request_value < 0) return 0;
    if (request_value > 0) return request_value;
    return configured_value > 0 ? configured_value : 0;
}

/**
 * @brief 同步执行 Windows 请求，全部尝试共享同一个总期限。
 * @param[in] self 创建后只读的客户端实例，必须非 NULL。
 * @param[in] request 借用请求、字符串、头数组、令牌及回调对象，均须覆盖整个调用。
 * @param[out] out_response 非 NULL 的空输出槽，入口清零，成功后调用方拥有头和正文。
 * @return 成功返回 CC_OK，包括非 2xx 状态，失败返回拥有型错误并清空响应。
 * @note 各请求使用独立 easy handle，可并发使用同一实例，销毁须等待全部调用及回调退出。
 */
static cc_result_t windows_http_perform(
    void *self,
    const cc_http_request_t *request,
    cc_http_response_t *out_response
)
{
    cc_windows_http_client_t *client = (cc_windows_http_client_t *)self;
    if (out_response) memset(out_response, 0, sizeof(*out_response));
    if (!client || !request || !request->url || !out_response) {
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
        client->options.connect_timeout_ms, 30000L);
    long first_byte_timeout_ms = choose_long(request->first_byte_timeout_ms,
        client->options.first_byte_timeout_ms, 0L);
    long idle_timeout_ms = choose_long(request->idle_timeout_ms,
        client->options.idle_timeout_ms, 0L);
    int retry_count = choose_int(request->retry_count, client->options.retry_count);
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
        cc_result_t rc = configure_curl_handle(curl, client, request, headers,
            &write_ctx, timeout_ms, connect_timeout_ms);
        if (rc.code != CC_OK) {
            curl_easy_cleanup(curl);
            final_error = rc;
            break;
        }
        cc_result_free(&rc);
        uint64_t attempt_start = cc_platform_monotonic_ms();
        CURLcode res = curl_easy_perform(curl);
        if (transfer_should_abort(&write_ctx)) res = CURLE_ABORTED_BY_CALLBACK;
        uint64_t attempt_end = cc_platform_monotonic_ms();
        CURLcode info_res = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out_response->status_code);
        if (res == CURLE_OK && info_res != CURLE_OK) res = info_res;
        curl_easy_cleanup(curl);
        int retry_performed = should_retry(res, &write_ctx, attempt, retry_count) &&
            !cc_cancel_token_is_cancelled(request->cancel_token) && attempt_end < deadline_ms;
        emit_attempt_metrics(request, out_response, &write_ctx, res, attempt,
            retry_performed, attempt_id, attempt_start, attempt_end);
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
        if (res == CURLE_OK) break;
        if (retry_performed) {
            attempt++;
            continue;
        }
        final_error = cc_result_error(map_curl_error(res), curl_easy_strerror(res));
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
 * @brief 完成当前逐请求 easy handle 实现的连接重置调用。
 * @param[in] self 客户端对象，当前不读取，可为 NULL。
 * @return 始终返回 CC_OK，当前没有 SDK 持久连接池需要清理。
 */
static cc_result_t windows_http_reset_connections(void *self)
{
    (void)self;
    return cc_result_ok();
}

/**
 * @brief 释放客户端拥有的标量配置副本。
 * @param[in] self 创建函数分配的客户端，可为 NULL。
 * @pre 调用方已经关闭入口并等到所有 perform、reset 及其同步回调退出。
 * @post 客户端释放，进程共享的 curl 和 Winsock 初始化状态继续有效。
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

/**
 * @brief 复制 Windows 默认配置，并在发布实例前完成 curl 全局初始化。
 * @param[in] options 借用创建期配置，NULL 使用 TLS 校验开启的内置默认值，size 当前不作版本检查。
 * @param[out] out_client 非 NULL 的空输出槽，入口清零，成功实例由调用方销毁。
 * @return 成功返回 CC_OK，参数、分配或全局初始化失败返回错误并保持空输出。
 * @note 未编入 curl 时仍创建实例，能力位为 0，perform 基本参数校验后直接返回 CC_ERR_PLATFORM。
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
#if CC_HAS_CURL
    out_client->capabilities = CC_HTTP_CAP_STREAM_BODY | CC_HTTP_CAP_CANCEL |
        CC_HTTP_CAP_RESOLVED_ADDRESS_VALIDATION;
#endif
    return cc_result_ok();
}

/**
 * @brief 释放响应拥有的头字段、头数组及正文并清零，支持重复释放空响应。
 * @param[in,out] response 本端口交付的响应或零初始化对象，NULL 时无操作。
 * @pre 调用方独占响应，所有正文和头字段借用已经结束。
 * @post 响应为空，可重新作为 perform 输出槽使用。
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
