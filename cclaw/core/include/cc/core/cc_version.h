#ifndef CC_VERSION_H
#define CC_VERSION_H

/* SDK 语义版本号，供编译期条件判断和日志输出使用。 */
#define CC_CLAW_VERSION_MAJOR 0
#define CC_CLAW_VERSION_MINOR 1
#define CC_CLAW_VERSION_PATCH 0

/* 人类可读版本字符串；与 cc_claw_version_string() 返回值一致。 */
#define CC_CLAW_VERSION_STRING "0.1.0"

/*
 * 公共 API 的废弃标记宏。
 *
 * 通过统一宏隐藏 GCC/Clang/MSVC 的属性差异。当前项目允许重塑 API，但稳定发布后
 * 可以用该宏声明接口废弃并为下游提供迁移窗口。
 */
#if defined(__GNUC__) || defined(__clang__)
#define CC_DEPRECATED(message) __attribute__((deprecated(message)))
#elif defined(_MSC_VER)
#define CC_DEPRECATED(message) __declspec(deprecated(message))
#else
#define CC_DEPRECATED(message)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 查询已链接 SDK 运行库编译时的版本。
 * @return 返回运行库 CC_CLAW_VERSION_STRING 的只读静态文本，进程期有效。
 * @note 可用于启动日志及消费方链接诊断，文本由运行库编译版本决定。
 */
const char *cc_claw_version_string(void);

#ifdef __cplusplus
}
#endif

#endif
