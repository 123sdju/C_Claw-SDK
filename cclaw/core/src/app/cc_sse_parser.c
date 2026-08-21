



#include "cc/app/cc_sse_parser.h"

#include <stdlib.h>
#include <string.h>

/*
 * SSE parser 内部状态。
 *
 * line 缓存跨 feed chunk 的半行；event_data 累积一个事件内的多个 data: 行。两个缓冲都
 * 按需扩容，finish/destroy 负责冲刷和释放。
 */
struct cc_sse_parser {
    char *line;
    size_t line_len;
    size_t line_cap;
    char *event_data;
    size_t event_len;
    size_t event_cap;
};

/* 向动态缓冲追加 n 字节，并保持 NUL 结尾。 */
static cc_result_t append_bytes(char **buffer, size_t *len, size_t *cap, const char *data, size_t n)
{
    if (n == 0) return cc_result_ok();
    if (*len + n + 1 > *cap) {
        size_t next_cap = *cap ? *cap : 64;
        while (next_cap < *len + n + 1) next_cap *= 2;
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

/* 追加单个字符；复用 append_bytes 保持扩容逻辑一致。 */
static cc_result_t append_char(char **buffer, size_t *len, size_t *cap, char ch)
{
    return append_bytes(buffer, len, cap, &ch, 1);
}

/*
 * 冲刷当前 SSE 事件。
 *
 * 空事件直接忽略；非空事件要求提供回调。回调成功或失败后都会清空 event_data，避免
 * 重复投递同一事件。
 */
static cc_result_t flush_event(
    cc_sse_parser_t *parser,
    cc_sse_event_fn on_event,
    void *user_data
)
{
    if (parser->event_len == 0) return cc_result_ok();
    if (!on_event) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "SSE event callback is required");



    cc_result_t rc = on_event(parser->event_data, user_data);
    parser->event_len = 0;
    if (parser->event_data) parser->event_data[0] = '\0';
    return rc;
}

/*
 * 处理一行 SSE 文本。
 *
 * 空行表示事件结束；冒号开头是注释；data: 行会追加到当前 event_data，多行 data 用
 * 换行连接。其他字段当前忽略，因为 provider 流只需要 data payload。
 */
static cc_result_t process_line(
    cc_sse_parser_t *parser,
    const char *line,
    size_t len,
    cc_sse_event_fn on_event,
    void *user_data
)
{
    while (len > 0 && line[len - 1] == '\r') len--;
    if (len == 0) {
        return flush_event(parser, on_event, user_data);
    }
    if (line[0] == ':') {
        return cc_result_ok();
    }
    if (len >= 5 && strncmp(line, "data:", 5) == 0) {
        const char *value = line + 5;
        size_t value_len = len - 5;
        if (value_len > 0 && *value == ' ') {
            value++;
            value_len--;
        }
        if (parser->event_len > 0) {
            cc_result_t rc = append_char(
                &parser->event_data, &parser->event_len, &parser->event_cap, '\n');
            if (rc.code != CC_OK) return rc;
        }
        return append_bytes(&parser->event_data, &parser->event_len, &parser->event_cap,
                            value, value_len);
    }
    return cc_result_ok();
}

/* 创建空 SSE parser。 */
cc_result_t cc_sse_parser_create(cc_sse_parser_t **out_parser)
{
    if (!out_parser) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null SSE parser output");
    cc_sse_parser_t *parser = calloc(1, sizeof(*parser));
    if (!parser) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create SSE parser");
    *out_parser = parser;
    return cc_result_ok();
}

/* 销毁 parser 和内部缓冲。 */
void cc_sse_parser_destroy(cc_sse_parser_t *parser)
{
    if (!parser) return;
    free(parser->line);
    free(parser->event_data);
    free(parser);
}

/*
 * 输入一段 SSE 字节流。
 *
 * data 不要求 NUL 结尾，parser 会按 '\n' 切行并保留最后半行。任何回调错误或 OOM 都会
 * 立即返回，调用方可停止 HTTP 流。
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

    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            cc_result_t rc = process_line(
                parser, parser->line ? parser->line : "", parser->line_len,
                on_event, user_data);
            parser->line_len = 0;
            if (parser->line) parser->line[0] = '\0';
            if (rc.code != CC_OK) return rc;
        } else {
            cc_result_t rc = append_char(&parser->line, &parser->line_len, &parser->line_cap, data[i]);
            if (rc.code != CC_OK) return rc;
        }
    }
    return cc_result_ok();
}

/*
 * 完成 SSE 输入。
 *
 * 如果还有未处理半行，先按一行处理，再冲刷最后事件。用于 HTTP 流正常结束或测试 fixture。
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
    }
    return flush_event(parser, on_event, user_data);
}
