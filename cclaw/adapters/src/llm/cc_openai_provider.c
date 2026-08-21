



#include "cc/adapters/cc_llm_providers.h"
#include "cc/adapters/cc_http_llm_provider.h"
#include "cc/core/cc_observability.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>

/*
 * 向协议请求追加 HTTP header。
 *
 * request 拥有 headers 数组和其中的 name/value 字符串；失败时调用方会通过
 * cc_llm_http_request_cleanup 释放已经添加的字段。
 */
static cc_result_t add_header(
    cc_llm_http_request_t *request,
    const char *name,
    const char *value
)
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

/*
 * 添加 OpenAI Bearer Authorization header。
 *
 * api_key 为空时允许构造无鉴权请求，便于本地代理或测试；真实云服务通常会返回 401。
 */
static cc_result_t add_bearer_header(cc_llm_http_request_t *request, const char *api_key)
{
    if (!api_key || !*api_key) return cc_result_ok();

    cc_string_builder_t auth;
    cc_result_t rc = cc_string_builder_init(&auth);
    if (rc.code != CC_OK) return rc;
    rc = cc_string_builder_appendf(&auth, "Bearer %s", api_key);
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&auth);
        return rc;
    }
    char *value = cc_string_builder_take(&auth);
    rc = add_header(request, "Authorization", value);
    free(value);
    return rc;
}

/* 返回协议名称，通用 HTTP provider 用它推断多模态 capability。 */
static const char *openai_name(void *self)
{
    (void)self;
    return "openai";
}

/*
 * 通过序列化/反序列化克隆 JSON 节点。
 *
 * cc_json 没有通用 clone API 时使用这个保守方法，避免把原始 messages AST 的节点所有权
 * 转移给 OpenAI body。
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

/* 读取 content part 的 mime 字段，缺失时返回 provider 需要的默认 MIME。 */
static const char *part_mime_or_default(const cc_json_value_t *part, const char *fallback)
{
    const char *mime = cc_json_string_value(cc_json_object_get(part, "mime"));
    return (mime && *mime) ? mime : fallback;
}

/* 将 MIME 映射成 OpenAI input_audio 支持的 format 字段。 */
static const char *audio_format_from_mime(const char *mime)
{
    if (!mime) return "wav";
    if (strstr(mime, "mp3") || strstr(mime, "mpeg")) return "mp3";
    if (strstr(mime, "wav")) return "wav";
    return "wav";
}

/*
 * 为 OpenAI 不支持直传的多模态 part 生成文本 fallback。
 *
 * 这样即使 artifact 只有 path/metadata 没有 inline base64，模型仍能看到资源描述，而不是
 * 静默丢失上下文。
 */
static char *describe_unsupported_part(const char *provider, const cc_json_value_t *part)
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
    cc_string_builder_appendf(&sb, "[%s multimodal fallback: type=%s",
        provider ? provider : "provider", type ? type : "file");
    if (id && *id) cc_string_builder_appendf(&sb, " id=%s", id);
    if (mime && *mime) cc_string_builder_appendf(&sb, " mime=%s", mime);
    if (path && *path) cc_string_builder_appendf(&sb, " path=%s", path);
    if (bytes > 0) cc_string_builder_appendf(&sb, " bytes=%.0f", bytes);
    if (width > 0 && height > 0) cc_string_builder_appendf(&sb, " size=%.0fx%.0f", width, height);
    if (duration_ms > 0) cc_string_builder_appendf(&sb, " duration_ms=%.0f", duration_ms);
    cc_string_builder_append(&sb, " multimodal_fallback=true]");
    return cc_string_builder_take(&sb);
}

/* 追加 OpenAI content 数组中的 text part。 */
static void openai_append_text_part(cc_json_value_t *arr, const char *text)
{
    cc_json_value_t *part = cc_json_create_object();
    cc_json_object_set(part, "type", cc_json_create_string("text"));
    cc_json_object_set(part, "text", cc_json_create_string(text ? text : ""));
    cc_json_array_append(arr, part);
}

/*
 * 将 SDK content parts 转成 OpenAI content 数组。
 *
 * text 直接映射；inline image 变成 image_url data URL；inline audio 变成 input_audio；
 * 其它或非 inline 资源降级成文本描述。
 */
