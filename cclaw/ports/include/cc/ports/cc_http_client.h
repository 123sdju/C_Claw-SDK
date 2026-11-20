



#ifndef CC_HTTP_CLIENT_H
#define CC_HTTP_CLIENT_H

#include "cc/core/cc_result.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/**
 * @brief 在平台访问 URL 前同步决定是否允许该地址。
 * @param[in] url 回调期间借用的完整 NUL 结尾地址。
 * @param[in,out] user_data 借用调用者状态，须在整个 perform 期间有效。
 * @return 非零允许，零拒绝，函数须在返回前结束对 url 的访问。
 * @note 回调运行于 perform 调用线程，其共享状态由调用者同步。
 */
typedef int (*cc_http_url_validator_fn)(const char *url, void *user_data);

/**
 * @brief 同步检查平台解析出的候选地址或已连接对端。
 * @param[in] host 回调期间借用的原主机名。
 * @param[in] numeric_address 回调期间借用的数字 IP 字符串。
 * @param[in,out] user_data 借用调用者状态，须在 perform 期间有效。
 * @return 非零允许，零拒绝，结束时归还所有借用。
 * @note 具体检查点由端口能力决定，回调共享状态须外层同步。
 */
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

/**
 * @brief 同步消费平台交付的一段响应正文。
 * @param[in] data 回调期间借用的 len 字节，无 NUL 终止保证，零长度时可为 NULL。
 * @param[in] len 本片段可读取的字节数。
 * @param[in,out] user_data perform 期间借用的调用者状态。
 * @return 结果的拥有权交给端口，包括 CC_OK 携带的动态消息；错误用于停止接收并传回调用者。
 * @note 片段边界由端口决定。ESP32 在连接池锁内回调，回调内重入 perform、reset 或 destroy 可能死锁。
 */
typedef cc_result_t (*cc_http_body_callback_fn)(
    const char *data,
    size_t len,
    void *user_data
);

/* 单次 HTTP 尝试的端口观测，未支持或尚未观测的字段由各端口约定。 */
typedef struct cc_http_attempt_metrics {
    size_t size;           /**< 结构体大小 */
    const char *stage;     /**< 回调期间借用的请求阶段名 */
    const char *err;       /**< 错误描述，成功时为 NULL */
    long status;           /**< HTTP 状态码 */
    int attempt_id;        /**< 端口生成的本次尝试标识 */
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

/**
 * @brief 在一次尝试结束时同步接收端口观测。
 * @param[in] metrics 回调期间借用的指标对象及其中字符串，后续使用须复制。
 * @param[in,out] user_data perform 期间借用的调用者状态，其共享访问须自行同步。
 * @post 不转移任何资源，返回前结束对 metrics 的访问。
 * @note ESP32 可在连接池锁内回调，禁止同步重入该客户端的 perform、reset 或 destroy。
 */
typedef void (*cc_http_attempt_metrics_callback_fn)(
    const cc_http_attempt_metrics_t *metrics,
    void *user_data
);

/*
 * HTTP 请求描述。
 *
 * 所有字符串、header 数组、取消令牌和回调状态均借用至 perform 返回。
 * body 是 NUL 结尾字符串，当前请求接口未提供二进制正文长度。
 * header_count 非零时 headers 须指向足够数量的有效元素。
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

    int retry_count; /**< 正数覆盖客户端重试次数，零继承客户端配置，负数明确禁止重试。 */

    size_t max_response_bytes;

    cc_http_body_callback_fn on_body;

    void *user_data;

    cc_http_attempt_metrics_callback_fn on_attempt_metrics;

    void *attempt_metrics_user_data;

    cc_cancel_token_t *cancel_token;

    /* 与 cc_platform_monotonic_ms 同一启动时间域的绝对截止时间，非零时进一步收紧总请求预算。 */
    uint64_t deadline_ms;

    /* 初始 URL 及支持自动跳转的每一跳同步校验。 */
    cc_http_url_validator_fn validate_url;
    void *validate_url_user_data;

    /* 端口支持时检查全部解析候选及连接对端，参见 capabilities。 */
    cc_http_address_validator_fn validate_address;
    void *validate_address_user_data;

    size_t max_redirects;

    /*
     * 可选连接池选择，NULL 采用平台默认值。ESP32 隔离模型流式、同步与本地连接。
     * 名称按端口规则选择已有池，字符串只借用至 perform 返回。
     */
    const char *connection_pool;

    /*
     * 不包含 header value 的请求形状指纹，用于 ESP32 的复用兼容性判断。
     * method 或 header 名集合变化时应采用不同值，NULL 采用端口默认形状。
     * 每次请求仍须提交全部 header，平台会重新安装本次字段。
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

/* 客户端创建时复制的值选项。size 是描述字段，当前端口按本版本完整结构读取。 */
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
    /**
     * @brief 同步执行请求并在返回前结束全部回调。
     * @param[in,out] self 借用的存活平台实例，拥有者保证调用期间不会销毁。
     * @param[in] request 借用且稳定的请求及关联资源。
     * @param[out] out_response 接收平台拥有响应资源，调用前为空，错误后也须按端口约定清理。
     * @return 拥有结果，调用者释放消息，HTTP 状态码保存在响应内。
     * @note 并发能力由端口提供，ESP32 按池串行，curl 每次调用独立持有传输资源。
     */
    cc_result_t (*perform)(
        void *self,
        const cc_http_request_t *request,
        cc_http_response_t *out_response);
    /**
     * @brief 同步丢弃缓存连接，未缓存连接的端口允许空操作。
     * @param[in,out] self 借用存活实例，拥有者保证不会并发销毁。
     * @return 拥有结果，调用者释放，ESP32 等待相关连接池锁后执行重置。
     * @note 允许的并发由端口约定，回调内同步重入可能阻塞自身。
     */
    cc_result_t (*reset_connections)(void *self);
    /**
     * @brief 释放已停止接收调用的平台实例及缓存连接。
     * @param[in,out] self 被消费的平台实例。
     * @pre 拥有者已关闭入口，等待全部已接纳 perform、reset 及其回调返回。
     * @post 实例已失效，禁止再经其别名发起调用，函数不提供独立的调用排空屏障。
     */
    void (*destroy)(void *self);
};

