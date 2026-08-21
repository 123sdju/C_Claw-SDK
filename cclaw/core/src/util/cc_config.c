



#include "cc/util/cc_config.h"
#include "cc/ports/cc_secret_store.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_platform.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#if CC_LLM_OPENAI
#define CC_DEFAULT_PROVIDER "openai"
#define CC_DEFAULT_MODEL "gpt-4o-mini"
#define CC_DEFAULT_BASE_URL "https://api.openai.com"
#elif CC_LLM_ANTHROPIC
#define CC_DEFAULT_PROVIDER "anthropic"
#define CC_DEFAULT_MODEL "claude-3-5-haiku-latest"
#define CC_DEFAULT_BASE_URL "https://api.anthropic.com"
#elif CC_LLM_OLLAMA
#define CC_DEFAULT_PROVIDER "ollama"
#define CC_DEFAULT_MODEL "qwen2.5-coder:7b"
#define CC_DEFAULT_BASE_URL "http://localhost:11434"
#else
#define CC_DEFAULT_PROVIDER "none"
#define CC_DEFAULT_MODEL "none"
#define CC_DEFAULT_BASE_URL ""
#endif

#if CC_STORAGE_SQLITE
#define CC_DEFAULT_STORAGE_TYPE "sqlite"
#elif CC_STORAGE_JSON_FILE
#define CC_DEFAULT_STORAGE_TYPE "json_segmented"
#else
#define CC_DEFAULT_STORAGE_TYPE "memory"
#endif

#ifndef CC_DEFAULT_DATA_DIR
#define CC_DEFAULT_DATA_DIR "runtime/data"
#endif

#ifndef CC_DEFAULT_WORKSPACE_PATH
#define CC_DEFAULT_WORKSPACE_PATH "runtime/workspace"
#endif

#ifndef CC_DEFAULT_STORAGE_PATH
#if CC_STORAGE_SQLITE
#define CC_DEFAULT_STORAGE_PATH "runtime/data/c-claw.db"
#else
#define CC_DEFAULT_STORAGE_PATH "runtime/data/sessions"
#endif
#endif

#ifndef CC_DEFAULT_SESSION_SEGMENT_BYTES
#define CC_DEFAULT_SESSION_SEGMENT_BYTES (256u * 1024u)
#endif

#ifndef CC_DEFAULT_SESSION_MEDIA_DIR
#define CC_DEFAULT_SESSION_MEDIA_DIR "media"
#endif

#ifndef CC_DEFAULT_MEMORY_PATH
#define CC_DEFAULT_MEMORY_PATH "runtime/data/memory.json"
#endif

#ifndef CC_DEFAULT_SOUL_FILE
#define CC_DEFAULT_SOUL_FILE "soul.md"
#endif

#ifndef CC_DEFAULT_USER_FILE
#define CC_DEFAULT_USER_FILE "user.md"
#endif

#if CC_PLATFORM == CC_PLATFORM_ESP32 || CC_PLATFORM == CC_PLATFORM_FREERTOS
#define CC_DEFAULT_MAX_STEPS 4
#else
#define CC_DEFAULT_MAX_STEPS 10
#endif

#if CC_HAS_MEMORY
#define CC_DEFAULT_MEMORY_BACKEND "json_file"
#else
#define CC_DEFAULT_MEMORY_BACKEND "noop"
#endif

#ifndef CC_ENABLE_MULTIMODAL
#define CC_ENABLE_MULTIMODAL 0
#endif
#ifndef CC_ENABLE_MEDIA_IMAGE
#define CC_ENABLE_MEDIA_IMAGE 0
#endif
#ifndef CC_ENABLE_MEDIA_AUDIO
#define CC_ENABLE_MEDIA_AUDIO 0
#endif
#ifndef CC_ENABLE_MEDIA_VIDEO
#define CC_ENABLE_MEDIA_VIDEO 0
#endif
#ifndef CC_ENABLE_MEDIA_FILE
#define CC_ENABLE_MEDIA_FILE 0
#endif
#ifndef CC_ENABLE_MEDIA_OUTPUT
#define CC_ENABLE_MEDIA_OUTPUT 0
#endif
#ifndef CC_ENABLE_INLINE_BASE64
#define CC_ENABLE_INLINE_BASE64 0
#endif

#define CC_CONFIG_LOG_TRACE 0
#define CC_CONFIG_LOG_DEBUG 1
#define CC_CONFIG_LOG_INFO 2
#define CC_CONFIG_LOG_WARN 3
#define CC_CONFIG_LOG_ERROR 4
#define CC_CONFIG_LOG_FATAL 5


/*
 * 读取整个文本文件。
 *
 * 该 helper 用于 system prompt 文件拼接，返回缓冲由调用方 free()。空文件或读取失败都
 * 返回 NULL，调用方会继续尝试其他 prompt 来源。
 */
static char *read_text_file(const char *path)
{
    cc_filesystem_t fs;
    memset(&fs, 0, sizeof(fs));
    cc_result_t rc = cc_filesystem_get_default(&fs);
    if (rc.code != CC_OK || !fs.vtable || !fs.vtable->read_text) {
        cc_result_free(&rc);
        return NULL;
    }
    char *text = NULL;
    rc = fs.vtable->read_text(fs.self, path, &text);
    cc_result_free(&rc);
    if (fs.vtable->destroy) fs.vtable->destroy(fs.self);
    return text;
}


/*
 * 替换配置字符串字段。
 *
 * 新值复制成功后才释放旧值，避免 OOM 时把原配置字段清空。value 为 NULL 表示不修改。
 */
static int replace_string(char **field, const char *value)
{
    if (!value) return 1;
    char *copy = cc_copy_string(value);
    if (!copy) return 0;
    free(*field);
    *field = copy;
    return 1;
}

/*
 * 将根目录和叶子名拼接为完整路径，写入 field（释放旧值）。
 *
 * 参数:
 *   field - 目标字符串指针（旧值会被释放）
 *   root  - 根目录路径
 *   leaf  - 文件名或子目录名
 * 返回: 1 成功，0 内存分配失败。
 *
 * 自动处理路径分隔符。
 */
static int replace_joined_path(char **field, const char *root, const char *leaf)
{
    if (!root || !leaf) return 1;
    size_t root_len = strlen(root);
    size_t leaf_len = strlen(leaf);
    int has_sep = root_len > 0 &&
        (root[root_len - 1] == '/' || root[root_len - 1] == '\\');
    char *path = malloc(root_len + leaf_len + (has_sep ? 1 : 2));
    if (!path) return 0;
    memcpy(path, root, root_len);
    size_t pos = root_len;
    if (!has_sep) path[pos++] = '/';
    memcpy(path + pos, leaf, leaf_len);
    path[pos + leaf_len] = '\0';
    free(*field);
    *field = path;
    return 1;
}


/* 从 JSON object 中读取字符串字段；返回 JSON AST 内部借用指针。 */
static const char *json_string_field(cc_json_value_t *obj, const char *key)
{
    if (!obj || !key) return NULL;
    return cc_json_string_value(cc_json_object_get(obj, key));
}


/*
 * 解析 bool-like 配置值。
 *
 * 支持 true/1/on/yes 字符串，也支持 JSON bool 或非零数字，方便手写配置文件兼容不同风格。
 */
static int json_boolish_value(cc_json_value_t *value)
{
    const char *s = cc_json_string_value(value);
    if (s) {
        return strcmp(s, "true") == 0 || strcmp(s, "1") == 0 ||
               strcmp(s, "on") == 0 || strcmp(s, "yes") == 0;
    }
    return cc_json_bool_value(value) || cc_json_int_value(value) != 0;
}

/*
 * 将 JSON 中的日志级别字符串解析为数值。
 *
 * 参数:
 *   value    - 包含级别字符串的 JSON 值（如 "debug"、"info"、"error"）
 *   fallback - JSON 值无效或为 NULL 时的默认值
 * 返回: 日志级别数值（0=trace 到 5=fatal）。
 */