static cc_json_value_t *openai_transform_content_parts(const cc_json_value_t *parts)
{
    cc_json_value_t *out = cc_json_create_array();
    int count = cc_json_array_size(parts);
    for (int i = 0; i < count; ++i) {
        cc_json_value_t *part = cc_json_array_get(parts, i);
        const char *type = cc_json_string_value(cc_json_object_get(part, "type"));
        if (!type) continue;

        if (strcmp(type, "text") == 0) {
            openai_append_text_part(out,
                cc_json_string_value(cc_json_object_get(part, "text")));
        } else if (strcmp(type, "image") == 0) {
            const char *data = cc_json_string_value(cc_json_object_get(part, "data_base64"));
            if (data && *data) {
                const char *mime = part_mime_or_default(part, "image/png");
                cc_string_builder_t url;
                cc_string_builder_init(&url);
                cc_string_builder_appendf(&url, "data:%s;base64,%s", mime, data);
                char *data_url = cc_string_builder_take(&url);
                cc_json_value_t *image_part = cc_json_create_object();
                cc_json_object_set(image_part, "type", cc_json_create_string("image_url"));
                cc_json_value_t *image_url = cc_json_create_object();
                cc_json_object_set(image_url, "url", cc_json_create_string(data_url ? data_url : ""));
                cc_json_object_set(image_part, "image_url", image_url);
                cc_json_array_append(out, image_part);
                free(data_url);
            } else {
                char *fallback = describe_unsupported_part("openai", part);
                openai_append_text_part(out, fallback);
                free(fallback);
            }
        } else if (strcmp(type, "audio") == 0) {
            const char *data = cc_json_string_value(cc_json_object_get(part, "data_base64"));
            if (data && *data) {
                const char *mime = part_mime_or_default(part, "audio/wav");
                cc_json_value_t *audio_part = cc_json_create_object();
                cc_json_object_set(audio_part, "type", cc_json_create_string("input_audio"));
                cc_json_value_t *input_audio = cc_json_create_object();
                cc_json_object_set(input_audio, "data", cc_json_create_string(data));
                cc_json_object_set(input_audio, "format",
                    cc_json_create_string(audio_format_from_mime(mime)));
                cc_json_object_set(audio_part, "input_audio", input_audio);
                cc_json_array_append(out, audio_part);
            } else {
                char *fallback = describe_unsupported_part("openai", part);
                openai_append_text_part(out, fallback);
                free(fallback);
            }
        } else {
            char *fallback = describe_unsupported_part("openai", part);
            openai_append_text_part(out, fallback);
            free(fallback);
        }
    }
    return out;
}

/*
 * 转换单条 SDK message。
 *
 * role、content、tool_call_id、tool_calls 按 OpenAI schema 复制；content 数组会额外做
 * 多模态 part 转换。reasoning_content 通常不应回灌到 input messages，但 DeepSeek
 * thinking mode 要求带 tool_calls 的 assistant 消息保留完整 reasoning_content。
 */
static cc_json_value_t *openai_transform_message(const cc_json_value_t *msg)
{
    cc_json_value_t *out = cc_json_create_object();
    const char *role = cc_json_string_value(cc_json_object_get(msg, "role"));
    cc_json_object_set(out, "role", cc_json_create_string(role ? role : "user"));

    cc_json_value_t *content = cc_json_object_get(msg, "content");
    if (content && cc_json_is_array(content)) {
        cc_json_object_set(out, "content", openai_transform_content_parts(content));
    } else if (content) {
        cc_json_value_t *copy = json_clone_value(content);
        if (copy) cc_json_object_set(out, "content", copy);
    }

    const char *tool_call_id = cc_json_string_value(cc_json_object_get(msg, "tool_call_id"));
    if (tool_call_id) cc_json_object_set(out, "tool_call_id", cc_json_create_string(tool_call_id));
    cc_json_value_t *tool_calls = cc_json_object_get(msg, "tool_calls");
    if (tool_calls) {
        cc_json_value_t *copy = json_clone_value(tool_calls);
        if (copy) cc_json_object_set(out, "tool_calls", copy);
    }
    if (tool_calls && cc_json_is_array(tool_calls) && cc_json_array_size(tool_calls) > 0) {
        const char *reasoning = cc_json_string_value(cc_json_object_get(msg, "reasoning_content"));
        if (reasoning) cc_json_object_set(out, "reasoning_content", cc_json_create_string(reasoning));
    }
    return out;
}

