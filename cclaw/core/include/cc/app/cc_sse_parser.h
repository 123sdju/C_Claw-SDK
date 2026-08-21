

#ifndef CC_SSE_PARSER_H
#define CC_SSE_PARSER_H

#include "cc/core/cc_result.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SSE parser 不透明句柄；内部缓存跨 chunk 的半行数据。 */
typedef struct cc_sse_parser cc_sse_parser_t;


/*
 * SSE data event 回调。
 *
 * data 只在回调期间有效；返回非 OK 会停止解析并把错误传给 feed/finish 调用方。
 */
typedef cc_result_t (*cc_sse_event_fn)(
    const char *data,
    void *user_data
);

/* 创建 SSE parser；成功后调用方用 cc_sse_parser_destroy()。 */
cc_result_t cc_sse_parser_create(cc_sse_parser_t **out_parser);

/* 销毁 parser 和内部缓存；允许 NULL。 */
void cc_sse_parser_destroy(cc_sse_parser_t *parser);


/*
 * 向 parser 输入一段字节。
 *
 * data 可以不是 NUL 结尾，len 指定有效长度。parser 会按 SSE 行协议累积 data: 字段，
 * 每完成一个事件就调用 on_event。
 */
cc_result_t cc_sse_parser_feed(
    cc_sse_parser_t *parser,
    const char *data,
    size_t len,
    cc_sse_event_fn on_event,
    void *user_data
);


/* 结束输入并冲刷最后一个未以空行结束的事件。 */
cc_result_t cc_sse_parser_finish(
    cc_sse_parser_t *parser,
    cc_sse_event_fn on_event,
    void *user_data
);

#ifdef __cplusplus
}
#endif

#endif
