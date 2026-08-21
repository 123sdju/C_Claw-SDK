



#ifndef CC_HTTP_CLIENT_H
#define CC_HTTP_CLIENT_H

#include "cc/core/cc_result.h"
#include <stddef.h>
#include <stdint.h>

/* cancel token 前置声明，HTTP port 只借用指针以支持请求取消。 */
typedef struct cc_cancel_token cc_cancel_token_t;
typedef struct cc_http_client cc_http_client_t;
typedef struct cc_http_client_vtable cc_http_client_vtable_t;

enum {
    CC_HTTP_CAP_STREAM_BODY = 1ULL << 0,
    CC_HTTP_CAP_CANCEL = 1ULL << 1,
    CC_HTTP_CAP_CONNECTION_POOL = 1ULL << 2,
    CC_HTTP_CAP_REDIRECT_VALIDATION = 1ULL << 3,
    CC_HTTP_CAP_RESOLVED_ADDRESS_VALIDATION = 1ULL << 4,
};

typedef int (*cc_http_url_validator_fn)(const char *url, void *user_data);
typedef int (*cc_http_address_validator_fn)(
    const char *host,
    const char *numeric_address,
    void *user_data);

/*
 * HTTP header 键值对。
 *
 * 在 request 中 header 字符串由调用方借用；在 response 中 header 数组和字符串由
 * response 拥有，并通过 cc_http_response_free() 释放。
 */
typedef struct cc_http_header {

    const char *name;

    const char *value;
} cc_http_header_t;

/*
 * 流式 body 回调。
 *
 * data 只在回调期间有效；返回非 OK 表示停止接收并把错误传回 HTTP 调用方。平台实现
 * 应遵守 max_response_bytes 和 cancel_token，避免大响应耗尽内存。
 */
typedef cc_result_t (*cc_http_body_callback_fn)(
    const char *data,
    size_t len,
    void *user_data
);

/* 单次 HTTP 重试的耗时指标；由 provider adapter 填充，供观测和诊断使用。 */
typedef struct cc_http_attempt_metrics {
    size_t size;           /**< 结构体大小 */
    const char *stage;     /**< 请求阶段名（dns/connect/tls/send/wait） */
    const char *err;       /**< 错误描述，成功时为 NULL */
    long status;           /**< HTTP 状态码 */
    int attempt_id;        /**< 本次尝试唯一 id */
    int attempt_index;     /**< 重试序号（从 0 开始） */
    int final_attempt;     /**< 是否为最后一次尝试 */
    int total_ms;          /**< 从请求开始到本次尝试结束的毫秒数 */
    int connect_ms;        /**< 连接建立耗时 */
    int header_ms;         /**< 收到首字节响应头耗时 */
    int first_body_ms;     /**< 收到首字节 body 耗时 */
    int saw_body;          /**< 是否已收到 body 数据 */
    int reused_connection; /**< 本次尝试是否复用了已有连接 */
    int connection_generation; /**< 平台连接代际编号，用于定位重建次数 */
    const char *origin;    /**< 诊断用 origin key，回调期间借用 */
    const char *connection_pool; /**< 平台连接池名，回调期间借用 */
    int reuse_count;       /**< 当前 handle 已连续复用次数 */
    int idle_ms_before_reuse; /**< 本次复用前 handle 空闲时间 */
    int max_reuse_reached; /**< 是否因达到最大复用次数而轮换 */
    const char *connection_discard_reason; /**< 本次尝试导致连接丢弃的原因 */
    int retry_allowed;     /**< 本次失败是否允许同请求重试 */
    int retry_performed;   /**< 是否已安排同请求重试 */
    int partial_stream;    /**< 流式请求是否已产生部分 body 后失败 */
    int saw_done;          /**< 观测回调触发时上层协议是否已报告完整结束 */
} cc_http_attempt_metrics_t;

/* HTTP 重试指标回调；metrics 只在回调期间有效。 */
typedef void (*cc_http_attempt_metrics_callback_fn)(
    const cc_http_attempt_metrics_t *metrics,
    void *user_data
);

/*
 * HTTP 请求描述。
 *
 * 所有字符串和 header 数组都是借用指针，只需在 cc_http_client_perform() 调用期间有效。
 * 如果 on_body 非 NULL，平台实现可以流式回调 body；否则通常把响应体放入 out_response。
 * 自动 redirect 必须重新校验 network allowlist，不能绕过 SDK 安全策略。
 */
