

#ifndef CC_AGENT_RUNTIME_INTERNAL_H
#define CC_AGENT_RUNTIME_INTERNAL_H

#include "cc/app/cc_agent_runtime.h"
#include <stddef.h>
#include <stdint.h>

#define CC_CONTEXT_SUMMARY_CACHE_SLOTS 4

typedef struct cc_context_summary_cache_entry {
    char *session_id;
    char *summary;
    uint64_t history_fingerprint;
    size_t summarized_count;
    uint64_t last_used;
} cc_context_summary_cache_entry_t;

/*
 * runtime 内部结构。
 *
 * public header 只暴露不透明句柄；内部结构集中保存配置、provider、registry、store、
 * policy、sandbox、observability、memory 和工具执行池。mutex 保护可变 runtime 状态。
 */
struct cc_agent_runtime {
    cc_agent_runtime_config_t config;
    cc_llm_provider_t llm;
    cc_tool_registry_t *tool_registry;
    cc_session_store_t store;
    cc_policy_engine_t policy;
    cc_sandbox_t sandbox;
    cc_event_bus_t *event_bus;
    cc_logger_t *logger;
    cc_memory_store_t *memory_store;
    cc_tool_executor_pool_t *tool_pool;
    cc_runtime_services_t services;
    int thinking_mode;
    int active_runs;
    cc_context_summary_cache_entry_t summary_cache[CC_CONTEXT_SUMMARY_CACHE_SLOTS];
    uint64_t summary_cache_clock;
    cc_mutex_t mutex;
};


/*
 * 执行一次 tool call step 并把结果写回 session。
 *
 * 这是 agent loop 内部 helper，不属于 public API；它复用 tool executor 的 schema/policy/
 * approval/observability 语义，并负责构造 tool role message。
 */
cc_result_t cc_agent_runtime_execute_tool_step(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_call_t *call,
    const char *reasoning_content,
    cc_cancel_token_t *cancel_token,
    uint64_t deadline_ms
);


/*
 * 将 assistant 文本落库。
 *
 * 只存完整 final response；stream partial、取消或错误路径不应通过该 helper 落成完整
 * assistant 消息。
 */
cc_result_t cc_agent_runtime_store_assistant_text(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *text,
    const char *reasoning_content
);

#endif
