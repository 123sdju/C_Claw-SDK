



#include "cc/app/cc_sse_parser.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CC_SSE_MAX_LINE_BYTES (16U * 1024U)
#define CC_SSE_MAX_EVENT_BYTES (64U * 1024U)

/* 单一拥有者串行维护半行、事件和跨块 CRLF 状态。缓冲容量保留到销毁。 */
struct cc_sse_parser {
    char *line;
    size_t line_len;
    size_t line_cap;
    char *event_data;
    size_t event_len;
    size_t event_cap;
    unsigned char first_line;
    unsigned char skip_lf;
    unsigned char has_data;
};

/**
 * @brief 扩容后向私有字节缓冲追加数据并维护字符串终止符。
 * @param[in,out] buffer 拥有的缓冲指针，扩容成功后替换原分配。
 * @param[in,out] len 已使用字节数，不含终止符。
 * @param[in,out] cap 当前分配容量，包含终止符位置。
 * @param[in] data 本次借用的 n 字节，必须与目标分配互不重叠。
 * @param[in] n 追加字节数，零表示成功且不改变缓冲。
 * @return 成功或参数、长度溢出、分配错误。失败保留原缓冲和已使用长度。
 */
static cc_result_t append_bytes(char **buffer, size_t *len, size_t *cap, const char *data, size_t n)
{
    if (n == 0) return cc_result_ok();
    if (!buffer || !len || !cap || !data) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid SSE parser buffer");
    }
    if (*len == SIZE_MAX || n > SIZE_MAX - *len - 1U) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "SSE parser buffer size overflow");
    }
    size_t required = *len + n + 1U;
    if (required > *cap) {
        size_t next_cap = *cap ? *cap : 64;
        while (next_cap < required) {
            if (next_cap > SIZE_MAX / 2U) {
                return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "SSE parser buffer size overflow");
            }
            next_cap *= 2U;
        }
        char *next = realloc(*buffer, next_cap);
        if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow SSE parser buffer");
        *buffer = next;
        *cap = next_cap;
    }
    memcpy(*buffer + *len, data, n);
    *len += n;
    (*buffer)[*len] = '\0';
    return cc_result_ok();
}

/**
 * @brief 通过共用扩容逻辑向私有缓冲追加一个字节。
 * @param[in,out] buffer 拥有的缓冲地址。
 * @param[in,out] len 已使用字节数。
 * @param[in,out] cap 分配容量。
 * @param[in] ch 追加字节。
 * @return 透传追加结果，失败保留原缓冲。
 */
static cc_result_t append_char(char **buffer, size_t *len, size_t *cap, char ch)
{
    return append_bytes(buffer, len, cap, &ch, 1);
}

/**
 * @brief 投递包含 data 字段的事件并消费其累积内容。
 * @param[in,out] parser 独占的有效解析器，空 data 字段也构成一个事件。
 * @param[in] on_event 同步回调，有待投递事件时必须有效。
 * @param[in,out] user_data 原样借给回调的上下文。
 * @return 无事件时成功，有事件时透传回调结果，缺少回调返回参数错误。
 * @note 回调返回后清空事件，即使回调失败也不会再次投递。同一解析器禁止回调重入或销毁。
 */
static cc_result_t flush_event(
    cc_sse_parser_t *parser,
    cc_sse_event_fn on_event,
    void *user_data
)
{
    if (!parser->has_data) return cc_result_ok();
    if (!on_event) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "SSE event callback is required");



    cc_result_t rc = on_event(parser->event_data ? parser->event_data : "", user_data);
    parser->event_len = 0;
    parser->has_data = 0;
    if (parser->event_data) parser->event_data[0] = '\0';
    return rc;
}

/**
 * @brief 解释一条已去除行终止符的 SSE 行并累积 data 字段。
 * @param[in,out] parser 独占状态，首次调用消费流首 BOM 标志。
 * @param[in] line 借用的完整行字节，允许空行。
 * @param[in] len 行字节数，包含可能存在的首次 UTF-8 BOM。
 * @param[in] on_event 空行遇待投递数据时调用的同步回调。
 * @param[in,out] user_data 借给回调的上下文。
 * @return 成功、事件超限、扩容错误或回调结果。失败后流拥有者应销毁解析器。
 * @note 字段名区分大小写，data 和 data: 都是空值字段，只剥除值前一个空格。
 *       注释和其他字段忽略，多条 data 之间保留一个 LF。
 */
