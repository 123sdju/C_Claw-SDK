



#ifndef CC_CONFIG_H
#define CC_CONFIG_H

#include "cc/core/cc_result.h"
#include <stddef.h>

/*
 * 配置中的字符串列表。
 *
 * items 数组和其中字符串由所属 config 结构拥有，统一通过 cc_config_destroy() 释放。
 */
typedef struct cc_config_string_list {
    char **items;
    size_t count;
} cc_config_string_list_t;

/*
 * 单个 agent profile。
 *
 * profile 允许不同 agent 使用不同 model/workspace/system prompt/skills；字符串字段由
 * config 拥有，runtime builder 读取后复制需要长期保存的值。
 */
typedef struct cc_config_agent_profile {
    char *id;
    char *model;
    char *workspace;
    char *agent_dir;
    char *system_prompt_file;
    cc_config_string_list_t skills;
} cc_config_agent_profile_t;

/* agents 配置：defaults 提供默认值，profiles 保存命名 agent 覆盖项。 */
typedef struct cc_config_agents {
    cc_config_agent_profile_t defaults;
    cc_config_agent_profile_t *profiles;
    size_t profile_count;
} cc_config_agents_t;

/* run queue 配置；用于控制 lane 并发、同 session 并发和 pending 上限。 */
typedef struct cc_config_queue {
    int main_concurrency;
    int subagent_concurrency;
    int plugin_concurrency;
    int mcp_concurrency;
    int per_session_concurrency;
    char *mode;
    int debounce_ms;
    int max_pending_per_session;
    size_t max_total_tasks;
    unsigned completed_ttl_ms;
} cc_config_queue_t;

/* 单个工具 lane 策略；按工具名配置并发和 timeout。 */
typedef struct cc_config_tool_policy {
    char *name;
    int concurrency;
    int timeout_ms;
} cc_config_tool_policy_t;

/* 工具配置：启用列表、网络 allowlist、默认 timeout 和按工具覆盖策略。 */
typedef struct cc_config_tools {
    cc_config_string_list_t enabled;
    cc_config_string_list_t network_allowlist;
    int default_timeout_ms;
    cc_config_tool_policy_t *policies;
    size_t policy_count;
} cc_config_tools_t;

/* plugin 对外声明的工具 schema；parameters_json 是 JSON Schema 参数对象文本。 */
typedef struct cc_config_plugin_tool {
    char *name;
    char *description;
    char *parameters_json;
} cc_config_plugin_tool_t;

/*
 * plugin 配置项。
 *
 * command/args 描述外部进程；workers/max_in_flight 控制并发；tools 描述可注册工具；
 * skill_dirs 允许 plugin 附带 skill 文档。SDK 只提供协议和加载边界。
 */
typedef struct cc_config_plugin_entry {
    char *id;
    int enabled;
    char *command;
    char **args;
    size_t arg_count;
    int workers;
    int timeout_ms;
    int max_in_flight;
    int restart_on_crash;
    cc_config_plugin_tool_t *tools;
    size_t tool_count;
    cc_config_string_list_t skill_dirs;
} cc_config_plugin_entry_t;

/* plugin 总配置；hot_reload/reload_debounce_ms 由 runtime builder/reload 解释。 */
typedef struct cc_config_plugins {
    int hot_reload;
    int reload_debounce_ms;
    cc_config_plugin_entry_t *entries;
    size_t entry_count;
} cc_config_plugins_t;

/* skill 加载配置；extra_dirs 由 skill catalog 读取，watch 交给上层热重载逻辑。 */
typedef struct cc_config_skills {
    int watch;
    int watch_debounce_ms;
    cc_config_string_list_t extra_dirs;
} cc_config_skills_t;

/*
 * MCP server 配置。
 *
 * 支持 command/args/cwd 的本地进程 transport，也预留 url/transport 给未来 HTTP/SSE 等
 * 传输。字符串由 config 拥有。
 */
typedef struct cc_config_mcp_server {
    char *name;
    char *command;
    char **args;
    size_t arg_count;
    char *cwd;
    char *url;
    char *transport;
    int connection_timeout_ms;
} cc_config_mcp_server_t;

