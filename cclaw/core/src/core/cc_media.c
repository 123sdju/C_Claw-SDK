#include "cc/core/cc_media.h"

#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/internal/cc_json_syntax_check.h"
#include "cc/internal/cc_json_fields.h"
#include "cc/util/cc_string_builder.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>




/**
 * @brief 获取可选媒体字符串的字节长度。
 * @param[in] s 稳定 NUL 文本，可为 NULL。
 * @return strlen 字节数，NULL 为零。
 */
static size_t string_len(const char *s)
{
    return s ? strlen(s) : 0;
}

/**
 * @brief 对 MIME 策略执行区分大小写的前缀比较。
 * @param[in] s 借用的完整字符串，可为 NULL。
 * @param[in] prefix 借用前缀，可为 NULL，空串可匹配任意非空指针输入。
 * @return 两参数均非 NULL 且前缀相同时为 1，否则为 0。
 */
static int starts_with(const char *s, const char *prefix)
{
    size_t n = string_len(prefix);
    return s && prefix && strncmp(s, prefix, n) == 0;
}

/**
 * @brief 为类型标记选择固定协议名称。
 * @param[in] kind 媒体或文本标记，未知值沿 FILE 映射。
 * @return 静态只读字符串，无分配，调用者不得释放。
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

/**
 * @brief 按准确小写名称解析类型，保留既有缺省映射。
 * @param[in] kind 稳定借用文本，可为 NULL。
 * @return NULL 或 text 为 TEXT，已知名对应原类型，未知名和空串为 FILE。
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

/**
 * @brief 将媒体策略清零，设置为禁止任何媒体项的文本模式。
 * @param[out] limits 输出结构，可为 NULL。
 * @post 非空策略清零，不释放旧策略借用的数组或字符串。
 */
void cc_media_limits_text_only(cc_media_limits_t *limits)
{
    if (!limits) return;
    memset(limits, 0, sizeof(*limits));
    limits->max_artifacts = 0;
    limits->allow_inline_base64 = 0;
}

/**
 * @brief 填写八项、单项十 MiB、编码二 MiB 的默认媒体策略。
 * @param[out] limits 输出结构，可为 NULL。
 * @post 清零后允许内联编码，MIME 前缀数为零，旧借用不被释放。
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

/**
 * @brief 将新或已清理媒体对象初始化为 FILE。
 * @param[out] artifact 新/已清理对象，可为 NULL。
 * @post 非空结构清零并设默认类型，不释放此前字段。
 */
void cc_media_artifact_init(cc_media_artifact_t *artifact)
{
    if (!artifact) return;
    memset(artifact, 0, sizeof(*artifact));
    artifact->kind = CC_MEDIA_FILE;
}

/**
 * @brief 消费媒体对象的七个独立拥有字符串。
 * @param[in,out] artifact 可为 NULL，非空字段须是可独立 free 的唯一分配。
 * @post 非空结构全部清零，结构本身继续由调用者保管。
 * @pre owner 已结束全部字段借用，字段不得互相共享同一拥有指针。
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

/**
 * @brief 为不同目标复制全部媒体标量与字符串，自身复制直接成功。
 * @param[in] src 非 NULL 稳定输入，可持有只读借用字符串。
 * @param[out] dst 非 NULL 新/已清理对象，允许与 src 相同。
 * @return 成功为 CC_OK，空参数为 INVALID_ARGUMENT，字符串 OOM 时清理不同目标并返回 OUT_OF_MEMORY。
 * @note 读取与写入由外层串行化，不同目标不能覆盖源结构或源字段存储。
 */
cc_result_t cc_media_artifact_copy(const cc_media_artifact_t *src, cc_media_artifact_t *dst)
{
    if (!src || !dst) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null artifact copy argument");
    }
    if (src == dst) return cc_result_ok();
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

/**
 * @brief 对非空 MIME 应用可选前缀数组。
 * @param[in] mime 稳定字符串，可为 NULL。
 * @param[in] limits 可空稳定策略，非零前缀数对应数组已由外层验证。
 * @return MIME 非空且至少一个前缀匹配为 1，无前缀策略也为 1，其余为 0。
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

/**
 * @brief 对单项声明执行类型、MIME、路径子串和资源预算检查。
 * @param[in] artifact 非 NULL 稳定元数据，TEXT 不是媒体项。
 * @param[in] limits 可空稳定策略，正前缀数量必须有数组。
 * @return 成功为 CC_OK，参数错误为 INVALID_ARGUMENT，策略拒绝为 UNSUPPORTED，path 含 '..' 为 PERMISSION_DENIED。
 * @note 不检查定位字段必填、真实字节数、尺寸、URI 或 Base64 语法，不访问文件和网络。
 */
