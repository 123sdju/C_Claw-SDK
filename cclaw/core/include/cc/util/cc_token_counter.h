



#ifndef CC_TOKEN_COUNTER_H
#define CC_TOKEN_COUNTER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 用固定字节启发式估算文本 token，供上下文预算低成本参考。
 * @param[in] text 可选稳定借用的 NUL 结尾文本，UTF-8 序列只按首字节跳过且未校验。
 * @return 非负估计，NULL 和空串为零，超出 int 范围时为 INT_MAX。
 * @note ASCII 段按四字节向上取整，非 ASCII 序列按一个估算，精确分词和计费用 provider 数据。
 */
int cc_token_estimate(const char *text);

/**
 * @brief 对消息 JSON 文本复用普通文本估计。
 * @param[in] messages_text 可选稳定借用正文，不解析消息结构。
 * @return cc_token_estimate 的饱和非负结果，NULL 为零。
 */
int cc_token_estimate_json_messages(const char *messages_text);

#ifdef __cplusplus
}
#endif

#endif
