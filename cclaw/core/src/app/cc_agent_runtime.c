



#include "cc_agent_runtime_internal.h"
#include "cc/app/cc_context_builder.h"
#include "cc/app/cc_tool_executor.h"
#include "cc/core/cc_media.h"
#include "cc/core/cc_id.h"
#include "cc/core/cc_observability.h"
#include "cc/util/cc_string_builder.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_platform.h"
#include "cc/ports/cc_thread.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <limits.h>
#include <stdint.h>

/* 释放配置里的字符串列表；runtime 创建时会深拷贝，销毁时必须成对清理。 */
static void runtime_string_list_cleanup(cc_config_string_list_t *list)
{
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

/* 深拷贝字符串列表，保证 runtime 生命周期不依赖外部 config 内存。 */
static cc_result_t runtime_string_list_copy(
    const cc_config_string_list_t *src,
    cc_config_string_list_t *dst
)
{
    if (!dst) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null string list copy output");
    }
    memset(dst, 0, sizeof(*dst));
    if (!src || src->count == 0) return cc_result_ok();

    dst->items = calloc(src->count, sizeof(char *));
    if (!dst->items) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy string list");
    }
    dst->count = src->count;
    for (size_t i = 0; i < src->count; i++) {
        if (!src->items[i]) continue;
        dst->items[i] = cc_copy_string(src->items[i]);
        if (!dst->items[i]) {
            runtime_string_list_cleanup(dst);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy string list item");
        }
    }
    return cc_result_ok();
}

/*
 * 安全复制可选字符串：若 src 为 NULL 则设 *dst = NULL，否则深拷贝 src 到 *dst。
 * 用于运行时配置字段的零拷贝传递，避免悬空指针。
 * 参数: src        - 源字符串（可为 NULL）
 *       dst        - 目标指针的指针
 *       field_name - 字段名（仅用于错误消息）
 * 返回: cc_result_t，内存不足时返回 CC_ERR_OUT_OF_MEMORY
 */
static cc_result_t runtime_copy_optional_string(
    const char *src,
    char **dst,
    const char *field_name
)
{
    if (!dst) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null string copy output");
    }
    *dst = NULL;
    if (!src) return cc_result_ok();
    *dst = cc_copy_string(src);
    if (!*dst) {
        return cc_result_errf(
            CC_ERR_OUT_OF_MEMORY,
            "Failed to copy runtime config field: %s",
            field_name ? field_name : "(unknown)");
    }
    return cc_result_ok();
}

/* 把配置层的多模态限制转换成 provider 请求使用的只读 limits 视图。 */
static void runtime_media_limits_from_config(
    const cc_multimodal_config_t *config,
    cc_media_limits_t *out_limits
)
{
    if (!out_limits) return;
    memset(out_limits, 0, sizeof(*out_limits));
    if (!config) {
        cc_media_limits_text_only(out_limits);
        return;
    }
    out_limits->max_artifacts = config->limits.max_artifacts;
    out_limits->max_artifact_bytes = config->limits.max_artifact_bytes;
    out_limits->max_base64_bytes = config->limits.max_base64_bytes;
    out_limits->allow_inline_base64 = config->limits.allow_inline_base64;
    out_limits->allowed_mime_prefixes =
        (const char **)config->limits.allowed_mime_prefixes.items;
    out_limits->allowed_mime_prefix_count =
        config->limits.allowed_mime_prefixes.count;
}

/*
 * Runtime 业务路径统一从这里发布观测事件。
 *
 * event bus 是底层传输，runtime 不再直接拼 payload 调底层发布函数。
 * attributes_json 必须是 object 文本；调用方只在需要补充事件私有字段时传入。
 */
static void runtime_publish_observability(
    cc_agent_runtime_t *runtime,
    const char *event_name,
    const char *session_id,
    int step,
    const char *status,
    const char *message,
    const cc_result_t *error,
    const char *attributes_json
)
{
    if (!runtime || !runtime->event_bus || !event_name) return;

    cc_observability_event_t event;
    memset(&event, 0, sizeof(event));
    event.size = sizeof(event);
    event.event = event_name;
    event.session_id = session_id;
    event.step = step;
    event.status = status ? status : "";
    event.message = message;
    event.error = error;
    event.attributes_json = attributes_json;

    cc_result_t rc = cc_observability_publish(runtime->event_bus, &event);
    cc_result_free(&rc);
}

/*
 * Stream 文本类事件都带 content attribute。这样 text/thinking/delta/error 在
 * 下游保持同一个读取位置，不再依赖旧版裸字符串 payload。
 */
/* Runtime-level LLM stage events stay platform neutral; firmware resource data is added in main. */
static long runtime_clock_ms(void)
{
    uint64_t now_ms = cc_platform_monotonic_ms();
    if (now_ms > 0) {
        return now_ms > (uint64_t)LONG_MAX ? LONG_MAX : (long)now_ms;
    }
    time_t now = time(NULL);
    return now < 0 ? 0 : (long)now * 1000L;
}

/*
 * 发布 LLM 处理阶段的可观测事件：将耗时、输入/输出计数等阶段指标打包为 JSON 属性，
 * 通过 runtime_publish_observability 发送到事件总线，供监控和调试使用。
 * 参数: runtime             - Agent 运行时实例
 *       event_name          - 事件名称
 *       session_id          - 会话 ID
 *       step                - 处理步骤序号
 *       status              - 阶段状态
 *       elapsed_ms          - 阶段耗时（毫秒，负值视为0）
 *       primary_count       - 主计数（如 token 数）
 *       primary_count_name  - 主计数字段名
 *       secondary_count     - 次计数
 *       secondary_count_name- 次计数字段名
 *       error               - 错误信息（可为 NULL）
 * 返回: 无
 */
static void runtime_publish_llm_stage(
    cc_agent_runtime_t *runtime,
    const char *event_name,
    const char *session_id,
    int step,
    const char *status,
    long elapsed_ms,
    size_t primary_count,
    const char *primary_count_name,
    size_t secondary_count,
    const char *secondary_count_name,
    const cc_result_t *error
)
{
    if (!runtime || !runtime->event_bus || !event_name) return;

    cc_json_value_t *attrs = cc_json_create_object();
    if (!attrs) {
        runtime_publish_observability(runtime, event_name, session_id, step,
            status, error ? error->message : NULL, error, NULL);
        return;
    }
    cc_json_object_set(attrs, "elapsed_ms",
        cc_json_create_number((double)(elapsed_ms < 0 ? 0 : elapsed_ms)));
    if (primary_count_name) {
        cc_json_object_set(attrs, primary_count_name,
            cc_json_create_number((double)primary_count));
    }
    if (secondary_count_name) {
        cc_json_object_set(attrs, secondary_count_name,
            cc_json_create_number((double)secondary_count));
    }
    char *attrs_json = cc_json_stringify_unformatted(attrs);
    cc_json_destroy(attrs);
    runtime_publish_observability(runtime, event_name, session_id, step,
        status, error ? error->message : NULL, error, attrs_json);
    free(attrs_json);
}

/*
 * 构建 LLM 请求的可观测属性 JSON：提取模型名、流式标志、思考模式、消息数、
 * 工具 JSON 字节数及 extra_body 键名等，用于请求阶段的可观测事件上报。
 * 参数: runtime          - Agent 运行时实例
 *       request          - LLM 聊天请求
 *       tools_json_bytes - 工具定义的 JSON 字节数
 * 返回: JSON 字符串（调用方需 free），失败返回 NULL
 */
static char *runtime_build_request_config_attributes(
    cc_agent_runtime_t *runtime,
    const cc_llm_chat_request_t *request,
    size_t tools_json_bytes
)
{
    (void)runtime;
    cc_json_value_t *attrs = cc_json_create_object();
    if (!attrs) return NULL;

    cc_json_object_set(attrs, "model",
        cc_json_create_string(request && request->model ? request->model : ""));
    cc_json_object_set(attrs, "stream",
        cc_json_create_bool(request && request->stream != 0));
    cc_json_object_set(attrs, "thinking_mode",
        cc_json_create_bool(request && request->thinking_mode != 0));
    cc_json_object_set(attrs, "message_count",
        cc_json_create_number(request ? (double)request->message_count : 0.0));
    cc_json_object_set(attrs, "tools_json_bytes",
        cc_json_create_number((double)tools_json_bytes));

    const char *extra_body_json = request ? request->extra_body_json : NULL;
    cc_json_object_set(attrs, "has_extra_body",
        cc_json_create_bool(extra_body_json && extra_body_json[0]));

    cc_json_value_t *keys = cc_json_create_array();
    int extra_body_parse_ok = 0;
    int has_enable_thinking = 0;
    if (extra_body_json && extra_body_json[0]) {
        cc_json_value_t *extra = NULL;
        cc_result_t rc = cc_json_parse(extra_body_json, &extra);
        if (rc.code == CC_OK && extra && cc_json_is_object(extra)) {
            extra_body_parse_ok = 1;
            int count = cc_json_object_size(extra);
            for (int i = 0; i < count; i++) {
                const char *key = cc_json_object_key_at(extra, i);
                if (!key) continue;
                cc_json_array_append(keys, cc_json_create_string(key));
                if (strcmp(key, "enable_thinking") == 0) {
                    has_enable_thinking = 1;
                }
            }
        }
        if (extra) cc_json_destroy(extra);
        cc_result_free(&rc);
    } else {
        extra_body_parse_ok = 1;
    }
    cc_json_object_set(attrs, "extra_body_parse_ok",
        cc_json_create_bool(extra_body_parse_ok));
    cc_json_object_set(attrs, "extra_body_keys", keys);
    cc_json_object_set(attrs, "has_enable_thinking",
        cc_json_create_bool(has_enable_thinking));

    char *attrs_json = cc_json_stringify_unformatted(attrs);
    cc_json_destroy(attrs);
    return attrs_json;
}