cc_result_t cc_media_artifact_validate(
    const cc_media_artifact_t *artifact,
    const cc_media_limits_t *limits
)
{
    if (!artifact) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null artifact");
    }
    if (limits && (limits->allowed_mime_prefix_count > SIZE_MAX / sizeof(*limits->allowed_mime_prefixes) ||
        (limits->allowed_mime_prefix_count > 0 && !limits->allowed_mime_prefixes))) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Missing MIME prefix array");
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

/**
 * @brief 清零新媒体列表包装。
 * @param[out] list 新或已清理对象，可为 NULL。
 * @post 非空结构清零，不消费旧数组与项。
 */
void cc_media_artifact_list_init(cc_media_artifact_list_t *list)
{
    if (!list) return;
    memset(list, 0, sizeof(*list));
}

/**
 * @brief 消费媒体数组的有效项与数组分配。
 * @param[in,out] list 独占拥有包装，可为 NULL，count 为真实有效项数。
 * @post 包装清零，items 为 NULL 时跳过项访问，结构存储保留。
 * @pre 结束数组与字段借用，非空数组可被 free 且覆盖 count 项。
 */
void cc_media_artifact_list_cleanup(cc_media_artifact_list_t *list)
{
    if (!list) return;
    for (size_t i = 0; list->items && i < list->count; i++) {
        cc_media_artifact_cleanup(&list->items[i]);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

/**
 * @brief 先取得独立媒体副本，再有界扩容并转移到新槽。
 * @param[in,out] list 已初始化的拥有列表，独占修改，count/capacity 匹配真实分配。
 * @param[in] artifact 非 NULL 稳定输入，可借自当前列表元素。
 * @return 成功为 CC_OK，形状错误为 INVALID_ARGUMENT，容量耗尽为 LIMIT_EXCEEDED，复制/扩容失败为 OUT_OF_MEMORY。
 * @note 所有失败清理临时副本并保留旧项/count，成功扩容使旧 items 地址失效。
 */
cc_result_t cc_media_artifact_list_append(
    cc_media_artifact_list_t *list,
    const cc_media_artifact_t *artifact
)
{
    if (!list || !artifact) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null artifact list append argument");
    }
    if (list->count > list->capacity || (list->capacity && !list->items) ||
        list->capacity > SIZE_MAX / sizeof(*list->items)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid mutable artifact list");
    }
    cc_media_artifact_t copy;
    cc_result_t rc = cc_media_artifact_copy(artifact, &copy);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    if (list->count == list->capacity) {
        size_t maximum = SIZE_MAX / sizeof(*list->items);
        if (list->capacity == maximum) {
            cc_media_artifact_cleanup(&copy);
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Artifact list capacity exhausted");
        }
        size_t next_cap = list->capacity == 0 ? 4 :
            list->capacity > maximum / 2 ? maximum : list->capacity * 2;
        if (next_cap > maximum) next_cap = maximum;
        cc_media_artifact_t *next =
            realloc(list->items, next_cap * sizeof(cc_media_artifact_t));
        if (!next) {
            cc_media_artifact_cleanup(&copy);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow artifact list");
        }
        memset(next + list->capacity, 0,
            (next_cap - list->capacity) * sizeof(cc_media_artifact_t));
        list->items = next;
        list->capacity = next_cap;
    }
    list->items[list->count] = copy;
    list->count++;
    return cc_result_ok();
}

/**
 * @brief 向不同的空目标逐项深拷贝媒体列表，自身复制保持原对象。
 * @param[in] src 非 NULL 稳定读取视图，items 覆盖 count，不依赖 capacity。
 * @param[out] dst 非 NULL 新/已清理包装，允许等于 src。
 * @return 成功为 CC_OK，参数/源形状错误为 INVALID_ARGUMENT，项复制或增长失败清空不同目标并交付原错误。
 */
cc_result_t cc_media_artifact_list_copy(
    const cc_media_artifact_list_t *src,
    cc_media_artifact_list_t *dst
)
{
    if (!src || !dst) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null artifact list copy argument");
    }
    if (src->count > SIZE_MAX / sizeof(*src->items) || (src->count && !src->items))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Missing artifact list items");
    if (src == dst) return cc_result_ok();
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

/**
 * @brief 核对列表计数与各媒体项的策略。
 * @param[in] list 非 NULL 稳定只读视图，items 覆盖 count，capacity 不参与读取。
 * @param[in] limits 可空稳定策略，数量上限零表示不允许媒体项。
 * @return 成功为 CC_OK，形状/策略数组错误为 INVALID_ARGUMENT，其余首个策略错误原样返回。
 */
cc_result_t cc_media_artifact_list_validate(
    const cc_media_artifact_list_t *list,
    const cc_media_limits_t *limits
)
{
    if (!list || list->count > SIZE_MAX / sizeof(*list->items) || (list->count && !list->items)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null artifact list");
    }
    if (limits && (limits->allowed_mime_prefix_count > SIZE_MAX / sizeof(*limits->allowed_mime_prefixes) ||
        (limits->allowed_mime_prefix_count > 0 && !limits->allowed_mime_prefixes)))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Missing MIME prefix array");
    /* max_artifacts 为零是显式禁止媒体项的文本模式。 */
    if (limits && list->count > limits->max_artifacts) {
        return cc_result_error(CC_ERR_UNSUPPORTED, "Artifact count exceeds max_artifacts");
    }
    for (size_t i = 0; i < list->count; i++) {
        cc_result_t rc = cc_media_artifact_validate(&list->items[i], limits);
        if (rc.code != CC_OK) return rc;
    }
    return cc_result_ok();
}

/**
 * @brief 仅在非空字符串存在时写入字段，并报告创建和挂接结果。
 * @param[in,out] obj 调用期间借用的有效 JSON 对象，调用者串行构造。
 * @param[in] key 借用字段名，底层复制名称。
 * @param[in] value 借用正文，NULL 或空字符串表示省略。
 * @return 字段省略或完整挂接成功为一，内存分配或挂接失败为零。
 */
static int json_set_string_if_present(cc_json_value_t *obj, const char *key, const char *value)
{
    return !value || !*value || cc_json_object_set_owned(obj, key, cc_json_create_string(value));
}

/**
 * @brief 仅在数值为正时写入媒体尺寸或时长字段。
 * @param[in,out] obj 调用期间借用的有效 JSON 对象，调用者串行构造。
 * @param[in] key 借用字段名，底层复制名称。
 * @param[in] value 要写入的数值，非正值沿用未知值省略规则。
 * @return 字段省略或完整挂接成功为一，创建或挂接失败为零。
 */
static int json_set_number_if_positive(cc_json_value_t *obj, const char *key, double value)
{
    return !(value > 0) || cc_json_object_set_owned(obj, key, cc_json_create_number(value));
}

/**
 * @brief 构造完整媒体对象，任一应有字段写入失败时销毁临时树。
 * @param[in] artifact 调用期间借用的有效稳定媒体记录。
 * @param[out] out_object 非 NULL 独立输出槽，先置 NULL，成功交接未挂接的拥有对象。
 * @return 成功为 CC_OK，bytes 的 double 表示越出 size_t 范围为 LIMIT_EXCEEDED，其余节点失败为 OUT_OF_MEMORY。
 * @note 读取期间源记录须保持稳定，省略规则沿用可选字符串及正数元数据约定。
 */
static cc_result_t artifact_to_json_object(
    const cc_media_artifact_t *artifact, cc_json_value_t **out_object)
{
    *out_object = NULL;
    double bytes = (double)artifact->bytes;
    double upper_bound = (double)(SIZE_MAX / 2U + 1U) * 2.0;
    if (bytes >= upper_bound)
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Artifact field 'bytes' exceeds JSON numeric representation");
    cc_json_value_t *obj = cc_json_create_object();
    if (!obj) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate artifact JSON");
    if (!cc_json_object_set_owned(obj, "kind", cc_json_create_string(cc_media_kind_string(artifact->kind))) ||
        !json_set_string_if_present(obj, "id", artifact->id) ||
        !json_set_string_if_present(obj, "mime", artifact->mime) ||
        !json_set_string_if_present(obj, "path", artifact->path) ||
        !json_set_string_if_present(obj, "uri", artifact->uri) ||
        !json_set_string_if_present(obj, "data_base64", artifact->data_base64) ||
        !json_set_number_if_positive(obj, "bytes", bytes) ||
        !json_set_number_if_positive(obj, "width", (double)artifact->width) ||
        !json_set_number_if_positive(obj, "height", (double)artifact->height) ||
        !json_set_number_if_positive(obj, "duration_ms", (double)artifact->duration_ms) ||
        !json_set_string_if_present(obj, "created_at", artifact->created_at) ||
        !json_set_string_if_present(obj, "metadata", artifact->metadata)) {
        cc_json_destroy(obj);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to serialize artifact fields");
    }
    *out_object = obj;
    return cc_result_ok();
}

/**
 * @brief 对已知字段查重和类型检查后，构造完整拥有媒体记录。
 * @param[in] obj 稳定 JSON 节点，须为对象，已由外层拒绝原文解码 NUL。
 * @param[out] artifact 非 NULL 新/已清理输出，先初始化，字段失败时清理。
 * @return 成功为 CC_OK，空输出为 INVALID_ARGUMENT，schema/bytes 范围错误为 JSON，字段分配失败为 OUT_OF_MEMORY。
 * @note 缺 kind 沿旧 TEXT 默认，未知字符串为 FILE；正 bytes 小数截断，有限 int 字段沿 cJSON 饱和视图。
 */
static cc_result_t artifact_from_json_object(
    const cc_json_value_t *obj,
    cc_media_artifact_t *artifact
)
{
    if (!artifact) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid artifact JSON object");
    }
    cc_media_artifact_init(artifact);
    if (!obj || !cc_json_is_object(obj))
        return cc_result_error(CC_ERR_JSON, "Artifact must be an object");
    static const char *const fields[] = {
        "kind", "id", "mime", "path", "uri", "data_base64", "bytes", "width",
        "height", "duration_ms", "created_at", "metadata"
    };
    const char *duplicate = cc_json_duplicate_known_field(obj, fields, sizeof(fields) / sizeof(fields[0]));
    if (duplicate)
        return cc_result_errf(CC_ERR_JSON, "Duplicate artifact field '%s'", duplicate);
    static const char *const string_fields[] = {
        "kind", "id", "mime", "path", "uri", "data_base64", "created_at", "metadata"
    };
    char **outputs[] = {
        NULL, &artifact->id, &artifact->mime, &artifact->path, &artifact->uri,
        &artifact->data_base64, &artifact->created_at, &artifact->metadata
    };
    cc_result_t rc = cc_result_ok();
    artifact->kind = CC_MEDIA_TEXT;
    for (size_t i = 0; i < sizeof(string_fields) / sizeof(string_fields[0]); i++) {
        cc_json_value_t *value = cc_json_object_get(obj, string_fields[i]);
        if (!value) continue;
        if (!cc_json_is_string(value)) {
            rc = cc_result_errf(CC_ERR_JSON, "Artifact field '%s' must be a string", string_fields[i]);
            goto fail;
        }
        if (i == 0) {
            artifact->kind = cc_media_kind_from_string(cc_json_string_value(value));
            continue;
        }
        *outputs[i] = cc_copy_string(cc_json_string_value(value));
        if (!*outputs[i]) {
            rc = cc_result_errf(CC_ERR_OUT_OF_MEMORY, "Failed to copy artifact field '%s'", string_fields[i]);
            goto fail;
        }
    }
    cc_json_value_t *bytes = cc_json_object_get(obj, "bytes");
    if (bytes) {
        double value = cc_json_number_value(bytes);
        double upper_bound = (double)(SIZE_MAX / 2U + 1U) * 2.0;
        if (!cc_json_is_number(bytes) || !isfinite(value) || value < 0 || value >= upper_bound) {
            rc = cc_result_error(CC_ERR_JSON, "Artifact field 'bytes' is outside the size_t range");
            goto fail;
        }
        artifact->bytes = (size_t)value;
    }
    static const char *const number_fields[] = {"width", "height", "duration_ms"};
    int *numbers[] = {&artifact->width, &artifact->height, &artifact->duration_ms};
    for (size_t i = 0; i < sizeof(number_fields) / sizeof(number_fields[0]); i++) {
        cc_json_value_t *value = cc_json_object_get(obj, number_fields[i]);
        if (!value) continue;
        if (!cc_json_is_number(value) || !isfinite(cc_json_number_value(value))) {
            rc = cc_result_errf(CC_ERR_JSON, "Artifact field '%s' must be a finite number", number_fields[i]);
            goto fail;
        }
        *numbers[i] = cc_json_int_value(value);
    }
    return cc_result_ok();
fail:
    cc_media_artifact_cleanup(artifact);
    return rc;
}

