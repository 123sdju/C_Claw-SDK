#include "cc/core/cc_media.h"

#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>




/*
 * 安全获取可空字符串长度。
 *
 * media 校验里很多字段可选，统一把 NULL 当作长度 0 可以减少调用点分支。
 */
static size_t string_len(const char *s)
{
    return s ? strlen(s) : 0;
}

/*
 * 判断字符串是否匹配前缀。
 *
 * MIME allowlist 使用前缀而不是完整等值匹配，方便表达 image/、audio/ 这类
 * 协议族；NULL 输入直接失败，避免校验路径里解引用空指针。
 */
static int starts_with(const char *s, const char *prefix)
{
    size_t n = string_len(prefix);
    return s && prefix && strncmp(s, prefix, n) == 0;
}

/*
 * media kind 到协议字符串的映射。
 *
 * provider adapter、JSON 序列化和日志都复用这里的名字，避免模块间出现 image/
 * img 等不一致拼写。未知值按 file 输出，代表最保守的二进制附件语义。
 */
const char *cc_media_kind_string(cc_media_kind_t kind)
{
    switch (kind) {
    case CC_MEDIA_TEXT: return "text";
    case CC_MEDIA_IMAGE: return "image";
    case CC_MEDIA_AUDIO: return "audio";
    case CC_MEDIA_VIDEO: return "video";
    case CC_MEDIA_FILE: return "file";
    default: return "file";
    }
}

/*
 * 从协议字符串解析 media kind。
 *
 * 解析策略保持宽松：NULL 或 text 返回文本，未知值落到 file。更严格的能力检查
 * 留给 limits/provider capability 层完成。
 */
cc_media_kind_t cc_media_kind_from_string(const char *kind)
{
    if (!kind || strcmp(kind, "text") == 0) return CC_MEDIA_TEXT;
    if (strcmp(kind, "image") == 0) return CC_MEDIA_IMAGE;
    if (strcmp(kind, "audio") == 0) return CC_MEDIA_AUDIO;
    if (strcmp(kind, "video") == 0) return CC_MEDIA_VIDEO;
    if (strcmp(kind, "file") == 0) return CC_MEDIA_FILE;
    return CC_MEDIA_FILE;
}

/*
 * 配置文本-only 媒体限制。
 *
 * 低资源 MCU 或不支持多模态的 provider 可以直接使用该配置，让 artifact 数量为 0
 * 且禁止 inline base64，从入口处阻断高内存/高带宽内容。
 */
void cc_media_limits_text_only(cc_media_limits_t *limits)
{
    if (!limits) return;
    memset(limits, 0, sizeof(*limits));
    limits->max_artifacts = 0;
    limits->allow_inline_base64 = 0;
}

/*
 * 配置默认多模态限制。
 *
 * 默认值给 desktop/embedded Linux profile 一个可用上限，但仍保守限制 artifact
 * 数量和 inline base64 大小。产品侧可以根据 RAM、flash、网络预算继续调小。
 */
void cc_media_limits_default_multimodal(cc_media_limits_t *limits)
{
    if (!limits) return;
    memset(limits, 0, sizeof(*limits));
    limits->max_artifacts = 8;
    limits->max_artifact_bytes = 10u * 1024u * 1024u;
    limits->max_base64_bytes = 2u * 1024u * 1024u;
    limits->allow_inline_base64 = 1;
}

/*
 * 初始化单个 artifact。
 *
 * 默认 kind 设为 file，是因为缺少类型信息时 file 是最通用、最少假设的表达。
 */
void cc_media_artifact_init(cc_media_artifact_t *artifact)
{
    if (!artifact) return;
    memset(artifact, 0, sizeof(*artifact));
    artifact->kind = CC_MEDIA_FILE;
}

/*
 * 清理 artifact 拥有的所有字符串资源。
 *
 * artifact 通常作为结构体成员嵌入在 content part 或数组中，因此只释放内部字段
 * 并清零，不释放 artifact 指针本身。
 */