/* 拥有平台 self 的描述符，复制结构仅产生别名，销毁和并发准入由唯一拥有者协调。 */
struct cc_http_client {
    void *self;
    const cc_http_client_vtable_t *vtable;
    size_t size;
    uint64_t capabilities;
};

/**
 * @brief 创建当前构建所选平台的 HTTP 客户端。
 * @param[in] options 调用期间借用的完整本版本选项，NULL 使用端口默认值。
 * @param[out] out_client 空描述符输出槽，成功后调用者负责关闭入口、排空并销毁。
 * @return 成功为 CC_OK，失败返回拥有错误消息的结果，由调用者释放。
 * @note 创建不同实例可并发，具体能力和默认超时取决于已编入的平台实现。
 */
cc_result_t cc_http_client_create_default(
    const cc_http_client_options_t *options,
    cc_http_client_t *out_client);

/**
 * @brief 检查描述符后同步转发平台 HTTP 请求。
 * @param[in,out] client 借用的存活描述符，拥有者保证整个调用期间不会销毁或改写。
 * @param[in] request 借用请求，字段校验和阶段预算由平台实现负责。
 * @param[out] out_response 接收响应资源的空槽，调用者最终 cc_http_response_free。
 * @return 拥有结果，由调用者释放，描述符无效时返回参数错误且不访问输出槽。
 * @note 包装层不增加锁、后台线程或销毁屏障。取消、流式和连接池能力参见端口描述符。
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

/**
 * @brief 检查描述符后同步重置平台缓存连接。
 * @param[in,out] client 借用的存活描述符，拥有者保证调用期间不会销毁。
 * @return 平台拥有结果，调用者释放，缺少重置入口时返回参数错误。
 * @note ESP32 等待池锁，curl 为无缓存空操作；禁止在持有同一池的回调内同步调用。
 */
static inline cc_result_t cc_http_client_reset_connections(cc_http_client_t *client)
{
    if (!client || !client->vtable || !client->vtable->reset_connections) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "HTTP client instance is unavailable");
    }
    return client->vtable->reset_connections(client->self);
}

/**
 * @brief 消费平台实例并清零客户端描述符。
 * @param[in,out] client 唯一拥有的描述符，可为 NULL，空描述符允许重复销毁。
 * @pre 拥有者已关闭入口，并等待所有已接纳 API 调用及回调完全返回。
 * @post 描述符清零，其原 self 和所有别名均失效，禁止继续调用。
 * @note 不与 perform、reset 或描述符读写并发，排空由 SDK 外层拥有者或自定义拥有者负责。
 */
static inline void cc_http_client_destroy(cc_http_client_t *client)
{
    if (!client) return;
    if (client->vtable && client->vtable->destroy) client->vtable->destroy(client->self);
    client->self = NULL;
    client->vtable = NULL;
    client->size = 0;
    client->capabilities = 0;
}

/**
 * @brief 释放响应拥有的 header 数组、字段字符串及正文，然后清零响应。
 * @param[in,out] response 独占响应，可为 NULL，空响应允许重复清理。
 * @post 非空响应已清零，原字段借用全部失效，response 结构本身仍归调用者。
 * @note 须在 perform 返回及所有正文使用者结束后调用，禁止并发读写同一响应。
 */
void cc_http_response_free(cc_http_response_t *response);

#ifdef __cplusplus
}
#endif

#endif
