



#include "cc/adapters/cc_llm_providers.h"
#include "cc/adapters/cc_http_llm_provider.h"
#include "cc/core/cc_observability.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/internal/cc_json_syntax_check.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

/**
 * @brief 解析完整协议 JSON，并拒绝会截断 C 字符串字段的解码 NUL。
 * @param[in] text 调用期 NUL 结尾文本借用，NULL 沿解析器参数错误返回。
 * @param[out] out 必填空树槽，存在时先置 NULL，成功取得拥有树。
 * @return 参数、JSON 或分配解析结果，失败输出为空，调用者释放拥有诊断。
 */
static cc_result_t parse_protocol_json(const char *text, cc_json_value_t **out)
{
    if (out) *out = NULL;
    if (cc_storage_json_has_nul_escape(text))
        return cc_result_error(CC_ERR_JSON, "OpenAI JSON contains a decoded NUL");
    return cc_json_parse_strict(text, out);
}

/**
 * @brief 扩容并向本次协议请求追加拥有 HTTP 头。
 * @param[in,out] request 已清零或初始化的独占请求，已有计数须对应有效数组。
 * @param[in] name 有效调用期名称借用，复制到请求。
 * @param[in] value 可选调用期值借用，NULL 复制为空串。
 * @return CC_OK 或 OOM，失败可能留下可清理部分项，外层统一 cleanup。
 */
