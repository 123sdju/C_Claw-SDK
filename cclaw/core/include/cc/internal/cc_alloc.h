#ifndef CC_INTERNAL_ALLOC_H
#define CC_INTERNAL_ALLOC_H

#include <stdlib.h>
#include <string.h>

/* 深拷贝 NUL 结尾字符串；返回堆上副本，调用方用 free() 释放；src 为 NULL 时返回 NULL。 */
static inline char *cc_copy_string(const char *src)
{
    if (!src) return NULL;
    size_t len = strlen(src) + 1;
    char *dst = (char *)malloc(len);
    if (dst) memcpy(dst, src, len);
    return dst;
}

#endif
