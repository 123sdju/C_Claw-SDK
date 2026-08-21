



#ifndef CC_AGENT_RUNTIME_H
#define CC_AGENT_RUNTIME_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_llm_provider.h"
#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_policy_engine.h"
#include "cc/ports/cc_sandbox.h"
#include "cc/ports/cc_memory_store.h"
#include "cc/ports/cc_event_bus.h"
#include "cc/ports/cc_logger.h"
#include "cc/ports/cc_thread.h"
#include "cc/core/cc_stream_chunk.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/util/cc_config.h"
#include <stddef.h>

/*
 * runtime 统一资源限制。
 *
 * size 用于结构扩展；0 表示使用 SDK/provider 默认值。limits 在 runtime create 时复制，
 * 后续调用方修改原结构不会影响 runtime。超限应统一映射到 CC_ERR_LIMIT_EXCEEDED，
 * 工具内部超限通常转成可恢复 tool result。
 */
typedef struct cc_runtime_limits {
    size_t size;
    /* 为 0 时使用 SDK/provider 默认值；limits 在 runtime create 时复制，后续修改不影响 runtime。 */
    int run_timeout_ms;
    int tool_timeout_ms;
    int provider_timeout_ms;
    size_t max_input_bytes;
    size_t max_output_bytes;
    size_t max_tool_result_bytes;
    size_t max_stream_bytes;
    int max_steps;
    int max_concurrency;
} cc_runtime_limits_t;

/*
 * Agent runtime 配置。
 *
 * 字符串字段在 create 时深拷贝或转移到 runtime 内部；调用方不需要保持临时配置对象。
 * 配置覆盖模型参数、上下文窗口、压缩摘要、多模态和 active memory。结构不承诺当前字段
 * ABI 长期不变，size 字段用于后续兼容扩展。
 */
typedef struct cc_agent_runtime_config {
    size_t size;
    int max_steps;

    char *system_prompt;

    char *workspace_dir;

    char *model;
    char *model_extra_body_json;
    int max_tokens;
    double temperature;
    int context_window_tokens;

    double context_compress_threshold;

    int context_keep_recent;

    int summary_max_tokens;

    double summary_temperature;

    cc_multimodal_config_t multimodal;
    int active_memory_enabled;
    int active_memory_write_summary;
    int active_memory_max_value_chars;
    char *active_memory_category;
    cc_runtime_limits_t limits;
} cc_agent_runtime_config_t;

/* runtime 不透明句柄；内部持有 provider、store、registry、policy 等依赖。 */
typedef struct cc_agent_runtime cc_agent_runtime_t;

/*
 * runtime 依赖注入集合。
 *
 * llm/store/policy/sandbox 按值传入，内部 self/vtable 生命周期由 runtime 使用；registry、
 * event_bus、logger、memory_store、tool_pool 是外部对象指针，由调用方保证生命周期至少
 * 覆盖 runtime。approval 回调和 user_data 只在工具审批时借用。
 */
typedef struct cc_agent_runtime_deps {
    size_t size;

    cc_llm_provider_t llm;

    cc_tool_registry_t *tool_registry;

    cc_session_store_t store;

    cc_policy_engine_t policy;

    cc_sandbox_t sandbox;

    cc_event_bus_t *event_bus;

    cc_logger_t *logger;

    cc_memory_store_t *memory_store;

    cc_tool_executor_pool_t *tool_pool;

    cc_tool_approval_fn approve_tool_call;

    void *approval_user_data;
} cc_agent_runtime_deps_t;

/*
 * runtime 创建选项。
 *
 * config 会被 runtime 复制；thinking_mode 控制是否请求 provider 的 reasoning 输出。
 */
typedef struct cc_agent_runtime_options {
    size_t size;

    cc_agent_runtime_config_t config;

    int thinking_mode;
} cc_agent_runtime_options_t;

/*
 * 单次同步 run 选项。
 *
 * cancel_token 由调用方拥有；runtime 在 provider/tool/step 边界检查取消状态。
 */
typedef struct cc_agent_runtime_run_options {
    size_t size;
    cc_cancel_token_t *cancel_token;
} cc_agent_runtime_run_options_t;

/*
 * runtime 流式输出回调。
 *
 * chunk 由 SDK 拥有，只在回调期间有效；如果 UI 或应用线程要缓存内容必须自行复制。
 * event bus 只承担 observability，不替代该实时输出回调。
 */
