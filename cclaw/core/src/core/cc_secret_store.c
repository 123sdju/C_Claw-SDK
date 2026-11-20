#include "cc/ports/cc_secret_store.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief 通过 volatile 字节写清除指定的独占缓冲区。
 * @param[out] data 借用的可写区域，NULL 时直接跳过。
 * @param[in] size 清除字节数，非空 data 须具有足够容量。
 * @post 有效区域的 size 个字节已置零，函数不释放缓冲。
 * @note 调用者排除该区域的并发读取和修改，本函数只访问指定区域。
 */
void cc_secret_zero(void *data, size_t size)
{
    volatile unsigned char *bytes = (volatile unsigned char *)data;
    while (bytes && size > 0) {
        *bytes++ = 0;
        size--;
    }
}

/**
 * @brief 消费一个拥有的 NUL 结尾字符串，清除正文后释放。
 * @param[in,out] secret 来自兼容 malloc 分配的独占字符串，可为 NULL。
 * @post 正文至首个 NUL 前的字节已清零，分配已释放，原指针借用失效。
 * @note 调用者保证字符串有效且终止，并排空全部使用者；参数按值传递，返回后旧地址失效。
 */
void cc_secret_free(char *secret)
{
    if (!secret) return;
    cc_secret_zero(secret, strlen(secret));
    free(secret);
}