static cc_result_t add_header(
    cc_llm_http_request_t *request,
    const char *name,
    const char *value
)
{
    if (request->header_count >= SIZE_MAX / sizeof(cc_http_header_t))
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Too many OpenAI HTTP headers");
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

/**
 * @brief 用临时 builder 构造 Bearer 头，再把副本交给请求。
 * @param[in,out] request 有效独占请求，部分头在外层统一 cleanup。
 * @param[in] api_key 可选调用期凭据借用，NULL 或空串跳过认证头。
 * @return builder 或头追加结果，临时认证文本在返回前释放。
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

/** @brief 提供能力映射的协议名。 @param[in] self 未使用，可为 NULL。 @return 静态借用字符串 openai。 */
static const char *openai_name(void *self)
{
    (void)self;
    return "openai";
}

/**
 * @brief 序列化再解析以取得独立 JSON 树副本。
 * @param[in] value 调用期间借用的有效源节点，原树继续由外层拥有。
 * @return 拥有副本或 NULL，调用者挂接或 cc_json_destroy，临时文本在返回前释放。
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
 * @brief 读取 part 的非空 MIME，缺失时采用调用者的默认值。
 * @param[in] part 借用源节点，缺失字段按空值处理。
 * @param[in] fallback 默认 MIME 借用，可为 NULL。
 * @return 原树或 fallback 的借用指针，其原 owner 保持寿命。
 */
static const char *part_mime_or_default(const cc_json_value_t *part, const char *fallback)
{
    const char *mime = cc_json_string_value(cc_json_object_get(part, "mime"));
    return (mime && *mime) ? mime : fallback;
}

/** @brief 将 MIME 文本映射为音频格式。 @param[in] mime 可选调用期借用，包含 mp3/mpeg 时选 mp3。 @return 静态 mp3 或默认 wav。 */
static const char *audio_format_from_mime(const char *mime)
{
    if (!mime) return "wav";
    if (strstr(mime, "mp3") || strstr(mime, "mpeg")) return "mp3";
    if (strstr(mime, "wav")) return "wav";
    return "wav";
}

/**
 * @brief 将无法直接上传的资源元数据构造成文本描述。
 * @param[in] provider 可选协议名称借用，NULL 使用 provider。
 * @param[in] part 调用期资源节点借用，读取类型、身份、路径、MIME 与正尺寸。
 * @return 拥有文本或分配失败的 NULL，调用者 free。
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
    cc_result_t rc = cc_string_builder_init(&sb);
    if (rc.code == CC_OK) rc = cc_string_builder_appendf(&sb, "[%s multimodal fallback: type=%s", provider ? provider : "provider", type ? type : "file");
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
 * @brief 创建 text 类型内容项并追加到目标数组。
 * @param[in,out] arr 有效拥有数组，本函数只追加项。
 * @param[in] text 调用期文本借用，NULL 使用空串。
 * @return 完整创建并挂接为一，失败为零且归还未交接节点。
 */
static int openai_append_text_part(cc_json_value_t *arr, const char *text)
{
    cc_json_value_t *part = cc_json_create_object();
    if (!part || !cc_json_object_set_owned(part, "type", cc_json_create_string("text")) ||
        !cc_json_object_set_owned(part, "text", cc_json_create_string(text ? text : ""))) {
        cc_json_destroy(part);
        return 0;
    }
    return cc_json_array_append_owned(arr, part);
}

/**
 * @brief 把 SDK 内容项转换为 OpenAI 文本、内联图片和音频数组。
 * @param[in] parts 调用期间借用源数组，其他或缺少内联数据的项使用资源文本描述。
 * @return 拥有转换数组或失败 NULL，源树保留在原 owner。
 */
static cc_json_value_t *openai_transform_content_parts(const cc_json_value_t *parts)
{
    cc_json_value_t *out = cc_json_create_array();
    if (!out) return NULL;
    int count = cc_json_array_size(parts);
    for (int i = 0; i < count; ++i) {
        cc_json_value_t *part = cc_json_array_get(parts, i);
        const char *type = cc_json_string_value(cc_json_object_get(part, "type"));
        if (!type) continue;

        if (strcmp(type, "text") == 0) {
            if (!openai_append_text_part(out,
                cc_json_string_value(cc_json_object_get(part, "text")))) goto fail;
        } else if (strcmp(type, "image") == 0) {
            const char *data = cc_json_string_value(cc_json_object_get(part, "data_base64"));
            if (data && *data) {
                const char *mime = part_mime_or_default(part, "image/png");
                cc_string_builder_t url;
                cc_result_t rc = cc_string_builder_init(&url);
                if (rc.code == CC_OK) rc = cc_string_builder_appendf(&url, "data:%s;base64,%s", mime, data);
                if (rc.code != CC_OK) { cc_result_free(&rc); cc_string_builder_deinit(&url); goto fail; }
                char *data_url = cc_string_builder_take(&url);
                if (!data_url) goto fail;
                cc_json_value_t *image_part = cc_json_create_object();
                cc_json_value_t *image_url = cc_json_create_object();
                int built = image_part && image_url &&
                    cc_json_object_set_owned(image_part, "type", cc_json_create_string("image_url")) &&
                    cc_json_object_set_owned(image_url, "url", cc_json_create_string(data_url));
                free(data_url);
                if (!built) { cc_json_destroy(image_part); cc_json_destroy(image_url); goto fail; }
                if (!cc_json_object_set_owned(image_part, "image_url", image_url)) { cc_json_destroy(image_part); goto fail; }
                if (!cc_json_array_append_owned(out, image_part)) goto fail;
            } else {
                char *fallback = describe_unsupported_part("openai", part);
                int added = fallback && openai_append_text_part(out, fallback);
                free(fallback);
                if (!added) goto fail;
            }
        } else if (strcmp(type, "audio") == 0) {
            const char *data = cc_json_string_value(cc_json_object_get(part, "data_base64"));
            if (data && *data) {
                const char *mime = part_mime_or_default(part, "audio/wav");
                cc_json_value_t *audio_part = cc_json_create_object();
                cc_json_value_t *input_audio = cc_json_create_object();
                if (!audio_part || !input_audio ||
                    !cc_json_object_set_owned(audio_part, "type", cc_json_create_string("input_audio")) ||
                    !cc_json_object_set_owned(input_audio, "data", cc_json_create_string(data)) ||
                    !cc_json_object_set_owned(input_audio, "format", cc_json_create_string(audio_format_from_mime(mime)))) {
                    cc_json_destroy(audio_part); cc_json_destroy(input_audio); goto fail;
                }
                if (!cc_json_object_set_owned(audio_part, "input_audio", input_audio)) { cc_json_destroy(audio_part); goto fail; }
                if (!cc_json_array_append_owned(out, audio_part)) goto fail;
            } else {
                char *fallback = describe_unsupported_part("openai", part);
                int added = fallback && openai_append_text_part(out, fallback);
                free(fallback);
                if (!added) goto fail;
            }
        } else {
            char *fallback = describe_unsupported_part("openai", part);
            int added = fallback && openai_append_text_part(out, fallback);
            free(fallback);
            if (!added) goto fail;
        }
    }
    return out;
fail:
    cc_json_destroy(out);
    return NULL;
}

/**
 * @brief 深拷贝单条消息并转换内容数组，保留工具消息配对字段。
 * @param[in] msg 调用期消息节点借用，缺角色时采用 user。
 * @return 拥有对象或失败 NULL，tool_calls 非空时也复制 reasoning_content 供兼容思考工具回合。
 */
static cc_json_value_t *openai_transform_message(const cc_json_value_t *msg)
{
    cc_json_value_t *out = cc_json_create_object();
    const char *role = cc_json_string_value(cc_json_object_get(msg, "role"));
    if (!out || !cc_json_object_set_owned(out, "role", cc_json_create_string(role ? role : "user"))) goto fail;

    cc_json_value_t *content = cc_json_object_get(msg, "content");
    if (content && cc_json_is_array(content)) {
        if (!cc_json_object_set_owned(out, "content", openai_transform_content_parts(content))) goto fail;
    } else if (content) {
        cc_json_value_t *copy = json_clone_value(content);
        if (!copy || !cc_json_object_set_owned(out, "content", copy)) goto fail;
    }

    const char *tool_call_id = cc_json_string_value(cc_json_object_get(msg, "tool_call_id"));
    if (tool_call_id && !cc_json_object_set_owned(out, "tool_call_id", cc_json_create_string(tool_call_id))) goto fail;
    cc_json_value_t *tool_calls = cc_json_object_get(msg, "tool_calls");
    if (tool_calls && !cc_json_is_null(tool_calls)) {
        cc_json_value_t *copy = json_clone_value(tool_calls);
        if (!copy || !cc_json_object_set_owned(out, "tool_calls", copy)) goto fail;
    }
    if (tool_calls && cc_json_is_array(tool_calls) && cc_json_array_size(tool_calls) > 0) {
        const char *reasoning = cc_json_string_value(cc_json_object_get(msg, "reasoning_content"));
        if (reasoning && !cc_json_object_set_owned(out, "reasoning_content", cc_json_create_string(reasoning))) goto fail;
    }
    return out;
fail:
    cc_json_destroy(out);
    return NULL;
}

/** @brief 判断角色是否为 system。 @param[in] msg 借用消息节点，缺角色返回零。 @return 精确匹配为一，其余为零。 */
static int openai_message_is_system(const cc_json_value_t *msg)
{
    const char *role = cc_json_string_value(cc_json_object_get(msg, "role"));
    return role && strcmp(role, "system") == 0;
}

/**
 * @brief 两遍转换消息，使 system 先输出并保留各组原相对顺序。
 * @param[in] messages 调用期源数组借用，NULL 或非数组沿兼容路径生成空数组。
 * @return 拥有数组或分配失败 NULL，调用者挂接或销毁。
 */
static cc_json_value_t *openai_transform_messages(cc_json_value_t *messages)
{
    if (!messages || !cc_json_is_array(messages)) return cc_json_create_array();
    cc_json_value_t *out = cc_json_create_array();
    if (!out) return NULL;
    int count = cc_json_array_size(messages);
    for (int i = 0; i < count; ++i) {
        cc_json_value_t *msg = cc_json_array_get(messages, i);
        if (openai_message_is_system(msg)) {
            if (!cc_json_array_append_owned(out, openai_transform_message(msg))) goto fail;
        }
    }
    for (int i = 0; i < count; ++i) {
        cc_json_value_t *msg = cc_json_array_get(messages, i);
        if (!openai_message_is_system(msg)) {
            if (!cc_json_array_append_owned(out, openai_transform_message(msg))) goto fail;
        }
    }
    return out;
fail:
    cc_json_destroy(out);
    return NULL;
}

/**
 * @brief 解析额外对象并逐字段深拷贝覆盖目标请求体。
 * @param[in,out] body 有效独占目标对象，失败前可能已有部分字段合并。
 * @param[in] extra_body_json 调用期可选文本借用，NULL 或空串跳过。
 * @return JSON、对象类型或副本/挂接错误，临时树在返回前销毁，失败目标由外层整体丢弃。
 */
static cc_result_t openai_merge_extra_body(cc_json_value_t *body, const char *extra_body_json)
{
    if (!body || !extra_body_json || !extra_body_json[0]) {
        return cc_result_ok();
    }

    cc_json_value_t *extra = NULL;
    cc_result_t rc = parse_protocol_json(extra_body_json, &extra);
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
        if (!cc_json_object_set_owned(body, key, copy)) {
            cc_json_destroy(extra);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot merge model.extra_body");
        }
    }

    cc_json_destroy(extra);
    return cc_result_ok();
}

