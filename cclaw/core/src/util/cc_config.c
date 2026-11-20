



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
#include <math.h>

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
#elif CC_STORAGE_JSON_SEGMENTED
#define CC_DEFAULT_STORAGE_TYPE "json_segmented"
#elif CC_STORAGE_JSON_FILE
#define CC_DEFAULT_STORAGE_TYPE "json"
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


/**
 * @brief 从默认文件系统读取整个文本，供配置文件和提示词来源使用。
 * @param[in] path 借用的路径，NULL 是否可读由文件系统端口决定。
 * @return 拥有的 NUL 结尾正文由调用者 free，无法取得读取能力或读取失败时通常为 NULL，空文件可返回空字符串。
 * @note 临时文件系统句柄在读取后销毁，读取错误消息在本函数消费。
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


/**
 * @brief 复制新字符串后替换配置字段，分配失败保留旧值。
 * @param[in,out] field 必需的拥有字符串槽，旧字符串可为 NULL。
 * @param[in] value 借用正文，可指向旧值，NULL 表示跳过。
 * @return 成功或跳过为一，复制失败为零。
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

/**
 * @brief 拼接根路径和叶子名，成功后替换并释放旧路径。
 * @param[in,out] field 必需的拥有字符串槽，失败保留旧值。
 * @param[in] root 借用根路径，末尾没有斜线或反斜线时补斜线，NULL 跳过。
 * @param[in] leaf 借用叶子名，NULL 跳过。
 * @return 成功或跳过为一，长度溢出或分配失败为零。
 */