static int parse_log_level_value(cc_json_value_t *value, int fallback)
{
    const char *s = cc_json_string_value(value);
    if (s) {
        if (strcmp(s, "trace") == 0) return CC_CONFIG_LOG_TRACE;
        if (strcmp(s, "debug") == 0) return CC_CONFIG_LOG_DEBUG;
        if (strcmp(s, "info") == 0) return CC_CONFIG_LOG_INFO;
        if (strcmp(s, "warn") == 0 || strcmp(s, "warning") == 0) return CC_CONFIG_LOG_WARN;
        if (strcmp(s, "error") == 0) return CC_CONFIG_LOG_ERROR;
        if (strcmp(s, "fatal") == 0) return CC_CONFIG_LOG_FATAL;
        return fallback;
    }
    return value ? cc_json_int_value(value) : fallback;
}

/*
 * 从 JSON 配置根节点解析 HTTP 网络配置（超时、重试、日志级别、trace）。
 *
 * 参数:
 *   config - 目标 cc_config_t（字段被直接修改）
 *   root   - JSON 根对象
 *
 * 读取 network.http 节点下的 connect_timeout_ms、first_byte_timeout_ms、
 * stream_idle_timeout_ms、retry_count、log_level 和 trace 子配置。
 */
static void parse_network_http_config(cc_config_t *config, cc_json_value_t *root)
{
    cc_json_value_t *network = cc_json_object_get(root, "network");
    cc_json_value_t *http = network && cc_json_is_object(network) ?
        cc_json_object_get(network, "http") : NULL;
    if (!http || !cc_json_is_object(http)) return;

    cc_json_value_t *v = cc_json_object_get(http, "connect_timeout_ms");
    if (!v) v = cc_json_object_get(http, "connectTimeoutMs");
    if (v) config->http_connect_timeout_ms = cc_json_int_value(v);

    v = cc_json_object_get(http, "first_byte_timeout_ms");
    if (!v) v = cc_json_object_get(http, "firstByteTimeoutMs");
    if (v) config->http_first_byte_timeout_ms = cc_json_int_value(v);

    v = cc_json_object_get(http, "stream_idle_timeout_ms");
    if (!v) v = cc_json_object_get(http, "streamIdleTimeoutMs");
    if (v) config->http_stream_idle_timeout_ms = cc_json_int_value(v);

    v = cc_json_object_get(http, "retry_count");
    if (!v) v = cc_json_object_get(http, "retryCount");
    if (v) config->http_retry_count = cc_json_int_value(v);

    v = cc_json_object_get(http, "log_level");
    if (!v) v = cc_json_object_get(http, "logLevel");
    if (v) config->http_log_level = parse_log_level_value(v, config->http_log_level);

    cc_json_value_t *trace = cc_json_object_get(http, "trace");
    if (trace && cc_json_is_object(trace)) {
        v = cc_json_object_get(trace, "persist");
        if (v) config->http_trace_persist = json_boolish_value(v);
    }
}

/* 释放配置字符串列表。 */
static void free_string_list(cc_config_string_list_t *list)
{
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

/* 向配置字符串列表追加一项深拷贝。 */
static int append_string_to_list(cc_config_string_list_t *list, const char *value)
{
    if (!list || !value) return 1;
    char **next = realloc(list->items, (list->count + 1) * sizeof(char *));
    if (!next) return 0;
    list->items = next;
    list->items[list->count] = cc_copy_string(value);
    if (!list->items[list->count]) return 0;
    list->count++;
    return 1;
}

/*
 * 从 JSON 字符串数组解析配置列表。
 *
 * 先释放 out 中旧列表，再逐项复制字符串；非数组或缺失字段表示空列表且不是错误。
 */
static int parse_string_array(cc_json_value_t *array, cc_config_string_list_t *out)
{
    if (!out) return 0;
    free_string_list(out);
    if (!array || !cc_json_is_array(array)) return 1;
    int count = cc_json_array_size(array);
    for (int i = 0; i < count; i++) {
        const char *value = cc_json_string_value(cc_json_array_get(array, i));
        if (value && !append_string_to_list(out, value)) return 0;
    }
    return 1;
}

/* 释放单个 agent profile 的所有字段。 */
static void free_agent_profile(cc_config_agent_profile_t *profile)
{
    if (!profile) return;
    free(profile->id);
    free(profile->model);
    free(profile->workspace);
    free(profile->agent_dir);
    free(profile->system_prompt_file);
    free_string_list(&profile->skills);
    memset(profile, 0, sizeof(*profile));
}

/* 释放单个 plugin 配置项及其 args/tools/skill_dirs。 */
static void free_plugin_entry(cc_config_plugin_entry_t *entry)
{
    if (!entry) return;
    free(entry->id);
    free(entry->command);
    for (size_t i = 0; i < entry->arg_count; i++) free(entry->args[i]);
    free(entry->args);
    for (size_t i = 0; i < entry->tool_count; i++) {
        free(entry->tools[i].name);
        free(entry->tools[i].description);
        free(entry->tools[i].parameters_json);
    }
    free(entry->tools);
    free_string_list(&entry->skill_dirs);
    memset(entry, 0, sizeof(*entry));
}

/* 释放单个 MCP server 配置项。 */
static void free_mcp_server(cc_config_mcp_server_t *server)
{
    if (!server) return;
    free(server->name);
    free(server->command);
    for (size_t i = 0; i < server->arg_count; i++) free(server->args[i]);
    free(server->args);
    free(server->cwd);
    free(server->url);
    free(server->transport);
    memset(server, 0, sizeof(*server));
}

/* 如果 JSON 中存在 key，就复制到 out 字符串字段。 */
static int copy_json_string_field(cc_json_value_t *obj, const char *key, char **out)
{
    const char *value = json_string_field(obj, key);
    return value ? replace_string(out, value) : 1;
}

/*
 * 解析字符串参数数组。
 *
 * 先释放旧 args，再为 JSON 数组中的字符串项建立新数组；非字符串项被忽略，适合配置
 * 容错，但仍会在 command 缺失等关键处由 validate 阶段报错。
 */
static int copy_args_array(cc_json_value_t *array, char ***out_args, size_t *out_count)
{
    if (!out_args || !out_count) return 0;
    for (size_t i = 0; i < *out_count; i++) free((*out_args)[i]);
    free(*out_args);
    *out_args = NULL;
    *out_count = 0;
    if (!array || !cc_json_is_array(array)) return 1;
    int count = cc_json_array_size(array);
    char **args = count > 0 ? calloc((size_t)count, sizeof(char *)) : NULL;
    if (count > 0 && !args) return 0;
    size_t written = 0;
    for (int i = 0; i < count; i++) {
        const char *value = cc_json_string_value(cc_json_array_get(array, i));
        if (!value) continue;
        args[written] = cc_copy_string(value);
        if (!args[written]) {
            for (size_t j = 0; j < written; j++) free(args[j]);
            free(args);
            return 0;
        }
        written++;
    }
    *out_args = args;
    *out_count = written;
    return 1;
}

/*
 * 解析单个 agent profile。
 *
 * fallback_id 用于 defaults 或对象 key 作为 profile id；同时支持 camelCase 和 snake_case
 * 字段，降低配置迁移成本。
 */
static int parse_agent_profile(cc_json_value_t *obj, const char *fallback_id, cc_config_agent_profile_t *out)
{
    if (!out) return 0;
    if (fallback_id && !out->id && !replace_string(&out->id, fallback_id)) return 0;
    if (!obj || !cc_json_is_object(obj)) return 1;
    if (!copy_json_string_field(obj, "id", &out->id)) return 0;
    if (!copy_json_string_field(obj, "model", &out->model)) return 0;
    if (!copy_json_string_field(obj, "workspace", &out->workspace)) return 0;
    if (!copy_json_string_field(obj, "agentDir", &out->agent_dir)) return 0;
    if (!copy_json_string_field(obj, "agent_dir", &out->agent_dir)) return 0;
    if (!copy_json_string_field(obj, "systemPromptFile", &out->system_prompt_file)) return 0;
    if (!copy_json_string_field(obj, "system_prompt_file", &out->system_prompt_file)) return 0;
    if (!parse_string_array(cc_json_object_get(obj, "skills"), &out->skills)) return 0;
    return 1;
}

/*
 * 同步旧 enabled_tools 缓存。
 *
 * 早期 API 直接读取 config->enabled_tools；新配置使用 tools.enabled。这里复制一份兼容
 * 缓存，避免 runtime builder 和旧测试读取不同字段。
 */
static int sync_enabled_tools_cache(cc_config_t *config)
{
    for (size_t i = 0; i < config->enabled_tools_count; i++) free(config->enabled_tools[i]);
    free(config->enabled_tools);
    config->enabled_tools = NULL;
    config->enabled_tools_count = 0;
    if (config->tools.enabled.count == 0) return 1;
    config->enabled_tools = calloc(config->tools.enabled.count, sizeof(char *));
    if (!config->enabled_tools) return 0;
    for (size_t i = 0; i < config->tools.enabled.count; i++) {
        config->enabled_tools[i] = cc_copy_string(config->tools.enabled.items[i]);
        if (!config->enabled_tools[i]) return 0;
    }
    config->enabled_tools_count = config->tools.enabled.count;
    return 1;
}


/* 检查默认配置中必须分配成功的字段。 */
static cc_result_t config_required_allocs_ok(const cc_config_t *config)
{
    if (!config->provider || !config->model || !config->base_url ||
        !config->storage_type || !config->data_dir || !config->storage_path ||
        !config->session_media_dir ||
        !config->workspace_path || !config->sandbox_type ||
        !config->memory_backend || !config->memory_path ||
        !config->active_memory_category ||
        !config->soul_file || !config->user_file ||
        !config->queue.mode) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate config defaults");
    }
    return cc_result_ok();
}

