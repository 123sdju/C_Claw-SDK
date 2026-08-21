#include "cc/core/cc_tool_call.h"

#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include <stdlib.h>
#include <string.h>

/*
 * 创建堆上 tool call。
 *
 * arguments_json 为空时使用 "{}"，保证后续 schema 校验和 provider 序列化总能拿到
 * 合法对象字符串。所有字段都深拷贝，调用方不需要保持输入字符串生命周期。
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

/*
 * 清理 tool call 内部字符串。
 *
 * tool call 常嵌入在列表中，因此这里不释放结构体指针，只释放字段并清零。
 */
void cc_tool_call_cleanup(cc_tool_call_t *call)
{
    if (!call) return;
    free(call->id);
    free(call->name);
    free(call->arguments_json);
    memset(call, 0, sizeof(*call));
}

/*
 * 销毁堆上 tool call。
 *
 * 与 cc_tool_call_create() 成对使用，允许 NULL 以简化失败路径。
 */
void cc_tool_call_destroy(cc_tool_call_t *call)
{
    if (!call) return;
    cc_tool_call_cleanup(call);
    free(call);
}

/*
 * 深拷贝 tool call。
 *
 * runtime 可能把 provider 返回的 tool call 放入 message 或 executor 队列；深拷贝
 * 能确保不同模块之间没有共享可变字符串。
 */
cc_result_t cc_tool_call_copy(const cc_tool_call_t *src, cc_tool_call_t *dst)
{
    if (!src || !dst) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool call copy argument");
    }
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

/*
 * 初始化 tool call list。
 *
 * 空动态数组比固定数组更适合不同 provider 的 tool call 数量差异；调用方可在
 * profile 层用 max steps/limits 控制整体规模。
 */
void cc_tool_call_list_init(cc_tool_call_list_t *list)
{
    if (!list) return;
    memset(list, 0, sizeof(*list));
}

/*
 * 清理 tool call list。
 *
 * 逐个释放元素字段后释放数组缓冲，最后清零，保证错误路径可以重复 cleanup。
 */
void cc_tool_call_list_cleanup(cc_tool_call_list_t *list)
{
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) cc_tool_call_cleanup(&list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

/*
 * 为 tool call list 预留一个写入槽位。
 *
 * 扩容使用 2 起步和倍增策略，兼顾小内存场景和多工具调用场景；新增槽位清零以
 * 便后续 cleanup 安全处理部分初始化元素。
 */
static cc_result_t tool_call_list_reserve(cc_tool_call_list_t *list)
{
    if (list->count < list->capacity) return cc_result_ok();
    size_t next_cap = list->capacity ? list->capacity * 2 : 2;
    cc_tool_call_t *next = realloc(list->items, next_cap * sizeof(cc_tool_call_t));
    if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow tool call list");
    memset(next + list->capacity, 0, (next_cap - list->capacity) * sizeof(cc_tool_call_t));
    list->items = next;
    list->capacity = next_cap;
    return cc_result_ok();
}

/*
 * 追加 tool call 深拷贝。
 *
 * 只有复制成功后才增加 count，保证 OOM 时 list 仍保持原状态。
 */
cc_result_t cc_tool_call_list_append(cc_tool_call_list_t *list, const cc_tool_call_t *call)
{
    if (!list || !call) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool call list append argument");
    }
    cc_result_t rc = tool_call_list_reserve(list);
    if (rc.code != CC_OK) return rc;
    rc = cc_tool_call_copy(call, &list->items[list->count]);
    if (rc.code != CC_OK) return rc;
    list->count++;
    return cc_result_ok();
}

/*
 * 从裸字段追加 tool call。
 *
 * 该 helper 构造一个不拥有内存的临时 call，再交给 append 做深拷贝；因此不会释放
 * 输入字符串，适合 JSON 解析或 provider 适配器临时字段。
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

/*
 * 深拷贝 tool call list。
 *
 * 任一元素失败都会清理 dst，避免调用方在复杂 runtime 回滚中追踪已复制数量。
 */
cc_result_t cc_tool_call_list_copy(const cc_tool_call_list_t *src, cc_tool_call_list_t *dst)
{
    if (!src || !dst) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool call list copy argument");
    }
    cc_tool_call_list_init(dst);
    for (size_t i = 0; i < src->count; i++) {
        cc_result_t rc = cc_tool_call_list_append(dst, &src->items[i]);
        if (rc.code != CC_OK) {
            cc_tool_call_list_cleanup(dst);
            return rc;
        }
    }
    return cc_result_ok();
}

/*
 * 序列化 tool call list 为 function-call JSON。
 *
 * 当前格式兼容主流 chat provider 的 tool_calls 数组：外层 item 包含 id/type，
 * function 子对象包含 name/arguments。arguments 保持字符串，避免重复解析和转义风险。
 */
