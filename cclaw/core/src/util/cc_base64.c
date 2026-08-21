#include "cc/util/cc_base64.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static const char k_base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*
 * 将 Base64 字符转换为 6-bit 索引值。
 * 参数: ch - 待转换的 Base64 字符
 * 返回: 0-63 有效字符索引，-2 填充符 '='，-3 空白字符（空格/换行/回车/制表），-1 非法字符
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

/*
 * 将二进制数据编码为 Base64 字符串。
 *
 * 参数:
 *   data     - 待编码的原始数据
 *   data_len - 数据长度（字节）
 *   out_text - 输出参数，接收动态分配的 Base64 字符串（调用方通过 free() 释放）
 * 返回: CC_OK 成功，CC_ERR_INVALID_ARGUMENT 参数无效，CC_ERR_OUT_OF_MEMORY 分配失败。
 *
 * 编码结果自动添加 padding（=）。每 3 个输入字节产生 4 个输出字符。
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

/*
 * 将 Base64 字符串解码为二进制数据。
 *
 * 参数:
 *   text     - Base64 编码的输入字符串
 *   out_data - 输出参数，接收动态分配的解码数据（调用方通过 free() 释放）
 *   out_len  - 输出参数，接收解码后数据长度
 * 返回: CC_OK 成功，CC_ERR_INVALID_ARGUMENT 参数无效或格式错误，CC_ERR_OUT_OF_MEMORY。
 *
 * 输入中的空白字符会被自动跳过，支持标准 Base64 和 URL-safe 变体。
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
