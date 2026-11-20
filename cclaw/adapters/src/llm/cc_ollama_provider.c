



#include "cc/adapters/cc_llm_providers.h"
#include "cc/adapters/cc_http_llm_provider.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/internal/cc_json_syntax_check.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/**
 * @brief 解析完整协议 JSON 并拒绝解码 NUL。
 * @param[in] text 调用期 NUL 结尾文本借用，NULL 返回解析器参数错误。
 * @param[out] out 必填树槽，存在时先清空，成功交付拥有树。
 * @return 参数或 JSON 结果，失败输出为空，拥有诊断交调用者。
 */
static cc_result_t parse_protocol_json(const char *text, cc_json_value_t **out)
{
    if (out) *out = NULL;
    if (cc_storage_json_has_nul_escape(text))
        return cc_result_error(CC_ERR_JSON, "Ollama JSON contains a decoded NUL");
    return cc_json_parse_strict(text, out);
}

/**
 * @brief 为本次请求扩容并复制追加一个 HTTP 头。
 * @param[in,out] request 独占初始化请求，已有计数对应有效数组。
 * @param[in] name 有效调用期名称借用。
 * @param[in] value 可选调用期值借用，NULL 复制为空串。
 * @return CC_OK 或 OOM，失败保留可清理部分项，外层统一 cleanup。
 */
static cc_result_t add_header(cc_llm_http_request_t *request, const char *name, const char *value)
{
    if (request->header_count >= SIZE_MAX / sizeof(cc_http_header_t))
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Too many Ollama HTTP headers");
    cc_http_header_t *headers = realloc(
        request->headers,
        sizeof(cc_http_header_t) * (request->header_count + 1));
    if (!headers) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to add HTTP header");
    request->headers = headers;
    size_t index = request->header_count++;
    request->headers[index].name = cc_copy_string(name);
    request->headers[index].value = cc_copy_string(value ? value : "");
    if (!request->headers[index].name || !request->headers[index].value) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy HTTP header");
    }
    return cc_result_ok();
}

/** @brief 提供能力映射的协议名。 @param[in] self 未使用，可为 NULL。 @return 静态 ollama 名称借用。 */
static const char *ollama_name(void *self)
{
    (void)self;
    return "ollama";
}

/**
 * @brief 序列化再解析以创建独立树副本。
 * @param[in] value 调用期间借用源节点，原树拥有权保留。
 * @return 拥有副本或 NULL，调用者挂接或销毁，临时文本归还。
 */
static cc_json_value_t *json_clone_value(const cc_json_value_t *value)
{
    char *json = cc_json_stringify_unformatted(value);
    if (!json) return NULL;
    cc_json_value_t *copy = NULL;
    cc_result_t rc = parse_protocol_json(json, &copy);
    cc_result_free(&rc);
    free(json);
    return copy;
}

/**
 * @brief 将非内联图片或其他资源元数据转成文本。
 * @param[in] part 调用期资源借用，读取身份、MIME、路径和正尺寸等字段。
 * @return 拥有文本或 NULL，调用者 free。
 */
