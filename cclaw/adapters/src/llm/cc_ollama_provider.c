



#include "cc/adapters/cc_llm_providers.h"
#include "cc/adapters/cc_http_llm_provider.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>

/*
 * 向 Ollama HTTP 请求追加 header。
 *
 * request 拥有 headers 数组和字段字符串；失败时外层通过 cc_llm_http_request_cleanup
 * 统一释放已添加资源。
 */
static cc_result_t add_header(cc_llm_http_request_t *request, const char *name, const char *value)
{
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

/* 返回协议名；通用 HTTP provider 用它标记本地 Ollama 的能力。 */
static const char *ollama_name(void *self)
{
    (void)self;
    return "ollama";
}

/*
 * 克隆 JSON 节点。
 *
 * 用序列化再解析的方式避免直接移动源 AST 节点，保持消息转换过程的所有权清晰。
 */
static cc_json_value_t *json_clone_value(const cc_json_value_t *value)
{
    char *json = cc_json_stringify_unformatted(value);
    if (!json) return NULL;
    cc_json_value_t *copy = NULL;
    cc_json_parse(json, &copy);
    free(json);
    return copy;
}

/*
 * 为 Ollama 不支持直接使用的多模态 part 生成文本描述。
 *
 * Ollama chat 原生主要支持文本和图片 base64；其它资源保留为 fallback 文本，避免上下文
 * 静默缺失。
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
    if (cc_string_builder_init(&sb).code != CC_OK) return cc_copy_string("[multimodal artifact]");
    cc_string_builder_appendf(&sb, "[ollama multimodal fallback: type=%s",
        type ? type : "file");
    if (id && *id) cc_string_builder_appendf(&sb, " id=%s", id);
    if (mime && *mime) cc_string_builder_appendf(&sb, " mime=%s", mime);
    if (path && *path) cc_string_builder_appendf(&sb, " path=%s", path);
    if (bytes > 0) cc_string_builder_appendf(&sb, " bytes=%.0f", bytes);
    if (width > 0 && height > 0) cc_string_builder_appendf(&sb, " size=%.0fx%.0f", width, height);
    if (duration_ms > 0) cc_string_builder_appendf(&sb, " duration_ms=%.0f", duration_ms);
    cc_string_builder_append(&sb, " multimodal_fallback=true]");
    return cc_string_builder_take(&sb);
}

/* 向 prompt 文本追加一个 part，多个 part 之间用换行分隔。 */
static void ollama_append_part_text(cc_string_builder_t *sb, const char *text)
{
    if (!text || !*text) return;
    if (sb->length > 0) cc_string_builder_append(sb, "\n");
    cc_string_builder_append(sb, text);
}

/*
 * 将 SDK message 转成 Ollama message。
 *
 * content parts 中 text 被合并成 content 字符串，inline image base64 放入 images 数组；
 * tool_call_id/reasoning/tool_calls 尽量透传，便于兼容支持工具调用的 Ollama 模型。
 */
static cc_json_value_t *ollama_transform_message(const cc_json_value_t *msg)
{
    cc_json_value_t *out = cc_json_create_object();
    const char *role = cc_json_string_value(cc_json_object_get(msg, "role"));
    cc_json_object_set(out, "role", cc_json_create_string(role ? role : "user"));

    cc_json_value_t *content = cc_json_object_get(msg, "content");
    if (content && cc_json_is_array(content)) {
        cc_string_builder_t text;
        cc_string_builder_init(&text);
        cc_json_value_t *images = cc_json_create_array();
        int count = cc_json_array_size(content);
        for (int i = 0; i < count; ++i) {
            cc_json_value_t *part = cc_json_array_get(content, i);
            const char *type = cc_json_string_value(cc_json_object_get(part, "type"));
            if (type && strcmp(type, "text") == 0) {
                ollama_append_part_text(&text,
                    cc_json_string_value(cc_json_object_get(part, "text")));
            } else if (type && strcmp(type, "image") == 0) {
                const char *data = cc_json_string_value(cc_json_object_get(part, "data_base64"));
                if (data && *data) {
                    cc_json_array_append(images, cc_json_create_string(data));
                } else {
                    char *fallback = ollama_describe_part(part);
                    ollama_append_part_text(&text, fallback);
                    free(fallback);
                }
            } else {
                char *fallback = ollama_describe_part(part);
                ollama_append_part_text(&text, fallback);
                free(fallback);
            }
        }
        cc_json_object_set(out, "content",
            cc_json_create_string(cc_string_builder_cstr(&text)));
        if (cc_json_array_size(images) > 0) {
            cc_json_object_set(out, "images", images);
        } else {
            cc_json_destroy(images);
        }
        cc_string_builder_deinit(&text);
    } else {
        const char *text = cc_json_string_value(content);
        cc_json_object_set(out, "content", cc_json_create_string(text ? text : ""));
    }

    const char *tool_call_id = cc_json_string_value(cc_json_object_get(msg, "tool_call_id"));
    if (tool_call_id) cc_json_object_set(out, "tool_call_id", cc_json_create_string(tool_call_id));
    const char *reasoning = cc_json_string_value(cc_json_object_get(msg, "reasoning_content"));
    if (reasoning) cc_json_object_set(out, "reasoning_content", cc_json_create_string(reasoning));
    cc_json_value_t *tool_calls = cc_json_object_get(msg, "tool_calls");
    if (tool_calls) {
        cc_json_value_t *copy = json_clone_value(tool_calls);
        if (copy) cc_json_object_set(out, "tool_calls", copy);
    }
    return out;
}

/* 转换 messages 数组；输入非法时返回空数组，保持请求构造可恢复。 */
static cc_json_value_t *ollama_transform_messages(cc_json_value_t *messages)
{
    if (!messages || !cc_json_is_array(messages)) return cc_json_create_array();
    cc_json_value_t *out = cc_json_create_array();
    int count = cc_json_array_size(messages);
    for (int i = 0; i < count; ++i) {
        cc_json_array_append(out, ollama_transform_message(cc_json_array_get(messages, i)));
    }
    return out;
}

/*
 * 构造 Ollama /api/chat 请求。
 *
 * Ollama 默认本地无鉴权，stream 使用 NDJSON framing；temperature/max_tokens 写入 options。
 * tools_json 直接透传给支持工具调用的模型。
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
    cc_json_object_set(body, "model", cc_json_create_string(
        request->model ? request->model : default_model));
    cc_json_object_set(body, "stream", cc_json_create_bool(stream));

    char *messages_text = NULL;
    rc = cc_messages_to_json(
        request->messages,
        request->message_count,
        request->thinking_mode,
        &messages_text);
    cc_json_value_t *messages = NULL;
    if (rc.code == CC_OK && messages_text) {
        rc = cc_json_parse(messages_text, &messages);
    }
    free(messages_text);
    if (rc.code == CC_OK && messages) {
        cc_json_value_t *converted = ollama_transform_messages(messages);
        cc_json_destroy(messages);
        cc_json_object_set(body, "messages", converted);
    } else {
        cc_result_free(&rc);
        cc_json_object_set(body, "messages", cc_json_create_array());
    }

    if (request->tools_json && strlen(request->tools_json) > 2) {
        cc_json_value_t *tools = NULL;
        rc = cc_json_parse(request->tools_json, &tools);
        if (rc.code == CC_OK && tools) {
            cc_json_object_set(body, "tools", tools);
        } else {
            cc_result_free(&rc);
        }
    }

    cc_json_value_t *options = cc_json_create_object();
    cc_json_object_set(options, "temperature", cc_json_create_number(request->temperature));
    cc_json_object_set(options, "num_predict", cc_json_create_number(request->max_tokens));
    cc_json_object_set(body, "options", options);

    out_request->body_json = cc_json_stringify_unformatted(body);
    cc_json_destroy(body);
    if (!out_request->url || !out_request->body_json) {
        cc_llm_http_request_cleanup(out_request);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build Ollama request");
    }
    return cc_result_ok();
}

/*
 * 解析 Ollama 同步响应。
 *
 * message.content 映射文本，message.reasoning_content 映射 thinking，message.tool_calls 映射
 * SDK tool calls；done 字段设置 finished。
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
    cc_result_t rc = cc_json_parse(response_json, &root);
    if (rc.code != CC_OK || !root) {
        if (root) cc_json_destroy(root);
        return cc_result_error(CC_ERR_MODEL, "Failed to parse Ollama response");
    }

    cc_json_value_t *api_error = cc_json_object_get(root, "error");
    if (api_error) {
        const char *msg = cc_json_string_value(api_error);
        cc_result_t err = cc_result_error(CC_ERR_MODEL, msg ? msg : "Unknown Ollama API error");
        cc_json_destroy(root);
        return err;
    }

    cc_json_value_t *message = cc_json_object_get(root, "message");
    if (message) {
        const char *content = cc_json_string_value(cc_json_object_get(message, "content"));
        if (content && strlen(content) > 0) {
            cc_llm_response_set_text(out_response, content);
        }

        const char *reasoning = cc_json_string_value(cc_json_object_get(message, "reasoning_content"));
        if (reasoning) out_response->reasoning_content = cc_copy_string(reasoning);

        cc_json_value_t *tool_calls = cc_json_object_get(message, "tool_calls");
        if (tool_calls && cc_json_is_array(tool_calls) && cc_json_array_size(tool_calls) > 0) {
            int n = cc_json_array_size(tool_calls);
            for (int i = 0; i < n; i++) {
                cc_json_value_t *tc = cc_json_array_get(tool_calls, i);
                cc_json_value_t *func = cc_json_object_get(tc, "function");
                cc_json_value_t *args = cc_json_object_get(func, "arguments");
                const char *args_text = cc_json_string_value(args);
                char *args_owned = NULL;
                if (!args_text && args) args_owned = cc_json_stringify_unformatted(args);
                cc_llm_response_add_tool_call(
                    out_response,
                    cc_json_string_value(cc_json_object_get(tc, "id")),
                    cc_json_string_value(cc_json_object_get(func, "name")),
                    args_text ? args_text : (args_owned ? args_owned : "{}"));
                free(args_owned);
            }
        }
    }

    out_response->finished = cc_json_bool_value(cc_json_object_get(root, "done"));
    cc_json_destroy(root);
    return cc_result_ok();
}

/*
 * 解析 Ollama NDJSON stream event。
 *
 * 每行 JSON 可能包含 content、reasoning_content、tool_calls 或 done；工具调用会立即输出
 * TOOL_START/TOOL_DELTA/TOOL_END，done=true 时通知通用 stream 层发送 FINISHED。
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
    cc_result_t rc = cc_json_parse(event_json, &root);
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
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_MODEL, msg ? msg : "Unknown Ollama API error");
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
                }

                cc_json_value_t *args = cc_json_object_get(func, "arguments");
                const char *args_text = cc_json_string_value(args);
                char *args_json = NULL;
                if (!args_text && args) args_json = cc_json_stringify_unformatted(args);
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

/*
 * 创建 Ollama provider。
 *
 * 将 Ollama protocol 注入通用 HTTP provider；默认 base_url 指向本机 11434，默认模型可
 * 通过配置覆盖。
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
