



#ifndef CC_STRING_BUILDER_H
#define CC_STRING_BUILDER_H

#include "cc/core/cc_result.h"
#include <stddef.h>

/*
 * 可增长字符串构造器。
 *
 * data 由 builder 拥有，length 不含 NUL，capacity 是已分配容量。该结构不加锁，只能由
 * 一个线程写；用于 JSON/prompt/摘要等需要多次 append 的路径。
 */
typedef struct cc_string_builder {
    char *data;

    size_t length;
    size_t capacity;
    /* 首次失败后保持错误，防止忽略 append 返回值的调用方拿到半截 JSON/协议数据。 */
    cc_error_code_t error_code;

} cc_string_builder_t;

/* 初始化 builder；成功后必须 deinit 或 take。 */
cc_result_t cc_string_builder_init(cc_string_builder_t *sb);

/* 追加 NUL 结尾字符串；NULL 可由实现按空串处理。 */
cc_result_t cc_string_builder_append(cc_string_builder_t *sb, const char *text);

/* printf 风格追加；失败返回 OOM 或格式化错误。 */
cc_result_t cc_string_builder_appendf(cc_string_builder_t *sb, const char *fmt, ...);

/* 追加单个字符。 */
cc_result_t cc_string_builder_append_char(cc_string_builder_t *sb, char c);

/* 取走内部字符串所有权；builder 被重置为空，返回值由调用方 free()。 */
char *cc_string_builder_take(cc_string_builder_t *sb);

/* 释放 builder 内部缓冲并清零；不释放 sb 指针。 */
void cc_string_builder_deinit(cc_string_builder_t *sb);

/* 清空内容但保留已分配缓冲，适合循环复用。 */
void cc_string_builder_clear(cc_string_builder_t *sb);

/* 返回当前 NUL 结尾字符串；指针由 builder 拥有。 */
const char *cc_string_builder_cstr(const cc_string_builder_t *sb);

/* 返回 sticky error；CC_OK 表示此前所有操作成功。 */
cc_error_code_t cc_string_builder_error(const cc_string_builder_t *sb);

#endif