void cc_media_artifact_cleanup(cc_media_artifact_t *artifact)
{
    if (!artifact) return;
    free(artifact->id);
    free(artifact->mime);
    free(artifact->path);
    free(artifact->uri);
    free(artifact->data_base64);
    free(artifact->created_at);
    free(artifact->metadata);
    memset(artifact, 0, sizeof(*artifact));
}

/*
 * 深拷贝 artifact。
 *
 * 多模态内容可能跨线程、跨 session store 或进入 provider 异步请求，因此不能共享
 * 调用方临时字符串。任一字段复制失败都会 cleanup dst，保持错误路径简单。
 */
cc_result_t cc_media_artifact_copy(const cc_media_artifact_t *src, cc_media_artifact_t *dst)
{
    if (!src || !dst) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null artifact copy argument");
    }
    cc_media_artifact_init(dst);
    dst->kind = src->kind;
    dst->id = cc_copy_string(src->id);
    dst->mime = cc_copy_string(src->mime);
    dst->path = cc_copy_string(src->path);
    dst->uri = cc_copy_string(src->uri);
    dst->data_base64 = cc_copy_string(src->data_base64);
    dst->bytes = src->bytes;
    dst->width = src->width;
    dst->height = src->height;
    dst->duration_ms = src->duration_ms;
    dst->created_at = cc_copy_string(src->created_at);
    dst->metadata = cc_copy_string(src->metadata);
    if ((src->id && !dst->id) ||
        (src->mime && !dst->mime) ||
        (src->path && !dst->path) ||
        (src->uri && !dst->uri) ||
        (src->data_base64 && !dst->data_base64) ||
        (src->created_at && !dst->created_at) ||
        (src->metadata && !dst->metadata)) {
        cc_media_artifact_cleanup(dst);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy artifact");
    }
    return cc_result_ok();
}

/*
 * 判断 MIME 是否在允许列表内。
 *
 * allowed_mime_prefixes 由调用方拥有；没有配置前缀时表示允许所有非空 MIME。
 * 这个策略适合 SDK 层做最小约束，具体业务可以传入更严格的 allowlist。
 */
static int mime_allowed(const char *mime, const cc_media_limits_t *limits)
{
    if (!mime || !*mime) return 0;
    if (!limits || limits->allowed_mime_prefix_count == 0) return 1;
    for (size_t i = 0; i < limits->allowed_mime_prefix_count; i++) {
        if (starts_with(mime, limits->allowed_mime_prefixes[i])) return 1;
    }
    return 0;
}

/*
 * 校验单个 artifact 的资源和安全边界。
 *
 * 这里不打开文件，也不访问网络，只检查元数据层面的 MIME、路径穿越和大小限制。
 * 真正的 workspace canonicalize 在 filesystem/path 工具层做；media 层先做低成本防线。
 */
cc_result_t cc_media_artifact_validate(
    const cc_media_artifact_t *artifact,
    const cc_media_limits_t *limits
)
{
    if (!artifact) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null artifact");
    }
    if (artifact->kind == CC_MEDIA_TEXT) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Text is not a media artifact");
    }
    if (!mime_allowed(artifact->mime, limits)) {
        return cc_result_error(CC_ERR_UNSUPPORTED, "Artifact MIME type is not allowed");
    }
    if (artifact->path && strstr(artifact->path, "..")) {
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "Artifact path must not contain '..'");
    }
    if (limits) {
        if (limits->max_artifact_bytes > 0 &&
            artifact->bytes > limits->max_artifact_bytes) {
            return cc_result_error(CC_ERR_UNSUPPORTED, "Artifact exceeds max_artifact_bytes");
        }
        if (artifact->data_base64 && artifact->data_base64[0]) {
            if (!limits->allow_inline_base64) {
                return cc_result_error(CC_ERR_UNSUPPORTED, "Inline base64 is disabled");
            }
            if (limits->max_base64_bytes > 0 &&
                string_len(artifact->data_base64) > limits->max_base64_bytes) {
                return cc_result_error(CC_ERR_UNSUPPORTED, "Artifact inline base64 exceeds limit");
            }
        }
    }
    return cc_result_ok();
}

/*
 * 初始化 artifact list。
 *
 * 动态数组从空状态开始，append 时按倍增扩容；这种设计比链表更适合嵌入式 cache
 * 局部性，也便于序列化为 JSON 数组。
 */