cc_result_t cc_tool_call_list_to_json(const cc_tool_call_list_t *list, char **out_json)
{
    if (!list || !out_json) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool call JSON argument");
    }
    *out_json = NULL;
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
        cc_json_object_set(item, "id", cc_json_create_string(call->id ? call->id : ""));
        cc_json_object_set(item, "type", cc_json_create_string("function"));
        cc_json_object_set(func, "name", cc_json_create_string(call->name ? call->name : ""));
        cc_json_object_set(func, "arguments",
            cc_json_create_string(call->arguments_json ? call->arguments_json : "{}"));
        cc_json_object_set(item, "function", func);
        cc_json_array_append(arr, item);
    }
    *out_json = cc_json_stringify_unformatted(arr);
    cc_json_destroy(arr);
    return *out_json ? cc_result_ok()
                     : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to stringify tool calls");
}

/*
 * 从 provider tool_calls JSON 解析列表。
 *
 * 函数只解析 SDK 需要的 function/name/arguments 字段；未知字段不保留。解析失败时
 * 清理 out_list，调用方不会拿到部分 tool call 列表。
 */
cc_result_t cc_tool_call_list_from_json(const char *json, cc_tool_call_list_t *out_list)
{
    if (!out_list) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool call list output");
    }
    cc_tool_call_list_init(out_list);
    if (!json || !*json) return cc_result_ok();
    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse(json, &root);
    if (rc.code != CC_OK) return rc;
    if (!cc_json_is_array(root)) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "Tool calls JSON must be an array");
    }
    int n = cc_json_array_size(root);
    for (int i = 0; i < n; i++) {
        cc_json_value_t *item = cc_json_array_get(root, i);
        cc_json_value_t *func = cc_json_object_get(item, "function");
        const char *id = cc_json_string_value(cc_json_object_get(item, "id"));
        const char *name = cc_json_string_value(cc_json_object_get(func, "name"));
        const char *args = cc_json_string_value(cc_json_object_get(func, "arguments"));
        rc = cc_tool_call_list_append_values(out_list, id, name, args ? args : "{}");
        if (rc.code != CC_OK) {
            cc_json_destroy(root);
            cc_tool_call_list_cleanup(out_list);
            return rc;
        }
    }
    cc_json_destroy(root);
    return cc_result_ok();
}

/*
 * 创建堆上工具结果。
 *
 * ok 表达工具业务是否成功；即使 ok=0，也通常作为可恢复结果返回给 LLM，而不是
 * 让整个 runtime 崩溃。文本、错误和元数据全部深拷贝。
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

/*
 * 清理工具结果。
 *
 * 工具结果可以携带 artifact 列表，所以 cleanup 同时释放文本字段和多模态资源。
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

/*
 * 销毁堆上工具结果。
 *
 * 与 cc_tool_result_create() 成对使用，允许 NULL 方便 executor 错误路径统一收尾。
 */
void cc_tool_result_destroy(cc_tool_result_t *result)
{
    if (!result) return;
    cc_tool_result_cleanup(result);
    free(result);
}

/*
 * 向工具结果追加 artifact。
 *
 * 具体 artifact 所有权仍属于调用方；list append 会深拷贝。这样工具可以复用同一
 * 临时 artifact 构造多个返回结构而不共享内部字符串。
 */
cc_result_t cc_tool_result_add_artifact(
    cc_tool_result_t *result,
    const cc_media_artifact_t *artifact
)
{
    if (!result) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool result");
    return cc_media_artifact_list_append(&result->artifacts, artifact);
}

/*
 * 替换工具结果中的 artifact 列表。
 *
 * 先复制新列表再清理旧列表，保证复制失败时 result 原有 artifacts 仍然有效。
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

/*
 * 转移工具结果中的 artifacts 所有权。
 *
 * take 模式避免大 artifact 列表再拷贝一次；返回列表由调用方负责 cleanup。result
 * 内部列表被重新初始化为空，后续 destroy 不会重复释放已转移资源。
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

/*
 * 初始化 provider 响应对象。
 *
 * provider adapter 在填充 response 前调用该函数，确保 content/artifacts/tool_calls
 * 三个嵌套容器处于可 cleanup 状态。
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

/*
 * 释放 provider 响应内部资源。
 *
 * response 通常由调用方栈上持有；这里只释放内部字段并清零，避免跨模块释放结构体
 * 本身造成 allocator 边界问题。
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

/*
 * 设置 LLM 文本输出。
 *
 * 成功后 has_text 置位，runtime 可以区分“模型返回空字符串”和“没有文本字段”。
 * 文本复制成功后才替换旧值，避免 OOM 时丢失已有响应。
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

/*
 * 向 LLM 响应追加 tool call。
 *
 * provider adapter 使用这个 helper 保持 tool call 深拷贝和默认 "{}" 参数规则一致，
 * runtime 后续再做 schema 校验、approval 和执行。
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

/*
 * 向 LLM 响应追加多模态 artifact。
 *
 * 该接口给支持图像/文件输出的 provider 预留通道；artifact 深拷贝后由 response
 * 生命周期统一管理。
 */
cc_result_t cc_llm_response_add_artifact(
    cc_llm_response_t *response,
    const cc_media_artifact_t *artifact
)
{
    if (!response) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null LLM response");
    return cc_media_artifact_list_append(&response->artifacts, artifact);
}