/* 解析 queue 配置段。 */
static int parse_queue_section(cc_config_t *config, cc_json_value_t *root)
{
    cc_json_value_t *queue = cc_json_object_get(root, "queue");
    if (!queue || !cc_json_is_object(queue)) return 1;
    cc_json_value_t *lanes = cc_json_object_get(queue, "lanes");
    if (lanes && cc_json_is_object(lanes)) {
        cc_json_value_t *v = cc_json_object_get(lanes, "main");
        if (v) config->queue.main_concurrency = cc_json_int_value(v);
        v = cc_json_object_get(lanes, "subagent");
        if (v) config->queue.subagent_concurrency = cc_json_int_value(v);
        v = cc_json_object_get(lanes, "plugin");
        if (v) config->queue.plugin_concurrency = cc_json_int_value(v);
        v = cc_json_object_get(lanes, "mcp");
        if (v) config->queue.mcp_concurrency = cc_json_int_value(v);
    }
    cc_json_value_t *v = cc_json_object_get(queue, "perSessionConcurrency");
    if (!v) v = cc_json_object_get(queue, "per_session_concurrency");
    if (v) config->queue.per_session_concurrency = cc_json_int_value(v);
    if (!copy_json_string_field(queue, "mode", &config->queue.mode)) return 0;
    v = cc_json_object_get(queue, "debounceMs");
    if (!v) v = cc_json_object_get(queue, "debounce_ms");
    if (v) config->queue.debounce_ms = cc_json_int_value(v);
    v = cc_json_object_get(queue, "maxPendingPerSession");
    if (!v) v = cc_json_object_get(queue, "max_pending_per_session");
    if (v) config->queue.max_pending_per_session = cc_json_int_value(v);
    v = cc_json_object_get(queue, "maxTotalTasks");
    if (!v) v = cc_json_object_get(queue, "max_total_tasks");
    if (v) {
        double value = cc_json_number_value(v);
        if (value < 0 || value > (double)SIZE_MAX) return 0;
        config->queue.max_total_tasks = (size_t)value;
    }
    v = cc_json_object_get(queue, "completedTtlMs");
    if (!v) v = cc_json_object_get(queue, "completed_ttl_ms");
    if (v) {
        double value = cc_json_number_value(v);
        if (value < 0 || value > (double)UINT_MAX) return 0;
        config->queue.completed_ttl_ms = (unsigned)value;
    }
    return 1;
}

/* 解析 agents 配置段，包括 defaults 和 list。 */
static int parse_agents_section(cc_config_t *config, cc_json_value_t *root)
{
    cc_json_value_t *agents = cc_json_object_get(root, "agents");
    if (!agents || !cc_json_is_object(agents)) return 1;
    if (!parse_agent_profile(cc_json_object_get(agents, "defaults"), "defaults", &config->agents.defaults)) {
        return 0;
    }
    cc_json_value_t *list = cc_json_object_get(agents, "list");
    if (!list || !cc_json_is_array(list)) return 1;
    for (size_t i = 0; i < config->agents.profile_count; i++) {
        free_agent_profile(&config->agents.profiles[i]);
    }
    free(config->agents.profiles);
    config->agents.profiles = NULL;
    config->agents.profile_count = 0;
    int count = cc_json_array_size(list);
    if (count <= 0) return 1;
    config->agents.profiles = calloc((size_t)count, sizeof(cc_config_agent_profile_t));
    if (!config->agents.profiles) return 0;
    for (int i = 0; i < count; i++) {
        if (!parse_agent_profile(
                cc_json_array_get(list, i),
                NULL,
                &config->agents.profiles[config->agents.profile_count])) {
            return 0;
        }
        config->agents.profile_count++;
    }
    return 1;
}

/*
 * 解析 tools 配置段。
 *
 * 包括 enabled、network allowlist、默认 timeout 和 perTool 策略；解析后同步旧 enabled_tools
 * 缓存，确保旧调用路径仍能看到工具启用列表。
 */
static int parse_tools_section(cc_config_t *config, cc_json_value_t *root)
{
    cc_json_value_t *tools = cc_json_object_get(root, "tools");
    if (!tools || !cc_json_is_object(tools)) return 1;
    cc_json_value_t *enabled = cc_json_object_get(tools, "enabled");
    if (enabled && cc_json_is_array(enabled)) {
        if (!parse_string_array(enabled, &config->tools.enabled)) return 0;
        if (!sync_enabled_tools_cache(config)) return 0;
    }
    cc_json_value_t *v = cc_json_object_get(tools, "defaultTimeoutMs");
    if (!v) v = cc_json_object_get(tools, "default_timeout_ms");
    if (v) config->tools.default_timeout_ms = cc_json_int_value(v);

    cc_json_value_t *network_allowlist = cc_json_object_get(tools, "networkAllowlist");
    if (!network_allowlist) network_allowlist = cc_json_object_get(tools, "network_allowlist");
    if (!network_allowlist) network_allowlist = cc_json_object_get(tools, "http_allowlist");
    if (network_allowlist && cc_json_is_array(network_allowlist)) {
        if (!parse_string_array(network_allowlist, &config->tools.network_allowlist)) return 0;
    }

    for (size_t i = 0; i < config->tools.policy_count; i++) {
        free(config->tools.policies[i].name);
    }
    free(config->tools.policies);
    config->tools.policies = NULL;
    config->tools.policy_count = 0;

    cc_json_value_t *per_tool = cc_json_object_get(tools, "perTool");
    if (!per_tool) per_tool = cc_json_object_get(tools, "per_tool");
    if (!per_tool || !cc_json_is_object(per_tool)) return 1;
    int count = cc_json_object_size(per_tool);
    if (count <= 0) return 1;
    config->tools.policies = calloc((size_t)count, sizeof(cc_config_tool_policy_t));
    if (!config->tools.policies) return 0;
    for (int i = 0; i < count; i++) {
        const char *name = cc_json_object_key_at(per_tool, i);
        cc_json_value_t *obj = cc_json_object_value_at(per_tool, i);
        cc_config_tool_policy_t *policy = &config->tools.policies[config->tools.policy_count];
        policy->name = name ? cc_copy_string(name) : NULL;
        if (name && !policy->name) return 0;
        v = cc_json_object_get(obj, "concurrency");
        if (v) policy->concurrency = cc_json_int_value(v);
        v = cc_json_object_get(obj, "timeoutMs");
        if (!v) v = cc_json_object_get(obj, "timeout_ms");
        if (v) policy->timeout_ms = cc_json_int_value(v);
        config->tools.policy_count++;
    }
    return 1;
}