void cc_media_artifact_list_init(cc_media_artifact_list_t *list)
{
    if (!list) return;
    memset(list, 0, sizeof(*list));
}

/*
 * 清理 artifact list。
 *
 * 先逐个 cleanup 元素，再释放数组缓冲。最后清零可以让调用方在失败路径重复调用
 * cleanup，而不会二次释放。
 */
void cc_media_artifact_list_cleanup(cc_media_artifact_list_t *list)
{
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) {
        cc_media_artifact_cleanup(&list->items[i]);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

/*
 * 追加 artifact 到动态数组。
 *
 * 函数会深拷贝 artifact，因此调用方保留原对象所有权。扩容后把新增槽位清零，
 * 让后续 cleanup 在部分初始化状态下也安全。
 */
cc_result_t cc_media_artifact_list_append(
    cc_media_artifact_list_t *list,
    const cc_media_artifact_t *artifact
)
{
    if (!list || !artifact) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null artifact list append argument");
    }
    if (list->count == list->capacity) {
        size_t next_cap = list->capacity ? list->capacity * 2 : 4;
        cc_media_artifact_t *next =
            realloc(list->items, next_cap * sizeof(cc_media_artifact_t));
        if (!next) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow artifact list");
        }
        memset(next + list->capacity, 0,
            (next_cap - list->capacity) * sizeof(cc_media_artifact_t));
        list->items = next;
        list->capacity = next_cap;
    }
    cc_result_t rc = cc_media_artifact_copy(artifact, &list->items[list->count]);
    if (rc.code != CC_OK) return rc;
    list->count++;
    return cc_result_ok();
}

/*
 * 深拷贝 artifact list。
 *
 * 逐项复用 append 的深拷贝逻辑；任何一项失败都会清理 dst，保证调用方不需要
 * 根据 count 判断哪些元素已经成功。
 */
cc_result_t cc_media_artifact_list_copy(
    const cc_media_artifact_list_t *src,
    cc_media_artifact_list_t *dst
)
{
    if (!src || !dst) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null artifact list copy argument");
    }
    cc_media_artifact_list_init(dst);
    for (size_t i = 0; i < src->count; i++) {
        cc_result_t rc = cc_media_artifact_list_append(dst, &src->items[i]);
        if (rc.code != CC_OK) {
            cc_media_artifact_list_cleanup(dst);
            return rc;
        }
    }
    return cc_result_ok();
}

/*
 * 校验 artifact list 的整体限制。
 *
 * max_artifacts 是列表级预算，每个 artifact 的 MIME/大小/base64 再交给单项校验。
 * 这让 provider capability 和 runtime limits 可以组合使用。
 */
cc_result_t cc_media_artifact_list_validate(
    const cc_media_artifact_list_t *list,
    const cc_media_limits_t *limits
)
{
    if (!list) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null artifact list");
    }
    /* A zero artifact budget is the explicit text-only/fail-closed mode. */
    if (limits && list->count > limits->max_artifacts) {
        return cc_result_error(CC_ERR_UNSUPPORTED, "Artifact count exceeds max_artifacts");
    }
    for (size_t i = 0; i < list->count; i++) {
        cc_result_t rc = cc_media_artifact_validate(&list->items[i], limits);
        if (rc.code != CC_OK) return rc;
    }
    return cc_result_ok();
}

/*
 * 如果字符串字段存在则写入 JSON。
 *
 * JSON payload 尽量省略空字段，减少 provider 请求大小，也避免把“空字符串”和
 * “字段不存在”混成同一个语义。
 */
static void json_set_string_if_present(cc_json_value_t *obj, const char *key, const char *value)
{
    if (value && *value) {
        cc_json_object_set(obj, key, cc_json_create_string(value));
    }
}

/*
 * 如果数值为正则写入 JSON。
 *
 * 媒体尺寸和时长的 0 通常表示未知，省略字段比输出 0 更符合协议含义。
 */
static void json_set_number_if_positive(cc_json_value_t *obj, const char *key, double value)
{
    if (value > 0) {
        cc_json_object_set(obj, key, cc_json_create_number(value));
    }
}