/**
 * @brief 完整序列化媒体记录数组，任何字段或节点失败时撤销输出。
 * @param[in] list 调用期间借用的稳定列表，非空列表须具有有效 items。
 * @param[out] out_json 输出独占字符串，调用者 free，失败为 NULL。
 * @return 成功为 CC_OK，参数、数值表示上限或内存错误的拥有结果由调用者释放。
 * @note 无内部共享写入，同一源列表的并发修改须由调用者排除。
 */
cc_result_t cc_media_artifact_list_to_json(
    const cc_media_artifact_list_t *list,
    char **out_json
)
{
    if (!out_json) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null artifact JSON argument");
    }
    *out_json = NULL;
    if (!list || list->count > SIZE_MAX / sizeof(*list->items) || (list->count && !list->items))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid artifact list");
    cc_json_value_t *arr = cc_json_create_array();
    if (!arr) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate artifact JSON");
    for (size_t i = 0; i < list->count; i++) {
        cc_json_value_t *obj = NULL;
        cc_result_t rc = artifact_to_json_object(&list->items[i], &obj);
        if (rc.code != CC_OK) {
            cc_result_t error = cc_result_errf(rc.code, "Artifact[%zu]: %s", i,
                rc.message ? rc.message : "serialization failed");
            cc_result_free(&rc);
            cc_json_destroy(arr);
            return error;
        }
        cc_result_free(&rc);
        if (!cc_json_array_append_owned(arr, obj)) {
            cc_json_destroy(arr);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to append artifact JSON");
        }
    }
    *out_json = cc_json_stringify_unformatted(arr);
    cc_json_destroy(arr);
    return *out_json ? cc_result_ok()
                     : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to stringify artifacts");
}