/* 解析 plugin 声明的 tools 数组，并把 parameters AST 转成 JSON 字符串保存。 */
static int parse_plugin_tools(cc_json_value_t *array, cc_config_plugin_entry_t *entry)
{
    if (!array || !cc_json_is_array(array)) return 1;
    int count = cc_json_array_size(array);
    entry->tools = count > 0 ? calloc((size_t)count, sizeof(cc_config_plugin_tool_t)) : NULL;
    if (count > 0 && !entry->tools) return 0;
    for (int i = 0; i < count; i++) {
        cc_json_value_t *tool = cc_json_array_get(array, i);
        cc_config_plugin_tool_t *out = &entry->tools[entry->tool_count];
        const char *s = json_string_field(tool, "name");
        out->name = s ? cc_copy_string(s) : NULL;
        s = json_string_field(tool, "description");
        out->description = s ? cc_copy_string(s) : NULL;
        cc_json_value_t *params = cc_json_object_get(tool, "parameters");
        out->parameters_json = params ? cc_json_stringify_unformatted(params) : NULL;
        if (!out->parameters_json) out->parameters_json = cc_copy_string("{\"type\":\"object\",\"properties\":{}}");
        if (!out->name || !out->description || !out->parameters_json) return 0;
        entry->tool_count++;
    }
    return 1;
}

/*
 * 解析 plugins 配置段。
 *
 * entries 是按对象 key 命名的 plugin 集合；每个 plugin 设置默认并发、timeout 和重启策略，
 * 再读取 command/args/tools/skills。
 */
static int parse_plugins_section(cc_config_t *config, cc_json_value_t *root)
{
    cc_json_value_t *plugins = cc_json_object_get(root, "plugins");
    if (!plugins || !cc_json_is_object(plugins)) return 1;
    cc_json_value_t *v = cc_json_object_get(plugins, "hotReload");
    if (!v) v = cc_json_object_get(plugins, "hot_reload");
    if (v) config->plugins.hot_reload = json_boolish_value(v);
    v = cc_json_object_get(plugins, "reloadDebounceMs");
    if (!v) v = cc_json_object_get(plugins, "reload_debounce_ms");
    if (v) config->plugins.reload_debounce_ms = cc_json_int_value(v);

    for (size_t i = 0; i < config->plugins.entry_count; i++) free_plugin_entry(&config->plugins.entries[i]);
    free(config->plugins.entries);
    config->plugins.entries = NULL;
    config->plugins.entry_count = 0;

    cc_json_value_t *entries = cc_json_object_get(plugins, "entries");
    if (!entries || !cc_json_is_object(entries)) return 1;
    int count = cc_json_object_size(entries);
    config->plugins.entries = count > 0 ? calloc((size_t)count, sizeof(cc_config_plugin_entry_t)) : NULL;
    if (count > 0 && !config->plugins.entries) return 0;
    for (int i = 0; i < count; i++) {
        const char *id = cc_json_object_key_at(entries, i);
        cc_json_value_t *entry_json = cc_json_object_value_at(entries, i);
        cc_config_plugin_entry_t *entry = &config->plugins.entries[config->plugins.entry_count];
        entry->id = id ? cc_copy_string(id) : NULL;
        entry->enabled = 1;
        entry->workers = 1;
        entry->timeout_ms = config->tools.default_timeout_ms ? config->tools.default_timeout_ms : 30000;
        entry->max_in_flight = 1;
        entry->restart_on_crash = 1;
        if (id && !entry->id) return 0;
        v = cc_json_object_get(entry_json, "enabled");
        if (v) entry->enabled = json_boolish_value(v);
        if (!copy_json_string_field(entry_json, "command", &entry->command)) return 0;
        if (!copy_args_array(cc_json_object_get(entry_json, "args"), &entry->args, &entry->arg_count)) return 0;
        v = cc_json_object_get(entry_json, "workers");
        if (v) entry->workers = cc_json_int_value(v);
        v = cc_json_object_get(entry_json, "timeoutMs");
        if (!v) v = cc_json_object_get(entry_json, "timeout_ms");
        if (v) entry->timeout_ms = cc_json_int_value(v);
        v = cc_json_object_get(entry_json, "maxInFlight");
        if (!v) v = cc_json_object_get(entry_json, "max_in_flight");
        if (v) entry->max_in_flight = cc_json_int_value(v);
        v = cc_json_object_get(entry_json, "restartOnCrash");
        if (!v) v = cc_json_object_get(entry_json, "restart_on_crash");
        if (v) entry->restart_on_crash = json_boolish_value(v);
        if (!parse_plugin_tools(cc_json_object_get(entry_json, "tools"), entry)) return 0;
        if (!parse_string_array(cc_json_object_get(entry_json, "skills"), &entry->skill_dirs)) return 0;
        config->plugins.entry_count++;
    }
    return 1;
}

/* 解析 skills.load 配置段。 */
static int parse_skills_section(cc_config_t *config, cc_json_value_t *root)
{
    cc_json_value_t *skills = cc_json_object_get(root, "skills");
    if (!skills || !cc_json_is_object(skills)) return 1;
    cc_json_value_t *load = cc_json_object_get(skills, "load");
    if (load && cc_json_is_object(load)) {
        cc_json_value_t *v = cc_json_object_get(load, "watch");
        if (v) config->skills.watch = json_boolish_value(v);
        v = cc_json_object_get(load, "watchDebounceMs");
        if (!v) v = cc_json_object_get(load, "watch_debounce_ms");
        if (v) config->skills.watch_debounce_ms = cc_json_int_value(v);
        cc_json_value_t *extra_dirs = cc_json_object_get(load, "extraDirs");
        if (!extra_dirs) extra_dirs = cc_json_object_get(load, "extra_dirs");
        if (extra_dirs && !parse_string_array(extra_dirs, &config->skills.extra_dirs)) return 0;
    }
    return 1;
}

/* 解析 MCP server 配置段。 */
static int parse_mcp_section(cc_config_t *config, cc_json_value_t *root)
{
    cc_json_value_t *mcp = cc_json_object_get(root, "mcp");
    if (!mcp || !cc_json_is_object(mcp)) return 1;
    cc_json_value_t *v = cc_json_object_get(mcp, "enabled");
    if (v) config->mcp.enabled = json_boolish_value(v);
    v = cc_json_object_get(mcp, "sessionIdleTtlMs");
    if (!v) v = cc_json_object_get(mcp, "session_idle_ttl_ms");
    if (v) config->mcp.session_idle_ttl_ms = cc_json_int_value(v);

    for (size_t i = 0; i < config->mcp.server_count; i++) free_mcp_server(&config->mcp.servers[i]);
    free(config->mcp.servers);
    config->mcp.servers = NULL;
    config->mcp.server_count = 0;

    cc_json_value_t *servers = cc_json_object_get(mcp, "servers");
    if (!servers || !cc_json_is_object(servers)) return 1;
    int count = cc_json_object_size(servers);
    config->mcp.servers = count > 0 ? calloc((size_t)count, sizeof(cc_config_mcp_server_t)) : NULL;
    if (count > 0 && !config->mcp.servers) return 0;
    for (int i = 0; i < count; i++) {
        const char *name = cc_json_object_key_at(servers, i);
        cc_json_value_t *server_json = cc_json_object_value_at(servers, i);
        cc_config_mcp_server_t *server = &config->mcp.servers[config->mcp.server_count];
        server->name = name ? cc_copy_string(name) : NULL;
        if (name && !server->name) return 0;
        if (!copy_json_string_field(server_json, "command", &server->command)) return 0;
        if (!copy_args_array(cc_json_object_get(server_json, "args"), &server->args, &server->arg_count)) return 0;
        if (!copy_json_string_field(server_json, "cwd", &server->cwd)) return 0;
        if (!copy_json_string_field(server_json, "workingDirectory", &server->cwd)) return 0;
        if (!copy_json_string_field(server_json, "url", &server->url)) return 0;
        if (!copy_json_string_field(server_json, "transport", &server->transport)) return 0;
        v = cc_json_object_get(server_json, "connectionTimeoutMs");
        if (!v) v = cc_json_object_get(server_json, "connection_timeout_ms");
        if (v) server->connection_timeout_ms = cc_json_int_value(v);
        config->mcp.server_count++;
    }
    return 1;
}

