



#ifndef CC_PLATFORM_H
#define CC_PLATFORM_H

#include <stdint.h>
#include <stddef.h>

/*
 * 编译期平台枚举。
 *
 * C-Claw 用宏而不是运行时判断来裁剪能力，便于 MCU/RTOS 构建把不可用模块直接排除。
 */
#define CC_PLATFORM_POSIX   100
#define CC_PLATFORM_WINDOWS 200
#define CC_PLATFORM_ESP32   300
#define CC_PLATFORM_FREERTOS 400

/*
 * 自动识别当前平台。
 *
 * 应用或 CMake profile 可以预先定义 CC_PLATFORM 覆盖自动识别；这对交叉编译很重要，
 * 因为构建机宏不一定等于目标机能力。
 */
#ifndef CC_PLATFORM
#  if defined(__XTENSA__) || defined(ESP_PLATFORM)
#    define CC_PLATFORM CC_PLATFORM_ESP32
#  elif defined(CC_FREERTOS_PLATFORM)
#    define CC_PLATFORM CC_PLATFORM_FREERTOS
#  elif defined(_WIN32)
#    define CC_PLATFORM CC_PLATFORM_WINDOWS
#  else
#    define CC_PLATFORM CC_PLATFORM_POSIX
#  endif
#endif

/* 初始化平台特定资源（内存池、日志、时钟等）；应在 main() 早期调用一次。 */
void cc_platform_init(void);
/* 返回单调递增的毫秒时间戳；用于 timeout、背压和观测计时，不受系统时间调整影响。 */
uint64_t cc_platform_monotonic_ms(void);
/* 使用平台 CSPRNG 填充缓冲区；成功返回 0，平台无安全熌源时返回 -1。 */
int cc_platform_random_bytes(void *buffer, size_t size);

/*
 * POSIX 默认能力矩阵。
 *
 * Linux/macOS 类平台默认具备线程、进程、管道、realpath、dirent、curl 和网络能力，
 * 因此适合完整 desktop-agent profile。各 CC_HAS_* 宏仍允许在 CMake profile 中收紧。
 */
#if CC_PLATFORM == CC_PLATFORM_POSIX
#  ifndef CC_HAS_FORK
#    define CC_HAS_FORK 1
#  endif
#  ifndef CC_HAS_THREADS
#    define CC_HAS_THREADS 1
#  endif
#  ifndef CC_HAS_PIPES
#    define CC_HAS_PIPES 1
#  endif
#  ifndef CC_HAS_PROCESS_RUN
#    define CC_HAS_PROCESS_RUN 1
#  endif
#  ifndef CC_HAS_PROCESS_PIPE
#    define CC_HAS_PROCESS_PIPE 1
#  endif
#  ifndef CC_HAS_SIGNALS
#    define CC_HAS_SIGNALS 1
#  endif
#  ifndef CC_HAS_REALPATH
#    define CC_HAS_REALPATH 1
#  endif
#  ifndef CC_HAS_DIRENT
#    define CC_HAS_DIRENT 1
#  endif
#  ifndef CC_HAS_MMAP
#    define CC_HAS_MMAP 1
#  endif
#  ifndef CC_HAS_FSYNC
#    define CC_HAS_FSYNC 1
#  endif
#  ifndef CC_HAS_CURL
#    define CC_HAS_CURL 1
#  endif
#  ifndef CC_HAS_NETWORK
#    define CC_HAS_NETWORK 1
#  endif
#  ifndef CC_HAS_SSL_TLS
#    define CC_HAS_SSL_TLS 1
#  endif
#  ifndef CC_HAS_DLOPEN
#    define CC_HAS_DLOPEN 1
#  endif
#  ifndef CC_HAS_CLOCK_MONOTONIC
#    define CC_HAS_CLOCK_MONOTONIC 1
#  endif
#  define CC_LIMIT_MAX_PATH 4096
#  define CC_LIMIT_HEAP_WARN_KB 0
#  define CC_LIMIT_STACK_SIZE_KB 1024
#  define CC_LIMIT_HTTP_RESP_MAX 0
#  define CC_LIMIT_SESSION_HISTORY 100
/*
 * Windows 默认能力矩阵。
 *
 * Windows 有线程、进程、管道和网络，但 fork/dirent/realpath 语义不同，所以默认关闭
 * 对应 POSIX 能力，由 Windows port 提供替代实现。
 */