/**
 * @brief 按 ASCII 大小写折叠查找子串。
 * @param[in] text 可选 NUL 结尾文本借用。
 * @param[in] needle 可选目标子串借用，空串视为未匹配。
 * @return 两个有效文本包含匹配时为一，其他为零。
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

/**
 * @brief 判断请求选用的模型名称是否含 Qwen 标识。
 * @param[in] request 可选请求借用，NULL 返回零，本函数不读取 thinking_mode。
 * @param[in] default_model 请求未指定模型时使用的可选默认名借用。
 * @return 大小写折叠后包含 qwen 为一，其他为零。
 */
static int openai_is_qwen_request(
    const cc_llm_chat_request_t *request,
    const char *default_model
)
{
    if (!request) return 0;
    const char *model = request->model ? request->model : default_model;
    return string_contains_case_insensitive(model, "qwen");
}

/**
 * @brief 为 Qwen 请求设置缺省思考开关及模板参数。
 * @param[in,out] body 独占请求对象，缺失或非 Qwen 时跳过。
 * @param[in] request 借用统一请求，Qwen 匹配后读取 thinking_mode。
 * @param[in] default_model 可选默认模型借用。
 * @return CC_OK 或 OOM，已有 enable_thinking 保留，模板参数由本次值构造，失败目标由外层丢弃。
 */