/* 转换完整 messages 数组；输入非法时返回空数组，避免请求构造失败扩大化。 */
static int openai_message_is_system(const cc_json_value_t *msg)
{
    const char *role = cc_json_string_value(cc_json_object_get(msg, "role"));
    return role && strcmp(role, "system") == 0;
}

/*
 * 将用户消息数组转为 OpenAI API 格式，并将 system 消息提前到数组首部。
 * 参数: messages - 原始消息 JSON 数组
 * 返回: 转换后的消息数组（调用方负责释放）
 */
static cc_json_value_t *openai_transform_messages(cc_json_value_t *messages)
{
    if (!messages || !cc_json_is_array(messages)) return cc_json_create_array();
    cc_json_value_t *out = cc_json_create_array();
    int count = cc_json_array_size(messages);
    for (int i = 0; i < count; ++i) {
        cc_json_value_t *msg = cc_json_array_get(messages, i);
        if (openai_message_is_system(msg)) {
            cc_json_array_append(out, openai_transform_message(msg));
        }
    }
    for (int i = 0; i < count; ++i) {
        cc_json_value_t *msg = cc_json_array_get(messages, i);
        if (!openai_message_is_system(msg)) {
            cc_json_array_append(out, openai_transform_message(msg));
        }
    }
    return out;
}

/*
 * 解析 extra_body_json 并将键值对合并到请求 body 中。
 * 参数: body - 目标请求体 JSON 对象, extra_body_json - 额外的 JSON 字符串
 * 返回: CC_OK 或解析/合并错误
 */
static cc_result_t openai_merge_extra_body(cc_json_value_t *body, const char *extra_body_json)
{
    if (!body || !extra_body_json || !extra_body_json[0]) {
        return cc_result_ok();
    }

    cc_json_value_t *extra = NULL;
    cc_result_t rc = cc_json_parse(extra_body_json, &extra);
    if (rc.code != CC_OK) {
        return rc;
    }
    if (!cc_json_is_object(extra)) {
        cc_json_destroy(extra);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "model.extra_body must be a JSON object");
    }

    int count = cc_json_object_size(extra);
    for (int i = 0; i < count; i++) {
        const char *key = cc_json_object_key_at(extra, i);
        cc_json_value_t *value = cc_json_object_value_at(extra, i);
        if (!key || !value) {
            continue;
        }
        cc_json_value_t *copy = json_clone_value(value);
        if (!copy) {
            cc_json_destroy(extra);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy model.extra_body");
        }
        cc_json_object_set(body, key, copy);
    }

    cc_json_destroy(extra);
    return cc_result_ok();
}

/*
 * 构造 OpenAI Chat Completions HTTP 请求。
 *
 * 该函数只生成 url/header/body 和 stream_kind，不执行网络请求。工具 schema 直接透传到
 * tools 字段，stream 模式使用 SSE framing。
 */
static int string_contains_case_insensitive(const char *text, const char *needle)
{
    if (!text || !needle || !*needle) return 0;
    size_t needle_len = strlen(needle);
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        for (; i < needle_len; i++) {
            char a = p[i];
            char b = needle[i];
            if (!a) return 0;
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (i == needle_len) return 1;
    }
    return 0;
}

/*
 * 判断是否应为 Qwen 模型启用思考模式（thinking_mode 且模型名含 "qwen"）。
 * 参数: request - 聊天请求, default_model - 默认模型名
 * 返回: 1 表示应启用, 0 表示不需要
 */
static int openai_should_enable_qwen_thinking(
    const cc_llm_chat_request_t *request,
    const char *default_model
)
{
    if (!request || !request->thinking_mode) return 0;
    const char *model = request->model ? request->model : default_model;
    return string_contains_case_insensitive(model, "qwen");
}

/*
 * 对 Qwen 模型在 thinking_mode 时自动注入 enable_thinking=true 到请求 body。
 * 参数: body - 请求体 JSON, request - 聊天请求, default_model - 默认模型名
 */
