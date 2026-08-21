

#include "cc/app/cc_plugin_protocol.h"

#include <stdlib.h>
#include <string.h>

/*
 * 验证 plugin JSON-RPC envelope helper。
 *
 * 覆盖请求构造不带换行、params 作为 JSON object 嵌入，以及响应 result/error 两条解析路径
 * 的输出所有权。
 */
int main(void)
{
    char *request = cc_plugin_protocol_build_request("weather.query", "{\"city\":\"Beijing\"}");
    if (!request) return 1;
    int request_ok = strstr(request, "\"jsonrpc\":\"2.0\"") &&
        strstr(request, "\"method\":\"weather.query\"") &&
        strstr(request, "\"params\":{\"city\":\"Beijing\"}") &&
        !strchr(request, '\n');
    free(request);
    if (!request_ok) return 1;

    char *result = NULL;
    char *error = NULL;
    cc_result_t rc = cc_plugin_protocol_parse_response(
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":{\"ok\":true}}",
        &result,
        &error
    );
    int result_ok = rc.code == CC_OK && result && strstr(result, "\"ok\"") && !error;
    cc_result_free(&rc);
    cc_plugin_protocol_free_response(result, error);
    if (!result_ok) return 1;

    result = NULL;
    error = NULL;
    rc = cc_plugin_protocol_parse_response(
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"error\":{\"message\":\"bad\"}}",
        &result,
        &error
    );
    int error_ok = rc.code == CC_OK && error && strstr(error, "\"bad\"") && !result;
    cc_result_free(&rc);
    cc_plugin_protocol_free_response(result, error);
    return error_ok ? 0 : 1;
}