/* MCP 总配置；enabled 控制是否加载，servers 保存多个 MCP server 描述。 */
typedef struct cc_config_mcp {
    int enabled;
    int session_idle_ttl_ms;
    cc_config_mcp_server_t *servers;
    size_t server_count;
} cc_config_mcp_t;

/* 多模态输入/输出开关集合。 */
typedef struct cc_config_modality_flags {
    int image;
    int audio;
    int video;
    int file;
} cc_config_modality_flags_t;

/* 多模态资源限制配置；最终会转换为 cc_media_limits_t。 */
typedef struct cc_config_multimodal_limits {
    size_t max_artifacts;
    size_t max_artifact_bytes;
    size_t max_base64_bytes;
    int allow_inline_base64;
    cc_config_string_list_t allowed_mime_prefixes;
} cc_config_multimodal_limits_t;

/* 多模态配置：分别描述输入/输出能力和资源限制。 */
typedef struct cc_multimodal_config {
    cc_config_modality_flags_t input;
    cc_config_modality_flags_t output;
    cc_config_multimodal_limits_t limits;
} cc_multimodal_config_t;

/*
 * SDK 主配置结构。
 *
 * 该结构由 cc_config_load* 填充，所有字符串、数组和嵌套对象由 config 拥有，并通过
 * cc_config_destroy() 统一释放。runtime builder 读取它来创建 provider、storage、
 * tools、queue、plugin、MCP、multimodal 和 active memory。产品 I/O、网络、Web、
 * 显示、OTA 等配置属于应用层，不进入 SDK runtime config。
 */
typedef struct cc_config {
    char *provider;

    char *model;

    char *model_extra_body_json;

    char *base_url;

    char *api_key;

    char *api_key_ref;

    int tls_verify;

    int http_connect_timeout_ms;

    int http_first_byte_timeout_ms;

    int http_stream_idle_timeout_ms;

    int http_retry_count;

    int http_log_level;

    int http_trace_persist;

    char *storage_type;

    char *data_dir;

    char *storage_path;

    size_t session_segment_bytes;

    char *session_media_dir;

    char *workspace_path;

    char *sandbox_type;

    int shell_requires_approval;

    int sandbox_timeout_ms;

    int max_steps;

    /* Runtime-wide resource limits. Zero selects the platform default. */
    int run_timeout_ms;
    int provider_timeout_ms;
    int tool_timeout_ms;
    int max_input_bytes;
    int max_output_bytes;
    int max_stream_bytes;
    int max_tool_result_bytes;
    int max_concurrency;

    int thinking_mode;

    int max_tokens;

    double temperature;

    int stream_mode;

    int debug_mode;

    char *memory_backend;

    char *memory_path;

    int active_memory_enabled;

    int active_memory_write_summary;
    int active_memory_max_value_chars;
    char *active_memory_category;
    char *system_prompt;

    char *soul_file;

    char *user_file;

    char **enabled_tools;

    size_t enabled_tools_count;
    char **plugin_commands;

    size_t plugin_count;
    int context_window_tokens;

    double context_compress_threshold;

    int context_keep_recent;

    int summary_max_tokens;

    double summary_temperature;



    cc_config_agents_t agents;
    cc_config_queue_t queue;
    cc_config_tools_t tools;
    cc_config_plugins_t plugins;
    cc_config_skills_t skills;
    cc_config_mcp_t mcp;
    cc_multimodal_config_t multimodal;
} cc_config_t;

/* 从指定路径加载配置；out_config 成功后由调用方 cc_config_destroy()。 */
cc_result_t cc_config_load(const char *path, cc_config_t *out_config);

/* 按默认路径/环境加载配置；适合简单应用启动。 */
cc_result_t cc_config_load_default(cc_config_t *out_config);

/* 校验配置基本一致性和必填项；不创建 runtime 资源。 */
cc_result_t cc_config_validate(const cc_config_t *config);

/* 释放 config 拥有的所有字符串、数组和嵌套配置。 */
void cc_config_destroy(cc_config_t *config);

/* 根据 system_prompt/soul/user 等配置构建最终 system prompt；返回字符串由调用方 free()。 */
cc_result_t cc_config_build_system_prompt(const cc_config_t *config, char **out_prompt);

#endif