/**
 * @brief 完整解析媒体数组，逐项深拷贝，任一坏项撤销全部输出。
 * @param[in] json 稳定 NUL 文本，NULL/空串表示空列表，其他文本要求完整尾部且无解码 NUL。
 * @param[out] out_list 非 NULL 新/已清理包装，先初始化，所有失败后保持空列表。
 * @return 成功为 CC_OK，空输出为 INVALID_ARGUMENT，损坏/schema/通用解析失败为 JSON，字段复制或列表分配失败为 OUT_OF_MEMORY。
 * @note 失败诊断保留项索引和字段，原始文本未修改；源 JSON 借用不得被输出初始化覆盖。
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
    if (cc_storage_json_has_nul_escape(json))
        return cc_result_error(CC_ERR_JSON, "Artifacts JSON contains a decoded NUL");
    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse_strict(json, &root);
    if (rc.code != CC_OK) return rc;
    if (!cc_json_is_array(root)) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "Artifacts JSON must be an array");
    }
    cc_result_free(&rc);
    int n = cc_json_array_size(root);
    for (int i = 0; i < n; i++) {
        cc_media_artifact_t artifact;
        rc = artifact_from_json_object(cc_json_array_get(root, i), &artifact);
        if (rc.code != CC_OK) {
            cc_result_t error = cc_result_errf(rc.code, "Artifact[%d]: %s", i,
                rc.message ? rc.message : "invalid artifact");
            cc_result_free(&rc);
            cc_json_destroy(root);
            cc_media_artifact_list_cleanup(out_list);
            return error;
        }
        cc_result_free(&rc);
        rc = cc_media_artifact_list_append(out_list, &artifact);
        cc_media_artifact_cleanup(&artifact);
        if (rc.code != CC_OK) {
            cc_json_destroy(root);
            cc_media_artifact_list_cleanup(out_list);
            return rc;
        }
        cc_result_free(&rc);
    }
    cc_json_destroy(root);
    return cc_result_ok();
}

/**
 * @brief 拼接媒体类型、身份、位置和正尺寸等声明字段的可读摘要。
 * @param[in] list 非 NULL 稳定只读列表，items 覆盖 count，capacity 不参与读取。
 * @param[out] out_summary 非 NULL 独立输出槽，先置 NULL，成功接管 free 可释放缓冲。
 * @return 成功为 CC_OK，参数/形状错误为 INVALID_ARGUMENT，builder 首次失败结果原样交付。
 * @note 失败释放构造缓冲，输入和字段在调用期间稳定，输出槽不得覆盖源字段。
 */
