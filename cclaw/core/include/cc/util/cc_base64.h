#ifndef CC_BASE64_H
#define CC_BASE64_H

#include "cc/core/cc_result.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 将二进制编码为标准字母表和等号填充的 Base64 字符串。
 * @param[in] data 借用 data_len 字节，零长度时可为 NULL。
 * @param[in] data_len 输入字节数，长度算术溢出时拒绝。
 * @param[out] out_text 必填独立输出槽，先置 NULL，成功交付由调用者 free 的 NUL 结尾字符串。
 * @return CC_OK 或参数、长度上限、OOM 错误，返回结果的动态消息由调用者消费。
 */
cc_result_t cc_base64_encode(
    const unsigned char *data,
    size_t data_len,
    char **out_text
);

/**
 * @brief 解码标准或 URL 安全 Base64，忽略空格、制表、换行和回车。
 * @param[in] text 必填稳定借用文本，非空输入去空白后必须为四字符分组并具有合法填充位置。
 * @param[out] out_data 必填独立输出槽，两个槽有效后置 NULL，成功缓冲由调用者 free。
 * @param[out] out_len 必填独立输出槽，两个槽有效后置零，成功为实际二进制长度。
 * @return CC_OK、参数/格式错误或 OOM，错误保持有效输出为空，结果消息由调用者消费。
 * @note 保留非零填充位的宽松行为，输出不追加 NUL，空输入仍返回拥有缓冲及零长度。
 */
cc_result_t cc_base64_decode(
    const char *text,
    unsigned char **out_data,
    size_t *out_len
);

#ifdef __cplusplus
}
#endif

#endif
