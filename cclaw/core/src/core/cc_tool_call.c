#include "cc/core/cc_tool_call.h"

#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/internal/cc_json_fields.h"
#include "cc/internal/cc_json_syntax_check.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/**
 * @brief 分配调用描述并复制三个可选字段。
 * @param[in] id 可选调用期 ID 借用。
 * @param[in] name 可选调用期工具名借用。
 * @param[in] arguments_json 可选原始参数文本借用，NULL 复制 {}，空串原样保留。
 * @param[out] out_call 必填空拥有槽，先置 NULL，全部成功后发布。
 * @return 参数或 OOM，失败通过 destroy 归还字段，诊断交调用者，本入口不解析参数。
 */
cc_result_t cc_tool_call_create(
    const char *id,
    const char *name,
    const char *arguments_json,
    cc_tool_call_t **out_call
)
{
    if (!out_call) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool call output");
    }
    *out_call = NULL;
    cc_tool_call_t *call = calloc(1, sizeof(cc_tool_call_t));
    if (!call) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate tool call");
    call->id = cc_copy_string(id);
    call->name = cc_copy_string(name);
    call->arguments_json = cc_copy_string(arguments_json ? arguments_json : "{}");
    if ((id && !call->id) || (name && !call->name) || !call->arguments_json) {
        cc_tool_call_destroy(call);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy tool call");
    }
    *out_call = call;
    return cc_result_ok();
}

/**
 * @brief 释放调用的三个拥有字段并清零。
 * @param[in,out] call 可空初始化拥有记录，借用者已退出。
 * @post 字符串归还，结构体保留，可再次初始化。
 */
void cc_tool_call_cleanup(cc_tool_call_t *call)
{
    if (!call) return;
    free(call->id);
    free(call->name);
    free(call->arguments_json);
    memset(call, 0, sizeof(*call));
}

/**
 * @brief cleanup 调用字段后释放堆记录。
 * @param[in,out] call 可空拥有堆地址，借用者已退出。
 * @post 非空对象释放，原地址与字段借用失效。
 */
void cc_tool_call_destroy(cc_tool_call_t *call)
{
    if (!call) return;
    cc_tool_call_cleanup(call);
    free(call);
}

/**
 * @brief 复制三个字段，保持源 NULL 状态并支持自复制跳过。
 * @param[in] src 必填稳定源借用。
 * @param[out] dst 必填新对象或已 cleanup 槽，src==dst 时原样成功。
 * @return 参数错误保持目标，其他情况先清零并深拷贝，OOM 清理部分目标，诊断交调用者。
 */
cc_result_t cc_tool_call_copy(const cc_tool_call_t *src, cc_tool_call_t *dst)
{
    if (!src || !dst) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool call copy argument");
    }
    if (src == dst) return cc_result_ok();
    memset(dst, 0, sizeof(*dst));
    dst->id = cc_copy_string(src->id);
    dst->name = cc_copy_string(src->name);
    dst->arguments_json = cc_copy_string(src->arguments_json);
    if ((src->id && !dst->id) ||
        (src->name && !dst->name) ||
        (src->arguments_json && !dst->arguments_json)) {
        cc_tool_call_cleanup(dst);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy tool call");
    }
    return cc_result_ok();
}

/**
 * @brief 清零调用列表记录。
 * @param[out] list 可空新对象或已 cleanup 槽。
 * @post 非空目标为空列表，旧拥有资源须由调用者在本次初始化前归还。
 */
void cc_tool_call_list_init(cc_tool_call_list_t *list)
{
    if (!list) return;
    memset(list, 0, sizeof(*list));
}

/**
 * @brief 逐项清理有效调用，释放拥有数组并清零。
 * @param[in,out] list 可空初始化拥有容器，借用者已退出，非 NULL items 须含 count 个有效项。
 * @post 各字段和数组归还，NULL items 不遍历，记录可再次初始化。
 */