/* 解析 image/audio/video/file 四个 modality bool 开关。 */
static void parse_modality_flags(cc_json_value_t *obj, cc_config_modality_flags_t *flags)
{
    if (!obj || !cc_json_is_object(obj) || !flags) return;
    cc_json_value_t *v = cc_json_object_get(obj, "image");
    if (v) flags->image = json_boolish_value(v);
    v = cc_json_object_get(obj, "audio");
    if (v) flags->audio = json_boolish_value(v);
    v = cc_json_object_get(obj, "video");
    if (v) flags->video = json_boolish_value(v);
    v = cc_json_object_get(obj, "file");
    if (v) flags->file = json_boolish_value(v);
}

/* 解析 multimodal 配置段，包括 input/output 开关和资源限制。 */
static int parse_multimodal_section(cc_config_t *config, cc_json_value_t *root)
{
    cc_json_value_t *mm = cc_json_object_get(root, "multimodal");
    if (!mm || !cc_json_is_object(mm)) return 1;
    parse_modality_flags(cc_json_object_get(mm, "input"), &config->multimodal.input);
    parse_modality_flags(cc_json_object_get(mm, "output"), &config->multimodal.output);

    cc_json_value_t *limits = cc_json_object_get(mm, "limits");
    if (!limits || !cc_json_is_object(limits)) return 1;
    cc_json_value_t *v = cc_json_object_get(limits, "max_artifacts");
    if (!v) v = cc_json_object_get(limits, "maxArtifacts");
    if (v) config->multimodal.limits.max_artifacts = (size_t)cc_json_int_value(v);
    v = cc_json_object_get(limits, "max_artifact_bytes");
    if (!v) v = cc_json_object_get(limits, "maxArtifactBytes");
    if (v) config->multimodal.limits.max_artifact_bytes = (size_t)cc_json_number_value(v);
    v = cc_json_object_get(limits, "max_base64_bytes");
    if (!v) v = cc_json_object_get(limits, "maxBase64Bytes");
    if (v) config->multimodal.limits.max_base64_bytes = (size_t)cc_json_number_value(v);
    v = cc_json_object_get(limits, "allow_inline_base64");
    if (!v) v = cc_json_object_get(limits, "allowInlineBase64");
    if (v) config->multimodal.limits.allow_inline_base64 = json_boolish_value(v);
    cc_json_value_t *mimes = cc_json_object_get(limits, "allowed_mime_prefixes");
    if (!mimes) mimes = cc_json_object_get(limits, "allowedMimePrefixes");
    if (mimes && !parse_string_array(mimes, &config->multimodal.limits.allowed_mime_prefixes)) {
        return 0;
    }
    return 1;
}

/*
 * 解析所有 runtime 相关扩展配置段。
 *
 * 拆成多个 section parser 可以让缺失段按默认值运行，任一段 OOM 或结构错误时统一返回 0。
 */
static int parse_runtime_sections(cc_config_t *config, cc_json_value_t *root)
{
    return parse_queue_section(config, root) &&
           parse_agents_section(config, root) &&
           parse_tools_section(config, root) &&
           parse_plugins_section(config, root) &&
           parse_skills_section(config, root) &&
           parse_mcp_section(config, root) &&
           parse_multimodal_section(config, root);
}

/* 判断字符串是否非空；用于 validate 阶段给出明确错误。 */
static int str_nonempty(const char *s)
{
    return s && s[0] != '\0';
}

/* 检查 MCP transport 名称是否为当前 SDK 认识的类型。 */
static int mcp_transport_known(const char *transport)
{
    const char *t = transport ? transport : "stdio";
    return strcmp(t, "stdio") == 0 ||
           strcmp(t, "http") == 0 ||
           strcmp(t, "sse") == 0 ||
           strcmp(t, "streamable_http") == 0;
}

/*
 * 校验 runtime 扩展配置。
 *
 * 该阶段不分配资源，只检查并发/timeout 非负、plugin command、MCP transport/url 等语义，
 * 让配置错误在 runtime builder 前 fail-fast。
 */
static cc_result_t validate_runtime_sections(const cc_config_t *config)
{
    if (!config) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null config");
    if (config->max_steps < 0 || config->run_timeout_ms < 0 ||
        config->provider_timeout_ms < 0 || config->tool_timeout_ms < 0 ||
        config->max_input_bytes < 0 || config->max_output_bytes < 0 ||
        config->max_stream_bytes < 0 || config->max_tool_result_bytes < 0 ||
        config->max_concurrency < 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,
                               "runtime limits must be non-negative");
    }
    if (config->context_window_tokens <= 0 ||
        config->context_keep_recent < 0 ||
        config->summary_max_tokens <= 0 ||
        config->context_compress_threshold < 0.0 ||
        config->context_compress_threshold > 1.0 ||
        config->summary_temperature < 0.0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,
                               "context compression settings are out of range");
    }
    if (config->queue.main_concurrency <= 0 ||
        config->queue.subagent_concurrency <= 0 ||
        config->queue.plugin_concurrency <= 0 ||
        config->queue.mcp_concurrency <= 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "queue.lanes values must be positive");
    }
    if (config->queue.max_pending_per_session < 0 || config->queue.debounce_ms < 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "queue limits must be non-negative");
    }
    if (config->tools.default_timeout_ms < 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "tools.defaultTimeoutMs must be non-negative");
    }
    for (size_t i = 0; i < config->tools.policy_count; i++) {
        const cc_config_tool_policy_t *policy = &config->tools.policies[i];
        if (!str_nonempty(policy->name)) {
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "tools.perTool entry has an empty name");
        }
        if (policy->concurrency < 0 || policy->timeout_ms < 0) {
            return cc_result_errf(
                CC_ERR_INVALID_ARGUMENT,
                "tools.perTool.%s concurrency/timeout must be non-negative",
                policy->name);
        }
    }
    for (size_t i = 0; i < config->plugins.entry_count; i++) {
        const cc_config_plugin_entry_t *plugin = &config->plugins.entries[i];
        if (!str_nonempty(plugin->id)) {
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "plugins.entries contains an empty id");
        }
        if (plugin->enabled && !str_nonempty(plugin->command)) {
            return cc_result_errf(
                CC_ERR_INVALID_ARGUMENT,
                "plugins.entries.%s requires command",
                plugin->id);
        }
        if (plugin->workers <= 0 || plugin->max_in_flight <= 0 || plugin->timeout_ms < 0) {
            return cc_result_errf(
                CC_ERR_INVALID_ARGUMENT,
                "plugins.entries.%s workers/maxInFlight must be positive and timeout non-negative",
                plugin->id);
        }
        for (size_t j = 0; j < plugin->tool_count; j++) {
            if (!str_nonempty(plugin->tools[j].name)) {
                return cc_result_errf(
                    CC_ERR_INVALID_ARGUMENT,
                    "plugins.entries.%s has a tool without name",
                    plugin->id);
            }
        }
    }
    if (config->skills.watch_debounce_ms < 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "skills.load.watchDebounceMs must be non-negative");
    }
    if (config->mcp.session_idle_ttl_ms < 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "mcp.sessionIdleTtlMs must be non-negative");
    }
    for (size_t i = 0; i < config->mcp.server_count; i++) {
        const cc_config_mcp_server_t *server = &config->mcp.servers[i];
        const char *transport = server->transport ? server->transport : "stdio";
        if (!str_nonempty(server->name)) {
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "mcp.servers contains an empty id");
        }
        if (!mcp_transport_known(transport)) {
            return cc_result_errf(
                CC_ERR_INVALID_ARGUMENT,
                "mcp.servers.%s has unknown transport '%s'",
                server->name,
                transport);
        }
        if (strcmp(transport, "stdio") == 0 && !str_nonempty(server->command)) {
            return cc_result_errf(
                CC_ERR_INVALID_ARGUMENT,
                "mcp.servers.%s stdio transport requires command",
                server->name);
        }
        if ((strcmp(transport, "http") == 0 ||
             strcmp(transport, "sse") == 0 ||
             strcmp(transport, "streamable_http") == 0) &&
            !str_nonempty(server->url)) {
            return cc_result_errf(
                CC_ERR_INVALID_ARGUMENT,
                "mcp.servers.%s %s transport requires url",
                server->name,
                transport);
        }
        if (server->connection_timeout_ms < 0) {
            return cc_result_errf(
                CC_ERR_INVALID_ARGUMENT,
                "mcp.servers.%s connectionTimeoutMs must be non-negative",
                server->name);
        }
    }
    if (config->active_memory_max_value_chars < 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "memory.active.maxValueChars must be non-negative");
    }
    if (config->session_segment_bytes == 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "storage.sessionSegmentBytes must be positive");
    }
    if (!str_nonempty(config->session_media_dir)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "storage.sessionMediaDir must be non-empty");
    }
    return cc_result_ok();
}

