



#ifndef CC_STREAM_CHUNK_H
#define CC_STREAM_CHUNK_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_llm_usage.h"

/*
 * 模型提供者流式输出的数据块类型。
 *
 * 流式回调承担实时输出，事件总线承担观测。TEXT/THINKING 表示模型
 * 文本增量，TOOL_* 表示工具调用构造过程，USAGE 是用量元数据，
 * FINISHED 表示完整结束，ERROR 表示流中断或提供者错误。调用方不要把每个数据块
 * 都当作最终可落库内容。
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
    CC_STREAM_CHUNK_ERROR = 8,
    CC_STREAM_CHUNK_USAGE = 9
} cc_stream_chunk_type_t;

/*
 * 一次流式回调的只读数据。
 *
 * chunk 指针只在回调期间有效，content/tool_name/tool_id 由 SDK 或提供者适配器拥有，
 * 回调异步使用前须自行复制。该结构由提供者串行交付，接收方须自行同步异步副本。
 * 取消和执行预算由运行时限制及取消令牌处理。
 */
typedef struct cc_stream_chunk {
    cc_stream_chunk_type_t type;
    char *content;
    char *tool_name;
    char *tool_id;
    /* 提供者工具调用或内容块索引，无相关索引时使用 -1。 */
    int tool_index;
    /* 用量元数据只在 USAGE 数据块有意义，一次请求可以多次报告累计值。
     * valid 表示整体用量报告可用，prompt/completion 的零值仍可能代表未提供。
     * 两项缓存计数分别用 valid/reported 区分可信值、缺失和已报告但无效。 */
    cc_llm_usage_t usage;
} cc_stream_chunk_t;

/**
 * @brief 同步接收提供者的文本、工具、用量、告警或终止数据块。
 * @param[in] chunk 只读借用数据及嵌套字符串，仅回调期间有效。
 * @param[in,out] user_data 注册方拥有的上下文，提供者原样借用。
 * @return 成功继续流处理，错误传给提供者以结束当前处理，诊断拥有权一并转移。
 * @note 用户自定义回调须遵守提供者的串行与重入契约，销毁前等待回调退出。
 */
typedef cc_result_t (*cc_llm_stream_callback_fn)(
    const cc_stream_chunk_t *chunk,
    void *user_data
);

#endif