/*
 * 发布流式内容片段到可观测事件总线：将流式文本内容包装为 JSON 属性后发布，
 * 用于实时追踪 LLM 流式输出的逐块内容。
 * 参数: runtime    - Agent 运行时实例
 *       event_name - 事件名称
 *       session_id - 会话 ID
 *       step       - 处理步骤序号
 *       status     - 事件状态
 *       content    - 流式内容文本
 * 返回: 无
 */
static void runtime_publish_stream_content(
    cc_agent_runtime_t *runtime,
    const char *event_name,
    const char *session_id,
    int step,
    const char *status,
    const char *content
)
{
    if (!runtime || !runtime->event_bus || !event_name) return;

    cc_json_value_t *attrs = cc_json_create_object();
    if (!attrs) return;
    cc_json_object_set(attrs, "content",
        cc_json_create_string(content ? content : ""));
    char *attrs_json = cc_json_stringify_unformatted(attrs);
    cc_json_destroy(attrs);
    if (!attrs_json) return;

    runtime_publish_observability(runtime, event_name, session_id, step,
        status, content, NULL, attrs_json);
    free(attrs_json);
}


/* 生成 CSPRNG UUID；熵源失败必须中止持久化步骤，不能退回时间或计数器。 */
static cc_result_t generate_id(char **out_id)
{
    return cc_id_generate_uuid_v4("msg_", out_id);
}


/*
 * 主动记忆写入钩子。
 *
 * 它只在最终 assistant 回复完成后写入，不在 stream partial、取消或错误路径写入，
 * 避免把不完整响应固化到长期记忆。
 */
static void active_memory_after_run(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    const char *assistant_text
)
{
#if CC_ENABLE_ACTIVE_MEMORY
    if (!runtime || !runtime->memory_store || !runtime->memory_store->vtable) return;
    if (!runtime->config.active_memory_enabled ||
        !runtime->config.active_memory_write_summary) return;
    if ((!user_input || !user_input[0]) && (!assistant_text || !assistant_text[0])) return;

    cc_string_builder_t sb;
    if (cc_string_builder_init(&sb).code != CC_OK) return;
    cc_string_builder_append(&sb, "User: ");
    cc_string_builder_append(&sb, user_input ? user_input : "");
    cc_string_builder_append(&sb, "\nAssistant: ");
    cc_string_builder_append(&sb, assistant_text ? assistant_text : "");
    char *value = cc_string_builder_take(&sb);
    if (!value) {
        cc_string_builder_deinit(&sb);
        return;
    }

    int max_chars = runtime->config.active_memory_max_value_chars > 0 ?
        runtime->config.active_memory_max_value_chars : 1600;
    if ((int)strlen(value) > max_chars) value[max_chars] = '\0';

    char *id = NULL;
    cc_result_t id_rc = generate_id(&id);
    if (id_rc.code != CC_OK) {
        cc_result_free(&id_rc);
        free(value);
        return;
    }
    cc_result_free(&id_rc);
    size_t key_len = strlen("active.") + strlen(id) + 1;
    char *key = malloc(key_len);
    if (!key) {
        free(id);
        free(value);
        return;
    }
    snprintf(key, key_len, "active.%s", id);
    free(id);

    cc_result_t rc = cc_memory_store_set(
        runtime->memory_store,
        key,
        value,
        runtime->config.active_memory_category ?
            runtime->config.active_memory_category : "active_summary",
        session_id
    );
    cc_result_free(&rc);
    free(key);
    free(value);
#else
    (void)runtime;
    (void)session_id;
    (void)user_input;
    (void)assistant_text;
#endif
}

/* 从同步 run options 中取取消 token；NULL 表示调用方没有启用取消。 */
static cc_cancel_token_t *run_cancel_token(const cc_agent_runtime_run_options_t *options)
{
    return options ? options->cancel_token : NULL;
}

static cc_result_t runtime_enter_run(cc_agent_runtime_t *runtime)
{
    if (!runtime) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null runtime");
    cc_mutex_lock(runtime->mutex);
    int limit = runtime->config.limits.max_concurrency > 0 ?
        runtime->config.limits.max_concurrency : 1;
    if (runtime->active_runs >= limit) {
        cc_mutex_unlock(runtime->mutex);
        return cc_result_error(CC_ERR_QUEUE_FULL,
                               "Runtime concurrency limit reached");
    }
    runtime->active_runs++;
    cc_mutex_unlock(runtime->mutex);
    return cc_result_ok();
}

static void runtime_leave_run(cc_agent_runtime_t *runtime)
{
    if (!runtime) return;
    cc_mutex_lock(runtime->mutex);
    if (runtime->active_runs > 0) runtime->active_runs--;
    cc_mutex_unlock(runtime->mutex);
}

/* 统一把取消状态映射成 CC_ERR_CANCELLED，便于上层做恢复或 UI 状态更新。 */
static cc_result_t check_run_cancelled(const cc_agent_runtime_run_options_t *options, const char *message)
{
    if (cc_cancel_token_is_cancelled(run_cancel_token(options))) {
        return cc_result_error(CC_ERR_CANCELLED, message ? message : "Agent run cancelled");
    }
    return cc_result_ok();
}

static uint64_t runtime_run_deadline(const cc_agent_runtime_t *runtime)
{
    if (!runtime || runtime->config.limits.run_timeout_ms <= 0) return 0;
    uint64_t now_ms = cc_platform_monotonic_ms();
    uint64_t timeout_ms = (uint64_t)runtime->config.limits.run_timeout_ms;
    return now_ms > UINT64_MAX - timeout_ms ? UINT64_MAX : now_ms + timeout_ms;
}

static cc_result_t check_run_state(
    cc_cancel_token_t *cancel_token,
    uint64_t deadline_ms,
    const char *cancel_message)
{
    if (cc_cancel_token_is_cancelled(cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED,
            cancel_message ? cancel_message : "Agent run cancelled");
    }
    if (deadline_ms > 0 && cc_platform_monotonic_ms() >= deadline_ms) {
        return cc_result_error(CC_ERR_TIMEOUT, "Agent run deadline exceeded");
    }
    return cc_result_ok();
}

static uint64_t provider_deadline(
    const cc_agent_runtime_t *runtime,
    uint64_t run_deadline_ms)
{
    uint64_t deadline_ms = run_deadline_ms;
    if (runtime && runtime->config.limits.provider_timeout_ms > 0) {
        uint64_t now_ms = cc_platform_monotonic_ms();
        uint64_t timeout_ms = (uint64_t)runtime->config.limits.provider_timeout_ms;
        uint64_t provider_end = now_ms > UINT64_MAX - timeout_ms ?
            UINT64_MAX : now_ms + timeout_ms;
        if (deadline_ms == 0 || provider_end < deadline_ms) deadline_ms = provider_end;
    }
    return deadline_ms;
}

/* stream options 有独立 token；stream callback API 可以不依赖普通 run options。 */
static cc_cancel_token_t *stream_cancel_token(const cc_agent_runtime_stream_options_t *options)
{
    return options ? options->cancel_token : NULL;
}

/* max_steps 同时支持旧字段和统一 limits 字段；limits 优先表达新的资源模型。 */
static int runtime_effective_max_steps(const cc_agent_runtime_t *runtime)
{
    if (!runtime) return 1;
    if (runtime->config.limits.max_steps > 0) return runtime->config.limits.max_steps;
    if (runtime->config.max_steps > 0) return runtime->config.max_steps;
    return 1;
}

/* 输入字节限制在写入 session 之前执行，避免超大请求进入历史上下文。 */
static cc_result_t check_input_limit(
    const cc_agent_runtime_t *runtime,
    const char *input
)
{
    size_t max_bytes = runtime ? runtime->config.limits.max_input_bytes : 0;
    if (max_bytes > 0 && input && strlen(input) > max_bytes) {
        return cc_result_errf(
            CC_ERR_LIMIT_EXCEEDED,
            "Input exceeds max_input_bytes (%zu)",
            max_bytes);
    }
    return cc_result_ok();
}

