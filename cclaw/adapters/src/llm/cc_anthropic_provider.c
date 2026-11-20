



#include "cc/adapters/cc_llm_providers.h"
#include "cc/adapters/cc_http_llm_provider.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/internal/cc_json_syntax_check.h"
#include "cc/util/cc_string_builder.h"
#include <ctype.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 解析完整协议 JSON 并拒绝解码 NUL 字符串。
 * @param[in] text 调用期间借用的 NUL 结尾文本，NULL 沿解析器报参数错误。
 * @param[out] out 必填槽，存在时先清空，成功交付拥有树。
 * @return 参数或 JSON 解析结果，失败保持空树，拥有诊断交调用者。
 */
static cc_result_t parse_protocol_json(const char *text, cc_json_value_t **out)
{
    if (out) *out = NULL;
    if (cc_storage_json_has_nul_escape(text))
        return cc_result_error(CC_ERR_JSON, "Anthropic JSON contains a decoded NUL");
    return cc_json_parse_strict(text, out);
}

/** @brief 实例默认与请求覆盖使用的缓存模式索引。 */
typedef enum anthropic_cache_mode {
    ANTHROPIC_CACHE_DISABLED = 0, /**< 不写缓存控制字段。 */
    ANTHROPIC_CACHE_5_MINUTES, /**< 写 ephemeral，采用五分钟默认 TTL。 */
    ANTHROPIC_CACHE_1_HOUR /**< 写 ephemeral 和显式一小时 TTL。 */
} anthropic_cache_mode_t;

/* 各实例只借用不可变模式项，重载后的新旧代之间没有共享可变缓存配置。 */
static const anthropic_cache_mode_t anthropic_cache_modes[] = {
    ANTHROPIC_CACHE_DISABLED, ANTHROPIC_CACHE_5_MINUTES, ANTHROPIC_CACHE_1_HOUR
};

/** @brief 忽略字符大小写比较字段名。 @param[in] a 可选文本借用。 @param[in] b 可选文本借用。 @return 非空文本完整匹配为一，NULL 或不匹配为零。 */
static int anthropic_key_equal(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

/**
 * @brief 从额外请求对象读取缓存模式，其他字段沿既有范围跳过。
 * @param[in] extra_body_json 可选调用期文本借用，NULL 或空串保留模式。
 * @param[in,out] mode 有效独占模式槽，缺字段保留原值，合法配置成功后更新。
 * @return JSON 或缓存字段参数错误，失败不修改模式，临时树在返回前销毁。
 * @note cache_control 须精确拼写且唯一，null 禁用，对象要求 ephemeral 与可选 5m/1h。
 */
static cc_result_t anthropic_cache_from_extra(const char *extra_body_json,
                                               anthropic_cache_mode_t *mode)
{
    if (!extra_body_json || !extra_body_json[0]) return cc_result_ok();
    cc_json_value_t *extra = NULL;
    cc_result_t rc = parse_protocol_json(extra_body_json, &extra);
    if (rc.code != CC_OK) return rc;
    if (!cc_json_is_object(extra)) {
        cc_json_destroy(extra);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "model.extra_body must be a JSON object");
    }
    cc_json_value_t *setting = NULL;
    for (int i = 0; i < cc_json_object_size(extra); ++i) {
        const char *key = cc_json_object_key_at(extra, i);
        if (!anthropic_key_equal(key, "cache_control")) continue;
        if (setting || strcmp(key, "cache_control") != 0) {
            cc_json_destroy(extra);
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Anthropic cache_control is duplicated or misspelled");
        }
        setting = cc_json_object_value_at(extra, i);
    }
    if (!setting) {
        cc_json_destroy(extra);
        return cc_result_ok();
    }
    if (cc_json_is_null(setting)) {
        *mode = ANTHROPIC_CACHE_DISABLED;
        cc_json_destroy(extra);
        return cc_result_ok();
    }
    if (!cc_json_is_object(setting)) {
        cc_json_destroy(extra);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Anthropic cache_control must be null or an object");
    }
    const char *type = NULL;
    const char *ttl = NULL;
    int seen_type = 0, seen_ttl = 0;
    for (int i = 0; i < cc_json_object_size(setting); ++i) {
        const char *key = cc_json_object_key_at(setting, i);
        cc_json_value_t *value = cc_json_object_value_at(setting, i);
        if (key && strcmp(key, "type") == 0 && !seen_type) {
            seen_type = 1;
            type = cc_json_string_value(value);
        } else if (key && strcmp(key, "ttl") == 0 && !seen_ttl) {
            seen_ttl = 1;
            ttl = cc_json_string_value(value);
        } else {
            cc_json_destroy(extra);
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid Anthropic cache_control field");
        }
    }
    if (!type || strcmp(type, "ephemeral") != 0 ||
        (seen_ttl && (!ttl || (strcmp(ttl, "5m") != 0 && strcmp(ttl, "1h") != 0)))) {
        cc_json_destroy(extra);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid Anthropic cache_control type or ttl");
    }
    *mode = ttl && strcmp(ttl, "1h") == 0 ? ANTHROPIC_CACHE_1_HOUR : ANTHROPIC_CACHE_5_MINUTES;
    cc_json_destroy(extra);
    return cc_result_ok();
}

/**
 * @brief 给对象附加拥有缓存控制字段。
 * @param[in,out] body 有效独占目标对象，成功后接管新控制节点。
 * @param[in] mode 有效内部模式，禁用时跳过。
 * @return CC_OK 或 OOM，失败清理未交接节点，目标随后由外层整体丢弃。
 */
