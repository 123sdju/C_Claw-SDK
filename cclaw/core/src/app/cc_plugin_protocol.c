



#include "cc/app/cc_plugin_protocol.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_json_syntax_check.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief 构造固定字符串 id 为 1 的 JSON-RPC 请求，完整嵌入对象或数组参数。
 * @param[in] method 调用期间借用的方法名，NULL 按空串处理。
 * @param[in] params_json 借用的完整 JSON 对象或数组，NULL 使用空对象，拒绝解码 NUL。
 * @return 成功返回调用者须 free 的紧凑 JSON；参数格式、类型或任何分配失败返回 NULL。
 * @note 各 JSON 节点经消费型 setter 交接，失败清理整棵临时树；同一传输的固定 ID 请求由外层串行配对。
 */
char *cc_plugin_protocol_build_request(
    const char *method,
    const char *params_json
)
{
    cc_json_value_t *req = cc_json_create_object();
    if (!req) return NULL;

    if (!cc_json_object_set_owned(req, "jsonrpc", cc_json_create_string("2.0")) ||
        !cc_json_object_set_owned(req, "id", cc_json_create_string("1")) ||
        !cc_json_object_set_owned(req, "method", cc_json_create_string(method ? method : ""))) {
        cc_json_destroy(req);
        return NULL;
    }
    cc_json_value_t *params = NULL;
    if (params_json) {
        if (cc_storage_json_has_nul_escape(params_json)) {
            cc_json_destroy(req);
            return NULL;
        }
        cc_result_t rc = cc_json_parse_strict(params_json, &params);
        if (rc.code != CC_OK || (!cc_json_is_object(params) && !cc_json_is_array(params))) {
            cc_result_free(&rc);
            cc_json_destroy(params);
            cc_json_destroy(req);
            return NULL;
        }
        cc_result_free(&rc);
    } else {
        params = cc_json_create_object();
    }
    if (!cc_json_object_set_owned(req, "params", params)) {
        cc_json_destroy(req);
        return NULL;
    }

    char *result = cc_json_stringify_unformatted(req);
    cc_json_destroy(req);
    return result;
}

/**
 * @brief 完整解析固定 ID 请求的响应，校验信封后复制唯一的 result 或 error 节点。
 * @param[in] response_json 借用的完整响应对象文本，拒绝解码 NUL。
 * @param[out] out_result_json 有效独立空输出槽，先置 NULL，成功 result 响应接收拥有 JSON 字符串。
 * @param[out] out_error_json 有效独立空输出槽，先置 NULL，成功 error 响应接收拥有 JSON 字符串。
 * @return CC_OK 表示解包完成；参数错误、JSON/信封错误或序列化 OOM 返回错误，两个输出均为 NULL。
 * @note jsonrpc/id/result/error 按精确大小写检查且各自唯一，id 必须为字符串 1，result/error 恰有一个。
 * error 节点按原 JSON 交付上层业务校验，拥有结果消息须 cc_result_free，输出须 free_response。
 */
cc_result_t cc_plugin_protocol_parse_response(
    const char *response_json,
    char **out_result_json,
    char **out_error_json
)
{
    if (out_result_json) *out_result_json = NULL;
    if (out_error_json) *out_error_json = NULL;
    if (!response_json || !out_result_json || !out_error_json || out_result_json == out_error_json) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid plugin response parse request");
    }
    if (cc_storage_json_has_nul_escape(response_json))
        return cc_result_error(CC_ERR_JSON, "Plugin response contains a decoded NUL");
    cc_json_value_t *resp = NULL;
    cc_result_t rc = cc_json_parse_strict(response_json, &resp);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    if (!cc_json_is_object(resp)) {
        cc_json_destroy(resp);
        return cc_result_error(CC_ERR_JSON, "Plugin response must be an object");
    }
    const char *const fields[] = {"jsonrpc", "id", "result", "error"};
    unsigned counts[4] = {0};
    int count = cc_json_object_size(resp);
    for (int i = 0; i < count; i++) {
        const char *key = cc_json_object_key_at(resp, i);
        for (size_t field = 0; field < 4; field++) {
            if (key && strcmp(key, fields[field]) == 0 && ++counts[field] > 1) {
                cc_json_destroy(resp);
                return cc_result_error(CC_ERR_JSON, "Plugin response has duplicate envelope fields");
            }
        }
    }
    const char *version = cc_json_string_value(cc_json_object_get_case_sensitive(resp, "jsonrpc"));
    const char *id = cc_json_string_value(cc_json_object_get_case_sensitive(resp, "id"));
    if (!version || strcmp(version, "2.0") != 0 || !id || strcmp(id, "1") != 0 ||
        (counts[2] + counts[3]) != 1) {
        cc_json_destroy(resp);
        return cc_result_error(CC_ERR_JSON, "Plugin response version, id or result/error is invalid");
    }
    cc_json_value_t *payload = cc_json_object_get_case_sensitive(resp, counts[2] ? "result" : "error");
    char *serialized = cc_json_stringify(payload);
    cc_json_destroy(resp);
    if (!serialized)
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to serialize plugin response payload");
    if (counts[2]) *out_result_json = serialized;
    else *out_error_json = serialized;
    return cc_result_ok();
}

/**
 * @brief 释放解包输出的两个独立拥有字符串。
 * @param[in,out] result_json 消费结果字符串，可为 NULL。
 * @param[in,out] error_json 消费错误字符串，可为 NULL，非 NULL 时须与结果指针不同。
 * @post 两个非 NULL 分配均已释放，所有相关借用失效，调用者槽值由调用者自行清空。
 */
void cc_plugin_protocol_free_response(char *result_json, char *error_json)
{
    free(result_json);
    free(error_json);
}