static char *ollama_describe_part(const cc_json_value_t *part)
{
    const char *type = cc_json_string_value(cc_json_object_get(part, "type"));
    const char *id = cc_json_string_value(cc_json_object_get(part, "id"));
    const char *mime = cc_json_string_value(cc_json_object_get(part, "mime"));
    const char *path = cc_json_string_value(cc_json_object_get(part, "path"));
    double bytes = cc_json_number_value(cc_json_object_get(part, "bytes"));
    double width = cc_json_number_value(cc_json_object_get(part, "width"));
    double height = cc_json_number_value(cc_json_object_get(part, "height"));
    double duration_ms = cc_json_number_value(cc_json_object_get(part, "duration_ms"));

    cc_string_builder_t sb;
    cc_result_t rc = cc_string_builder_init(&sb);
    if (rc.code == CC_OK) rc = cc_string_builder_appendf(&sb, "[ollama multimodal fallback: type=%s", type ? type : "file");
    if (rc.code == CC_OK && id && *id) rc = cc_string_builder_appendf(&sb, " id=%s", id);
    if (rc.code == CC_OK && mime && *mime) rc = cc_string_builder_appendf(&sb, " mime=%s", mime);
    if (rc.code == CC_OK && path && *path) rc = cc_string_builder_appendf(&sb, " path=%s", path);
    if (rc.code == CC_OK && bytes > 0) rc = cc_string_builder_appendf(&sb, " bytes=%.0f", bytes);
    if (rc.code == CC_OK && width > 0 && height > 0) rc = cc_string_builder_appendf(&sb, " size=%.0fx%.0f", width, height);
    if (rc.code == CC_OK && duration_ms > 0) rc = cc_string_builder_appendf(&sb, " duration_ms=%.0f", duration_ms);
    if (rc.code == CC_OK) rc = cc_string_builder_append(&sb, " multimodal_fallback=true]");
    int success = rc.code == CC_OK;
    cc_result_free(&rc);
    if (!success) { cc_string_builder_deinit(&sb); return NULL; }
    return cc_string_builder_take(&sb);
}

/**
 * @brief 把一个非空文本项用换行连接到已有文本。
 * @param[in,out] sb 有效独占 builder。
 * @param[in] text 可选调用期文本借用，NULL 或空串跳过。
 * @return 追加结果，成功后 builder 拥有文本副本，调用者消费诊断并最终 deinit 或 take。
 */
static cc_result_t ollama_append_part_text(cc_string_builder_t *sb, const char *text)
{
    if (!text || !*text) return cc_result_ok();
    cc_result_t rc = cc_result_ok();
    if (sb->length > 0) rc = cc_string_builder_append(sb, "\n");
    if (rc.code != CC_OK) return rc;
    return cc_string_builder_append(sb, text);
}

/**
 * @brief 转换一条 SDK 消息的文本、内联图片与工具字段。
 * @param[in] msg 调用期源节点借用，角色缺失时采用 user。
 * @return 拥有目标对象或失败 NULL，文本项合并为 content，图片 base64 放入 images。
 */