/*
 * 校验完整配置。
 *
 * 先校验 runtime sections，再校验 multimodal 与当前编译 profile 的能力开关是否匹配，
 * 避免未编译的媒体能力在运行时静默降级。
 */
cc_result_t cc_config_validate(const cc_config_t *config)
{
    if (!config) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null config");
    cc_result_t rc = validate_runtime_sections(config);
    if (rc.code != CC_OK) return rc;
    const cc_multimodal_config_t *mm = &config->multimodal;
    if (!CC_ENABLE_MULTIMODAL &&
        (mm->input.image || mm->input.audio || mm->input.video || mm->input.file ||
         mm->output.image || mm->output.audio || mm->output.video || mm->output.file)) {
        return cc_result_error(CC_ERR_UNSUPPORTED, "multimodal is not compiled in this profile");
    }
    if ((mm->input.image || mm->output.image) && !CC_ENABLE_MEDIA_IMAGE) {
        return cc_result_error(CC_ERR_UNSUPPORTED, "image modality is not compiled");
    }
    if ((mm->input.audio || mm->output.audio) && !CC_ENABLE_MEDIA_AUDIO) {
        return cc_result_error(CC_ERR_UNSUPPORTED, "audio modality is not compiled");
    }
    if ((mm->input.video || mm->output.video) && !CC_ENABLE_MEDIA_VIDEO) {
        return cc_result_error(CC_ERR_UNSUPPORTED, "video modality is not compiled");
    }
    if ((mm->input.file || mm->output.file) && !CC_ENABLE_MEDIA_FILE) {
        return cc_result_error(CC_ERR_UNSUPPORTED, "file modality is not compiled");
    }
    if ((mm->output.image || mm->output.audio || mm->output.video || mm->output.file) &&
        !CC_ENABLE_MEDIA_OUTPUT) {
        return cc_result_error(CC_ERR_UNSUPPORTED, "media output is not compiled");
    }
    if (mm->limits.allow_inline_base64 && !CC_ENABLE_INLINE_BASE64) {
        return cc_result_error(CC_ERR_UNSUPPORTED, "inline base64 is not compiled");
    }
    return cc_result_ok();
}


/*
 * 加载默认配置。
 *
 * 默认值来自编译期 profile 宏，体现“核心 SDK 可裁剪”的设计。所有字段都在 config 中
 * 拥有，失败时调用 cc_config_destroy() 回滚已分配资源。
 */
cc_result_t cc_config_load_default(cc_config_t *out_config)
{
    memset(out_config, 0, sizeof(cc_config_t));
    out_config->provider = cc_copy_string(CC_DEFAULT_PROVIDER);
    out_config->model = cc_copy_string(CC_DEFAULT_MODEL);
    out_config->base_url = cc_copy_string(CC_DEFAULT_BASE_URL);
    out_config->tls_verify = 1;
    out_config->http_connect_timeout_ms = 15000;
    out_config->http_first_byte_timeout_ms = 30000;
    out_config->http_stream_idle_timeout_ms = 60000;
    out_config->http_retry_count = 1;
    out_config->http_log_level = CC_CONFIG_LOG_WARN;
    out_config->http_trace_persist = 0;
    out_config->storage_type = cc_copy_string(CC_DEFAULT_STORAGE_TYPE);
    out_config->data_dir = cc_copy_string(CC_DEFAULT_DATA_DIR);
    out_config->storage_path = cc_copy_string(CC_DEFAULT_STORAGE_PATH);
    out_config->session_segment_bytes = CC_DEFAULT_SESSION_SEGMENT_BYTES;
    out_config->session_media_dir = cc_copy_string(CC_DEFAULT_SESSION_MEDIA_DIR);
    out_config->workspace_path = cc_copy_string(CC_DEFAULT_WORKSPACE_PATH);
    out_config->sandbox_type = cc_copy_string("local");
    out_config->shell_requires_approval = 1;
    out_config->sandbox_timeout_ms = 30000;
    out_config->max_steps = CC_DEFAULT_MAX_STEPS;
    out_config->run_timeout_ms = 0;
    out_config->provider_timeout_ms = 0;
    out_config->tool_timeout_ms = 0;
    out_config->max_input_bytes = 0;
    out_config->max_output_bytes = 0;
    out_config->max_stream_bytes = 0;
    out_config->max_tool_result_bytes = 0;
    out_config->max_concurrency = 0;
    out_config->stream_mode = 0;
    out_config->debug_mode = 0;
    out_config->max_tokens = 4096;
    out_config->temperature = 0.7;
    out_config->memory_backend = cc_copy_string(CC_DEFAULT_MEMORY_BACKEND);
    out_config->memory_path = cc_copy_string(CC_DEFAULT_MEMORY_PATH);
    out_config->active_memory_enabled = 0;
    out_config->active_memory_write_summary = 1;
    out_config->active_memory_max_value_chars = 1600;
    out_config->active_memory_category = cc_copy_string("active_summary");
    out_config->soul_file = cc_copy_string(CC_DEFAULT_SOUL_FILE);
    out_config->user_file = cc_copy_string(CC_DEFAULT_USER_FILE);
    out_config->context_window_tokens = 8192;
    out_config->context_compress_threshold = 0.8;
    out_config->context_keep_recent = 20;
    out_config->summary_max_tokens = 1024;
    out_config->summary_temperature = 0.3;
    out_config->queue.main_concurrency = 1;
    out_config->queue.subagent_concurrency = 1;
    out_config->queue.plugin_concurrency = 1;
    out_config->queue.mcp_concurrency = 1;
    out_config->queue.per_session_concurrency = 1;
    out_config->queue.mode = cc_copy_string("steer");
    out_config->queue.debounce_ms = 500;
    out_config->queue.max_pending_per_session = 2;
    out_config->queue.max_total_tasks = 256;
    out_config->queue.completed_ttl_ms = 300000;
    out_config->tools.default_timeout_ms = 30000;
    out_config->plugins.hot_reload = 0;
    out_config->plugins.reload_debounce_ms = 300;
    out_config->skills.watch = 0;
    out_config->skills.watch_debounce_ms = 250;
    out_config->mcp.enabled = 0;
    out_config->mcp.session_idle_ttl_ms = 600000;
    out_config->multimodal.limits.max_artifacts = 0;
    out_config->multimodal.limits.max_artifact_bytes = 0;
    out_config->multimodal.limits.max_base64_bytes = 0;
    out_config->multimodal.limits.allow_inline_base64 = 0;
    cc_result_t rc = config_required_allocs_ok(out_config);
    if (rc.code != CC_OK) {
        cc_config_destroy(out_config);
        return rc;
    }
    return cc_result_ok();
}


/*
 * 从 JSON 文件加载配置并覆盖默认值。
 *
 * 流程是先加载默认配置，再按文件中 model/storage/workspace/sandbox/memory/tools/system/cli
 * 以及 runtime sections 覆盖。任何 OOM 走统一 oom 标签，JSON AST 始终释放。
 */