typedef cc_result_t (*cc_agent_runtime_stream_callback_fn)(
    /* chunk 数据由 SDK 拥有，仅在回调期间有效；如需缓存必须自行复制。 */
    const cc_stream_chunk_t *chunk,
    void *user_data
);

/*
 * 单次流式 run 选项。
 *
 * cancel_token 用于取消流和工具执行；on_chunk 是实时输出入口；user_data 由调用方拥有。
 */
typedef struct cc_agent_runtime_stream_options {
    size_t size;
    /* 可选取消 token；在 provider/tool/stream chunk 之间检查取消状态。 */
    cc_cancel_token_t *cancel_token;
    /* 可选实时输出回调；event bus 仍承担 observability，该回调是实时输出主通道。 */
    cc_agent_runtime_stream_callback_fn on_chunk;
    void *user_data;
} cc_agent_runtime_stream_options_t;

/*
 * 创建 runtime。
 *
 * 函数会校验 provider capabilities、复制配置并绑定依赖。成功后 *out_runtime 由调用方
 * cc_agent_runtime_destroy()；失败时不会返回半初始化 runtime。
 */
cc_result_t cc_agent_runtime_create(
    const cc_agent_runtime_deps_t *deps,
    const cc_agent_runtime_options_t *options,
    cc_agent_runtime_t **out_runtime
);

/* 销毁 runtime；不销毁外部拥有的 registry/event_bus/logger/memory_store/tool_pool。 */
void cc_agent_runtime_destroy(cc_agent_runtime_t *runtime);

/* 动态开关 thinking mode；是否生效仍取决于 provider capability。 */
void cc_agent_runtime_set_thinking_mode(cc_agent_runtime_t *runtime, int enabled);

/* 读取当前 thinking mode，NULL runtime 返回 0。 */
int cc_agent_runtime_get_thinking_mode(cc_agent_runtime_t *runtime);

/* 更新工具审批回调；user_data 生命周期由调用方管理。 */
void cc_agent_runtime_set_tool_approval(
    cc_agent_runtime_t *runtime,
    cc_tool_approval_fn approve_tool_call,
    void *user_data
);

/* 返回 runtime 使用的 event bus；调用方不能销毁该指针，除非本来就是外部拥有者。 */
cc_event_bus_t *cc_agent_runtime_event_bus(cc_agent_runtime_t *runtime);

/* 返回 runtime 使用的 tool registry；registry 仍由其原所有者管理。 */
cc_tool_registry_t *cc_agent_runtime_tool_registry(cc_agent_runtime_t *runtime);

/* 返回 runtime 使用的工具执行池；指针生命周期不超过 runtime，调用方不能销毁。 */
cc_tool_executor_pool_t *cc_agent_runtime_tool_pool(cc_agent_runtime_t *runtime);

/* 返回 runtime 的 session store 句柄；不要直接销毁其 self，除非 runtime 已销毁。 */
cc_session_store_t *cc_agent_runtime_session_store(cc_agent_runtime_t *runtime);

/* 查询 provider 是否原生支持 streaming；同步响应适配为 chunk 不计作原生能力。 */
int cc_agent_runtime_supports_stream(cc_agent_runtime_t *runtime);

/* 创建 session 记录；workspace_dir 会作为文件工具安全根目录的一部分。 */
cc_result_t cc_agent_runtime_create_session(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *workspace_dir
);

/* 处理一条用户输入并返回完整 assistant 文本；out_response 由调用方 free()。 */
cc_result_t cc_agent_runtime_handle_message(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    char **out_response
);

/* 带取消选项的同步消息处理。 */
cc_result_t cc_agent_runtime_handle_message_with_options(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    const cc_agent_runtime_run_options_t *options,
    char **out_response
);

/*
 * 正式流式回调入口。
 *
 * runtime 会把 text/thinking/tool delta/tool start/tool end/finished/error chunk 推给
 * on_chunk，同时通过 observability 发布 stream.* 事件。默认只落库完整 final response，
 * 取消、错误或超限的 partial chunk 不应作为完整 assistant final 落库。
 * 对只实现 chat() 的 provider，完整响应会先转换成同一 chunk 协议，因此该入口仍可用；
 * 这种情况下 chunk 是 provider 返回后的缓冲适配，不具备网络级实时性。
 */
cc_result_t cc_agent_runtime_handle_message_stream_cb(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    const cc_agent_runtime_stream_options_t *options,
    char **out_response
);

#endif
