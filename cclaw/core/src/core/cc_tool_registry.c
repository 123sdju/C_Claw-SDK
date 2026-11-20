



#include "cc/ports/cc_tool_registry.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_json_syntax_check.h"
#include "cc/ports/cc_thread.h"
#include <stdlib.h>
#include <string.h>

#define MAX_TOOLS 64 /**< 单个注册表可接管的工具条目上限。 */

/*
 * 注册表拥有成功注册的工具状态，固定数组与能力位通过同一 mutex 发布。
 * name/description/schema/destroy 回调在锁内执行，不得重入本注册表。
 * 销毁前由 owner 关闭入口并排空全部 API 及 find 返回的借用工具。
 */
struct cc_tool_registry {
    cc_tool_t tools[MAX_TOOLS];
    unsigned capabilities[MAX_TOOLS];
    size_t count;
    int frozen;
    cc_mutex_t mutex;
};

/**
 * @brief 分配空注册表并建立互斥锁，成功后发布句柄。
 * @param[out] out_registry 必填空槽，先置 NULL，成功交付须 destroy 的拥有实例。
 * @return 参数、分配或 mutex 创建结果，失败归还部分实例，诊断由调用者释放。
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

/**
 * @brief 在锁内销毁各个非空拥有状态，再释放 mutex 和注册表。
 * @param[in,out] registry 消费的独占实例，可为 NULL。
 * @pre owner 已关闭入口并等待 API 和所有浅借用工具调用退出，destroy 回调不得重入本注册表。
 * @post 注册表及其拥有工具已释放，所有旧句柄、工具借用失效。
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

/**
 * @brief 以零能力标记注册工具，委托带能力入口完成准入和交接。
 * @param[in,out] registry 必填存活注册表借用。
 * @param[in] tool 按值传入的拥有描述符，仅成功时 self 交注册表，vtable 覆盖实例寿命。
 * @return 带能力注册结果，任何拒绝均保持原集合及输入拥有权，诊断由调用者释放。
 */
cc_result_t cc_tool_registry_add(
    cc_tool_registry_t *registry,
    cc_tool_t tool
)
{
    return cc_tool_registry_add_with_capabilities(registry, tool, 0);
}

/**
 * @brief 锁内拒绝冻结、重复拥有状态或同名工具，再原子发布工具及能力。
 * @param[in,out] registry 必填存活注册表。
 * @param[in] tool 成功时交接 self，失败由调用者继续持有，元数据回调返回稳定文本且不得重入本表。
 * @param[in] capabilities 可信注册方声明的能力位，只允许 READ_ONLY 或零。
 * @return 参数、重复或冻结为 INVALID_ARGUMENT，满六十四项沿既有 OOM，成功为 CC_OK，诊断交调用者。
 * @note 无名称或缺少方法的描述符仍可注册，其可执行性由使用者检查，重复名称按字节比较。
 */
