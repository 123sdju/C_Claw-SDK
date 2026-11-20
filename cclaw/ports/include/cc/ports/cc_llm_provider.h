



#ifndef CC_LLM_PROVIDER_H
#define CC_LLM_PROVIDER_H

#include "cc/core/cc_message.h"
#include "cc/core/cc_result.h"
#include "cc/core/cc_tool_call.h"
#include "cc/core/cc_stream_chunk.h"
#include "cc/ports/cc_event_bus.h"
#include <stddef.h>
#include <stdint.h>

/* 前置声明取消令牌，减少提供者端口对应用层完整类型的依赖。 */
typedef struct cc_cancel_token cc_cancel_token_t;

/*
 * 模型提供者的能力快照，无动态拥有成员。
 *
 * 运行时创建/重载阶段据此校验流式、工具和多模态配置。mime_types 数组与字符串由
 * 提供者拥有，查询者仅借用，使用期间保持该实例及其能力元数据有效。
 * size 描述实现写出的布局大小，调用者按实际支持范围读取。
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
 * 一次同步或流式模型请求，动态成员均由调用者拥有。
 *
 * messages、tools_json、model、extra_body_json、media_limits、cancel_token 和观测标识
 * 均按调用期借用，调用者保持数组及嵌套内容稳定。stream 请求由运行时预先校验能力。
 * timeout_ms 表示请求预算，deadline_ms 使用平台单调毫秒时钟，具体默认及两者组合
 * 由适配器定义，取消令牌在整个调用内存活。event_bus 同样借用，销毁前等待请求退出。
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
    /* 可选持久运行的追踪 ID，只用于本地观测，调用期间借用。 */
    const char *run_id;
    /* 异步记忆评估来源轮次 ID，调用期间借用，独立于持久运行 ID。 */
    const char *origin_turn_id;
} cc_llm_chat_request_t;

/**
 * @brief 按请求布局大小安全读取可选运行追踪 ID。
 * @param[in] request 调用期借用的有效请求，允许 NULL，实际对象须至少包含 size。
 * @return 字段完整存在时返回原借用指针，NULL/零 size/短布局返回 NULL。
 * @note 只对后追加标识做布局检查，基础请求字段仍遵循具体提供者的输入契约。
 */
static inline const char *cc_llm_chat_request_run_id(const cc_llm_chat_request_t *request)
{
    return request && request->size >= offsetof(cc_llm_chat_request_t,run_id)+sizeof(request->run_id) ?
        request->run_id : NULL;
}
/**
 * @brief 按布局大小读取异步记忆评估的来源轮次 ID。
 * @param[in] request 借用请求或 NULL，实际对象须至少包含 size。
 * @return 字段完整存在时返回借用指针，零 size 或不足字段范围返回 NULL。
 */
static inline const char *cc_llm_chat_request_origin_turn_id(const cc_llm_chat_request_t *request)
{
    return request && request->size >= offsetof(cc_llm_chat_request_t,origin_turn_id)+sizeof(request->origin_turn_id) ?
        request->origin_turn_id : NULL;
}


/* 模型提供者虚表前置声明。 */
typedef struct cc_llm_provider_vtable cc_llm_provider_vtable_t;

/* 模型提供者接口对象前置声明。 */
typedef struct cc_llm_provider cc_llm_provider_t;

/*
 * 模型提供者接口对象，self 拥有关系由构造及运行时交接约定确定。
 *
 * self 指向 OpenAI/Ollama/Anthropic 等具体适配器，vtable 提供请求、销毁和能力查询。
 * 运行时通过该端口消费统一协议。接口值复制只复制借用地址，不增加引用计数。
 */
struct cc_llm_provider {
    void *self;
    const cc_llm_provider_vtable_t *vtable;
};


/*
 * 模型提供者虚表，函数指针在实例生命周期内稳定。
 *
 * 适配器将请求转换为供应商协议并映射统一错误。运行时把完整响应或流式数据块交给
 * 同一执行循环。自动重试由外层策略和底层 HTTP 请求选项控制。实例是否允许并发请求
 * 由具体实现声明，拥有者在销毁前排空全部请求和回调。
 */
struct cc_llm_provider_vtable {


    /**
     * @brief 同步执行一条模型请求并填入完整响应。
     * @param[in,out] self 实例状态借用，整个调用期间存活。
     * @param[in] request 调用期只读借用请求及嵌套输入。
     * @param[out] out_response 调用者提供的新/已清理槽，最终用 cc_llm_response_free 清理。
     * @return 成功或网络/协议/取消/预算等统一错误，结果诊断归调用者。
     * @note 调用者按具体实现的失败输出约定清理可能产生的响应资源。
     */
    cc_result_t (*chat)(
        void *self,
        const cc_llm_chat_request_t *request,
        cc_llm_response_t *out_response
    );



    /**
     * @brief 同步执行流式请求并逐块调用接收者。
     * @param[in,out] self 调用期间有效的实例借用。
     * @param[in] request 调用期间稳定的请求借用。
     * @param[in] on_chunk 同步回调，块及其文本仅在回调期间借用。
     * @param[in,out] user_data 调用者拥有的借用上下文。
     * @return 成功、回调错误或提供者错误，不支持流式返回 UNSUPPORTED。
     * @note 返回前已有数据块可能已被消费。回调禁止销毁当前实例或违反实现重入约束。
     */
    cc_result_t (*chat_stream)(
        void *self,
        const cc_llm_chat_request_t *request,
        cc_llm_stream_callback_fn on_chunk,
        void *user_data
    );



    /**
     * @brief 销毁拥有的提供者实例及其内部资源。
     * @param[in] self 消费对应构造函数交出的实例，NULL 支持由具体实现约定。
     * @note 拥有者先关闭请求入口，等待已接纳调用和回调退出，再调用一次。
     * @post 实现内部资源已释放，旧状态别名失效，外部接口描述符由调用者清理。
     */
    void (*destroy)(void *self);



    /**
     * @brief 查询当前实例支持的输入输出、工具和流式能力。
     * @param[in] self 有效实例借用。
     * @param[out] out_capabilities 当前完整布局的输出槽，mime_types 继续借用实例元数据。
     * @return 成功或实现错误，调用者清理结果诊断，能力结构本身无动态释放责任。
     */
    cc_result_t (*capabilities)(
        void *self,
        cc_llm_provider_capabilities_t *out_capabilities
    );
};

#endif
