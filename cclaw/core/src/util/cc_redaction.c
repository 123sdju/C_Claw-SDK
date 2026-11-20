#include "cc/util/cc_redaction.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/internal/cc_json_syntax_check.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *const sensitive_keys[] = {
    "api_key",
    "apikey",
    "authorization",
    "bearer",
    "api-key",
    "token",
    "access_token",
    "refresh_token",
    "secret",
    "password",
    "passwd",
    "private_key",
    "client_secret"
};

/**
 * @brief 逐字节比较固定 ASCII 前缀，忽略 ASCII 字母大小写。
 * @param[in] a 必填 NUL 结尾输入借用，提前遇 NUL 即失败。
 * @param[in] b 至少含 n 个可读字节的模式借用，本文件传入固定敏感键。
 * @param[in] n 比较字节数，零长度返回一。
 * @return 前缀匹配为一，短输入或不匹配为零，ASCII 映射不依赖 locale。
 */
static int ascii_case_equal_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca == '\0') return 0;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
    }
    return 1;
}

/**
 * @brief 在文本当前位置匹配固定敏感键，并排除其后的单词延续字节。
 * @param[in] text 必填稳定 NUL 结尾文本。
 * @param[in] pos text 内的有效字节位置，由 fallback 扫描控制。
 * @return 命中敏感键且后继不是字母数字、下划线或短横线为一，其他为零。
 * @note 当前不要求左侧单词边界，可能保守遮盖包含敏感后缀的更长键。
 */
static int is_sensitive_at(const char *text, size_t pos)
{
    for (size_t i = 0; i < sizeof(sensitive_keys) / sizeof(sensitive_keys[0]); i++) {
        size_t len = strlen(sensitive_keys[i]);
        if (ascii_case_equal_n(text + pos, sensitive_keys[i], len)) {
            char c = text[pos + len];
            if (isalnum((unsigned char)c) || c == '_' || c == '-') continue;
            return 1;
        }
    }
    return 0;
}

/**
 * @brief 按 ASCII 大小写无关规则比较两个完整键名。
 * @param[in] a 可选稳定 NUL 结尾文本。
 * @param[in] b 可选稳定 NUL 结尾文本。
 * @return 两者均非 NULL 且完整相等为一，其他为零。
 */
static int ascii_case_equal(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/**
 * @brief 规范化键名并按敏感片段或完整固定键分类。
 * @param[in] key 可选稳定键名，NULL 为非敏感。
 * @return 包含已知敏感片段、完整键匹配或规范化长度溢出为一，其他为零。
 * @note 本地 128 字节缓冲有界，过长规范化键保守遮盖；字符分类沿当前 C locale。
 */
static int is_sensitive_key(const char *key)
{
    if (!key) return 0;
    char normalized[128];
    size_t len = 0;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        if (!isalnum(*p)) continue;
        if (len + 1 >= sizeof(normalized)) return 1;
        normalized[len++] = (char)tolower(*p);
    }
    normalized[len] = '\0';
    if (strstr(normalized, "apikey") || strstr(normalized, "authorization") ||
        strstr(normalized, "bearer") || strstr(normalized, "token") ||
        strstr(normalized, "secret") || strstr(normalized, "password") ||
        strstr(normalized, "passwd") || strstr(normalized, "privatekey")) {
        return 1;
    }
    for (size_t i = 0; i < sizeof(sensitive_keys) / sizeof(sensitive_keys[0]); i++) {
        if (ascii_case_equal(key, sensitive_keys[i])) return 1;
    }
    return 0;
}

/**
 * @brief 为精确拼写且类型正确的 token 指标提供保留例外。
 * @param[in] key 可选稳定键名，比较区分大小写。
 * @param[in] value 对应借用节点，必须为名单规定的数字或布尔类型。
 * @return 精确指标名及类型都匹配为一，其他为零，字符串和对象继续受敏感键规则遮盖。
 */
static int is_safe_token_metric(const char *key, const cc_json_value_t *value)
{
    if (!key) return 0;
    static const char *const numeric[] = {
        "prompt_tokens", "completion_tokens", "cached_tokens", "cache_write_tokens",
        "max_tokens", "summary_max_tokens", "context_window_tokens",
        "max_completion_tokens", "input_tokens", "output_tokens", "total_tokens",
        "cache_read_input_tokens", "cache_creation_input_tokens"
    };
    static const char *const flags[] = {
        "cached_tokens_reported", "cache_write_tokens_reported"
    };
    for (size_t i = 0; i < sizeof(numeric) / sizeof(numeric[0]); i++)
        if (strcmp(key, numeric[i]) == 0) return cc_json_is_number(value);
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
        if (strcmp(key, flags[i]) == 0) return cc_json_is_bool(value);
    return 0;
}

