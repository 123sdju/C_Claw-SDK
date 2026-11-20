#ifndef CC_JSON_SYNTAX_CHECK_H
#define CC_JSON_SYNTAX_CHECK_H

#include <string.h>

/**
 * @brief 扫描 JSON 原文中会解码为 NUL 的反斜杠 u0000 转义，跳过成对转义的反斜杠。
 * @param[in] json 借用的 NUL 结尾 JSON 原文，可为 NULL。
 * @return 发现 NUL 转义为一，NULL 或未发现为零。整体语法由解析器检查。
 * @note 此无分配检查用于要求 C 字符串可无损表示的存储及输入边界，通用 JSON 解析仍允许合法 u0000。
 */
static inline int cc_storage_json_has_nul_escape(const char *json)
{
    if (!json) return 0;
    for (const char *p = json; *p; p++) {
        if (*p != '\\' || !p[1]) continue;
        if (strncmp(p + 1, "u0000", 5) == 0) return 1;
        p++;
    }
    return 0;
}

/**
 * @brief 无分配的有限 JSON 语法识别结果，供解析失败后区分可证明的语法损坏与未定状态。
 *
 * 常规解析器将语法错误与分配失败合并报告，存储用此识别器辅助判断。
 * VALID 只表示本识别器的词法与结构检查通过，Unicode 和业务约束另由对应解析与校验路径处理。
 */
typedef enum cc_json_syntax_status {
    CC_JSON_SYNTAX_VALID, /**< 已完整消费一个满足本识别器规则的 JSON 值。 */
    CC_JSON_SYNTAX_INVALID, /**< 已发现明确的词法或结构错误。 */
    CC_JSON_SYNTAX_INCONCLUSIVE /**< NULL 输入或嵌套超过 64 层，单凭此状态无法判为损坏。 */
} cc_json_syntax_status_t;

/**
 * @brief 判断一个字符是否为 ASCII 十六进制数字。
 * @param[in] c 待检查的单字节字符。
 * @return 0 至 9、a 至 f 或 A 至 F 为一，其他字符为零。
 */
static inline int cc_json_syntax_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/**
 * @brief 识别一个双引号字符串，拒绝未转义控制字节与非法转义形式。
 * @param[in,out] input 有效游标槽，*input 指向有效的 NUL 结尾文本，成功移到闭引号之后，失败保持原值。
 * @return 字符串完整且转义形式合法为一，其他情况为零。
 * @note Unicode 转义只要求四位十六进制，允许 u0000，代理项配对及原始 UTF-8 字节由其他层处理。
 */
static inline int cc_json_syntax_string(const char **input)
{
    const unsigned char *p = (const unsigned char *)*input;
    if (*p++ != '"') return 0;
    while (*p && *p != '"') {
        if (*p < 0x20U) return 0;
        if (*p++ != '\\') continue;
        unsigned char escape = *p++;
        if (!escape) return 0;
        if (escape == 'u') {
            for (int i = 0; i < 4; i++) {
                if (!*p || !cc_json_syntax_hex((char)*p)) return 0;
                p++;
            }
        } else if (!strchr("\"\\/bfnrt", (int)escape)) return 0;
    }
    if (*p != '"') return 0;
    *input = (const char *)(p + 1);
    return 1;
}

/**
 * @brief 识别可选负号、整数、小数和指数组成的 JSON 数字前缀。
 * @param[in,out] input 有效游标槽，成功推进到数字之后，失败保持原值，容器或根状态随后检查分隔符。
 * @return 数字各已出现部分均完整为一，缺少必需数字为零，函数仅检查词法形式。
 * @note 前导零后的多余数字由外层状态拒绝，数值精度和范围由后续业务层校验。
 */
static inline int cc_json_syntax_number(const char **input)
{
    const char *p = *input;
    if (*p == '-') p++;
    if (*p == '0') p++;
    else {
        if (*p < '1' || *p > '9') return 0;
        do { p++; } while (*p >= '0' && *p <= '9');
    }
    if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9') return 0;
        do { p++; } while (*p >= '0' && *p <= '9');
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (*p < '0' || *p > '9') return 0;
        do { p++; } while (*p >= '0' && *p <= '9');
    }
    *input = p;
    return 1;
}

/**
 * @brief 使用固定状态栈检查一个完整 JSON 值的词法和容器结构。
 * @param[in] text 借用的 NUL 结尾文本，可为 NULL，空白仅接受空格、制表符、换行和回车。
 * @return 完整通过为 CC_JSON_SYNTAX_VALID，发现结构或词法错误为 CC_JSON_SYNTAX_INVALID，
 * NULL 或容器嵌套超过 64 层为 CC_JSON_SYNTAX_INCONCLUSIVE。
 * @note 全程无动态分配，检查数字词法、字符串转义和尾部内容。重复对象键、Unicode 代理项配对、
 * 原始 UTF-8 合法性及业务字段约束由其他层判断，数字只做词法识别。
 */
static inline cc_json_syntax_status_t cc_json_syntax_check(const char *text)
{
    enum { ROOT_VALUE, ROOT_END, ARRAY_FIRST, ARRAY_VALUE, ARRAY_NEXT,
           OBJECT_FIRST, OBJECT_KEY, OBJECT_COLON, OBJECT_VALUE, OBJECT_NEXT };
    unsigned char states[65] = {ROOT_VALUE};
    unsigned depth = 0;
    const char *p = text;
    if (!p) return CC_JSON_SYNTAX_INCONCLUSIVE;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        unsigned char state = states[depth];
        if (state == ROOT_END) return *p ? CC_JSON_SYNTAX_INVALID : CC_JSON_SYNTAX_VALID;
        if (state == ARRAY_FIRST && *p == ']') { p++; depth--; continue; }
        if (state == OBJECT_FIRST && *p == '}') { p++; depth--; continue; }
        if (state == ARRAY_NEXT || state == OBJECT_NEXT) {
            if (*p == (state == ARRAY_NEXT ? ']' : '}')) { p++; depth--; continue; }
            if (*p++ != ',') return CC_JSON_SYNTAX_INVALID;
            states[depth] = state == ARRAY_NEXT ? ARRAY_VALUE : OBJECT_KEY;
            continue;
        }
        if (state == OBJECT_FIRST || state == OBJECT_KEY) {
            if (*p != '"' || !cc_json_syntax_string(&p)) return CC_JSON_SYNTAX_INVALID;
            states[depth] = OBJECT_COLON;
            continue;
        }
        if (state == OBJECT_COLON) {
            if (*p++ != ':') return CC_JSON_SYNTAX_INVALID;
            states[depth] = OBJECT_VALUE;
            continue;
        }
        states[depth] = state == ROOT_VALUE ? ROOT_END :
                        (state == OBJECT_VALUE ? OBJECT_NEXT : ARRAY_NEXT);
        if (*p == '[' || *p == '{') {
            if (depth == 64U) return CC_JSON_SYNTAX_INCONCLUSIVE;
            states[++depth] = *p++ == '[' ? ARRAY_FIRST : OBJECT_FIRST;
        } else if (*p == '"') {
            if (!cc_json_syntax_string(&p)) return CC_JSON_SYNTAX_INVALID;
        } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
            if (!cc_json_syntax_number(&p)) return CC_JSON_SYNTAX_INVALID;
        } else if (strncmp(p, "true", 4) == 0 || strncmp(p, "null", 4) == 0) p += 4;
        else if (strncmp(p, "false", 5) == 0) p += 5;
        else return CC_JSON_SYNTAX_INVALID;
    }
}

#endif