cc_result_t cc_media_artifact_list_summarize(
    const cc_media_artifact_list_t *list,
    char **out_summary
)
{
    if (!out_summary) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null artifact summary argument");
    }
    *out_summary = NULL;
    if (!list || list->count > SIZE_MAX / sizeof(*list->items) || (list->count && !list->items))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid artifact summary list");
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

/**
 * @brief 消费内容项的文本与媒体拥有字段。
 * @param[in,out] part 独占拥有对象，可为 NULL，各非空字段须为独立分配。
 * @post 非空结构清零，结构存储保留，清理前结束全部借用。
 */
void cc_content_part_cleanup(cc_content_part_t *part)
{
    if (!part) return;
    free(part->text);
    cc_media_artifact_cleanup(&part->artifact);
    memset(part, 0, sizeof(*part));
}

/**
 * @brief 复制标记、可选文本及非 TEXT 媒体，完全自身复制为空操作。
 * @param[in] src 非 NULL 稳定输入。
 * @param[out] dst 非 NULL 新/已清理输出，允许与 src 相同。
 * @return 成功为 CC_OK，空参数为 INVALID_ARGUMENT，复制 OOM 时不同目标无半成品。
 * @note TEXT 项中未使用的媒体字段不复制，不同目标不得覆盖源字段存储。
 */
