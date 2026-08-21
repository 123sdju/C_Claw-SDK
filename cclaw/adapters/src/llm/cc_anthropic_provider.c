



#include "cc/adapters/cc_llm_providers.h"
#include "cc/adapters/cc_http_llm_provider.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>

/*
 * 向 Anthropic HTTP 请求追加 header。
 *
 * request 拥有 headers 数组和 name/value 字符串；失败时外层会调用
 * cc_llm_http_request_cleanup 释放已经成功追加的 header。
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

/* 返回协议名，通用 HTTP provider 用它推断 capability。 */
static const char *anthropic_name(void *self)
{
    (void)self;
    return "anthropic";
}

/* 构造 Anthropic 简单文本 message；content 是纯字符串形式。 */
static cc_json_value_t *anthropic_text_message(const char *role, const char *content)
{
    cc_json_value_t *message = cc_json_create_object();
    cc_json_object_set(message, "role", cc_json_create_string(role));
    cc_json_object_set(message, "content", cc_json_create_string(content ? content : ""));
    return message;
}

/* 构造 Anthropic block message；content 数组的所有权转移给返回的 message。 */
static cc_json_value_t *anthropic_block_message(const char *role, cc_json_value_t *content)
{
    cc_json_value_t *message = cc_json_create_object();
    cc_json_object_set(message, "role", cc_json_create_string(role));
    cc_json_object_set(message, "content", content ? content : cc_json_create_array());
    return message;
}

/*
 * 把不支持直接上传的多模态 part 描述成文本。
 *
 * Anthropic 只原生支持部分 image base64；其它文件、音频或非 inline 资源用结构化文本
 * 保留 id/mime/path/尺寸等信息，避免上下文静默丢失。
 */
