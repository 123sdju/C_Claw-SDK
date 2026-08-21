



#ifndef CC_TYPES_H
#define CC_TYPES_H

/*
 * 核心公共基础类型占位头。
 *
 * 当前只暴露 size_t，保留这个头是为了给未来稳定 API 放置跨模块共享的轻量类型
 * 或 ABI 辅助宏。不要在这里引入平台 adapter 私有类型，否则会扩大 public API 面。
 */
#include <stddef.h>

#endif