cc_result_t cc_content_part_copy(const cc_content_part_t *src, cc_content_part_t *dst)
{
    if (!src || !dst) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null content part copy argument");
    }
    if (src == dst) return cc_result_ok();
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

/**
 * @brief 清零新内容列表包装。
 * @param[out] parts 新或已清理结构，可为 NULL。
 * @post 非空结构清零，不释放旧数组或项。
 */
void cc_content_parts_init(cc_content_parts_t *parts)
{
    if (!parts) return;
    memset(parts, 0, sizeof(*parts));
}

/**
 * @brief 消费内容数组每个有效项及数组分配。
 * @param[in,out] parts 独占拥有列表，可为 NULL，count 位于真实数组边界内。
 * @post 包装清零，items 为 NULL 时跳过项，结构存储保留。
 * @pre owner 已排空数组和字段借用，非空数组可由 free 释放。
 */
void cc_content_parts_cleanup(cc_content_parts_t *parts)
{
    if (!parts) return;
    for (size_t i = 0; parts->items && i < parts->count; i++) {
        cc_content_part_cleanup(&parts->items[i]);
    }
    free(parts->items);
    memset(parts, 0, sizeof(*parts));
}

/**
 * @brief 在检查可写状态与容量算术后，为内容列表保留一个新槽。
 * @param[in,out] parts 非 NULL 已初始化拥有列表，调用者独占修改。
 * @return 成功为 CC_OK，非法 count/capacity/数组为 INVALID_ARGUMENT，容量耗尽为 LIMIT_EXCEEDED，realloc 失败为 OUT_OF_MEMORY。
 * @note 新增容量清零，失败保留旧数组和容量，成功可能使所有旧元素指针失效。
 */
static cc_result_t content_parts_reserve(cc_content_parts_t *parts)
{
    if (parts->count > parts->capacity || (parts->capacity && !parts->items) ||
        parts->capacity > SIZE_MAX / sizeof(*parts->items))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid mutable content parts");
    if (parts->count < parts->capacity) return cc_result_ok();
    size_t maximum = SIZE_MAX / sizeof(*parts->items);
    if (parts->capacity == maximum)
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Content parts capacity exhausted");
    size_t next_cap = parts->capacity == 0 ? 4 :
        parts->capacity > maximum / 2 ? maximum : parts->capacity * 2;
    if (next_cap > maximum) next_cap = maximum;
    cc_content_part_t *next = realloc(parts->items, next_cap * sizeof(cc_content_part_t));
    if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow content parts");
    memset(next + parts->capacity, 0, (next_cap - parts->capacity) * sizeof(cc_content_part_t));
    parts->items = next;
    parts->capacity = next_cap;
    return cc_result_ok();
}

/**
 * @brief 在任何数组搬迁前复制文本，再转移到新 TEXT 槽。
 * @param[in,out] parts 非 NULL 初始化拥有列表，独占修改。
 * @param[in] text 调用期间借用文本，可为 NULL，按空串复制，可借自当前数组存储。
 * @param[in] direction 原样保存的方向标记。
 * @return 成功为 CC_OK，形状/容量/分配失败返回对应结果，失败不改变旧项和 count。
 */
cc_result_t cc_content_parts_append_text(
    cc_content_parts_t *parts,
    const char *text,
    cc_content_part_direction_t direction
)
{
    if (!parts) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null content parts");
    if (parts->count > parts->capacity || (parts->capacity && !parts->items) ||
        parts->capacity > SIZE_MAX / sizeof(*parts->items))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid mutable content parts");
    char *copy = cc_copy_string(text ? text : "");
    if (!copy) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy text part");
    cc_result_t rc = content_parts_reserve(parts);
    if (rc.code != CC_OK) {
        free(copy);
        return rc;
    }
    cc_result_free(&rc);
    cc_content_part_t *part = &parts->items[parts->count];
    memset(part, 0, sizeof(*part));
    part->kind = CC_MEDIA_TEXT;
    part->direction = direction;
    part->text = copy;
    parts->count++;
    return cc_result_ok();
}


