#ifndef CC_JSON_RECORD_VALIDATION_H
#define CC_JSON_RECORD_VALIDATION_H

#include "cc/util/cc_json.h"
#include "cc/internal/cc_json_syntax_check.h"
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CC_JSON_MAX_MESSAGE_PARTS 4096U

/**
 * @brief 按 tolower 逐字节比较字段名，适配 cJSON 的忽略大小写查找。
 * @param[in] a 有效的借用字段名。
 * @param[in] b 有效的借用字段名。
 * @return 折叠大小写后整个字符串相等为一，其他情况为零，字符折叠遵循当前 C 区域设置。
 */
static int cc_storage_json_field_equal(const char *a, const char *b)
{
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

/**
 * @brief 限制对象最多 64 项，并拒绝所列业务字段的忽略大小写重复。
 * @param[in] obj 借用的 JSON 节点，NULL 或非对象检查失败。
 * @param[in] fields 借用的有效业务字段名数组，count 为零时可为 NULL。
 * @param[in] count 业务字段名数量，最多 64，各名称须互不重复。
 * @return 对象及业务字段唯一性通过为一，其他情况为零，无内存分配。
 * @note 未列出的字段允许存在且允许重复。限制对象大小可约束按索引遍历链表的二次开销。
 */
static int cc_storage_json_fields_unique(const cc_json_value_t *obj, const char *const *fields, size_t count)
{
    if (!cc_json_is_object(obj) || count > 64U) return 0;
    unsigned long long seen = 0;
    int n = cc_json_object_size(obj);
    if (n > 64) return 0;
    for (int i = 0; i < n; i++) {
        const char *key = cc_json_object_key_at(obj, i);
        if (!key) return 0;
        for (size_t k = 0; k < count; k++) {
            if (cc_storage_json_field_equal(key, fields[k])) {
                unsigned long long bit = 1ULL << k;
                if (seen & bit) return 0;
                seen |= bit;
                break;
            }
        }
    }
    return 1;
}

/**
 * @brief 为 qsort 按字符串字节顺序比较工具调用 ID。
 * @param[in] a 指向有效借用 ID 字符串指针的数组槽。
 * @param[in] b 指向另一个有效借用 ID 字符串指针的数组槽。
 * @return strcmp 的负数、零或正数，分别表示前者较小、相等或较大。
 */
static int cc_storage_json_compare_ids(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/**
 * @brief 校验持久化媒体描述的已知字段、媒体种类及整数转换边界。
 * @param[in] item 借用的媒体对象，须通过已知字段唯一性检查，全部对象项最多 64 个。
 * @param[in] expected_kind 可选借用的外层媒体类型，须由调用者保证为 image、audio、video 或 file。
 * 存在时允许省略 kind，显式 kind 必须与之相同，NULL 时必须有合法 kind。
 * @return 通过为 CC_OK，种类、字段类型、重复业务字段或数值非法为 CC_ERR_STORAGE，错误消息由调用者 cc_result_free。
 * @note bytes 必须为非负整数且严格小于 double 表示的 SIZE_MAX，边界采用保守限制。
 * width、height、duration_ms 须为零至 INT_MAX 的整数。created_at、metadata 等可选字符串只检查类型。
 * 函数保持节点借用关系，媒体路径可访问性、Base64 内容、日期格式及 UTF-8 合法性由对应使用层处理。
 */
static inline cc_result_t cc_storage_json_validate_artifact(
    const cc_json_value_t *item, const char *expected_kind)
{
    static const char *const fields[] = {"kind", "id", "mime", "path", "uri", "data_base64",
                                         "bytes", "width", "height", "duration_ms", "created_at", "metadata"};
    static const char *const strings[] = {"id", "mime", "path", "uri", "data_base64", "created_at", "metadata"};
    static const char *const numbers[] = {"bytes", "width", "height", "duration_ms"};
    cc_json_value_t *kind_value = cc_json_object_get(item, "kind");
    const char *kind = cc_json_string_value(kind_value);
    if (!cc_storage_json_fields_unique(item, fields, 12) ||
        (kind_value && (!kind || (strcmp(kind, "image") && strcmp(kind, "audio") &&
         strcmp(kind, "video") && strcmp(kind, "file")))) ||
        (!expected_kind && !kind) || (expected_kind && kind && strcmp(expected_kind, kind)))
        return cc_result_error(CC_ERR_STORAGE, "Stored artifact kind or fields are invalid");
    for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
        cc_json_value_t *value = cc_json_object_get(item, strings[i]);
        if (value && !cc_json_is_string(value))
            return cc_result_error(CC_ERR_STORAGE, "Stored artifact string is invalid");
    }
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        cc_json_value_t *value = cc_json_object_get(item, numbers[i]);
        if (!value) continue;
        double n = cc_json_number_value(value);
        if (!cc_json_is_number(value) || !(n >= 0) ||
            (i == 0 && (!(n < (double)SIZE_MAX) || (double)(size_t)n != n)) ||
            (i != 0 && (!(n <= INT_MAX) || (double)(int)n != n)))
            return cc_result_error(CC_ERR_STORAGE, "Stored artifact number is invalid");
    }
    return cc_result_ok();
}

