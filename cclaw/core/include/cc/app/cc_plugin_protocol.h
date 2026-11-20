

#ifndef CC_PLUGIN_PROTOCOL_H
#define CC_PLUGIN_PROTOCOL_H

#include "cc/core/cc_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 构造 jsonrpc 为 2.0、id 固定为字符串 1 的紧凑请求信封。
 * @param[in] method 调用期间借用的方法字符串，NULL 使用空串。
 * @param[in] params_json 调用期间借用的完整 JSON 对象或数组，NULL 使用空对象。
 * @return 成功返回独立拥有的 NUL 结尾 JSON，调用者 free；格式、类型、解码 NUL 或分配失败返回 NULL。
 * @note 参数经完整尾部及 Unicode 转义检查，JSON 词法兼容范围沿 cc_json_parse_strict。
 * 同一传输中由外层为固定 ID 请求协调顺序和响应归属，进程及传输寿命也由外层拥有。
 */
char *cc_plugin_protocol_build_request(
    const char *method,
    const char *params_json
);

/**
 * @brief 完整解析固定 ID 响应并交付唯一的结果或业务错误 JSON。
 * @param[in] response_json 调用期间借用的 NUL 结尾响应对象，不能为空，拒绝解码 NUL。
 * @param[out] out_result_json 必填空输出槽，必须与错误槽不同；可访问的槽先置 NULL，成功可获得拥有结果字符串。
 * @param[out] out_error_json 必填空输出槽；成功可获得拥有错误节点字符串，result/error 仅一个非 NULL。
 * @return 信封解包成功返回 CC_OK，参数错误返回 INVALID_ARGUMENT，完整解析或信封错误返回 JSON，输出序列化失败返回 OOM。
 * 失败不交付字符串，两个输出均为 NULL；返回结果的诊断由调用者 cc_result_free。
 * @note jsonrpc 必须为 2.0，id 必须为字符串 1，四个已知信封字段区分大小写且各自唯一，result/error 恰有一个。
 * error 的业务 schema 继续交上层处理。CC_OK 表示解包完成，收到 error 时由调用者处理业务失败。
 * 输入和输出独立拥有，成功输出通过 cc_plugin_protocol_free_response 释放；实现无共享状态，可对独立槽并发调用。
 */
cc_result_t cc_plugin_protocol_parse_response(
    const char *response_json,
    char **out_result_json,
    char **out_error_json
);

/**
 * @brief 消费解包获得的独立结果与错误字符串。
 * @param[in,out] result_json 可为 NULL 的拥有结果指针。
 * @param[in,out] error_json 可为 NULL 的拥有错误指针，非 NULL 时须与结果指针不同。
 * @post 非 NULL 指针对应分配被 free，原借用结束；调用者保管并清空自身槽值。
 */
void cc_plugin_protocol_free_response(
    char *result_json,
    char *error_json
);

#ifdef __cplusplus
}
#endif

#endif
