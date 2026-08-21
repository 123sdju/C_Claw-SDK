



#ifndef CC_LLM_PROVIDER_H
#define CC_LLM_PROVIDER_H

#include "cc/core/cc_message.h"
#include "cc/core/cc_result.h"
#include "cc/core/cc_tool_call.h"
#include "cc/core/cc_stream_chunk.h"
#include "cc/ports/cc_event_bus.h"
#include <stddef.h>
#include <stdint.h>

/* cancel token 前置声明，避免 provider port 依赖 app 层完整定义。 */
typedef struct cc_cancel_token cc_cancel_token_t;

/*
 * LLM provider 能力矩阵。
 *
 * runtime create/reload 阶段用它校验配置，避免请求 stream/tool/multimodal 时 provider
 * 静默降级。mime_types 数组和字符串由 provider 拥有，只在能力查询结果生命周期内借用；
 * size 字段用于未来扩展结构。
 */
typedef struct cc_llm_provider_capabilities {
    size_t size;
    int text_input;
    int text_output;
    int image_input;
    int audio_input;
    int video_input;
    int file_input;
    int image_output;
    int audio_output;
    int video_output;
    int file_output;
    int tool_calling;
    int reasoning;
    int streaming;
    size_t max_artifacts;
    size_t max_artifact_bytes;
    const char **mime_types;
    size_t mime_type_count;
} cc_llm_provider_capabilities_t;

/*
 * 一次 chat 请求。
 *
 * messages/media_limits/cancel_token 为借用指针；tools_json/model 目前是调用方提供的
 * 字符串，provider 不能保存超过调用生命周期。stream=1 时 runtime 应先确认 provider
 * 支持 streaming；timeout_ms 和 cancel_token 共同表达资源限制和取消传播。
 */
typedef struct cc_llm_chat_request {
    size_t size;
    const cc_message_t *messages;
    size_t message_count;
    char *tools_json;

    char *model;
    char *extra_body_json;
    int stream;

    int max_tokens;
    double temperature;

    int thinking_mode;

    const cc_media_limits_t *media_limits;
    cc_cancel_token_t *cancel_token;
    int timeout_ms;
    size_t max_response_bytes;
    uint64_t deadline_ms;

    cc_event_bus_t *event_bus;
    const char *session_id;
    int step;
} cc_llm_chat_request_t;


/* provider vtable 前置声明。 */
typedef struct cc_llm_provider_vtable cc_llm_provider_vtable_t;

/* provider 接口对象前置声明。 */
typedef struct cc_llm_provider cc_llm_provider_t;

/*
 * LLM provider 接口对象。
 *
 * self 指向 OpenAI/Ollama/Anthropic 等具体 adapter，vtable 提供同步、流式、销毁和能力
 * 查询函数。runtime 只依赖该 port，因此 SDK 核心不绑定任何 HTTP 客户端或云厂商。
 */
struct cc_llm_provider {
    void *self;
    const cc_llm_provider_vtable_t *vtable;
};


/*
 * LLM provider vtable。
 *
 * provider adapter 负责把 cc_llm_chat_request_t 转换成供应商协议，并把错误映射到
 * cc_result_t/cc_error_detail_t。Runtime 的 step engine 只消费统一 chunk 协议：chat()
 * 的完整响应由 Runtime 适配为 chunk，chat_stream() 则直接产生 chunk；两者不会形成
 * 两套工具循环。SDK 不在这里做自动 retry/backoff，只暴露可恢复语义。
 */
struct cc_llm_provider_vtable {


    /*
     * 同步 chat 请求。
     *
     * out_response 由调用方初始化，provider 填充后由调用方 cc_llm_response_free()。
     * 429/5xx/timeout/cancel/JSON 错误应映射为稳定 cc_result_t。
     */
    cc_result_t (*chat)(
        void *self,
        const cc_llm_chat_request_t *request,
        cc_llm_response_t *out_response
    );



    /*
     * 流式 chat 请求。
     *
     * on_chunk 在 provider 解析到增量时调用；chunk 生命周期只覆盖回调期间。provider
     * 不应在不支持 streaming 时静默退化，应返回 CC_ERR_UNSUPPORTED。
     */
    cc_result_t (*chat_stream)(
        void *self,
        const cc_llm_chat_request_t *request,
        cc_llm_stream_callback_fn on_chunk,
        void *user_data
    );



    /* 销毁 provider adapter self。 */
    void (*destroy)(void *self);



    /*
     * 查询 provider 能力。
     *
     * out_capabilities 由调用方提供，provider 填充 size 和能力字段；runtime 用结果做
     * fail-fast 配置校验。
     */
    cc_result_t (*capabilities)(
        void *self,
        cc_llm_provider_capabilities_t *out_capabilities
    );
};

#endif
