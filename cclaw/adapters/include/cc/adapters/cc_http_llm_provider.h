

#ifndef CC_HTTP_LLM_PROVIDER_H
#define CC_HTTP_LLM_PROVIDER_H

#include "cc/ports/cc_llm_provider.h"
#include "cc/ports/cc_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 协议构造器为本次请求选择的响应分帧方式。 */
typedef enum cc_llm_stream_kind {

    CC_LLM_STREAM_NONE = 0, /**< 普通整体响应，不进入流式分发。 */

    CC_LLM_STREAM_SSE = 1, /**< 按 SSE 空行拆事件，再合并各 data 行。 */

    CC_LLM_STREAM_NDJSON = 2 /**< 按换行拆 JSON，干净 EOF 可处理最后一行。 */
} cc_llm_stream_kind_t;

/**
 * @brief 协议构造器交付的拥有请求，首次使用清零，完成或失败后统一 cleanup。
 * @note url/api_key/body_json、header 数组及各字符串由请求拥有，HTTP 仅在同步 perform 期间借用。
 * 实际认证头由协议写入 headers，api_key 字段随请求统一清理。
 */
typedef struct cc_llm_http_request {

    char *url; /**< 拥有的目标 URL。 */

    char *api_key; /**< 可选拥有的凭据副本。 */

    char *body_json; /**< 拥有的 NUL 结尾 JSON 请求体。 */

    cc_http_header_t *headers; /**< 拥有数组及各 name/value 字符串。 */

    size_t header_count; /**< 可清理且有效的 header 条数，非零时须匹配已分配数组。 */

    cc_llm_stream_kind_t stream_kind; /**< 本次响应使用的分帧方式。 */
    /* 非零时语义完成后继续接收 usage，直到传输终止标记或成功 EOF 才交付 FINISHED。 */
    int defer_finished_until_end;
} cc_llm_http_request_t;


/* LLM 协议 adapter 前置声明。 */
typedef struct cc_llm_protocol cc_llm_protocol_t;

/* LLM 协议 vtable 前置声明。 */
typedef struct cc_llm_protocol_vtable cc_llm_protocol_vtable_t;


/** @brief 按值交接的协议对象，成功创建 provider 后由 provider 调用可选析构。 */
struct cc_llm_protocol {

    void *self; /**< 协议状态，可为 NULL；没有析构时其底层寿命由提供方覆盖 provider。 */

    const cc_llm_protocol_vtable_t *vtable; /**< 借用操作表，覆盖 provider 的最终销毁。 */
};


/** @brief 同步协议转换回调，状态并发与回调重入由协议提供方协调。 */
struct cc_llm_protocol_vtable {

    /**
     * @brief 提供用于能力映射的协议名称。
     * @param[in] self 调用期间借用的协议状态，可空性由协议决定。
     * @return 借用名称，至少覆盖本次调用；NULL 按未知协议处理。
     */
    const char *(*name)(void *self);

    /**
     * @brief 将统一请求转成供应商 HTTP 请求。
     * @param[in,out] self 借用协议状态，覆盖本次同步调用。
     * @param[in] base_url 借用 provider 拥有的地址，至少为空串。
     * @param[in] api_key 可选凭据借用。
     * @param[in] default_model 借用 provider 拥有的默认模型，至少为空串。
     * @param[in] request 有效统一请求借用，字段保持到本次 provider 调用完成。
     * @param[in] stream 非零要求构造流式请求。
     * @param[in,out] out_request 已清零的有效槽，已发布拥有字段在成功或失败时均由 provider cleanup。
     * @return 转换结果及拥有诊断；自行清理已发布字段时须清零相应槽，避免重复释放。
     */
    cc_result_t (*build_request)(
        void *self,
        const char *base_url,
        const char *api_key,
        const char *default_model,
        const cc_llm_chat_request_t *request,
        int stream,
        cc_llm_http_request_t *out_request
    );