static cc_result_t openai_apply_thinking_defaults(
    cc_json_value_t *body,
    const cc_llm_chat_request_t *request,
    const char *default_model
)
{
    if (!body || !openai_is_qwen_request(request, default_model)) {
        return cc_result_ok();
    }
    if (!cc_json_object_get(body, "enable_thinking")) {
        if (!cc_json_object_set_owned(body, "enable_thinking", cc_json_create_bool(request->thinking_mode != 0)))
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot set Qwen thinking option");
    }
    cc_json_value_t *template_args = cc_json_create_object();
    if (!template_args || !cc_json_object_set_owned(template_args, "enable_thinking", cc_json_create_bool(request->thinking_mode != 0))) {
        cc_json_destroy(template_args);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot create Qwen template options");
    }
    if (!cc_json_object_set_owned(body, "chat_template_kwargs", template_args))
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot attach Qwen template options");
    return cc_result_ok();
}

/**
 * @brief 按既定顺序查找首个非空推理文本。
 * @param[in] object 可选响应节点借用，依次查 reasoning_content/reasoning/reasoning_text/thinking。
 * @return 原树中非空文本借用或 NULL，树销毁后失效。
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

/** @brief 返回诊断用 JSON 类型名。 @param[in] value 可选节点借用，NULL 表示 missing。 @return 静态类型名称，不含原始模型正文。 */
static const char *openai_json_type_name(const cc_json_value_t *value)
{
    if (!value) return "missing";
    if (cc_json_is_null(value)) return "null";
    if (cc_json_is_array(value)) return "array";
    if (cc_json_is_object(value)) return "object";
    if (cc_json_is_string(value)) return "string";
    if (cc_json_is_number(value)) return "number";
    if (cc_json_is_bool(value)) return "boolean";
    return "unknown";
}

/**
 * @brief 发布请求选项和额外字段名的独立观测事件。
 * @param[in] request 调用期借用统一请求，NULL 或无 event_bus 时跳过。
 * @param[in] body 已构造请求体借用，NULL 时跳过。
 * @post 临时属性树、序列化文本及发布结果均归还，观测分配失败不终止业务请求。
 */
static void openai_publish_request_config(
    const cc_llm_chat_request_t *request,
    cc_json_value_t *body
)
{
    if (!request || !request->event_bus || !body) return;

    cc_json_value_t *attrs = cc_json_create_object();
    if (!attrs) return;
    if (!cc_json_object_set_owned(attrs, "provider_protocol", cc_json_create_string("openai")) ||
        !cc_json_object_set_owned(attrs, "model", cc_json_create_string(request->model ? request->model : "")) ||
        !cc_json_object_set_owned(attrs, "stream", cc_json_create_bool(request->stream != 0)) ||
        !cc_json_object_set_owned(attrs, "thinking_mode", cc_json_create_bool(request->thinking_mode != 0)) ||
        !cc_json_object_set_owned(attrs, "enable_thinking_in_body", cc_json_create_bool(cc_json_object_get(body, "enable_thinking") != NULL))) {
        cc_json_destroy(attrs);
        return;
    }

    cc_json_value_t *extra_keys = cc_json_create_array();
    if (!extra_keys) { cc_json_destroy(attrs); return; }
    if (request->extra_body_json && request->extra_body_json[0]) {
        cc_json_value_t *extra = NULL;
        cc_result_t rc = parse_protocol_json(request->extra_body_json, &extra);
        if (rc.code != CC_OK) {
            cc_json_destroy(extra); cc_json_destroy(extra_keys); cc_json_destroy(attrs); cc_result_free(&rc);
            return;
        }
        if (rc.code == CC_OK && extra && cc_json_is_object(extra)) {
            int count = cc_json_object_size(extra);
            for (int i = 0; i < count; i++) {
                const char *key = cc_json_object_key_at(extra, i);
                if (key && !cc_json_array_append_owned(extra_keys, cc_json_create_string(key))) {
                    cc_json_destroy(extra_keys); cc_json_destroy(extra); cc_json_destroy(attrs); cc_result_free(&rc);
                    return;
                }
            }
        }
        if (extra) cc_json_destroy(extra);
        cc_result_free(&rc);
    }
    if (!cc_json_object_set_owned(attrs, "extra_body_keys", extra_keys)) { cc_json_destroy(attrs); return; }

    char *attrs_json = cc_json_stringify_unformatted(attrs);
    cc_json_destroy(attrs);
    if (!attrs_json) return;

    cc_observability_event_t event;
    memset(&event, 0, sizeof(event));
    event.size = sizeof(event);
    event.event = CC_OBS_EVENT_LLM_REQUEST_CONFIG;
    event.session_id = request->session_id;
    event.run_id = cc_llm_chat_request_run_id(request);
    event.step = request->step;
    event.status = "provider_body";
    event.attributes_json = attrs_json;
    cc_result_t rc = cc_observability_publish(request->event_bus, &event);
    cc_result_free(&rc);
    free(attrs_json);
}

