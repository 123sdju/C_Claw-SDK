#include "cc/util/cc_base64.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static const char k_base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/**
 * @brief 将标准或 URL 安全 Base64 字节分类为六位索引、填充或空白。
 * @param[in] ch 单个输入字节，支持 +/ 与 -_ 两套末尾字符。
 * @return 0..63 为数据，-2 为等号，-3 为四类允许空白，-1 为其他非法字节。
 */
static int base64_index(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+' || ch == '-') return 62;
    if (ch == '/' || ch == '_') return 63;
    if (ch == '=') return -2;
    if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') return -3;
    return -1;
}

/**
 * @brief 检查长度算术并将二进制编码为带等号填充的标准 Base64。
 * @param[in] data 调用期间稳定借用的 data_len 字节，长度为零时允许 NULL。
 * @param[in] data_len 输入字节数，输出每三字节生成四字符并另加 NUL。
 * @param[out] out_text 必填独立输出槽，有效槽先置 NULL，成功字符串由调用者 free。
 * @return 成功为 CC_OK，参数错误、长度溢出和分配失败分别返回拥有错误结果。
 * @note 空输入也交付一个拥有空字符串，接口不带共享状态或内部锁。
 */
cc_result_t cc_base64_encode(
    const unsigned char *data,
    size_t data_len,
    char **out_text
)
{
    if (!out_text) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null base64 output");
    }
    *out_text = NULL;
    if (!data && data_len > 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null base64 input");
    }

    if (data_len > SIZE_MAX - 2U) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Base64 input length overflow");
    }
    size_t groups = (data_len + 2U) / 3U;
    if (groups > (SIZE_MAX - 1U) / 4U) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Base64 output length overflow");
    }
    size_t out_len = groups * 4U;
    char *out = malloc(out_len + 1);
    if (!out) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate base64 output");
    }

    size_t i = 0;
    size_t j = 0;
    while (i < data_len) {
        unsigned int octet_a = i < data_len ? data[i++] : 0;
        unsigned int octet_b = i < data_len ? data[i++] : 0;
        unsigned int octet_c = i < data_len ? data[i++] : 0;
        unsigned int triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out[j++] = k_base64_chars[(triple >> 18) & 0x3f];
        out[j++] = k_base64_chars[(triple >> 12) & 0x3f];
        out[j++] = (i - 1 > data_len) ? '=' : k_base64_chars[(triple >> 6) & 0x3f];
        out[j++] = (i > data_len) ? '=' : k_base64_chars[triple & 0x3f];
    }

    if (data_len % 3 == 1) {
        out[out_len - 2] = '=';
        out[out_len - 1] = '=';
    } else if (data_len % 3 == 2) {
        out[out_len - 1] = '=';
    }
    out[out_len] = '\0';
    *out_text = out;
    return cc_result_ok();
}

/**
 * @brief 校验字符、四字节分组和填充位置后解码 Base64 字节。
 * @param[in] text 必填稳定借用的 NUL 结尾文本，支持两套字母表及空格、制表、回车和换行。
 * @param[out] out_data 必填独立输出槽，两个输出槽有效后先置 NULL，成功缓冲由调用者 free。
 * @param[out] out_len 必填独立长度槽，两个输出槽有效后先置零，成功返回实际二进制字节数。
 * @return 成功为 CC_OK，字符、长度、填充或参数错误为 INVALID_ARGUMENT，分配失败为 OOM。
 * @note 非空输入清理空白后必须为四的倍数，支持非零填充位的既有宽松解码。
 * 空输入分配一字节但返回长度零，输出为二进制且不追加字符串终止符。
 */
cc_result_t cc_base64_decode(
    const char *text,
    unsigned char **out_data,
    size_t *out_len
)
{
    if (!out_data || !out_len) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null base64 decode output");
    }
    *out_data = NULL;
    *out_len = 0;
    if (!text) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null base64 text");
    }

    size_t clean_len = 0;
    for (const char *p = text; *p; p++) {
        int idx = base64_index(*p);
        if (idx == -1) {
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid base64 character");
        }
        if (idx != -3) clean_len++;
    }
    if (clean_len == 0) {
        *out_data = malloc(1);
        if (!*out_data) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate base64 decode output");
        }
        return cc_result_ok();
    }
    if (clean_len % 4 != 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid base64 length");
    }

    size_t max_len = (clean_len / 4) * 3;
    unsigned char *out = malloc(max_len ? max_len : 1);
    if (!out) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate base64 decode output");
    }

    int quad[4];
    int q = 0;
    size_t written = 0;
    int saw_pad = 0;
    for (const char *p = text; *p; p++) {
        int idx = base64_index(*p);
        if (idx == -3) continue;
        if (idx == -2) {
            saw_pad = 1;
            quad[q++] = -2;
        } else {
            if (saw_pad) {
                free(out);
                return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid base64 padding");
            }
            quad[q++] = idx;
        }
        if (q != 4) continue;

        if (quad[0] < 0 || quad[1] < 0) {
            free(out);
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid base64 quantum");
        }
        unsigned int triple =
            ((unsigned int)quad[0] << 18) |
            ((unsigned int)quad[1] << 12) |
            ((unsigned int)(quad[2] >= 0 ? quad[2] : 0) << 6) |
            (unsigned int)(quad[3] >= 0 ? quad[3] : 0);

        out[written++] = (unsigned char)((triple >> 16) & 0xff);
        if (quad[2] != -2) out[written++] = (unsigned char)((triple >> 8) & 0xff);
        if (quad[3] != -2) out[written++] = (unsigned char)(triple & 0xff);
        q = 0;
    }

    *out_data = out;
    *out_len = written;
    return cc_result_ok();
}