#elif CC_PLATFORM == CC_PLATFORM_WINDOWS
#  ifndef CC_HAS_FORK
#    define CC_HAS_FORK 0
#  endif
#  ifndef CC_HAS_THREADS
#    define CC_HAS_THREADS 1
#  endif
#  ifndef CC_HAS_PIPES
#    define CC_HAS_PIPES 1
#  endif
#  ifndef CC_HAS_PROCESS_RUN
#    define CC_HAS_PROCESS_RUN 1
#  endif
#  ifndef CC_HAS_PROCESS_PIPE
#    define CC_HAS_PROCESS_PIPE 1
#  endif
#  ifndef CC_HAS_SIGNALS
#    define CC_HAS_SIGNALS 0
#  endif
#  ifndef CC_HAS_REALPATH
#    define CC_HAS_REALPATH 0
#  endif
#  ifndef CC_HAS_DIRENT
#    define CC_HAS_DIRENT 0
#  endif
#  ifndef CC_HAS_MMAP
#    define CC_HAS_MMAP 0
#  endif
#  ifndef CC_HAS_FSYNC
#    define CC_HAS_FSYNC 1
#  endif
#  ifndef CC_HAS_CURL
#    define CC_HAS_CURL 1
#  endif
#  ifndef CC_HAS_NETWORK
#    define CC_HAS_NETWORK 1
#  endif
#  ifndef CC_HAS_SSL_TLS
#    define CC_HAS_SSL_TLS 1
#  endif
#  ifndef CC_HAS_DLOPEN
#    define CC_HAS_DLOPEN 1
#  endif
#  ifndef CC_HAS_CLOCK_MONOTONIC
#    define CC_HAS_CLOCK_MONOTONIC 1
#  endif
#  define CC_LIMIT_MAX_PATH 260
#  define CC_LIMIT_HEAP_WARN_KB 0
#  define CC_LIMIT_STACK_SIZE_KB 1024
#  define CC_LIMIT_HTTP_RESP_MAX 0
#  define CC_LIMIT_SESSION_HISTORY 100
/*
 * ESP32 默认能力矩阵。
 *
 * ESP32 具备 FreeRTOS 线程和网络，但没有 fork/pipe/dlopen；默认限制路径、堆、栈和
 * HTTP 响应大小，体现嵌入式设备资源预算。
 */
#elif CC_PLATFORM == CC_PLATFORM_ESP32
#  ifndef CC_HAS_FORK
#    define CC_HAS_FORK 0
#  endif
#  ifndef CC_HAS_THREADS
#    define CC_HAS_THREADS 1
#  endif
#  ifndef CC_HAS_PIPES
#    define CC_HAS_PIPES 0
#  endif
#  ifndef CC_HAS_PROCESS_RUN
#    define CC_HAS_PROCESS_RUN 0
#  endif
#  ifndef CC_HAS_PROCESS_PIPE
#    define CC_HAS_PROCESS_PIPE 0
#  endif
#  ifndef CC_HAS_SIGNALS
#    define CC_HAS_SIGNALS 0
#  endif
#  ifndef CC_HAS_REALPATH
#    define CC_HAS_REALPATH 1
#  endif
#  ifndef CC_HAS_DIRENT
#    define CC_HAS_DIRENT 1
#  endif
#  ifndef CC_HAS_MMAP
#    define CC_HAS_MMAP 0
#  endif
#  ifndef CC_HAS_FSYNC
#    define CC_HAS_FSYNC 1
#  endif
#  ifndef CC_HAS_CURL
#    define CC_HAS_CURL 0
#  endif
#  ifndef CC_HAS_NETWORK
#    define CC_HAS_NETWORK 1
#  endif
#  ifndef CC_HAS_SSL_TLS
#    define CC_HAS_SSL_TLS 1
#  endif
#  ifndef CC_HAS_DLOPEN
#    define CC_HAS_DLOPEN 0
#  endif
#  ifndef CC_HAS_CLOCK_MONOTONIC
#    define CC_HAS_CLOCK_MONOTONIC 1
#  endif
#  define CC_LIMIT_MAX_PATH 256
#  define CC_LIMIT_HEAP_WARN_KB 400
#  define CC_LIMIT_STACK_SIZE_KB 16
#  define CC_LIMIT_HTTP_RESP_MAX 32768
#  define CC_LIMIT_SESSION_HISTORY 10
/*
 * 通用 FreeRTOS 默认能力矩阵。
 *
 * 默认假设没有网络、文件系统目录遍历、进程和动态加载；下游 BSP 可以按实际能力打开
 * 对应宏并提供 port 实现。
 */
