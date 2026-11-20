#include "cc/app/cc_mcp_runtime_manager.h"
#include <stddef.h>

/**
 * @brief 核对完整对象、唯一协议字段及保留类型的 ID 匹配，消费每次拥有诊断。
 * @return 数字、字符串、缺失和非法输入全部符合预期为零，否则一。
 * @note 本入口只验证对象与 ID 归属，具体响应信封由真实工具调用回归覆盖。
 */
int main(void)
{
    const char *request = "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\"}";
    struct { const char *response; int valid; int matches; } cases[] = {
        {"{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{}}", 1, 1},
        {"{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":{}}", 1, 0},
        {"{\"jsonrpc\":\"2.0\",\"id\":\"7\",\"result\":{}}", 1, 0},
        {"{\"jsonrpc\":\"2.0\",\"result\":{}}", 1, 0},
        {"{\"id\":7,\"id\":8,\"result\":{}}", 0, 0},
        {"{\"id\":7,\"result\":{}} trailing", 0, 0},
        {"{\"id\":true,\"result\":{}}", 0, 0},
        {"{\"id\":\"x\\u0000tail\",\"result\":{}}", 0, 0},
        {"[]", 0, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int matches = 77;
        cc_result_t rc = cc_mcp_jsonrpc_response_matches_request(request, cases[i].response, &matches);
        int passed = (rc.code == CC_OK) == cases[i].valid && matches == cases[i].matches;
        cc_result_free(&rc);
        if (!passed) return 1;
    }
    int matches = 0;
    cc_result_t rc = cc_mcp_jsonrpc_response_matches_request(
        "{\"id\":\"abc\",\"method\":\"tools/list\"}", "{\"id\":\"abc\",\"result\":{}}", &matches);
    int passed = rc.code == CC_OK && matches;
    cc_result_free(&rc);
    if (!passed) return 1;
    return 0;
}
