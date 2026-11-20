



#ifndef CC_RESULT_H
#define CC_RESULT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

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
 * 调用方创建该结构时使用 {0} 初始化并设置 size。当前接口按完整的当前布局访问字段。
 * 清理时由结构体拥有字符串，构造 result 时深拷贝输入字符串，双方分别管理自己的资源。
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

/**
 * @brief 构造无动态成员的成功结果。
 * @return 按值返回 CC_OK、当前 size 及空动态成员，可传给 cc_result_free。
 */
cc_result_t cc_result_ok(void);

/**
 * @brief 构造保存指定 code 的结果，并复制可选描述。
 * @param[in] code 要保存的状态码。
 * @param[in] message 调用期间借用的描述，允许 NULL。
 * @return 动态成员由调用者 cc_result_free，描述复制失败时 message 为空、code 保持。
 */
cc_result_t cc_result_error(cc_error_code_t code, const char *message);

/**
 * @brief 按 printf 格式构造拥有的错误描述。
 * @param[in] code 要保存的状态码。
 * @param[in] fmt 同步借用的格式串，允许 NULL。
 * @param[in] ... 与格式串匹配的参数，引用数据须覆盖两次格式化。
 * @return 按值返回结果，由调用者 cc_result_free。格式化或分配失败时 message 为空，
 *         code 保持输入值。
 */
cc_result_t cc_result_errf(cc_error_code_t code, const char *fmt, ...);

/**
 * @brief 深拷贝描述与可选的结构化错误详情。
 * @param[in] code 输入状态码，详情分配失败时返回 OUT_OF_MEMORY。
 * @param[in] message 调用期间借用的可选描述。
 * @param[in] detail 调用期间借用的完整当前布局详情，允许 NULL。
 * @return 拥有动态成员的按值结果，由调用者 cc_result_free，输入资源仍归调用者。
 *         详情复制失败时回收本次已分配资源，描述复制失败沿用普通错误构造的语义。
 */
cc_result_t cc_result_with_detail(
    cc_error_code_t code,
    const char *message,
    const cc_error_detail_t *detail
);

/**
 * @brief 释放结果独占的动态成员。
 * @param[in,out] result 已初始化且独占的结果，允许 NULL。
 * @post message 和 detail 置空，code、size 和结构体存储保留，可再次清理。
 * @note 按值复制包含动态成员的结果时，调用者须明确交接唯一释放责任。
 */
void cc_result_free(cc_result_t *result);

/**
 * @brief 释放错误详情拥有的三个字符串。
 * @param[in,out] detail 已初始化且独占的详情，允许 NULL，字符串须可 free。
 * @post 非空详情全部归零，结构体存储继续由调用者管理。
 */
void cc_error_detail_cleanup(cc_error_detail_t *detail);

/**
 * @brief 查询用于日志和展示的英文错误描述。
 * @param[in] code 状态码，允许未知枚举值。
 * @return 进程期有效的只读静态文本，未知值返回 Unknown。
 */
const char *cc_error_string(cc_error_code_t code);

/**
 * @brief 查询错误码的机器可读符号名。
 * @param[in] code 状态码，允许未知枚举值。
 * @return 进程期有效的只读静态文本，未知值返回 CC_ERR_UNKNOWN。
 */
const char *cc_error_code_name(cc_error_code_t code);

#ifdef __cplusplus
}
#endif

#endif
