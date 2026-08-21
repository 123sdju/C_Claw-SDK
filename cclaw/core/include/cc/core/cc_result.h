



#ifndef CC_RESULT_H
#define CC_RESULT_H

#include <stddef.h>


/*
 * SDK 统一错误码。
 *
 * 这些错误码是跨模块、跨平台的稳定分类；调用方应优先根据 code 做控制流判断，
 * 再把 message 用于日志或用户提示。嵌入式场景中建议避免解析 message 文本。
 */
typedef enum cc_error_code {
    CC_OK = 0,


    CC_ERR_UNKNOWN,
    CC_ERR_INVALID_ARGUMENT,
    CC_ERR_INVALID_STATE,

    CC_ERR_OUT_OF_MEMORY,
    CC_ERR_NOT_FOUND,


    CC_ERR_PERMISSION_DENIED,


    CC_ERR_IO,
    CC_ERR_NETWORK,
    CC_ERR_JSON,
    CC_ERR_TIMEOUT,
    CC_ERR_RATE_LIMIT,


    CC_ERR_CANCELLED,
    CC_ERR_MODEL,
    CC_ERR_TOOL,
    CC_ERR_STORAGE,
    CC_ERR_UNSUPPORTED,
    CC_ERR_PLATFORM,
    CC_ERR_LIMIT_EXCEEDED,
    CC_ERR_QUEUE_FULL
} cc_error_code_t;

/*
 * 结构化错误细节。
 *
 * size 用于后续 ABI 扩展；调用方创建该结构时应使用 {0} 初始化并设置 size。
 * 字符串字段由结构体拥有，复制到 cc_result_t 后由 cc_result_free() 释放。
 * HTTP provider 会用 http_status、retry_after_ms、recoverable 和 raw_redacted_body
 * 向上层暴露恢复语义，但 SDK 不在内部自动 retry。
 */
typedef struct cc_error_detail {
    size_t size;
    long http_status;
    int retry_after_ms;
    int recoverable;
    char *error_code;
    char *provider_error_code;
    char *raw_redacted_body;
} cc_error_detail_t;

/*
 * SDK 函数统一返回值。
 *
 * code == CC_OK 表示成功；message 和 detail 仅在错误或需要补充信息时存在。
 * 调用方收到非临时结果后应调用 cc_result_free()，即使当前 message/detail 为空也安全。
 */
typedef struct cc_result {
    size_t size;
    cc_error_code_t code;
    char *message;
    cc_error_detail_t *detail;
} cc_result_t;

/* 构造成功结果；返回值仍可安全传给 cc_result_free()。 */
cc_result_t cc_result_ok(void);

/* 构造普通错误；message 会被深拷贝，调用方仍拥有传入字符串。 */
cc_result_t cc_result_error(cc_error_code_t code, const char *message);

/* 按 printf 风格构造错误 message；失败时 message 可能为空，但 code 保持传入值。 */
cc_result_t cc_result_errf(cc_error_code_t code, const char *fmt, ...);

/*
 * 构造带结构化 detail 的错误。
 *
 * detail 会被深拷贝，调用方可在返回后立即清理自己的 detail。适合 provider、HTTP、
 * storage 等需要把机器可读元数据交给上层恢复策略的场景。
 */
cc_result_t cc_result_with_detail(
    cc_error_code_t code,
    const char *message,
    const cc_error_detail_t *detail
);

/* 释放 result 内部拥有的 message/detail，并把指针字段清空；不会释放 result 本身。 */
void cc_result_free(cc_result_t *result);

/* 释放 detail 内部拥有的字符串字段；不会释放 detail 本身。 */
void cc_error_detail_cleanup(cc_error_detail_t *detail);

/* 返回适合展示的错误描述，字符串为静态常量，调用方不要 free。 */
const char *cc_error_string(cc_error_code_t code);

/* 返回稳定错误码名字，适合 observability、日志和机器解析。 */
const char *cc_error_code_name(cc_error_code_t code);

#endif