static cc_json_value_t *ollama_transform_message(const cc_json_value_t *msg)
{
    cc_json_value_t *out = cc_json_create_object();
    const char *role = cc_json_string_value(cc_json_object_get(msg, "role"));
    if (!out || !cc_json_object_set_owned(out, "role", cc_json_create_string(role ? role : "user"))) goto fail;

    cc_json_value_t *content = cc_json_object_get(msg, "content");
    if (content && cc_json_is_array(content)) {
        cc_string_builder_t text;
        cc_result_t rc = cc_string_builder_init(&text);
        if (rc.code != CC_OK) { cc_result_free(&rc); goto fail; }
        cc_json_value_t *images = cc_json_create_array();
        if (!images) { cc_string_builder_deinit(&text); goto fail; }
        int count = cc_json_array_size(content);
        for (int i = 0; i < count; ++i) {
            cc_json_value_t *part = cc_json_array_get(content, i);
            const char *type = cc_json_string_value(cc_json_object_get(part, "type"));
            if (type && strcmp(type, "text") == 0) {
                rc = ollama_append_part_text(&text,
                    cc_json_string_value(cc_json_object_get(part, "text")));
            } else if (type && strcmp(type, "image") == 0) {
                const char *data = cc_json_string_value(cc_json_object_get(part, "data_base64"));
                if (data && *data) {
                    if (!cc_json_array_append_owned(images, cc_json_create_string(data)))
                        rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot append Ollama image");
                } else {
                    char *fallback = ollama_describe_part(part);
                    rc = fallback ? ollama_append_part_text(&text, fallback) :
                        cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot describe Ollama image");
                    free(fallback);
                }
            } else {
                char *fallback = ollama_describe_part(part);
                rc = fallback ? ollama_append_part_text(&text, fallback) :
                    cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot describe Ollama content");
                free(fallback);
            }
            if (rc.code != CC_OK) {
                cc_result_free(&rc); cc_json_destroy(images); cc_string_builder_deinit(&text); goto fail;
            }
            cc_result_free(&rc);
        }
        int content_set = cc_json_object_set_owned(out, "content", cc_json_create_string(cc_string_builder_cstr(&text)));
        cc_string_builder_deinit(&text);
        if (!content_set) { cc_json_destroy(images); goto fail; }
        if (cc_json_array_size(images) > 0) {
            if (!cc_json_object_set_owned(out, "images", images)) goto fail;
        } else {
            cc_json_destroy(images);
        }
    } else {
        const char *text = cc_json_string_value(content);
        if (!cc_json_object_set_owned(out, "content", cc_json_create_string(text ? text : ""))) goto fail;
    }

    const char *tool_call_id = cc_json_string_value(cc_json_object_get(msg, "tool_call_id"));
    if (tool_call_id && !cc_json_object_set_owned(out, "tool_call_id", cc_json_create_string(tool_call_id))) goto fail;
    const char *reasoning = cc_json_string_value(cc_json_object_get(msg, "reasoning_content"));
    if (reasoning && !cc_json_object_set_owned(out, "reasoning_content", cc_json_create_string(reasoning))) goto fail;
    cc_json_value_t *tool_calls = cc_json_object_get(msg, "tool_calls");
    if (tool_calls) {
        cc_json_value_t *copy = json_clone_value(tool_calls);
        if (!copy || !cc_json_object_set_owned(out, "tool_calls", copy)) goto fail;
    }
    return out;
fail:
    cc_json_destroy(out);
    return NULL;
}

/**
 * @brief 按原顺序转换完整消息数组。
 * @param[in] messages 调用期源数组借用，NULL 或非数组沿兼容路径返回空数组。
 * @return 拥有转换数组或失败 NULL，源树继续归原 owner。
 */
static cc_json_value_t *ollama_transform_messages(cc_json_value_t *messages)
{
    if (!messages || !cc_json_is_array(messages)) return cc_json_create_array();
    cc_json_value_t *out = cc_json_create_array();
    if (!out) return NULL;
    int count = cc_json_array_size(messages);
    for (int i = 0; i < count; ++i) {
        if (!cc_json_array_append_owned(out, ollama_transform_message(cc_json_array_get(messages, i)))) {
            cc_json_destroy(out);
            return NULL;
        }
    }
    return out;
}

/**
 * @brief 构造 Ollama /api/chat 的拥有 HTTP 请求。
 * @param[in] self 无状态协议，未使用，可为 NULL。
 * @param[in] base_url 有效调用期地址借用，追加 /api/chat。
 * @param[in] api_key 未使用，Ollama 此适配器不添加认证头。
 * @param[in] default_model 有效默认模型借用，请求模型优先。
 * @param[in] request 有效统一请求借用，消息、工具 JSON 和选项保持到返回。
 * @param[in] stream 非零使用 NDJSON，零为整体响应。
 * @param[in,out] out_request 有效空请求槽，入口清零，成功或部分字段交外层 cleanup。
 * @return 构造结果与拥有诊断，失败清理临时 JSON 与请求。
 */