/**
 * @brief 复制输入为独立 NUL 结尾缓冲，NULL 使用空串。
 * @param[in] text 可选稳定文本借用。
 * @return 调用者 free 的拥有副本，分配失败为 NULL。
 */
static char *dup_text(const char *text)
{
    if (!text) text = "";
    size_t len = strlen(text) + 1;
    char *copy = malloc(len);
    if (copy) memcpy(copy, text, len);
    return copy;
}

/**
 * @brief 跳过前导空白，检查文本是否以对象或数组起始字符开头。
 * @param[in] text 可选稳定 NUL 结尾文本，NULL 使用空串。
 * @return 首个非空白字节为左花括号或左方括号时为一，其他为零，未验证语法。
 */
static int looks_like_json(const char *text)
{
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    while (*p && isspace(*p)) p++;
    return *p == '{' || *p == '[';
}

/**
 * @brief 从键末尾扫描一个日志值的结束位置，支持引号、转义和 Bearer 前缀。
 * @param[in] text 必填稳定 NUL 结尾文本。
 * @param[in] i 键末尾的有效下标，可指向闭引号、分隔符或空白。
 * @return 值结束的有效下标，可能指向闭引号、分隔符、空白或末尾 NUL。
 * @note 这是日志词法扫描，分隔规则不覆盖任意 JSON 或任意凭据格式。
 */
static size_t skip_sensitive_value(const char *text, size_t i)
{
    if (text[i] == '"') i++;
    while (text[i] && isspace((unsigned char)text[i])) i++;
    if (text[i] == ':' || text[i] == '=') i++;
    while (text[i] && isspace((unsigned char)text[i])) i++;

    if (ascii_case_equal_n(text + i, "bearer", 6) &&
        isspace((unsigned char)text[i + 6])) {
        i += 6;
        while (text[i] && isspace((unsigned char)text[i])) i++;
        while (text[i] && text[i] != ',' && text[i] != '&' &&
               text[i] != '\n' && text[i] != '\r' && text[i] != '}' &&
               text[i] != '"' && !isspace((unsigned char)text[i])) {
            i++;
        }
        return i;
    }

    if (text[i] == '"') {
        i++;
        while (text[i]) {
            if (text[i] == '\\' && text[i + 1]) {
                i += 2;
                continue;
            }
            if (text[i] == '"') return i;
            i++;
        }
        return i;
    }

    while (text[i] && text[i] != ',' && text[i] != '&' &&
           text[i] != '\n' && text[i] != '\r' && text[i] != '}' &&
           !isspace((unsigned char)text[i])) {
        i++;
    }
    return i;
}

/**
 * @brief 复制普通日志并将匹配键后的值字节替换为星号，保留引号和原长度。
 * @param[in] text 可选稳定文本，NULL 生成拥有空串。
 * @return 调用者 free 的拥有文本，复制失败为 NULL。
 * @note 只识别固定键及本文件分隔规则，未知字段和自由文本凭据由上层避免写入。
 */
static char *redact_text_fallback(const char *text)
{
    char *out = dup_text(text);
    if (!out) return NULL;

    size_t len = strlen(out);
    for (size_t i = 0; i < len; i++) {
        if (!is_sensitive_at(out, i)) continue;

        size_t key_end = i;
        while (out[key_end] && (isalnum((unsigned char)out[key_end]) ||
               out[key_end] == '_' || out[key_end] == '-')) {
            key_end++;
        }

        size_t value_start = key_end;
        if (out[value_start] == '"') value_start++;
        while (out[value_start] && isspace((unsigned char)out[value_start])) value_start++;
        if (out[value_start] != ':' && out[value_start] != '=') continue;
        value_start++;
        while (out[value_start] && isspace((unsigned char)out[value_start])) value_start++;
        if (out[value_start] == '"') value_start++;

        size_t value_end = skip_sensitive_value(out, key_end);
        for (size_t j = value_start; j < value_end; j++) {
            if (out[j] != '"' && out[j] != '\0') out[j] = '*';
        }
        i = value_end;
    }
    return out;
}

/**
 * @brief 递归构造脱敏 JSON 树，识别敏感键和字符串中嵌套的 JSON。
 * @param[in] value 稳定借用节点，NULL 生成 JSON null。
 * @param[in] key 所属对象的可选键名借用，数组元素和根为 NULL。
 * @param[in] depth 当前深度，超过 16 层遮盖整项，嵌套 JSON 字符串仅在小于八层时解析。
 * @return 调用者 destroy 的拥有树，分配或字段挂接失败清理部分树并返回 NULL。
 * @note 精确且类型正确的 token 指标保留，其他敏感键的任意类型值都替换为固定遮盖文本。
 */