/* 输出字节限制在落库之前执行，超限响应不会写入 assistant final。 */
static cc_result_t check_output_limit(
    const cc_agent_runtime_t *runtime,
    const char *output,
    const char *label
)
{
    size_t max_bytes = runtime ? runtime->config.limits.max_output_bytes : 0;
    if (max_bytes > 0 && output && strlen(output) > max_bytes) {
        return cc_result_errf(
            CC_ERR_LIMIT_EXCEEDED,
            "%s exceeds max_output_bytes (%zu)",
            label ? label : "Output",
            max_bytes);
    }
    return cc_result_ok();
}

/*
 * Runtime 创建阶段的 provider 能力协商。
 *
 * 这里选择 fail-fast：如果配置启用了 tool、stream 或多模态能力，但 provider 不声明支持，
 * runtime 创建直接失败，避免运行时静默降级。
 */
static cc_result_t validate_provider_capabilities(
    const cc_agent_runtime_deps_t *deps,
    const cc_agent_runtime_config_t *config
)
{
    if (!deps || !deps->llm.vtable || !deps->llm.vtable->capabilities) {
        return cc_result_ok();
    }

    cc_llm_provider_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    cc_result_t rc = deps->llm.vtable->capabilities(deps->llm.self, &caps);
    if (rc.code != CC_OK) return rc;

    if (!caps.text_input || !caps.text_output) {
        return cc_result_error(CC_ERR_UNSUPPORTED,
            "Provider must support text input and output");
    }

    if (deps->tool_registry && cc_tool_registry_count(deps->tool_registry) > 0 &&
        !caps.tool_calling) {
        return cc_result_error(CC_ERR_UNSUPPORTED,
            "Configured tools require provider tool-calling support");
    }

    if (!config) return cc_result_ok();
    if (config->multimodal.input.image && !caps.image_input)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Provider does not support image input");
    if (config->multimodal.input.audio && !caps.audio_input)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Provider does not support audio input");
    if (config->multimodal.input.video && !caps.video_input)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Provider does not support video input");
    if (config->multimodal.input.file && !caps.file_input)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Provider does not support file input");
    if (config->multimodal.output.image && !caps.image_output)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Provider does not support image output");
    if (config->multimodal.output.audio && !caps.audio_output)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Provider does not support audio output");
    if (config->multimodal.output.video && !caps.video_output)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Provider does not support video output");
    if (config->multimodal.output.file && !caps.file_output)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Provider does not support file output");

    return cc_result_ok();
}

/*
 * 创建 runtime 并接管一份配置副本。
 *
 * provider/store/registry/event_bus 等端口仍由外部持有；runtime 只保存 vtable/self 引用。
 * 这种依赖注入方式是 C 语言里实现可测试、可移植 SDK 的核心设计。
 */
cc_result_t cc_agent_runtime_create(
    const cc_agent_runtime_deps_t *deps,
    const cc_agent_runtime_options_t *options,
    cc_agent_runtime_t **out_runtime
)
{
    if (!deps || !options || !out_runtime) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null runtime create argument");
    }
    cc_result_t caps_rc = validate_provider_capabilities(deps, &options->config);
    if (caps_rc.code != CC_OK) return caps_rc;

    cc_agent_runtime_config_t config = options->config;
    cc_agent_runtime_t *runtime = calloc(1, sizeof(cc_agent_runtime_t));
    if (!runtime) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create agent runtime");

    cc_result_t mutex_rc = cc_mutex_create(&runtime->mutex);
    if (mutex_rc.code != CC_OK) {
        free(runtime);
        return mutex_rc;
    }


    cc_config_string_list_t allowed_mime_prefixes =
        config.multimodal.limits.allowed_mime_prefixes;
    runtime->config = config;
    runtime->config.system_prompt = NULL;
    runtime->config.workspace_dir = NULL;
    runtime->config.model = NULL;
    runtime->config.model_extra_body_json = NULL;
    runtime->config.active_memory_category = NULL;
    memset(&runtime->config.multimodal.limits.allowed_mime_prefixes, 0,
        sizeof(runtime->config.multimodal.limits.allowed_mime_prefixes));
    cc_result_t copy_rc = runtime_string_list_copy(
        &allowed_mime_prefixes,
        &runtime->config.multimodal.limits.allowed_mime_prefixes);
    if (copy_rc.code != CC_OK) {
        cc_mutex_destroy(runtime->mutex);
        free(runtime);
        return copy_rc;
    }
    copy_rc = runtime_copy_optional_string(
        config.system_prompt, &runtime->config.system_prompt, "system_prompt");
    if (copy_rc.code == CC_OK) {
        copy_rc = runtime_copy_optional_string(
            config.workspace_dir, &runtime->config.workspace_dir, "workspace_dir");
    }
    if (copy_rc.code == CC_OK) {
        copy_rc = runtime_copy_optional_string(
            config.model, &runtime->config.model, "model");
    }
    if (copy_rc.code == CC_OK) {
        copy_rc = runtime_copy_optional_string(
            config.model_extra_body_json,
            &runtime->config.model_extra_body_json,
            "model_extra_body_json");
    }
    if (copy_rc.code == CC_OK) {
        copy_rc = runtime_copy_optional_string(
            config.active_memory_category,
            &runtime->config.active_memory_category,
            "active_memory_category");
    }
    if (copy_rc.code != CC_OK) {
        cc_agent_runtime_destroy(runtime);
        return copy_rc;
    }


    runtime->llm = deps->llm;
    runtime->tool_registry = deps->tool_registry;
    runtime->store = deps->store;
    runtime->policy = deps->policy;
    runtime->sandbox = deps->sandbox;
    runtime->event_bus = deps->event_bus;
    runtime->logger = deps->logger;
    runtime->memory_store = deps->memory_store;
    runtime->tool_pool = deps->tool_pool;
    runtime->thinking_mode = options->thinking_mode;
    runtime->services.event_bus = deps->event_bus;
    runtime->services.logger = deps->logger;
    runtime->services.memory_store = deps->memory_store;
    runtime->services.tool_pool = deps->tool_pool;
    runtime->services.approve_tool_call = deps->approve_tool_call;
    runtime->services.approval_user_data = deps->approval_user_data;

    *out_runtime = runtime;
    return cc_result_ok();
}


static cc_result_t runtime_append_records(
    cc_agent_runtime_t *runtime,
    const cc_session_record_t *records,
    size_t count
)
{
    if (!runtime || !runtime->store.vtable || !runtime->store.vtable->append_records) {
        return cc_result_error(CC_ERR_STORAGE, "Session store does not support atomic records");
    }
    return runtime->store.vtable->append_records(runtime->store.self, records, count);
}

static cc_result_t runtime_append_message(
    cc_agent_runtime_t *runtime,
    const cc_message_t *message
)
{
    cc_session_record_t record;
    memset(&record, 0, sizeof(record));
    record.type = CC_SESSION_RECORD_MESSAGE;
    record.session_id = message ? message->session_id : NULL;
    record.data.message = message;
    return runtime_append_records(runtime, &record, 1);
}

/* 写入最终 assistant 文本；只有完整 final response 才调用，partial stream 不落库。 */
cc_result_t cc_agent_runtime_store_assistant_text(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *text,
    const char *reasoning_content
)
{
    if (!runtime || !session_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null assistant text argument");
    }
    cc_message_t *assistant_msg = NULL;
    char *aid = NULL;
    cc_result_t rc = generate_id(&aid);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    rc = cc_message_create_text(
        aid, session_id, CC_ROLE_ASSISTANT, text ? text : "", NULL, &assistant_msg);
    free(aid);
    if (rc.code != CC_OK) return rc;
    if (reasoning_content && reasoning_content[0]) {
        rc = cc_message_set_reasoning_content(assistant_msg, reasoning_content);
        if (rc.code != CC_OK) {
            cc_message_destroy(assistant_msg);
            return rc;
        }
    }
    rc = runtime_append_message(runtime, assistant_msg);
    cc_message_destroy(assistant_msg);
    return rc;
}