/*
 * 将 artifact 转成 JSON object。
 *
 * 该 helper 只构建单个对象，不负责数组生命周期；调用方把返回对象交给 JSON AST
 * 后由 cc_json_destroy() 统一释放。
 */
static cc_json_value_t *artifact_to_json_object(const cc_media_artifact_t *artifact)
{
    cc_json_value_t *obj = cc_json_create_object();
    if (!obj) return NULL;
    cc_json_object_set(obj, "kind",
        cc_json_create_string(cc_media_kind_string(artifact->kind)));
    json_set_string_if_present(obj, "id", artifact->id);
    json_set_string_if_present(obj, "mime", artifact->mime);
    json_set_string_if_present(obj, "path", artifact->path);
    json_set_string_if_present(obj, "uri", artifact->uri);
    json_set_string_if_present(obj, "data_base64", artifact->data_base64);
    json_set_number_if_positive(obj, "bytes", (double)artifact->bytes);
    json_set_number_if_positive(obj, "width", (double)artifact->width);
    json_set_number_if_positive(obj, "height", (double)artifact->height);
    json_set_number_if_positive(obj, "duration_ms", (double)artifact->duration_ms);
    json_set_string_if_present(obj, "created_at", artifact->created_at);
    json_set_string_if_present(obj, "metadata", artifact->metadata);
    return obj;
}

/*
 * 从 JSON object 解析 artifact。
 *
 * 输出 artifact 先初始化，再逐字段深拷贝；如果某个存在的字符串字段复制失败，
 * 函数会清理 artifact 并返回 OOM，避免调用方处理半解析对象。
 */
static cc_result_t artifact_from_json_object(
    const cc_json_value_t *obj,
    cc_media_artifact_t *artifact
)
{
    if (!obj || !cc_json_is_object(obj) || !artifact) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid artifact JSON object");
    }
    cc_media_artifact_init(artifact);
    artifact->kind = cc_media_kind_from_string(
        cc_json_string_value(cc_json_object_get(obj, "kind")));
    artifact->id = cc_copy_string(cc_json_string_value(cc_json_object_get(obj, "id")));
    artifact->mime = cc_copy_string(cc_json_string_value(cc_json_object_get(obj, "mime")));
    artifact->path = cc_copy_string(cc_json_string_value(cc_json_object_get(obj, "path")));
    artifact->uri = cc_copy_string(cc_json_string_value(cc_json_object_get(obj, "uri")));
    artifact->data_base64 = cc_copy_string(cc_json_string_value(cc_json_object_get(obj, "data_base64")));
    artifact->bytes = (size_t)cc_json_number_value(cc_json_object_get(obj, "bytes"));
    artifact->width = cc_json_int_value(cc_json_object_get(obj, "width"));
    artifact->height = cc_json_int_value(cc_json_object_get(obj, "height"));
    artifact->duration_ms = cc_json_int_value(cc_json_object_get(obj, "duration_ms"));
    artifact->created_at = cc_copy_string(cc_json_string_value(cc_json_object_get(obj, "created_at")));
    artifact->metadata = cc_copy_string(cc_json_string_value(cc_json_object_get(obj, "metadata")));
    if ((cc_json_object_get(obj, "id") && !artifact->id) ||
        (cc_json_object_get(obj, "mime") && !artifact->mime) ||
        (cc_json_object_get(obj, "path") && !artifact->path) ||
        (cc_json_object_get(obj, "uri") && !artifact->uri) ||
        (cc_json_object_get(obj, "data_base64") && !artifact->data_base64) ||
        (cc_json_object_get(obj, "created_at") && !artifact->created_at) ||
        (cc_json_object_get(obj, "metadata") && !artifact->metadata)) {
        cc_media_artifact_cleanup(artifact);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to parse artifact");
    }
    return cc_result_ok();
}

/*
 * 序列化 artifact list。
 *
 * 这里使用 JSON AST 生成字符串，避免手工拼接时遗漏转义。返回的 out_json 由调用方
 * free()，函数失败时 out_json 保持 NULL。
 */