static int replace_joined_path(char **field, const char *root, const char *leaf)
{
    if (!root || !leaf) return 1;
    size_t root_len = strlen(root);
    size_t leaf_len = strlen(leaf);
    int has_sep = root_len > 0 &&
        (root[root_len - 1] == '/' || root[root_len - 1] == '\\');
    size_t extra = has_sep ? 1U : 2U;
    if (leaf_len > SIZE_MAX - extra || root_len > SIZE_MAX - leaf_len - extra) return 0;
    char *path = malloc(root_len + leaf_len + extra);
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


/**
 * @brief 读取 JSON 对象的字符串字段。
 * @param[in] obj 借用对象，可为 NULL。
 * @param[in] key 借用字段名，可为 NULL。
 * @return 字符串节点内借用指针，空参数、字段缺失或类型不符为 NULL，寿命沿 AST。
 */
static const char *json_string_field(cc_json_value_t *obj, const char *key)
{
    if (!obj || !key) return NULL;
    return cc_json_string_value(cc_json_object_get(obj, key));
}


/**
 * @brief 将兼容形式的 JSON 配置值转换为布尔值。
 * @param[in] value 可选借用节点，字符串仅识别小写 true、1、on、yes，其余沿 bool/int getter。
 * @return 命中真字符串、JSON true 或非零整数视图为一，其余为零。
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

/**
 * @brief 解析小写日志级别名称或节点的整数视图。
 * @param[in] value 可选借用节点，非字符串节点交 int getter。
 * @param[in] fallback 节点缺失或字符串名称未知时采用的值。
 * @return 已识别名称对应的 trace 到 fatal 数值、整数视图或 fallback，不额外限制整数范围。
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

/**
 * @brief 覆盖 network.http 的超时、重试、日志级别和 trace 标志。
 * @param[in,out] config 必需的已初始化配置，原位覆盖存在的标量。
 * @param[in] root 借用 JSON 根，缺少有效 http 对象时跳过。
 * @post 存在字段已按兼容别名和 getter 规则转换，缺失字段保留原值，无资源交接。
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

/**
 * @brief 释放配置列表拥有的字符串及数组并清零。
 * @param[in,out] list 可选列表，count 个槽必须可读且各字符串可独立 free。
 * @post 非 NULL 列表归零，可再次清理，所有旧借用失效。
 */
static void free_string_list(cc_config_string_list_t *list)
{
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

/**
 * @brief 向配置列表追加一个拥有的字符串副本。
 * @param[in,out] list 可选有效列表，NULL 时跳过，失败保留已计数内容，数组地址可能改变。
 * @param[in] value 借用正文，NULL 时跳过。
 * @return 成功或跳过为一，长度溢出、扩容或复制失败为零。
 */
static int append_string_to_list(cc_config_string_list_t *list, const char *value)
{
    if (!list || !value) return 1;
    if (list->count >= SIZE_MAX / sizeof(char *)) return 0;
    char **next = realloc(list->items, (list->count + 1) * sizeof(char *));
    if (!next) return 0;
    list->items = next;
    list->items[list->count] = cc_copy_string(value);
    if (!list->items[list->count]) return 0;
    list->count++;
    return 1;
}

/**
 * @brief 清理旧列表后复制 JSON 数组中的字符串项。
 * @param[in] array 可选借用数组，缺失或非数组得到空列表，非字符串项忽略。
 * @param[in,out] out 必需的有效拥有列表，复制失败保留可清理的部分结果。
 * @return 完成或空数组为一，空输出、扩容或复制失败为零。
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

/**
 * @brief 释放 agent profile 的全部拥有字段并清零。
 * @param[in,out] profile 可选有效 profile，允许部分初始化。
 * @post 原字符串与技能列表已释放，非 NULL 结构归零。
 */
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

/**
 * @brief 释放 plugin 的字符串、参数、工具声明及技能目录。
 * @param[in,out] entry 可选有效条目，已计数槽可部分初始化。
 * @post 非 NULL 条目的拥有资源全部释放并归零。
 */
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

/**
 * @brief 释放 MCP server 的拥有字符串与参数列表。
 * @param[in,out] server 可选有效条目，允许部分初始化。
 * @post 非 NULL 条目归零，原借用指针失效。
 */
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

/**
 * @brief 将存在的 JSON 字符串字段复制到配置槽。
 * @param[in] obj 可选借用对象。
 * @param[in] key 借用字段名，NULL 视为缺失。
 * @param[in,out] out 有字符串字段时必需的拥有槽，复制成功后释放旧值。
 * @return 缺失、类型不符或替换成功为一，复制失败为零。
 */
static int copy_json_string_field(cc_json_value_t *obj, const char *key, char **out)
{
    const char *value = json_string_field(obj, key);
    return value ? replace_string(out, value) : 1;
}

/**
 * @brief 清理旧参数数组后深拷贝 JSON 数组中的字符串。
 * @param[in] array 可选借用节点，缺失或非数组得到空参数列表。
 * @param[in,out] out_args 必需的有效拥有数组槽，进入后释放旧值并置 NULL。
 * @param[in,out] out_count 必需的旧数组条数，进入后置零，成功写有效字符串数。
 * @return 完成为一，空输出或分配失败为零，失败清理新部分结果并保留空输出。
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

/**
 * @brief 覆盖单个 agent profile，并重建其技能列表。
 * @param[in] obj 可选借用对象，非对象时仅应用缺省 ID。
 * @param[in] fallback_id 可选借用缺省 ID，只在 out 尚无 ID 时复制。
 * @param[in,out] out 必需的有效 profile，字段副本归其拥有，失败允许部分覆盖并可清理。
 * @return 完成为一，空输出或分配失败为零，同字段 snake_case 在后读取时覆盖 camelCase。
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

/**
 * @brief 从 tools.enabled 重建独立拥有的 enabled_tools 兼容缓存。
 * @param[in,out] config 必需有效配置，旧缓存先释放，源列表保留。
 * @return 完成为一，分配失败为零，失败时计数覆盖已复制项以便 destroy 清理。
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
        config->enabled_tools_count++;
    }
    return 1;
}


/**
 * @brief 检查所有必需默认字符串均已分配。
 * @param[in] config 必需借用的默认配置，可以部分初始化。
 * @return 所列字段均非 NULL 为 CC_OK，否则为 CC_ERR_OUT_OF_MEMORY，错误消息由调用者释放。
 */
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

/**
 * @brief 将节点的数值视图安全转换为 size_t。
 * @param[in] node 借用节点，类型转换规则沿 number getter。
 * @param[out] out_value 必需输出，有限非负且可表示时写入截断整数，失败保持原值。
 * @return 可安全转换为一，负值、非有限或超出 size_t 表示范围为零。
 */
static int parse_size_value(cc_json_value_t *node, size_t *out_value)
{
    double value = cc_json_number_value(node);
    double exclusive_limit = (double)(SIZE_MAX / 2U + 1U) * 2.0;
    if (!isfinite(value) || value < 0.0 || value >= exclusive_limit) return 0;
    *out_value = (size_t)value;
    return 1;
}

/**
 * @brief 覆盖队列 lane、模式、等待与完成项预算。
 * @param[in,out] config 必需的已初始化配置，缺失字段保留原值，失败可部分覆盖。
 * @param[in] root 借用根节点，缺失或非对象 queue 跳过。
 * @return 完成为一，字符串分配失败为零，不可表示的无符号数值为负一。
 */
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
    if (v && !parse_size_value(v, &config->queue.max_total_tasks)) return -1;
    v = cc_json_object_get(queue, "completedTtlMs");
    if (!v) v = cc_json_object_get(queue, "completed_ttl_ms");
    if (v) {
        double value = cc_json_number_value(v);
        if (!isfinite(value) || value < 0 || value > (double)UINT_MAX) return -1;
        config->queue.completed_ttl_ms = (unsigned)value;
    }
    return 1;
}

/**
 * @brief 覆盖 agents.defaults 并重建存在的 list 数组。
 * @param[in,out] config 必需有效配置，列表槽在解析前计数，失败保留可销毁的部分结果。
 * @param[in] root 借用根节点，缺失 agents 或 list 时保留对应旧值。
 * @return 完成或缺失段为一，分配或 profile 复制失败为零。
 */
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
        cc_config_agent_profile_t *profile = &config->agents.profiles[config->agents.profile_count++];
        if (!parse_agent_profile(
                cc_json_array_get(list, i),
                NULL,
                profile)) {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief 解析工具启用列表、网络允许列表、超时与逐工具策略。
 * @param[in,out] config 必需有效配置，存在 enabled 时重建独立兼容缓存，失败保留可清理状态。
 * @param[in] root 借用根节点，缺失 tools 时跳过，存在 tools 时重建策略数组。
 * @return 完成为一，数组、字符串或兼容缓存分配失败为零。
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

/**
 * @brief 复制 plugin 工具声明，将参数 schema 序列化为拥有正文。
 * @pre entry 的 tools 数组和计数为空。
 * @param[in] array 可选借用数组，缺失或非数组时跳过。
 * @param[in,out] entry 必需条目，解析前计数新槽，失败部分字段由 free_plugin_entry 清理。
 * @return 完成为一，缺少名称或描述、分配或现有 schema 序列化失败为零。
 */
static int parse_plugin_tools(cc_json_value_t *array, cc_config_plugin_entry_t *entry)
{
    if (!array || !cc_json_is_array(array)) return 1;
    int count = cc_json_array_size(array);
    entry->tools = count > 0 ? calloc((size_t)count, sizeof(cc_config_plugin_tool_t)) : NULL;
    if (count > 0 && !entry->tools) return 0;
    for (int i = 0; i < count; i++) {
        cc_json_value_t *tool = cc_json_array_get(array, i);
        cc_config_plugin_tool_t *out = &entry->tools[entry->tool_count++];
        const char *s = json_string_field(tool, "name");
        out->name = s ? cc_copy_string(s) : NULL;
        s = json_string_field(tool, "description");
        out->description = s ? cc_copy_string(s) : NULL;
        cc_json_value_t *params = cc_json_object_get(tool, "parameters");
        out->parameters_json = params ? cc_json_stringify_unformatted(params) :
            cc_copy_string("{\"type\":\"object\",\"properties\":{}}");
        if (!out->name || !out->description || !out->parameters_json) return 0;
    }
    return 1;
}

/**
 * @brief 覆盖插件热重载设置并重建按对象键命名的 entries。
 * @param[in,out] config 必需有效配置，新条目先计数再解析，失败可由 destroy 清理。
 * @param[in] root 借用根节点，缺失 plugins 时跳过，缺失 entries 时清空旧集合。
 * @return 完成为一，字符串、参数、工具或技能目录复制失败为零。
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
        cc_config_plugin_entry_t *entry = &config->plugins.entries[config->plugins.entry_count++];
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
    }
    return 1;
}

/**
 * @brief 覆盖 skills.load 的监听标志、去抖毫秒和额外目录。
 * @param[in,out] config 必需有效配置，存在 extraDirs 时替换旧列表。
 * @param[in] root 借用根节点，缺失有效 load 时跳过。
 * @return 完成为一，目录数组复制失败为零，失败保留可清理的部分列表。
 */
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

/**
 * @brief 覆盖 MCP 标志与空闲期限并重建命名 server 集合。
 * @param[in,out] config 必需有效配置，新 server 先计数，字段副本归配置拥有。
 * @param[in] root 借用根节点，缺失 mcp 时跳过，存在 mcp 时缺失 servers 清空旧集合。
 * @return 完成为一，数组或字段复制失败为零，部分结果可由 destroy 清理。
 */
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
        cc_config_mcp_server_t *server = &config->mcp.servers[config->mcp.server_count++];
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
    }
    return 1;
}