typedef struct cc_http_request {

    const char *method;

    const char *url;

    const cc_http_header_t *headers;

    size_t header_count;

    const char *body;

    /* timeout_ms 为总请求预算；connect_timeout_ms/first_byte_timeout_ms/idle_timeout_ms
     * 进一步细化阶段超时，为 0 时由平台 port 使用安全默认值。 */
    long timeout_ms;

    long connect_timeout_ms;

    long first_byte_timeout_ms;

    long idle_timeout_ms;

    int retry_count;

    size_t max_response_bytes;

    cc_http_body_callback_fn on_body;

    void *user_data;

    cc_http_attempt_metrics_callback_fn on_attempt_metrics;

    void *attempt_metrics_user_data;

    cc_cancel_token_t *cancel_token;

    /* Absolute monotonic deadline. Zero means derive one from timeout_ms. */
    uint64_t deadline_ms;

    /* Rechecked for the initial URL and every redirect hop. */
    cc_http_url_validator_fn validate_url;
    void *validate_url_user_data;

    /* Called for every resolved candidate and the connected peer when supported. */
    cc_http_address_validator_fn validate_address;
    void *validate_address_user_data;

    size_t max_redirects;

    /*
     * Optional platform reuse controls. NULL keeps the platform default.
     * ESP32 uses these to isolate LLM stream/sync/local handles while
     * retaining keep-alive reuse inside each pool.
     */
    const char *connection_pool;

    /*
     * 不含敏感 header value 的请求形状指纹。ESP32 用它隔离 handle 级 header 状态；
     * 调用方应在 method 或 header 名集合变化时提供不同值，NULL 视为独立的默认形状。
     */
    const char *header_profile;
} cc_http_request_t;

/*
 * HTTP 响应。
 *
 * status_code 是服务器状态码；headers/body 由平台实现分配，调用方用
 * cc_http_response_free() 清理。body_size 用于安全处理二进制或非 NUL 结尾数据。
 */
typedef struct cc_http_response {

    long status_code;


    cc_http_header_t *headers;

    size_t header_count;

    char *body;

    size_t body_size;
} cc_http_response_t;

/* HTTP 客户端实例选项。创建后只属于该实例，不影响其它 runtime。 */
typedef struct cc_http_client_options {
    size_t size;              /**< 结构体大小 */
    int tls_verify;           /**< 是否验证 TLS 证书 */
    int connect_timeout_ms;   /**< 连接超时 */
    int first_byte_timeout_ms; /**< 首字节超时 */
    int idle_timeout_ms;      /**< 空闲超时 */
    int retry_count;          /**< 重试次数 */
    int log_level;            /**< 日志等级 */
    int trace_persist;        /**< 是否持久化请求 trace */
} cc_http_client_options_t;

struct cc_http_client_vtable {
    cc_result_t (*perform)(
        void *self,
        const cc_http_request_t *request,
        cc_http_response_t *out_response);
    cc_result_t (*reset_connections)(void *self);
    void (*destroy)(void *self);
};

struct cc_http_client {
    void *self;
    const cc_http_client_vtable_t *vtable;
    size_t size;
    uint64_t capabilities;
};

cc_result_t cc_http_client_create_default(
    const cc_http_client_options_t *options,
    cc_http_client_t *out_client);

/*
 * 执行 HTTP 请求。
 *
 * 这是平台 port 的统一入口，POSIX/Windows/ESP32/FreeRTOS 可分别实现。错误应映射为
 * CC_ERR_NETWORK、CC_ERR_TIMEOUT、CC_ERR_CANCELLED、CC_ERR_LIMIT_EXCEEDED 等稳定结果。
 */
static inline cc_result_t cc_http_client_perform(
    cc_http_client_t *client,
    const cc_http_request_t *request,
    cc_http_response_t *out_response
)
{
    if (!client || !client->vtable || !client->vtable->perform) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP client instance is unavailable");
    }
    return client->vtable->perform(client->self, request, out_response);
}

/* 重置缓存的持久 HTTP/TLS 连接句柄，Wi-Fi 断开后调用可避免重用无效连接。 */
static inline cc_result_t cc_http_client_reset_connections(cc_http_client_t *client)
{
    if (!client || !client->vtable || !client->vtable->reset_connections) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP client instance is unavailable");
    }
    return client->vtable->reset_connections(client->self);
}

static inline void cc_http_client_destroy(cc_http_client_t *client)
{
    if (!client) return;
    if (client->vtable && client->vtable->destroy) client->vtable->destroy(client->self);
    client->self = NULL;
    client->vtable = NULL;
    client->size = 0;
    client->capabilities = 0;
}

/* 释放 HTTP 响应拥有的 headers/body；不释放 response 指针本身。 */
void cc_http_response_free(cc_http_response_t *response);

#endif
