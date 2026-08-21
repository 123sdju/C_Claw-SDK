#include "cc/util/cc_redaction.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *const sensitive_keys[] = {
    "api_key",
    "apikey",
    "authorization",
    "bearer",
    "token",
    "access_token",
    "refresh_token",
    "secret",
    "password",
    "passwd"
};

/*
 * 比较前 n 个 ASCII 字符，忽略大小写。
 *
 * 脱敏 key 只定义为 ASCII，避免 locale 相关行为；这比 strncasecmp 更容易在 MCU C 库中
 * 移植。
 */
static int ascii_case_equal_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (tolower(ca) != tolower(cb)) return 0;
    }
    return 1;
}

/*
 * 判断 text 的某个位置是否出现敏感 key。
 *
 * fallback 文本扫描不能依赖 JSON 结构，因此只做保守匹配：key 后面如果仍是字母数字、
 * 下划线或短横线，就认为它只是更长单词的一部分。
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

/* 完整 ASCII 忽略大小写比较，用于 JSON object key 判断。 */
static int ascii_case_equal(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (tolower(ca) != tolower(cb)) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* 判断 JSON object key 是否属于敏感字段集合。 */
static int is_sensitive_key(const char *key)
{
    if (!key) return 0;
    for (size_t i = 0; i < sizeof(sensitive_keys) / sizeof(sensitive_keys[0]); i++) {
        if (ascii_case_equal(key, sensitive_keys[i])) return 1;
    }
    return 0;
}

/* 复制文本；NULL 输入按空字符串处理，方便 fallback 路径保持可返回字符串。 */
static char *dup_text(const char *text)
{
    if (!text) text = "";
    size_t len = strlen(text) + 1;
    char *copy = malloc(len);
    if (copy) memcpy(copy, text, len);
    return copy;
}

/*
 * 快速判断文本是否可能是 JSON 格式（以 { 或 [ 开头）。
 *
 * 参数:
 *   text - 待检测文本
 * 返回: 1 如果看起来像 JSON，否则 0。
 *
 * 跳过前导空白字符后检查首字符是否为 { 或 [。
 * 用于脱敏模块判断是否需要对事件 payload 进行 JSON 字段脱敏。
 */
static int looks_like_json(const char *text)
{
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    while (*p && isspace(*p)) p++;
    return *p == '{' || *p == '[';
}

/*
 * 跳过敏感 key 后面的 value 范围。
 *
 * fallback 扫描支持 key:value、key=value、带引号字符串和常见 query/log 分隔符。它不是
 * 完整 parser，只用于非法 JSON 或普通日志文本的保守脱敏。
 */
static size_t skip_sensitive_value(const char *text, size_t i)
{
    if (text[i] == '"') i++;
    while (text[i] && isspace((unsigned char)text[i])) i++;
    if (text[i] == ':' || text[i] == '=') i++;
    while (text[i] && isspace((unsigned char)text[i])) i++;

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

/*
 * 普通文本 fallback 脱敏。
 *
 * 复制原文后原地把敏感值字符替换为 '*'，尽量保留原始日志形状，便于排查问题同时不
 * 泄漏 secret。
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

/*
 * 递归复制 JSON 并脱敏敏感 key 的值。
 *
 * 命中敏感 key 时无论原值类型如何都替换为 "[REDACTED]"；数组和对象递归处理，普通
 * 标量按原类型复制。返回的新 AST 由调用方 destroy。
 */
static cc_json_value_t *json_redacted_copy(const cc_json_value_t *value, const char *key)
{
    if (is_sensitive_key(key)) {
        return cc_json_create_string("[REDACTED]");
    }
    if (cc_json_is_object(value)) {
        cc_json_value_t *copy = cc_json_create_object();
        if (!copy) return NULL;
        int count = cc_json_object_size(value);
        for (int i = 0; i < count; i++) {
            const char *child_key = cc_json_object_key_at(value, i);
            cc_json_value_t *child = cc_json_object_value_at(value, i);
            cc_json_value_t *child_copy = json_redacted_copy(child, child_key);
            if (!child_copy) {
                cc_json_destroy(copy);
                return NULL;
            }
            cc_json_object_set(copy, child_key ? child_key : "", child_copy);
        }
        return copy;
    }
    if (cc_json_is_array(value)) {
        cc_json_value_t *copy = cc_json_create_array();
        if (!copy) return NULL;
        int count = cc_json_array_size(value);
        for (int i = 0; i < count; i++) {
            cc_json_value_t *child_copy = json_redacted_copy(cc_json_array_get(value, i), NULL);
            if (!child_copy) {
                cc_json_destroy(copy);
                return NULL;
            }
            cc_json_array_append(copy, child_copy);
        }
        return copy;
    }
    if (cc_json_is_string(value)) return cc_json_create_string(cc_json_string_value(value));
    if (cc_json_is_number(value)) return cc_json_create_number(cc_json_number_value(value));
    if (cc_json_is_bool(value)) return cc_json_create_bool(cc_json_bool_value(value));
    return cc_json_create_null();
}

/*
 * 对 JSON 或文本进行脱敏。
 *
 * 优先按 JSON 解析并结构化替换敏感字段；JSON 解析失败时退回普通文本扫描。event bus
 * 和 logger 依赖这个函数做最后防线，因此它不应该返回原始 secret。
 */
char *cc_redact_secrets(const char *text)
{
    if (looks_like_json(text)) {
        cc_json_value_t *root = NULL;
        cc_result_t rc = cc_json_parse(text ? text : "", &root);
        if (rc.code == CC_OK && root) {
            cc_json_value_t *copy = json_redacted_copy(root, NULL);
            cc_json_destroy(root);
            if (copy) {
                char *redacted = cc_json_stringify_unformatted(copy);
                cc_json_destroy(copy);
                if (redacted) return redacted;
            }
        } else {
            if (root) cc_json_destroy(root);
            cc_result_free(&rc);
        }
    }
    return redact_text_fallback(text);
}
