



#include "cc/util/cc_token_counter.h"
#include <limits.h>
#include <stddef.h>

/**
 * @brief 无分配扫描文本，按四个 ASCII 字节或一个非 ASCII 序列估计一个 token。
 * @param[in] text 可选稳定借用的 NUL 结尾文本，NULL 返回零。
 * @return 非负估计，超过 int 表示范围时饱和到 INT_MAX，空串为零。
 * @note ASCII 连续段分别向上取整，UTF-8 依据首字节跳过后续字节但不验证合法性。
 * 估计可与模型实际分词不同，调用方按 provider 用量解释精确计量。
 */
int cc_token_estimate(const char *text)
{
    if (!text) return 0;

    size_t tokens = 0;
    int ascii_run = 0;

    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;

        if (c < 0x80) {


            ascii_run++;
            if (ascii_run == 4) {
                tokens++;
                ascii_run = 0;
            }
        } else {





            if (ascii_run > 0) {
                tokens += (ascii_run + 3) / 4;
                ascii_run = 0;
            }



            int extra = 0;
            if      (c >= 0xF0) extra = 3;
            else if (c >= 0xE0) extra = 2;
            else if (c >= 0xC0) extra = 1;


            while (extra > 0 && p[1]) { p++; extra--; }



            tokens++;
        }
    }



    if (ascii_run > 0) {
        tokens += (ascii_run + 3) / 4;
    }

    return tokens > (size_t)INT_MAX ? INT_MAX : (int)tokens;
}


/**
 * @brief 将消息 JSON 正文按普通文本估算，保留独立调用入口。
 * @param[in] messages_text 可选稳定借用的 NUL 结尾正文，未执行 JSON 解析或角色加权。
 * @return cc_token_estimate 的饱和非负估计，NULL 和空串为零。
 */
int cc_token_estimate_json_messages(const char *messages_text)
{
    return cc_token_estimate(messages_text);
}