void cc_tool_call_list_cleanup(cc_tool_call_list_t *list)
{
    if (!list) return;
    for (size_t i = 0; list->items && i < list->count; i++) cc_tool_call_cleanup(&list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

/**
 * @brief 校验可变形态并为下一项扩容，新增槽位清零。
 * @param[in,out] list 必填独占拥有列表，数组须可 realloc。
 * @return 畸形 count/capacity/数组为 INVALID_ARGUMENT，容量溢出为 LIMIT_EXCEEDED，分配失败为 OOM，失败保留旧分配。
 * @note 从二项起步并倍增，接近 size_t 元素上界时饱和到最大可分配项数。
 */
static cc_result_t tool_call_list_reserve(cc_tool_call_list_t *list)
{
    if (list->count > list->capacity || (list->capacity && !list->items))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid mutable tool call list");
    size_t max_capacity = SIZE_MAX / sizeof(cc_tool_call_t);
    if (list->capacity > max_capacity || list->count >= max_capacity)
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Tool call list capacity overflow");
    if (list->count < list->capacity) return cc_result_ok();
    size_t next_cap = list->capacity ? list->capacity : 2;
    if (list->capacity) next_cap = list->capacity > max_capacity / 2 ? max_capacity : list->capacity * 2;
    cc_tool_call_t *next = realloc(list->items, next_cap * sizeof(cc_tool_call_t));
    if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow tool call list");
    memset(next + list->capacity, 0, (next_cap - list->capacity) * sizeof(cc_tool_call_t));
    list->items = next;
    list->capacity = next_cap;
    return cc_result_ok();
}

/**
 * @brief 先按值快照源字段指针，再预留槽位并深拷贝。
 * @param[in,out] list 必填独占拥有容器，满足可变形态。
 * @param[in] call 必填稳定源借用，可位于自身 items，字符串拥有分配独立于数组。
 * @return 参数、容量或复制结果，复制成功才增加 count，失败保留旧有效项，已扩容容量可增加，诊断交调用者。
 */
cc_result_t cc_tool_call_list_append(cc_tool_call_list_t *list, const cc_tool_call_t *call)
{
    if (!list || !call) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool call list append argument");
    }
    cc_tool_call_t source = *call;
    cc_result_t rc = tool_call_list_reserve(list);
    if (rc.code != CC_OK) return rc;
    rc = cc_tool_call_copy(&source, &list->items[list->count]);
    if (rc.code != CC_OK) return rc;
    list->count++;
    return cc_result_ok();
}

/**
 * @brief 用借用字段构造栈上临时描述并调用深拷贝追加。
 * @param[in,out] list 必填独占拥有列表。
 * @param[in] id 可选调用期 ID 借用。
 * @param[in] name 可选调用期工具名借用。
 * @param[in] arguments_json 可选原始参数文本，NULL 使用 {}。
 * @return 列表追加结果，临时描述不消费输入字符串，诊断交调用者。
 */
cc_result_t cc_tool_call_list_append_values(
    cc_tool_call_list_t *list,
    const char *id,
    const char *name,
    const char *arguments_json
)
{
    cc_tool_call_t call;
    memset(&call, 0, sizeof(call));
    call.id = (char *)id;
    call.name = (char *)name;
    call.arguments_json = (char *)(arguments_json ? arguments_json : "{}");
    return cc_tool_call_list_append(list, &call);
}

/**
 * @brief 逐项复制只读列表，自复制保持原值。
 * @param[in] src 必填稳定源借用，按 count 访问 items，允许 capacity 缺省的只读视图。
 * @param[out] dst 必填新对象或已 cleanup 槽，非自复制时先初始化为空。
 * @return 参数、尺寸或 OOM，任何项失败清理部分目标，源列表保持，诊断交调用者。
 */
cc_result_t cc_tool_call_list_copy(const cc_tool_call_list_t *src, cc_tool_call_list_t *dst)
{
    if (!src || !dst) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool call list copy argument");
    }
    if (src == dst) return cc_result_ok();
    cc_tool_call_list_init(dst);
    if (src->count && !src->items)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid tool call source list");
    if (src->count > SIZE_MAX / sizeof(cc_tool_call_t))
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Tool call source count overflow");
    for (size_t i = 0; i < src->count; i++) {
        cc_result_t rc = cc_tool_call_list_append(dst, &src->items[i]);
        if (rc.code != CC_OK) {
            cc_tool_call_list_cleanup(dst);
            return rc;
        }
    }
    return cc_result_ok();
}

/**
 * @brief 完整构造工具调用数组，字段或节点挂接失败时撤销全部输出。
 * @param[in] list 调用期间借用的稳定列表，非空列表须具有有效 items。
 * @param[out] out_json 输出独占字符串，调用者 free，失败为 NULL。
 * @return 成功为 CC_OK，参数、尺寸或内存错误的诊断由调用者释放。
 * @note arguments 保留字符串形式，无共享可变状态，源列表的写入由调用者同步。
 */
