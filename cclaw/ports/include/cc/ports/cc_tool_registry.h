



#ifndef CC_TOOL_REGISTRY_H
#define CC_TOOL_REGISTRY_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_tool.h"

/*
 * tool registry 的不透明句柄。
 *
 * registry 持有注册进去的 cc_tool_t self 所有权；销毁 registry 时会调用工具 vtable
 * destroy。内部带 mutex，支持多个线程查找/列举，但注册阶段通常在 runtime 启动时完成。
 */
typedef struct cc_tool_registry cc_tool_registry_t;

/* 创建空 registry；成功后调用方用 cc_tool_registry_destroy() 释放。 */
cc_result_t cc_tool_registry_create(cc_tool_registry_t **out_registry);

/* 销毁 registry，并销毁其中持有的工具实例。 */
void cc_tool_registry_destroy(cc_tool_registry_t *registry);

/*
 * 注册工具。
 *
 * tool 按值复制到 registry，tool.self 的生命周期转移给 registry。freeze 后不允许继续
 * add，避免 runtime 运行中工具集合变化导致 provider schema 和 executor 不一致。
 */
cc_result_t cc_tool_registry_add(
    cc_tool_registry_t *registry,
    cc_tool_t tool
);

/* 冻结 registry；之后只能查找和列举，不能注册新工具。 */
cc_result_t cc_tool_registry_freeze(cc_tool_registry_t *registry);

/* 查询 registry 是否冻结；NULL registry 返回 0。 */
int cc_tool_registry_is_frozen(cc_tool_registry_t *registry);

/*
 * 按工具名查找工具。
 *
 * out_tool 得到的是 cc_tool_t 的浅拷贝，registry 仍然拥有 self；调用方不要 destroy
 * out_tool.self，只在当前 registry 生命周期内使用。
 */
cc_result_t cc_tool_registry_find(
    cc_tool_registry_t *registry,
    const char *name,
    cc_tool_t *out_tool
);

/*
 * 构造 provider tools schema JSON。
 *
 * 遍历所有提供 schema_json() 的工具，拼成 function tool 数组。返回字符串由调用方
 * free()，常用于 LLM request 构建阶段。
 */
cc_result_t cc_tool_registry_build_schema_json(
    cc_tool_registry_t *registry,
    char **out_tools_json
);

/* 返回已注册工具数量；线程安全读取，NULL registry 返回 0。 */
size_t cc_tool_registry_count(cc_tool_registry_t *registry);

/*
 * 列出工具名。
 *
 * 返回 names 数组和其中字符串都由调用方释放；数组元素可能为 NULL，表示对应工具
 * 没有 name vtable 或复制失败。
 */
cc_result_t cc_tool_registry_list_names(
    cc_tool_registry_t *registry,
    char ***out_names,
    size_t *out_count
);

#endif
