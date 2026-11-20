

#ifndef CC_SSE_PARSER_H
#define CC_SSE_PARSER_H

#include "cc/core/cc_result.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一条输入流的独占解析器，保存跨块半行、CRLF 和当前 data 事件。 */
typedef struct cc_sse_parser cc_sse_parser_t;


/**
 * @brief 同步消费一个 SSE data 事件，空 data 字段产生空字符串事件。
 * @param[in] data 仅回调期间有效的 NUL 结尾借用文本，异步使用须自行复制。
 * @param[in,out] user_data feed/finish 调用者拥有的借用上下文。
 * @return 成功继续处理，其他结果转交 feed/finish 调用者，当前事件随后被消费。
 * @note 回调运行期间禁止对同一解析器重入 feed/finish 或调用 destroy。
 */
typedef cc_result_t (*cc_sse_event_fn)(
    const char *data,
    void *user_data
);

/**
 * @brief 创建空解析器并将其拥有权交给调用者。
 * @param[out] out_parser 必需的空输出槽，失败置 NULL。
 * @return 成功、参数错误或分配失败，调用者最终负责 destroy。
 */
cc_result_t cc_sse_parser_create(cc_sse_parser_t **out_parser);

/**
 * @brief 释放解析器、半行和事件缓冲，放弃未提交内容。
 * @param[in] parser 被消费的拥有指针，允许 NULL。
 * @note 拥有者先关闭输入并等待全部调用和回调退出，再销毁对象。
 */
void cc_sse_parser_destroy(cc_sse_parser_t *parser);


/**
 * @brief 累积 UTF-8 字节并同步投递完整的 data 事件。
 * @param[in,out] parser 独占的有效解析器，跨调用保留半行和事件。
 * @param[in] data 借用的 len 字节，零长度允许 NULL，无需尾部终止符。
 * @param[in] len 本块字节数，块内 NUL 会在消费本块前返回参数错误。
 * @param[in] on_event 有待投递事件时必需的同步回调。
 * @param[in,out] user_data 原样传给回调的借用上下文，允许 NULL。
 * @return 成功、参数/容量/分配错误或回调结果，调用者负责释放结果诊断。
 * @note 支持 LF、CR、跨块 CRLF、流首 BOM 及空 data 字段。行最多 16 KiB，
 *       事件载荷最多 64 KiB，多行之间的 LF 计入载荷。只解释 data 字段。
 *       UTF-8 合法性由输入方保证。接口不报告错误消费偏移，失败后应终止流并销毁。
 *       同一对象的所有 API 和回调由拥有者串行化。
 */
cc_result_t cc_sse_parser_feed(
    cc_sse_parser_t *parser,
    const char *data,
    size_t len,
    cc_sse_event_fn on_event,
    void *user_data
);


/**
 * @brief 在正常传输结束时显式提交尾部半行和最后一个 data 事件。
 * @param[in,out] parser 独占的有效解析器。
 * @param[in] on_event 有待投递事件时必需的同步回调。
 * @param[in,out] user_data 借给回调的上下文，允许 NULL。
 * @return 成功或尾部处理/回调错误，重复调用且无新输入时成功且不重投。
 * @note 尾部提交沿用 SDK 语义，WHATWG EventSource 的 EOF 会丢弃未完成事件。
 *       传输失败、取消或解析失败应直接 destroy，禁止用 finish 提交损坏尾部。
 */
cc_result_t cc_sse_parser_finish(
    cc_sse_parser_t *parser,
    cc_sse_event_fn on_event,
    void *user_data
);

#ifdef __cplusplus
}
#endif

#endif