cc_result_t cc_media_artifact_list_to_json(
    const cc_media_artifact_list_t *list,
    char **out_json
)
{
    if (!list || !out_json) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null artifact JSON argument");
    }
    *out_json = NULL;
    cc_json_value_t *arr = cc_json_create_array();
    if (!arr) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate artifact JSON");
    for (size_t i = 0; i < list->count; i++) {
        cc_json_value_t *obj = artifact_to_json_object(&list->items[i]);
        if (!obj) {
            cc_json_destroy(arr);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to serialize artifact");
        }
        cc_json_array_append(arr, obj);
    }
    *out_json = cc_json_stringify_unformatted(arr);
    cc_json_destroy(arr);
    return *out_json ? cc_result_ok()
                     : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to stringify artifacts");
}

/*
 * 从 JSON 数组解析 artifact list。
 *
 * 空字符串按空列表处理，方便配置文件里省略多模态字段。解析每个临时 artifact 后
 * append 深拷贝到 list，再立即 cleanup 临时对象，所有权边界清晰。
 */
cc_result_t cc_media_artifact_list_from_json(
    const char *json,
    cc_media_artifact_list_t *out_list
)
{
    if (!out_list) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null artifact list output");
    }
    cc_media_artifact_list_init(out_list);
    if (!json || !*json) return cc_result_ok();
    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse(json, &root);
    if (rc.code != CC_OK) return rc;
    if (!cc_json_is_array(root)) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "Artifacts JSON must be an array");
    }
    int n = cc_json_array_size(root);
    for (int i = 0; i < n; i++) {
        cc_media_artifact_t artifact;
        rc = artifact_from_json_object(cc_json_array_get(root, i), &artifact);
        if (rc.code != CC_OK) {
            cc_json_destroy(root);
            cc_media_artifact_list_cleanup(out_list);
            return rc;
        }
        rc = cc_media_artifact_list_append(out_list, &artifact);
        cc_media_artifact_cleanup(&artifact);
        if (rc.code != CC_OK) {
            cc_json_destroy(root);
            cc_media_artifact_list_cleanup(out_list);
            return rc;
        }
    }
    cc_json_destroy(root);
    return cc_result_ok();
}

/*
 * 生成人类可读 artifact 摘要。
 *
 * 摘要用于日志、memory 和调试 UI，不是稳定机器协议。函数使用 string builder
 * 累积内容，任意 append 失败都会释放 builder，避免泄漏部分字符串。
 */
cc_result_t cc_media_artifact_list_summarize(
    const cc_media_artifact_list_t *list,
    char **out_summary
)
{
    if (!list || !out_summary) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null artifact summary argument");
    }
    *out_summary = NULL;
    cc_string_builder_t sb;
    cc_result_t rc = cc_string_builder_init(&sb);
    if (rc.code != CC_OK) return rc;
    if (list->count == 0) {
        rc = cc_string_builder_append(&sb, "");
    } else {
        rc = cc_string_builder_append(&sb, "Multimodal artifacts:\n");
    }
    for (size_t i = 0; i < list->count && rc.code == CC_OK; i++) {
        const cc_media_artifact_t *a = &list->items[i];
        rc = cc_string_builder_appendf(
            &sb,
            "- %s id=%s mime=%s",
            cc_media_kind_string(a->kind),
            a->id ? a->id : "",
            a->mime ? a->mime : "");
        if (rc.code == CC_OK && a->path) rc = cc_string_builder_appendf(&sb, " path=%s", a->path);
        if (rc.code == CC_OK && a->uri) rc = cc_string_builder_appendf(&sb, " uri=%s", a->uri);
        if (rc.code == CC_OK && a->bytes > 0) rc = cc_string_builder_appendf(&sb, " bytes=%zu", a->bytes);
        if (rc.code == CC_OK && a->width > 0 && a->height > 0) {
            rc = cc_string_builder_appendf(&sb, " size=%dx%d", a->width, a->height);
        }
        if (rc.code == CC_OK && a->duration_ms > 0) {
            rc = cc_string_builder_appendf(&sb, " duration_ms=%d", a->duration_ms);
        }
        if (rc.code == CC_OK) rc = cc_string_builder_append(&sb, "\n");
    }
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&sb);
        return rc;
    }
    *out_summary = cc_string_builder_take(&sb);
    return *out_summary ? cc_result_ok()
                        : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate artifact summary");
}