static void openai_apply_thinking_defaults(
    cc_json_value_t *body,
    const cc_llm_chat_request_t *request,
    const char *default_model
)
{
    if (!body || !openai_should_enable_qwen_thinking(request, default_model)) {
        return;
    }
    if (!cc_json_object_get(body, "enable_thinking")) {
        cc_json_object_set(body, "enable_thinking", cc_json_create_bool(1));
    }
}

/*
 * 从响应 JSON 对象中提取 reasoning 内容，尝试多个可能的键名（reasoning_content/reasoning/reasoning_text/thinking）。
 * 参数: object - 响应 JSON 对象
 * 返回: 推理内容字符串指针或 NULL
 */
static const char *openai_reasoning_string(cc_json_value_t *object)
{
    if (!object) return NULL;
    const char *value = cc_json_string_value(cc_json_object_get(object, "reasoning_content"));
    if (value && *value) return value;
    value = cc_json_string_value(cc_json_object_get(object, "reasoning"));
    if (value && *value) return value;
    value = cc_json_string_value(cc_json_object_get(object, "reasoning_text"));
    if (value && *value) return value;
    value = cc_json_string_value(cc_json_object_get(object, "thinking"));
    if (value && *value) return value;
    return NULL;
}

/*
 * 将请求配置（provider、model、stream、thinking_mode、extra_body 键）发布为 LLM_REQUEST_CONFIG 可观测事件。
 * 参数: request - 聊天请求（含 event_bus 和配置）, body - 已构建的请求 body
 */
static void openai_publish_request_config(
    const cc_llm_chat_request_t *request,
    cc_json_value_t *body
)
{
    if (!request || !request->event_bus || !body) return;

    cc_json_value_t *attrs = cc_json_create_object();
    if (!attrs) return;
    cc_json_object_set(attrs, "provider_protocol", cc_json_create_string("openai"));
    cc_json_object_set(attrs, "model",
        cc_json_create_string(request->model ? request->model : ""));
    cc_json_object_set(attrs, "stream", cc_json_create_bool(request->stream != 0));
    cc_json_object_set(attrs, "thinking_mode",
        cc_json_create_bool(request->thinking_mode != 0));
    cc_json_object_set(attrs, "enable_thinking_in_body",
        cc_json_create_bool(cc_json_object_get(body, "enable_thinking") != NULL));

    cc_json_value_t *extra_keys = cc_json_create_array();
    if (request->extra_body_json && request->extra_body_json[0]) {
        cc_json_value_t *extra = NULL;
        cc_result_t rc = cc_json_parse(request->extra_body_json, &extra);
        if (rc.code == CC_OK && extra && cc_json_is_object(extra)) {
            int count = cc_json_object_size(extra);
            for (int i = 0; i < count; i++) {
                const char *key = cc_json_object_key_at(extra, i);
                if (key) cc_json_array_append(extra_keys, cc_json_create_string(key));
            }
        }
        if (extra) cc_json_destroy(extra);
        cc_result_free(&rc);
    }
    cc_json_object_set(attrs, "extra_body_keys", extra_keys);

    char *attrs_json = cc_json_stringify_unformatted(attrs);
    cc_json_destroy(attrs);
    if (!attrs_json) return;

    cc_observability_event_t event;
    memset(&event, 0, sizeof(event));
    event.size = sizeof(event);
    event.event = CC_OBS_EVENT_LLM_REQUEST_CONFIG;
    event.session_id = request->session_id;
    event.step = request->step;
    event.status = "provider_body";
    event.attributes_json = attrs_json;
    cc_result_t rc = cc_observability_publish(request->event_bus, &event);
    cc_result_free(&rc);
    free(attrs_json);
}

/*
 * 构建完整的 OpenAI Chat Completions HTTP 请求（url/header/body/stream_kind），不执行网络请求。
 * 参数: self - provider 实例, base_url - API 基础 URL, api_key - API 密钥,
 *        default_model - 默认模型, request - 聊天请求, stream - 是否流式,
 *        out_request - 输出的 HTTP 请求结构
 * 返回: CC_OK 或构建错误
 */
