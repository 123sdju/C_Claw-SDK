



#include "cc/util/cc_string_builder.h"
#include "cc/internal/cc_alloc.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>


#define SB_INITIAL_CAPACITY 256


/**
 * @brief 清零新构造器并申请 256 字节的空字符串缓冲。
 * @pre sb 是新对象或已 deinit/take 的对象，调用者独占它，覆盖拥有缓冲的旧对象会丢失资源。
 * @param[out] sb 必填输出构造器，失败分配时保留可 deinit 的清零字段及 OOM 状态。
 * @return 成功为 CC_OK，空指针为参数错误，分配失败为 OOM，返回消息由调用者消费。
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


/**
 * @brief 检查容量算术并按倍增策略为追加内容和末尾 NUL 预留空间。
 * @param[in,out] sb 借用且独占的有效构造器，空指针或无缓冲返回状态错误。
 * @param[in] additional 待追加字节数，不包含终止 NUL。
 * @return 成功为 CC_OK，已有失败返回原错误码，溢出或分配失败记录 sticky 错误并保留旧缓冲。
 * 返回结果的动态成员由调用者消费，扩容成功可能使旧 data 借用失效。
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


/**
 * @brief 测量输入后追加完整文本，支持输入借用本构造器当前字符串的子串。
 * @param[in,out] sb 借用且独占的构造器，必须满足初始化后的字段不变量。
 * @param[in] text 调用期间借用的 NUL 结尾文本，NULL 直接成功且不清除已有 sticky 错误。
 * @return 成功为 CC_OK，空 sb 为参数错误，容量失败沿 ensure_capacity 交付拥有结果。
 * @post 成功时 length 增加输入字节数并追加 NUL，失败保留原文本，扩容可结束旧缓冲借用。
 */
cc_result_t cc_string_builder_append(cc_string_builder_t *sb, const char *text)
{
    if (!sb) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null string builder");
    if (!text) return cc_result_ok();
    size_t len = strlen(text);
    uintptr_t source_address = (uintptr_t)text;
    uintptr_t buffer_address = (uintptr_t)sb->data;
    int source_is_internal = sb->data && source_address >= buffer_address &&
        source_address - buffer_address <= sb->length;
    size_t source_offset = source_is_internal ? (size_t)(source_address - buffer_address) : 0;
    cc_result_t rc = ensure_capacity(sb, len);
    if (rc.code != CC_OK) return rc;

    if (source_is_internal) text = sb->data + source_offset;
    memcpy(sb->data + sb->length, text, len);
    sb->length += len;
    sb->data[sb->length] = '\0';
    return cc_result_ok();
}


/**
 * @brief 两次调用 vsnprintf 测量并追加格式化文本，检查实际写入长度。
 * @pre 目标 C 库支持 vsnprintf(NULL,0,...)，fmt 和参数引用的对象覆盖两次格式化及中间扩容。
 * @param[in,out] sb 借用且独占的有效构造器，已有 sticky 错误直接返回。
 * @param[in] fmt 必填格式串，遵守 printf 类型规则，其借用不能因本次扩容或写入而失效。
 * @param[in] ... 对应格式参数，指针参数须覆盖两次读取，并遵守 vsnprintf 的不重叠要求。
 * @return 成功为 CC_OK，空参数为参数错误，测量或写入异常记录 UNKNOWN，容量失败保留其错误。
 * @note 失败构造器由 clear、take 或 deinit 收尾，返回结果仍须由调用者消费。
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


/**
 * @brief 预留一个字节并追加字符，随后维护末尾 NUL。
 * @param[in,out] sb 借用且独占的有效构造器，NULL 为参数错误。
 * @param[in] c 待追加字节，NUL 也计入 length，字符串视图会在首个 NUL 处结束。
 * @return 成功为 CC_OK，未初始化、sticky 错误或容量失败返回对应拥有结果。
 */
cc_result_t cc_string_builder_append_char(cc_string_builder_t *sb, char c)
{
    if (!sb) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null string builder");
    cc_result_t rc = ensure_capacity(sb, 1);
    if (rc.code != CC_OK) return rc;

    sb->data[sb->length++] = c;
    sb->data[sb->length] = '\0';
    return cc_result_ok();
}


/**
 * @brief 转移成功构造的缓冲，失败状态则释放残留缓冲并清零。
 * @param[in,out] sb 借用且独占的构造器，NULL 直接返回 NULL。
 * @return 成功缓冲由调用者 free，NULL 表示空对象或此前失败，输出交付后 sb 不再拥有它。
 * @post 原缓冲借用交由返回值的新 owner 保管，构造器再次追加前须重新 init。
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


/**
 * @brief 释放当前缓冲并清零构造器，结束所有内部字符串借用。
 * @param[in,out] sb 可选独占构造器，须已初始化或为零状态，NULL 为空操作。
 * @post 缓冲已释放，sb 结构存储仍归调用者，可再次 init 或重复 deinit。
 */
void cc_string_builder_deinit(cc_string_builder_t *sb)
{
    if (!sb) return;
    free(sb->data);
    memset(sb, 0, sizeof(*sb));
}


/**
 * @brief 清空文本及 sticky 错误，保留已分配缓冲供下一轮构造。
 * @param[in,out] sb 可选独占构造器，NULL 为空操作。
 * @post length 为零且 error_code 为 CC_OK，现有缓冲首字节置 NUL，无缓冲对象仍须 init 后追加。
 */
void cc_string_builder_clear(cc_string_builder_t *sb)
{
    if (!sb) return;
    sb->length = 0;
    sb->error_code = CC_OK;
    if (sb->data) sb->data[0] = '\0';
}


/**
 * @brief 读取成功构造器的当前字符串视图，失败或空对象使用静态空串。
 * @param[in] sb 可选稳定借用，读取期间由 owner 排除并发修改和销毁。
 * @return 借用文本，追加扩容、clear、take 或 deinit 可结束或改变其内容，禁止 free。
 */
const char *cc_string_builder_cstr(const cc_string_builder_t *sb)
{
    return sb && sb->data && sb->error_code == CC_OK ? sb->data : "";
}

/**
 * @brief 读取当前 sticky 错误码。
 * @param[in] sb 可选稳定借用构造器，读取由 owner 与修改串行化。
 * @return NULL 为参数错误，其他返回 error_code，CC_OK 也可对应已 take 的空状态。
 */
cc_error_code_t cc_string_builder_error(const cc_string_builder_t *sb)
{
    return sb ? sb->error_code : CC_ERR_INVALID_ARGUMENT;
}
