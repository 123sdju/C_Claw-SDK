#ifndef CC_BASE64_H
#define CC_BASE64_H

#include "cc/core/cc_result.h"
#include <stddef.h>

/* 将二进制数据编码为 Base64 字符串；out_text 成功后由调用方 free()。 */
cc_result_t cc_base64_encode(
    const unsigned char *data,
    size_t data_len,
    char **out_text
);

/* 将 Base64 字符串解码为二进制数据；out_data 成功后由调用方 free()，out_len 保存实际字节数。 */
cc_result_t cc_base64_decode(
    const char *text,
    unsigned char **out_data,
    size_t *out_len
);

#endif
