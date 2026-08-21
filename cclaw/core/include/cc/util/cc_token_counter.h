



#ifndef CC_TOKEN_COUNTER_H
#define CC_TOKEN_COUNTER_H

/*
 * 粗略估算文本 token 数。
 *
 * 该函数不是模型 tokenizer，只用于 context window 裁剪的保守估计。嵌入式场景可用它
 * 避免引入大 tokenizer 表。
 */
int cc_token_estimate(const char *text);

/*
 * 粗略估算 messages JSON 的 token 数。
 *
 * 用于 context builder 判断历史消息是否接近窗口上限；精确计费仍应以 provider 返回为准。
 */
int cc_token_estimate_json_messages(const char *messages_text);

#endif