/**
 * @brief 按存在字段覆盖 image、audio、video 和 file 标志。
 * @param[in] obj 借用对象，NULL 或非对象时跳过。
 * @param[in,out] flags 可选标量输出，NULL 时跳过，字段按 bool-like 规则读取。
 * @post 存在的标志已覆盖，缺失字段保留原值，无资源分配。
 */
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

/**
 * @brief 覆盖多模态输入输出标志、字节预算和 MIME 前缀列表。
 * @param[in,out] config 必需有效配置，分配失败保留可销毁的部分覆盖状态。
 * @param[in] root 借用根节点，缺失有效段或字段时保留原值。
 * @return 完成为一，列表分配失败为零，不可表示的 size_t 预算为负一。
 */
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
    if (v && !parse_size_value(v, &config->multimodal.limits.max_artifacts)) return -1;
    v = cc_json_object_get(limits, "max_artifact_bytes");
    if (!v) v = cc_json_object_get(limits, "maxArtifactBytes");
    if (v && !parse_size_value(v, &config->multimodal.limits.max_artifact_bytes)) return -1;
    v = cc_json_object_get(limits, "max_base64_bytes");
    if (!v) v = cc_json_object_get(limits, "maxBase64Bytes");
    if (v && !parse_size_value(v, &config->multimodal.limits.max_base64_bytes)) return -1;
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