static cc_result_t ollama_build_request(
    void *self,
    const char *base_url,
    const char *api_key,
    const char *default_model,
    const cc_llm_chat_request_t *request,
    int stream,
    cc_llm_http_request_t *out_request
)
{
    (void)self;
    (void)api_key;
    memset(out_request, 0, sizeof(*out_request));
    out_request->stream_kind = stream ? CC_LLM_STREAM_NDJSON : CC_LLM_STREAM_NONE;

    cc_string_builder_t url;
    cc_result_t rc = cc_string_builder_init(&url);
    if (rc.code != CC_OK) return rc;
    rc = cc_string_builder_appendf(&url, "%s/api/chat", base_url);
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&url);
        return rc;
    }
    out_request->url = cc_string_builder_take(&url);

    rc = add_header(out_request, "Content-Type", "application/json");
    if (rc.code != CC_OK) {
        cc_llm_http_request_cleanup(out_request);
        return rc;
    }

    cc_json_value_t *body = cc_json_create_object();
    if (!body) {
        cc_llm_http_request_cleanup(out_request);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create Ollama request");
    }
    if (!cc_json_object_set_owned(body, "model", cc_json_create_string(request->model ? request->model : default_model)) ||
        !cc_json_object_set_owned(body, "stream", cc_json_create_bool(stream))) goto oom;

    char *messages_text = NULL;
    rc = cc_messages_to_json(
        request->messages,
        request->message_count,
        request->thinking_mode,
        &messages_text);
    cc_json_value_t *messages = NULL;
    if (rc.code == CC_OK && messages_text) {
        rc = parse_protocol_json(messages_text, &messages);
    }
    free(messages_text);
    if (rc.code != CC_OK) goto error;
    cc_json_value_t *converted = ollama_transform_messages(messages);
    cc_json_destroy(messages);
    if (!converted || !cc_json_object_set_owned(body, "messages", converted)) goto oom;

    if (request->tools_json) {
        cc_json_value_t *tools = NULL;
        rc = parse_protocol_json(request->tools_json, &tools);
        if (rc.code != CC_OK) goto error;
        if (!cc_json_is_array(tools)) {
            cc_json_destroy(tools);
            rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Ollama tools must be a JSON array");
            goto error;
        }
        if (!cc_json_object_set_owned(body, "tools", tools)) goto oom;
    }

    cc_json_value_t *options = cc_json_create_object();
    if (!options || !cc_json_object_set_owned(options, "temperature", cc_json_create_number(request->temperature)) ||
        !cc_json_object_set_owned(options, "num_predict", cc_json_create_number(request->max_tokens))) {
        cc_json_destroy(options);
        goto oom;
    }
    if (!cc_json_object_set_owned(body, "options", options)) goto oom;

    out_request->body_json = cc_json_stringify_unformatted(body);
    cc_json_destroy(body);
    if (!out_request->url || !out_request->body_json) {
        cc_llm_http_request_cleanup(out_request);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build Ollama request");
    }
    return cc_result_ok();
oom:
    rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build complete Ollama request");
error:
    cc_json_destroy(body);
    cc_llm_http_request_cleanup(out_request);
    return rc;
}

/**
 * @brief 将 Ollama 同步消息转换为统一文本、推理和工具调用。
 * @param[in] self 无状态协议，未使用，可为 NULL。
 * @param[in] response_json 调用期间借用响应文本。
 * @param[out] out_response 有效空资源槽，入口初始化，拥有字段由上层 cc_llm_response_free。
 * @return 模型、JSON、字段或分配结果，拥有诊断交调用者，失败归还输出字段，成功将 done 记录到 finished。
 */
