

#ifndef CC_HTTP_LLM_PROVIDER_H
#define CC_HTTP_LLM_PROVIDER_H

#include "cc/ports/cc_llm_provider.h"
#include "cc/ports/cc_http_client.h"

/*
 * HTTP provider 的流式响应格式。
 *
 * SSE 用于 OpenAI/Anthropic 类 event stream，NDJSON 适合 Ollama 类逐行 JSON；NONE 表示
 * 普通一次性响应。
 */
typedef enum cc_llm_stream_kind {

    CC_LLM_STREAM_NONE = 0,

    CC_LLM_STREAM_SSE = 1,

    CC_LLM_STREAM_NDJSON = 2
} cc_llm_stream_kind_t;

/*
 * 协议层构造出的 HTTP 请求。
 *
 * url/api_key/body_json/headers 由 request 拥有，调用 cc_llm_http_request_cleanup() 释放。
 * protocol 负责把通用 chat request 翻译成供应商 HTTP payload。
 */
typedef struct cc_llm_http_request {

    char *url;

    char *api_key;

    char *body_json;

    cc_http_header_t *headers;

    size_t header_count;

    cc_llm_stream_kind_t stream_kind;
} cc_llm_http_request_t;


/* LLM 协议 adapter 前置声明。 */
typedef struct cc_llm_protocol cc_llm_protocol_t;

/* LLM 协议 vtable 前置声明。 */
typedef struct cc_llm_protocol_vtable cc_llm_protocol_vtable_t;


/* 供应商协议接口对象：OpenAI/Ollama/Anthropic 等具体协议挂在 self/vtable 上。 */
struct cc_llm_protocol {

    void *self;

    const cc_llm_protocol_vtable_t *vtable;
};


/* HTTP LLM 协议 vtable。 */
struct cc_llm_protocol_vtable {

    /* 返回协议名；用于日志、错误详情和诊断。 */
    const char *(*name)(void *self);

    /* 构造 HTTP 请求；out_request 成功后由调用方 cleanup。 */
    cc_result_t (*build_request)(
        void *self,
        const char *base_url,
        const char *api_key,
        const char *default_model,
        const cc_llm_chat_request_t *request,
        int stream,
        cc_llm_http_request_t *out_request
    );

    /* 解析非流式 HTTP 响应 JSON 到通用 LLM response。 */
    cc_result_t (*parse_response)(
        void *self,
        const char *response_json,
        cc_llm_response_t *out_response
    );

    /* 解析单个流式事件并通过 on_chunk 输出；out_finished 表示 provider 流结束。 */
    cc_result_t (*parse_stream_event)(
        void *self,
        const char *event_json,
        cc_llm_stream_callback_fn on_chunk,
        void *user_data,
        int *out_finished
    );

    /* 销毁协议 self。 */
    void (*destroy)(void *self);
};


/* 清理协议构造的 HTTP 请求字段；不释放 request 指针。 */
void cc_llm_http_request_cleanup(cc_llm_http_request_t *request);


/*
 * 创建通用 HTTP LLM provider。
 *
 * base_url/api_key/model 在创建时复制；protocol 所有权转移给 provider。provider 负责
 * HTTP 错误分类、Retry-After 解析、stream 分发和 redaction。
 */
cc_result_t cc_http_llm_provider_create(
    cc_http_client_t *http_client,
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_protocol_t protocol,
    cc_llm_provider_t *out_provider
);

#endif