    /**
     * @brief 将成功 HTTP 正文转成统一响应。
     * @param[in,out] self 调用期借用协议状态。
     * @param[in] response_json 有效 NUL 结尾响应正文，仅在本次调用借用。
     * @param[in,out] out_response 已清零目标，交付的消息或部分输出由上层 cc_llm_response_free。
     * @return 解析结果及拥有诊断，JSON 错误在 provider 层转换成稳定提示。
     */
    cc_result_t (*parse_response)(
        void *self,
        const char *response_json,
        cc_llm_response_t *out_response
    );

    /**
     * @brief 同步解析一条已分帧正文，并逐块调用输出回调。
     * @param[in,out] self 调用期借用协议状态。
     * @param[in] event_json 借用事件文本，SSE 多 data 行已合并，寿命至返回。
     * @param[in] on_chunk 有效同步输出回调，非 OK 结果应沿解析调用向上传递。
     * @param[in,out] user_data 回调上下文借用，至少覆盖本次解析与全部嵌套回调。
     * @param[out] out_finished 有效槽，调用前为零，语义完成时置非零，后续传输完成时点由 provider 协调。
     * @return 解析或下游回调结果，拥有诊断交调用者释放；失败前可已交付部分 chunk。
     * @note chunk 及字段仅借用到回调返回，长期保存须由接收者复制。
     */
    cc_result_t (*parse_stream_event)(
        void *self,
        const char *event_json,
        cc_llm_stream_callback_fn on_chunk,
        void *user_data,
        int *out_finished
    );

    /**
     * @brief 释放 provider 成功接管的协议状态。
     * @param[in,out] self 消费协议状态，可空性由具体协议决定。
     * @post 状态拥有资源释放，所有协议借用结束；调用前全部请求与回调已退出。
     */
    void (*destroy)(void *self);
};


/**
 * @brief 释放协议请求拥有的完整或部分字段并清零。
 * @param[in,out] request 已清零或初始化的请求，可为 NULL。
 * @post 非 NULL 目标归零，字段借用失效，结构体本身继续由调用者拥有。
 */
void cc_llm_http_request_cleanup(cc_llm_http_request_t *request);


/**
 * @brief 复制连接配置并成功接管协议，创建拥有 HTTP LLM provider。
 * @param[in,out] http_client 有效 HTTP 端口借用，覆盖 provider 全部请求与销毁，端口继续由外层拥有。
 * @param[in] base_url 调用期借用地址，NULL 复制为空串。
 * @param[in] api_key 调用期可选凭据，非 NULL 时须成功复制。
 * @param[in] model 调用期借用默认模型，NULL 复制为空串。
 * @param[in,out] protocol 按值提交的协议，须有 build_request/parse_response，成功交接，失败仍归调用者。
 * @param[out] out_provider 必填空资源槽，成功取得拥有端口，失败保留原值。
 * @return CC_OK、参数错误或 OOM，诊断由调用者释放，失败清理本次副本且保留协议所有权。
 * @note provider 和协议可以在其 HTTP/协议能力允许时并发请求，销毁前由 owner 关闭入口并等全部调用及回调退出。
 * HTTP body/metrics 回调均须在同步 perform 返回前退出，流式失败前可能已交付部分 chunk。
 * HTTP 响应 body_size 非零时按明确字节数补 NUL，成功正文含原始 NUL 返回 JSON 错误，错误正文内 NUL 转为空格后脱敏。
 * body_size 为零且 body 非空时兼容既有 NUL 结尾文本端口，端口须保证字符串终止；响应 body 由标准分配器分配并交 provider 释放。
 * perform 的传输或 body 回调错误优先返回，成功 perform 的非 2xx 响应才进入后续 HTTP 状态分类。
 */
cc_result_t cc_http_llm_provider_create(
    cc_http_client_t *http_client,
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_protocol_t protocol,
    cc_llm_provider_t *out_provider
);

#ifdef __cplusplus
}
#endif

#endif