static cc_result_t anthropic_add_cache_control(cc_json_value_t *body, anthropic_cache_mode_t mode)
{
    if (mode == ANTHROPIC_CACHE_DISABLED) return cc_result_ok();
    cc_json_value_t *control = cc_json_create_object();
    cc_json_value_t *type = cc_json_create_string("ephemeral");
    if (!control || !type) {
        cc_json_destroy(control);
        cc_json_destroy(type);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot create Anthropic cache control");
    }
    if (!cc_json_object_set_owned(control, "type", type)) {
        cc_json_destroy(control);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot set Anthropic cache control type");
    }
    if (mode == ANTHROPIC_CACHE_1_HOUR) {
        cc_json_value_t *ttl = cc_json_create_string("1h");
        if (!ttl) {
            cc_json_destroy(control);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot create Anthropic cache TTL");
        }
        if (!cc_json_object_set_owned(control, "ttl", ttl)) {
            cc_json_destroy(control);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot set Anthropic cache TTL");
        }
    }
    if (!cc_json_object_set_owned(body, "cache_control", control)) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot attach Anthropic cache control");
    }
    return cc_result_ok();
}

/**
 * @brief 为最后工具及非空 system 文本附加相同 TTL 的稳定断点。
 * @param[in,out] body 有效独占请求，system 字符串可被替换为 text block 数组。
 * @param[in] mode 有效内部缓存模式，禁用时保持请求原字段。
 * @return CC_OK 或分配/挂接错误，失败前可已修改最后工具，外层整体丢弃失败请求。
 */
static cc_result_t anthropic_add_stable_cache_breakpoints(cc_json_value_t *body,
                                                          anthropic_cache_mode_t mode)
{
    if (mode == ANTHROPIC_CACHE_DISABLED) return cc_result_ok();
    cc_json_value_t *tools = cc_json_object_get(body, "tools");
    int tool_count = cc_json_array_size(tools);
    if (tool_count > 0) {
        cc_json_value_t *last_tool = cc_json_array_get(tools, tool_count - 1);
        cc_result_t rc = anthropic_add_cache_control(last_tool, mode);
        if (rc.code != CC_OK) return rc;
    }

    cc_json_value_t *system = cc_json_object_get(body, "system");
    const char *text = cc_json_string_value(system);
    if (!text || !text[0]) return cc_result_ok();
    cc_json_value_t *blocks = cc_json_create_array();
    cc_json_value_t *block = cc_json_create_object();
    cc_json_value_t *type = cc_json_create_string("text");
    cc_json_value_t *content = cc_json_create_string(text);
    if (!blocks || !block || !type || !content) {
        cc_json_destroy(blocks); cc_json_destroy(block);
        cc_json_destroy(type); cc_json_destroy(content);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot create Anthropic system cache block");
    }
    if (!cc_json_object_set_owned(block, "type", type)) {
        cc_json_destroy(content); cc_json_destroy(block); cc_json_destroy(blocks);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot set Anthropic system block type");
    }
    if (!cc_json_object_set_owned(block, "text", content)) {
        cc_json_destroy(block); cc_json_destroy(blocks);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot set Anthropic system block text");
    }
    cc_result_t rc = anthropic_add_cache_control(block, mode);
    if (rc.code != CC_OK) { cc_json_destroy(block); cc_json_destroy(blocks); return rc; }
    if (!cc_json_array_append_owned(blocks, block)) {
        cc_json_destroy(blocks);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot attach Anthropic system block");
    }
    if (!cc_json_object_set_owned(body, "system", blocks)) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot attach Anthropic system blocks");
    }
    return cc_result_ok();
}

/**
 * @brief 扩容并为本次 HTTP 请求复制一个头。
 * @param[in,out] request 独占初始化请求，已有计数对应有效数组。
 * @param[in] name 有效调用期名称借用。
 * @param[in] value 可选调用期值借用，NULL 复制为空串。
 * @return CC_OK 或 OOM，部分项仍可由外层 cleanup 释放。
 */