cc_result_t cc_tool_call_list_to_json(const cc_tool_call_list_t *list, char **out_json)
{
    if (!out_json) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool call JSON argument");
    }
    *out_json = NULL;
    if (!list || (list->count && !list->items))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid tool call list");
    if (list->count > SIZE_MAX / sizeof(cc_tool_call_t))
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Tool call source count overflow");
    cc_json_value_t *arr = cc_json_create_array();
    if (!arr) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate tool calls JSON");
    for (size_t i = 0; i < list->count; i++) {
        const cc_tool_call_t *call = &list->items[i];
        cc_json_value_t *item = cc_json_create_object();
        cc_json_value_t *func = cc_json_create_object();
        if (!item || !func) {
            cc_json_destroy(item);
            cc_json_destroy(func);
            cc_json_destroy(arr);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to serialize tool call");
        }
        if (!cc_json_object_set_owned(item, "id", cc_json_create_string(call->id ? call->id : "")) ||
            !cc_json_object_set_owned(item, "type", cc_json_create_string("function")) ||
            !cc_json_object_set_owned(func, "name", cc_json_create_string(call->name ? call->name : "")) ||
            !cc_json_object_set_owned(func, "arguments",
                cc_json_create_string(call->arguments_json ? call->arguments_json : "{}"))) {
            cc_json_destroy(item);
            cc_json_destroy(func);
            cc_json_destroy(arr);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to set tool call fields");
        }
        if (!cc_json_object_set_owned(item, "function", func)) {
            cc_json_destroy(item);
            cc_json_destroy(arr);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to set tool function");
        }
        if (!cc_json_array_append_owned(arr, item)) {
            cc_json_destroy(arr);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to append tool call JSON");
        }
    }
    *out_json = cc_json_stringify_unformatted(arr);
    cc_json_destroy(arr);
    return *out_json ? cc_result_ok()
                     : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to stringify tool calls");
}

/**
 * @brief 完整解析调用数组并验证每项已知字段，失败撤销整列表。
 * @param[in] json 可选调用期文本借用，NULL/空串生成空列表，其他须完整数组且无解码 NUL。
 * @param[out] out_list 必填新对象或已 cleanup 槽，先置空，再接管逐项副本。
 * @return 参数、JSON/字段、容量或复制错误，诊断标明项索引及字段，失败列表为空。
 * @note 已知字段名沿忽略大小写读取，重复拒绝，未知字段跳过。function 必需对象。
 * id/name 缺失/null 为 NULL，arguments 缺失/null 为 {}，已提供其他值须字符串，参数内部 schema 留给执行链。
 */
cc_result_t cc_tool_call_list_from_json(const char *json, cc_tool_call_list_t *out_list)
{
    if (!out_list) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool call list output");
    }
    cc_tool_call_list_init(out_list);
    if (!json || !*json) return cc_result_ok();
    if (cc_storage_json_has_nul_escape(json))
        return cc_result_error(CC_ERR_JSON, "Tool calls JSON contains a decoded NUL");
    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse_strict(json, &root);
    if (rc.code != CC_OK) return rc;
    if (!cc_json_is_array(root)) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "Tool calls JSON must be an array");
    }
    int n = cc_json_array_size(root);
    for (int i = 0; i < n; i++) {
        cc_json_value_t *item = cc_json_array_get(root, i);
        if (!cc_json_is_object(item)) {
            rc = cc_result_errf(CC_ERR_JSON, "Tool calls item %d must be an object", i);
            goto error;
        }
        static const char *const item_fields[] = {"id", "type", "function"};
        static const char *const function_fields[] = {"name", "arguments"};
        const char *duplicate = cc_json_duplicate_known_field(item, item_fields, sizeof(item_fields) / sizeof(item_fields[0]));
        if (duplicate) {
            rc = cc_result_errf(CC_ERR_JSON, "Tool calls item %d repeats field %s", i, duplicate);
            goto error;
        }
        cc_json_value_t *func = cc_json_object_get(item, "function");
        if (!cc_json_is_object(func)) {
            rc = cc_result_errf(CC_ERR_JSON, "Tool calls item %d field function must be an object", i);
            goto error;
        }
        duplicate = cc_json_duplicate_known_field(func, function_fields, sizeof(function_fields) / sizeof(function_fields[0]));
        if (duplicate) {
            rc = cc_result_errf(CC_ERR_JSON, "Tool calls item %d repeats function.%s", i, duplicate);
            goto error;
        }
        cc_json_value_t *id_value = cc_json_object_get(item, "id");
        cc_json_value_t *type_value = cc_json_object_get(item, "type");
        cc_json_value_t *name_value = cc_json_object_get(func, "name");
        cc_json_value_t *args_value = cc_json_object_get(func, "arguments");
        if (id_value && !cc_json_is_null(id_value) && !cc_json_is_string(id_value)) {
            rc = cc_result_errf(CC_ERR_JSON, "Tool calls item %d field id must be a string or null", i);
            goto error;
        }
        if (type_value && !cc_json_is_string(type_value)) {
            rc = cc_result_errf(CC_ERR_JSON, "Tool calls item %d field type must be a string", i);
            goto error;
        }
        if (name_value && !cc_json_is_null(name_value) && !cc_json_is_string(name_value)) {
            rc = cc_result_errf(CC_ERR_JSON, "Tool calls item %d function.name must be a string or null", i);
            goto error;
        }
        if (args_value && !cc_json_is_null(args_value) && !cc_json_is_string(args_value)) {
            rc = cc_result_errf(CC_ERR_JSON, "Tool calls item %d function.arguments must be a string or null", i);
            goto error;
        }
        const char *id = cc_json_string_value(id_value);
        const char *name = cc_json_string_value(name_value);
        const char *args = cc_json_string_value(args_value);
        rc = cc_tool_call_list_append_values(out_list, id, name, args ? args : "{}");
        if (rc.code != CC_OK) {
            goto error;
        }
    }
    cc_json_destroy(root);
    return cc_result_ok();
