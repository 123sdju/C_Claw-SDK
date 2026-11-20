#include "cc/util/cc_tool_name.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 将一个组件编码后的字节数加入分配预算，逐步检查加法溢出。
 * @param[in] component 必填借用 C 字符串，点号和百分号各计三字节，其他各计一字节。
 * @param[in,out] length 必填累计预算，包含调用者已预留的分隔符和 NUL，失败时可已部分增加。
 * @return 完整加入为一，尺寸溢出为零，调用者在失败时丢弃本地预算。
 */
static int add_component_length(const char *component, size_t *length)
{
    for (const char *cursor = component; *cursor; cursor++) {
        size_t encoded_length = (*cursor == '.' || *cursor == '%') ? 3U : 1U;
        if (*length > SIZE_MAX - encoded_length) return 0;
        *length += encoded_length;
    }
    return 1;
}

/**
 * @brief 把一个组件编码写入已核算容量的输出，不写结尾 NUL。
 * @pre 调用者已用 add_component_length 为完整组件预留空间。
 * @param[out] output 借用可写缓冲区，至少容纳组件编码正文。
 * @param[in] component 借用输入 C 字符串，允许空串，与输出不重叠。
 * @return 编码正文后的下一个可写位置，仍借用同一输出缓冲区。
 */
static char *append_component(char *output, const char *component)
{
    for (const char *cursor = component; *cursor; cursor++) {
        if (*cursor == '.' || *cursor == '%') {
            memcpy(output, *cursor == '.' ? "%2E" : "%25", 3U);
            output += 3U;
        } else {
            *output++ = *cursor;
        }
    }
    return output;
}

/**
 * @brief 核算完整编码长度后一次分配并拼接 namespace、来源及可选工具。
 * @param[in] namespace_name 非空借用结构前缀，点号或百分号视为前缀格式错误。
 * @param[in] source 必填借用来源，允许空串，输入保持到本次返回。
 * @param[in] tool 可选借用工具，NULL 表示只构建 lane，非 NULL 允许空串。
 * @param[out] out_name 必填，先置 NULL，成功写调用者需 free 的完整字符串。
 * @return CC_OK 或参数、尺寸溢出、分配错误，失败不交付部分输出。
 */
cc_result_t cc_tool_name_build(const char *namespace_name, const char *source,
                               const char *tool, char **out_name)
{
    if (!out_name) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool name output");
    *out_name = NULL;
    if (!namespace_name || !namespace_name[0] || strpbrk(namespace_name, ".%") || !source) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid tool name components");
    }
    size_t namespace_length = strlen(namespace_name);
    if (namespace_length > SIZE_MAX - 2U) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Tool name size overflow");
    }
    size_t length = namespace_length + 2U;
    if (!add_component_length(source, &length)) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Tool name size overflow");
    }
    if (tool) {
        if (length == SIZE_MAX) return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Tool name size overflow");
        length++;
        if (!add_component_length(tool, &length)) {
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Tool name size overflow");
        }
    }
    char *name = malloc(length);
    if (!name) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate tool name");
    memcpy(name, namespace_name, namespace_length);
    char *output = name + namespace_length;
    *output++ = '.';
    output = append_component(output, source);
    if (tool) {
        *output++ = '.';
        output = append_component(output, tool);
    }
    *output = '\0';
    *out_name = name;
    return cc_result_ok();
}

/**
 * @brief 按编码组件边界复制扩展 lane，或为普通工具添加 tool. 前缀。
 * @param[in] qualified 借用公开名称，NULL/空串采用 unknown，扩展组件遵循 build 编码。
 * @param[out] out_lane 必填，先置 NULL，成功写需 free 的完整拥有字符串。
 * @return CC_OK 或参数、长度溢出、分配错误，成功仅为输出分配一次内存。
 */
cc_result_t cc_tool_name_lane(const char *qualified, char **out_lane)
{
    if (!out_lane) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool lane output");
    *out_lane = NULL;
    const char *name = qualified && qualified[0] ? qualified : "unknown";
    size_t prefix_length = 0;
    if (strncmp(name, "mcp.", 4U) == 0) prefix_length = 4U;
    else if (strncmp(name, "plugin.", 7U) == 0) prefix_length = 7U;
    if (prefix_length) {
        const char *separator = strchr(name + prefix_length, '.');
        size_t length = separator ? (size_t)(separator - name) : strlen(name);
        if (length == SIZE_MAX) return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Tool lane size overflow");
        char *lane = malloc(length + 1U);
        if (!lane) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate tool lane");
        memcpy(lane, name, length);
        lane[length] = '\0';
        *out_lane = lane;
        return cc_result_ok();
    }
    size_t length = strlen(name);
    if (length > SIZE_MAX - 6U) return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Tool lane size overflow");
    char *lane = malloc(length + 6U);
    if (!lane) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate tool lane");
    memcpy(lane, "tool.", 5U);
    memcpy(lane + 5U, name, length + 1U);
    *out_lane = lane;
    return cc_result_ok();
}