static char *anthropic_describe_part(const cc_json_value_t *part)
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
    cc_string_builder_appendf(&sb, "[anthropic multimodal fallback: type=%s",
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

/* 追加 Anthropic content block 中的 text 块；空文本不追加。 */
static void anthropic_append_text_block(cc_json_value_t *blocks, const char *text)
{
    if (!text || !*text) return;
    cc_json_value_t *block = cc_json_create_object();
    cc_json_object_set(block, "type", cc_json_create_string("text"));
    cc_json_object_set(block, "text", cc_json_create_string(text));
    cc_json_array_append(blocks, block);
}

/*
 * 将 SDK content parts 转成 Anthropic content blocks。
 *
 * text 映射 text block，inline image 映射 image/source/base64，其它 part 转成 fallback 文本。
 * 如果最终没有 block，补一个空 text，避免 Anthropic message content 为空数组。
 */
static cc_json_value_t *anthropic_content_blocks_from_parts(const cc_json_value_t *parts)
{
    cc_json_value_t *blocks = cc_json_create_array();
    int count = cc_json_array_size(parts);
    for (int i = 0; i < count; ++i) {
        cc_json_value_t *part = cc_json_array_get(parts, i);
        const char *type = cc_json_string_value(cc_json_object_get(part, "type"));
        if (type && strcmp(type, "text") == 0) {
            anthropic_append_text_block(blocks,
                cc_json_string_value(cc_json_object_get(part, "text")));
        } else if (type && strcmp(type, "image") == 0) {
            const char *data = cc_json_string_value(cc_json_object_get(part, "data_base64"));
            const char *mime = cc_json_string_value(cc_json_object_get(part, "mime"));
            if (data && *data) {
                cc_json_value_t *block = cc_json_create_object();
                cc_json_object_set(block, "type", cc_json_create_string("image"));
                cc_json_value_t *source = cc_json_create_object();
                cc_json_object_set(source, "type", cc_json_create_string("base64"));
                cc_json_object_set(source, "media_type",
                    cc_json_create_string((mime && *mime) ? mime : "image/png"));
                cc_json_object_set(source, "data", cc_json_create_string(data));
                cc_json_object_set(block, "source", source);
                cc_json_array_append(blocks, block);
            } else {
                char *fallback = anthropic_describe_part(part);
                anthropic_append_text_block(blocks, fallback);
                free(fallback);
            }
        } else {
            char *fallback = anthropic_describe_part(part);
            anthropic_append_text_block(blocks, fallback);
            free(fallback);
        }
    }
    if (cc_json_array_size(blocks) == 0) {
        anthropic_append_text_block(blocks, "");
    }
    return blocks;
}

/*
 * 将 SDK content parts 压平成文本。
 *
 * 该 helper 用于 system、assistant、tool result 等 Anthropic 不适合携带多模态 block 的
 * 场景；非文本 part 仍用 fallback 描述保留关键信息。
 */
static char *anthropic_text_from_parts(const cc_json_value_t *parts)
{
    cc_string_builder_t sb;
    if (cc_string_builder_init(&sb).code != CC_OK) return cc_copy_string("");
    int count = cc_json_array_size(parts);
    for (int i = 0; i < count; ++i) {
        cc_json_value_t *part = cc_json_array_get(parts, i);
        const char *type = cc_json_string_value(cc_json_object_get(part, "type"));
        char *owned = NULL;
        const char *text = NULL;
        if (type && strcmp(type, "text") == 0) {
            text = cc_json_string_value(cc_json_object_get(part, "text"));
        } else {
            owned = anthropic_describe_part(part);
            text = owned;
        }
        if (text && *text) {
            if (sb.length > 0) cc_string_builder_append(&sb, "\n");
            cc_string_builder_append(&sb, text);
        }
        free(owned);
    }
    return cc_string_builder_take(&sb);
}

/*
 * 把 SDK messages JSON 转成 Anthropic messages/system 字段。
 *
 * system role 会合并进 body.system；tool role 会改写成 user 文本消息；user 消息可保留
 * image block。这样 core 不需要了解 Anthropic 特有的消息形态。
 */
static void append_anthropic_messages(
    cc_json_value_t *body,
    const char *messages_text
)
{
    cc_json_value_t *src = NULL;
    cc_result_t rc = cc_json_parse(messages_text, &src);
    if (rc.code != CC_OK || !src || !cc_json_is_array(src)) {
        cc_result_free(&rc);
        if (src) cc_json_destroy(src);
        cc_json_object_set(body, "messages", cc_json_create_array());
        return;
    }

    cc_json_value_t *messages = cc_json_create_array();
    cc_string_builder_t system;
    cc_string_builder_init(&system);

    int count = cc_json_array_size(src);
    for (int i = 0; i < count; i++) {
        cc_json_value_t *msg = cc_json_array_get(src, i);
        const char *role = cc_json_string_value(cc_json_object_get(msg, "role"));
        cc_json_value_t *content_value = cc_json_object_get(msg, "content");
        const char *content = cc_json_string_value(content_value);
        if (!role) continue;

        if (strcmp(role, "system") == 0) {
            char *parts_text = NULL;
            if (content_value && cc_json_is_array(content_value)) {
                parts_text = anthropic_text_from_parts(content_value);
                content = parts_text;
            }
            if (content && *content) {
                if (system.length > 0) cc_string_builder_append(&system, "\n");
                cc_string_builder_append(&system, content);
            }
            free(parts_text);
            continue;
        }

        if (strcmp(role, "assistant") == 0) {
            if (content_value && cc_json_is_array(content_value)) {
                char *text = anthropic_text_from_parts(content_value);
                cc_json_array_append(messages, anthropic_text_message("assistant", text ? text : ""));
                free(text);
            } else {
                cc_json_array_append(messages, anthropic_text_message("assistant", content ? content : ""));
            }
        } else if (strcmp(role, "tool") == 0) {
            cc_string_builder_t tool_result;
            cc_string_builder_init(&tool_result);
            const char *tool_id = cc_json_string_value(cc_json_object_get(msg, "tool_call_id"));
            char *parts_text = NULL;
            if (content_value && cc_json_is_array(content_value)) {
                parts_text = anthropic_text_from_parts(content_value);
                content = parts_text;
            }
            cc_string_builder_appendf(&tool_result, "Tool result%s%s: %s",
                tool_id ? " " : "",
                tool_id ? tool_id : "",
                content ? content : "");
            char *text = cc_string_builder_take(&tool_result);
            cc_json_array_append(messages, anthropic_text_message("user", text ? text : ""));
            free(text);
            free(parts_text);
        } else {
            if (content_value && cc_json_is_array(content_value)) {
                cc_json_array_append(messages, anthropic_block_message("user",
                    anthropic_content_blocks_from_parts(content_value)));
            } else {
                cc_json_array_append(messages, anthropic_text_message("user", content ? content : ""));
            }
        }
    }

    if (system.length > 0) {
        cc_json_object_set(body, "system", cc_json_create_string(cc_string_builder_cstr(&system)));
    }
    cc_string_builder_deinit(&system);
    cc_json_destroy(src);
    cc_json_object_set(body, "messages", messages);
}

/*
 * 将 OpenAI 风格工具 schema 转换成 Anthropic tools schema。
 *
 * core/tool registry 输出的是统一 tools_json，这里提取 function.name/description/parameters
 * 并改名为 Anthropic 的 name/description/input_schema。
 */
static void append_anthropic_tools(cc_json_value_t *body, const char *tools_json)
{
    if (!tools_json || strlen(tools_json) <= 2) return;
    cc_json_value_t *src = NULL;
    cc_result_t rc = cc_json_parse(tools_json, &src);
    if (rc.code != CC_OK || !src || !cc_json_is_array(src)) {
        cc_result_free(&rc);
        if (src) cc_json_destroy(src);
        return;
    }

    cc_json_value_t *tools = cc_json_create_array();
    int count = cc_json_array_size(src);
    for (int i = 0; i < count; i++) {
        cc_json_value_t *tool = cc_json_array_get(src, i);
        cc_json_value_t *func = cc_json_object_get(tool, "function");
        if (!func) continue;

        cc_json_value_t *anthropic_tool = cc_json_create_object();
        const char *name = cc_json_string_value(cc_json_object_get(func, "name"));
        const char *description = cc_json_string_value(cc_json_object_get(func, "description"));
        cc_json_object_set(anthropic_tool, "name", cc_json_create_string(name ? name : ""));
        cc_json_object_set(anthropic_tool, "description", cc_json_create_string(description ? description : ""));

        cc_json_value_t *params = cc_json_object_get(func, "parameters");
        if (params) {
            char *schema = cc_json_stringify_unformatted(params);
            cc_json_value_t *schema_copy = NULL;
            if (schema && cc_json_parse(schema, &schema_copy).code == CC_OK && schema_copy) {
                cc_json_object_set(anthropic_tool, "input_schema", schema_copy);
            } else {
                cc_json_object_set(anthropic_tool, "input_schema", cc_json_create_object());
            }
            free(schema);
        } else {
            cc_json_object_set(anthropic_tool, "input_schema", cc_json_create_object());
        }
        cc_json_array_append(tools, anthropic_tool);
    }

    if (cc_json_array_size(tools) > 0) {
        cc_json_object_set(body, "tools", tools);
    } else {
        cc_json_destroy(tools);
    }
    cc_json_destroy(src);
}

/*
 * 构造 Anthropic Messages API HTTP 请求。
 *
 * 该函数设置 x-api-key、anthropic-version、messages/tools/system 和 stream 字段；stream
 * 使用 SSE framing，由通用 HTTP provider 负责传输。
 */
static cc_result_t anthropic_build_request(
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
    memset(out_request, 0, sizeof(*out_request));
    out_request->stream_kind = stream ? CC_LLM_STREAM_SSE : CC_LLM_STREAM_NONE;

    cc_string_builder_t url;
    cc_result_t rc = cc_string_builder_init(&url);
    if (rc.code != CC_OK) return rc;
    rc = cc_string_builder_appendf(&url, "%s/v1/messages", base_url);
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&url);
        return rc;
    }
    out_request->url = cc_string_builder_take(&url);
    out_request->api_key = api_key ? cc_copy_string(api_key) : NULL;

    rc = add_header(out_request, "Content-Type", "application/json");
    if (rc.code == CC_OK) rc = add_header(out_request, "x-api-key", api_key ? api_key : "");
    if (rc.code == CC_OK) rc = add_header(out_request, "anthropic-version", "2023-06-01");
    if (rc.code != CC_OK) {
        cc_llm_http_request_cleanup(out_request);
        return rc;
    }

    cc_json_value_t *body = cc_json_create_object();
    if (!body) {
        cc_llm_http_request_cleanup(out_request);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create Anthropic request");
    }
    cc_json_object_set(body, "model", cc_json_create_string(
        request->model ? request->model : default_model));
    cc_json_object_set(body, "max_tokens", cc_json_create_number(request->max_tokens));
    cc_json_object_set(body, "temperature", cc_json_create_number(request->temperature));
    cc_json_object_set(body, "stream", cc_json_create_bool(stream));
    char *messages_text = NULL;
    cc_result_t msg_rc = cc_messages_to_json(
        request->messages,
        request->message_count,
        request->thinking_mode,
        &messages_text);
    if (msg_rc.code == CC_OK) {
        append_anthropic_messages(body, messages_text);
    } else {
        cc_result_free(&msg_rc);
        append_anthropic_messages(body, "[]");
    }
    free(messages_text);
    append_anthropic_tools(body, request->tools_json);

    out_request->body_json = cc_json_stringify_unformatted(body);
    cc_json_destroy(body);
    if (!out_request->url || !out_request->body_json ||
        (api_key && !out_request->api_key)) {
        cc_llm_http_request_cleanup(out_request);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build Anthropic request");
    }
    return cc_result_ok();
}

