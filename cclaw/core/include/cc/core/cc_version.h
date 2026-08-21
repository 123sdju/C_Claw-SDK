#ifndef CC_VERSION_H
#define CC_VERSION_H

/* SDK 语义版本号，供编译期条件判断和日志输出使用。 */
#define CC_CLAW_VERSION_MAJOR 0
#define CC_CLAW_VERSION_MINOR 1
#define CC_CLAW_VERSION_PATCH 0

/* 人类可读版本字符串；与 cc_claw_version_string() 返回值一致。 */
#define CC_CLAW_VERSION_STRING "0.1.0"

/*
 * public API 废弃标记宏。
 *
 * 通过统一宏隐藏 GCC/Clang/MSVC 的属性差异。当前项目允许重塑 API，但稳定发布后
 * 可以用该宏给下游迁移窗口，而不是突然删除接口。
 */
#if defined(__GNUC__) || defined(__clang__)
#define CC_DEPRECATED(message) __attribute__((deprecated(message)))
#elif defined(_MSC_VER)
#define CC_DEPRECATED(message) __declspec(deprecated(message))
#else
#define CC_DEPRECATED(message)
#endif

/*
 * 返回运行库编译时版本字符串。
 *
 * 返回静态字符串，调用方不能释放；适合启动日志、诊断事件和 packaging consumer
 * 测试确认链接到的 C-Claw runtime。
 */
const char *cc_claw_version_string(void);

#endif