/**
 * @brief 先复制媒体，再扩容并转移副本到内容槽。
 * @param[in,out] parts 非 NULL 已初始化拥有列表，独占修改。
 * @param[in] artifact 非 NULL 稳定输入，可借自本列表内嵌媒体，原 owner 保留原字段。
 * @param[in] direction 原样保存的方向标记，kind 从媒体副本取得。
 * @return 成功为 CC_OK，坏参数/形状、容量耗尽或 OOM 返回对应错误，失败清理副本并保留旧项/count。
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
    if (parts->count > parts->capacity || (parts->capacity && !parts->items) ||
        parts->capacity > SIZE_MAX / sizeof(*parts->items))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid mutable content parts");
    cc_media_artifact_t copy;
    cc_result_t rc = cc_media_artifact_copy(artifact, &copy);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    rc = content_parts_reserve(parts);
    if (rc.code != CC_OK) {
        cc_media_artifact_cleanup(&copy);
        return rc;
    }
    cc_result_free(&rc);
    cc_content_part_t *part = &parts->items[parts->count];
    memset(part, 0, sizeof(*part));
    part->kind = copy.kind;
    part->direction = direction;
    part->artifact = copy;
    parts->count++;
    return cc_result_ok();
}

/**
 * @brief 为不同空目标逐项复制内容列表，自身复制保持原状态。
 * @param[in] src 非 NULL 稳定只读列表，items 覆盖 count，允许视图 capacity<count。
 * @param[out] dst 非 NULL 新/已清理拥有包装，允许与 src 相同。
 * @return 成功为 CC_OK，源形状/参数错误为 INVALID_ARGUMENT，复制失败清空不同目标并返回原错误。
 */
cc_result_t cc_content_parts_copy(const cc_content_parts_t *src, cc_content_parts_t *dst)
{
    if (!src || !dst) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null content parts copy argument");
    }
    if (src->count > SIZE_MAX / sizeof(*src->items) || (src->count && !src->items))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Missing content parts items");
    if (src == dst) return cc_result_ok();
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

/**
 * @brief 将非 TEXT 媒体深拷贝到临时列表并复用媒体策略校验。
 * @param[in] parts 非 NULL 稳定只读列表，items 覆盖 count，不使用 capacity 读取边界。
 * @param[in] limits 可空稳定策略。
 * @return 成功为 CC_OK，坏形状、临时副本 OOM、数量或单项策略错误交付对应结果。
 * @note 所有临时拥有字段在返回前清理，文本不计入媒体预算，无内部锁。
 */
cc_result_t cc_content_parts_validate(
    const cc_content_parts_t *parts,
    const cc_media_limits_t *limits
)
{
    if (!parts || parts->count > SIZE_MAX / sizeof(*parts->items) || (parts->count && !parts->items))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid content parts");
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

/**
 * @brief 完整序列化文本及媒体内容数组，任一节点构造失败时撤销输出。
 * @param[in] parts 调用期间借用的稳定内容列表，非空列表须具有有效 items。
 * @param[out] out_json 输出独占字符串，调用者 free，失败为 NULL。
 * @return 成功为 CC_OK，参数、媒体数值表示上限或内存错误的拥有结果由调用者释放。
 * @note 每项保留 type、direction 及 text 或 artifact，同一源列表的写入须外层同步。
 */
cc_result_t cc_content_parts_to_json(const cc_content_parts_t *parts, char **out_json)
{
    if (!out_json) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null content parts JSON argument");
    }
    *out_json = NULL;
    if (!parts || parts->count > SIZE_MAX / sizeof(*parts->items) || (parts->count && !parts->items))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid content parts");
    cc_json_value_t *arr = cc_json_create_array();
    if (!arr) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate parts JSON");
    for (size_t i = 0; i < parts->count; i++) {
        const cc_content_part_t *part = &parts->items[i];
        cc_json_value_t *obj = cc_json_create_object();
        if (!obj) {
            cc_json_destroy(arr);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate content part JSON");
        }
        if (!cc_json_object_set_owned(obj, "type", cc_json_create_string(cc_media_kind_string(part->kind))) ||
            !cc_json_object_set_owned(obj, "direction",
                cc_json_create_string(part->direction == CC_CONTENT_PART_OUTPUT ? "output" : "input"))) {
            cc_json_destroy(obj);
            cc_json_destroy(arr);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to set content part fields");
        }
        if (part->kind == CC_MEDIA_TEXT) {
            if (!cc_json_object_set_owned(obj, "text", cc_json_create_string(part->text ? part->text : ""))) {
                cc_json_destroy(obj);
                cc_json_destroy(arr);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to set content text");
            }
        } else {
            cc_json_value_t *artifact = NULL;
            cc_result_t rc = artifact_to_json_object(&part->artifact, &artifact);
            if (rc.code != CC_OK) {
                cc_result_t error = cc_result_errf(rc.code, "Content part[%zu]: %s", i,
                    rc.message ? rc.message : "artifact serialization failed");
                cc_result_free(&rc);
                cc_json_destroy(obj);
                cc_json_destroy(arr);
                return error;
            }
            cc_result_free(&rc);
            if (!cc_json_object_set_owned(obj, "artifact", artifact)) {
                cc_json_destroy(obj);
                cc_json_destroy(arr);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to set part artifact");
            }
        }
        if (!cc_json_array_append_owned(arr, obj)) {
            cc_json_destroy(arr);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to append content part JSON");
        }
    }
    *out_json = cc_json_stringify_unformatted(arr);
    cc_json_destroy(arr);
    return *out_json ? cc_result_ok()
                     : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to stringify content parts");
}