cc_result_t cc_tool_registry_add_with_capabilities(
    cc_tool_registry_t *registry, cc_tool_t tool, unsigned capabilities)
{
    if (!registry) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null registry");
    if (capabilities & ~CC_TOOL_CAPABILITY_READ_ONLY)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Unknown tool capability");
    cc_mutex_lock(registry->mutex);
    if (registry->frozen) {
        cc_mutex_unlock(registry->mutex);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Tool registry is frozen");
    }
    const char *name = tool.vtable && tool.vtable->name ? tool.vtable->name(tool.self) : NULL;
    for (size_t i = 0; i < registry->count; i++) {
        cc_tool_t *existing = &registry->tools[i];
        if (tool.self && existing->self == tool.self) {
            cc_mutex_unlock(registry->mutex);
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Tool state is already owned by registry");
        }
        const char *existing_name = existing->vtable && existing->vtable->name ?
            existing->vtable->name(existing->self) : NULL;
        if (name && existing_name && strcmp(name, existing_name) == 0) {
            cc_mutex_unlock(registry->mutex);
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Tool name is already registered");
        }
    }
    if (registry->count >= MAX_TOOLS) {
        cc_mutex_unlock(registry->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Tool registry full");
    }
    registry->tools[registry->count] = tool;
    registry->capabilities[registry->count++] = capabilities;
    cc_mutex_unlock(registry->mutex);
    return cc_result_ok();
}

/**
 * @brief 锁内永久关闭新增准入，可重复调用。
 * @param[in,out] registry 必填存活注册表借用。
 * @return NULL 为 INVALID_ARGUMENT，其他为 CC_OK，诊断交调用者。
 */
cc_result_t cc_tool_registry_freeze(cc_tool_registry_t *registry)
{
    if (!registry) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null registry");
    cc_mutex_lock(registry->mutex);
    registry->frozen = 1;
    cc_mutex_unlock(registry->mutex);
    return cc_result_ok();
}

/** @brief 锁内快照冻结标志。 @param[in] registry 可空存活借用。 @return NULL 为零，其他为当前标志，后续注册仍以 add 结果为准。 */
int cc_tool_registry_is_frozen(cc_tool_registry_t *registry)
{
    if (!registry) return 0;
    cc_mutex_lock(registry->mutex);
    int frozen = registry->frozen;
    cc_mutex_unlock(registry->mutex);
    return frozen;
}

/**
 * @brief 委托带能力查找取得工具浅借用。
 * @param[in] registry 必填存活注册表。
 * @param[in] name 调用期稳定名称，按字节匹配。
 * @param[out] out_tool 必填借用槽，仅成功时覆盖，失败保持原值。
 * @return 参数或 NOT_FOUND 错误，成功为 CC_OK，诊断交调用者；输出使用期由 owner 保持注册表存活。
 */
cc_result_t cc_tool_registry_find(
    cc_tool_registry_t *registry,
    const char *name,
    cc_tool_t *out_tool
)
{
    return cc_tool_registry_find_with_capabilities(registry, name, out_tool, NULL);
}

/**
 * @brief 在同一锁内按名称取得描述符和对应可信能力。
 * @param[in] registry 必填存活注册表，元数据回调在锁内执行。
 * @param[in] name 必填调用期借用名称。
 * @param[out] out_tool 必填浅借用槽，仅成功覆盖，self 仍归注册表。
 * @param[out] out_capabilities 可选值槽，存在时先置零，成功写匹配条目能力。
 * @return 参数错误、NOT_FOUND 或 CC_OK，拥有诊断交调用者。
 */
cc_result_t cc_tool_registry_find_with_capabilities(
    cc_tool_registry_t *registry, const char *name, cc_tool_t *out_tool, unsigned *out_capabilities)
{
    if (out_capabilities) *out_capabilities = 0;
    if (!registry || !name || !out_tool) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null argument");
    cc_mutex_lock(registry->mutex);
    for (size_t i = 0; i < registry->count; i++) {
        cc_tool_t *tool = &registry->tools[i];


        if (tool->vtable && tool->vtable->name) {
            const char *tool_name = tool->vtable->name(tool->self);
            if (tool_name && strcmp(tool_name, name) == 0) {
                *out_tool = *tool;
                if (out_capabilities) *out_capabilities = registry->capabilities[i];
                cc_mutex_unlock(registry->mutex);
                return cc_result_ok();
            }
        }
    }
    cc_mutex_unlock(registry->mutex);
    return cc_result_errf(CC_ERR_NOT_FOUND, "Tool not found: %s", name);
}

/**
 * @brief 锁内读取工具元数据，以拥有 JSON 节点构造完整 function 数组。
 * @param[in] registry 必填存活注册表，回调须同步返回稳定文本且不得重入本表。
 * @param[out] out_tools_json 必填空槽，基础参数齐全后置 NULL，成功文本由调用者 free。
 * @return 参数、schema JSON 或 OOM 错误，失败清理全部部分树并保持空输出，诊断交调用者。
 * @note 缺少 schema 回调或回调返回 NULL 的条目跳过，缺少名称沿既有 unknown。
 * schema 须完整对象且无解码 NUL，name/description 通过 JSON 字符串节点转义。
 */
cc_result_t cc_tool_registry_build_schema_json(
    cc_tool_registry_t *registry,
    char **out_tools_json
)
{
    if (!registry || !out_tools_json) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null registry");
    *out_tools_json = NULL;

    cc_json_value_t *array = cc_json_create_array();
    if (!array) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create tool schema array");
    cc_json_value_t *parameters = NULL;
    cc_json_value_t *function = NULL;
    cc_json_value_t *entry = NULL;
    cc_result_t rc = cc_result_ok();
    cc_mutex_lock(registry->mutex);
    for (size_t i = 0; i < registry->count; i++) {
        cc_tool_t *tool = &registry->tools[i];


        if (!tool->vtable || !tool->vtable->schema_json) continue;

        const char *schema = tool->vtable->schema_json(tool->self);
        if (!schema) continue;

        const char *name = tool->vtable->name ? tool->vtable->name(tool->self) : NULL;
        const char *desc = tool->vtable->description ? tool->vtable->description(tool->self) : NULL;

        if (cc_storage_json_has_nul_escape(schema)) {
            rc = cc_result_error(CC_ERR_JSON, "Tool schema contains a decoded NUL");
            goto error_locked;
        }
        rc = cc_json_parse_strict(schema, &parameters);
        if (rc.code != CC_OK) goto error_locked;
        if (!cc_json_is_object(parameters)) {
            rc = cc_result_error(CC_ERR_JSON, "Tool schema must be an object");
            goto error_locked;
        }
        function = cc_json_create_object();
        entry = cc_json_create_object();
        if (!function || !entry ||
            !cc_json_object_set_owned(function, "name", cc_json_create_string(name ? name : "unknown")) ||
            (desc && !cc_json_object_set_owned(function, "description", cc_json_create_string(desc)))) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create tool schema fields");
            goto error_locked;
        }
        int attached = cc_json_object_set_owned(function, "parameters", parameters);
        parameters = NULL;
        if (!attached || !cc_json_object_set_owned(entry, "type", cc_json_create_string("function"))) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to attach tool parameters");
            goto error_locked;
        }
        attached = cc_json_object_set_owned(entry, "function", function);
        function = NULL;
        if (!attached) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to attach tool function");
            goto error_locked;
        }
        attached = cc_json_array_append_owned(array, entry);
        entry = NULL;
        if (!attached) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to append tool schema");
            goto error_locked;
        }
    }
    cc_mutex_unlock(registry->mutex);
    cc_result_free(&rc);
    *out_tools_json = cc_json_stringify_unformatted(array);
    cc_json_destroy(array);
    return *out_tools_json ? cc_result_ok()
                           : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build tool schema JSON");

error_locked:
    cc_mutex_unlock(registry->mutex);
    cc_json_destroy(parameters);
    cc_json_destroy(function);
    cc_json_destroy(entry);
    cc_json_destroy(array);
    return rc;
}

/** @brief 锁内快照条目数量。 @param[in] registry 可空存活借用。 @return NULL 为零，其他为当前条目数，包含无名称或无 schema 的条目。 */
size_t cc_tool_registry_count(cc_tool_registry_t *registry)
{
    if (!registry) return 0;
    cc_mutex_lock(registry->mutex);
    size_t count = registry->count;
    cc_mutex_unlock(registry->mutex);
    return count;
}

/**
 * @brief 锁内按注册顺序复制全部名称，输出脱离注册表寿命。
 * @param[in] registry 必填存活注册表，名称回调不得重入本表。
 * @param[out] out_names 必填空槽，基础参数齐全后置 NULL，成功数组及非空元素由调用者逐项 free。
 * @param[out] out_count 必填值槽，基础参数齐全后先置零，成功写注册条目数。
 * @return 参数或 OOM 错误，失败回收部分名称并保持空输出，成功为 CC_OK，诊断交调用者。
 * @note 零条目输出 NULL/零，无名称回调或返回 NULL 的对应元素为空。
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
