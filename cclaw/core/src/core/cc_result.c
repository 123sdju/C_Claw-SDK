

#define _POSIX_C_SOURCE 200809L



#include "cc/core/cc_result.h"
#include "cc/core/cc_version.h"
#include "cc/internal/cc_alloc.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/**
 * @brief 构造无动态成员的成功结果。
 * @return 按值返回 code 为 CC_OK、size 为当前结构体大小的结果，其余字段清零。
 */
cc_result_t cc_result_ok(void)
{
    cc_result_t result;
    memset(&result, 0, sizeof(result));
    result.size = sizeof(result);
    result.code = CC_OK;
    return result;
}

/**
 * @brief 保存指定状态码，并复制可选的错误描述。
 * @param[in] code 要保留的状态码，直接按值保存。
 * @param[in] message 调用期间借用的 NUL 结尾文本，允许 NULL。
 * @return 按值返回结果，复制成功的 message 由调用者 cc_result_free。
 *         文本缺失或复制失败时 message 为 NULL，code 仍保持输入值。
 */
cc_result_t cc_result_error(cc_error_code_t code, const char *message)
{
    cc_result_t result;
    memset(&result, 0, sizeof(result));
    result.size = sizeof(result);
    result.code = code;
    result.message = message ? cc_copy_string(message) : NULL;
    return result;
}

/**
 * @brief 通过两次 vsnprintf 生成拥有独立存储的格式化描述。
 * @param[in] code 要保留的状态码。
 * @param[in] fmt 调用期间借用的 printf 格式串，允许 NULL。
 * @param[in] ... 与格式串匹配的可变参数，引用的数据须覆盖两次格式化。
 * @return 按值返回指定 code，生成的 message 由调用者 cc_result_free。
 *         NULL 格式串、分配失败、编码失败或第二次结果超出容量时 message 为 NULL。
 * @note 用 size_t 计算包含 NUL 的容量，先检查负长度，再进行分配。
 */
cc_result_t cc_result_errf(cc_error_code_t code, const char *fmt, ...)
{
    cc_result_t result;
    memset(&result, 0, sizeof(result));
    result.size = sizeof(result);
    result.code = code;
    if (fmt) {
        va_list args;


        va_start(args, fmt);
        int len = vsnprintf(NULL, 0, fmt, args);
        va_end(args);


        if (len < 0) return result;
        size_t capacity = (size_t)len + 1U;
        result.message = malloc(capacity);
        if (result.message) {
            va_start(args, fmt);
            int written = vsnprintf(result.message, capacity, fmt, args);
            va_end(args);
            if (written < 0 || (size_t)written >= capacity) {
                free(result.message);
                result.message = NULL;
            }
        }


    } else {
        result.message = NULL;
    }
    return result;
}

/**
 * @brief 复制结构化错误中的可选文本。
 * @param[in] value 调用期间借用的 NUL 结尾字符串，允许 NULL。
 * @return 返回调用者负责 free 的副本，输入 NULL 或分配失败时返回 NULL。
 */
static char *copy_optional_string(const char *value)
{
    return value ? cc_copy_string(value) : NULL;
}

/**
 * @brief 深拷贝状态、重试信息及三个可选错误文本。
 * @param[in] detail 调用期间借用的完整当前布局结构，允许 NULL。
 * @return 成功返回拥有的堆对象，调用者 cleanup 后 free，或交给 cc_result_free。
 *         NULL 输入或任一分配失败返回 NULL，本次已分配字段全部回收。
 * @note 输出 size 使用当前结构体大小，本复制函数按完整的当前布局访问输入字段。
 */
static cc_error_detail_t *cc_error_detail_dup(const cc_error_detail_t *detail)
{
    if (!detail) return NULL;
    cc_error_detail_t *copy = calloc(1, sizeof(*copy));
    if (!copy) return NULL;
    copy->size = sizeof(*copy);
    copy->http_status = detail->http_status;
    copy->retry_after_ms = detail->retry_after_ms;
    copy->recoverable = detail->recoverable;
    copy->error_code = copy_optional_string(detail->error_code);
    copy->provider_error_code = copy_optional_string(detail->provider_error_code);
    copy->raw_redacted_body = copy_optional_string(detail->raw_redacted_body);
    if ((detail->error_code && !copy->error_code) ||
        (detail->provider_error_code && !copy->provider_error_code) ||
        (detail->raw_redacted_body && !copy->raw_redacted_body)) {
        cc_error_detail_cleanup(copy);
        free(copy);
        return NULL;
    }
    return copy;
}

/**
 * @brief 复制可选描述和结构化详情，形成独立拥有资源的结果。
 * @param[in] code 保存的状态码，详情复制失败时改为 OUT_OF_MEMORY。
 * @param[in] message 调用期间借用的可选描述，允许 NULL。
 * @param[in] detail 调用期间借用的可选完整详情，允许 NULL，文本均深拷贝。
 * @return 按值返回结果，动态成员由调用者 cc_result_free。详情复制失败时清理先前
 *         描述并返回 OOM，描述自身复制失败沿用 cc_result_error 的 code 保持语义。
 */
cc_result_t cc_result_with_detail(
    cc_error_code_t code,
    const char *message,
    const cc_error_detail_t *detail
)
{
    cc_result_t result = cc_result_error(code, message);
    if (detail) {
        result.detail = cc_error_detail_dup(detail);
        if (!result.detail) {
            cc_result_free(&result);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy error detail");
        }
    }
    return result;
}

