#ifndef CC_REDACTION_H
#define CC_REDACTION_H

/*
 * 对 JSON 或普通文本做敏感字段脱敏。
 *
 * 合法 JSON 会按 key 递归替换 api_key、authorization、token、secret、password 等字段；
 * 非 JSON 文本走 fallback 扫描。返回字符串由调用方 free()；NULL 输入可返回 NULL。
 */
char *cc_redact_secrets(const char *text);

#endif