/* 把工具产生的 artifact 摘要追加为 observation，让下一轮模型能理解工具产物。 */
static cc_result_t cc_agent_runtime_append_artifact_observation(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_result_t *tool_result
)
{
    if (!runtime || !session_id || !tool_result ||
        tool_result->artifacts.count == 0) {
        return cc_result_ok();
    }

    char *summary = NULL;
    cc_result_t rc = cc_media_artifact_list_summarize(&tool_result->artifacts, &summary);
    if (rc.code != CC_OK) return rc;

    cc_string_builder_t sb;
    rc = cc_string_builder_init(&sb);
    if (rc.code == CC_OK) {
        rc = cc_string_builder_append(&sb, "Tool produced multimodal artifacts.\n");
    }
    if (rc.code == CC_OK && summary && summary[0]) {
        rc = cc_string_builder_append(&sb, summary);
    }
    free(summary);
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&sb);
        return rc;
    }

    char *content = cc_string_builder_take(&sb);
    if (!content) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build artifact observation");
    }

    cc_content_parts_t parts;
    cc_content_parts_init(&parts);
    rc = cc_content_parts_append_text(&parts, content, CC_CONTENT_PART_INPUT);
    for (size_t i = 0; rc.code == CC_OK && i < tool_result->artifacts.count; i++) {
        rc = cc_content_parts_append_artifact(
            &parts, &tool_result->artifacts.items[i], CC_CONTENT_PART_INPUT);
    }
    if (rc.code != CC_OK) {
        cc_content_parts_cleanup(&parts);
        free(content);
        return rc;
    }
    cc_message_t *msg = NULL;
    char *id = NULL;
    rc = generate_id(&id);
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = cc_message_create_parts(id, session_id, CC_ROLE_USER, &parts, NULL, &msg);
    }
    free(id);
    cc_content_parts_cleanup(&parts);
    free(content);
    if (rc.code == CC_OK) {
        rc = runtime_append_message(runtime, msg);
    }
    cc_message_destroy(msg);
    return rc;
}


/* 同步 run 的工具步骤：执行工具、审计 tool call/result，并把 tool 消息写回历史。 */
cc_result_t cc_agent_runtime_execute_tool_step(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_call_t *call,
    const char *reasoning_content,
    cc_cancel_token_t *cancel_token,
    uint64_t deadline_ms
)
{
    if (!runtime || !session_id || !call) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool step argument");
    }

    cc_message_t *asst_msg = NULL;
    char *aid = NULL;
    cc_result_t rc = generate_id(&aid);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    rc = cc_message_create_text(aid, session_id, CC_ROLE_ASSISTANT,
        NULL, call->id, &asst_msg);
    free(aid);
    if (rc.code == CC_OK) {
        rc = cc_message_add_tool_call(asst_msg, call);
    }
    if (rc.code == CC_OK && reasoning_content && reasoning_content[0]) {
        rc = cc_message_set_reasoning_content(asst_msg, reasoning_content);
    }
    if (rc.code != CC_OK) {
        cc_message_destroy(asst_msg);
        return rc;
    }
    cc_tool_result_t tool_result;
    memset(&tool_result, 0, sizeof(tool_result));
    cc_tool_executor_options_t exec_options;
    memset(&exec_options, 0, sizeof(exec_options));
    exec_options.cancel_token = cancel_token;
    exec_options.deadline_ms = deadline_ms;
    rc = cc_tool_executor_execute_with_options(
        runtime, session_id, call, &exec_options, &tool_result);
    if (rc.code != CC_OK) {
        cc_message_destroy(asst_msg);
        cc_tool_result_cleanup(&tool_result);
        return rc;
    }

    cc_message_t *tool_msg = NULL;
    char *tid = NULL;
    rc = generate_id(&tid);
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = cc_message_create_text(tid, session_id, CC_ROLE_TOOL,
            tool_result.ok ? tool_result.text : tool_result.error,
            call->id, &tool_msg);
    }
    free(tid);
    if (rc.code == CC_OK) {
        cc_session_record_t records[4];
        memset(records, 0, sizeof(records));
        records[0].type = CC_SESSION_RECORD_MESSAGE;
        records[0].session_id = session_id;
        records[0].data.message = asst_msg;
        records[1].type = CC_SESSION_RECORD_TOOL_CALL;
        records[1].session_id = session_id;
        records[1].data.tool_call = call;
        records[2].type = CC_SESSION_RECORD_TOOL_RESULT;
        records[2].session_id = session_id;
        records[2].data.tool_result.tool_call_id = call->id;
        records[2].data.tool_result.result = &tool_result;
        records[3].type = CC_SESSION_RECORD_MESSAGE;
        records[3].session_id = session_id;
        records[3].data.message = tool_msg;
        rc = runtime_append_records(runtime, records, 4);
    }
    cc_message_destroy(asst_msg);
    cc_message_destroy(tool_msg);
    if (rc.code == CC_OK) {
        rc = cc_agent_runtime_append_artifact_observation(
            runtime, session_id, &tool_result);
    }
    cc_tool_result_cleanup(&tool_result);
    return rc.code == CC_OK ? cc_result_ok() : rc;
}

/*
 * 流式主循环的临时状态。
 *
 * 这个结构体只在一次 stream run 的栈帧中存在，不跨线程共享；其中 runtime/session_id
 * 是借用引用，builder 和 cur_tool_* 是本循环拥有并负责释放的资源。把这些状态集中起来，
 * 可以让 provider callback 在没有全局变量的情况下累积 text、thinking 和 tool 参数，
 * 这也是嵌入式 C 中常见的“显式上下文指针”写法。
 */
#define CC_STREAM_MAX_TOOL_CALLS 8

typedef struct stream_tool_slot {
    int used;
    int ended;
    int provider_index;
    char *name;
    char *id;
    cc_string_builder_t arguments;
} stream_tool_slot_t;

typedef struct {
    cc_agent_runtime_t *runtime;
    const char *session_id;
    int step;
    int chunk_count;
    cc_string_builder_t text_builder;
    cc_string_builder_t thinking_builder;
    stream_tool_slot_t tools[CC_STREAM_MAX_TOOL_CALLS];
    size_t tool_count;
    int has_tool_call;
    int finished;
    int cancelled;
    int limit_exceeded;
    cc_cancel_token_t *cancel_token;
    uint64_t deadline_ms;
    cc_agent_runtime_stream_callback_fn on_chunk;
    void *chunk_user_data;
    size_t emitted_stream_bytes;
    size_t max_stream_bytes;
    char *response_text;
} stream_loop_ctx_t;

static void stream_tool_slots_cleanup(stream_loop_ctx_t *ctx)
{
    if (!ctx) return;
    for (size_t i = 0; i < CC_STREAM_MAX_TOOL_CALLS; i++) {
        stream_tool_slot_t *slot = &ctx->tools[i];
        if (slot->used) cc_string_builder_deinit(&slot->arguments);
        free(slot->name);
        free(slot->id);
        memset(slot, 0, sizeof(*slot));
    }
    ctx->tool_count = 0;
    ctx->has_tool_call = 0;
}

static void stream_loop_ctx_cleanup(stream_loop_ctx_t *ctx)
{
    if (!ctx) return;
    stream_tool_slots_cleanup(ctx);
    cc_string_builder_deinit(&ctx->text_builder);
    cc_string_builder_deinit(&ctx->thinking_builder);
}

static stream_tool_slot_t *stream_find_tool_slot(
    stream_loop_ctx_t *ctx,
    int provider_index,
    const char *tool_id)
{
    if (!ctx) return NULL;
    for (size_t i = 0; i < CC_STREAM_MAX_TOOL_CALLS; i++) {
        stream_tool_slot_t *slot = &ctx->tools[i];
        if (!slot->used) continue;
        if (provider_index >= 0 && slot->provider_index == provider_index) return slot;
        if (tool_id && tool_id[0] && slot->id && strcmp(slot->id, tool_id) == 0) return slot;
    }
    return NULL;
}

static cc_result_t stream_start_tool(stream_loop_ctx_t *ctx, const cc_stream_chunk_t *chunk)
{
    if (!ctx || !chunk || !chunk->tool_name || !chunk->tool_name[0]) {
        return cc_result_error(CC_ERR_MODEL, "Streaming tool start is missing a name");
    }
    if (stream_find_tool_slot(ctx, chunk->tool_index, chunk->tool_id)) {
        return cc_result_error(CC_ERR_MODEL, "Duplicate streaming tool index or id");
    }
    if (ctx->tool_count >= CC_STREAM_MAX_TOOL_CALLS) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Too many streaming tool calls");
    }
    stream_tool_slot_t *slot = NULL;
    for (size_t i = 0; i < CC_STREAM_MAX_TOOL_CALLS; i++) {
        if (!ctx->tools[i].used) { slot = &ctx->tools[i]; break; }
    }
    if (!slot) return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Streaming tool map is full");
    slot->name = cc_copy_string(chunk->tool_name);
    slot->id = cc_copy_string(chunk->tool_id ? chunk->tool_id : "");
    if (!slot->name || !slot->id) {
        free(slot->name);
        free(slot->id);
        memset(slot, 0, sizeof(*slot));
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy streaming tool metadata");
    }
    cc_result_t rc = cc_string_builder_init(&slot->arguments);
    if (rc.code != CC_OK) {
        free(slot->name);
        free(slot->id);
        memset(slot, 0, sizeof(*slot));
        return rc;
    }
    slot->provider_index = chunk->tool_index;
    slot->used = 1;
    ctx->tool_count++;
    ctx->has_tool_call = 1;
    return cc_result_ok();
}



