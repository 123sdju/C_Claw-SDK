



#ifndef CC_STREAM_CHUNK_H
#define CC_STREAM_CHUNK_H

#include "cc/core/cc_result.h"

/*
 * provider streaming 输出的 chunk 类型。
 *
 * stream callback 是实时输出主通道，event bus 只负责观测。TEXT/THINKING 表示模型
 * token 增量，TOOL_* 表示工具调用构造过程，FINISHED 表示完整结束，ERROR 表示
 * 流中断或 provider 错误。调用方不要把每个 chunk 都当作最终可落库内容。
 */
typedef enum {
    CC_STREAM_CHUNK_TEXT = 0,

    CC_STREAM_CHUNK_THINKING = 1,

    CC_STREAM_CHUNK_TOOL_START = 2,

    CC_STREAM_CHUNK_TOOL_DELTA = 3,

    CC_STREAM_CHUNK_TOOL_END = 4,

    CC_STREAM_CHUNK_FINISHED = 5,
    CC_STREAM_CHUNK_ARTIFACT = 6,
    CC_STREAM_CHUNK_PROVIDER_WARNING = 7,
    CC_STREAM_CHUNK_ERROR = 8
} cc_stream_chunk_type_t;

/*
 * 一次 streaming 回调的只读数据。
 *
 * chunk 指针只在回调期间有效；content/tool_name/tool_id 的所有权属于 SDK 或 provider
 * adapter，回调若要异步使用必须自行复制。结构不承诺跨线程复用，取消和超限由
 * runtime limits/cancel token 控制。
 */
typedef struct cc_stream_chunk {
    cc_stream_chunk_type_t type;
    char *content;
    char *tool_name;
    char *tool_id;
    /* Provider tool-call/content-block index；非工具 chunk 使用 -1。 */
    int tool_index;
} cc_stream_chunk_t;

/*
 * LLM provider 的底层流式回调签名。
 *
 * provider adapter 调用该函数把 chunk 推给 runtime；runtime 再转换成稳定的
 * cc_runtime_stream_callback_fn 和 observability 事件。user_data 由注册方拥有。
 */
typedef cc_result_t (*cc_llm_stream_callback_fn)(
    const cc_stream_chunk_t *chunk,
    void *user_data
);

#endif