static cc_result_t ollama_parse_response(
    void *self,
    const char *response_json,
    cc_llm_response_t *out_response
)
{
    (void)self;
    cc_llm_response_init(out_response);

    cc_json_value_t *root = NULL;
    cc_result_t rc = parse_protocol_json(response_json, &root);
    if (rc.code != CC_OK) return rc;
    if (!cc_json_is_object(root)) goto invalid;

    cc_json_value_t *api_error = cc_json_object_get(root, "error");
    if (api_error && !cc_json_is_null(api_error)) {
        const char *msg = cc_json_string_value(api_error);
        cc_result_t err = cc_result_error(CC_ERR_MODEL, msg ? msg : "Unknown Ollama API error");
        cc_json_destroy(root);
        return err;
    }

    cc_json_value_t *message = cc_json_object_get(root, "message");
    if (message && !cc_json_is_object(message)) goto invalid;
    if (message) {
        cc_json_value_t *content_value = cc_json_object_get(message, "content");
        const char *content = cc_json_string_value(content_value);
        if (content_value && !cc_json_is_null(content_value) && !content) goto invalid;
        if (content && *content) {
            rc = cc_llm_response_set_text(out_response, content);
            if (rc.code != CC_OK) goto error;
        }

        cc_json_value_t *reasoning_value = cc_json_object_get(message, "reasoning_content");
        const char *reasoning = cc_json_string_value(reasoning_value);
        if (reasoning_value && !cc_json_is_null(reasoning_value) && !reasoning) goto invalid;
        if (reasoning) {
            out_response->reasoning_content = cc_copy_string(reasoning);
            if (!out_response->reasoning_content) goto oom;
        }

        cc_json_value_t *tool_calls = cc_json_object_get(message, "tool_calls");
        if (tool_calls && !cc_json_is_null(tool_calls) && !cc_json_is_array(tool_calls)) goto invalid;
        if (tool_calls && cc_json_is_array(tool_calls) && cc_json_array_size(tool_calls) > 0) {
            int n = cc_json_array_size(tool_calls);
            for (int i = 0; i < n; i++) {
                cc_json_value_t *tc = cc_json_array_get(tool_calls, i);
                cc_json_value_t *func = cc_json_object_get(tc, "function");
                if (!cc_json_is_object(tc) || !cc_json_is_object(func)) goto invalid;
                cc_json_value_t *id_value = cc_json_object_get(tc, "id");
                const char *id = cc_json_string_value(id_value);
                const char *name = cc_json_string_value(cc_json_object_get(func, "name"));
                if ((id_value && !cc_json_is_null(id_value) && !id) || !name || !*name) goto invalid;
                cc_json_value_t *args = cc_json_object_get(func, "arguments");
                const char *args_text = cc_json_string_value(args);
                char *args_owned = NULL;
                if (!args_text && args && !cc_json_is_null(args)) {
                    if (!cc_json_is_object(args)) goto invalid;
                    args_owned = cc_json_stringify_unformatted(args);
                    if (!args_owned) goto oom;
                }
                rc = cc_llm_response_add_tool_call(out_response, id, name,
                    args_text ? args_text : (args_owned ? args_owned : "{}"));
                free(args_owned);
                if (rc.code != CC_OK) goto error;
                cc_result_free(&rc);
            }
        }
    }

    cc_json_value_t *done = cc_json_object_get(root, "done");
    if (done && !cc_json_is_bool(done)) goto invalid;
    out_response->finished = cc_json_bool_value(done);
    cc_json_destroy(root);
    return cc_result_ok();
invalid:
    rc = cc_result_error(CC_ERR_JSON, "Ollama response fields are invalid");
    goto error;
oom:
    rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy Ollama response");
error:
    cc_json_destroy(root);
    cc_llm_response_free(out_response);
    return rc;
}

/**
 * @brief 解析一条 NDJSON 事件并同步交付推理、文本与完整工具调用块。
 * @param[in] self 无状态协议，未使用，可为 NULL。
 * @param[in] event_json 有效事件文本借用。
 * @param[in] on_chunk 有效同步回调，块及其字段只借用到回调返回。
 * @param[in,out] user_data 可选回调上下文借用。
 * @param[out] out_finished 必填，参数通过后清零，done=true 时置一。
 * @return 参数、JSON、模型或回调结果，失败前可已交付部分块，树和临时参数文本在返回前清理。
 */