/*
 * stream provider 可能把一个 tool call 拆成 start/delta/end 多个 chunk。
 *
 * 这个函数在 end 到达时把累积的 arguments 组装成 cc_tool_call_t，调用统一工具执行器，
 * 再把 tool result 写回 session store。这样同步和流式工具执行共享同一套安全策略、
 * 参数校验、approval 和资源限制。函数内部对 cur_tool_name/cur_tool_id 拥有释放责任，
 * 调用完成后会清空当前工具状态，避免下一段 chunk 误复用上一次工具调用。
 */
static cc_result_t execute_pending_tools(stream_loop_ctx_t *ctx)
{
    if (!ctx || ctx->tool_count == 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "No completed streaming tool calls");
    }
    cc_result_t state_rc = check_run_state(
        ctx->cancel_token, ctx->deadline_ms, "Streaming tool call cancelled");
    if (state_rc.code != CC_OK) {
        ctx->cancelled = 1;
        return state_rc;
    }

    const char *thinking = cc_string_builder_cstr(&ctx->thinking_builder);
    for (size_t i = 0; i < CC_STREAM_MAX_TOOL_CALLS; i++) {
        stream_tool_slot_t *slot = &ctx->tools[i];
        if (!slot->used) continue;
        if (!slot->ended) return cc_result_error(CC_ERR_MODEL, "Provider stream ended with an incomplete tool call");
        const char *arguments = cc_string_builder_cstr(&slot->arguments);
        if (!arguments || !arguments[0]) arguments = "{}";
        cc_json_value_t *json = NULL;
        cc_result_t valid = cc_json_parse(arguments, &json);
        if (valid.code != CC_OK || !json || !cc_json_is_object(json)) {
            cc_json_destroy(json);
            if (valid.code == CC_OK) cc_result_free(&valid);
            return valid.code != CC_OK ? valid :
                cc_result_error(CC_ERR_MODEL, "Streaming tool arguments must be a complete JSON object");
        }
        cc_json_destroy(json);
        cc_result_free(&valid);
    }

    cc_result_t rc = cc_result_ok();
    for (size_t i = 0; i < CC_STREAM_MAX_TOOL_CALLS && rc.code == CC_OK; i++) {
        stream_tool_slot_t *slot = &ctx->tools[i];
        if (!slot->used) continue;
        const char *arguments = cc_string_builder_cstr(&slot->arguments);
        if (!arguments || !arguments[0]) arguments = "{}";
        cc_tool_call_t call = {
            .id = slot->id,
            .name = slot->name,
            .arguments_json = (char *)arguments,
        };
        rc = cc_agent_runtime_execute_tool_step(
            ctx->runtime, ctx->session_id, &call,
            thinking && thinking[0] ? thinking : NULL, ctx->cancel_token,
            ctx->deadline_ms);
        if (rc.code == CC_ERR_CANCELLED) ctx->cancelled = 1;
        runtime_publish_observability(ctx->runtime, CC_OBS_EVENT_STREAM_TOOL_END,
            ctx->session_id, ctx->step, rc.code == CC_OK ? "ok" : "error",
            rc.message, rc.code == CC_OK ? NULL : &rc, NULL);
    }
    stream_tool_slots_cleanup(ctx);
    return rc;
}


/*
 * 通过 stream callback 把 runtime 本地错误作为 error chunk 交给实时 UI。
 *
 * 注意这里不直接写 session store；stream 错误、取消和超限都属于 partial response，
 * 默认不落库，避免下游把半截 assistant 回复当成稳定上下文。
 */
static cc_result_t stream_loop_emit_error(stream_loop_ctx_t *ctx, const char *message)
{
    if (!ctx || !ctx->on_chunk) return cc_result_ok();
    cc_stream_chunk_t error_chunk;
    memset(&error_chunk, 0, sizeof(error_chunk));
    error_chunk.type = CC_STREAM_CHUNK_ERROR;
    error_chunk.content = (char *)(message ? message : "stream error");
    return ctx->on_chunk(&error_chunk, ctx->chunk_user_data);
}


/*
 * 转发 provider chunk，同时执行 stream 字节限制，超限后停止继续处理。
 *
 * 字节预算按实际发给应用 callback 的 content 计算；一旦超过 max_stream_bytes，
 * 设置 limit_exceeded/finished 状态并发送 error chunk，主循环随后统一返回
 * CC_ERR_LIMIT_EXCEEDED。这样 UI 能立刻看到错误，业务层也能拿到稳定错误码。
 */
static cc_result_t stream_loop_forward_chunk(
    stream_loop_ctx_t *ctx,
    const cc_stream_chunk_t *chunk
)
{
    if (!ctx || !chunk) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid runtime stream chunk");
    }
    if (ctx->limit_exceeded) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Stream exceeds max_stream_bytes");
    }

    if (ctx->max_stream_bytes > 0 && chunk->content) {
        size_t len = strlen(chunk->content);
        if (len > SIZE_MAX - ctx->emitted_stream_bytes ||
            ctx->emitted_stream_bytes + len > ctx->max_stream_bytes) {
            ctx->limit_exceeded = 1;
            ctx->finished = 1;
            cc_result_t callback_rc = stream_loop_emit_error(
                ctx, "Stream exceeds max_stream_bytes");
            if (callback_rc.code != CC_OK) return callback_rc;
            cc_result_free(&callback_rc);
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED,
                "Stream exceeds max_stream_bytes");
        }
        ctx->emitted_stream_bytes += len;
    }

    if (ctx->on_chunk) return ctx->on_chunk(chunk, ctx->chunk_user_data);
    return cc_result_ok();
}


/*
 * provider stream callback 的核心状态机。
 *
 * 它同时负责三件事：把 chunk 透传给应用 callback、累积最终文本/思考/tool 参数、
 * 发布统一 observability 事件。工具调用在这里被识别并延迟到 tool_end 后执行。
 *
 * 设计上 callback 不拥有 chunk 内存，只在回调期间读取；需要跨 chunk 保存的内容都复制
 * 到 string builder 或 strdup 的字段里。这样 provider 可以使用栈上 chunk，也不会造成
 * 悬空指针问题。
 */
static cc_result_t stream_loop_callback(const cc_stream_chunk_t *chunk, void *user_data)
{
    stream_loop_ctx_t *ctx = (stream_loop_ctx_t *)user_data;
    if (!ctx || !ctx->runtime || !chunk) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid runtime stream callback");
    }
    if (cc_cancel_token_is_cancelled(ctx->cancel_token)) {
        ctx->cancelled = 1;
        ctx->finished = 1;
        return cc_result_error(CC_ERR_CANCELLED, "Agent stream run cancelled");
    }

    ctx->chunk_count++;
    cc_result_t rc = stream_loop_forward_chunk(ctx, chunk);
    if (rc.code != CC_OK) return rc;

    if (ctx->step >= 2 && getenv("CCLAW_DEBUG") && ctx->runtime->logger) {
        static const char *cnames[] = {
            "TEXT","THINKING","TOOL_START","TOOL_DELTA","TOOL_END","FINISHED",
            "ARTIFACT","PROVIDER_WARNING","ERROR"
        };
        cc_logger_log(ctx->runtime->logger, CC_LOG_DEBUG,
            "[CB] step=%d chunk=%d/%s",
            ctx->step, chunk->type,
            (chunk->type >= 0 && chunk->type < 9) ? cnames[chunk->type] : "?");
    }

    switch (chunk->type) {

    case CC_STREAM_CHUNK_THINKING:


        if (chunk->content && strlen(chunk->content) > 0) {
            rc = cc_string_builder_append(&ctx->thinking_builder, chunk->content);
            if (rc.code != CC_OK) return rc;
            runtime_publish_stream_content(ctx->runtime, CC_OBS_EVENT_STREAM_THINKING,
                ctx->session_id, ctx->step, "delta", chunk->content);
        }
        break;

    case CC_STREAM_CHUNK_TEXT:


        if (chunk->content && strlen(chunk->content) > 0) {
            rc = cc_string_builder_append(&ctx->text_builder, chunk->content);
            if (rc.code != CC_OK) return rc;
            runtime_publish_stream_content(ctx->runtime, CC_OBS_EVENT_STREAM_TEXT,
                ctx->session_id, ctx->step, "delta", chunk->content);
        }
        break;

    case CC_STREAM_CHUNK_TOOL_START:
        rc = stream_start_tool(ctx, chunk);
        if (rc.code != CC_OK) return rc;

        {
            cc_json_value_t *attrs = cc_json_create_object();
            if (attrs) {
                cc_json_object_set(attrs, "tool_name",
                    cc_json_create_string(chunk->tool_name ? chunk->tool_name : ""));
                cc_json_object_set(attrs, "tool_id",
                    cc_json_create_string(chunk->tool_id ? chunk->tool_id : ""));
                char *attrs_json = cc_json_stringify_unformatted(attrs);
                cc_json_destroy(attrs);
                if (attrs_json) {
                    runtime_publish_observability(ctx->runtime,
                        CC_OBS_EVENT_STREAM_TOOL_START, ctx->session_id,
                        ctx->step, "started", NULL, NULL, attrs_json);
                    free(attrs_json);
                }
            }
        }
        break;

    case CC_STREAM_CHUNK_TOOL_DELTA:
    {
        stream_tool_slot_t *slot = stream_find_tool_slot(
            ctx, chunk->tool_index, chunk->tool_id);
        if (!slot || slot->ended) {
            return cc_result_error(CC_ERR_MODEL,
                "Streaming tool delta does not match an open tool call");
        }
        if (chunk->content && strlen(chunk->content) > 0) {
            rc = cc_string_builder_append(&slot->arguments, chunk->content);
            if (rc.code != CC_OK) return rc;
            runtime_publish_stream_content(ctx->runtime, CC_OBS_EVENT_STREAM_TOOL_DELTA,
                ctx->session_id, ctx->step, "delta", chunk->content);
        }
        break;
    }

    case CC_STREAM_CHUNK_TOOL_END:
        if (chunk->tool_index < 0 && (!chunk->tool_id || !chunk->tool_id[0])) {
            int open = 0;
            for (size_t i = 0; i < CC_STREAM_MAX_TOOL_CALLS; i++) {
                if (ctx->tools[i].used && !ctx->tools[i].ended) {
                    ctx->tools[i].ended = 1;
                    open++;
                }
            }
            if (open == 0 && ctx->tool_count == 0) {
                return cc_result_error(CC_ERR_MODEL,
                    "Provider ended tool calls that were never started");
            }
        } else {
            stream_tool_slot_t *slot = stream_find_tool_slot(
                ctx, chunk->tool_index, chunk->tool_id);
            if (!slot || slot->ended) {
                return cc_result_error(CC_ERR_MODEL,
                    "Streaming tool end does not match an open tool call");
            }
            slot->ended = 1;
        }
        break;

    case CC_STREAM_CHUNK_FINISHED:
        ctx->finished = 1;
        break;

    case CC_STREAM_CHUNK_ARTIFACT:
        if (chunk->content) {
            runtime_publish_stream_content(ctx->runtime, CC_OBS_EVENT_STREAM_ARTIFACT,
                ctx->session_id, ctx->step, "artifact", chunk->content);
        }
        break;

    case CC_STREAM_CHUNK_PROVIDER_WARNING:
        if (chunk->content) {
            runtime_publish_stream_content(ctx->runtime,
                CC_OBS_EVENT_STREAM_PROVIDER_WARNING, ctx->session_id,
                ctx->step, "warning", chunk->content);
        }
        break;

    case CC_STREAM_CHUNK_ERROR:
        if (chunk->content) {
            runtime_publish_stream_content(ctx->runtime, CC_OBS_EVENT_STREAM_ERROR,
                ctx->session_id, ctx->step, "error", chunk->content);
        }
        break;
    }
    return cc_result_ok();
}


