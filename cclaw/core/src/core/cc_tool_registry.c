



#include "cc/ports/cc_tool_registry.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_thread.h"
#include "cc/internal/cc_alloc.h"
#include <stdlib.h>
#include <string.h>

#define MAX_TOOLS 64

/*
 * 工具注册表内部结构。
 *
 * 使用固定上限数组是有意的：工具集合通常在 runtime 启动时确定，固定容量比动态链表
 * 更容易控制内存上限。mutex 保护注册、冻结、查找和列举，freeze 用来锁定启动后的
 * 工具集合。
 */
struct cc_tool_registry {
    cc_tool_t tools[MAX_TOOLS];
    size_t count;
    int frozen;
    cc_mutex_t mutex;
};

/*
 * 创建空工具注册表。
 *
 * registry 初始化后由调用方持有；mutex 创建失败时释放 registry，避免返回半可用对象。
 */
cc_result_t cc_tool_registry_create(cc_tool_registry_t **out_registry)
{
    if (!out_registry) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null registry output");
    *out_registry = NULL;
    cc_tool_registry_t *registry = calloc(1, sizeof(cc_tool_registry_t));
    if (!registry) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create tool registry");
    registry->count = 0;
    cc_result_t rc = cc_mutex_create(&registry->mutex);
    if (rc.code != CC_OK) {
        free(registry);
        return rc;
    }
    *out_registry = registry;
    return cc_result_ok();
}

/*
 * 销毁工具注册表。
 *
 * registry 拥有 tool.self；销毁时按 vtable destroy 释放具体工具实例。这里在持锁状态
 * 下遍历工具，保证没有其他线程同时读取工具数组；destroy 回调应避免再次访问同一 registry。
 */
void cc_tool_registry_destroy(cc_tool_registry_t *registry)
{
    if (!registry) return;
    cc_mutex_lock(registry->mutex);
    for (size_t i = 0; i < registry->count; i++) {
        cc_tool_t *tool = &registry->tools[i];
        if (tool->vtable && tool->vtable->destroy && tool->self) {
            tool->vtable->destroy(tool->self);
        }
    }
    cc_mutex_unlock(registry->mutex);
    cc_mutex_destroy(registry->mutex);
    free(registry);
}

/*
 * 注册一个工具。
 *
 * tool 按值复制，self 所有权随之交给 registry。冻结后的 registry 拒绝新增工具，避免
 * LLM 已看到的 schema 和 executor 可执行工具集合不一致。
 */
