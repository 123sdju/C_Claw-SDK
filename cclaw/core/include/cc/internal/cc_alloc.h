#ifndef CC_INTERNAL_ALLOC_H
#define CC_INTERNAL_ALLOC_H

#include <stdlib.h>
#include <string.h>

/**
 * @brief 深拷贝借用的 C 字符串及终止符。
 * @param[in] src 调用期间稳定的 NUL 结尾字符串，可为 NULL。
 * @return 成功返回独占堆副本，源为空或分配失败返回 NULL，调用者最终 free。
 * @note 源须覆盖首个 NUL 的完整范围，函数不保留借用，无共享可变状态。
 */
static inline char *cc_copy_string(const char *src)
{
    if (!src) return NULL;
    size_t len = strlen(src) + 1;
    char *dst = (char *)malloc(len);
    if (dst) memcpy(dst, src, len);
    return dst;
}

#endif