/**
 * @brief 完整解析内容数组，任一坏项或已知字段错误撤销整列表。
 * @param[in] json 稳定 NUL 文本，NULL/空串表示空列表，拒绝解码 NUL 和尾垃圾。
 * @param[out] out_parts 非 NULL 新/已清理包装，先初始化，失败归还全部已追加内容。
 * @return 成功为 CC_OK，空输出为 INVALID_ARGUMENT，损坏/schema/通用解析失败为 JSON，字段/列表分配失败为 OUT_OF_MEMORY。
 * @note 单个大小写键、缺失可选值、未知字符串默认及旧平铺媒体形式继续接受，原文不修改。
 */
cc_result_t cc_content_parts_from_json(const char *json, cc_content_parts_t *out_parts)
{
    if (!out_parts) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null content parts output");
    }
    cc_content_parts_init(out_parts);
    if (!json || !*json) return cc_result_ok();
    if (cc_storage_json_has_nul_escape(json))
        return cc_result_error(CC_ERR_JSON, "Content parts JSON contains a decoded NUL");
    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse_strict(json, &root);
    if (rc.code != CC_OK) return rc;
    if (!cc_json_is_array(root)) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_JSON, "Content parts JSON must be an array");
    }
    cc_result_free(&rc);
    int n = cc_json_array_size(root);
    int index = 0;
    cc_result_t error;
    for (; index < n; index++) {
        cc_json_value_t *obj = cc_json_array_get(root, index);
        if (!obj || !cc_json_is_object(obj)) {
            rc = cc_result_error(CC_ERR_JSON, "Content part must be an object");
            goto fail;
        }
        static const char *const fields[] = {"type", "direction", "text", "artifact"};
        const char *duplicate = cc_json_duplicate_known_field(obj, fields, sizeof(fields) / sizeof(fields[0]));
        if (duplicate) {
            rc = cc_result_errf(CC_ERR_JSON, "Duplicate content field '%s'", duplicate);
            goto fail;
        }
        static const char *const string_fields[] = {"type", "direction", "text"};
        for (size_t field = 0; field < sizeof(string_fields) / sizeof(string_fields[0]); field++) {
            cc_json_value_t *value = cc_json_object_get(obj, string_fields[field]);
            if (value && !cc_json_is_string(value)) {
                rc = cc_result_errf(CC_ERR_JSON, "Content field '%s' must be a string", string_fields[field]);
                goto fail;
            }
        }
        cc_json_value_t *artifact_obj = cc_json_object_get(obj, "artifact");
        if (artifact_obj && !cc_json_is_object(artifact_obj)) {
            rc = cc_result_error(CC_ERR_JSON, "Content field 'artifact' must be an object");
            goto fail;
        }
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
            if (!artifact_obj) artifact_obj = obj;
            rc = artifact_from_json_object(artifact_obj, &artifact);
            if (rc.code == CC_OK) {
                artifact.kind = kind;
                cc_result_free(&rc);
                rc = cc_content_parts_append_artifact(out_parts, &artifact, dir);
                cc_media_artifact_cleanup(&artifact);
            }
        }
        if (rc.code != CC_OK) goto fail;
        cc_result_free(&rc);
    }
    cc_json_destroy(root);
    return cc_result_ok();
fail:
    error = cc_result_errf(rc.code, "Content part[%d]: %s", index,
        rc.message ? rc.message : "invalid content part");
    cc_result_free(&rc);
    cc_json_destroy(root);
    cc_content_parts_cleanup(out_parts);
    return error;
}

/**
 * @brief 拼接原始文本和媒体占位符，项间加入换行。
 * @param[in] parts 非 NULL 稳定只读列表，items 覆盖 count。
 * @param[out] out_summary 非 NULL 独立输出槽，先置 NULL，成功接管 free 可释放缓冲。
 * @return 成功为 CC_OK，参数/形状错误为 INVALID_ARGUMENT，构造失败交付首个拥有错误并释放 builder。
 * @note 无内部锁，所有源字段借用覆盖构造期，输出槽不得覆盖输入字段。
 */
cc_result_t cc_content_parts_text_summary(
    const cc_content_parts_t *parts,
    char **out_summary
)
{
    if (!out_summary) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null content summary argument");
    }
    *out_summary = NULL;
    if (!parts || parts->count > SIZE_MAX / sizeof(*parts->items) || (parts->count && !parts->items))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid content summary parts");
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