/**
 * @brief 构造 OpenAI HTTP 请求，完成选项合并和流用量配置。
 * @param[in] self 未使用的无状态协议，可为 NULL。
 * @param[in] base_url 有效地址借用，覆盖调用。
 * @param[in] api_key 可选凭据借用，复制到请求与认证头。
 * @param[in] default_model 有效默认模型借用，请求模型优先。
 * @param[in] request 有效统一请求借用，消息与 JSON 字段保持到返回。
 * @param[in] stream 非零选择 SSE 请求。
 * @param[in,out] out_request 有效空请求槽，入口清零，成功或部分字段由外层 cleanup。
 * @return 构造结果与拥有诊断，失败清理本次请求和临时 JSON。
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
    if (!cc_json_object_set_owned(body, "model", cc_json_create_string(request->model ? request->model : default_model))) goto oom;

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
    cc_json_value_t *converted = openai_transform_messages(messages);
    cc_json_destroy(messages);
    if (!converted || !cc_json_object_set_owned(body, "messages", converted)) goto oom;

    if (!cc_json_object_set_owned(body, "stream", cc_json_create_bool(stream)) ||
        !cc_json_object_set_owned(body, "max_tokens", cc_json_create_number(request->max_tokens)) ||
        !cc_json_object_set_owned(body, "temperature", cc_json_create_number(request->temperature))) goto oom;
    rc = openai_apply_thinking_defaults(body, request, default_model);
    if (rc.code != CC_OK) goto error;
    rc = openai_merge_extra_body(body, request->extra_body_json);
    if (rc.code != CC_OK) {
        goto error;
    }

    /* 只为官方规范地址自动请求尾部用量事件，兼容网关由调用者选择，显式 stream_options 始终优先。 */
    cc_json_value_t *stream_value=cc_json_object_get(body,"stream");
    int is_stream=stream && cc_json_is_bool(stream_value) && cc_json_bool_value(stream_value);
    if (is_stream && base_url && strcmp(base_url,"https://api.openai.com")==0 &&
        !cc_json_object_get(body,"stream_options")) {
        cc_json_value_t *options=cc_json_create_object();
        if (!options || !cc_json_object_set_owned(options, "include_usage", cc_json_create_bool(1))) {
            cc_json_destroy(options);
            goto oom;
        }
        if (!cc_json_object_set_owned(body, "stream_options", options)) goto oom;
    }
    cc_json_value_t *options=cc_json_object_get(body,"stream_options");
    cc_json_value_t *include=cc_json_object_get(options,"include_usage");
    out_request->defer_finished_until_end=is_stream && cc_json_is_bool(include) && cc_json_bool_value(include);

    if (request->tools_json) {
        cc_json_value_t *tools = NULL;
        rc = parse_protocol_json(request->tools_json, &tools);
        if (rc.code != CC_OK) goto error;
        if (!cc_json_is_array(tools)) {
            cc_json_destroy(tools);
            rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "OpenAI tools must be a JSON array");
            goto error;
        }
        if (!cc_json_object_set_owned(body, "tools", tools) ||
            !cc_json_object_set_owned(body, "tool_choice", cc_json_create_string("auto"))) goto oom;
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
oom:
    rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build complete OpenAI request");
error:
    cc_json_destroy(body);
    cc_llm_http_request_cleanup(out_request);
    return rc;
}

/**
 * @brief 读取可精确表示为 uint64_t 的非负整数用量。
 * @param[in] object 调用期借用对象，缺字段或类型错误视为无有效数值。
 * @param[in] key 有效字段名借用。
 * @param[in,out] out_present 可选标志，仅有效时置一，其他情况保持原值。
 * @return 有效计数含零，非法、缺失、小数或越界返回零并保留标志。
 */
static uint64_t openai_usage_number(
    const cc_json_value_t *object,
    const char *key,
    int *out_present
)
{
    const cc_json_value_t *value = cc_json_object_get(object, key);
    if (!value || !cc_json_is_number(value)) return 0;
    double number = cc_json_number_value(value);
    if (!(number >= 0.0) || number >= (double)UINT64_MAX) return 0;
    uint64_t count = (uint64_t)number;
    if ((double)count != number) return 0;
    if (out_present) *out_present = 1;
    return count;
}

/**
 * @brief 清零并解析总用量与缓存读写的报告/有效标志。
 * @param[in] root 响应树借用，无对象 usage 时输出空值。
 * @param[out] out_usage 可选值槽，NULL 跳过，其他先清零。
 * @post 数值零与缺失通过 reported/valid 区分，无效但已报告的缓存字段仍保留 reported。
 */