/*
 * 清理单个 content part。
 *
 * 不管当前 part 是文本还是 artifact，都释放两个可能持有资源的字段，最后清零。
 * 这种幂等风格让数组清理和错误回滚更简单。
 */
void cc_content_part_cleanup(cc_content_part_t *part)
{
    if (!part) return;
    free(part->text);
    cc_media_artifact_cleanup(&part->artifact);
    memset(part, 0, sizeof(*part));
}

/*
 * 深拷贝 content part。
 *
 * 文本 part 只复制 text；artifact part 还要复制完整 artifact。kind/direction 先复制，
 * 失败时 cleanup dst，保证 dst 不会留下已分配但未登记的字段。
 */
cc_result_t cc_content_part_copy(const cc_content_part_t *src, cc_content_part_t *dst)
{
    if (!src || !dst) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null content part copy argument");
    }
    memset(dst, 0, sizeof(*dst));
    dst->kind = src->kind;
    dst->direction = src->direction;
    dst->text = cc_copy_string(src->text);
    if (src->text && !dst->text) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy text part");
    }
    if (src->kind != CC_MEDIA_TEXT) {
        cc_result_t rc = cc_media_artifact_copy(&src->artifact, &dst->artifact);
        if (rc.code != CC_OK) {
            cc_content_part_cleanup(dst);
            return rc;
        }
    }
    return cc_result_ok();
}

/*
 * 初始化 content parts 动态数组。
 *
 * content parts 是 message 的核心容器，空状态表示没有任何文本或多模态片段。
 */
void cc_content_parts_init(cc_content_parts_t *parts)
{
    if (!parts) return;
    memset(parts, 0, sizeof(*parts));
}

/*
 * 清理 content parts 数组。
 *
 * 先清理每个 part 的内部资源，再释放数组缓冲；适合 message cleanup 和解析失败回滚。
 */
void cc_content_parts_cleanup(cc_content_parts_t *parts)
{
    if (!parts) return;
    for (size_t i = 0; i < parts->count; i++) {
        cc_content_part_cleanup(&parts->items[i]);
    }
    free(parts->items);
    memset(parts, 0, sizeof(*parts));
}

/*
 * 确保 content parts 至少有一个可写槽位。
 *
 * 使用倍增扩容降低 append 的均摊成本。扩容后清零新增槽位，是为了让后续错误路径
 * 能安全调用 cc_content_part_cleanup()。
 */
static cc_result_t content_parts_reserve(cc_content_parts_t *parts)
{
    if (parts->count < parts->capacity) return cc_result_ok();
    size_t next_cap = parts->capacity ? parts->capacity * 2 : 4;
    cc_content_part_t *next = realloc(parts->items, next_cap * sizeof(cc_content_part_t));
    if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow content parts");
    memset(next + parts->capacity, 0, (next_cap - parts->capacity) * sizeof(cc_content_part_t));
    parts->items = next;
    parts->capacity = next_cap;
    return cc_result_ok();
}

/*
 * 追加文本 part。
 *
 * NULL 文本被转换为空字符串，保持序列化时字段总是可用。append 成功后 count 才
 * 增加，因此复制失败时数组状态不变。
 */
cc_result_t cc_content_parts_append_text(
    cc_content_parts_t *parts,
    const char *text,
    cc_content_part_direction_t direction
)
{
    if (!parts) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null content parts");
    cc_result_t rc = content_parts_reserve(parts);
    if (rc.code != CC_OK) return rc;
    cc_content_part_t *part = &parts->items[parts->count];
    memset(part, 0, sizeof(*part));
    part->kind = CC_MEDIA_TEXT;
    part->direction = direction;
    part->text = cc_copy_string(text ? text : "");
    if (!part->text) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy text part");
    parts->count++;
    return cc_result_ok();
}