/**
 * @brief 按队列、agent、工具、插件、技能、MCP 和多模态顺序覆盖扩展段。
 * @param[in,out] config 必需有效配置，失败保留已覆盖字段，调用者负责 destroy。
 * @param[in] root 借用根节点，各段沿自身的缺失字段兼容规则。
 * @return 完成为一，分配或必需插件字段缺失为零，无符号预算不可表示为负一。
 */
static int parse_runtime_sections(cc_config_t *config, cc_json_value_t *root)
{
    int queue_status = parse_queue_section(config, root);
    if (queue_status != 1) return queue_status;
    if (!(parse_agents_section(config, root) &&
           parse_tools_section(config, root) &&
           parse_plugins_section(config, root) &&
           parse_skills_section(config, root) &&
           parse_mcp_section(config, root))) return 0;
    return parse_multimodal_section(config, root);
}

/**
 * @brief 判断可选字符串是否至少含一个非 NUL 字节。
 * @param[in] s 可选借用 NUL 结尾字符串。
 * @return 非 NULL 且首字节非零为一，其余为零。
 */
static int str_nonempty(const char *s)
{
    return s && s[0] != '\0';
}

/**
 * @brief 判断 MCP 传输名是否属于配置层认识的集合。
 * @param[in] transport 可选借用名称，NULL 使用 stdio。
 * @return stdio、http、sse 或 streamable_http 为一，其余为零，实际可用性仍由适配器决定。
 */
static int mcp_transport_known(const char *transport)
{
    const char *t = transport ? transport : "stdio";
    return strcmp(t, "stdio") == 0 ||
           strcmp(t, "http") == 0 ||
           strcmp(t, "sse") == 0 ||
           strcmp(t, "streamable_http") == 0;
}

