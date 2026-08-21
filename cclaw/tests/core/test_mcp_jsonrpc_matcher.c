#include "cc/app/cc_mcp_runtime_manager.h"

/*
 * 验证 MCP JSON-RPC response id 匹配。
 *
 * 覆盖数字 id 相等、数字 id 不相等和字符串 id 相等，确保 runtime manager 不会把响应
 * 误归属给错误请求。
 */
int main(void)
{
    int matches = 0;
    if (cc_mcp_jsonrpc_response_matches_request(
            "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\"}",
            "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{}}",
            &matches).code != CC_OK || !matches) {
        return 1;
    }
    if (cc_mcp_jsonrpc_response_matches_request(
            "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\"}",
            "{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":{}}",
            &matches).code != CC_OK || matches) {
        return 1;
    }
    if (cc_mcp_jsonrpc_response_matches_request(
            "{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":\"tools/list\"}",
            "{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"result\":{}}",
            &matches).code != CC_OK || !matches) {
        return 1;
    }
    return 0;
}