static cc_result_t ollama_parse_stream_event(
    void *self,
    const char *event_json,
    cc_llm_stream_callback_fn on_chunk,
    void *user_data,
    int *out_finished
)
{
    (void)self;
    if (!event_json || !on_chunk || !out_finished) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid Ollama stream parser request");
    }
    *out_finished = 0;

    cc_json_value_t *root = NULL;
    cc_result_t rc = parse_protocol_json(event_json, &root);
    if (rc.code != CC_OK || !root || !cc_json_is_object(root)) {
        if (root) cc_json_destroy(root);
        if (rc.code == CC_OK) {
            return cc_result_error(CC_ERR_JSON, "Ollama stream event is not an object");
        }
        return rc;
    }

    cc_json_value_t *api_error = cc_json_object_get(root, "error");
    if (api_error) {
        const char *msg = cc_json_string_value(api_error);
        cc_result_t error = cc_result_error(CC_ERR_MODEL, msg ? msg : "Unknown Ollama API error");
        cc_json_destroy(root);
        return error;
    }

    cc_json_value_t *message = cc_json_object_get(root, "message");
    cc_json_value_t *done = cc_json_object_get(root, "done");
    if (message && !cc_json_is_object(message)) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "Ollama stream message is not an object");
    }
    if (done && !cc_json_is_bool(done)) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "Ollama stream done field is not boolean");
    }
    if (!message && !done) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "Ollama stream event has neither message nor done state");
    }
    if (message) {
        cc_json_value_t *reasoning_value = cc_json_object_get(message, "reasoning_content");
        const char *reasoning = cc_json_string_value(reasoning_value);
        if (reasoning_value && !cc_json_is_null(reasoning_value) && !reasoning) {
            cc_json_destroy(root);
            return cc_result_error(CC_ERR_JSON, "Ollama stream reasoning is not a string");
        }
        if (reasoning && *reasoning) {
            cc_stream_chunk_t chunk = {
                .type = CC_STREAM_CHUNK_THINKING,
                .content = (char *)reasoning,
                .tool_index = -1,
            };
            rc = on_chunk(&chunk, user_data);
            if (rc.code != CC_OK) {
                cc_json_destroy(root);
                return rc;
            }
            cc_result_free(&rc);
        }

        cc_json_value_t *content_value = cc_json_object_get(message, "content");
        const char *content = cc_json_string_value(content_value);
        if (content_value && !cc_json_is_null(content_value) && !content) {
            cc_json_destroy(root);
            return cc_result_error(CC_ERR_JSON, "Ollama stream content is not a string");
        }
        if (content && *content) {
            cc_stream_chunk_t chunk = {
                .type = CC_STREAM_CHUNK_TEXT,
                .content = (char *)content,
                .tool_index = -1,
            };
            rc = on_chunk(&chunk, user_data);
            if (rc.code != CC_OK) {
                cc_json_destroy(root);
                return rc;
            }
            cc_result_free(&rc);
        }

        cc_json_value_t *tool_calls = cc_json_object_get(message, "tool_calls");
        if (tool_calls && !cc_json_is_array(tool_calls)) {
            cc_json_destroy(root);
            return cc_result_error(CC_ERR_JSON, "Ollama stream tool_calls is not an array");
        }
        if (tool_calls && cc_json_array_size(tool_calls) > 0) {
            int count = cc_json_array_size(tool_calls);
            for (int i = 0; i < count; i++) {
                cc_json_value_t *tc = cc_json_array_get(tool_calls, i);
                if (!tc || !cc_json_is_object(tc)) {
                    cc_json_destroy(root);
                    return cc_result_error(CC_ERR_JSON, "Ollama stream tool call is not an object");
                }
                cc_json_value_t *func = cc_json_object_get(tc, "function");
                if (!func || !cc_json_is_object(func)) {
                    cc_json_destroy(root);
                    return cc_result_error(CC_ERR_JSON, "Ollama stream tool call has no function");
                }

                cc_json_value_t *tool_id_value = cc_json_object_get(tc, "id");
                const char *tool_id = cc_json_string_value(tool_id_value);
                if (tool_id_value && !cc_json_is_null(tool_id_value) && !tool_id) {
                    cc_json_destroy(root);
                    return cc_result_error(CC_ERR_JSON, "Ollama stream tool id is not a string");
                }
                const char *name = cc_json_string_value(cc_json_object_get(func, "name"));
                if (!name || !name[0]) {
                    cc_json_destroy(root);
                    return cc_result_error(CC_ERR_JSON, "Ollama stream tool call has no name");
                }
                {
                    cc_stream_chunk_t chunk = {
                        .type = CC_STREAM_CHUNK_TOOL_START,
                        .tool_name = (char *)name,
                        .tool_id = (char *)(tool_id ? tool_id : ""),
                        .tool_index = i,
                    };
                    rc = on_chunk(&chunk, user_data);
                    if (rc.code != CC_OK) {
                        cc_json_destroy(root);
                        return rc;
                    }
                    cc_result_free(&rc);
                }

                cc_json_value_t *args = cc_json_object_get(func, "arguments");
                const char *args_text = cc_json_string_value(args);
                char *args_json = NULL;
                if (!args_text && args && !cc_json_is_null(args)) {
                    if (!cc_json_is_object(args)) {
                        cc_json_destroy(root);
                        return cc_result_error(CC_ERR_JSON, "Ollama tool arguments are not an object or string");
                    }
                    args_json = cc_json_stringify_unformatted(args);
                    if (!args_json) {
                        cc_json_destroy(root);
                        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot serialize Ollama tool arguments");
                    }
                }
                const char *payload = args_text ? args_text : args_json;
                if (payload && *payload) {
                    cc_stream_chunk_t chunk = {
                        .type = CC_STREAM_CHUNK_TOOL_DELTA,
                        .content = (char *)payload,
                        .tool_id = (char *)(tool_id ? tool_id : ""),
                        .tool_index = i,
                    };
                    rc = on_chunk(&chunk, user_data);
                    if (rc.code != CC_OK) {
                        free(args_json);
                        cc_json_destroy(root);
                        return rc;
                    }
                    cc_result_free(&rc);
                }
                free(args_json);
            }
            cc_stream_chunk_t end = {
                .type = CC_STREAM_CHUNK_TOOL_END,
                .tool_index = -1,
            };
            rc = on_chunk(&end, user_data);
            if (rc.code != CC_OK) {
                cc_json_destroy(root);
                return rc;
            }
            cc_result_free(&rc);
        }
    }

    if (cc_json_bool_value(done)) {
        *out_finished = 1;
    }

    cc_json_destroy(root);
    return cc_result_ok();
}