cc_result_t cc_config_load(const char *path, cc_config_t *out_config)
{
    cc_result_t rc = cc_config_load_default(out_config);
    if (rc.code != CC_OK || !path) return rc;

    char *config_text = read_text_file(path);
    if (!config_text) {
        return cc_result_error(CC_ERR_IO, "Cannot read config through Filesystem port");
    }
    cc_json_value_t *root = NULL;
    rc = cc_json_parse(config_text, &root);
    free(config_text);
    if (rc.code != CC_OK) {
        return rc;
    }

    cc_json_value_t *model = cc_json_object_get(root, "model");
    if (model) {
        const char *s = json_string_field(model, "provider");
        if (s && !replace_string(&out_config->provider, s)) goto oom;
        s = json_string_field(model, "model");
        if (s && !replace_string(&out_config->model, s)) goto oom;
        cc_json_value_t *extra_body = cc_json_object_get(model, "extra_body");
        if (!extra_body) extra_body = cc_json_object_get(model, "extraBody");
        if (extra_body && cc_json_is_object(extra_body)) {
            char *extra_body_json = cc_json_stringify_unformatted(extra_body);
            if (!extra_body_json) goto oom;
            int ok = replace_string(&out_config->model_extra_body_json, extra_body_json);
            free(extra_body_json);
            if (!ok) goto oom;
        }
        s = json_string_field(model, "base_url");
        if (s && !replace_string(&out_config->base_url, s)) goto oom;
        s = json_string_field(model, "api_key");
        if (s && !replace_string(&out_config->api_key, s)) goto oom;
        s = json_string_field(model, "api_key_ref");
        if (s && !replace_string(&out_config->api_key_ref, s)) goto oom;
        cc_json_value_t *tls = cc_json_object_get(model, "tls");
        if (tls && cc_json_is_object(tls)) {
            cc_json_value_t *verify = cc_json_object_get(tls, "verify");
            if (verify) out_config->tls_verify = json_boolish_value(verify);
        }
        s = json_string_field(model, "api_key_env");
        if (s && *s) {
            const char *env_value = getenv(s);
            if (env_value && *env_value && !replace_string(&out_config->api_key, env_value)) goto oom;
        }
        cc_json_value_t *tm = cc_json_object_get(model, "thinking_mode");
        out_config->thinking_mode = tm ? cc_json_int_value(tm) : 0;
        cc_json_value_t *mt = cc_json_object_get(model, "max_tokens");
        if (mt) out_config->max_tokens = cc_json_int_value(mt);
        cc_json_value_t *temp = cc_json_object_get(model, "temperature");
        if (temp) out_config->temperature = cc_json_number_value(temp);
        cc_json_value_t *sm = cc_json_object_get(model, "stream_mode");
        if (sm) out_config->stream_mode = json_boolish_value(sm);
    }

    if (out_config->api_key_ref && out_config->api_key_ref[0]) {
        cc_secret_store_t secrets;
        memset(&secrets, 0, sizeof(secrets));
        rc = cc_secret_store_get_default(&secrets);
        if (rc.code == CC_OK && secrets.vtable && secrets.vtable->get) {
            char *resolved = NULL;
            rc = secrets.vtable->get(secrets.self, out_config->api_key_ref, &resolved);
            if (rc.code == CC_OK) {
                cc_secret_free(out_config->api_key);
                out_config->api_key = resolved;
            }
        }
        if (secrets.vtable && secrets.vtable->destroy) secrets.vtable->destroy(secrets.self);
        if (rc.code != CC_OK) {
            cc_json_destroy(root);
            return rc;
        }
    }

    parse_network_http_config(out_config, root);

    cc_json_value_t *storage = cc_json_object_get(root, "storage");
    if (storage) {
        const char *s = json_string_field(storage, "type");
        if (!s) s = json_string_field(storage, "session_backend");
        if (!s) s = json_string_field(storage, "sessionBackend");
        if (s && !replace_string(&out_config->storage_type, s)) goto oom;
        s = json_string_field(storage, "data_dir");
        if (!s) s = json_string_field(storage, "root");
        int has_storage_root = s && *s;
        if (s && !replace_string(&out_config->data_dir, s)) goto oom;
        s = json_string_field(storage, "path");
        if (!s) s = json_string_field(storage, "sessions");
        if (s && !replace_string(&out_config->storage_path, s)) goto oom;
        if (!s && has_storage_root) {
            const char *leaf = out_config->storage_type &&
                strcmp(out_config->storage_type, "json_segmented") == 0 ?
                "sessions" : "sessions.json";
            if (!replace_joined_path(&out_config->storage_path, out_config->data_dir, leaf)) goto oom;
        }
        cc_json_value_t *v = cc_json_object_get(storage, "session_segment_bytes");
        if (!v) v = cc_json_object_get(storage, "sessionSegmentBytes");
        if (v) out_config->session_segment_bytes = (size_t)cc_json_number_value(v);
        s = json_string_field(storage, "session_media_dir");
        if (!s) s = json_string_field(storage, "sessionMediaDir");
        if (s && !replace_string(&out_config->session_media_dir, s)) goto oom;
    }

    cc_json_value_t *workspace = cc_json_object_get(root, "workspace");
    if (workspace) {
        const char *s = json_string_field(workspace, "path");
        if (s && !replace_string(&out_config->workspace_path, s)) goto oom;
    }

    cc_json_value_t *sandbox = cc_json_object_get(root, "sandbox");
    if (sandbox) {
        const char *s = json_string_field(sandbox, "type");
        if (s && !replace_string(&out_config->sandbox_type, s)) goto oom;
        cc_json_value_t *v = cc_json_object_get(sandbox, "shell_requires_approval");
        out_config->shell_requires_approval = v ? cc_json_bool_value(v) : 1;
        v = cc_json_object_get(sandbox, "timeout_ms");
        out_config->sandbox_timeout_ms = v ? cc_json_int_value(v) : 30000;
    }



    cc_json_value_t *memory = cc_json_object_get(root, "memory");
    if (memory) {
        const char *s = json_string_field(memory, "backend");
        if (s && !replace_string(&out_config->memory_backend, s)) goto oom;
        s = json_string_field(memory, "path");
        if (s && !replace_string(&out_config->memory_path, s)) goto oom;
        cc_json_value_t *active = cc_json_object_get(memory, "active");
        if (active && cc_json_is_object(active)) {
            cc_json_value_t *v = cc_json_object_get(active, "enabled");
            if (v) out_config->active_memory_enabled = json_boolish_value(v);
            v = cc_json_object_get(active, "writeSummary");
            if (!v) v = cc_json_object_get(active, "write_summary");
            if (v) out_config->active_memory_write_summary = json_boolish_value(v);
            v = cc_json_object_get(active, "maxValueChars");
            if (!v) v = cc_json_object_get(active, "max_value_chars");
            if (v) out_config->active_memory_max_value_chars = cc_json_int_value(v);
            s = json_string_field(active, "category");
            if (s && !replace_string(&out_config->active_memory_category, s)) goto oom;
        }
    }

    cc_json_value_t *tools = cc_json_object_get(root, "tools");
    if (tools) {
        cc_json_value_t *enabled = cc_json_object_get(tools, "enabled");
        if (enabled && cc_json_is_array(enabled)) {
            int count = cc_json_array_size(enabled);
            size_t string_count = 0;
            for (int i = 0; i < count; i++) {
                cc_json_value_t *item = cc_json_array_get(enabled, i);
                const char *name = cc_json_string_value(item);
                if (name) string_count++;
            }
            char **tools_list = string_count ? calloc(string_count, sizeof(char *)) : NULL;
            if (string_count && !tools_list) goto oom;

            size_t write_i = 0;
            for (int i = 0; i < count; i++) {
                const char *name = cc_json_string_value(cc_json_array_get(enabled, i));
                if (!name) continue;
                tools_list[write_i] = cc_copy_string(name);
                if (!tools_list[write_i]) {
                    for (size_t j = 0; j < write_i; j++) free(tools_list[j]);
                    free(tools_list);
                    goto oom;
                }
                write_i++;
            }
            for (size_t i = 0; i < out_config->enabled_tools_count; i++)
                free(out_config->enabled_tools[i]);
            free(out_config->enabled_tools);
            out_config->enabled_tools = tools_list;
            out_config->enabled_tools_count = string_count;
        }
    }

    cc_json_value_t *system = cc_json_object_get(root, "system");
    if (system) {
        const char *s = json_string_field(system, "soul_file");
        if (s && !replace_string(&out_config->soul_file, s)) goto oom;
        s = json_string_field(system, "user_file");
        if (s && !replace_string(&out_config->user_file, s)) goto oom;
        s = json_string_field(system, "system_prompt");
        if (s && !replace_string(&out_config->system_prompt, s)) goto oom;
        cc_json_value_t *v = cc_json_object_get(system, "max_steps");
        out_config->max_steps = v ? cc_json_int_value(v) : 10;

        v = cc_json_object_get(system, "debug");
        if (v) out_config->debug_mode = json_boolish_value(v);
        v = cc_json_object_get(system, "debug_mode");
        if (v) out_config->debug_mode = json_boolish_value(v);



        v = cc_json_object_get(system, "context_window_tokens");
        out_config->context_window_tokens = v ? cc_json_int_value(v) : 8192;
        v = cc_json_object_get(system, "context_compress_threshold");
        if (v) {
            double threshold = cc_json_number_value(v);
            /* 兼容旧配置的百分数写法 80，同时接受与 runtime 一致的 0.8。 */
            out_config->context_compress_threshold =
                threshold > 1.0 ? threshold / 100.0 : threshold;
        } else {
            out_config->context_compress_threshold = 0.8;
        }
        v = cc_json_object_get(system, "context_keep_recent");
        out_config->context_keep_recent = v ? cc_json_int_value(v) : 20;
        v = cc_json_object_get(system, "summary_max_tokens");
        if (v) out_config->summary_max_tokens = cc_json_int_value(v);
        v = cc_json_object_get(system, "summary_temperature");
        if (v) out_config->summary_temperature = cc_json_number_value(v);
    }

    cc_json_value_t *runtime = cc_json_object_get(root, "runtime");
    if (runtime && cc_json_is_object(runtime)) {
        cc_json_value_t *v = cc_json_object_get(runtime, "max_steps");
        if (v) out_config->max_steps = cc_json_int_value(v);
        v = cc_json_object_get(runtime, "run_timeout_ms");
        if (v) out_config->run_timeout_ms = cc_json_int_value(v);
        v = cc_json_object_get(runtime, "provider_timeout_ms");
        if (v) out_config->provider_timeout_ms = cc_json_int_value(v);
        v = cc_json_object_get(runtime, "tool_timeout_ms");
        if (v) out_config->tool_timeout_ms = cc_json_int_value(v);
        v = cc_json_object_get(runtime, "max_input_bytes");
        if (v) out_config->max_input_bytes = cc_json_int_value(v);
        v = cc_json_object_get(runtime, "max_output_bytes");
        if (v) out_config->max_output_bytes = cc_json_int_value(v);
        v = cc_json_object_get(runtime, "max_stream_bytes");
        if (v) out_config->max_stream_bytes = cc_json_int_value(v);
        v = cc_json_object_get(runtime, "max_tool_result_bytes");
        if (v) out_config->max_tool_result_bytes = cc_json_int_value(v);
        v = cc_json_object_get(runtime, "max_concurrency");
        if (v) out_config->max_concurrency = cc_json_int_value(v);
    }

    cc_json_value_t *cli = cc_json_object_get(root, "cli");
    if (cli) {
        cc_json_value_t *v = cc_json_object_get(cli, "debug_mode");
        if (v) out_config->debug_mode = json_boolish_value(v);
        v = cc_json_object_get(cli, "debug");
        if (v) out_config->debug_mode = json_boolish_value(v);
    }



    if (!parse_runtime_sections(out_config, root)) goto oom;
    cc_result_t validation_rc = cc_config_validate(out_config);
    if (validation_rc.code != CC_OK) {
        cc_json_destroy(root);
        return validation_rc;
    }

    if (out_config->thinking_mode) {
        out_config->stream_mode = 1;
    }

    cc_json_destroy(root);
    return cc_result_ok();

oom:
    cc_json_destroy(root);
    return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate config field");
}