/*
 * 追加 artifact part。
 *
 * artifact 会被深拷贝到新槽位，调用方可以立即释放原 artifact。kind 来自 artifact
 * 本身，保证内容类型和 artifact 元数据一致。
 */
cc_result_t cc_content_parts_append_artifact(
    cc_content_parts_t *parts,
    const cc_media_artifact_t *artifact,
    cc_content_part_direction_t direction
)
{
    if (!parts || !artifact) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null content artifact part");
    }
    cc_result_t rc = content_parts_reserve(parts);
    if (rc.code != CC_OK) return rc;
    cc_content_part_t *part = &parts->items[parts->count];
    memset(part, 0, sizeof(*part));
    part->kind = artifact->kind;
    part->direction = direction;
    rc = cc_media_artifact_copy(artifact, &part->artifact);
    if (rc.code != CC_OK) return rc;
    parts->count++;
    return cc_result_ok();
}

/*
 * 深拷贝 content parts。
 *
 * 逐项复制可以复用单个 part 的所有权规则；任何失败都会清理 dst，避免调用方根据
 * dst->count 做复杂回滚。
 */
cc_result_t cc_content_parts_copy(const cc_content_parts_t *src, cc_content_parts_t *dst)
{
    if (!src || !dst) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null content parts copy argument");
    }
    cc_content_parts_init(dst);
    for (size_t i = 0; i < src->count; i++) {
        cc_result_t rc = content_parts_reserve(dst);
        if (rc.code != CC_OK) {
            cc_content_parts_cleanup(dst);
            return rc;
        }
        rc = cc_content_part_copy(&src->items[i], &dst->items[dst->count]);
        if (rc.code != CC_OK) {
            cc_content_parts_cleanup(dst);
            return rc;
        }
        dst->count++;
    }
    return cc_result_ok();
}

/*
 * 校验 content parts 的多模态限制。
 *
 * 文本片段不进入 artifact 限制；非文本 part 先收集到临时 artifact list，再复用
 * artifact list 校验逻辑，避免维护两套 max_artifacts/MIME 规则。
 */
cc_result_t cc_content_parts_validate(
    const cc_content_parts_t *parts,
    const cc_media_limits_t *limits
)
{
    if (!parts) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null content parts");
    cc_media_artifact_list_t artifacts;
    cc_media_artifact_list_init(&artifacts);
    for (size_t i = 0; i < parts->count; i++) {
        if (parts->items[i].kind == CC_MEDIA_TEXT) continue;
        cc_result_t rc = cc_media_artifact_list_append(&artifacts, &parts->items[i].artifact);
        if (rc.code != CC_OK) {
            cc_media_artifact_list_cleanup(&artifacts);
            return rc;
        }
    }
    cc_result_t rc = cc_media_artifact_list_validate(&artifacts, limits);
    cc_media_artifact_list_cleanup(&artifacts);
    return rc;
}

/*
 * 序列化 content parts。
 *
 * 每个 part 输出 type 和 direction；文本直接输出 text，非文本输出 artifact object。
 * 该格式是 SDK 内部稳定交换格式，provider adapter 可再转换成具体供应商协议。
 */
cc_result_t cc_content_parts_to_json(const cc_content_parts_t *parts, char **out_json)
{
    if (!parts || !out_json) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null content parts JSON argument");
    }
    *out_json = NULL;
    cc_json_value_t *arr = cc_json_create_array();
    if (!arr) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate parts JSON");
    for (size_t i = 0; i < parts->count; i++) {
        const cc_content_part_t *part = &parts->items[i];
        cc_json_value_t *obj = cc_json_create_object();
        if (!obj) {
            cc_json_destroy(arr);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate content part JSON");
        }
        cc_json_object_set(obj, "type", cc_json_create_string(cc_media_kind_string(part->kind)));
        cc_json_object_set(obj, "direction",
            cc_json_create_string(part->direction == CC_CONTENT_PART_OUTPUT ? "output" : "input"));
        if (part->kind == CC_MEDIA_TEXT) {
            cc_json_object_set(obj, "text", cc_json_create_string(part->text ? part->text : ""));
        } else {
            cc_json_value_t *artifact = artifact_to_json_object(&part->artifact);
            if (!artifact) {
                cc_json_destroy(obj);
                cc_json_destroy(arr);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to serialize part artifact");
            }
            cc_json_object_set(obj, "artifact", artifact);
        }
        cc_json_array_append(arr, obj);
    }
    *out_json = cc_json_stringify_unformatted(arr);
    cc_json_destroy(arr);
    return *out_json ? cc_result_ok()
                     : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to stringify content parts");
}