#elif CC_PLATFORM == CC_PLATFORM_FREERTOS
#  ifndef CC_HAS_FORK
#    define CC_HAS_FORK 0
#  endif
#  ifndef CC_HAS_THREADS
#    define CC_HAS_THREADS 1
#  endif
#  ifndef CC_HAS_PIPES
#    define CC_HAS_PIPES 0
#  endif
#  ifndef CC_HAS_PROCESS_RUN
#    define CC_HAS_PROCESS_RUN 0
#  endif
#  ifndef CC_HAS_PROCESS_PIPE
#    define CC_HAS_PROCESS_PIPE 0
#  endif
#  ifndef CC_HAS_SIGNALS
#    define CC_HAS_SIGNALS 0
#  endif
#  ifndef CC_HAS_REALPATH
#    define CC_HAS_REALPATH 0
#  endif
#  ifndef CC_HAS_DIRENT
#    define CC_HAS_DIRENT 0
#  endif
#  ifndef CC_HAS_MMAP
#    define CC_HAS_MMAP 0
#  endif
#  ifndef CC_HAS_FSYNC
#    define CC_HAS_FSYNC 0
#  endif
#  ifndef CC_HAS_CURL
#    define CC_HAS_CURL 0
#  endif
#  ifndef CC_HAS_NETWORK
#    define CC_HAS_NETWORK 0
#  endif
#  ifndef CC_HAS_SSL_TLS
#    define CC_HAS_SSL_TLS 0
#  endif
#  ifndef CC_HAS_DLOPEN
#    define CC_HAS_DLOPEN 0
#  endif
#  ifndef CC_HAS_CLOCK_MONOTONIC
#    define CC_HAS_CLOCK_MONOTONIC 0
#  endif
#  define CC_LIMIT_MAX_PATH 256
#  define CC_LIMIT_HEAP_WARN_KB 256
#  define CC_LIMIT_STACK_SIZE_KB 16
#  define CC_LIMIT_HTTP_RESP_MAX 0
#  define CC_LIMIT_SESSION_HISTORY 8
#else
#  error "Unknown CC_PLATFORM value"
#endif

/*
 * SDK 功能裁剪开关。
 *
 * 默认保持核心 SDK 定位：shell/http/plugin/外部 provider 默认关闭，文件读写和基础存储
 * 默认开启。gateway CLI/UART 明确为 0，表示不把产品入口放进核心 SDK。
 */
#ifndef CC_TOOL_SHELL_RUN
#  define CC_TOOL_SHELL_RUN 0
#endif
#ifndef CC_SANDBOX_LOCAL
#  define CC_SANDBOX_LOCAL 0
#endif
#ifndef CC_SANDBOX_DOCKER
#  define CC_SANDBOX_DOCKER 0
#endif
#ifndef CC_TOOL_HTTP_REQUEST
#  define CC_TOOL_HTTP_REQUEST 0
#endif
#ifndef CC_TOOL_FILE_READ
#  define CC_TOOL_FILE_READ 1
#endif
#ifndef CC_TOOL_FILE_WRITE
#  define CC_TOOL_FILE_WRITE 1
#endif
#ifndef CC_TOOL_PLUGIN
#  define CC_TOOL_PLUGIN 0
#endif
#ifndef CC_STORAGE_SQLITE
#  define CC_STORAGE_SQLITE 0
#endif
#ifndef CC_STORAGE_JSON_FILE
#  define CC_STORAGE_JSON_FILE 1
#endif
#ifndef CC_STORAGE_MEMORY
#  define CC_STORAGE_MEMORY 1
#endif
#ifndef CC_LLM_OPENAI
#  define CC_LLM_OPENAI 0
#endif
#ifndef CC_LLM_OLLAMA
#  define CC_LLM_OLLAMA 0
#endif
#ifndef CC_LLM_ANTHROPIC
#  define CC_LLM_ANTHROPIC 0
#endif
#ifndef CC_HAS_MEMORY
#  define CC_HAS_MEMORY 1
#endif
#ifndef CC_GATEWAY_CLI
#  define CC_GATEWAY_CLI 0
#endif
#ifndef CC_GATEWAY_UART
#  define CC_GATEWAY_UART 0
#endif

/* 编译期一致性检查集中放在单独头文件，避免无效 profile 静默通过。 */
#include "cc/ports/cc_platform_check.h"

#endif