cc_result_t cc_tool_registry_add(
    cc_tool_registry_t *registry,
    cc_tool_t tool
)
{
    if (!registry) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null registry");
    cc_mutex_lock(registry->mutex);
    if (registry->frozen) {
        cc_mutex_unlock(registry->mutex);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Tool registry is frozen");
    }
    if (registry->count >= MAX_TOOLS) {
        cc_mutex_unlock(registry->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Tool registry full");
    }
    registry->tools[registry->count++] = tool;
    cc_mutex_unlock(registry->mutex);
    return cc_result_ok();
}

/*
 * 冻结工具注册表。
 *
 * freeze 是启动阶段和运行阶段的边界：启动阶段允许装配工具，运行阶段只允许查找。
 * 这是 C 里常见的“构造完成后只读”并发简化模式。
 */
cc_result_t cc_tool_registry_freeze(cc_tool_registry_t *registry)
{
    if (!registry) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null registry");
    cc_mutex_lock(registry->mutex);
    registry->frozen = 1;
    cc_mutex_unlock(registry->mutex);
    return cc_result_ok();
}

/*
 * 查询工具注册表是否冻结。
 *
 * 返回值在调用瞬间有效；如果另一个线程随后 freeze，调用方仍需按 add 的返回码处理。
 */
int cc_tool_registry_is_frozen(cc_tool_registry_t *registry)
{
    if (!registry) return 0;
    cc_mutex_lock(registry->mutex);
    int frozen = registry->frozen;
    cc_mutex_unlock(registry->mutex);
    return frozen;
}

/*
 * 按名称查找工具。
 *
 * 返回 cc_tool_t 的浅拷贝，self 仍归 registry 所有。调用方不能销毁 self，只能在 registry
 * 生命周期内使用它调用 execute/schema 等 vtable。
 */
cc_result_t cc_tool_registry_find(
    cc_tool_registry_t *registry,
    const char *name,
    cc_tool_t *out_tool
)
{
    if (!registry || !name || !out_tool) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null argument");
    cc_mutex_lock(registry->mutex);
    for (size_t i = 0; i < registry->count; i++) {
        cc_tool_t *tool = &registry->tools[i];


        if (tool->vtable && tool->vtable->name) {
            const char *tool_name = tool->vtable->name(tool->self);
            if (tool_name && strcmp(tool_name, name) == 0) {
                *out_tool = *tool;
                cc_mutex_unlock(registry->mutex);
                return cc_result_ok();
            }
        }
    }
    cc_mutex_unlock(registry->mutex);
    return cc_result_errf(CC_ERR_NOT_FOUND, "Tool not found: %s", name);
}

/*
 * 构造 provider 可消费的 tools schema JSON。
 *
 * 当前 registry 只拼接有 schema_json() 的工具。函数在持锁期间读取 vtable 字段和 schema，
 * 因此工具的 schema_json() 不应阻塞或回调 registry。返回字符串由调用方 free()。
 */
cc_result_t cc_tool_registry_build_schema_json(
    cc_tool_registry_t *registry,
    char **out_tools_json
)
{
    if (!registry || !out_tools_json) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null registry");
    *out_tools_json = NULL;

    cc_string_builder_t sb;
    cc_result_t rc = cc_string_builder_init(&sb);
    if (rc.code != CC_OK) return rc;
    rc = cc_string_builder_append(&sb, "[");
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&sb);
        return rc;
    }

    int first = 1;
    cc_mutex_lock(registry->mutex);
    for (size_t i = 0; i < registry->count; i++) {
        cc_tool_t *tool = &registry->tools[i];


        if (!tool->vtable || !tool->vtable->schema_json) continue;

        const char *schema = tool->vtable->schema_json(tool->self);
        if (!schema) continue;

        const char *name = tool->vtable->name ? tool->vtable->name(tool->self) : NULL;
        const char *desc = tool->vtable->description ? tool->vtable->description(tool->self) : NULL;

        if (!first) rc = cc_string_builder_append(&sb, ",");
        if (rc.code != CC_OK) break;
        first = 0;


        rc = cc_string_builder_append(&sb, "{");
        if (rc.code == CC_OK) rc = cc_string_builder_appendf(&sb, "\"type\":\"function\",");
        if (rc.code == CC_OK) rc = cc_string_builder_appendf(&sb, "\"function\":{");
        if (rc.code == CC_OK) rc = cc_string_builder_appendf(&sb, "\"name\":\"%s\"", name ? name : "unknown");
        if (rc.code == CC_OK && desc) rc = cc_string_builder_appendf(&sb, ",\"description\":\"%s\"", desc);
        if (rc.code == CC_OK) rc = cc_string_builder_appendf(&sb, ",\"parameters\":%s", schema);
        if (rc.code == CC_OK) rc = cc_string_builder_append(&sb, "}}");
        if (rc.code != CC_OK) break;
    }
    cc_mutex_unlock(registry->mutex);

    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&sb);
        return rc;
    }
    rc = cc_string_builder_append(&sb, "]");
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&sb);
        return rc;
    }



    *out_tools_json = cc_string_builder_take(&sb);
    return *out_tools_json ? cc_result_ok()
                           : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build tool schema JSON");
}

/*
 * 返回已注册工具数量。
 *
 * count 在锁内读取，避免调用方看到 torn value；这是一个瞬时值，不代表后续 add/freeze
 * 不会改变状态。
 */
size_t cc_tool_registry_count(cc_tool_registry_t *registry)
{
    if (!registry) return 0;
    cc_mutex_lock(registry->mutex);
    size_t count = registry->count;
    cc_mutex_unlock(registry->mutex);
    return count;
}

/*
 * 复制工具名列表。
 *
 * 返回数组和字符串由调用方释放。函数在锁内读取 vtable name，但复制结果脱离 registry；
 * 如果复制任一名称失败，会释放已复制项并返回 OOM。
 */
cc_result_t cc_tool_registry_list_names(
    cc_tool_registry_t *registry,
    char ***out_names,
    size_t *out_count
)
{
    if (!registry || !out_names || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null registry");
    }
    *out_names = NULL;
    *out_count = 0;

    cc_mutex_lock(registry->mutex);
    size_t count = registry->count;

    if (count == 0) {
        cc_mutex_unlock(registry->mutex);
        return cc_result_ok();
    }


    char **names = calloc(count, sizeof(char *));
    if (!names) {
        cc_mutex_unlock(registry->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate names");
    }

    for (size_t i = 0; i < count; i++) {
        cc_tool_t *tool = &registry->tools[i];

        if (tool->vtable && tool->vtable->name) {
            const char *name = tool->vtable->name(tool->self);
            names[i] = name ? cc_copy_string(name) : NULL;
            if (name && !names[i]) {
                for (size_t j = 0; j < i; j++) free(names[j]);
                free(names);
                cc_mutex_unlock(registry->mutex);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy tool name");
            }
        }
    }
    cc_mutex_unlock(registry->mutex);

    *out_names = names;
    *out_count = count;
    return cc_result_ok();
}
