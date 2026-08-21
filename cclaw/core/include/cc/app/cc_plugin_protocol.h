

#ifndef CC_PLUGIN_PROTOCOL_H
#define CC_PLUGIN_PROTOCOL_H

#include "cc/core/cc_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 构造 plugin JSON-RPC 请求。
 *
 * method/params_json 只在调用期间借用；返回字符串由调用方 free()。该 helper 只处理协议
 * 包装，不负责启动进程或业务 plugin 生命周期。
 */
char *cc_plugin_protocol_build_request(
    const char *method,
    const char *params_json
);

/*
 * 解析 plugin JSON-RPC 响应。
 *
 * result_json/error_json 成功后由调用方通过 cc_plugin_protocol_free_response() 释放；
 * 如果响应包含错误，out_error_json 会保存错误对象字符串，方便上层转成可观测事件。
 */
cc_result_t cc_plugin_protocol_parse_response(
    const char *response_json,
    char **out_result_json,
    char **out_error_json
);

/* 释放 parse_response 返回的 result/error 字符串；允许任一参数为 NULL。 */
void cc_plugin_protocol_free_response(
    char *result_json,
    char *error_json
);

#ifdef __cplusplus
}
#endif

#endif