static cc_json_value_t *json_redacted_copy(
    const cc_json_value_t *value,
    const char *key,
    unsigned depth)
{
    if (is_sensitive_key(key) && !is_safe_token_metric(key, value)) {
        return cc_json_create_string("[REDACTED]");
    }
    if (depth > 16U) return cc_json_create_string("[REDACTED]");
    if (cc_json_is_object(value)) {
        cc_json_value_t *copy = cc_json_create_object();
        if (!copy) return NULL;
        int count = cc_json_object_size(value);
        for (int i = 0; i < count; i++) {
            const char *child_key = cc_json_object_key_at(value, i);
            cc_json_value_t *child = cc_json_object_value_at(value, i);
            cc_json_value_t *child_copy = json_redacted_copy(child, child_key, depth + 1U);
            if (!child_copy) {
                cc_json_destroy(copy);
                return NULL;
            }
            if (!cc_json_object_set_owned(copy, child_key ? child_key : "", child_copy)) {
                cc_json_destroy(copy);
                return NULL;
            }
        }
        return copy;
    }
    if (cc_json_is_array(value)) {
        cc_json_value_t *copy = cc_json_create_array();
        if (!copy) return NULL;
        int count = cc_json_array_size(value);
        for (int i = 0; i < count; i++) {
            cc_json_value_t *child_copy = json_redacted_copy(
                cc_json_array_get(value, i), NULL, depth + 1U);
            if (!child_copy) {
                cc_json_destroy(copy);
                return NULL;
            }
            cc_json_array_append(copy, child_copy);
        }
        return copy;
    }
    if (cc_json_is_string(value)) {
        const char *string_value = cc_json_string_value(value);
        if (depth < 8U && looks_like_json(string_value)) {
            cc_json_value_t *nested = NULL;
            cc_result_t parse_rc = cc_json_parse(string_value, &nested);
            if (parse_rc.code == CC_OK && nested) {
                cc_json_value_t *nested_copy = json_redacted_copy(nested, NULL, depth + 1U);
                cc_json_destroy(nested);
                cc_result_free(&parse_rc);
                if (!nested_copy) return NULL;
                char *serialized = cc_json_stringify_unformatted(nested_copy);
                cc_json_destroy(nested_copy);
                if (!serialized) return NULL;
                cc_json_value_t *copy = cc_json_create_string(serialized);
                free(serialized);
                return copy;
            }
            if (nested) cc_json_destroy(nested);
            cc_result_free(&parse_rc);
            if (cc_json_syntax_check(string_value) != CC_JSON_SYNTAX_INVALID) return NULL;
        }
        char *redacted = redact_text_fallback(string_value);
        if (!redacted) return NULL;
        cc_json_value_t *copy = cc_json_create_string(redacted);
        free(redacted);
        return copy;
    }
    if (cc_json_is_number(value)) return cc_json_create_number(cc_json_number_value(value));
    if (cc_json_is_bool(value)) return cc_json_create_bool(cc_json_bool_value(value));
    return cc_json_create_null();
}

/**
 * @brief 为 JSON 或普通日志生成拥有的脱敏文本，完整结构化路径失败时交付 NULL。
 * @param[in] text 可选稳定 NUL 结尾文本，NULL 按普通空串处理。
 * @return 调用者 free 的拥有结果，分配、挂接、打印或未能确定的结构化解析失败返回 NULL。
 * @note 明确坏语法和普通文本沿固定键 fallback 扫描，调用方用静态失败标记处理 NULL。
 * 本函数无共享可变状态，输入寿命与 cJSON 全局分配钩子由外层协调。
 */
char *cc_redact_secrets(const char *text)
{
    if (looks_like_json(text)) {
        cc_json_value_t *root = NULL;
        cc_result_t rc = cc_json_parse(text ? text : "", &root);
        cc_result_free(&rc);
        if (rc.code == CC_OK && root) {
            cc_json_value_t *copy = json_redacted_copy(root, NULL, 0);
            cc_json_destroy(root);
            if (copy) {
                char *redacted = cc_json_stringify_unformatted(copy);
                cc_json_destroy(copy);
                if (redacted) return redacted;
            }
            return NULL;
        } else {
            if (root) cc_json_destroy(root);
            if (cc_json_syntax_check(text) != CC_JSON_SYNTAX_INVALID) return NULL;
        }
    }
    return redact_text_fallback(text);
}
