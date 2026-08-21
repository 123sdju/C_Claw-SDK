

#define _POSIX_C_SOURCE 200809L



#include "cc/core/cc_result.h"
#include "cc/core/cc_version.h"
#include "cc/internal/cc_alloc.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/*
 * 构造成功结果。
 *
 * result 结构包含 size 字段，方便未来扩展 ABI 时让调用方和库侧判断结构版本。
 * 成功结果不分配动态内存，因此调用方即使忘记 cc_result_free() 也不会泄漏。
 */
cc_result_t cc_result_ok(void)
{
    cc_result_t result;
    memset(&result, 0, sizeof(result));
    result.size = sizeof(result);
    result.code = CC_OK;
    return result;
}

/*
 * 构造带静态错误码和可选消息的失败结果。
 *
 * message 会被 cc_copy_string()，所以调用方传入栈字符串或临时字符串都安全；返回值由
 * 调用方最终通过 cc_result_free() 释放。嵌入式移植时如果禁用 malloc，需要在
 * 这一层替换为固定缓冲或错误池策略。
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

/*
 * 构造 printf 风格的错误消息。
 *
 * 先用 vsnprintf(NULL, 0, ...) 计算长度，再分配精确大小，避免固定长度缓冲截断。
 * 这是 POSIX 风格实现；若目标 C 库不支持该语义，平台移植层需要重点验证。
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


        result.message = malloc(len + 1);
        if (result.message) {
            va_start(args, fmt);
            vsnprintf(result.message, len + 1, fmt, args);
            va_end(args);
        }


    } else {
        result.message = NULL;
    }
    return result;
}

/*
 * 复制可选字符串字段。
 *
 * 结构化错误 detail 有多个可空字符串；用这个小 helper 可以把 NULL 保持为 NULL，
 * 避免每个字段都重复写三元表达式。
 */
static char *copy_optional_string(const char *value)
{
    return value ? cc_copy_string(value) : NULL;
}

/*
 * 深拷贝结构化错误 detail。
 *
 * provider 错误详情可能带 redacted body、provider code 和 retry_after 信息。
 * 这些字段必须独立拥有内存，避免 HTTP adapter 释放临时响应缓冲后 result 中
 * 留下悬垂指针。任一字段复制失败都会回滚整个 detail。
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

/*
 * 构造带结构化 detail 的失败结果。
 *
 * SDK 不在内部自动 retry，但会把 recoverable、HTTP 状态和 provider 错误码传给
 * 下游应用。这里复用 cc_result_error() 管理 message，再深拷贝 detail，保证
 * result 的所有权完全独立。
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

/*
 * 清理栈上 detail 的动态字段。
 *
 * 该函数不释放 detail 指针本身，适合清理调用方构造在栈上的临时 detail，也被
 * cc_result_free() 用来释放堆上 detail 的内部字段。
 */
void cc_error_detail_cleanup(cc_error_detail_t *detail)
{
    if (!detail) return;
    free(detail->error_code);
    free(detail->provider_error_code);
    free(detail->raw_redacted_body);
    memset(detail, 0, sizeof(*detail));
}

/*
 * 释放 result 拥有的所有动态资源。
 *
 * cc_result_t 通常按值返回，但其中 message/detail 可能动态分配。统一提供 free
 * 函数可以避免调用方误判字段所有权；释放后字段置空，便于错误路径重复调用。
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

/*
 * 返回面向用户或日志的错误描述。
 *
 * 该字符串是静态常量，调用方不能释放。它适合简短日志，不适合作为稳定机器协议；
 * 机器可读场景应使用 cc_error_code_name()。
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

/*
 * 返回稳定的错误码符号名。
 *
 * observability、JSON 错误详情和测试断言需要不受语言环境影响的名字，因此这里
 * 输出枚举常量名而不是自然语言描述。
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

/*
 * 返回 SDK 编译时版本字符串。
 *
 * 版本信息来自 public version header，方便二进制和下游应用在日志中确认使用的
 * C-Claw 版本，不涉及动态分配或运行时状态。
 */
const char *cc_claw_version_string(void)
{
    return CC_CLAW_VERSION_STRING;
}