/*
 * 解析 Anthropic 同步响应。
 *
 * content 中 text block 聚合为 assistant 文本；tool_use block 转成 SDK tool call；
 * error object 映射 CC_ERR_MODEL。
 */
static cc_result_t anthropic_parse_response(
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
        return cc_result_error(CC_ERR_MODEL, "Failed to parse Anthropic response");
    }

    cc_json_value_t *api_error = cc_json_object_get(root, "error");
    if (api_error) {
        const char *msg = cc_json_string_value(cc_json_object_get(api_error, "message"));
        cc_result_t err = cc_result_error(CC_ERR_MODEL, msg ? msg : "Unknown Anthropic API error");
        cc_json_destroy(root);
        return err;
    }

    cc_json_value_t *content = cc_json_object_get(root, "content");
    if (content && cc_json_is_array(content)) {
        cc_string_builder_t text;
        cc_string_builder_init(&text);
        int count = cc_json_array_size(content);
        for (int i = 0; i < count; i++) {
            cc_json_value_t *block = cc_json_array_get(content, i);
            const char *type = cc_json_string_value(cc_json_object_get(block, "type"));
            if (type && strcmp(type, "text") == 0) {
                const char *block_text = cc_json_string_value(cc_json_object_get(block, "text"));
                if (block_text) cc_string_builder_append(&text, block_text);
            } else if (type && strcmp(type, "tool_use") == 0) {
                const char *id = cc_json_string_value(cc_json_object_get(block, "id"));
                const char *name = cc_json_string_value(cc_json_object_get(block, "name"));
                cc_json_value_t *input = cc_json_object_get(block, "input");
                char *args = input ? cc_json_stringify_unformatted(input) : cc_copy_string("{}");
                cc_llm_response_add_tool_call(out_response, id, name, args ? args : "{}");
                free(args);
            }
        }
        if (text.length > 0) {
            char *taken = cc_string_builder_take(&text);
            cc_llm_response_set_text(out_response, taken);
            free(taken);
        } else {
            cc_string_builder_deinit(&text);
        }
    }

    const char *stop_reason = cc_json_string_value(cc_json_object_get(root, "stop_reason"));
    out_response->finished = (stop_reason && strcmp(stop_reason, "end_turn") == 0) ? 1 : 0;
    cc_json_destroy(root);
    return cc_result_ok();
}