static cc_result_t openai_build_request(
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

    cc_string_builder_t url;
    cc_result_t rc = cc_string_builder_init(&url);
    if (rc.code != CC_OK) return rc;
    rc = cc_string_builder_appendf(&url, "%s/v1/chat/completions", base_url);
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&url);
        return rc;
    }
    out_request->url = cc_string_builder_take(&url);
    out_request->api_key = api_key ? cc_copy_string(api_key) : NULL;
    out_request->stream_kind = stream ? CC_LLM_STREAM_SSE : CC_LLM_STREAM_NONE;

    rc = add_header(out_request, "Content-Type", "application/json");
    if (rc.code == CC_OK) rc = add_bearer_header(out_request, api_key);
    if (rc.code != CC_OK) {
        cc_llm_http_request_cleanup(out_request);
        return rc;
    }

    cc_json_value_t *body = cc_json_create_object();
    if (!body) {
        cc_llm_http_request_cleanup(out_request);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create OpenAI request");
    }
    cc_json_object_set(body, "model", cc_json_create_string(
        request->model ? request->model : default_model));

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
        cc_json_value_t *converted = openai_transform_messages(messages);
        cc_json_destroy(messages);
        cc_json_object_set(body, "messages", converted);
    } else {
        cc_result_free(&rc);
        cc_json_object_set(body, "messages", cc_json_create_array());
    }

    cc_json_object_set(body, "stream", cc_json_create_bool(stream));
    cc_json_object_set(body, "max_tokens", cc_json_create_number(request->max_tokens));
    cc_json_object_set(body, "temperature", cc_json_create_number(request->temperature));
    openai_apply_thinking_defaults(body, request, default_model);
    rc = openai_merge_extra_body(body, request->extra_body_json);
    if (rc.code != CC_OK) {
        cc_json_destroy(body);
        cc_llm_http_request_cleanup(out_request);
        return rc;
    }

    if (request->tools_json && strlen(request->tools_json) > 2) {
        cc_json_value_t *tools = NULL;
        rc = cc_json_parse(request->tools_json, &tools);
        if (rc.code == CC_OK && tools) {
            cc_json_object_set(body, "tools", tools);
            cc_json_object_set(body, "tool_choice", cc_json_create_string("auto"));
        } else {
            cc_result_free(&rc);
        }
    }

    openai_publish_request_config(request, body);

    out_request->body_json = cc_json_stringify_unformatted(body);
    cc_json_destroy(body);
    if (!out_request->url || !out_request->body_json ||
        (api_key && !out_request->api_key)) {
        cc_llm_http_request_cleanup(out_request);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build OpenAI request");
    }
    return cc_result_ok();
}

/*
 * 解析 OpenAI 同步响应。
 *
 * 提取 choices[0].message.content、reasoning_content 和 tool_calls，填入 SDK 统一
 * cc_llm_response_t。API error object 会转换成 CC_ERR_MODEL。
 */
static cc_result_t openai_parse_response(
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
        return cc_result_error(CC_ERR_MODEL, "Failed to parse OpenAI response");
    }

    cc_json_value_t *api_error = cc_json_object_get(root, "error");
    if (api_error) {
        const char *msg = cc_json_string_value(cc_json_object_get(api_error, "message"));
        cc_result_t err = cc_result_error(CC_ERR_MODEL, msg ? msg : "Unknown OpenAI API error");
        cc_json_destroy(root);
        return err;
    }

    cc_json_value_t *choices = cc_json_object_get(root, "choices");
    if (choices && cc_json_is_array(choices) && cc_json_array_size(choices) > 0) {
        cc_json_value_t *choice = cc_json_array_get(choices, 0);
        cc_json_value_t *message = cc_json_object_get(choice, "message");
        if (message) {
            const char *content = cc_json_string_value(cc_json_object_get(message, "content"));
            if (content) {
                cc_llm_response_set_text(out_response, content);
            }

            const char *reasoning = openai_reasoning_string(message);
            if (reasoning) out_response->reasoning_content = cc_copy_string(reasoning);

            cc_json_value_t *tool_calls = cc_json_object_get(message, "tool_calls");
            if (tool_calls && cc_json_is_array(tool_calls) && cc_json_array_size(tool_calls) > 0) {
                int n = cc_json_array_size(tool_calls);
                for (int i = 0; i < n; i++) {
                    cc_json_value_t *tc = cc_json_array_get(tool_calls, i);
                    cc_json_value_t *func = cc_json_object_get(tc, "function");
                    cc_llm_response_add_tool_call(
                        out_response,
                        cc_json_string_value(cc_json_object_get(tc, "id")),
                        cc_json_string_value(cc_json_object_get(func, "name")),
                        cc_json_string_value(cc_json_object_get(func, "arguments")));
                }
            }
        }

        const char *finish = cc_json_string_value(cc_json_object_get(choice, "finish_reason"));
        out_response->finished = (finish && strcmp(finish, "stop") == 0) ? 1 : 0;
    }

    cc_json_destroy(root);
    return cc_result_ok();
}