/**
 * @brief 核对运行预算、上下文、队列、插件、MCP 与分段存储的已实现约束。
 * @param[in] config 借用有效配置，NULL 返回参数错误，数组计数与指针须一致。
 * @return 已检查项满足约束为 CC_OK，否则为带拥有错误消息的 CC_ERR_INVALID_ARGUMENT。
 * @note 不创建运行资源，错误诊断自身可以分配内存，未列入检查的字段沿下游契约。
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
        !isfinite(config->context_compress_threshold) ||
        config->context_compress_threshold < 0.0 ||
        config->context_compress_threshold > 1.0 ||
        !isfinite(config->summary_temperature) ||
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

/**
 * @brief 核对配置已实现的语义约束及多模态编译能力。
 * @param[in] config 借用配置，NULL 返回参数错误，校验期间由拥有者保持稳定。
 * @return 通过为 CC_OK，标量或必需字段非法为 CC_ERR_INVALID_ARGUMENT，媒体能力未编译为 CC_ERR_UNSUPPORTED。
 * @note 不修改配置，不验证网络连通性、路径可读性或所有 provider 与工具是否已装配。
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


/**
 * @brief 按编译配置构造独立拥有的默认配置。
 * @param[out] out_config 必需且无已有资源，进入后清零，成功字段归调用者 destroy。
 * @return 成功为 CC_OK，空输出为 CC_ERR_INVALID_ARGUMENT，必需默认字符串分配失败为 CC_ERR_OUT_OF_MEMORY 并清零输出。
 * @note 本函数直接创建默认值，不读取文件或环境变量，也不执行完整 validate。
 */
cc_result_t cc_config_load_default(cc_config_t *out_config)
{
    if (!out_config) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null config output");
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


/**
 * @brief 先构造默认配置，再读取 JSON 文件逐段覆盖并校验。
 * @param[in] path 可选借用路径，NULL 时仅构造默认值，非 NULL 使用默认文件系统读取。
 * @param[out] out_config 必需且无已有资源，失败也可能拥有默认或部分覆盖字段，均由调用者 destroy。
 * @return 默认构造、文件读取、JSON、密钥引用及校验错误原样返回，字段复制失败为 CC_ERR_OUT_OF_MEMORY，不可表示的无符号预算为 CC_ERR_INVALID_ARGUMENT。
 * @note 文件使用普通 JSON 入口的首值尾部兼容，字符串深拷贝。api_key_env 覆盖文件明文，api_key_ref 随后解析。并发读取环境和修改文件由外层协调。
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
        if (v && !parse_size_value(v, &out_config->session_segment_bytes)) goto invalid_size;
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
        if (v) out_config->max_steps = cc_json_int_value(v);

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



    int section_status = parse_runtime_sections(out_config, root);
    if (section_status < 0) goto invalid_size;
    if (!section_status) goto oom;
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

invalid_size:
    cc_json_destroy(root);
    return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Config unsigned size is out of range");
}


/**
 * @brief 释放配置全部拥有字段并清零，适用于加载成功和部分失败。
 * @param[in,out] config 可选有效配置，计数须覆盖可释放数组槽，字段需独立拥有。
 * @post 非 NULL 配置归零，可再次 destroy，旧字段借用全部失效。
 * @pre 外层已排空配置及字段的并发读者，不与 load、builder 读取等重叠。
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


/**
 * @brief 复制显式提示词，或拼接可读 soul/user 文件，再按需采用内置提示词。
 * @param[in] config 必需借用配置，字段在同步读取期间保持稳定。
 * @param[out] out_prompt 必需空资源输出，基础参数合法后置 NULL，成功正文由调用者 free。
 * @return 成功为 CC_OK，空参数为 CC_ERR_INVALID_ARGUMENT，复制、builder 或默认正文分配错误返回对应错误。
 * @note 文件读取错误在 helper 消费，允许继续其他来源，append 失败回收缓冲并传回拥有错误结果。
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
        builder_rc = cc_string_builder_append(&sb, soul_content);
        if (builder_rc.code == CC_OK) builder_rc = cc_string_builder_append(&sb, "\n\n");
    }
    free(soul_content);
    if (builder_rc.code != CC_OK) {
        cc_string_builder_deinit(&sb);
        return builder_rc;
    }

    char *user_content = read_text_file(config->user_file);
    if (user_content && strlen(user_content) > 0) {
        builder_rc = cc_string_builder_append(&sb, user_content);
        if (builder_rc.code == CC_OK) builder_rc = cc_string_builder_append(&sb, "\n\n");
    }
    free(user_content);
    if (builder_rc.code != CC_OK) {
        cc_string_builder_deinit(&sb);
        return builder_rc;
    }



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