/**
 * @brief 释放错误详情拥有的字符串，并清零详情值。
 * @param[in,out] detail 由调用者独占的已初始化详情，允许 NULL。
 * @pre 非空字符串字段为该详情独占且可 free 的动态内存。
 * @post 非空详情的三个字符串被释放、全部字段归零，结构体存储继续由调用者持有。
 */
void cc_error_detail_cleanup(cc_error_detail_t *detail)
{
    if (!detail) return;
    free(detail->error_code);
    free(detail->provider_error_code);
    free(detail->raw_redacted_body);
    memset(detail, 0, sizeof(*detail));
}

/**
 * @brief 回收结果拥有的描述与详情，并置空动态成员。
 * @param[in,out] result 已初始化且由调用者独占的结果，允许 NULL。
 * @pre 动态成员仅由该结果拥有，按值复制结果后的所有权交接由调用者协调。
 * @post message 与 detail 为 NULL，code 和 size 保留，结构体本身继续有效，可重复清理。
 */
void cc_result_free(cc_result_t *result)
{
    if (!result) return;
    free(result->message);
    result->message = NULL;
    if (result->detail) {
        cc_error_detail_cleanup(result->detail);
        free(result->detail);
        result->detail = NULL;
    }
}

/**
 * @brief 查询错误码对应的英文展示文本。
 * @param[in] code 要查询的状态码，允许枚举范围外的值。
 * @return 返回进程期有效的只读静态文本，未知枚举值返回 Unknown。
 */
const char *cc_error_string(cc_error_code_t code)
{
    switch (code) {
    case CC_OK:                  return "OK";
    case CC_ERR_UNKNOWN:         return "Unknown error";
    case CC_ERR_INVALID_ARGUMENT: return "Invalid argument";
    case CC_ERR_INVALID_STATE:    return "Invalid state";
    case CC_ERR_OUT_OF_MEMORY:   return "Out of memory";
    case CC_ERR_NOT_FOUND:       return "Not found";
    case CC_ERR_PERMISSION_DENIED: return "Permission denied";
    case CC_ERR_IO:              return "I/O error";
    case CC_ERR_NETWORK:         return "Network error";
    case CC_ERR_JSON:            return "JSON error";
    case CC_ERR_TIMEOUT:         return "Timeout";
    case CC_ERR_RATE_LIMIT:      return "Rate limited";
    case CC_ERR_CANCELLED:       return "Cancelled";
    case CC_ERR_MODEL:           return "Model error";
    case CC_ERR_TOOL:            return "Tool error";
    case CC_ERR_STORAGE:         return "Storage error";
    case CC_ERR_UNSUPPORTED:     return "Unsupported capability";
    case CC_ERR_PLATFORM:        return "Platform error";
    case CC_ERR_LIMIT_EXCEEDED:  return "Limit exceeded";
    case CC_ERR_QUEUE_FULL:      return "Queue full";
    default:                     return "Unknown";
    }
}

/**
 * @brief 查询用于观测事件和机器处理的错误码符号名。
 * @param[in] code 要查询的状态码，允许枚举范围外的值。
 * @return 返回进程期有效的只读静态符号名，未知枚举值映射为 CC_ERR_UNKNOWN。
 */
const char *cc_error_code_name(cc_error_code_t code)
{
    switch (code) {
    case CC_OK:                  return "CC_OK";
    case CC_ERR_UNKNOWN:         return "CC_ERR_UNKNOWN";
    case CC_ERR_INVALID_ARGUMENT: return "CC_ERR_INVALID_ARGUMENT";
    case CC_ERR_INVALID_STATE:    return "CC_ERR_INVALID_STATE";
    case CC_ERR_OUT_OF_MEMORY:   return "CC_ERR_OUT_OF_MEMORY";
    case CC_ERR_NOT_FOUND:       return "CC_ERR_NOT_FOUND";
    case CC_ERR_PERMISSION_DENIED: return "CC_ERR_PERMISSION_DENIED";
    case CC_ERR_IO:              return "CC_ERR_IO";
    case CC_ERR_NETWORK:         return "CC_ERR_NETWORK";
    case CC_ERR_JSON:            return "CC_ERR_JSON";
    case CC_ERR_TIMEOUT:         return "CC_ERR_TIMEOUT";
    case CC_ERR_RATE_LIMIT:      return "CC_ERR_RATE_LIMIT";
    case CC_ERR_CANCELLED:       return "CC_ERR_CANCELLED";
    case CC_ERR_MODEL:           return "CC_ERR_MODEL";
    case CC_ERR_TOOL:            return "CC_ERR_TOOL";
    case CC_ERR_STORAGE:         return "CC_ERR_STORAGE";
    case CC_ERR_UNSUPPORTED:     return "CC_ERR_UNSUPPORTED";
    case CC_ERR_PLATFORM:        return "CC_ERR_PLATFORM";
    case CC_ERR_LIMIT_EXCEEDED:  return "CC_ERR_LIMIT_EXCEEDED";
    case CC_ERR_QUEUE_FULL:      return "CC_ERR_QUEUE_FULL";
    default:                     return "CC_ERR_UNKNOWN";
    }
}

/**
 * @brief 查询编译该 SDK 时使用的版本字符串。
 * @return 返回 CC_CLAW_VERSION_STRING 对应的进程期只读静态文本。
 */
const char *cc_claw_version_string(void)
{
    return CC_CLAW_VERSION_STRING;
}