static void openai_parse_usage(
    const cc_json_value_t *root,
    cc_llm_usage_t *out_usage
)
{
    if (!out_usage) return;
    memset(out_usage, 0, sizeof(*out_usage));
    const cc_json_value_t *usage = cc_json_object_get(root, "usage");
    if (!usage || !cc_json_is_object(usage)) return;
    int present = 0;
    out_usage->prompt_tokens = openai_usage_number(usage, "prompt_tokens", &present);
    out_usage->completion_tokens = openai_usage_number(usage, "completion_tokens", &present);
    const cc_json_value_t *prompt_details =
        cc_json_object_get(usage, "prompt_tokens_details");
    out_usage->cached_tokens_reported = cc_json_object_get(prompt_details, "cached_tokens") != NULL;
    out_usage->cache_write_tokens_reported = cc_json_object_get(prompt_details, "cache_write_tokens") != NULL;
    int cached_present = 0;
    int cache_write_present = 0;
    out_usage->cached_tokens = openai_usage_number(
        prompt_details, "cached_tokens", &cached_present);
    out_usage->cache_write_tokens = openai_usage_number(
        prompt_details, "cache_write_tokens", &cache_write_present);
    out_usage->cached_tokens_valid = cached_present;
    out_usage->cache_write_tokens_valid = cache_write_present;
    if (out_usage->cached_tokens_reported || out_usage->cache_write_tokens_reported) present = 1;
    out_usage->valid = present;
}

/**
 * @brief 将可观察用量按值填入一个同步 usage 块。
 * @param[in] root 本次事件树借用。
 * @param[in] on_chunk 有效同步回调，块仅在回调内借用。
 * @param[in,out] user_data 可选回调上下文借用。
 * @return 无用量为 CC_OK，其他返回回调拥有结果供调用者释放或转交。
 */
static cc_result_t openai_emit_stream_usage(
    const cc_json_value_t *root,
    cc_llm_stream_callback_fn on_chunk,
    void *user_data
)
{
    cc_llm_usage_t usage;
    openai_parse_usage(root, &usage);
    if (!usage.valid) return cc_result_ok();
    cc_stream_chunk_t chunk;
    memset(&chunk, 0, sizeof(chunk));
    chunk.type = CC_STREAM_CHUNK_USAGE;
    chunk.tool_index = -1;
    chunk.usage = usage;
    return on_chunk(&chunk, user_data);
}

