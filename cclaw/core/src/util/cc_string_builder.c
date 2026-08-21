



#include "cc/util/cc_string_builder.h"
#include "cc/internal/cc_alloc.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>


#define SB_INITIAL_CAPACITY 256


/*
 * 初始化 string builder。
 *
 * 初始分配 256 字节，覆盖大多数短 JSON/prompt 片段，后续按需扩容。调用方必须在成功后
 * deinit 或 take，避免缓冲泄漏。
 */
cc_result_t cc_string_builder_init(cc_string_builder_t *sb)
{
    if (!sb) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null string builder");
    memset(sb, 0, sizeof(*sb));
    sb->data = malloc(SB_INITIAL_CAPACITY);
    if (!sb->data) {
        sb->error_code = CC_ERR_OUT_OF_MEMORY;
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to init string builder");
    }
    sb->data[0] = '\0';
    sb->length = 0;
    sb->capacity = SB_INITIAL_CAPACITY;
    return cc_result_ok();
}


/*
 * 确保追加 additional 字节后仍有 NUL 结尾空间。
 *
 * 扩容采用倍增策略，减少多次 append 时 realloc 次数；失败时原缓冲保持有效，调用方可
 * 继续 deinit 做清理。
 */
static cc_result_t ensure_capacity(cc_string_builder_t *sb, size_t additional)
{
    if (!sb || !sb->data) {
        return cc_result_error(CC_ERR_INVALID_STATE, "String builder is not initialized");
    }
    if (sb->error_code != CC_OK) {
        return cc_result_error(sb->error_code, "String builder is in a failed state");
    }
    if (additional > SIZE_MAX - sb->length - 1U) {
        sb->error_code = CC_ERR_LIMIT_EXCEEDED;
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "String builder size overflow");
    }
    size_t needed = sb->length + additional + 1U;
    if (needed <= sb->capacity) return cc_result_ok();

    size_t new_capacity = sb->capacity;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2U) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2U;
    }

    char *new_data = realloc(sb->data, new_capacity);
    if (!new_data) {
        sb->error_code = CC_ERR_OUT_OF_MEMORY;
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow string builder");
    }

    sb->data = new_data;
    sb->capacity = new_capacity;
    return cc_result_ok();
}


/*
 * 追加字符串。
 *
 * NULL 文本视为 no-op，方便调用方在可选字段拼接时少写分支。成功后 builder 始终保持
 * NUL 结尾。
 */
cc_result_t cc_string_builder_append(cc_string_builder_t *sb, const char *text)
{
    if (!sb) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null string builder");
    if (!text) return cc_result_ok();
    size_t len = strlen(text);
    cc_result_t rc = ensure_capacity(sb, len);
    if (rc.code != CC_OK) return rc;

    memcpy(sb->data + sb->length, text, len);
    sb->length += len;
    sb->data[sb->length] = '\0';
    return cc_result_ok();
}


/*
 * printf 风格追加。
 *
 * 先用 vsnprintf 计算需要长度，再确保容量并写入目标位置，避免固定缓冲截断。该函数
 * 依赖目标 C 库支持 vsnprintf(NULL, 0, ...)，嵌入式移植时要验证。
 */
cc_result_t cc_string_builder_appendf(cc_string_builder_t *sb, const char *fmt, ...)
{
    if (!sb || !fmt) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid string builder format");
    if (sb->error_code != CC_OK) {
        return cc_result_error(sb->error_code, "String builder is in a failed state");
    }
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (needed < 0) {
        sb->error_code = CC_ERR_UNKNOWN;
        return cc_result_error(CC_ERR_UNKNOWN, "Format error");
    }

    cc_result_t rc = ensure_capacity(sb, (size_t)needed);
    if (rc.code != CC_OK) {
        return rc;
    }

    va_start(args, fmt);
    int written = vsnprintf(sb->data + sb->length, sb->capacity - sb->length, fmt, args);
    va_end(args);

    if (written != needed) {
        sb->error_code = CC_ERR_UNKNOWN;
        return cc_result_error(CC_ERR_UNKNOWN, "String builder format write failed");
    }

    sb->length += (size_t)needed;
    return cc_result_ok();
}


/* 追加单个字符，并维护 NUL 结尾。 */
cc_result_t cc_string_builder_append_char(cc_string_builder_t *sb, char c)
{
    if (!sb) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null string builder");
    cc_result_t rc = ensure_capacity(sb, 1);
    if (rc.code != CC_OK) return rc;

    sb->data[sb->length++] = c;
    sb->data[sb->length] = '\0';
    return cc_result_ok();
}


/*
 * 取走内部字符串。
 *
 * 调用后 builder 不再拥有 data，调用方负责 free 返回值；builder 被置为空状态，避免
 * 后续 deinit 重复释放。
 */
char *cc_string_builder_take(cc_string_builder_t *sb)
{
    if (!sb) return NULL;
    if (sb->error_code != CC_OK) {
        free(sb->data);
        memset(sb, 0, sizeof(*sb));
        return NULL;
    }
    char *result = sb->data;
    sb->data = NULL;
    sb->length = 0;
    sb->capacity = 0;
    return result;
}


/* 释放 builder 内部缓冲并清零，适合错误路径统一调用。 */
void cc_string_builder_deinit(cc_string_builder_t *sb)
{
    if (!sb) return;
    free(sb->data);
    memset(sb, 0, sizeof(*sb));
}


/* 清空内容但保留容量，用于循环构造多个字符串时减少分配。 */
void cc_string_builder_clear(cc_string_builder_t *sb)
{
    if (!sb) return;
    sb->length = 0;
    sb->error_code = CC_OK;
    if (sb->data) sb->data[0] = '\0';
}


/* 返回当前字符串视图；builder 为空时返回静态空字符串。 */
const char *cc_string_builder_cstr(const cc_string_builder_t *sb)
{
    return sb && sb->data && sb->error_code == CC_OK ? sb->data : "";
}

cc_error_code_t cc_string_builder_error(const cc_string_builder_t *sb)
{
    return sb ? sb->error_code : CC_ERR_INVALID_ARGUMENT;
}