static cc_result_t add_header(cc_llm_http_request_t *request, const char *name, const char *value)
{
    if (request->header_count >= SIZE_MAX / sizeof(cc_http_header_t))
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Too many Anthropic HTTP headers");
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

/** @brief 提供能力映射的协议名。 @param[in] self 未使用，可为 NULL。 @return 静态 anthropic 名称借用。 */
static const char *anthropic_name(void *self)
{
    (void)self;
    return "anthropic";
}

/**
 * @brief 深拷贝角色和文本以构造一条消息。
 * @param[in] role 有效调用期角色借用。
 * @param[in] content 可选调用期文本，NULL 使用空串。
 * @return 拥有对象或失败 NULL，调用者挂接或销毁。
 */
static cc_json_value_t *anthropic_text_message(const char *role, const char *content)
{
    cc_json_value_t *message = cc_json_create_object();
    if (!message || !cc_json_object_set_owned(message, "role", cc_json_create_string(role)) ||
        !cc_json_object_set_owned(message, "content", cc_json_create_string(content ? content : ""))) {
        cc_json_destroy(message);
        return NULL;
    }
    return message;
}

/**
 * @brief 构造 content block 消息并接管内容数组。
 * @param[in] role 有效调用期角色借用。
 * @param[in,out] content 独立拥有数组，NULL 时创建空数组。
 * @return 拥有消息或 NULL，内容的消费规则由本函数统一承担。
 */
static cc_json_value_t *anthropic_block_message(const char *role, cc_json_value_t *content)
{
    cc_json_value_t *message = cc_json_create_object();
    if (!message || !cc_json_object_set_owned(message, "role", cc_json_create_string(role))) {
        cc_json_destroy(content); cc_json_destroy(message);
        return NULL;
    }
    if (!cc_json_object_set_owned(message, "content", content ? content : cc_json_create_array())) {
        cc_json_destroy(message);
        return NULL;
    }
    return message;
}

/**
 * @brief 为未直接映射的资源构造元数据文本。
 * @param[in] part 调用期节点借用，读取身份、MIME、路径与正尺寸。
 * @return 拥有文本或 NULL，调用者 free。
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
    cc_result_t rc = cc_string_builder_init(&sb);
    if (rc.code == CC_OK) rc = cc_string_builder_appendf(&sb, "[anthropic multimodal fallback: type=%s", type ? type : "file");
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
 * @brief 创建并追加一个 text 内容块。
 * @param[in,out] blocks 有效独占目标数组，接管成功节点。
 * @param[in] text 可选调用期文本借用。
 * @return 完整创建并挂接为一，失败为零并归还未交接节点，空文本也生成 text 块。
 */
static int anthropic_append_text_block(cc_json_value_t *blocks, const char *text)
{
    cc_json_value_t *block = cc_json_create_object();
    if (!block || !cc_json_object_set_owned(block, "type", cc_json_create_string("text")) ||
        !cc_json_object_set_owned(block, "text", cc_json_create_string(text ? text : ""))) {
        cc_json_destroy(block);
        return 0;
    }
    return cc_json_array_append_owned(blocks, block);
}

/**
 * @brief 转换文本、内联图片及资源描述为 Anthropic 内容数组。
 * @param[in] parts 调用期源数组借用，源树拥有权保留。
 * @return 拥有内容数组或失败 NULL，调用者挂接或销毁。
 */
static cc_json_value_t *anthropic_content_blocks_from_parts(const cc_json_value_t *parts)
{
    cc_json_value_t *blocks = cc_json_create_array();
    if (!blocks) return NULL;
    int count = cc_json_array_size(parts);
    for (int i = 0; i < count; ++i) {
        cc_json_value_t *part = cc_json_array_get(parts, i);
        const char *type = cc_json_string_value(cc_json_object_get(part, "type"));
        if (type && strcmp(type, "text") == 0) {
            if (!anthropic_append_text_block(blocks,
                cc_json_string_value(cc_json_object_get(part, "text")))) goto fail;
        } else if (type && strcmp(type, "image") == 0) {
            const char *data = cc_json_string_value(cc_json_object_get(part, "data_base64"));
            const char *mime = cc_json_string_value(cc_json_object_get(part, "mime"));
            if (data && *data) {
                cc_json_value_t *block = cc_json_create_object();
                cc_json_value_t *source = cc_json_create_object();
                if (!block || !source || !cc_json_object_set_owned(block, "type", cc_json_create_string("image")) ||
                    !cc_json_object_set_owned(source, "type", cc_json_create_string("base64")) ||
                    !cc_json_object_set_owned(source, "media_type", cc_json_create_string((mime && *mime) ? mime : "image/png")) ||
                    !cc_json_object_set_owned(source, "data", cc_json_create_string(data))) {
                    cc_json_destroy(block); cc_json_destroy(source); goto fail;
                }
                if (!cc_json_object_set_owned(block, "source", source)) { cc_json_destroy(block); goto fail; }
                if (!cc_json_array_append_owned(blocks, block)) goto fail;
            } else {
                char *fallback = anthropic_describe_part(part);
                int added = fallback && anthropic_append_text_block(blocks, fallback);
                free(fallback);
                if (!added) goto fail;
            }
        } else {
            char *fallback = anthropic_describe_part(part);
            int added = fallback && anthropic_append_text_block(blocks, fallback);
            free(fallback);
            if (!added) goto fail;
        }
    }
    if (cc_json_array_size(blocks) == 0) {
        if (!anthropic_append_text_block(blocks, "")) goto fail;
    }
    return blocks;
fail:
    cc_json_destroy(blocks);
    return NULL;
}

/**
 * @brief 把内容项按换行合并成文本，非文本项使用资源描述。
 * @param[in] parts 调用期源数组借用，NULL 或非数组按零项处理。
 * @return 拥有文本或失败 NULL，调用者 free。
 */
static char *anthropic_text_from_parts(const cc_json_value_t *parts)
{
    cc_string_builder_t sb;
    cc_result_t rc = cc_string_builder_init(&sb);
    if (rc.code != CC_OK) { cc_result_free(&rc); return NULL; }
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
            if (!owned) { cc_string_builder_deinit(&sb); return NULL; }
            text = owned;
        }
        if (text && *text) {
            if (sb.length > 0) rc = cc_string_builder_append(&sb, "\n");
            if (rc.code == CC_OK) rc = cc_string_builder_append(&sb, text);
        }
        free(owned);
        if (rc.code != CC_OK) { cc_result_free(&rc); cc_string_builder_deinit(&sb); return NULL; }
    }
    return cc_string_builder_take(&sb);
}

/**
 * @brief 保留 assistant 文本及工具调用身份，转换为 Anthropic tool_use 内容块。
 * @param[in] msg 调用期 SDK 消息节点借用，tool_calls 参数沿统一字符串 JSON 表示。
 * @param[out] out 有效空槽，成功交付拥有消息，错误保持 NULL。
 * @return JSON、工具字段或 OOM 结果，所有临时块和参数树在失败时归还。
 */