/**
 * @brief 解析首个 OpenAI choice 的文本、推理、工具调用和用量。
 * @param[in] self 无状态协议借用，未使用，可为 NULL。
 * @param[in] response_json 调用期间借用的有效响应文本。
 * @param[out] out_response 有效空资源槽，入口初始化，拥有字段由上层响应清理函数释放。
 * @return 解析、字段、模型或分配错误，拥有诊断交调用者，失败归还已分配字段并保持空响应。
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
    cc_result_t rc = parse_protocol_json(response_json, &root);
    if (rc.code != CC_OK) return rc;
    if (!cc_json_is_object(root)) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "OpenAI response is not an object");
    }

    cc_json_value_t *api_error = cc_json_object_get(root, "error");
    if (api_error && !cc_json_is_null(api_error)) {
        const char *msg = cc_json_string_value(cc_json_object_get(api_error, "message"));
        cc_result_t err = cc_result_error(CC_ERR_MODEL, msg ? msg : "Unknown OpenAI API error");
        cc_json_destroy(root);
        return err;
    }

    openai_parse_usage(root, &out_response->usage);

    cc_json_value_t *choices = cc_json_object_get(root, "choices");
    if (choices && !cc_json_is_array(choices)) {
        cc_json_destroy(root);
        cc_llm_response_free(out_response);
        return cc_result_error(CC_ERR_JSON, "OpenAI response choices is not an array");
    }
    if (choices && cc_json_is_array(choices) && cc_json_array_size(choices) > 0) {
        cc_json_value_t *choice = cc_json_array_get(choices, 0);
        if (!choice || !cc_json_is_object(choice)) {
            cc_json_destroy(root);
            cc_llm_response_free(out_response);
            return cc_result_error(CC_ERR_JSON, "OpenAI response choice is not an object");
        }
        cc_json_value_t *message = cc_json_object_get(choice, "message");
        if (message && !cc_json_is_object(message)) {
            cc_json_destroy(root);
            cc_llm_response_free(out_response);
            return cc_result_error(CC_ERR_JSON, "OpenAI response message is not an object");
        }
        if (message) {
            cc_json_value_t *content_value = cc_json_object_get(message, "content");
            const char *content = cc_json_string_value(content_value);
            if (content_value && !cc_json_is_null(content_value) && !content) {
                cc_json_destroy(root);
                cc_llm_response_free(out_response);
                return cc_result_error(CC_ERR_JSON, "OpenAI response content is not a string");
            }
            if (content) {
                rc = cc_llm_response_set_text(out_response, content);
                if (rc.code != CC_OK) {
                    cc_json_destroy(root);
                    cc_llm_response_free(out_response);
                    return rc;
                }
                cc_result_free(&rc);
            }

            cc_json_value_t *reasoning_value = cc_json_object_get(message, "reasoning_content");
            if (!reasoning_value) reasoning_value = cc_json_object_get(message, "reasoning");
            if (!reasoning_value) reasoning_value = cc_json_object_get(message, "reasoning_text");
            if (!reasoning_value) reasoning_value = cc_json_object_get(message, "thinking");
            const char *reasoning = openai_reasoning_string(message);
            if (reasoning_value && !cc_json_is_null(reasoning_value) && !cc_json_is_string(reasoning_value)) {
                cc_json_destroy(root);
                cc_llm_response_free(out_response);
                return cc_result_error(CC_ERR_JSON, "OpenAI response reasoning is not a string");
            }
            if (reasoning) {
                out_response->reasoning_content = cc_copy_string(reasoning);
                if (!out_response->reasoning_content) {
                    cc_json_destroy(root);
                    cc_llm_response_free(out_response);
                    return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy OpenAI reasoning");
                }
            }

            cc_json_value_t *tool_calls = cc_json_object_get(message, "tool_calls");
            if (tool_calls && !cc_json_is_null(tool_calls) && !cc_json_is_array(tool_calls)) {
                const char *type_name = openai_json_type_name(tool_calls);
                cc_json_destroy(root);
                cc_llm_response_free(out_response);
                return cc_result_errf(CC_ERR_JSON,
                                      "OpenAI response tool_calls has invalid type: %s (expected array or null)",
                                      type_name);
            }
            if (tool_calls && cc_json_is_array(tool_calls) && cc_json_array_size(tool_calls) > 0) {
                int n = cc_json_array_size(tool_calls);
                for (int i = 0; i < n; i++) {
                    cc_json_value_t *tc = cc_json_array_get(tool_calls, i);
                    if (!tc || !cc_json_is_object(tc)) {
                        cc_json_destroy(root);
                        cc_llm_response_free(out_response);
                        return cc_result_error(CC_ERR_JSON,
                                               "OpenAI response tool call is incomplete");
                    }
                    cc_json_value_t *func = cc_json_object_get(tc, "function");
                    if (!func || !cc_json_is_object(func)) {
                        cc_json_destroy(root);
                        cc_llm_response_free(out_response);
                        return cc_result_error(CC_ERR_JSON,
                                               "OpenAI response tool call is incomplete");
                    }
                    cc_json_value_t *id_value = cc_json_object_get(tc, "id");
                    cc_json_value_t *name_value = cc_json_object_get(func, "name");
                    cc_json_value_t *args_value = cc_json_object_get(func, "arguments");
                    const char *id = cc_json_string_value(id_value);
                    const char *name = cc_json_string_value(name_value);
                    const char *args = cc_json_string_value(args_value);
                    if ((id_value && !cc_json_is_null(id_value) && !id) ||
                        !name || !*name ||
                        (args_value && !cc_json_is_null(args_value) && !args)) {
                        cc_json_destroy(root);
                        cc_llm_response_free(out_response);
                        return cc_result_error(CC_ERR_JSON,
                                               "OpenAI response tool call fields are invalid");
                    }
                    rc = cc_llm_response_add_tool_call(
                        out_response,
                        id,
                        name,
                        args ? args : "{}");
                    if (rc.code != CC_OK) {
                        cc_json_destroy(root);
                        cc_llm_response_free(out_response);
                        return rc;
                    }
                    cc_result_free(&rc);
                }
            }
        }

        cc_json_value_t *finish_value = cc_json_object_get(choice, "finish_reason");
        const char *finish = cc_json_string_value(finish_value);
        if (finish_value && !cc_json_is_null(finish_value) && !finish) {
            cc_json_destroy(root);
            cc_llm_response_free(out_response);
            return cc_result_error(CC_ERR_JSON, "OpenAI response finish reason is not a string");
        }
        out_response->finished = (finish && strcmp(finish, "stop") == 0) ? 1 : 0;
    }

    cc_json_destroy(root);
    return cc_result_ok();
}

/**
 * @brief 解析单个 SSE 正文并同步交付工具、推理、文本及用量块。
 * @param[in] self 无状态协议，未使用，可为 NULL。
 * @param[in] event_json 有效事件文本借用，内容保持到返回。
 * @param[in] on_chunk 有效同步块回调，字符串借用只覆盖嵌套回调。
 * @param[in,out] user_data 可选上层上下文借用。
 * @param[out] out_finished 必填，参数通过后置零，stop 语义完成时置一，最终交付由通用层协调。
 * @return 参数、JSON、模型或回调结果，失败前可能已交付部分块，临时树在返回前销毁。
 * @note 仅处理首个 choice，tool_calls 结束原因交付 TOOL_END，stop 置语义完成，usage/元数据事件可没有 choice。
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
    cc_result_t rc = parse_protocol_json(event_json, &root);
    if (rc.code != CC_OK || !root || !cc_json_is_object(root)) {
        if (root) cc_json_destroy(root);
        if (rc.code == CC_OK) {
            return cc_result_error(CC_ERR_JSON, "OpenAI stream event is not an object");
        }
        return rc;
    }

    cc_json_value_t *api_error = cc_json_object_get(root, "error");
    if (api_error && !cc_json_is_null(api_error)) {
        const char *msg = cc_json_string_value(cc_json_object_get(api_error, "message"));
        cc_result_t error = cc_result_error(CC_ERR_MODEL, msg ? msg : "OpenAI stream API error");
        cc_json_destroy(root);
        return error;
    }

    cc_json_value_t *choices = cc_json_object_get(root, "choices");
    /* 用量或元数据事件允许省略 choices、使用 null 或空数组，仍尝试提取其用量。 */
    if (!choices || cc_json_is_null(choices) ||
        (cc_json_is_array(choices) && cc_json_array_size(choices) == 0)) {
        rc = openai_emit_stream_usage(root, on_chunk, user_data);
        cc_json_destroy(root);
        return rc;
    }
    if (!cc_json_is_array(choices)) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "OpenAI stream choices is not an array");
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
        /* tool_calls 为 null 表示本次没有工具增量，其他非数组值返回协议错误。 */
        if (tool_calls && !cc_json_is_null(tool_calls) && !cc_json_is_array(tool_calls)) {
            const char *type_name = openai_json_type_name(tool_calls);
            cc_json_destroy(root);
            return cc_result_errf(CC_ERR_JSON,
                                  "OpenAI stream tool_calls has invalid type: %s (expected array or null)",
                                  type_name);
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
                double index_number = cc_json_number_value(index_value);
                if (index_value && !cc_json_is_null(index_value) &&
                    (!cc_json_is_number(index_value) || !(index_number >= 0.0 && index_number <= INT_MAX) ||
                     index_number != (double)(int)index_number)) {
                    cc_json_destroy(root);
                    return cc_result_error(CC_ERR_JSON, "OpenAI stream tool index is not a number");
                }
                int tool_index = index_value && !cc_json_is_null(index_value) ? cc_json_int_value(index_value) : i;
                if (tool_index < 0 ||
                    (func && !cc_json_is_null(func) && !cc_json_is_object(func))) {
                    cc_json_destroy(root);
                    return cc_result_error(CC_ERR_JSON, "OpenAI stream tool call is incomplete");
                }

                if (func && !cc_json_is_null(func)) {
                    cc_json_value_t *name_value = cc_json_object_get(func, "name");
                    const char *name = cc_json_string_value(name_value);
                    if (name_value && !cc_json_is_null(name_value) && !name) {
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
                        cc_result_free(&rc);
                    }

                    cc_json_value_t *args_value = cc_json_object_get(func, "arguments");
                    const char *args = cc_json_string_value(args_value);
                    if (args_value && !cc_json_is_null(args_value) && !args) {
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
                        cc_result_free(&rc);
                    }
                }
            }
        }

        cc_json_value_t *reasoning_value = cc_json_object_get(delta, "reasoning_content");
        if (!reasoning_value) reasoning_value = cc_json_object_get(delta, "reasoning");
        if (!reasoning_value) reasoning_value = cc_json_object_get(delta, "reasoning_text");
        if (!reasoning_value) reasoning_value = cc_json_object_get(delta, "thinking");
        const char *reasoning = openai_reasoning_string(delta);
        if (reasoning_value && !cc_json_is_null(reasoning_value) && !cc_json_is_string(reasoning_value)) {
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
            cc_result_free(&rc);
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
            cc_result_free(&rc);
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
        cc_result_free(&rc);
    } else if (finish && strcmp(finish, "stop") == 0) {
        *out_finished = 1;
    }

    rc = openai_emit_stream_usage(root, on_chunk, user_data);
    if (rc.code != CC_OK) {
        cc_json_destroy(root);
        return rc;
    }
    cc_result_free(&rc);

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

/**
 * @brief 将静态 OpenAI 协议交给通用 provider 并复制连接配置。
 * @param[in,out] http_client 有效 HTTP 借用，寿命覆盖请求、回调与 provider 销毁。
 * @param[in] base_url 调用期可选地址，NULL 使用官方默认地址。
 * @param[in] api_key 调用期可选凭据，交通用构造器复制。
 * @param[in] model 调用期可选模型，NULL 使用 gpt-4o-mini。
 * @param[out] out_provider 必填空资源槽，成功交付拥有实例，失败保持原值。
 * @return 通用构造结果与拥有诊断，owner 关闭排空后销毁 provider，HTTP 留归外层。
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
