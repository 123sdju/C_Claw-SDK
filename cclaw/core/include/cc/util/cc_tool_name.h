#ifndef CC_TOOL_NAME_H
#define CC_TOOL_NAME_H

#include "cc/core/cc_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 编码来源及工具组件，构造完整公开工具名或来源 lane。
 * @param[in] namespace_name 非空结构前缀，不含点号或百分号，当前调用方使用 mcp 或 plugin。
 * @param[in] source 必填借用来源 C 字符串，允许空串，点号编码为 %2E、百分号编码为 %25。
 * @param[in] tool 可选借用工具 C 字符串，NULL 生成 namespace.source lane，非 NULL 按同规则编码，允许空串。
 * @param[out] out_name 必填，入口先置 NULL，成功交付一次精确分配的拥有字符串，调用者 free。
 * @return CC_OK 或参数错误、尺寸溢出的 CC_ERR_LIMIT_EXCEEDED、输出分配失败的 CC_ERR_OUT_OF_MEMORY。
 * @note 仅编码组件内的点号和百分号，其余字节保持原样，输入寿命覆盖本次调用，失败无半成品交付。
 */
cc_result_t cc_tool_name_build(const char *namespace_name, const char *source,
                               const char *tool, char **out_name);

/**
 * @brief 从已编码扩展工具名提取完整来源 lane，普通工具生成 tool. 加完整名称。
 * @param[in] qualified 借用公开工具名，mcp./plugin. 后第一个原始点号分隔工具，缺少该点号时保留整名。
 * NULL 或空串采用 tool.unknown，其他普通名称保持原文且不截断。
 * @param[out] out_lane 必填，入口先置 NULL，成功交付一次精确分配的拥有 lane，调用者 free。
 * @return CC_OK 或输出参数错误、尺寸溢出的 CC_ERR_LIMIT_EXCEEDED、分配失败的 CC_ERR_OUT_OF_MEMORY。
 * @note 扩展输入遵循 build 的组件编码约定，本函数按原始分隔点切分，不解码或验证百分号转义。
 */
cc_result_t cc_tool_name_lane(const char *qualified, char **out_lane);

#ifdef __cplusplus
}
#endif

#endif