static cc_result_t anthropic_assistant_message(const cc_json_value_t *msg, cc_json_value_t **out)
{
    *out = NULL;
    cc_json_value_t *content = cc_json_object_get(msg, "content");
    char *owned_text = cc_json_is_array(content) ? anthropic_text_from_parts(content) : NULL;
    if (cc_json_is_array(content) && !owned_text)
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot flatten Anthropic assistant content");
    const char *text = owned_text ? owned_text : cc_json_string_value(content);
    cc_json_value_t *calls = cc_json_object_get(msg, "tool_calls");
    if (calls && !cc_json_is_null(calls) && !cc_json_is_array(calls)) {
        free(owned_text);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Anthropic assistant tool_calls must be an array");
    }
    if (cc_json_array_size(calls) == 0) {
        *out = anthropic_text_message("assistant", text);
        free(owned_text);
        return *out ? cc_result_ok() : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot create Anthropic assistant message");
    }
    cc_json_value_t *blocks = cc_json_create_array();
    cc_result_t rc = cc_result_ok();
    if (!blocks || (text && *text && !anthropic_append_text_block(blocks, text))) goto oom;
    free(owned_text); owned_text = NULL;
    for (int i = 0; i < cc_json_array_size(calls); i++) {
        cc_json_value_t *call = cc_json_array_get(calls, i);
        cc_json_value_t *function = cc_json_object_get(call, "function");
        const char *id = cc_json_string_value(cc_json_object_get(call, "id"));
        const char *name = cc_json_string_value(cc_json_object_get(function, "name"));
        cc_json_value_t *arguments = cc_json_object_get(function, "arguments");
        const char *arguments_text = cc_json_string_value(arguments);
        if (!cc_json_is_object(call) || !cc_json_is_object(function) || !id || !*id || !name || !*name ||
            (arguments && !cc_json_is_null(arguments) && !arguments_text)) {
            rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Anthropic assistant tool call is incomplete");
            goto error;
        }
        cc_json_value_t *input = NULL;
        rc = parse_protocol_json(arguments_text ? arguments_text : "{}", &input);
        if (rc.code != CC_OK) goto error;
        if (!cc_json_is_object(input)) {
            cc_json_destroy(input);
            rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Anthropic tool input must be a JSON object");
            goto error;
        }
        cc_json_value_t *block = cc_json_create_object();
        if (!block || !cc_json_object_set_owned(block, "type", cc_json_create_string("tool_use")) ||
            !cc_json_object_set_owned(block, "id", cc_json_create_string(id)) ||
            !cc_json_object_set_owned(block, "name", cc_json_create_string(name))) {
            cc_json_destroy(input); cc_json_destroy(block); goto oom;
        }
        if (!cc_json_object_set_owned(block, "input", input)) { cc_json_destroy(block); goto oom; }
        if (!cc_json_array_append_owned(blocks, block)) goto oom;
    }
    *out = anthropic_block_message("assistant", blocks);
    return *out ? cc_result_ok() : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot attach Anthropic assistant blocks");
oom:
    rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot build Anthropic assistant tool blocks");
error:
    free(owned_text);
    cc_json_destroy(blocks);
    return rc;
}

/**
 * @brief 将 SDK tool 消息转换为带原调用 ID 和原内容的 tool_result 块。
 * @param[in] msg 调用期 SDK 节点借用，须有非空 tool_call_id。
 * @param[out] out 有效空槽，成功交付拥有块，失败保持 NULL。
 * @return 字段或 OOM 结果，DTO 无 is_error 字段，本函数保留结果正文且不推断业务错误。
 */
static cc_result_t anthropic_tool_result_block(const cc_json_value_t *msg, cc_json_value_t **out)
{
    *out = NULL;
    const char *id = cc_json_string_value(cc_json_object_get(msg, "tool_call_id"));
    if (!id || !*id) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Anthropic tool result has no call id");
    cc_json_value_t *content = cc_json_object_get(msg, "content");
    cc_json_value_t *result_content = cc_json_is_array(content) ? anthropic_content_blocks_from_parts(content) :
        cc_json_create_string(cc_json_string_value(content));
    cc_json_value_t *block = cc_json_create_object();
    if (!result_content || !block ||
        !cc_json_object_set_owned(block, "type", cc_json_create_string("tool_result")) ||
        !cc_json_object_set_owned(block, "tool_use_id", cc_json_create_string(id))) {
        cc_json_destroy(result_content); cc_json_destroy(block);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot create Anthropic tool result");
    }
    if (!cc_json_object_set_owned(block, "content", result_content)) {
        cc_json_destroy(block);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot attach Anthropic tool result content");
    }
    *out = block;
    return cc_result_ok();
}

/**
 * @brief 将 SDK 消息文本转换为独立 messages 与合并 system 字段。
 * @param[in,out] body 有效独占请求体，成功接管目标消息。
 * @param[in] messages_text 调用期源 JSON 文本借用。
 * @return JSON、字段或 OOM 结果，失败前可已修改目标字段，外层整体丢弃请求，临时资源全部归还。
 */
