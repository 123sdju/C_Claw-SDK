



#include "cc/app/cc_plugin_protocol.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>

/*
 * 构造 plugin JSON-RPC 请求。
 *
 * 当前 id 固定为 "1"，适合一次请求一次响应的简单管道协议；复杂并发匹配由 MCP runtime
 * manager 处理。params_json 无效时退化为空 object，避免把坏参数直接拼进协议文本。
 */
char *cc_plugin_protocol_build_request(
    const char *method,
    const char *params_json
)
{
    cc_json_value_t *req = cc_json_create_object();
    if (!req) return NULL;

    cc_json_object_set(req, "jsonrpc", cc_json_create_string("2.0"));



    cc_json_object_set(req, "id", cc_json_create_string("1"));
    cc_json_object_set(req, "method", cc_json_create_string(method ? method : ""));

    if (params_json) {
        cc_json_value_t *params = NULL;
        cc_result_t rc = cc_json_parse(params_json, &params);
        if (rc.code == CC_OK && params) {
            cc_json_object_set(req, "params", params);
        } else {
            cc_json_object_set(req, "params", cc_json_create_object());
            cc_result_free(&rc);
        }
    } else {
        cc_json_object_set(req, "params", cc_json_create_object());
    }

    char *result = cc_json_stringify_unformatted(req);
    cc_json_destroy(req);
    return result;
}

/*
 * 解析 plugin JSON-RPC 响应。
 *
 * result 和 error 都序列化成独立 JSON 字符串返回给调用方，方便上层决定是继续解析工具
 * 结果还是记录错误事件。
 */
cc_result_t cc_plugin_protocol_parse_response(
    const char *response_json,
    char **out_result_json,
    char **out_error_json
)
{
    if (!response_json || !out_result_json || !out_error_json) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid plugin response parse request");
    }
    *out_result_json = NULL;
    *out_error_json = NULL;

    cc_json_value_t *resp = NULL;
    cc_result_t rc = cc_json_parse(response_json, &resp);
    if (rc.code != CC_OK) return rc;

    cc_json_value_t *result = cc_json_object_get(resp, "result");
    if (result) {
        *out_result_json = cc_json_stringify(result);
    }

    cc_json_value_t *error = cc_json_object_get(resp, "error");
    if (error) {
        *out_error_json = cc_json_stringify(error);
    }

    cc_json_destroy(resp);
    return cc_result_ok();
}

/* 释放 parse_response 分配的 result/error 字符串。 */
void cc_plugin_protocol_free_response(char *result_json, char *error_json)
{
    free(result_json);
    free(error_json);
}