/*
 * 销毁配置对象。
 *
 * config 拥有大量嵌套字符串、数组和子结构；集中释放可以避免调用方理解每个 section 的
 * 内部布局。最后 memset 清零，降低重复 destroy 的风险。
 */
void cc_config_destroy(cc_config_t *config)
{
    if (!config) return;
    free(config->provider);
    free(config->model);
    free(config->model_extra_body_json);
    free(config->base_url);
    free(config->api_key);
    free(config->api_key_ref);
    free(config->storage_type);
    free(config->data_dir);
    free(config->storage_path);
    free(config->session_media_dir);
    free(config->workspace_path);
    free(config->sandbox_type);
    free(config->memory_backend);
    free(config->memory_path);
    free(config->active_memory_category);
    free(config->system_prompt);
    free(config->soul_file);
    free(config->user_file);
    for (size_t i = 0; i < config->enabled_tools_count; i++) {
        free(config->enabled_tools[i]);
    }
    free(config->enabled_tools);
    for (size_t i = 0; i < config->plugin_count; i++) {
        free(config->plugin_commands[i]);
    }
    free(config->plugin_commands);
    free_agent_profile(&config->agents.defaults);
    for (size_t i = 0; i < config->agents.profile_count; i++) {
        free_agent_profile(&config->agents.profiles[i]);
    }
    free(config->agents.profiles);
    free(config->queue.mode);
    free_string_list(&config->tools.enabled);
    free_string_list(&config->tools.network_allowlist);
    for (size_t i = 0; i < config->tools.policy_count; i++) {
        free(config->tools.policies[i].name);
    }
    free(config->tools.policies);
    for (size_t i = 0; i < config->plugins.entry_count; i++) {
        free_plugin_entry(&config->plugins.entries[i]);
    }
    free(config->plugins.entries);
    free_string_list(&config->skills.extra_dirs);
    for (size_t i = 0; i < config->mcp.server_count; i++) {
        free_mcp_server(&config->mcp.servers[i]);
    }
    free(config->mcp.servers);
    free_string_list(&config->multimodal.limits.allowed_mime_prefixes);
    memset(config, 0, sizeof(cc_config_t));
}


/*
 * 构建 system prompt。
 *
 * 优先使用 config->system_prompt；否则读取 soul/user 文件并拼接；仍为空时回退到内置
 * 默认提示词。返回字符串由调用方 free()。
 */
cc_result_t cc_config_build_system_prompt(const cc_config_t *config, char **out_prompt)
{
    if (!config || !out_prompt) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid system prompt build request");
    }
    *out_prompt = NULL;



    if (config->system_prompt && strlen(config->system_prompt) > 0) {
        *out_prompt = cc_copy_string(config->system_prompt);
        return *out_prompt ? cc_result_ok() :
            cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy system prompt");
    }



    cc_string_builder_t sb;
    cc_result_t builder_rc = cc_string_builder_init(&sb);
    if (builder_rc.code != CC_OK) return builder_rc;
    cc_result_free(&builder_rc);

    char *soul_content = read_text_file(config->soul_file);
    if (soul_content && strlen(soul_content) > 0) {
        cc_string_builder_append(&sb, soul_content);
        cc_string_builder_append(&sb, "\n\n");
    }
    free(soul_content);

    char *user_content = read_text_file(config->user_file);
    if (user_content && strlen(user_content) > 0) {
        cc_string_builder_append(&sb, user_content);
        cc_string_builder_append(&sb, "\n\n");
    }
    free(user_content);



    cc_error_code_t builder_error = cc_string_builder_error(&sb);
    char *combined = cc_string_builder_take(&sb);
    if (builder_error != CC_OK) {
        return cc_result_error(builder_error, "Failed to build system prompt");
    }
    if (combined && strlen(combined) > 0) {
        *out_prompt = combined;
        return cc_result_ok();
    }
    free(combined);



    *out_prompt = cc_copy_string("You are a helpful AI assistant. Use tools to help the user.");
    return *out_prompt ? cc_result_ok() :
        cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate default system prompt");
}
