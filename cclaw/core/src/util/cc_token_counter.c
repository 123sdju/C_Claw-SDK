



#include "cc/util/cc_token_counter.h"
#include <string.h>

/*
 * 粗略估算 token 数。
 *
 * ASCII 文本按约 4 字符 1 token 估算，非 ASCII UTF-8 codepoint 近似按 1 token 计。这个
 * 方法不追求模型级精度，而是提供低成本、无词表的上下文窗口保护，适合 MCU/RTOS 裁剪。
 */
int cc_token_estimate(const char *text)
{
    if (!text) return 0;

    int tokens = 0;
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

    return tokens;
}


/*
 * 估算 messages JSON token 数。
 *
 * 当前直接复用文本估算；保留单独入口是为了未来可以针对 message JSON 结构加入更准确
 * 的角色/content/tool_calls 权重，而不修改调用方。
 */
int cc_token_estimate_json_messages(const char *messages_text)
{
    return cc_token_estimate(messages_text);
}