/*
 * 把同步 provider 的完整响应适配成统一 step engine 使用的 chunk 协议。
 *
 * 这里仅转换传输形态，不执行工具、不写会话历史。同步 provider 返回后，text、thinking
 * 和 tool calls 依次进入 stream_loop_callback，与原生 streaming provider 共享完全相同的
 * 限额、参数完整性检查、工具策略、最终响应落库和 observability 路径。
 */
static cc_result_t stream_loop_consume_sync_response(
    stream_loop_ctx_t *ctx,
    const cc_llm_response_t *response
)
{
    if (!ctx || !response) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,
                               "Invalid synchronous provider response");
    }
    if (response->tool_calls.count > CC_STREAM_MAX_TOOL_CALLS) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED,
                               "Too many synchronous tool calls");
    }

    cc_result_t rc = cc_result_ok();
    if (response->reasoning_content && response->reasoning_content[0]) {
        cc_stream_chunk_t thinking = {
            .type = CC_STREAM_CHUNK_THINKING,
            .content = response->reasoning_content,
            .tool_index = -1,
        };
        rc = stream_loop_callback(&thinking, ctx);
        if (rc.code != CC_OK) return rc;
        cc_result_free(&rc);
    }

    if (response->has_text && response->text && response->text[0]) {
        cc_stream_chunk_t text = {
            .type = CC_STREAM_CHUNK_TEXT,
            .content = response->text,
            .tool_index = -1,
        };
        rc = stream_loop_callback(&text, ctx);
        if (rc.code != CC_OK) return rc;
        cc_result_free(&rc);
    }

    for (size_t i = 0; i < response->tool_calls.count; i++) {
        const cc_tool_call_t *call = &response->tool_calls.items[i];
        cc_stream_chunk_t start = {
            .type = CC_STREAM_CHUNK_TOOL_START,
            .tool_name = call->name,
            .tool_id = call->id,
            .tool_index = (int)i,
        };
        rc = stream_loop_callback(&start, ctx);
        if (rc.code != CC_OK) return rc;
        cc_result_free(&rc);

        cc_stream_chunk_t delta = {
            .type = CC_STREAM_CHUNK_TOOL_DELTA,
            .content = call->arguments_json && call->arguments_json[0] ?
                call->arguments_json : "{}",
            .tool_id = call->id,
            .tool_index = (int)i,
        };
        rc = stream_loop_callback(&delta, ctx);
        if (rc.code != CC_OK) return rc;
        cc_result_free(&rc);

        cc_stream_chunk_t end = {
            .type = CC_STREAM_CHUNK_TOOL_END,
            .tool_id = call->id,
            .tool_index = (int)i,
        };
        rc = stream_loop_callback(&end, ctx);
        if (rc.code != CC_OK) return rc;
        cc_result_free(&rc);
    }

    if (response->artifacts.count > 0) {
        char *artifacts_json = NULL;
        rc = cc_media_artifact_list_to_json(&response->artifacts, &artifacts_json);
        if (rc.code != CC_OK) return rc;
        cc_result_free(&rc);
        cc_stream_chunk_t artifact = {
            .type = CC_STREAM_CHUNK_ARTIFACT,
            .content = artifacts_json,
            .tool_index = -1,
        };
        rc = stream_loop_callback(&artifact, ctx);
        free(artifacts_json);
        if (rc.code != CC_OK) return rc;
        cc_result_free(&rc);
    }

    cc_stream_chunk_t finished = {
        .type = CC_STREAM_CHUNK_FINISHED,
        .tool_index = -1,
    };
    return stream_loop_callback(&finished, ctx);
}


/*
 * Provider 传输适配层。
 *
 * 原生 streaming provider 直接把 chunk 推给统一状态机；只实现 chat() 的 provider
 * 先返回 cc_llm_response_t，再由上面的适配器转换成相同 chunk。该函数是 Runtime 中
 * 唯一选择 chat/chat_stream 的位置，step engine 不再按 provider 能力分叉。
 */
static cc_result_t runtime_submit_provider_step(
    cc_agent_runtime_t *runtime,
    cc_llm_chat_request_t *request,
    stream_loop_ctx_t *ctx
)
{
    if (!runtime || !runtime->llm.vtable || !request || !ctx) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,
                               "Invalid provider step submission");
    }
    if (runtime->llm.vtable->chat_stream) {
        request->stream = 1;
        return runtime->llm.vtable->chat_stream(
            runtime->llm.self, request, stream_loop_callback, ctx);
    }
    if (!runtime->llm.vtable->chat) {
        return cc_result_error(CC_ERR_UNSUPPORTED,
                               "Provider implements neither chat nor chat_stream");
    }

    request->stream = 0;
    cc_llm_response_t response;
    cc_result_t rc = cc_llm_response_init(&response);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    rc = runtime->llm.vtable->chat(runtime->llm.self, request, &response);
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = stream_loop_consume_sync_response(ctx, &response);
    }
    cc_llm_response_free(&response);
    return rc;
}


/*
 * 统一 agent step engine。
 *
 * 每一步构造上下文 -> 调 provider 传输适配层 -> 根据是否出现 tool call 决定继续循环或
 * 落库。provider 原生流式与同步完整响应都进入同一个 stream_loop_ctx 状态机；取消、
 * 超限和 provider 错误不会写入不完整 assistant final。
 *
 * out_response 由调用方释放；内部 builder 在每一步复用，只有在确认没有 tool call 且
 * 输出限制通过后，才把最终 assistant 文本写入 session store 和 active memory。
 * 这条路径是“实时输出 callback”和“持久化最终响应”的分界点，面试时可以强调它避免
 * partial chunk 污染会话历史。
 */
