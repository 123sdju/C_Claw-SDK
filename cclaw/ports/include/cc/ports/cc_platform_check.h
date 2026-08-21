



#ifndef CC_PLATFORM_CHECK_H
#define CC_PLATFORM_CHECK_H


/*
 * 基础平台能力依赖检查。
 *
 * 这些 #error 在编译期阻止无效 profile，例如 fork 没有 pipe、process pipe 没有基础
 * process run。对嵌入式项目来说，越早失败越容易定位移植缺口。
 */
#if CC_HAS_FORK && !CC_HAS_PIPES
#  error "CC_HAS_FORK requires CC_HAS_PIPES"
#endif

#if CC_HAS_PROCESS_PIPE && !CC_HAS_PROCESS_RUN
#  error "CC_HAS_PROCESS_PIPE requires CC_HAS_PROCESS_RUN"
#endif


/* 工具功能依赖检查：打开高层工具时，底层平台能力必须存在。 */
#if CC_TOOL_SHELL_RUN && !CC_HAS_PROCESS_RUN
#  error "CC_TOOL_SHELL_RUN requires CC_HAS_PROCESS_RUN"
#endif

#if CC_TOOL_PLUGIN && !CC_HAS_PROCESS_PIPE
#  error "CC_TOOL_PLUGIN requires CC_HAS_PROCESS_PIPE"
#endif


/* sandbox 依赖检查：本地沙箱需要进程执行能力。 */
#if CC_SANDBOX_LOCAL && !CC_HAS_PROCESS_RUN
#  error "CC_SANDBOX_LOCAL requires CC_HAS_PROCESS_RUN"
#endif


/*
 * 网络/HTTP 依赖检查。
 *
 * 云 provider 和 http.request 工具都依赖网络能力；非 ESP32/FreeRTOS 平台默认要求 curl
 * 或等价 HTTP client adapter，避免链接阶段才暴露缺失实现。
 */
#if (CC_LLM_OPENAI || CC_LLM_OLLAMA || CC_LLM_ANTHROPIC) && !CC_HAS_NETWORK
#  error "LLM providers require CC_HAS_NETWORK"
#endif

#if CC_TOOL_HTTP_REQUEST && !CC_HAS_NETWORK
#  error "CC_TOOL_HTTP_REQUEST requires CC_HAS_NETWORK"
#endif

#if (CC_LLM_OPENAI || CC_LLM_OLLAMA || CC_LLM_ANTHROPIC || CC_TOOL_HTTP_REQUEST) && !CC_HAS_CURL && CC_PLATFORM != CC_PLATFORM_ESP32 && CC_PLATFORM != CC_PLATFORM_FREERTOS
#  error "HTTP-backed adapters require CC_HAS_CURL or a replacement HTTP client adapter"
#endif


/* 存储依赖检查：至少启用一个后端，runtime/session/memory 才有可用落点。 */
#if !CC_STORAGE_SQLITE && !CC_STORAGE_JSON_FILE && !CC_STORAGE_MEMORY
#  error "At least one storage backend must be enabled"
#endif

#endif