static cc_result_t process_line(
    cc_sse_parser_t *parser,
    const char *line,
    size_t len,
    cc_sse_event_fn on_event,
    void *user_data
)
{
    if (parser->first_line) {
        parser->first_line = 0;
        if (len >= 3 && memcmp(line, "\xef\xbb\xbf", 3) == 0) {
            line += 3;
            len -= 3;
        }
    }
    if (len == 0) {
        return flush_event(parser, on_event, user_data);
    }
    if (line[0] == ':') {
        return cc_result_ok();
    }
    if ((len == 4 && memcmp(line, "data", 4) == 0) ||
        (len >= 5 && memcmp(line, "data:", 5) == 0)) {
        size_t prefix_len = len == 4 ? 4U : 5U;
        const char *value = line + prefix_len;
        size_t value_len = len - prefix_len;
        if (value_len > 0 && *value == ' ') {
            value++;
            value_len--;
        }
        if (parser->has_data) {
            if (parser->event_len >= CC_SSE_MAX_EVENT_BYTES) {
                return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "SSE event exceeds 64 KiB limit");
            }
            cc_result_t rc = append_char(
                &parser->event_data, &parser->event_len, &parser->event_cap, '\n');
            if (rc.code != CC_OK) return rc;
        }
        if (value_len > CC_SSE_MAX_EVENT_BYTES - parser->event_len) {
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "SSE event exceeds 64 KiB limit");
        }
        cc_result_t rc = append_bytes(&parser->event_data, &parser->event_len, &parser->event_cap,
                                      value, value_len);
        if (rc.code == CC_OK) parser->has_data = 1;
        return rc;
    }
    return cc_result_ok();
}

/**
 * @brief 创建用于一条模型响应字节流的空解析器。
 * @param[out] out_parser 必需的输出地址，先置 NULL，成功后转移拥有权。
 * @return 成功、输出参数错误或分配错误，拥有者最终调用 destroy。
 */
cc_result_t cc_sse_parser_create(cc_sse_parser_t **out_parser)
{
    if (!out_parser) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null SSE parser output");
    *out_parser = NULL;
    cc_sse_parser_t *parser = calloc(1, sizeof(*parser));
    if (!parser) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create SSE parser");
    parser->first_line = 1;
    *out_parser = parser;
    return cc_result_ok();
}

/**
 * @brief 释放解析器及其半行、事件缓冲，放弃所有未投递内容。
 * @param[in] parser 转入待销毁对象，允许 NULL。
 * @note 拥有者先停止 feed/finish 和同步回调，再销毁。函数返回后禁止使用旧句柄。
 */
void cc_sse_parser_destroy(cc_sse_parser_t *parser)
{
    if (!parser) return;
    free(parser->line);
    free(parser->event_data);
    free(parser);
}

/**
 * @brief 逐字节处理一个输入块并保留跨块半行及 CRLF 状态。
 * @param[in,out] parser 独占的有效解析器。
 * @param[in] data 借用的 UTF-8 输入，len 为零时允许 NULL，不要求尾部 NUL。
 * @param[in] len 有效字节数，块内 NUL 在消费本块前整体拒绝。
 * @param[in] on_event 同步事件回调，存在 data 事件时必须有效。
 * @param[in,out] user_data 原样借给回调，解析器不保存或释放它。
 * @return 成功、参数、行/事件超限、分配错误或回调结果。
 * @note 支持 LF、CR 和跨块 CRLF。错误可发生于已投递前序事件之后，接口不报告消费
 *       偏移，调用方应终止该流并销毁解析器，避免重放整个输入块。
 */
cc_result_t cc_sse_parser_feed(
    cc_sse_parser_t *parser,
    const char *data,
    size_t len,
    cc_sse_event_fn on_event,
    void *user_data
)
{
    if (!parser || (!data && len > 0)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid SSE parser feed");
    }
    if (len > 0 && memchr(data, '\0', len)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "SSE input contains NUL byte");
    }

    for (size_t i = 0; i < len; i++) {
        if (parser->skip_lf) {
            parser->skip_lf = 0;
            if (data[i] == '\n') continue;
        }
        if (data[i] == '\n' || data[i] == '\r') {
            parser->skip_lf = data[i] == '\r';
            cc_result_t rc = process_line(
                parser, parser->line ? parser->line : "", parser->line_len,
                on_event, user_data);
            parser->line_len = 0;
            if (parser->line) parser->line[0] = '\0';
            if (rc.code != CC_OK) return rc;
            cc_result_free(&rc);
        } else {
            if (parser->line_len >= CC_SSE_MAX_LINE_BYTES) {
                return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "SSE line exceeds 16 KiB limit");
            }
            cc_result_t rc = append_char(&parser->line, &parser->line_len, &parser->line_cap, data[i]);
            if (rc.code != CC_OK) return rc;
        }
    }
    return cc_result_ok();
}

/**
 * @brief 正常结束模型输入，按 SDK 既有规则提交尾部半行和待投递事件。
 * @param[in,out] parser 独占的有效解析器。
 * @param[in] on_event 有待投递 data 时必须提供的同步回调。
 * @param[in,out] user_data 借给回调的上下文。
 * @return 成功或尾部处理、回调错误，无待处理数据时重复调用成功。
 * @note 该显式尾部提交是 SDK 扩展，WHATWG EventSource 在 EOF 丢弃未完成事件。
 *       调用方只在传输正常完成时调用，异常、取消或解析错误应直接销毁。
 */
cc_result_t cc_sse_parser_finish(
    cc_sse_parser_t *parser,
    cc_sse_event_fn on_event,
    void *user_data
)
{
    if (!parser) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null SSE parser");
    if (parser->line_len > 0) {
        cc_result_t rc = process_line(
            parser, parser->line, parser->line_len, on_event, user_data);
        parser->line_len = 0;
        if (parser->line) parser->line[0] = '\0';
        if (rc.code != CC_OK) return rc;
        cc_result_free(&rc);
    }
    return flush_event(parser, on_event, user_data);
}