static cc_result_t cc_agent_runtime_handle_message_internal(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    const cc_agent_runtime_run_options_t *options,
    const cc_agent_runtime_stream_options_t *stream_options,
    char **out_response
)
{
    if (!runtime || !session_id || !user_input || !out_response) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "NULL argument");
    }
    *out_response = NULL;
    cc_result_t limit_rc = check_input_limit(runtime, user_input);
    if (limit_rc.code != CC_OK) return limit_rc;
    cc_result_t cancel_rc = check_run_cancelled(options, "Agent run cancelled before start");
    if (cancel_rc.code != CC_OK) return cancel_rc;
    uint64_t run_deadline_ms = runtime_run_deadline(runtime);
    cancel_rc = check_run_state(run_cancel_token(options), run_deadline_ms,
        "Agent run cancelled before start");
    if (cancel_rc.code != CC_OK) return cancel_rc;
    if (cc_cancel_token_is_cancelled(stream_cancel_token(stream_options))) {
        return cc_result_error(CC_ERR_CANCELLED, "Agent run cancelled before start");
    }



    cc_message_t *user_msg = NULL;
    char *msg_id = NULL;
    cc_result_t rc = generate_id(&msg_id);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    rc = cc_message_create_text(msg_id, session_id, CC_ROLE_USER, user_input, NULL, &user_msg);
    free(msg_id);

    if (rc.code != CC_OK) return rc;

    rc = runtime_append_message(runtime, user_msg);
    if (rc.code != CC_OK) {
        cc_message_destroy(user_msg);
        return rc;
    }
    cc_message_destroy(user_msg);



    stream_loop_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.runtime = runtime;
    ctx.session_id = session_id;
    ctx.cancel_token = run_cancel_token(options);
    if (!ctx.cancel_token) ctx.cancel_token = stream_cancel_token(stream_options);
    ctx.deadline_ms = run_deadline_ms;
    if (stream_options) {
        ctx.on_chunk = stream_options->on_chunk;
        ctx.chunk_user_data = stream_options->user_data;
    }
    ctx.max_stream_bytes = runtime->config.limits.max_stream_bytes;
    rc = cc_string_builder_init(&ctx.text_builder);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    rc = cc_string_builder_init(&ctx.thinking_builder);
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&ctx.text_builder);
        return rc;
    }
    cc_result_free(&rc);

    for (int step = 0; step < runtime_effective_max_steps(runtime); step++) {
        rc = check_run_state(ctx.cancel_token, run_deadline_ms,
            "Agent run cancelled");
        if (rc.code != CC_OK) {
            stream_loop_ctx_cleanup(&ctx);
            return rc;
        }
        ctx.step = step + 1;
        ctx.finished = 0;
        stream_tool_slots_cleanup(&ctx);
        ctx.chunk_count = 0;
        cc_string_builder_clear(&ctx.text_builder);
        cc_string_builder_clear(&ctx.thinking_builder);



        cc_message_t *messages = NULL;
        size_t message_count = 0;
        long stage_start_ms = runtime_clock_ms();
        rc = cc_context_builder_build_messages(runtime, session_id,
            runtime->config.system_prompt, ctx.cancel_token, &messages, &message_count);
        runtime_publish_llm_stage(runtime, CC_OBS_EVENT_LLM_CONTEXT_BUILD,
            session_id, step, rc.code == CC_OK ? "ok" : "error",
            runtime_clock_ms() - stage_start_ms, message_count,
            "message_count", 0, NULL, rc.code == CC_OK ? NULL : &rc);
        if (rc.code != CC_OK) {
            cc_result_t emit_rc = stream_loop_emit_error(
                &ctx, rc.message ? rc.message : cc_error_string(rc.code));
            cc_result_free(&emit_rc);
            runtime_publish_observability(runtime, CC_OBS_EVENT_LLM_REQUEST_START,
                session_id, step, "error", rc.message, &rc, NULL);
            stream_loop_ctx_cleanup(&ctx);
            return rc;
        }



        char *tools_json = NULL;
        stage_start_ms = runtime_clock_ms();
        rc = cc_tool_registry_build_schema_json(runtime->tool_registry, &tools_json);
        runtime_publish_llm_stage(runtime, CC_OBS_EVENT_LLM_TOOLS_SCHEMA_BUILD,
            session_id, step, rc.code == CC_OK ? "ok" : "error",
            runtime_clock_ms() - stage_start_ms,
            tools_json ? strlen(tools_json) : 0, "tools_json_bytes",
            0, NULL, rc.code == CC_OK ? NULL : &rc);
        if (rc.code != CC_OK) {
            cc_result_t emit_rc = stream_loop_emit_error(
                &ctx, rc.message ? rc.message : cc_error_string(rc.code));
            cc_result_free(&emit_rc);
            runtime_publish_observability(runtime, CC_OBS_EVENT_LLM_REQUEST_START,
                session_id, step, "error", rc.message, &rc, NULL);
            for (size_t mi = 0; mi < message_count; mi++) cc_message_cleanup(&messages[mi]);
            free(messages);
            free(tools_json);
            stream_loop_ctx_cleanup(&ctx);
            return rc;
        }



        runtime_publish_observability(runtime, CC_OBS_EVENT_LLM_REQUEST_START,
            session_id, step, "started", NULL, NULL, NULL);



        cc_llm_chat_request_t req;
        memset(&req, 0, sizeof(req));
        cc_media_limits_t media_limits;
        runtime_media_limits_from_config(&runtime->config.multimodal, &media_limits);
        req.model = runtime->config.model;
        req.extra_body_json = runtime->config.model_extra_body_json;
        req.messages = messages;
        req.message_count = message_count;
        req.media_limits = &media_limits;
        req.max_tokens = runtime->config.max_tokens;
        req.temperature = runtime->config.temperature;
        req.stream = runtime->llm.vtable && runtime->llm.vtable->chat_stream ? 1 : 0;
        req.thinking_mode = cc_agent_runtime_get_thinking_mode(runtime);
        req.cancel_token = ctx.cancel_token;
        req.timeout_ms = runtime->config.limits.provider_timeout_ms;
        req.deadline_ms = provider_deadline(runtime, run_deadline_ms);
        req.max_response_bytes = runtime->config.limits.max_output_bytes;
        req.event_bus = runtime->event_bus;
        req.session_id = session_id;
        req.step = step;

        if (tools_json && strlen(tools_json) > 2) {
            req.tools_json = tools_json;
        }

        {
            char *request_attrs = runtime_build_request_config_attributes(
                runtime, &req, tools_json ? strlen(tools_json) : 0);
            runtime_publish_observability(runtime,
                CC_OBS_EVENT_LLM_REQUEST_CONFIG, session_id, step,
                "ok", NULL, NULL, request_attrs);
            free(request_attrs);
        }

        if (getenv("CCLAW_DEBUG") && runtime->logger) {
            cc_logger_log(runtime->logger, CC_LOG_DEBUG,
                "[DEBUG] step=%d provider_transport=%s message_count=%zu tools_json_len=%zu",
                step,
                req.stream ? "stream" : "sync-adapter",
                message_count,
                tools_json ? strlen(tools_json) : 0);
        }

        stage_start_ms = runtime_clock_ms();
        rc = runtime_submit_provider_step(runtime, &req, &ctx);
        runtime_publish_llm_stage(runtime, CC_OBS_EVENT_LLM_PROVIDER_SUBMIT,
            session_id, step, rc.code == CC_OK ? "ok" : "error",
            runtime_clock_ms() - stage_start_ms,
            (size_t)ctx.chunk_count, "chunk_count",
            strlen(cc_string_builder_cstr(&ctx.text_builder)), "text_bytes",
            rc.code == CC_OK ? NULL : &rc);

        for (size_t mi = 0; mi < message_count; mi++) cc_message_cleanup(&messages[mi]);
        free(messages);
        free(tools_json);

        if (getenv("CCLAW_DEBUG") && runtime->logger) {
            cc_logger_log(runtime->logger, CC_LOG_DEBUG,
                "[DEBUG] step=%d chunks=%d has_tool_call=%d finished=%d "
                "text_len=%zu rc=%d",
                step, ctx.chunk_count, ctx.has_tool_call, ctx.finished,
                strlen(cc_string_builder_cstr(&ctx.text_builder)), rc.code);
        }

        runtime_publish_observability(runtime, CC_OBS_EVENT_LLM_RESPONSE_FINISH,
            session_id, step, rc.code == CC_OK ? "ok" : "error",
            rc.message, rc.code == CC_OK ? NULL : &rc, NULL);

        if (ctx.limit_exceeded) {
            stream_loop_ctx_cleanup(&ctx);
            cc_result_free(&rc);
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Stream exceeds max_stream_bytes");
        }

        if (rc.code != CC_OK) {
            stream_loop_ctx_cleanup(&ctx);
            *out_response = cc_copy_string("Provider error");
            return rc;
        }
        rc = check_run_state(ctx.cancel_token, run_deadline_ms,
            "Agent run cancelled");
        if (ctx.cancelled || rc.code != CC_OK) {
            stream_loop_ctx_cleanup(&ctx);
            return rc.code != CC_OK ? rc :
                cc_result_error(CC_ERR_CANCELLED, "Agent stream run cancelled");
        }



        if (!ctx.has_tool_call) {
            const char *final_text = cc_string_builder_cstr(&ctx.text_builder);
            const char *thinking = cc_string_builder_cstr(&ctx.thinking_builder);

            rc = check_output_limit(runtime, final_text, "Response");
            if (rc.code != CC_OK) {
                stream_loop_ctx_cleanup(&ctx);
                return rc;
            }

            rc = cc_agent_runtime_store_assistant_text(
                runtime, session_id, final_text, thinking);
            if (rc.code != CC_OK) {
                stream_loop_ctx_cleanup(&ctx);
                return rc;
            }
            active_memory_after_run(runtime, session_id, user_input, final_text);

            *out_response = final_text ? cc_copy_string(final_text) : cc_copy_string("");
            runtime_publish_llm_stage(runtime, CC_OBS_EVENT_LLM_STREAM_FINISH,
                session_id, step, "finished", 0,
                (size_t)ctx.chunk_count, "chunk_count",
                final_text ? strlen(final_text) : 0, "text_bytes", NULL);
            stream_loop_ctx_cleanup(&ctx);

            runtime_publish_observability(runtime, CC_OBS_EVENT_STREAM_FINISHED,
                session_id, step, "finished", NULL, NULL, NULL);
            runtime_publish_observability(runtime, CC_OBS_EVENT_RUN_FINISHED,
                session_id, step, "ok", NULL, NULL, NULL);
            return cc_result_ok();
        }

        /* 工具执行必须发生在 provider 返回之后，不能占用 HTTP body callback。 */
        rc = execute_pending_tools(&ctx);
        if (rc.code != CC_OK) {
            stream_loop_ctx_cleanup(&ctx);
            return rc;
        }



    }



    stream_loop_ctx_cleanup(&ctx);

    *out_response = cc_copy_string("Agent stopped: max steps reached.");
    runtime_publish_llm_stage(runtime, CC_OBS_EVENT_LLM_STREAM_FINISH,
        session_id, runtime_effective_max_steps(runtime), "max_steps_reached",
        0, 0, "chunk_count", 0, "text_bytes", NULL);
    runtime_publish_observability(runtime, CC_OBS_EVENT_STREAM_FINISHED,
        session_id, runtime_effective_max_steps(runtime), "finished", NULL, NULL, NULL);
    runtime_publish_observability(runtime, CC_OBS_EVENT_RUN_FINISHED,
        session_id, runtime_effective_max_steps(runtime), "max_steps_reached",
        "max_steps_reached", NULL, "{\"reason\":\"max_steps_reached\"}");
    return cc_result_ok();
}