error:
    cc_json_destroy(root);
    cc_tool_call_list_cleanup(out_list);
    return rc;
}

/**
 * @brief 分配业务结果，复制可选文本并初始化空资源列表。
 * @param[in] ok 业务状态，按值保存。
 * @param[in] text 可选调用期结果文本借用。
 * @param[in] error 可选调用期错误说明借用。
 * @param[in] metadata 可选调用期元数据借用。
 * @param[out] out_result 必填空拥有槽，先清空，成功交付堆对象。
 * @return 参数或 OOM，失败销毁局部对象，诊断交调用者。
 */
cc_result_t cc_tool_result_create(
    int ok,
    const char *text,
    const char *error,
    const char *metadata,
    cc_tool_result_t **out_result
)
{
    if (!out_result) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool result output");
    }
    *out_result = NULL;
    cc_tool_result_t *result = calloc(1, sizeof(cc_tool_result_t));
    if (!result) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate tool result");
    result->ok = ok;
    result->text = cc_copy_string(text);
    result->error = cc_copy_string(error);
    result->metadata = cc_copy_string(metadata);
    cc_media_artifact_list_init(&result->artifacts);
    if ((text && !result->text) || (error && !result->error) || (metadata && !result->metadata)) {
        cc_tool_result_destroy(result);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy tool result");
    }
    *out_result = result;
    return cc_result_ok();
}

/**
 * @brief 释放业务文本、元数据和资源列表后清零。
 * @param[in,out] result 可空初始化拥有记录，借用者已退出。
 * @post 内部资源归还，记录本身仍由调用者保管。
 */
void cc_tool_result_cleanup(cc_tool_result_t *result)
{
    if (!result) return;
    free(result->text);
    free(result->error);
    free(result->metadata);
    cc_media_artifact_list_cleanup(&result->artifacts);
    memset(result, 0, sizeof(*result));
}

/**
 * @brief cleanup 业务结果后释放堆记录。
 * @param[in,out] result 可空拥有地址，借用者已退出。
 * @post 非空对象及内部字段释放，原借用失效。
 */
void cc_tool_result_destroy(cc_tool_result_t *result)
{
    if (!result) return;
    cc_tool_result_cleanup(result);
    free(result);
}

/**
 * @brief 复用媒体列表向业务结果追加资源副本。
 * @param[in,out] result 必填独占结果，其资源为有效可变拥有列表。
 * @param[in] artifact 必填源借用，可来自同列表现有项。
 * @return 参数或媒体追加结果，源拥有权保留，诊断交调用者。
 */
cc_result_t cc_tool_result_add_artifact(
    cc_tool_result_t *result,
    const cc_media_artifact_t *artifact
)
{
    if (!result) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool result");
    return cc_media_artifact_list_append(&result->artifacts, artifact);
}

