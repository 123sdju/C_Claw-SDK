
#ifndef CC_BUILTIN_TOOLS_H
#define CC_BUILTIN_TOOLS_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_http_client.h"
#include "cc/ports/cc_tool.h"
#include "cc/util/cc_config.h"
#include "cc/util/cc_network_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建 file_read，调用时委托 ROOT_SCOPED 根相对读取。
 * @param[in] fs 浅拷贝端口，创建者保持后端及方法表覆盖工具调用期，不支持的端口在 call 反馈业务错误。
 * @param[out] out_tool 必填空描述符，先清零，成功拥有 self，调用者 destroy 或成功交给 registry。
 * @return CC_OK、INVALID_ARGUMENT 或 OOM，失败无新资源，拥有诊断由调用者释放。
 * @note 工具解析完整参数对象，拒绝解码 NUL 和重复已知 path，文件大小由后端限制，executor 另查输出预算。
 */
cc_result_t cc_file_read_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool);

/**
 * @brief 创建 file_write，调用时委托 ROOT_SCOPED 根相对文本写入。
 * @param[in] fs 浅拷贝端口，创建者保持 self/vtable 覆盖调用期，后端实施根约束与父目录规则。
 * @param[out] out_tool 必填空描述符，先清零，成功拥有 self，调用者 destroy 或成功交给 registry。
 * @return CC_OK、INVALID_ARGUMENT 或 OOM，失败无新资源，拥有诊断由调用者释放。
 * @note 参数 path/content 须为单个字符串字段，拒绝尾随文本、解码 NUL 和已知字段重复。
 * 普通写入失败可能已改变文件，成功后确认文本 OOM 也须按已写入处理，工具销毁前 owner 排空调用。
 */
cc_result_t cc_file_write_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool);

/**
 * @brief 创建缺省拒绝网络的 http.request 工具。
 * @param[in] http_client 必填存活端口指针，须有 vtable 和已连接地址校验能力，工具只借用。
 * @param[out] out_tool 必填空描述符，早期参数/能力/容量失败保留原值，其后先清零，成功由调用者 destroy。
 * @return policy 工厂结果，诊断由调用者释放，成功后端口寿命覆盖工具调用和销毁。
 */
cc_result_t cc_http_request_tool_create(
    cc_http_client_t *http_client,
    cc_tool_t *out_tool);

/**
 * @brief 将可选允许列表包装为网络策略并创建 http.request。
 * @param[in] http_client 必填端口借用，须支持已连接地址校验，保持至工具使用结束。
 * @param[in] network_allowlist 调用期借用的有效列表，NULL/零条缺省拒绝，非零 count 须有对应 items。
 * @param[out] out_tool 必填空描述符，输出及拥有权沿完整 policy 工厂。
 * @return 参数、能力或副本分配结果，诊断由调用者释放。
 */
cc_result_t cc_http_request_tool_create_with_allowlist(
    cc_http_client_t *http_client,
    const cc_config_string_list_t *network_allowlist,
    cc_tool_t *out_tool
);

/**
 * @brief 深拷贝网络允许列表及私网策略，并借用 HTTP 端口创建工具。
 * @param[in] http_client 必填存活端口，工具不接管其销毁责任，vtable 与能力须满足实现要求。
 * @param[in] network_policy 可选调用期策略借用，非空允许列表须有效，成功后配置可独立释放。
 * @param[out] out_tool 必填空描述符，早期参数/能力/容量拒绝保留原值，其后先清零，成功交付拥有工具。
 * @return 参数、能力或 OOM 错误，失败归还本次部分资源，诊断交调用者。
 * @note 工具的 destroy 只清理策略副本及自身，owner 先排空全部调用，再销毁工具和所借端口。
 */
cc_result_t cc_http_request_tool_create_with_policy(
    cc_http_client_t *http_client,
    const cc_network_policy_t *network_policy,
    cc_tool_t *out_tool
);

/**
 * @brief 委托公共网络策略检查 URL 是否命中允许列表。
 * @param[in] network_allowlist 稳定有效列表借用，NULL/空列表拒绝。
 * @param[in] url 调用期 NUL 结尾 URL 借用，无效或 NULL 拒绝。
 * @return 允许为一，其他为零，不分配或执行网络请求。
 */
int cc_http_request_url_allowed(
    const cc_config_string_list_t *network_allowlist,
    const char *url
);

#ifdef __cplusplus
}
#endif

#endif