/* 正式 stream callback API：同步 provider 也通过适配后的 chunk 走同一 callback。 */
cc_result_t cc_agent_runtime_handle_message_stream_cb(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    const cc_agent_runtime_stream_options_t *options,
    char **out_response
)
{
    cc_agent_runtime_run_options_t run_options;
    memset(&run_options, 0, sizeof(run_options));
    if (options) run_options.cancel_token = options->cancel_token;
    cc_result_t enter_rc = runtime_enter_run(runtime);
    if (enter_rc.code != CC_OK) return enter_rc;
    cc_result_free(&enter_rc);
    cc_result_t rc = cc_agent_runtime_handle_message_internal(
        runtime, session_id, user_input,
        options ? &run_options : NULL,
        options,
        out_response);
    runtime_leave_run(runtime);
    return rc;
}

/* 销毁 runtime 自有配置内存；注入的端口对象由创建者或 builder 负责销毁。 */
void cc_agent_runtime_destroy(cc_agent_runtime_t *runtime)
{
    if (!runtime) return;
    cc_mutex_lock(runtime->mutex);
    free(runtime->config.system_prompt);
    free(runtime->config.workspace_dir);
    free(runtime->config.model);
    free(runtime->config.model_extra_body_json);
    free(runtime->config.active_memory_category);
    runtime_string_list_cleanup(&runtime->config.multimodal.limits.allowed_mime_prefixes);
    for (size_t i = 0; i < CC_CONTEXT_SUMMARY_CACHE_SLOTS; i++) {
        free(runtime->summary_cache[i].session_id);
        free(runtime->summary_cache[i].summary);
        memset(&runtime->summary_cache[i], 0, sizeof(runtime->summary_cache[i]));
    }
    cc_mutex_unlock(runtime->mutex);
    cc_mutex_destroy(runtime->mutex);
    free(runtime);
}


/* 运行时切换 thinking mode，最终会透传到 provider request。 */
void cc_agent_runtime_set_thinking_mode(cc_agent_runtime_t *runtime, int enabled)
{
    if (!runtime) return;
    cc_mutex_lock(runtime->mutex);
    runtime->thinking_mode = enabled ? 1 : 0;
    cc_mutex_unlock(runtime->mutex);
}


/* 注入人工审批回调；高风险工具 require_approval 时会调用它。 */
void cc_agent_runtime_set_tool_approval(
    cc_agent_runtime_t *runtime,
    cc_tool_approval_fn approve_tool_call,
    void *user_data
)
{
    if (!runtime) return;
    cc_mutex_lock(runtime->mutex);
    runtime->services.approve_tool_call = approve_tool_call;
    runtime->services.approval_user_data = user_data;
    cc_mutex_unlock(runtime->mutex);
}


/* 查询当前 thinking mode，provider request 构造时使用这个值。 */
int cc_agent_runtime_get_thinking_mode(cc_agent_runtime_t *runtime)
{
    if (!runtime) return 0;
    cc_mutex_lock(runtime->mutex);
    int enabled = runtime->thinking_mode;
    cc_mutex_unlock(runtime->mutex);
    return enabled;
}

/*
 * 暴露 runtime 内部 event bus 的借用指针。
 *
 * 调用方不能销毁返回值；它通常用于测试、调试 UI 或上层日志模块订阅统一
 * observability 事件。业务路径发布事件仍应走 cc_observability_publish 封装。
 */
cc_event_bus_t *cc_agent_runtime_event_bus(cc_agent_runtime_t *runtime)
{
    return runtime ? runtime->event_bus : NULL;
}

/*
 * 暴露工具注册表的借用指针。
 *
 * 下游应用通过它注册工具，runtime 销毁时统一释放 registry；多线程注册与执行的并发
 * 语义由 tool registry 自身的锁保护，调用方不应缓存内部数组指针。
 */
cc_tool_registry_t *cc_agent_runtime_tool_registry(cc_agent_runtime_t *runtime)
{
    return runtime ? runtime->tool_registry : NULL;
}

/*
 * 获取运行时绑定的工具执行器池：返回 runtime 内部的 tool_pool 指针，
 * 用于查询或执行 Agent 可用的工具集。
 * 参数: runtime - Agent 运行时实例
 * 返回: cc_tool_executor_pool_t*，runtime 为 NULL 时返回 NULL
 */
cc_tool_executor_pool_t *cc_agent_runtime_tool_pool(cc_agent_runtime_t *runtime)
{
    return runtime ? runtime->tool_pool : NULL;
}

/*
 * 暴露 session store 端口的借用视图。
 *
 * 返回的是 runtime 内嵌端口结构地址，便于测试或上层做只读查询；端口 self/vtable 的
 * 生命周期仍由 runtime/builder 注入约定管理，调用方不能 free 这个指针。
 */
cc_session_store_t *cc_agent_runtime_session_store(cc_agent_runtime_t *runtime)
{
    return runtime ? &runtime->store : NULL;
}


/* 判断当前 provider 是否实现原生 stream vtable；同步适配不改变能力探测结果。 */
int cc_agent_runtime_supports_stream(cc_agent_runtime_t *runtime)
{
    return runtime && runtime->llm.vtable && runtime->llm.vtable->chat_stream;
}


/*
 * 创建会话并记录 workspace 边界。
 *
 * 如果 store 没有实现 create_session，runtime 选择 no-op 成功，保持最小嵌入式配置可用；
 * 一旦实现了 session store，workspace_dir 会优先使用调用方传入值，否则回退到 runtime
 * config。文件工具后续会依赖这个 workspace 做路径归一化和越界检查。
 */
cc_result_t cc_agent_runtime_create_session(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *workspace_dir
)
{
    if (!runtime || !session_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null session create argument");
    }
    if (!runtime->store.vtable || !runtime->store.vtable->create_session) {
        return cc_result_ok();
    }
    return runtime->store.vtable->create_session(
        runtime->store.self,
        session_id,
        workspace_dir ? workspace_dir : runtime->config.workspace_dir
    );
}




cc_result_t cc_agent_runtime_handle_message_with_options(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    const cc_agent_runtime_run_options_t *options,
    char **out_response
)
{
    cc_result_t enter_rc = runtime_enter_run(runtime);
    if (enter_rc.code != CC_OK) return enter_rc;
    cc_result_free(&enter_rc);
    cc_result_t rc = cc_agent_runtime_handle_message_internal(
        runtime, session_id, user_input, options, NULL, out_response);
    runtime_leave_run(runtime);
    return rc;
}

/* 默认同步入口：不带取消 token，返回完整 assistant 文本。 */
cc_result_t cc_agent_runtime_handle_message(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    char **out_response
)
{
    return cc_agent_runtime_handle_message_with_options(
        runtime, session_id, user_input, NULL, out_response);
}