static cc_result_t append_anthropic_messages(
    cc_json_value_t *body,
    const char *messages_text
)
{
    cc_json_value_t *src = NULL;
    cc_result_t rc = parse_protocol_json(messages_text, &src);
    if (rc.code != CC_OK) return rc;
    if (!cc_json_is_array(src)) { cc_json_destroy(src); return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Anthropic messages must be an array"); }

    cc_json_value_t *messages = cc_json_create_array();
    cc_string_builder_t system;
    rc = cc_string_builder_init(&system);
    if (rc.code != CC_OK) { cc_json_destroy(src); cc_json_destroy(messages); return rc; }
    if (!messages) goto oom;

    int count = cc_json_array_size(src);
    int preceding_tool_result = 0;
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
                if (!parts_text) goto oom;
                content = parts_text;
            }
            if (content && *content) {
                if (system.length > 0) rc = cc_string_builder_append(&system, "\n");
                if (rc.code == CC_OK) rc = cc_string_builder_append(&system, content);
            }
            free(parts_text);
            if (rc.code != CC_OK) goto error;
            continue;
        }

        cc_json_value_t *converted = NULL;
        if (strcmp(role, "assistant") == 0) {
            rc = anthropic_assistant_message(msg, &converted);
        } else if (strcmp(role, "tool") == 0) {
            rc = anthropic_tool_result_block(msg, &converted);
            if (rc.code != CC_OK) goto error;
            if (preceding_tool_result) {
                cc_json_value_t *previous = cc_json_array_get(messages, cc_json_array_size(messages) - 1);
                if (!cc_json_array_append_owned(cc_json_object_get(previous, "content"), converted)) goto oom;
                continue;
            }
            cc_json_value_t *blocks = cc_json_create_array();
            if (!blocks) { cc_json_destroy(converted); goto oom; }
            if (!cc_json_array_append_owned(blocks, converted)) { cc_json_destroy(blocks); goto oom; }
            converted = anthropic_block_message("user", blocks);
        } else {
            if (content_value && cc_json_is_array(content_value)) {
                cc_json_value_t *blocks = anthropic_content_blocks_from_parts(content_value);
                if (!blocks) goto oom;
                converted = anthropic_block_message("user", blocks);
            } else {
                converted = anthropic_text_message("user", content ? content : "");
            }
        }
        if (rc.code != CC_OK) goto error;
        if (!converted || !cc_json_array_append_owned(messages, converted)) goto oom;
        preceding_tool_result = strcmp(role, "tool") == 0;
    }

    if (system.length > 0) {
        if (!cc_json_object_set_owned(body, "system", cc_json_create_string(cc_string_builder_cstr(&system)))) goto oom;
    }
    cc_string_builder_deinit(&system);
    cc_json_destroy(src);
    if (!cc_json_object_set_owned(body, "messages", messages))
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot attach Anthropic messages");
    return cc_result_ok();
oom:
    rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot convert Anthropic messages");
error:
    cc_json_destroy(messages); cc_json_destroy(src); cc_string_builder_deinit(&system);
    return rc;
}

/**
 * @brief 将统一函数工具描述映射为 Anthropic 名称、说明与输入 schema。
 * @param[in,out] body 有效独占请求体，接管成功 tools 数组。
 * @param[in] tools_json 可选调用期 JSON 文本借用，未提供时跳过。
 * @return JSON、工具字段或 OOM 结果，临时树和未交接节点清理，成功字段归请求体。
 */