/**
 * @brief 复制完整资源列表到局部变量，再替换旧值。
 * @param[in,out] result 必填独占拥有结果。
 * @param[in] artifacts 必填稳定源借用，可为自身列表。
 * @return 参数或复制错误保持旧列表，成功清理旧值并接管副本，诊断交调用者。
 */
cc_result_t cc_tool_result_set_artifacts(
    cc_tool_result_t *result,
    const cc_media_artifact_list_t *artifacts
)
{
    if (!result || !artifacts) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool result artifacts");
    }
    cc_media_artifact_list_t copy;
    cc_result_t rc = cc_media_artifact_list_copy(artifacts, &copy);
    if (rc.code != CC_OK) return rc;
    cc_media_artifact_list_cleanup(&result->artifacts);
    result->artifacts = copy;
    return cc_result_ok();
}

/**
 * @brief 按值移动资源列表并清空原槽。
 * @param[in,out] result 可空独占结果，其他字段保持。
 * @return 拥有列表，接收方最终媒体 list_cleanup，NULL 返回空值，不执行分配。
 */
cc_media_artifact_list_t cc_tool_result_take_artifacts(cc_tool_result_t *result)
{
    cc_media_artifact_list_t taken;
    cc_media_artifact_list_init(&taken);
    if (!result) return taken;
    taken = result->artifacts;
    cc_media_artifact_list_init(&result->artifacts);
    return taken;
}

/**
 * @brief 清零响应并初始化三个空嵌套列表。
 * @param[out] response 必填新对象或已 free 槽，已有资源须先归还。
 * @return NULL 为 INVALID_ARGUMENT，其他为 CC_OK，诊断交调用者。
 */
cc_result_t cc_llm_response_init(cc_llm_response_t *response)
{
    if (!response) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null LLM response");
    memset(response, 0, sizeof(*response));
    cc_content_parts_init(&response->content);
    cc_media_artifact_list_init(&response->artifacts);
    cc_tool_call_list_init(&response->tool_calls);
    return cc_result_ok();
}

/**
 * @brief 归还响应所有拥有字段并清零记录。
 * @param[in,out] response 可空初始化拥有响应，所有借用者已退出。
 * @post 文本、内容、资源、调用和推理释放，结构体留归调用者。
 */
void cc_llm_response_free(cc_llm_response_t *response)
{
    if (!response) return;
    free(response->text);
    cc_content_parts_cleanup(&response->content);
    cc_media_artifact_list_cleanup(&response->artifacts);
    cc_tool_call_list_cleanup(&response->tool_calls);
    free(response->reasoning_content);
    memset(response, 0, sizeof(*response));
}

/**
 * @brief 先复制文本再替换旧值，并登记显式文本标志。
 * @param[in,out] response 必填独占拥有响应，其他字段保持。
 * @param[in] text 可选调用期借用，NULL 使用空串，可指向旧文本内部。
 * @return 参数或 OOM 保持旧值，成功接管副本并置 has_text=1，诊断交调用者。
 */
cc_result_t cc_llm_response_set_text(cc_llm_response_t *response, const char *text)
{
    if (!response) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null LLM response");
    char *copy = cc_copy_string(text ? text : "");
    if (!copy) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy LLM text");
    free(response->text);
    response->text = copy;
    response->has_text = 1;
    return cc_result_ok();
}

/**
 * @brief 复用字段追加向响应添加独立工具调用。
 * @param[in,out] response 必填独占响应，工具列表满足可变约定。
 * @param[in] id 可选调用期 ID 借用。
 * @param[in] name 可选调用期工具名借用。
 * @param[in] arguments_json 可选参数文本借用，NULL 使用 {}。
 * @return 参数或追加结果，原字段拥有权保持，诊断交调用者。
 */
cc_result_t cc_llm_response_add_tool_call(
    cc_llm_response_t *response,
    const char *id,
    const char *name,
    const char *arguments_json
)
{
    if (!response) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null LLM response");
    return cc_tool_call_list_append_values(&response->tool_calls, id, name, arguments_json);
}

/**
 * @brief 复用媒体列表向响应追加资源副本。
 * @param[in,out] response 必填独占响应，资源列表满足可变约定。
 * @param[in] artifact 必填源借用，可为同列表已有项。
 * @return 参数或媒体追加结果，成功副本由响应拥有，诊断交调用者。
 */
cc_result_t cc_llm_response_add_artifact(
    cc_llm_response_t *response,
    const cc_media_artifact_t *artifact
)
{
    if (!response) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null LLM response");
    return cc_media_artifact_list_append(&response->artifacts, artifact);
}