/*
 * 解析 Anthropic stream event。
 *
 * content_block_start/tool_use 映射 TOOL_START，text_delta 映射 TEXT，thinking_delta 映射
 * THINKING，input_json_delta 映射 TOOL_DELTA，message_delta/message_stop 控制 tool_end
 * 和 finished。
 */
static cc_result_t anthropic_parse_stream_event(
    void *self,
    const char *event_json,
    cc_llm_stream_callback_fn on_chunk,
    void *user_data,
    int *out_finished
)
{
    (void)self;
    if (!event_json || !on_chunk || !out_finished) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid Anthropic stream parser request");
    }
    *out_finished = 0;

    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse(event_json, &root);
    if (rc.code != CC_OK || !root || !cc_json_is_object(root)) {
        if (root) cc_json_destroy(root);
        if (rc.code == CC_OK) {
            return cc_result_error(CC_ERR_JSON, "Anthropic stream event is not an object");
        }
        return rc;
    }

    cc_json_value_t *api_error = cc_json_object_get(root, "error");
    if (api_error) {
        const char *msg = cc_json_string_value(cc_json_object_get(api_error, "message"));
        cc_result_t err = cc_result_error(CC_ERR_MODEL, msg ? msg : "Unknown Anthropic API error");
        cc_json_destroy(root);
        return err;
    }

    const char *type = cc_json_string_value(cc_json_object_get(root, "type"));
    if (!type || !type[0]) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "Anthropic stream event has no type");
    }
    cc_json_value_t *index_value = cc_json_object_get(root, "index");
    if (index_value && !cc_json_is_number(index_value)) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "Anthropic content block index is not a number");
    }
    int block_index = index_value ? cc_json_int_value(index_value) : -1;
    if (type && strcmp(type, "content_block_start") == 0) {
        cc_json_value_t *block = cc_json_object_get(root, "content_block");
        if (block_index < 0 || !block || !cc_json_is_object(block)) {
            cc_json_destroy(root);
            return cc_result_error(CC_ERR_JSON, "Anthropic content block start is incomplete");
        }
        const char *block_type = cc_json_string_value(cc_json_object_get(block, "type"));
        if (!block_type || !block_type[0]) {
            cc_json_destroy(root);
            return cc_result_error(CC_ERR_JSON, "Anthropic content block has no type");
        }
        if (block_type && strcmp(block_type, "tool_use") == 0) {
            const char *id = cc_json_string_value(cc_json_object_get(block, "id"));
            const char *name = cc_json_string_value(cc_json_object_get(block, "name"));
            if (!id || !id[0] || !name || !name[0]) {
                cc_json_destroy(root);
                return cc_result_error(CC_ERR_JSON, "Anthropic tool block is missing id or name");
            }
            {
                cc_stream_chunk_t chunk = {
                    .type = CC_STREAM_CHUNK_TOOL_START,
                    .tool_name = (char *)name,
                    .tool_id = (char *)(id ? id : ""),
                    .tool_index = block_index,
                };
                rc = on_chunk(&chunk, user_data);
                if (rc.code != CC_OK) {
                    cc_json_destroy(root);
                    return rc;
                }
            }
        }
    } else if (type && strcmp(type, "content_block_delta") == 0) {
        cc_json_value_t *delta = cc_json_object_get(root, "delta");
        if (block_index < 0 || !delta || !cc_json_is_object(delta)) {
            cc_json_destroy(root);
            return cc_result_error(CC_ERR_JSON, "Anthropic content block delta is incomplete");
        }
        const char *delta_type = cc_json_string_value(cc_json_object_get(delta, "type"));
        if (!delta_type || !delta_type[0]) {
            cc_json_destroy(root);
            return cc_result_error(CC_ERR_JSON, "Anthropic content delta has no type");
        }
        if (delta_type && strcmp(delta_type, "text_delta") == 0) {
            cc_json_value_t *text_value = cc_json_object_get(delta, "text");
            const char *text = cc_json_string_value(text_value);
            if (!text_value || !text) {
                cc_json_destroy(root);
                return cc_result_error(CC_ERR_JSON, "Anthropic text delta has no text string");
            }
            if (text && *text) {
                cc_stream_chunk_t chunk = {
                    .type = CC_STREAM_CHUNK_TEXT,
                    .content = (char *)text,
                    .tool_index = block_index,
                };
                rc = on_chunk(&chunk, user_data);
                if (rc.code != CC_OK) {
                    cc_json_destroy(root);
                    return rc;
                }
            }
        } else if (delta_type && strcmp(delta_type, "thinking_delta") == 0) {
            cc_json_value_t *thinking_value = cc_json_object_get(delta, "thinking");
            const char *thinking = cc_json_string_value(thinking_value);
            if (!thinking_value || !thinking) {
                cc_json_destroy(root);
                return cc_result_error(CC_ERR_JSON, "Anthropic thinking delta has no thinking string");
            }
            if (thinking && *thinking) {
                cc_stream_chunk_t chunk = {
                    .type = CC_STREAM_CHUNK_THINKING,
                    .content = (char *)thinking,
                    .tool_index = block_index,
                };
                rc = on_chunk(&chunk, user_data);
                if (rc.code != CC_OK) {
                    cc_json_destroy(root);
                    return rc;
                }
            }
        } else if (delta_type && strcmp(delta_type, "input_json_delta") == 0) {
            cc_json_value_t *partial_value = cc_json_object_get(delta, "partial_json");
            const char *partial = cc_json_string_value(partial_value);
            if (!partial_value || !partial) {
                cc_json_destroy(root);
                return cc_result_error(CC_ERR_JSON, "Anthropic input JSON delta has no partial_json string");
            }
            if (partial && *partial) {
                cc_stream_chunk_t chunk = {
                    .type = CC_STREAM_CHUNK_TOOL_DELTA,
                    .content = (char *)partial,
                    .tool_index = block_index,
                };
                rc = on_chunk(&chunk, user_data);
                if (rc.code != CC_OK) {
                    cc_json_destroy(root);
                    return rc;
                }
            }
        }
        else if (strcmp(delta_type, "signature_delta") != 0 &&
                 strcmp(delta_type, "citations_delta") != 0) {
            cc_json_destroy(root);
            return cc_result_error(CC_ERR_JSON, "Unsupported Anthropic content delta type");
        }
    } else if (type && strcmp(type, "content_block_stop") == 0) {
        if (block_index < 0) {
            cc_json_destroy(root);
            return cc_result_error(CC_ERR_JSON, "Anthropic content block stop has no index");
        }
        /* Text and thinking blocks also stop here; tool completion is message-scoped. */
    } else if (type && strcmp(type, "message_delta") == 0) {
        cc_json_value_t *delta = cc_json_object_get(root, "delta");
        if (!delta || !cc_json_is_object(delta)) {
            cc_json_destroy(root);
            return cc_result_error(CC_ERR_JSON, "Anthropic message delta is not an object");
        }
        cc_json_value_t *stop_reason_value = cc_json_object_get(delta, "stop_reason");
        const char *stop_reason = cc_json_string_value(stop_reason_value);
        if (stop_reason_value && !cc_json_is_null(stop_reason_value) && !stop_reason) {
            cc_json_destroy(root);
            return cc_result_error(CC_ERR_JSON, "Anthropic stop reason is not a string");
        }
        if (stop_reason && strcmp(stop_reason, "tool_use") == 0) {
            cc_stream_chunk_t chunk = {
                .type = CC_STREAM_CHUNK_TOOL_END,
                .tool_index = -1,
            };
            rc = on_chunk(&chunk, user_data);
            if (rc.code != CC_OK) {
                cc_json_destroy(root);
                return rc;
            }
        } else if (stop_reason &&
                   (strcmp(stop_reason, "end_turn") == 0 ||
                    strcmp(stop_reason, "max_tokens") == 0 ||
                    strcmp(stop_reason, "stop_sequence") == 0 ||
                    strcmp(stop_reason, "pause_turn") == 0 ||
                    strcmp(stop_reason, "refusal") == 0)) {
            *out_finished = 1;
        }
    } else if (type && strcmp(type, "message_stop") == 0) {
        *out_finished = 1;
    } else if (strcmp(type, "message_start") != 0 && strcmp(type, "ping") != 0) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "Unsupported Anthropic stream event type");
    }

    cc_json_destroy(root);
    return cc_result_ok();
}

/* Anthropic 协议 vtable；无私有状态，因此 destroy 为 NULL。 */
static cc_llm_protocol_vtable_t anthropic_protocol_vtable = {
    anthropic_name,
    anthropic_build_request,
    anthropic_parse_response,
    anthropic_parse_stream_event,
    NULL
};

/*
 * 创建 Anthropic provider。
 *
 * 将 Anthropic protocol 注入通用 HTTP provider；默认 base_url/model 可由配置覆盖。
 */
cc_result_t cc_anthropic_provider_create(
    cc_http_client_t *http_client,
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_provider_t *out_provider
)
{
    cc_llm_protocol_t protocol = { NULL, &anthropic_protocol_vtable };
    return cc_http_llm_provider_create(
        http_client,
        base_url ? base_url : "https://api.anthropic.com",
        api_key,
        model ? model : "claude-3-5-haiku-latest",
        protocol,
        out_provider);
}