/* Ollama 协议 vtable；无私有状态，因此 destroy 为 NULL。 */
static cc_llm_protocol_vtable_t ollama_protocol_vtable = {
    ollama_name,
    ollama_build_request,
    ollama_parse_response,
    ollama_parse_stream_event,
    NULL
};

/**
 * @brief 注入静态 Ollama 协议并创建通用 provider。
 * @param[in,out] http_client 有效 HTTP 借用，覆盖全部请求、回调与 provider 销毁。
 * @param[in] base_url 调用期可选地址借用，NULL 使用 http://localhost:11434。
 * @param[in] model 调用期可选模型借用，NULL 使用 qwen2.5-coder:7b。
 * @param[out] out_provider 必填空资源槽，成功交付拥有 provider，失败保持原值。
 * @return 通用构造结果，外层清理拥有诊断并在关闭排空后销毁 provider，HTTP 留归外层。
 */
cc_result_t cc_ollama_provider_create(
    cc_http_client_t *http_client,
    const char *base_url,
    const char *model,
    cc_llm_provider_t *out_provider
)
{
    cc_llm_protocol_t protocol = { NULL, &ollama_protocol_vtable };
    return cc_http_llm_provider_create(
        http_client,
        base_url ? base_url : "http://localhost:11434",
        NULL,
        model ? model : "qwen2.5-coder:7b",
        protocol,
        out_provider);
}