static cc_result_t append_anthropic_tools(cc_json_value_t *body, const char *tools_json)
{
    if (!tools_json) return cc_result_ok();
    cc_json_value_t *src = NULL;
    cc_result_t rc = parse_protocol_json(tools_json, &src);
    if (rc.code != CC_OK) return rc;
    if (!cc_json_is_array(src)) { cc_json_destroy(src); return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Anthropic tools must be an array"); }

    cc_json_value_t *tools = cc_json_create_array();
    if (!tools) goto oom;
    int count = cc_json_array_size(src);
    for (int i = 0; i < count; i++) {
        cc_json_value_t *tool = cc_json_array_get(src, i);
        cc_json_value_t *func = cc_json_object_get(tool, "function");
        if (!cc_json_is_object(tool) || !cc_json_is_object(func)) goto invalid;

        cc_json_value_t *anthropic_tool = cc_json_create_object();
        const char *name = cc_json_string_value(cc_json_object_get(func, "name"));
        const char *description = cc_json_string_value(cc_json_object_get(func, "description"));
        if (!name || !*name) { cc_json_destroy(anthropic_tool); goto invalid; }
        if (!anthropic_tool || !cc_json_object_set_owned(anthropic_tool, "name", cc_json_create_string(name)) ||
            !cc_json_object_set_owned(anthropic_tool, "description", cc_json_create_string(description ? description : ""))) {
            cc_json_destroy(anthropic_tool); goto oom;
        }

        cc_json_value_t *params = cc_json_object_get(func, "parameters");
        cc_json_value_t *schema_copy = NULL;
        if (params) {
            if (!cc_json_is_object(params)) { cc_json_destroy(anthropic_tool); goto invalid; }
            char *schema = cc_json_stringify_unformatted(params);
            if (!schema) { cc_json_destroy(anthropic_tool); goto oom; }
            rc = parse_protocol_json(schema, &schema_copy);
            free(schema);
            if (rc.code != CC_OK) { cc_json_destroy(anthropic_tool); goto error; }
        } else {
            schema_copy = cc_json_create_object();
        }
        if (!cc_json_object_set_owned(anthropic_tool, "input_schema", schema_copy)) { cc_json_destroy(anthropic_tool); goto oom; }
        if (!cc_json_array_append_owned(tools, anthropic_tool)) goto oom;
    }

    if (cc_json_array_size(tools) > 0) {
        if (!cc_json_object_set_owned(body, "tools", tools)) { cc_json_destroy(src); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot attach Anthropic tools"); }
    } else {
        cc_json_destroy(tools);
    }
    cc_json_destroy(src);
    return cc_result_ok();
invalid:
    rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Anthropic tool schema is incomplete");
    goto error;
oom:
    rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot convert Anthropic tools");
error:
    cc_json_destroy(tools); cc_json_destroy(src);
    return rc;
}

/**
 * @brief 构造 Anthropic Messages API 请求并应用实例/请求缓存模式。
 * @param[in] self 可选不可变模式借用，NULL 使用五分钟模式。
 * @param[in] base_url 有效调用期地址借用，追加 /v1/messages。
 * @param[in] api_key 可选调用期凭据借用，复制到请求与 x-api-key 头。
 * @param[in] default_model 有效默认模型借用，请求模型优先。
 * @param[in] request 有效统一请求借用，全部字段保持到返回。
 * @param[in] stream 非零选择 SSE。
 * @param[in,out] out_request 有效空请求槽，拥有字段在成功或失败后均交外层 cleanup。
 * @return 缓存配置、消息转换、分配或挂接结果，失败清理本次临时树与请求。
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
    anthropic_cache_mode_t cache_mode = self ? *(const anthropic_cache_mode_t *)self : ANTHROPIC_CACHE_5_MINUTES;
    cc_result_t cache_rc = anthropic_cache_from_extra(request->extra_body_json, &cache_mode);
    if (cache_rc.code != CC_OK) return cache_rc;
    memset(out_request, 0, sizeof(*out_request));
    out_request->stream_kind = stream ? CC_LLM_STREAM_SSE : CC_LLM_STREAM_NONE;
    out_request->defer_finished_until_end = stream != 0;

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
    if (!cc_json_object_set_owned(body, "model", cc_json_create_string(request->model ? request->model : default_model)) ||
        !cc_json_object_set_owned(body, "max_tokens", cc_json_create_number(request->max_tokens)) ||
        !cc_json_object_set_owned(body, "temperature", cc_json_create_number(request->temperature)) ||
        !cc_json_object_set_owned(body, "stream", cc_json_create_bool(stream))) {
        rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot create complete Anthropic request");
        goto error;
    }
    cache_rc = anthropic_add_cache_control(body, cache_mode);
    if (cache_rc.code != CC_OK) {
        cc_json_destroy(body);
        cc_llm_http_request_cleanup(out_request);
        return cache_rc;
    }
    char *messages_text = NULL;
    cc_result_t msg_rc = cc_messages_to_json(
        request->messages,
        request->message_count,
        request->thinking_mode,
        &messages_text);
    if (msg_rc.code == CC_OK) msg_rc = append_anthropic_messages(body, messages_text);
    free(messages_text);
    if (msg_rc.code != CC_OK) { rc = msg_rc; goto error; }
    rc = append_anthropic_tools(body, request->tools_json);
    if (rc.code != CC_OK) goto error;
    cache_rc = anthropic_add_stable_cache_breakpoints(body, cache_mode);
    if (cache_rc.code != CC_OK) {
        cc_json_destroy(body);
        cc_llm_http_request_cleanup(out_request);
        return cache_rc;
    }

    out_request->body_json = cc_json_stringify_unformatted(body);
    cc_json_destroy(body);
    if (!out_request->url || !out_request->body_json ||
        (api_key && !out_request->api_key)) {
        cc_llm_http_request_cleanup(out_request);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build Anthropic request");
    }
    return cc_result_ok();
error:
    cc_json_destroy(body);
    cc_llm_http_request_cleanup(out_request);
    return rc;
}

/**
 * @brief 校验单个非负整数 token 计数并复制到输出。
 * @param[in] usage 调用期对象借用。
 * @param[in] key 有效字段名借用。
 * @param[out] out 有效槽，仅合法整数时写入，其他情况保持原值。
 * @return 有效含零为一，缺失、类型错误、小数或超 uint64_t 可表示范围为零。
 */
static int anthropic_token_count(const cc_json_value_t *usage, const char *key, uint64_t *out)
{
    const cc_json_value_t *value=cc_json_object_get(usage,key);
    if (!value || !cc_json_is_number(value)) return 0;
    double number=cc_json_number_value(value);
    if (!(number>=0.0) || number >= (double)UINT64_MAX) return 0;
    uint64_t count=(uint64_t)number;
    if ((double)count!=number) return 0;
    *out=count; return 1;
}

/**
 * @brief 解析输入、缓存读写与可选输出用量，保留报告与有效性区别。
 * @param[in] usage 调用期对象借用，缺失或非对象输出空值。
 * @param[in] include_output 非零读取 output_tokens，message_start 阶段传零。
 * @param[out] out 有效值槽，入口清零。
 * @post 三项输入计数齐全且合计不溢出时填写 prompt_tokens，缓存字段独立保留 reported/valid，合法零可报告。
 */
static void anthropic_parse_usage(const cc_json_value_t *usage, int include_output,
                                  cc_llm_usage_t *out)
{
    memset(out,0,sizeof(*out));
    if (!usage || !cc_json_is_object(usage)) return;
    uint64_t input=0,read=0,write=0,output=0;
    int has_input=anthropic_token_count(usage,"input_tokens",&input);
    int has_read=anthropic_token_count(usage,"cache_read_input_tokens",&read);
    int has_write=anthropic_token_count(usage,"cache_creation_input_tokens",&write);
    int has_output=include_output && anthropic_token_count(usage,"output_tokens",&output);
    out->cached_tokens_reported=cc_json_object_get(usage,"cache_read_input_tokens")!=NULL;
    out->cache_write_tokens_reported=cc_json_object_get(usage,"cache_creation_input_tokens")!=NULL;
    out->valid=has_input || has_read || has_write || has_output ||
        out->cached_tokens_reported || out->cache_write_tokens_reported;
    out->cached_tokens_valid=has_read; out->cached_tokens=read;
    out->cache_write_tokens_valid=has_write; out->cache_write_tokens=write;
    if (has_input && has_read && has_write && input<=UINT64_MAX-read && input+read<=UINT64_MAX-write)
        out->prompt_tokens=input+read+write;
    if (has_output) out->completion_tokens=output;
}

/**
 * @brief 把本事件的可观察用量转换为同步 usage 块。
 * @param[in] usage 调用期用量对象借用。
 * @param[in] include_output 是否读取输出计数。
 * @param[in] on_chunk 有效同步块回调，按值 usage 仅借用到回调返回。
 * @param[in,out] user_data 可选回调上下文借用。
 * @return 无用量为 CC_OK，其他返回回调拥有结果供上层处理。
 */
static cc_result_t anthropic_emit_usage(const cc_json_value_t *usage, int include_output,
    cc_llm_stream_callback_fn on_chunk, void *user_data)
{
    cc_stream_chunk_t chunk={0};
    chunk.type=CC_STREAM_CHUNK_USAGE; chunk.tool_index=-1;
    anthropic_parse_usage(usage,include_output,&chunk.usage);
    return chunk.usage.valid ? on_chunk(&chunk,user_data) : cc_result_ok();
}

/**
 * @brief 聚合 Anthropic text/tool_use 内容并解析用量与结束原因。
 * @param[in] self 此解析不读取缓存模式，允许 NULL。
 * @param[in] response_json 调用期间借用的有效响应文本。
 * @param[out] out_response 有效空资源槽，入口初始化，拥有输出交上层响应清理。
 * @return JSON、字段、模型或分配结果，拥有诊断交调用者，失败归还输出字段，临时树与文本 builder 清理。
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
    cc_result_t rc = parse_protocol_json(response_json, &root);
    if (rc.code != CC_OK) return rc;
    if (!cc_json_is_object(root)) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "Anthropic response is not an object");
    }

    cc_json_value_t *api_error = cc_json_object_get(root, "error");
    if (api_error && !cc_json_is_null(api_error)) {
        const char *msg = cc_json_string_value(cc_json_object_get(api_error, "message"));
        cc_result_t err = cc_result_error(CC_ERR_MODEL, msg ? msg : "Unknown Anthropic API error");
        cc_json_destroy(root);
        return err;
    }
    anthropic_parse_usage(cc_json_object_get(root,"usage"),1,&out_response->usage);

    cc_string_builder_t text = {0};
    cc_json_value_t *content = cc_json_object_get(root, "content");
    if (content && !cc_json_is_array(content)) goto invalid;
    if (content && cc_json_is_array(content)) {
        rc = cc_string_builder_init(&text);
        if (rc.code != CC_OK) goto error;
        int count = cc_json_array_size(content);
        for (int i = 0; i < count; i++) {
            cc_json_value_t *block = cc_json_array_get(content, i);
            const char *type = cc_json_string_value(cc_json_object_get(block, "type"));
            if (!cc_json_is_object(block) || !type || !*type) goto invalid;
            if (type && strcmp(type, "text") == 0) {
                const char *block_text = cc_json_string_value(cc_json_object_get(block, "text"));
                if (!block_text) goto invalid;
                rc = cc_string_builder_append(&text, block_text);
                if (rc.code != CC_OK) goto error;
            } else if (type && strcmp(type, "tool_use") == 0) {
                const char *id = cc_json_string_value(cc_json_object_get(block, "id"));
                const char *name = cc_json_string_value(cc_json_object_get(block, "name"));
                cc_json_value_t *input = cc_json_object_get(block, "input");
                if (!id || !*id || !name || !*name || (input && !cc_json_is_object(input))) goto invalid;
                char *args = input ? cc_json_stringify_unformatted(input) : cc_copy_string("{}");
                if (!args) goto oom;
                rc = cc_llm_response_add_tool_call(out_response, id, name, args);
                free(args);
                if (rc.code != CC_OK) goto error;
                cc_result_free(&rc);
            }
        }
        if (text.length > 0) {
            char *taken = cc_string_builder_take(&text);
            if (!taken) goto oom;
            rc = cc_llm_response_set_text(out_response, taken);
            free(taken);
            if (rc.code != CC_OK) goto error;
        } else {
            cc_string_builder_deinit(&text);
        }
    }

    cc_json_value_t *stop_reason_value = cc_json_object_get(root, "stop_reason");
    const char *stop_reason = cc_json_string_value(stop_reason_value);
    if (stop_reason_value && !cc_json_is_null(stop_reason_value) && !stop_reason) goto invalid;
    out_response->finished = (stop_reason && strcmp(stop_reason, "end_turn") == 0) ? 1 : 0;
    cc_json_destroy(root);
    return cc_result_ok();
invalid:
    rc = cc_result_error(CC_ERR_JSON, "Anthropic response fields are invalid");
    goto error;
oom:
    rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy Anthropic response");
error:
    cc_string_builder_deinit(&text);
    cc_json_destroy(root);
    cc_llm_response_free(out_response);
    return rc;
}

/**
 * @brief 解析单个 SSE 正文并同步交付内容增量、工具边界及用量。
 * @param[in] self 此解析不读取缓存模式，允许 NULL。
 * @param[in] event_json 有效事件文本借用。
 * @param[in] on_chunk 有效同步回调，树内字符串仅借用到回调返回。
 * @param[in,out] user_data 可选回调上下文借用。
 * @param[out] out_finished 必填，参数通过后清零，end_turn/max_tokens/stop_sequence/pause_turn/refusal 或 message_stop 时置一。
 * @return 参数、JSON、模型或下游回调结果，失败前可已交付部分块，临时树统一销毁。
 * @note tool_use 原因交付 TOOL_END，缓存模式只在构造请求时读取，通用层保留语义结束后的 usage 接收期。
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
    cc_result_t rc = parse_protocol_json(event_json, &root);
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
    double index_number = cc_json_number_value(index_value);
    if (index_value && (!cc_json_is_number(index_value) ||
        !(index_number >= 0.0 && index_number <= INT_MAX) || index_number != (double)(int)index_number)) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "Anthropic content block index is not a number");
    }
    int block_index = index_value ? cc_json_int_value(index_value) : -1;
    if (strcmp(type, "message_start") == 0) {
        const cc_json_value_t *message=cc_json_object_get(root,"message");
        rc=anthropic_emit_usage(cc_json_object_get(message,"usage"),0,on_chunk,user_data);
        if (rc.code!=CC_OK) { cc_json_destroy(root); return rc; }
        cc_result_free(&rc);
    } else if (type && strcmp(type, "content_block_start") == 0) {
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
                cc_result_free(&rc);
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
                cc_result_free(&rc);
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
                cc_result_free(&rc);
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
                cc_result_free(&rc);
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
        /* 文本和思考块也在此结束，工具完成统一按 message_delta 的 tool_use 原因处理。 */
    } else if (type && strcmp(type, "message_delta") == 0) {
        cc_json_value_t *delta = cc_json_object_get(root, "delta");
        if (!delta || !cc_json_is_object(delta)) {
            cc_json_destroy(root);
            return cc_result_error(CC_ERR_JSON, "Anthropic message delta is not an object");
        }
        rc=anthropic_emit_usage(cc_json_object_get(root,"usage"),1,on_chunk,user_data);
        if (rc.code!=CC_OK) { cc_json_destroy(root); return rc; }
        cc_result_free(&rc);
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
            cc_result_free(&rc);
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

/* 协议状态借用不可变静态模式项，操作表不需要析构回调。 */
static cc_llm_protocol_vtable_t anthropic_protocol_vtable = {
    anthropic_name,
    anthropic_build_request,
    anthropic_parse_response,
    anthropic_parse_stream_event,
    NULL
};

/**
 * @brief 以默认五分钟缓存配置调用带配置创建器。
 * @param[in,out] http_client 有效 HTTP 借用，覆盖全部请求、回调与 provider 销毁。
 * @param[in] base_url 可选调用期地址借用，NULL 使用官方默认。
 * @param[in] api_key 可选调用期凭据借用，创建时复制。
 * @param[in] model 可选调用期模型借用，NULL 使用默认 Haiku 模型。
 * @param[out] out_provider 必填空资源槽，成功移交拥有 provider，失败保持原值。
 * @return 带配置创建结果及拥有诊断，外层关闭排空后消费成功 provider。
 */
cc_result_t cc_anthropic_provider_create(
    cc_http_client_t *http_client,
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_provider_t *out_provider
)
{
    return cc_anthropic_provider_create_configured(http_client, base_url, api_key, model, NULL, out_provider);
}

/**
 * @brief 校验实例缓存配置并向通用 provider 交付不可变协议模式。
 * @param[in,out] http_client 有效借用 HTTP，底层由外层拥有并保持到 provider 销毁。
 * @param[in] base_url 可选调用期地址借用，NULL 使用 https://api.anthropic.com。
 * @param[in] api_key 可选调用期凭据借用，交通用构造器复制。
 * @param[in] model 可选调用期模型借用，NULL 使用 claude-3-5-haiku-latest。
 * @param[in] extra_body_json 可选调用期 JSON 借用，只提取缓存控制，缺失默认五分钟。
 * @param[out] out_provider 必填空资源槽，配置或构造失败保持原值，成功接管 provider。
 * @return 配置错误或通用构造结果，拥有诊断交调用者，临时配置树在返回前释放。
 */
cc_result_t cc_anthropic_provider_create_configured(
    cc_http_client_t *http_client,
    const char *base_url,
    const char *api_key,
    const char *model,
    const char *extra_body_json,
    cc_llm_provider_t *out_provider
)
{
    anthropic_cache_mode_t cache_mode = ANTHROPIC_CACHE_5_MINUTES;
    cc_result_t rc = anthropic_cache_from_extra(extra_body_json, &cache_mode);
    if (rc.code != CC_OK) return rc;
    cc_llm_protocol_t protocol = { (void *)&anthropic_cache_modes[cache_mode], &anthropic_protocol_vtable };
    return cc_http_llm_provider_create(
        http_client,
        base_url ? base_url : "https://api.anthropic.com",
        api_key,
        model ? model : "claude-3-5-haiku-latest",
        protocol,
        out_provider);
}