/*
 * 解析 OpenAI SSE data 事件。
 *
 * delta.content 映射 TEXT，delta.reasoning_content 映射 THINKING，delta.tool_calls 会按
 * TOOL_START/TOOL_DELTA/TOOL_END 输出。函数不拥有 chunk 字符串，它们只在回调期间有效。
 */
static cc_result_t openai_parse_stream_event(
    void *self,
    const char *event_json,
    cc_llm_stream_callback_fn on_chunk,
    void *user_data,
    int *out_finished
)
{
    (void)self;
    if (!event_json || !on_chunk || !out_finished) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid OpenAI stream parser request");
    }
    *out_finished = 0;

    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse(event_json, &root);
    if (rc.code != CC_OK || !root || !cc_json_is_object(root)) {
        if (root) cc_json_destroy(root);
        if (rc.code == CC_OK) {
            return cc_result_error(CC_ERR_JSON, "OpenAI stream event is not an object");
        }
        return rc;
    }

    cc_json_value_t *choices = cc_json_object_get(root, "choices");
    if (!choices || !cc_json_is_array(choices) || cc_json_array_size(choices) == 0) {
        if (cc_json_object_get(root, "usage")) {
            cc_json_destroy(root);
            return cc_result_ok();
        }
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "OpenAI stream event has no choices");
    }

    cc_json_value_t *choice = cc_json_array_get(choices, 0);
    if (!choice || !cc_json_is_object(choice)) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "OpenAI stream choice is not an object");
    }
    cc_json_value_t *finish_value = cc_json_object_get(choice, "finish_reason");
    const char *finish = cc_json_string_value(finish_value);
    if (finish_value && !cc_json_is_null(finish_value) && !finish) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "OpenAI stream finish reason is not a string");
    }
    cc_json_value_t *delta = cc_json_object_get(choice, "delta");
    if (delta && !cc_json_is_object(delta)) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "OpenAI stream delta is not an object");
    }
    if (!delta && !finish) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "OpenAI stream choice has neither delta nor finish reason");
    }

    if (delta) {
        cc_json_value_t *tool_calls = cc_json_object_get(delta, "tool_calls");
        if (tool_calls && !cc_json_is_array(tool_calls)) {
            cc_json_destroy(root);
            return cc_result_error(CC_ERR_JSON, "OpenAI stream tool_calls is not an array");
        }
        if (tool_calls && cc_json_array_size(tool_calls) > 0) {
            int count = cc_json_array_size(tool_calls);
            for (int i = 0; i < count; i++) {
                cc_json_value_t *tc = cc_json_array_get(tool_calls, i);
                if (!tc || !cc_json_is_object(tc)) {
                    cc_json_destroy(root);
                    return cc_result_error(CC_ERR_JSON, "OpenAI stream tool call is not an object");
                }
                cc_json_value_t *func = cc_json_object_get(tc, "function");
                cc_json_value_t *tool_id_value = cc_json_object_get(tc, "id");
                const char *tool_id = cc_json_string_value(tool_id_value);
                if (tool_id_value && !cc_json_is_null(tool_id_value) && !tool_id) {
                    cc_json_destroy(root);
                    return cc_result_error(CC_ERR_JSON, "OpenAI stream tool id is not a string");
                }
                cc_json_value_t *index_value = cc_json_object_get(tc, "index");
                if (index_value && !cc_json_is_number(index_value)) {
                    cc_json_destroy(root);
                    return cc_result_error(CC_ERR_JSON, "OpenAI stream tool index is not a number");
                }
                int tool_index = index_value ? cc_json_int_value(index_value) : i;
                if (tool_index < 0 || !func || !cc_json_is_object(func)) {
                    cc_json_destroy(root);
                    return cc_result_error(CC_ERR_JSON, "OpenAI stream tool call is incomplete");
                }

                cc_json_value_t *name_value = cc_json_object_get(func, "name");
                const char *name = cc_json_string_value(cc_json_object_get(func, "name"));
                if (name_value && !name) {
                    cc_json_destroy(root);
                    return cc_result_error(CC_ERR_JSON, "OpenAI stream tool name is not a string");
                }
                if (name && *name) {
                    cc_stream_chunk_t chunk = {
                        .type = CC_STREAM_CHUNK_TOOL_START,
                        .tool_name = (char *)name,
                        .tool_id = (char *)(tool_id ? tool_id : ""),
                        .tool_index = tool_index,
                    };
                    rc = on_chunk(&chunk, user_data);
                    if (rc.code != CC_OK) {
                        cc_json_destroy(root);
                        return rc;
                    }
                }

                cc_json_value_t *args_value = cc_json_object_get(func, "arguments");
                const char *args = cc_json_string_value(args_value);
                if (args_value && !args) {
                    cc_json_destroy(root);
                    return cc_result_error(CC_ERR_JSON, "OpenAI stream tool arguments are not a string");
                }
                if (args && *args) {
                    cc_stream_chunk_t chunk = {
                        .type = CC_STREAM_CHUNK_TOOL_DELTA,
                        .content = (char *)args,
                        .tool_id = (char *)(tool_id ? tool_id : ""),
                        .tool_index = tool_index,
                    };
                    rc = on_chunk(&chunk, user_data);
                    if (rc.code != CC_OK) {
                        cc_json_destroy(root);
                        return rc;
                    }
                }
            }
        }

        cc_json_value_t *reasoning_value = cc_json_object_get(delta, "reasoning_content");
        if (!reasoning_value) reasoning_value = cc_json_object_get(delta, "reasoning");
        const char *reasoning = openai_reasoning_string(delta);
        if (reasoning_value && !cc_json_is_null(reasoning_value) && !reasoning) {
            cc_json_destroy(root);
            return cc_result_error(CC_ERR_JSON, "OpenAI stream reasoning is not a string");
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

        cc_json_value_t *content_value = cc_json_object_get(delta, "content");
        const char *content = cc_json_string_value(content_value);
        if (content_value && !cc_json_is_null(content_value) && !content) {
            cc_json_destroy(root);
            return cc_result_error(CC_ERR_JSON, "OpenAI stream content is not a string");
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
    }

    if (finish && strcmp(finish, "tool_calls") == 0) {
        cc_stream_chunk_t chunk = {
            .type = CC_STREAM_CHUNK_TOOL_END,
            .tool_index = -1,
        };
        rc = on_chunk(&chunk, user_data);
        if (rc.code != CC_OK) {
            cc_json_destroy(root);
            return rc;
        }
    } else if (finish && strcmp(finish, "stop") == 0) {
        *out_finished = 1;
    }

    cc_json_destroy(root);
    return cc_result_ok();
}

/* OpenAI 协议 vtable；destroy 为 NULL，因为该协议没有私有状态。 */
static cc_llm_protocol_vtable_t openai_protocol_vtable = {
    openai_name,
    openai_build_request,
    openai_parse_response,
    openai_parse_stream_event,
    NULL
};

/*
 * 创建 OpenAI provider。
 *
 * 这里把 OpenAI protocol 注入通用 HTTP provider；默认 base_url/model 只是 SDK 默认值，
 * 下游可通过配置覆盖。
 */
cc_result_t cc_openai_provider_create(
    cc_http_client_t *http_client,
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_provider_t *out_provider
)
{
    cc_llm_protocol_t protocol = { NULL, &openai_protocol_vtable };
    return cc_http_llm_provider_create(
        http_client,
        base_url ? base_url : "https://api.openai.com",
        api_key,
        model ? model : "gpt-4o-mini",
        protocol,
        out_provider);
}
