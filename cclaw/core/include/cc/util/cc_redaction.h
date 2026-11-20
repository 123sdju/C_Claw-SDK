#ifndef CC_REDACTION_H
#define CC_REDACTION_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 生成按已知敏感键遮盖后的 JSON 或普通日志副本。
 * @param[in] text 调用期间稳定借用的可选文本，NULL 按空串复制。
 * @return 调用者 free 的拥有字符串，分配或结构化处理失败为 NULL，调用方使用静态失败标记。
 * @note JSON 递归替换敏感值，精确且类型正确的 token 指标保留，过深内容遮盖。
 * 普通文本和明确坏语法使用固定键扫描，未知格式的凭据仍由上层避免写入日志。
 */
char *cc_redact_secrets(const char *text);

#ifdef __cplusplus
}
#endif

#endif
