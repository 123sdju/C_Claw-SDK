
#ifndef CC_BUILTIN_TOOLS_H
#define CC_BUILTIN_TOOLS_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_http_client.h"
#include "cc/ports/cc_tool.h"
#include "cc/util/cc_config.h"
#include "cc/util/cc_network_policy.h"

/*
 * 创建内置文件读取工具。
 *
 * fs 按值保存到工具内部，具体文件访问仍必须受 workspace path 检查约束。成功后 out_tool
 * 通常交给 registry 管理 self。
 */
cc_result_t cc_file_read_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool);

/* 创建内置文件写入工具；写入路径需要 canonical parent + workspace boundary 校验。 */
cc_result_t cc_file_write_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool);

/* 创建 HTTP 请求工具；默认无 allowlist 时应安全拒绝外网访问。 */
cc_result_t cc_http_request_tool_create(
    cc_http_client_t *http_client,
    cc_tool_t *out_tool);

/* 使用配置 allowlist 创建 HTTP 请求工具；allowlist 内容会被工具复制或转换为 policy。 */
cc_result_t cc_http_request_tool_create_with_allowlist(
    cc_http_client_t *http_client,
    const cc_config_string_list_t *network_allowlist,
    cc_tool_t *out_tool
);

/* 使用完整 network policy 创建 HTTP 请求工具；policy 只在创建期间借用。 */
cc_result_t cc_http_request_tool_create_with_policy(
    cc_http_client_t *http_client,
    const cc_network_policy_t *network_policy,
    cc_tool_t *out_tool
);

/* 检查 URL 是否被 allowlist 允许；供工具和测试复用。 */
int cc_http_request_url_allowed(
    const cc_config_string_list_t *network_allowlist,
    const char *url
);

#endif