/**
 * @brief 在调用通用消息解析器前校验持久化内容数组或工具调用数组。
 * @param[in] json 借用的完整 JSON 数组字符串，NULL、解码 NUL 转义均拒绝，数组最多 4096 项。
 * @param[in] tools 非零校验工具调用，为零校验文本或媒体内容分片。
 * @return 通过为 CC_OK，业务字段、类型或工具调用 ID 重复为 CC_ERR_STORAGE，项数超限为 CC_ERR_LIMIT_EXCEEDED，
 * ID 暂存分配失败为 CC_ERR_OUT_OF_MEMORY，cc_json_parse_strict 错误原样返回，结果消息由调用者 cc_result_free。
 * @note 工具调用要求非空 id/name 和字符串 arguments，type 可省略，存在时须为 function。
 * 内容分片要求已知 type，direction 可省略，存在时须为 input/output。文本要求字符串 text，媒体可使用内嵌 artifact 或平铺字段。
 * 已知业务字段检查忽略大小写重复，工具 ID 按字节检查重复，arguments 的内层 JSON 语法由后续使用者处理。
 * 所有出口释放本次 JSON 树及 ID 指针数组，输入保持借用。
 */
static inline cc_result_t cc_storage_json_validate_array(const char *json, int tools)
{
    if (!json || cc_storage_json_has_nul_escape(json))
        return cc_result_error(CC_ERR_STORAGE, "Stored message array is missing or contains NUL");
    cc_json_value_t *array = NULL;
    cc_result_t rc = cc_json_parse_strict(json, &array);
    if (rc.code != CC_OK) return rc;
    if (!cc_json_is_array(array)) {
        cc_json_destroy(array);
        return cc_result_error(CC_ERR_STORAGE, "Stored message field is not an array");
    }
    int n = cc_json_array_size(array);
    if ((unsigned)n > CC_JSON_MAX_MESSAGE_PARTS) {
        cc_json_destroy(array);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Stored message array is too large");
    }
    const char **ids = tools && n > 1 ? calloc((size_t)n, sizeof(*ids)) : NULL;
    if (tools && n > 1 && !ids) {
        cc_json_destroy(array);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot validate stored tool-call ids");
    }
    int i = 0;
    for (const cc_json_value_t *item = cc_json_array_first(array); item;
         item = cc_json_array_next(item), i++) {
        if (tools) {
            static const char *const item_fields[] = {"id", "type", "function"};
            static const char *const function_fields[] = {"name", "arguments"};
            cc_json_value_t *function = cc_json_object_get(item, "function");
            const char *id = cc_json_string_value(cc_json_object_get(item, "id"));
            const char *type = cc_json_string_value(cc_json_object_get(item, "type"));
            const char *name = cc_json_string_value(cc_json_object_get(function, "name"));
            if (!cc_storage_json_fields_unique(item, item_fields, 3) ||
                !cc_storage_json_fields_unique(function, function_fields, 2) ||
                !id || !id[0] || !name || !name[0] ||
                (cc_json_object_get(item, "type") && (!type || strcmp(type, "function") != 0)) ||
                !cc_json_is_string(cc_json_object_get(function, "arguments"))) {
                rc = cc_result_error(CC_ERR_STORAGE, "Stored tool call is invalid");
                break;
            }
            if (ids) ids[i] = id;
        } else {
            static const char *const fields[] = {"type", "direction", "text", "artifact"};
            const char *type = cc_json_string_value(cc_json_object_get(item, "type"));
            const char *direction = cc_json_string_value(cc_json_object_get(item, "direction"));
            int text = type && strcmp(type, "text") == 0;
            int media = type && (strcmp(type, "image") == 0 || strcmp(type, "audio") == 0 ||
                                 strcmp(type, "video") == 0 || strcmp(type, "file") == 0);
            cc_json_value_t *artifact = cc_json_object_get(item, "artifact");
            if (!cc_storage_json_fields_unique(item, fields, 4) || (!text && !media) ||
                (cc_json_object_get(item, "direction") &&
                 (!direction || (strcmp(direction, "input") != 0 && strcmp(direction, "output") != 0))) ||
                (text && !cc_json_is_string(cc_json_object_get(item, "text"))) ||
                (media && artifact && !cc_json_is_object(artifact))) {
                rc = cc_result_error(CC_ERR_STORAGE, "Stored content part is invalid");
                break;
            }
            if (media) {
                rc = cc_storage_json_validate_artifact(artifact ? artifact : item, type);
                if (rc.code != CC_OK) break;
            }
        }
    }
    if (rc.code == CC_OK && ids) {
        qsort(ids, (size_t)n, sizeof(*ids), cc_storage_json_compare_ids);
        for (int i = 1; i < n; i++) {
            if (strcmp(ids[i - 1], ids[i]) == 0) {
                rc = cc_result_error(CC_ERR_STORAGE, "Stored message repeats a tool-call id");
                break;
            }
        }
    }
    free(ids);
    cc_json_destroy(array);
    return rc;
}

#endif