/*
 * 从 JSON 解析 content parts。
 *
 * 解析时允许旧格式把 artifact 字段直接放在 part object 上，以兼容早期测试数据。
 * 任一 part 解析失败都会清理 out_parts，保证调用方不需要处理部分列表。
 */
cc_result_t cc_content_parts_from_json(const char *json, cc_content_parts_t *out_parts)
{
    if (!out_parts) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null content parts output");
    }
    cc_content_parts_init(out_parts);
    if (!json || !*json) return cc_result_ok();
    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse(json, &root);
    if (rc.code != CC_OK) return rc;
    if (!cc_json_is_array(root)) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "Content parts JSON must be an array");
    }
    int n = cc_json_array_size(root);
    for (int i = 0; i < n; i++) {
        cc_json_value_t *obj = cc_json_array_get(root, i);
        if (!obj || !cc_json_is_object(obj)) continue;
        cc_media_kind_t kind = cc_media_kind_from_string(
            cc_json_string_value(cc_json_object_get(obj, "type")));
        const char *dir_s = cc_json_string_value(cc_json_object_get(obj, "direction"));
        cc_content_part_direction_t dir =
            (dir_s && strcmp(dir_s, "output") == 0) ? CC_CONTENT_PART_OUTPUT : CC_CONTENT_PART_INPUT;
        if (kind == CC_MEDIA_TEXT) {
            rc = cc_content_parts_append_text(
                out_parts,
                cc_json_string_value(cc_json_object_get(obj, "text")),
                dir);
        } else {
            cc_media_artifact_t artifact;
            cc_json_value_t *artifact_obj = cc_json_object_get(obj, "artifact");
            if (!artifact_obj) artifact_obj = obj;
            rc = artifact_from_json_object(artifact_obj, &artifact);
            if (rc.code == CC_OK) {
                artifact.kind = kind;
                rc = cc_content_parts_append_artifact(out_parts, &artifact, dir);
                cc_media_artifact_cleanup(&artifact);
            }
        }
        if (rc.code != CC_OK) {
            cc_json_destroy(root);
            cc_content_parts_cleanup(out_parts);
            return rc;
        }
    }
    cc_json_destroy(root);
    return cc_result_ok();
}

/*
 * 生成 content parts 的文本摘要。
 *
 * 文本 part 保留原文，artifact part 生成带 kind/id/mime 的占位符。这个函数常用于
 * memory 检索摘要或不支持多模态 provider 的降级输入。
 */
cc_result_t cc_content_parts_text_summary(
    const cc_content_parts_t *parts,
    char **out_summary
)
{
    if (!parts || !out_summary) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null content summary argument");
    }
    *out_summary = NULL;
    cc_string_builder_t sb;
    cc_result_t rc = cc_string_builder_init(&sb);
    if (rc.code != CC_OK) return rc;
    for (size_t i = 0; i < parts->count && rc.code == CC_OK; i++) {
        const cc_content_part_t *part = &parts->items[i];
        if (part->kind == CC_MEDIA_TEXT) {
            rc = cc_string_builder_append(&sb, part->text ? part->text : "");
        } else {
            rc = cc_string_builder_appendf(
                &sb,
                "[%s artifact id=%s mime=%s]",
                cc_media_kind_string(part->kind),
                part->artifact.id ? part->artifact.id : "",
                part->artifact.mime ? part->artifact.mime : "");
        }
        if (rc.code == CC_OK && i + 1 < parts->count) {
            rc = cc_string_builder_append(&sb, "\n");
        }
    }
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&sb);
        return rc;
    }
    *out_summary = cc_string_builder_take(&sb);
    return *out_summary ? cc_result_ok()
                        : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate content summary");
}
