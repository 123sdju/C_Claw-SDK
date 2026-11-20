



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
#include <stdbool.h>

static const char runtime_non_execution_note[] =
    "The tool invocation did not begin. Explicit run recovery may retry this planned node with a new call ID.";

/**
 * @brief 释放配置列表拥有的各字符串及数组，随后清零列表。
 * @param[in,out] list 拥有的列表，可为 NULL，各 count 项可为空指针。
 * @post 非 NULL 列表的资源已释放，items/count 清零。
 */
static void runtime_string_list_cleanup(cc_config_string_list_t *list)
{
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

/**
 * @brief 为 runtime 深拷贝配置字符串列表，保留列表中的 NULL 槽。
 * @param[in] src 借用列表，可为 NULL，count 非零时 items 须有对应可读槽。
 * @param[out] dst 必需的无已有资源输出，进入后清零，成功副本由 runtime 清理。
 * @return 空源或复制成功为 CC_OK，空输出为 CC_ERR_INVALID_ARGUMENT，分配失败为 CC_ERR_OUT_OF_MEMORY 且清理部分副本。
 */
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

/**
 * @brief 深拷贝可选配置字符串，输出不依赖原配置对象。
 * @param[in] src 借用的 NUL 结尾正文，可为 NULL。
 * @param[out] dst 必需的空资源输出槽，非 NULL 时先置空，成功字符串归调用者。
 * @param[in] field_name 可选借用字段名，仅用于分配失败消息。
 * @return 空源或复制成功为 CC_OK，空输出为 CC_ERR_INVALID_ARGUMENT，分配失败为 CC_ERR_OUT_OF_MEMORY。
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

/**
 * @brief 将多模态标量限制和 MIME 列表转为 provider 借用视图。
 * @param[in] config 借用配置，可为 NULL，此时采用 text-only 限制。
 * @param[out] out_limits 可选输出，非 NULL 时先清零，MIME 指针继续借用 config。
 * @post 非 NULL 输出已填写，MIME 列表的拥有者须覆盖请求使用期。
 */
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

/**
 * @brief 填写统一观测事件并尝试发布，内部消费发布结果。
 * @param[in] runtime 借用实例，NULL 或无 event_bus 时直接返回。
 * @param[in] event_name 借用事件名称，NULL 时直接返回。
 * @param[in] session_id 可选借用会话标识。
 * @param[in] run_id 可选借用运行标识。
 * @param[in] step 模型步骤序号或辅助调用约定值。
 * @param[in] status 可选借用状态，NULL 转为空字符串。
 * @param[in] message 可选借用事件正文。
 * @param[in] error 可选借用错误，发布期间保持有效。
 * @param[in] attributes_json 可选借用 JSON 对象正文，具体校验由观测封装完成。
 * @post 已尝试发布并释放操作结果，借用输入仍由调用者管理，发布失败仅影响观测。
 */
static void runtime_publish_observability_with_run(
    cc_agent_runtime_t *runtime,
    const char *event_name,
    const char *session_id,
    const char *run_id,
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
    event.run_id = run_id;
    event.step = step;
    event.status = status ? status : "";
    event.message = message;
    event.error = error;
    event.attributes_json = attributes_json;

    cc_result_t rc = cc_observability_publish(runtime->event_bus, &event);
    cc_result_free(&rc);
}

/**
 * @brief 以空 run_id 委托统一观测发布。
 * @param[in] runtime 借用实例，可为 NULL。
 * @param[in] event_name 借用事件名，NULL 时跳过。
 * @param[in] session_id 可选借用会话标识。
 * @param[in] step 观测步骤序号。
 * @param[in] status 可选借用状态。
 * @param[in] message 可选借用正文。
 * @param[in] error 可选借用错误。
 * @param[in] attributes_json 可选借用 JSON 对象正文。
 * @post 观测已按统一发布入口尝试提交，错误结果由内部消费。
 */
static void runtime_publish_observability(
    cc_agent_runtime_t *runtime, const char *event_name, const char *session_id,
    int step, const char *status, const char *message, const cc_result_t *error,
    const char *attributes_json)
{
    runtime_publish_observability_with_run(runtime,event_name,session_id,NULL,step,
                                          status,message,error,attributes_json);
}

/**
 * @brief 读取用于阶段耗时观测的毫秒时钟，优先使用单调时钟，零值时回退墙钟。
 * @return 非负毫秒数，超过 long 上限时饱和，墙钟失败时为零。
 */
static long runtime_clock_ms(void)
{
    uint64_t now_ms = cc_platform_monotonic_ms();
    if (now_ms > 0) {
        return now_ms > (uint64_t)LONG_MAX ? LONG_MAX : (long)now_ms;
    }
    time_t now = time(NULL);
    if (now < 0) return 0;
    return (uint64_t)now > (uint64_t)LONG_MAX / 1000U ? LONG_MAX : (long)now * 1000L;
}

/**
 * @brief 将阶段耗时和可选计数编码为 JSON 后发布，资源不足时保留基础事件。
 * @param[in] runtime 借用实例，NULL 或无事件总线时跳过。
 * @param[in] event_name 必需借用事件名，NULL 时跳过。
 * @param[in] session_id 可选借用会话标识。
 * @param[in] run_id 可选借用运行标识。
 * @param[in] step 阶段所属步骤序号。
 * @param[in] status 可选借用状态。
 * @param[in] elapsed_ms 耗时毫秒，负数编码为零。
 * @param[in] primary_count 主计数，无量纲，字段名非 NULL 时编码。
 * @param[in] primary_count_name 可选借用主计数字段名。
 * @param[in] secondary_count 次计数，无量纲，字段名非 NULL 时编码。
 * @param[in] secondary_count_name 可选借用次计数字段名。
 * @param[in] error 可选借用错误，正文使用其 message。
 * @post 已尝试发布，临时 JSON 和正文已释放，观测失败由内部消费。
 */
static void runtime_publish_llm_stage(
    cc_agent_runtime_t *runtime,
    const char *event_name,
    const char *session_id,
    const char *run_id,
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
        runtime_publish_observability_with_run(runtime, event_name, session_id, run_id, step,
            status, error ? error->message : NULL, error, NULL);
        return;
    }
    cc_json_object_set_owned(attrs, "elapsed_ms",
        cc_json_create_number((double)(elapsed_ms < 0 ? 0 : elapsed_ms)));
    if (primary_count_name) {
        cc_json_object_set_owned(attrs, primary_count_name,
            cc_json_create_number((double)primary_count));
    }
    if (secondary_count_name) {
        cc_json_object_set_owned(attrs, secondary_count_name,
            cc_json_create_number((double)secondary_count));
    }
    char *attrs_json = cc_json_stringify_unformatted(attrs);
    cc_json_destroy(attrs);
    runtime_publish_observability_with_run(runtime, event_name, session_id, run_id, step,
        status, error ? error->message : NULL, error, attrs_json);
    free(attrs_json);
}

/**
 * @brief 将一次模型步骤的已报告用量及身份编码为最佳努力观测。
 * @param[in] runtime 借用实例，NULL 或无总线时跳过，provider_name 从实例借用。
 * @param[in] session_id 可选借用会话标识。
 * @param[in] run_id 可选借用运行标识。
 * @param[in] origin_turn_id 可选借用原对话标识，非空时编码为属性。
 * @param[in] step 模型步骤序号，辅助调用使用负一。
 * @param[in] model 可选借用模型名，NULL 编码为空字符串。
 * @param[in] purpose 可选借用用途名称，非空时编码。
 * @param[in] usage 可选借用用量，NULL 或 valid 为零时记录未报告，缓存计数另依 reported/valid 位解释。
 * @post 临时资源及发布结果已清理，缺失用量通过 usage_reported 表达。
 */
void cc_agent_runtime_publish_llm_usage_with_identity(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *run_id,
    const char *origin_turn_id,
    int step,
    const char *model,
    const char *purpose,
    const cc_llm_usage_t *usage
)
{
    if (!runtime || !runtime->event_bus) return;
    int reported = usage && usage->valid;
    cc_json_value_t *attrs = cc_json_create_object();
    if (!attrs) return;
    cc_json_object_set_owned(attrs, "model",
        cc_json_create_string(model ? model : ""));
    if (purpose && purpose[0])
        cc_json_object_set_owned(attrs, "purpose", cc_json_create_string(purpose));
    if (origin_turn_id && origin_turn_id[0])
        cc_json_object_set_owned(attrs, "origin_turn_id", cc_json_create_string(origin_turn_id));
    if (runtime->provider_name && runtime->provider_name[0])
        cc_json_object_set_owned(attrs, "provider", cc_json_create_string(runtime->provider_name));
    cc_json_object_set_owned(attrs, "usage_reported", cc_json_create_bool(reported));
    /* 旧字段用零同时表示缺失，prompt/completion 仅发布正值，缓存字段按独立有效位发布。 */
    if (reported && usage->prompt_tokens > 0) cc_json_object_set_owned(attrs, "prompt_tokens",
        cc_json_create_number((double)usage->prompt_tokens));
    if (reported && usage->completion_tokens > 0) cc_json_object_set_owned(attrs, "completion_tokens",
        cc_json_create_number((double)usage->completion_tokens));
    cc_json_object_set_owned(attrs, "cached_tokens_reported",
        cc_json_create_bool(reported && (usage->cached_tokens_reported || usage->cached_tokens_valid)));
    cc_json_object_set_owned(attrs, "cache_write_tokens_reported",
        cc_json_create_bool(reported && (usage->cache_write_tokens_reported || usage->cache_write_tokens_valid)));
    if (reported && usage->cached_tokens_valid) {
        cc_json_object_set_owned(attrs, "cached_tokens",
            cc_json_create_number((double)usage->cached_tokens));
    }
    if (reported && usage->cache_write_tokens_valid) {
        cc_json_object_set_owned(attrs, "cache_write_tokens",
            cc_json_create_number((double)usage->cache_write_tokens));
    }
    if (reported && usage->prompt_tokens > 0 && usage->cached_tokens_valid &&
        usage->cached_tokens <= usage->prompt_tokens) {
        cc_json_object_set_owned(attrs, "cache_hit_rate",
            cc_json_create_number((double)usage->cached_tokens /
                                  (double)usage->prompt_tokens));
    }
    char *attrs_json = cc_json_stringify_unformatted(attrs);
    cc_json_destroy(attrs);
    if (!attrs_json) return;
    runtime_publish_observability_with_run(runtime, CC_OBS_EVENT_LLM_USAGE,
        session_id, run_id, step, reported ? "reported" : "unreported", NULL, NULL, attrs_json);
    free(attrs_json);
}

/**
 * @brief 以空运行和原对话标识委托模型用量发布。
 * @param[in] runtime 借用实例，可为 NULL。
 * @param[in] session_id 可选借用会话标识。
 * @param[in] step 步骤序号。
 * @param[in] model 可选借用模型名。
 * @param[in] purpose 可选借用用途名称。
 * @param[in] usage 可选借用用量，独立有效位的解释沿统一入口。
 * @post 已尝试发布并清理内部资源，输入保持借用。
 */
void cc_agent_runtime_publish_llm_usage(cc_agent_runtime_t *runtime, const char *session_id,
    int step, const char *model, const char *purpose, const cc_llm_usage_t *usage)
{
    cc_agent_runtime_publish_llm_usage_with_identity(runtime,session_id,NULL,NULL,step,model,purpose,usage);
}

/**
 * @brief 以空原对话标识记录一次辅助模型调用。
 * @param[in] runtime 借用实例，可为 NULL。
 * @param[in] purpose 当前支持 MEMORY_EXTRACTION，其他值跳过。
 * @param[in] model 可选借用模型名，缺失或空时回退实例配置。
 * @param[in] session_id 可选借用会话标识。
 * @param[in] usage 可选借用用量，NULL 记录未报告。
 * @post 已委托带原对话标识的入口进行最佳努力观测。
 */
void cc_agent_runtime_report_auxiliary_usage(cc_agent_runtime_t *runtime,
    cc_auxiliary_usage_purpose_t purpose, const char *model, const char *session_id,
    const cc_llm_usage_t *usage)
{
    cc_agent_runtime_report_auxiliary_usage_for_turn(runtime,purpose,model,session_id,NULL,usage);
}

/**
 * @brief 以用途和原对话身份归因独立的记忆提取用量。
 * @param[in] runtime 借用实例，NULL 时跳过。
 * @param[in] purpose 当前支持 MEMORY_EXTRACTION，其他值跳过。
 * @param[in] model 可选借用模型名，空时采用配置模型。
 * @param[in] session_id 可选借用原会话标识。
 * @param[in] turn_id 可选借用原对话标识，允许与 checkpoint/run ID 独立。
 * @param[in] usage 可选借用用量，NULL 保留未报告语义。
 * @post 以 step=-1 和 memory_extraction 用途尝试发布，内部清理临时资源。
 */
void cc_agent_runtime_report_auxiliary_usage_for_turn(cc_agent_runtime_t *runtime,
    cc_auxiliary_usage_purpose_t purpose, const char *model, const char *session_id,
    const char *turn_id, const cc_llm_usage_t *usage)
{
    if (!runtime) return;
    const char *label = purpose == CC_AUXILIARY_USAGE_MEMORY_EXTRACTION ? "memory_extraction" : NULL;
    if (!label) return;
    const char *effective_model = model && model[0] ? model : runtime->config.model;
    cc_agent_runtime_publish_llm_usage_with_identity(runtime,session_id,NULL,turn_id,-1,effective_model,label,usage);
}

/**
 * @brief 编码请求配置及 extra_body 顶层字段名，供请求观测使用。
 * @param[in] runtime 保留的实例参数，本实现通过 request 取得配置。
 * @param[in] request 借用请求，可为 NULL，相应属性采用零或空字符串。
 * @param[in] tools_json_bytes 工具 schema 原文字节数，不含 NUL。
 * @return 拥有的紧凑 JSON 正文，由调用者 free，创建或序列化失败为 NULL。
 * @note 额外正文只提取键名，解析采用普通首值入口，extra_body_parse_ok 表示该观察步骤的结果。
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

    cc_json_object_set_owned(attrs, "model",
        cc_json_create_string(request && request->model ? request->model : ""));
    cc_json_object_set_owned(attrs, "stream",
        cc_json_create_bool(request && request->stream != 0));
    cc_json_object_set_owned(attrs, "thinking_mode",
        cc_json_create_bool(request && request->thinking_mode != 0));
    cc_json_object_set_owned(attrs, "message_count",
        cc_json_create_number(request ? (double)request->message_count : 0.0));
    cc_json_object_set_owned(attrs, "tools_json_bytes",
        cc_json_create_number((double)tools_json_bytes));

    const char *extra_body_json = request ? request->extra_body_json : NULL;
    cc_json_object_set_owned(attrs, "has_extra_body",
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
                if (keys) cc_json_array_append(keys, cc_json_create_string(key));
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
    cc_json_object_set_owned(attrs, "extra_body_parse_ok",
        cc_json_create_bool(extra_body_parse_ok));
    cc_json_object_set_owned(attrs, "extra_body_keys", keys);
    cc_json_object_set_owned(attrs, "has_enable_thinking",
        cc_json_create_bool(has_enable_thinking));

    char *attrs_json = cc_json_stringify_unformatted(attrs);
    cc_json_destroy(attrs);
    return attrs_json;
}

/**
 * @brief 将流片段包装为 content 属性，再尝试发布统一观测。
 * @param[in] runtime 借用实例，NULL 或无总线时跳过。
 * @param[in] event_name 借用事件名称，NULL 时跳过。
 * @param[in] session_id 可选借用会话标识。
 * @param[in] run_id 可选借用运行标识。
 * @param[in] step 当前步骤序号。
 * @param[in] status 可选借用状态。
 * @param[in] content 可选借用片段，属性中 NULL 转为空字符串。
 * @post 临时 JSON 和正文已清理，分配或发布失败仅影响本次观测。
 */
static void runtime_publish_stream_content(
    cc_agent_runtime_t *runtime,
    const char *event_name,
    const char *session_id,
    const char *run_id,
    int step,
    const char *status,
    const char *content
)
{
    if (!runtime || !runtime->event_bus || !event_name) return;

    cc_json_value_t *attrs = cc_json_create_object();
    if (!attrs) return;
    cc_json_object_set_owned(attrs, "content",
        cc_json_create_string(content ? content : ""));
    char *attrs_json = cc_json_stringify_unformatted(attrs);
    cc_json_destroy(attrs);
    if (!attrs_json) return;

    runtime_publish_observability_with_run(runtime, event_name, session_id, run_id, step,
        status, content, NULL, attrs_json);
    free(attrs_json);
}


/**
 * @brief 从安全熵源生成带 msg_ 前缀的 UUID。
 * @param[out] out_id 必需的无已有资源输出槽，初始化及拥有规则沿 cc_id_generate_uuid_v4，成功字符串由调用者 free。
 * @return UUID 生成结果原样返回，熵源失败终止对应持久化步骤。
 */
static cc_result_t generate_id(char **out_id)
{
    return cc_id_generate_uuid_v4("msg_", out_id);
}

#if CC_ENABLE_ACTIVE_MEMORY
/**
 * @brief 对截断后的活动摘要正文计算 64 位 FNV-1a 键指纹。
 * @param[in] text 借用正文，NULL 按空字符串处理。
 * @return 按 uint64_t 回绕计算的内容指纹，用于相同活动摘要覆盖同一主键。
 */
static uint64_t runtime_fnv1a64(const char *text)
{
    uint64_t hash = 14695981039346656037ull;
    const unsigned char *bytes = (const unsigned char *)(text ? text : "");
    while (*bytes) {
        hash ^= (uint64_t)*bytes++;
        hash *= 1099511628211ull;
    }
    return hash;
}
#endif


/**
 * @brief 完整答复落库后，按可选活动记忆配置保存截断的问答摘要。
 * @param[in] runtime 借用实例，编译开关、实例配置及 memory_store 均可关闭本钩子。
 * @param[in] session_id 可选借用来源会话，转交存储 set。
 * @param[in] user_input 可选借用原输入。
 * @param[in] assistant_text 可选借用完整答复，两段都空时跳过。
 * @post 本次临时缓冲及存储结果已清理，保存采用内容指纹键，失败按可选钩子策略消费。
 * @note active_memory_max_value_chars 实际按字节截断，并退回 UTF-8 字首，缺省 1600 字节。
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
    cc_result_t rc = cc_string_builder_init(&sb);
    if (rc.code == CC_OK) rc = cc_string_builder_append(&sb, "User: ");
    if (rc.code == CC_OK) rc = cc_string_builder_append(&sb, user_input ? user_input : "");
    if (rc.code == CC_OK) rc = cc_string_builder_append(&sb, "\nAssistant: ");
    if (rc.code == CC_OK) rc = cc_string_builder_append(&sb, assistant_text ? assistant_text : "");
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        cc_string_builder_deinit(&sb);
        return;
    }
    char *value = cc_string_builder_take(&sb);
    if (!value) {
        cc_string_builder_deinit(&sb);
        return;
    }

    int max_chars = runtime->config.active_memory_max_value_chars > 0 ?
        runtime->config.active_memory_max_value_chars : 1600;
    if (strlen(value) > (size_t)max_chars) {
        size_t cutoff = (size_t)max_chars;
        while (cutoff > 0 && (((unsigned char)value[cutoff] & 0xc0U) == 0x80U)) cutoff--;
        value[cutoff] = '\0';
    }

    char key[32];
    snprintf(key, sizeof(key), "active.%016llx",
        (unsigned long long)runtime_fnv1a64(value));

    rc = cc_memory_store_set(
        runtime->memory_store,
        key,
        value,
        runtime->config.active_memory_category ?
            runtime->config.active_memory_category : "active_summary",
        session_id
    );
    cc_result_free(&rc);
    free(value);
#else
    (void)runtime;
    (void)session_id;
    (void)user_input;
    (void)assistant_text;
#endif
}

/**
 * @brief 取得本次同步调用借用的取消 token。
 * @param[in] options 借用选项，可为 NULL，基础结构由调用者保证完整。
 * @return 借用 token，缺少选项或 token 时为 NULL，拥有者须覆盖调用期。
 */
static cc_cancel_token_t *run_cancel_token(const cc_agent_runtime_run_options_t *options)
{
    return options ? options->cancel_token : NULL;
}

/**
 * @brief 在实例锁内检查并发上限，成功占用一个活跃调用名额。
 * @param[in,out] runtime 借用实例，成功后 active_runs 加一，结束时须配对 leave_run。
 * @return 成功为 CC_OK，NULL 为 CC_ERR_INVALID_ARGUMENT，达到并发上限为 CC_ERR_QUEUE_FULL。
 * @note 缺省上限一，该计数用于准入，外层负责实例销毁屏障。
 */
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

/**
 * @brief 归还已取得的运行准入名额。
 * @param[in,out] runtime 借用实例，NULL 时跳过。
 * @post 持锁将正数 active_runs 减一，零值保持零。
 */
static void runtime_leave_run(cc_agent_runtime_t *runtime)
{
    if (!runtime) return;
    cc_mutex_lock(runtime->mutex);
    if (runtime->active_runs > 0) runtime->active_runs--;
    cc_mutex_unlock(runtime->mutex);
}

/**
 * @brief 检查同步选项中的借用 token，映射为统一取消错误。
 * @param[in] options 可选借用选项。
 * @param[in] message 可选借用错误正文，NULL 使用缺省取消消息。
 * @return 已取消为 CC_ERR_CANCELLED，其他情况为 CC_OK。
 */
static cc_result_t check_run_cancelled(const cc_agent_runtime_run_options_t *options, const char *message)
{
    if (cc_cancel_token_is_cancelled(run_cancel_token(options))) {
        return cc_result_error(CC_ERR_CANCELLED, message ? message : "Agent run cancelled");
    }
    return cc_result_ok();
}

/**
 * @brief 按运行超时配置计算单调时钟的绝对截止时间。
 * @param[in] runtime 借用实例，NULL 或非正运行超时表示无限期。
 * @return 截止毫秒数，零表示无限期，加法溢出时饱和到 UINT64_MAX。
 */
static uint64_t runtime_run_deadline(const cc_agent_runtime_t *runtime)
{
    if (!runtime || runtime->config.limits.run_timeout_ms <= 0) return 0;
    uint64_t now_ms = cc_platform_monotonic_ms();
    uint64_t timeout_ms = (uint64_t)runtime->config.limits.run_timeout_ms;
    return now_ms > UINT64_MAX - timeout_ms ? UINT64_MAX : now_ms + timeout_ms;
}

/**
 * @brief 先检查取消，再检查单调时钟截止时间。
 * @param[in] cancel_token 可选借用 token，拥有者覆盖检查期。
 * @param[in] deadline_ms 绝对单调毫秒时间，零表示无限期。
 * @param[in] cancel_message 可选借用取消消息。
 * @return 已取消为 CC_ERR_CANCELLED，超时为 CC_ERR_TIMEOUT，其他为 CC_OK。
 */
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

/**
 * @brief 从运行截止时间和本次 provider 超时中选择较早的有效截止时间。
 * @param[in] runtime 可选借用实例，provider_timeout_ms 非正时沿用运行截止时间。
 * @param[in] run_deadline_ms 绝对单调毫秒时间，零表示无限期。
 * @return 本次 provider 绝对截止毫秒，零表示无限期，加法饱和到 UINT64_MAX。
 */
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

/**
 * @brief 取得流式选项中借用的取消 token。
 * @param[in] options 可选借用选项，基础结构由调用者保证完整。
 * @return 借用 token，缺少选项或 token 时为 NULL。
 */
static cc_cancel_token_t *stream_cancel_token(const cc_agent_runtime_stream_options_t *options)
{
    return options ? options->cancel_token : NULL;
}

/**
 * @brief 按 limits.max_steps、旧 max_steps、缺省一的顺序选择正数步数上限。
 * @param[in] runtime 可选借用实例，配置创建后固定。
 * @return 正数模型步骤上限，NULL 实例为一。
 */
static int runtime_effective_max_steps(const cc_agent_runtime_t *runtime)
{
    if (!runtime) return 1;
    if (runtime->config.limits.max_steps > 0) return runtime->config.limits.max_steps;
    if (runtime->config.max_steps > 0) return runtime->config.max_steps;
    return 1;
}

/**
 * @brief 在输入写历史前检查其正文长度。
 * @param[in] runtime 可选借用实例，零预算表示不限。
 * @param[in] input 可选借用输入，长度按 NUL 前字节计算。
 * @return 超出正数预算为 CC_ERR_LIMIT_EXCEEDED，其余为 CC_OK。
 */
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

/**
 * @brief 在最终答复落库前检查正文长度。
 * @param[in] runtime 可选借用实例，零预算表示不限。
 * @param[in] output 可选借用正文，长度不含 NUL。
 * @param[in] label 可选借用诊断名称，NULL 使用 Output。
 * @return 超出正数预算为 CC_ERR_LIMIT_EXCEEDED，其余为 CC_OK。
 */
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

/**
 * @brief 在 provider 提供能力查询时核对文本、已配置工具和各多模态输入输出。
 * @param[in] deps 借用依赖，NULL 或缺少 capabilities 时沿兼容路径成功。
 * @param[in] config 可选借用配置，NULL 时跳过多模态配置核对。
 * @return 满足要求或未提供能力查询为 CC_OK，能力不足为 CC_ERR_UNSUPPORTED，查询错误原样返回。
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
    cc_result_free(&rc);

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

/**
 * @brief 校验能力、深拷贝配置并绑定借用依赖，完成后发布 runtime。
 * @param[in] deps 借用依赖，基础布局须完整，journal/history 尾字段按 size 读取。
 * @param[in] options 借用选项，基础配置按值复制，字符串及 MIME 列表深拷贝，身份尾字段按 size 读取。
 * @param[out] out_runtime 必需输出槽，仅成功时赋拥有实例，失败保持原值。
 * @return 成功为 CC_OK，必需参数或恢复依赖组合非法为 CC_ERR_INVALID_ARGUMENT，分配失败为 CC_ERR_OUT_OF_MEMORY，锁及能力查询错误原样返回。
 * @note 所有端口 self/vtable 和外部对象继续借用。checkpoint 要求 complete history find、已附加且必需的 journal。
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
    if (copy_rc.code == CC_OK && options->size >=
        offsetof(cc_agent_runtime_options_t,provider_name)+sizeof(options->provider_name))
        copy_rc = runtime_copy_optional_string(options->provider_name,&runtime->provider_name,"provider_name");
    if (copy_rc.code == CC_OK && options->size >=
        offsetof(cc_agent_runtime_options_t, summary_cache_namespace) + sizeof(options->summary_cache_namespace) &&
        options->summary_cache_namespace && options->summary_cache_namespace[0]) {
        const unsigned char *p = (const unsigned char *)options->summary_cache_namespace;
        uint64_t hash = UINT64_C(14695981039346656037);
        while (*p) {
            hash ^= *p++;
            hash *= UINT64_C(1099511628211);
        }
        runtime->summary_cache_namespace_hash = hash;
        runtime->summary_cache_namespace_available = 1;
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
    if (deps->size >= offsetof(cc_agent_runtime_deps_t, operation_journal) +
                      sizeof(deps->operation_journal)) {
        runtime->operation_journal = deps->operation_journal;
    }
    if (deps->size >= offsetof(cc_agent_runtime_deps_t, require_operation_journal) +
                      sizeof(deps->require_operation_journal)) {
        runtime->require_operation_journal = deps->require_operation_journal;
    }
    if (deps->size >= offsetof(cc_agent_runtime_deps_t, run_history_find) +
                      sizeof(deps->run_history_find)) {
        runtime->run_checkpoints = deps->run_checkpoints;
        runtime->run_history_find = deps->run_history_find;
    }
    if (deps->size >= offsetof(cc_agent_runtime_deps_t, run_history_visit) +
                      sizeof(deps->run_history_visit)) {
        runtime->run_history_visit = deps->run_history_visit;
    }
    if (runtime->run_checkpoints && (!runtime->run_history_find ||
        !runtime->operation_journal || !runtime->require_operation_journal)) {
        cc_agent_runtime_destroy(runtime);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,
            "Run recovery requires complete history lookup and a required operation journal");
    }
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


/**
 * @brief 通过 SessionStore 的原子批次入口同步提交借用记录。
 * @param[in] runtime 借用实例，须提供 append_records。
 * @param[in] records 调用期间借用的记录数组，各字段寿命覆盖端口调用。
 * @param[in] count 记录条数，校验及持久性由具体端口负责。
 * @return 缺少入口为 CC_ERR_STORAGE，其他结果由后端原样返回。
 */
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

/**
 * @brief 将一条借用消息包装为单记录批次提交。
 * @param[in] runtime 借用实例，要求有效的批次追加入口。
 * @param[in] message 调用期借用消息，NULL 也转交后端作记录校验。
 * @return runtime_append_records 的结果原样返回，消息所有权仍归调用者。
 */
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

#define RUNTIME_RECOVERY_SCAN_MAX_MESSAGES 500

typedef struct runtime_pending_tool_call {
    const cc_tool_call_t *call; /**< 借用本轮历史消息中的工具调用，扫描消息释放前有效。 */
    int resolved; /**< 已遇到后续同 ID 的 tool role 结果时置一。 */
} runtime_pending_tool_call_t;

/**
 * @brief 释放临时待核对索引数组，消息内的工具调用由原历史数组持有。
 * @param[in,out] calls 本次拥有的索引数组，可为 NULL。
 * @param[in] count 保留的计数参数，本实现无需逐项释放借用字段。
 * @post 索引数组已释放，原消息及其工具调用仍归消息数组拥有者。
 */
static void runtime_pending_tool_calls_cleanup(
    runtime_pending_tool_call_t *calls,
    size_t count
)
{
    (void)count;
    if (!calls) return;
    free(calls);
}

/**
 * @brief 扫描后端返回的最多 500 条历史，定位尚无后续 tool role 结果的 assistant 工具意图。
 * @param[in] runtime 借用实例，须提供 load_messages，历史排序和条数语义沿后端。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] wanted_id 可选借用目标 ID，NULL 选择返回历史范围内最早的未配对项。
 * @param[in] skip_journaled 非零时跳过已在设备账本中的同一调用。
 * @param[out] out_call 可选的无已有资源输出，基础参数合法后清零，命中时取得拥有副本。
 * @param[out] out_found 必需输出，基础参数合法后置零，仅成功命中时置一。
 * @return 成功扫描为 CC_OK，指定 ID 缺失为 CC_ERR_NOT_FOUND，参数非法为 CC_ERR_INVALID_ARGUMENT，容量、复制、读取和账本错误原样返回。
 * @note 意图先于副作用持久化，缺少结果表示仍须核对。所有临时历史消息和索引在返回前释放。
 */
static cc_result_t runtime_find_pending_tool_call(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *wanted_id,
    int skip_journaled,
    cc_tool_call_t *out_call,
    int *out_found
)
{
    if (!runtime || !session_id || !out_found) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pending tool scan");
    }
    *out_found = 0;
    if (out_call) memset(out_call, 0, sizeof(*out_call));
    if (!runtime->store.vtable || !runtime->store.vtable->load_messages) {
        return cc_result_error(CC_ERR_STORAGE, "Session store cannot scan interrupted tool calls");
    }

    cc_message_t *messages = NULL;
    size_t message_count = 0;
    cc_result_t rc = runtime->store.vtable->load_messages(
        runtime->store.self, session_id, RUNTIME_RECOVERY_SCAN_MAX_MESSAGES,
        &messages, &message_count);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);

    runtime_pending_tool_call_t *pending = NULL;
    size_t pending_count = 0;
    size_t pending_capacity = 0;
    for (size_t mi = 0; mi < message_count && rc.code == CC_OK; mi++) {
        const cc_message_t *message = &messages[mi];
        if (message->role == CC_ROLE_ASSISTANT) {
            for (size_t ti = 0; ti < message->tool_calls.count; ti++) {
                if (pending_count == pending_capacity) {
                    size_t next_capacity = pending_capacity ? pending_capacity * 2U : 8U;
                    if (next_capacity < pending_capacity ||
                        next_capacity > SIZE_MAX / sizeof(*pending)) {
                        rc = cc_result_error(CC_ERR_LIMIT_EXCEEDED,
                            "Interrupted tool scan capacity overflow");
                        break;
                    }
                    runtime_pending_tool_call_t *next = realloc(
                        pending, next_capacity * sizeof(*pending));
                    if (!next) {
                        rc = cc_result_error(CC_ERR_OUT_OF_MEMORY,
                            "Failed to grow interrupted tool scan");
                        break;
                    }
                    memset(next + pending_capacity, 0,
                        (next_capacity - pending_capacity) * sizeof(*next));
                    pending = next;
                    pending_capacity = next_capacity;
                }
                pending[pending_count].call = &message->tool_calls.items[ti];
                pending_count++;
            }
        } else if (message->role == CC_ROLE_TOOL && message->tool_call_id) {
            for (size_t pi = pending_count; pi > 0; pi--) {
                runtime_pending_tool_call_t *candidate = &pending[pi - 1U];
                if (!candidate->resolved && candidate->call && candidate->call->id &&
                    strcmp(candidate->call->id, message->tool_call_id) == 0) {
                    candidate->resolved = 1;
                    break;
                }
            }
        }
    }

    if (rc.code == CC_OK) {
        for (size_t i = 0; i < pending_count; i++) {
            if (pending[i].resolved) continue;
            if (wanted_id && (!pending[i].call || !pending[i].call->id ||
                strcmp(wanted_id, pending[i].call->id) != 0)) continue;
            int already_journaled = 0;
            if (skip_journaled && runtime->operation_journal && pending[i].call &&
                pending[i].call->id && pending[i].call->name && pending[i].call->arguments_json) {
                rc = cc_operation_journal_contains(runtime->operation_journal, session_id,
                                                   pending[i].call, &already_journaled);
                if (rc.code != CC_OK) break;
            }
            if (already_journaled) continue;
            if (out_call) {
                rc = cc_tool_call_copy(pending[i].call, out_call);
                if (rc.code != CC_OK) break;
                cc_result_free(&rc);
            }
            *out_found = 1;
            break;
        }
        if (wanted_id && !*out_found && rc.code == CC_OK) {
            rc = cc_result_error(CC_ERR_NOT_FOUND,
                "No unresolved tool call matches the supplied id");
        }
    }

    for (size_t i = 0; i < message_count; i++) cc_message_cleanup(&messages[i]);
    free(messages);
    runtime_pending_tool_calls_cleanup(pending, pending_count);
    return rc;
}

/**
 * @brief 以给定稳定消息 ID 构造 assistant 文本及思考内容并提交历史。
 * @param[in] runtime 借用实例，要求有效批次追加端口。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] id 必需且非空的借用消息 ID，checkpoint final 路径使用稳定 ID。
 * @param[in] text 可选借用正文，NULL 采用空字符串。
 * @param[in] reasoning_content 可选借用思考正文，非空时复制进消息。
 * @return 非法必需参数为 CC_ERR_INVALID_ARGUMENT，消息构造、复制或存储错误原样返回。
 * @note 临时消息始终清理，调用者须在使用前确认本正文已达到完整答复提交条件。
 */
static cc_result_t runtime_store_assistant_text_with_id(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *id,
    const char *text,
    const char *reasoning_content
)
{
    if (!runtime || !session_id || !id || !id[0]) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null assistant text argument");
    }
    cc_message_t *assistant_msg = NULL;
    cc_result_t rc = cc_message_create_text(
        id, session_id, CC_ROLE_ASSISTANT, text ? text : "", NULL, &assistant_msg);
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

/**
 * @brief 生成随机消息 ID 后委托完整 assistant 消息保存。
 * @param[in] runtime 借用实例，保存入口校验其有效性。
 * @param[in] session_id 借用会话标识。
 * @param[in] text 可选借用正文，NULL 采用空字符串。
 * @param[in] reasoning_content 可选借用思考正文。
 * @return UUID 生成或消息保存结果原样返回，本次拥有 ID 在返回前释放。
 */
cc_result_t cc_agent_runtime_store_assistant_text(
    cc_agent_runtime_t *runtime, const char *session_id, const char *text,
    const char *reasoning_content)
{
    char *id = NULL;
    cc_result_t rc = generate_id(&id);
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = runtime_store_assistant_text_with_id(runtime, session_id, id, text, reasoning_content);
    }
    free(id);
    return rc;
}

/**
 * @brief 构造含工具产物摘要和媒体引用的 USER 观察消息，交由调用者决定提交批次。
 * @param[in] runtime 借用实例，NULL 时成功返回空输出。
 * @param[in] session_id 借用会话标识，NULL 时成功返回空输出。
 * @param[in] tool_result 借用结果，NULL 或无产物时成功返回空输出。
 * @param[in] stable_id 可选借用消息 ID，NULL 时生成本次随机 ID。
 * @param[in] tool_call_id 可选借用工具调用来源 ID，消息创建时复制。
 * @param[out] out_message 必需的无已有资源输出槽，进入后置空，成功拥有消息由调用者销毁。
 * @return 构造成功或无产物为 CC_OK，摘要、缓冲、UUID、内容复制和消息构造错误原样返回。
 */
static cc_result_t runtime_create_artifact_observation(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_result_t *tool_result,
    const char *stable_id,
    const char *tool_call_id,
    cc_message_t **out_message
)
{
    *out_message = NULL;
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
    rc = stable_id ? cc_result_ok() : generate_id(&id);
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = cc_message_create_parts(stable_id ? stable_id : id, session_id,
                                     CC_ROLE_USER, &parts, tool_call_id, &msg);
    }
    free(id);
    cc_content_parts_cleanup(&parts);
    free(content);
    if (rc.code == CC_OK) *out_message = msg;
    else cc_message_destroy(msg);
    return rc;
}

/**
 * @brief 构造工具产物观察消息并追加到历史。
 * @param[in] runtime 借用实例。
 * @param[in] session_id 借用会话标识。
 * @param[in] tool_call_id 可选借用来源工具调用 ID。
 * @param[in] tool_result 借用工具结果，无产物时成功跳过。
 * @return 消息构造或追加结果原样返回，临时观察消息始终销毁。
 */
static cc_result_t cc_agent_runtime_append_artifact_observation(
    cc_agent_runtime_t *runtime, const char *session_id, const char *tool_call_id,
    const cc_tool_result_t *tool_result)
{
    cc_message_t *message = NULL;
    cc_result_t rc = runtime_create_artifact_observation(runtime, session_id, tool_result,
                                                        NULL, tool_call_id, &message);
    if (rc.code == CC_OK && message) rc = runtime_append_message(runtime, message);
    cc_message_destroy(message);
    return rc;
}


/**
 * @brief 先提交工具意图，再经守卫执行并持久化调用、结果和必要的未执行说明。
 * @param[in] runtime 借用实例，依赖的 journal、store 和 executor 在调用期间保持有效。
 * @param[in] session_id 必需借用会话标识，外层负责同会话写入串行化。
 * @param[in] call 必需借用调用描述，ID、名称和参数须满足执行器与账本契约。
 * @param[in] reasoning_content 可选借用模型思考，复制到意图消息。
 * @param[in] cancel_token 可选借用取消 token，转交执行器。
 * @param[in] deadline_ms 绝对单调毫秒截止时间，零表示无限期。
 * @param[out] out_manual_result 可选的空资源输出，仅完整成功且无派发错误时接管本次工具结果。
 * @param[in] trusted_tool 可选借用显式工具实现，转交守卫执行器，寿命覆盖调用。
 * @param[in] run_id 可选借用观测运行标识。
 * @param[in] step 工具所属模型步骤序号。
 * @return 必需参数非法为 CC_ERR_INVALID_ARGUMENT，恢复屏障为 CC_ERR_INVALID_STATE，其余构造、执行、历史或账本结果原样返回。
 * @note 执行后错误可保留 UNKNOWN 账本，同名工具继续阻断。结果批次成功后再 finish 账本，artifact 观察另行追加。
 * 派发失败且明确未执行时保存用户说明。返回错误前也可能已持久化意图或结果，临时消息和未移交结果均清理。
 */
static cc_result_t runtime_execute_tool_step(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_call_t *call,
    const char *reasoning_content,
    cc_cancel_token_t *cancel_token,
    uint64_t deadline_ms,
    cc_tool_result_t *out_manual_result,
    const cc_tool_t *trusted_tool,
    const char *run_id,
    int step
)
{
    if (!runtime || !session_id || !call) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool step argument");
    }
    /* 恢复屏障在保存意图前检查，阻断请求维持未开始派发的状态。 */
    if (cc_agent_runtime_recovery_blocks_tools(runtime)) {
        return cc_result_error(CC_ERR_INVALID_STATE,
            "Tool execution is unavailable because operation recovery storage is not ready");
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

    /* 先持久化意图，再进入可能产生外部副作用的工具执行器。 */
    rc = runtime_append_message(runtime, asst_msg);
    if (rc.code != CC_OK) {
        cc_message_destroy(asst_msg);
        return rc;
    }
    cc_json_value_t *intent_attrs = cc_json_create_object();
    if (intent_attrs) {
        cc_json_object_set_owned(intent_attrs, "tool_call_id",
            cc_json_create_string(call->id ? call->id : ""));
        cc_json_object_set_owned(intent_attrs, "tool_name",
            cc_json_create_string(call->name ? call->name : ""));
        char *intent_json = cc_json_stringify_unformatted(intent_attrs);
        cc_json_destroy(intent_attrs);
        if (intent_json) {
            runtime_publish_observability_with_run(runtime, CC_OBS_EVENT_TOOL_INTENT,
                session_id, run_id, step, "intent", NULL, NULL, intent_json);
            free(intent_json);
        }
    }

    cc_tool_result_t tool_result;
    memset(&tool_result, 0, sizeof(tool_result));
    cc_tool_executor_options_t exec_options;
    memset(&exec_options, 0, sizeof(exec_options));
    exec_options.cancel_token = cancel_token;
    exec_options.deadline_ms = deadline_ms;
    cc_tool_dispatch_checkpoint_t checkpoint = {0};
    rc = cc_tool_executor_execute_guarded(
        runtime, session_id, call, &exec_options, &tool_result, &checkpoint, trusted_tool, run_id, step);
    cc_result_t dispatch_error = cc_result_ok();
    if (rc.code != CC_OK) {
        if (checkpoint.operation_id || checkpoint.not_dispatched) {
            if (!checkpoint.not_dispatched) {
                cc_result_t mark_rc = cc_operation_journal_mark_unknown(
                    runtime->operation_journal, checkpoint.operation_id);
                cc_result_free(&mark_rc);
                checkpoint.outcome_unknown = 1;
            }
            dispatch_error = rc;
            memset(&rc, 0, sizeof(rc));
            rc = cc_result_ok();
            tool_result.ok = 0;
            tool_result.error = cc_copy_string(dispatch_error.message ?
                dispatch_error.message : "Tool execution did not return a confirmed result");
            if (!tool_result.error) {
                cc_result_free(&dispatch_error);
                free(checkpoint.operation_id);
                cc_message_destroy(asst_msg);
                cc_tool_result_cleanup(&tool_result);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot report unknown tool outcome");
            }
        } else {
            free(checkpoint.operation_id);
            cc_message_destroy(asst_msg);
            cc_tool_result_cleanup(&tool_result);
            return rc;
        }
    }

    if (checkpoint.operation_id && checkpoint.outcome_unknown) {
        cc_string_builder_t notice;
        rc = cc_string_builder_init(&notice);
        if (rc.code == CC_OK) {
            rc = cc_string_builder_append(&notice,
                "The tool outcome is unknown and its name is disabled until a user verifies it. ");
        }
        if (rc.code == CC_OK && tool_result.error) {
            rc = cc_string_builder_append(&notice, tool_result.error);
        }
        if (rc.code != CC_OK) {
            cc_string_builder_deinit(&notice);
            cc_result_free(&dispatch_error);
            free(checkpoint.operation_id);
            cc_message_destroy(asst_msg);
            cc_tool_result_cleanup(&tool_result);
            return rc;
        }
        char *text = cc_string_builder_take(&notice);
        if (!text) {
            cc_result_free(&dispatch_error);
            free(checkpoint.operation_id);
            cc_message_destroy(asst_msg);
            cc_tool_result_cleanup(&tool_result);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot report unknown tool outcome");
        }
        free(tool_result.error);
        tool_result.error = text;
        tool_result.ok = 0;
    }

    cc_message_t *tool_msg = NULL;
    cc_message_t *non_execution_note = NULL;
    char *tid = NULL;
    rc = generate_id(&tid);
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = cc_message_create_text(tid, session_id, CC_ROLE_TOOL,
            tool_result.ok ? tool_result.text : tool_result.error,
            call->id, &tool_msg);
    }
    free(tid);
    if (rc.code == CC_OK && checkpoint.operation_id && checkpoint.outcome_unknown)
        tool_msg->tool_outcome = CC_MESSAGE_TOOL_OUTCOME_UNKNOWN;
    if (rc.code == CC_OK && checkpoint.not_dispatched && dispatch_error.code != CC_OK) {
        char *note_id = NULL;
        rc = generate_id(&note_id);
        if (rc.code == CC_OK)
            rc = cc_message_create_text(note_id, session_id, CC_ROLE_USER,
                runtime_non_execution_note,
                call->id, &non_execution_note);
        free(note_id);
        if (rc.code == CC_OK)
            non_execution_note->tool_outcome = CC_MESSAGE_TOOL_OUTCOME_RECONCILED_NOT_EXECUTED;
    }
    if (rc.code == CC_OK) {
        cc_session_record_t records[4];
        memset(records, 0, sizeof(records));
        records[0].type = CC_SESSION_RECORD_TOOL_CALL;
        records[0].session_id = session_id;
        records[0].data.tool_call = call;
        records[1].type = CC_SESSION_RECORD_TOOL_RESULT;
        records[1].session_id = session_id;
        records[1].data.tool_result.tool_call_id = call->id;
        records[1].data.tool_result.result = &tool_result;
        records[2].type = CC_SESSION_RECORD_MESSAGE;
        records[2].session_id = session_id;
        records[2].data.message = tool_msg;
        if (non_execution_note) {
            records[3].type = CC_SESSION_RECORD_MESSAGE;
            records[3].session_id = session_id;
            records[3].data.message = non_execution_note;
        }
        rc = runtime_append_records(runtime, records, non_execution_note ? 4 : 3);
    }
    if (checkpoint.operation_id) {
        if (rc.code == CC_OK && !checkpoint.outcome_unknown) {
            cc_result_free(&rc);
            rc = cc_operation_journal_finish(runtime->operation_journal, checkpoint.operation_id);
        } else if (rc.code != CC_OK) {
            cc_result_t mark_rc = cc_operation_journal_mark_unknown(
                runtime->operation_journal, checkpoint.operation_id);
            cc_result_free(&mark_rc);
        }
    }
    free(checkpoint.operation_id);
    cc_message_destroy(asst_msg);
    cc_message_destroy(tool_msg);
    cc_message_destroy(non_execution_note);
    if (rc.code == CC_OK) {
        rc = cc_agent_runtime_append_artifact_observation(
            runtime, session_id, call->id, &tool_result);
    }
    if (rc.code == CC_OK && dispatch_error.code == CC_OK && out_manual_result) {
        *out_manual_result = tool_result;
        memset(&tool_result, 0, sizeof(tool_result));
    }
    cc_tool_result_cleanup(&tool_result);
    if (rc.code == CC_OK && dispatch_error.code != CC_OK) return dispatch_error;
    cc_result_free(&dispatch_error);
    return rc.code == CC_OK ? cc_result_ok() : rc;
}

/**
 * @brief 使用注册表工具执行默认步骤，委托统一工具提交链。
 * @param[in] runtime 借用实例。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] call 必需借用工具调用。
 * @param[in] reasoning_content 可选借用思考内容。
 * @param[in] cancel_token 可选借用取消 token。
 * @param[in] deadline_ms 绝对单调毫秒截止时间，零表示无限期。
 * @return 工具执行及历史、账本提交结果原样返回，部分持久化语义沿统一工具步骤。
 */
cc_result_t cc_agent_runtime_execute_tool_step(
    cc_agent_runtime_t *runtime, const char *session_id, const cc_tool_call_t *call,
    const char *reasoning_content, cc_cancel_token_t *cancel_token, uint64_t deadline_ms)
{
    return runtime_execute_tool_step(runtime, session_id, call, reasoning_content,
                                     cancel_token, deadline_ms, NULL, NULL, NULL, 0);
}

/**
 * @brief 对附加账本的手动调用检查 ID 是否已出现在完整会话历史。
 * @param[in] runtime 借用实例，无 journal 时仅检查 ID 非空。
 * @param[in] session_id 借用会话标识。
 * @param[in] id 必需且非空的借用调用 ID。
 * @return 新 ID 为 CC_OK，空 ID 为 CC_ERR_INVALID_ARGUMENT，已使用为 CC_ERR_INVALID_STATE，完整历史查询错误原样返回。
 * @note 优先使用 run_history_find，否则要求 load_messages(-1) 提供完整历史，临时消息始终清理。
 */
static cc_result_t runtime_check_manual_tool_id(cc_agent_runtime_t *runtime,
                                                const char *session_id, const char *id)
{
    if (!id || !id[0]) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Manual tool call id is required");
    if (!runtime->operation_journal) return cc_result_ok();
    int found = 0;
    cc_result_t rc = cc_result_ok();
    if (runtime->run_history_find) {
        cc_message_t message = {0};
        rc = runtime->run_history_find(&runtime->store, session_id, CC_RUN_HISTORY_TOOL_INTENT,
                                       id, &message, &found);
        cc_message_cleanup(&message);
        if (rc.code == CC_OK && !found) {
            rc = runtime->run_history_find(&runtime->store, session_id, CC_RUN_HISTORY_TOOL_RESULT,
                                           id, &message, &found);
            cc_message_cleanup(&message);
        }
    } else if (runtime->store.vtable && runtime->store.vtable->load_messages) {
        cc_message_t *messages = NULL;
        size_t count = 0;
        rc = runtime->store.vtable->load_messages(runtime->store.self, session_id, -1, &messages, &count);
        for (size_t i = 0; rc.code == CC_OK && i < count && !found; i++) {
            cc_message_t *message = &messages[i];
            if (message->tool_call_id && strcmp(message->tool_call_id, id) == 0) found = 1;
            if (message->role == CC_ROLE_ASSISTANT) {
                for (size_t j = 0; j < message->tool_calls.count; j++) {
                    if (message->tool_calls.items[j].id && strcmp(message->tool_calls.items[j].id, id) == 0) {
                        found = 1;
                        break;
                    }
                }
            }
        }
        for (size_t i = 0; i < count; i++) cc_message_cleanup(&messages[i]);
        free(messages);
    } else {
        rc = cc_result_error(CC_ERR_STORAGE, "Manual tool history cannot be checked");
    }
    if (rc.code == CC_OK && found)
        return cc_result_error(CC_ERR_INVALID_STATE, "Manual tool call id was already used in this session");
    return rc;
}

/**
 * @brief 为手动工具取得运行准入及可选会话 claim，再检查 ID 并执行统一工具步骤。
 * @param[in,out] runtime 借用实例，临时增减 active_runs，有 checkpoint 时使用随机 ID 取得会话 claim。
 * @param[in] session_id 必需借用的已有会话标识。
 * @param[in] call 必需借用工具调用。
 * @param[in] trusted_tool 可选借用显式实现，须与调用名称一致，外部入口负责其 journal 要求。
 * @param[in] cancel_token 可选借用取消 token。
 * @param[out] out_result 必需无已有资源输出，基础参数合法后清零，完整成功时取得拥有字段。
 * @return 参数、恢复屏障、准入、claim、ID 校验或工具提交错误原样返回，结束时归还准入与已取得的 claim。
 * @note claim 释放失败可能在工具结果已交付后返回，调用者无论返回码都须清理 out_result。
 */
static cc_result_t runtime_execute_manual_tool(
    cc_agent_runtime_t *runtime, const char *session_id, const cc_tool_call_t *call,
    const cc_tool_t *trusted_tool, cc_cancel_token_t *cancel_token, cc_tool_result_t *out_result)
{
    if (!runtime || !session_id || !call || !out_result)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid manual tool request");
    memset(out_result, 0, sizeof(*out_result));
    if (cc_agent_runtime_recovery_blocks_tools(runtime))
        return cc_result_error(CC_ERR_INVALID_STATE,
            "Tool execution is unavailable because operation recovery storage is not ready");
    if (trusted_tool) {
        if (!call->name || !trusted_tool->vtable || !trusted_tool->vtable->name || !trusted_tool->vtable->call)
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid trusted manual tool");
        const char *name=trusted_tool->vtable->name(trusted_tool->self);
        if (!name || strcmp(name,call->name)!=0)
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Manual tool name does not match the call");
    }
    cc_result_t rc = runtime_enter_run(runtime);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    char *claim_id = NULL;
    int claimed = 0;
    if (runtime->run_checkpoints) {
        rc = cc_id_generate_uuid_v4("run-", &claim_id);
        if (rc.code == CC_OK) {
            rc = cc_run_checkpoint_claim(runtime->run_checkpoints, claim_id, session_id);
            if (rc.code == CC_OK) claimed = 1;
        }
    }
    if (rc.code == CC_OK) rc = runtime_check_manual_tool_id(runtime, session_id, call->id);
    if (rc.code == CC_OK)
        rc = runtime_execute_tool_step(runtime, session_id, call, NULL, cancel_token, 0,
                                       out_result, trusted_tool, NULL, 0);
    if (claimed) {
        cc_result_t release_rc = cc_run_checkpoint_release(runtime->run_checkpoints, claim_id);
        if (rc.code == CC_OK && release_rc.code != CC_OK) rc = release_rc;
        else cc_result_free(&release_rc);
    }
    free(claim_id);
    runtime_leave_run(runtime);
    return rc;
}

/**
 * @brief 从模型注册表选择手动工具，委托统一手动入口。
 * @param[in,out] runtime 借用实例，运行名额和会话 claim 由委托入口管理。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] call 必需借用调用描述。
 * @param[out] out_result 必需的空资源输出，初始化及成功移交规则沿手动入口，所有返回路径由调用者清理。
 * @return 统一手动执行结果原样返回。
 */
cc_result_t cc_agent_runtime_execute_manual_tool(
    cc_agent_runtime_t *runtime, const char *session_id, const cc_tool_call_t *call,
    cc_tool_result_t *out_result)
{ return runtime_execute_manual_tool(runtime,session_id,call,NULL,NULL,out_result); }

/**
 * @brief 要求已附加账本后执行借用的应用显式工具实现。
 * @param[in,out] runtime 借用实例，须有 operation_journal。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] call 必需借用调用描述，名称须与实现一致。
 * @param[in] trusted_tool 必需借用实现，生命周期覆盖整个同步调用。
 * @param[out] out_result 非 NULL 时先清零，成功取得拥有结果，调用者在所有返回路径清理。
 * @return 空实现为 CC_ERR_INVALID_ARGUMENT，缺少实例或账本为 CC_ERR_INVALID_STATE，其他结果由手动入口返回。
 */
cc_result_t cc_agent_runtime_execute_manual_tool_with_impl(
    cc_agent_runtime_t *runtime, const char *session_id, const cc_tool_call_t *call,
    const cc_tool_t *trusted_tool, cc_tool_result_t *out_result)
{
    if (out_result) memset(out_result, 0, sizeof(*out_result));
    if (!trusted_tool) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Trusted manual tool is required");
    if (!runtime || !runtime->operation_journal)
        return cc_result_error(CC_ERR_INVALID_STATE, "Trusted manual tool requires an operation journal");
    return runtime_execute_manual_tool(runtime,session_id,call,trusted_tool,NULL,out_result);
}

/**
 * @brief 校验取消选项和显式工具的账本要求，再执行手动工具。
 * @param[in,out] runtime 借用实例。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] call 必需借用调用描述。
 * @param[in] trusted_tool 可选借用显式实现，非 NULL 时要求已附加 journal。
 * @param[in] options 可选借用选项，存在时 size 须覆盖当前结构，token 覆盖本次调用。
 * @param[out] out_result 非 NULL 时先清零，拥有字段在返回后由调用者清理。
 * @return 选项尺寸非法为 CC_ERR_INVALID_ARGUMENT，显式实现缺少账本为 CC_ERR_INVALID_STATE，其他手动执行结果原样返回。
 */
cc_result_t cc_agent_runtime_execute_manual_tool_with_options(
    cc_agent_runtime_t *runtime, const char *session_id, const cc_tool_call_t *call,
    const cc_tool_t *trusted_tool, const cc_agent_runtime_run_options_t *options,
    cc_tool_result_t *out_result)
{
    if (out_result) memset(out_result,0,sizeof(*out_result));
    if (options && options->size<sizeof(*options))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid manual tool options");
    if (trusted_tool && (!runtime || !runtime->operation_journal))
        return cc_result_error(CC_ERR_INVALID_STATE,"Trusted manual tool requires an operation journal");
    return runtime_execute_manual_tool(runtime,session_id,call,trusted_tool,
                                       options ? options->cancel_token : NULL,out_result);
}

/* 单次模型调用的回调状态由外层同步栈帧拥有，provider 须串行回调并在返回前结束所有回调。 */
#define CC_STREAM_MAX_TOOL_CALLS 8

typedef struct stream_tool_slot {
    int used; /**< 元数据及参数 builder 已完整初始化。 */
    int ended; /**< 已收到本槽的工具结束事件。 */
    int provider_index; /**< provider 工具索引，负数表示未提供。 */
    char *name; /**< 拥有的工具名称副本。 */
    char *id; /**< 拥有的 provider ID，可先为空再由后续 start 补齐。 */
    cc_string_builder_t arguments; /**< 本槽拥有的参数增量缓冲。 */
} stream_tool_slot_t;

typedef struct {
    cc_agent_runtime_t *runtime; /**< 借用的运行 generation，外层租约覆盖全部回调。 */
    const char *session_id; /**< 借用的会话标识。 */
    cc_run_checkpoint_t *checkpoint; /**< 可选借用的已 claim 检查点，工具执行可推进并保存。 */
    int step; /**< 本次模型步骤的一基序号。 */
    int chunk_count; /**< 本步骤已转发到普通片段处理链的事件数，USAGE 单独处理。 */
    int usage_event_seen; /**< 已取得本次同步响应或有效 usage 事件。 */
    cc_llm_usage_t observed_usage; /**< 本步骤累计字段视图，流片段按字段覆盖，不按事件数累加。 */
    cc_string_builder_t text_builder; /**< 本步骤拥有的答复增量缓冲。 */
    cc_string_builder_t thinking_builder; /**< 本步骤拥有的思考增量缓冲。 */
    stream_tool_slot_t tools[CC_STREAM_MAX_TOOL_CALLS]; /**< 本步骤最多八个拥有的工具槽。 */
    size_t tool_count; /**< 已使用的工具槽数量。 */
    int has_tool_call; /**< 本步骤已创建工具槽，provider 返回后进入工具路径。 */
    int finished; /**< 已观察 FINISHED，取消或流预算超限也会置位。 */
    int cancelled; /**< 回调或工具入口观察到取消。 */
    int limit_exceeded; /**< 已超过本次流内容字节预算。 */
    cc_cancel_token_t *cancel_token; /**< 本轮借用 token，拥有者覆盖整个同步调用。 */
    uint64_t deadline_ms; /**< 运行的绝对单调毫秒截止时间，零表示无限期。 */
    cc_agent_runtime_stream_callback_fn on_chunk; /**< 可选借用的应用同步回调。 */
    void *chunk_user_data; /**< 借用的应用回调上下文。 */
    size_t emitted_stream_bytes; /**< 正数流预算下累计的 content 字节数，跨步骤保留。 */
    size_t max_stream_bytes; /**< 本次运行流内容预算，零表示不限。 */
    char *response_text; /**< 保留的答复指针槽，当前引擎通过 out_response 交付拥有正文。 */
} stream_loop_ctx_t;

/**
 * @brief 从本轮借用检查点取得观测运行 ID。
 * @param[in] ctx 可选借用回调上下文。
 * @return 检查点拥有的借用 ID，无上下文或检查点时为 NULL。
 */
static const char *stream_run_id(const stream_loop_ctx_t *ctx)
{ return ctx && ctx->checkpoint ? ctx->checkpoint->run_id : NULL; }

/**
 * @brief 清理全部工具槽的参数 builder、名称和 ID，并重置工具计数。
 * @param[in,out] ctx 本轮上下文，可为 NULL，调用时各回调已退出。
 * @post 工具槽清零，tool_count 和 has_tool_call 清零，文本及思考 builder 继续保留。
 */
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

/**
 * @brief 清理本轮拥有的全部工具、文本和思考缓冲。
 * @param[in,out] ctx 本轮上下文，可为 NULL，provider 与应用回调均已结束。
 * @post 三类 builder 及工具元数据已释放，借用 runtime、checkpoint、token 和应用上下文仍由外层管理。
 */
static void stream_loop_ctx_cleanup(stream_loop_ctx_t *ctx)
{
    if (!ctx) return;
    stream_tool_slots_cleanup(ctx);
    cc_string_builder_deinit(&ctx->text_builder);
    cc_string_builder_deinit(&ctx->thinking_builder);
}

/**
 * @brief 按非负 provider 索引或非空工具 ID 查找已使用槽。
 * @param[in,out] ctx 借用的串行回调状态，NULL 返回 NULL。
 * @param[in] provider_index provider 索引，负数跳过索引匹配。
 * @param[in] tool_id 可选借用工具 ID，空字符串跳过 ID 匹配。
 * @return 借用的首个匹配槽，无匹配为 NULL，槽清理后失效。
 */
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

/**
 * @brief 核对已有开放工具的身份，或完整初始化一个新工具槽。
 * @param[in,out] ctx 借用的本轮串行上下文，最多八个工具槽。
 * @param[in] chunk 借用 start 片段，必须提供非空工具名，索引和 ID 用于关联。
 * @param[out] out_new 可选输出，进入后置 false，只有本次新增槽成功时置 true。
 * @return 成功为 CC_OK，缺名、结束后重启或身份变化为 CC_ERR_MODEL，槽满为 CC_ERR_LIMIT_EXCEEDED，分配及 builder 错误原样返回。
 * @note 已有空 ID 可由后续 start 补齐，其他元数据冲突保留原槽，本次新增失败清理部分资源。
 */
static cc_result_t stream_start_tool(
    stream_loop_ctx_t *ctx,
    const cc_stream_chunk_t *chunk,
    bool *out_new)
{
    if (out_new) *out_new = false;
    if (!ctx || !chunk || !chunk->tool_name || !chunk->tool_name[0]) {
        return cc_result_error(CC_ERR_MODEL, "Streaming tool start is missing a name");
    }
    stream_tool_slot_t *existing = stream_find_tool_slot(ctx, chunk->tool_index, chunk->tool_id);
    if (existing) {
        if (existing->ended) {
            return cc_result_error(CC_ERR_MODEL, "Streaming tool start arrived after tool end");
        }
        if (strcmp(existing->name ? existing->name : "", chunk->tool_name) != 0) {
            return cc_result_error(CC_ERR_MODEL, "Streaming tool name changed for an open call");
        }
        if (chunk->tool_index >= 0 && existing->provider_index >= 0 &&
            existing->provider_index != chunk->tool_index) {
            return cc_result_error(CC_ERR_MODEL, "Streaming tool index changed for an open call");
        }
        if (chunk->tool_id && chunk->tool_id[0] && existing->id && existing->id[0] &&
            strcmp(existing->id, chunk->tool_id) != 0) {
            return cc_result_error(CC_ERR_MODEL, "Streaming tool id changed for an open call");
        }
        if (chunk->tool_id && chunk->tool_id[0] && existing->id && !existing->id[0]) {
            char *id = cc_copy_string(chunk->tool_id);
            if (!id) return cc_result_error(CC_ERR_OUT_OF_MEMORY,
                                             "Failed to copy streaming tool id");
            free(existing->id);
            existing->id = id;
        }
        return cc_result_ok();
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
    if (out_new) *out_new = true;
    return cc_result_ok();
}



/**
 * @brief 比较工具调用的完整 ID、名称和参数原文。
 * @param[in] a 借用调用，可为 NULL。
 * @param[in] b 借用调用，可为 NULL。
 * @return 两个调用及三字段均非 NULL 且逐字节相同为一，其余为零。
 */
static int runtime_tool_call_equal(const cc_tool_call_t *a, const cc_tool_call_t *b)
{
    return a && b && a->id && b->id && a->name && b->name && a->arguments_json && b->arguments_json &&
        strcmp(a->id, b->id) == 0 && strcmp(a->name, b->name) == 0 &&
        strcmp(a->arguments_json, b->arguments_json) == 0;
}

/**
 * @brief 从完整历史查询意图，并核对会话、角色和唯一的完整调用身份。
 * @param[in] runtime 借用实例，须有 run_history_find。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] call 借用的已验证 checkpoint 调用，三字段均有效。
 * @param[out] out_found 必需输出，进入后置零，查询结果沿回调填写，错误时仍须看返回码。
 * @return 查询及身份验证成功为 CC_OK，身份、数量或会话不符为 CC_ERR_STORAGE，查询错误原样返回。
 * @note 临时消息在所有返回路径清理，found 为一仅在返回成功时可用于推进。
 */
static cc_result_t runtime_find_verified_tool_intent(
    cc_agent_runtime_t *runtime, const char *session_id, const cc_tool_call_t *call, int *out_found)
{
    cc_message_t message = {0};
    *out_found = 0;
    cc_result_t rc = runtime->run_history_find(&runtime->store, session_id,
        CC_RUN_HISTORY_TOOL_INTENT, call->id, &message, out_found);
    if (rc.code == CC_OK && *out_found) {
        size_t matches = 0;
        if (message.role != CC_ROLE_ASSISTANT || !message.session_id ||
            (message.tool_calls.count && !message.tool_calls.items) ||
            strcmp(message.session_id, session_id) != 0)
            rc = cc_result_error(CC_ERR_STORAGE, "Historical tool intent has invalid identity");
        for (size_t i = 0; rc.code == CC_OK && i < message.tool_calls.count; i++) {
            const cc_tool_call_t *old = &message.tool_calls.items[i];
            if (!old->id || strcmp(old->id, call->id) != 0) continue;
            if (!runtime_tool_call_equal(old, call) || ++matches > 1U)
                rc = cc_result_error(CC_ERR_STORAGE, "Historical tool intent does not match checkpoint");
        }
        if (rc.code == CC_OK && matches != 1U)
            rc = cc_result_error(CC_ERR_STORAGE, "Historical tool intent is missing or ambiguous");
    }
    cc_message_cleanup(&message);
    return rc;
}

/**
 * @brief 从检查点 next_call 继续工具批次，按账本和历史证据跳过已完成节点或阻断未知结果。
 * @param[in,out] ctx 有效上下文，checkpoint 须处于 TOOLS，借用实例须具备完整历史和 journal。
 * @return 全部完成并保存 MODEL 下一步为 CC_OK，等待核对为 CC_ERR_INVALID_STATE，证据不符为 CC_ERR_STORAGE，取消、超时和依赖错误原样返回。
 * @note 每个确认完成节点先推进 next_call 再保存，已核实未执行的节点生成新 ID 保存后重试。
 * 遇到孤立历史意图时导入 UNKNOWN 并返回，恢复时依赖完整身份核对而非仅计数。
 */
static cc_result_t checkpoint_execute_tools(stream_loop_ctx_t *ctx)
{
    cc_run_checkpoint_t *checkpoint = ctx->checkpoint;
    cc_agent_runtime_t *runtime = ctx->runtime;
    if (!checkpoint || checkpoint->phase != CC_RUN_CHECKPOINT_TOOLS ||
        !runtime->run_history_find || !runtime->operation_journal)
        return cc_result_error(CC_ERR_INVALID_STATE, "Run tool checkpoint is unavailable");

    while (checkpoint->next_call < checkpoint->planned_calls.count) {
        cc_result_t rc = check_run_state(ctx->cancel_token, ctx->deadline_ms,
                                          "Checkpointed tool call cancelled");
        if (rc.code != CC_OK) return rc;
        cc_tool_call_t *call = &checkpoint->planned_calls.items[checkpoint->next_call];
        int pending = 0;
        if (checkpoint->retry_after_call_id) {
            cc_tool_call_t prior = {checkpoint->retry_after_call_id, call->name, call->arguments_json};
            rc = cc_operation_journal_contains(runtime->operation_journal, ctx->session_id, &prior, &pending);
            if (rc.code != CC_OK) return rc;
            int name_blocked = 0;
            rc = cc_operation_journal_is_blocked(runtime->operation_journal, call->name, &name_blocked);
            if (rc.code != CC_OK) return rc;
            if (pending || name_blocked)
                return cc_result_error(CC_ERR_INVALID_STATE, "Verified retry is waiting for tool reconciliation");
            free(checkpoint->retry_after_call_id);
            checkpoint->retry_after_call_id = NULL;
            rc = cc_run_checkpoint_save(runtime->run_checkpoints, checkpoint);
            if (rc.code != CC_OK) return rc;
        }
        rc = cc_operation_journal_contains(runtime->operation_journal, ctx->session_id, call, &pending);
        if (rc.code != CC_OK) return rc;
        if (pending)
            return cc_result_error(CC_ERR_INVALID_STATE, "Tool outcome requires user reconciliation");

        cc_message_t found_message = {0};
        int found = 0;
        rc = runtime->run_history_find(&runtime->store, ctx->session_id,
                                      CC_RUN_HISTORY_TOOL_RESULT, call->id,
                                      &found_message, &found);
        if (rc.code != CC_OK) { cc_message_cleanup(&found_message); return rc; }
        if (found && (!found_message.session_id || strcmp(found_message.session_id, ctx->session_id) != 0 ||
            !found_message.tool_call_id || strcmp(found_message.tool_call_id, call->id) != 0 ||
            !cc_message_tool_outcome_valid(&found_message))) {
            cc_message_cleanup(&found_message);
            return cc_result_error(CC_ERR_STORAGE, "Recovery returned an invalid tool result");
        }
        if (found && cc_message_tool_outcome_is_unknown(&found_message)) found = 0;
        int verified_not_executed = found && found_message.role == CC_ROLE_USER &&
            found_message.tool_outcome == CC_MESSAGE_TOOL_OUTCOME_RECONCILED_NOT_EXECUTED;
        if (found && found_message.role != CC_ROLE_TOOL &&
            !(found_message.role == CC_ROLE_USER &&
              (found_message.tool_outcome == CC_MESSAGE_TOOL_OUTCOME_RECONCILED_EXECUTED || verified_not_executed))) {
            cc_message_cleanup(&found_message);
            return cc_result_error(CC_ERR_STORAGE, "Recovery returned an invalid tool result");
        }
        cc_message_cleanup(&found_message);
        int intent_found = 0;
        rc = runtime_find_verified_tool_intent(runtime, ctx->session_id, call, &intent_found);
        if (rc.code != CC_OK) return rc;
        if (found && !intent_found)
            return cc_result_error(CC_ERR_STORAGE, "Historical tool result has no matching intent");
        if (verified_not_executed) {
            char *prior_id = cc_copy_string(call->id), *retry_id = NULL;
            if (!prior_id) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot preserve reconciled tool id");
            rc = cc_id_generate_uuid_v4("tool_", &retry_id);
            if (rc.code != CC_OK) { free(prior_id); return rc; }
            free(call->id); call->id = retry_id;
            free(checkpoint->retry_after_call_id); checkpoint->retry_after_call_id = prior_id;
            rc = cc_run_checkpoint_save(runtime->run_checkpoints, checkpoint);
            if (rc.code != CC_OK) return rc;
            continue;
        }
        if (!found) {
            if (intent_found) {
                char *operation_id = NULL;
                rc = cc_operation_journal_import_unknown(runtime->operation_journal,
                                                         ctx->session_id, call, &operation_id);
                free(operation_id);
                if (rc.code != CC_OK) return rc;
                return cc_result_error(CC_ERR_INVALID_STATE, "Interrupted tool intent requires user reconciliation");
            }
            rc = runtime_execute_tool_step(runtime,ctx->session_id,call,
                checkpoint->reasoning_content,ctx->cancel_token,ctx->deadline_ms,
                NULL,NULL,stream_run_id(ctx),ctx->step);
            if (rc.code != CC_OK) return rc;
            rc = cc_operation_journal_contains(runtime->operation_journal, ctx->session_id, call, &pending);
            if (rc.code != CC_OK) return rc;
            if (pending)
                return cc_result_error(CC_ERR_INVALID_STATE, "Tool outcome requires user reconciliation");
            found = 0;
            rc = runtime->run_history_find(&runtime->store, ctx->session_id,
                                           CC_RUN_HISTORY_TOOL_RESULT, call->id,
                                           &found_message, &found);
            if (rc.code == CC_OK && found && (!found_message.session_id ||
                strcmp(found_message.session_id, ctx->session_id) != 0 || found_message.role != CC_ROLE_TOOL ||
                !found_message.tool_call_id || strcmp(found_message.tool_call_id, call->id) != 0 ||
                !cc_message_tool_outcome_valid(&found_message) || cc_message_tool_outcome_is_unknown(&found_message)))
                rc = cc_result_error(CC_ERR_STORAGE, "Completed tool result has invalid identity");
            cc_message_cleanup(&found_message);
            if (rc.code != CC_OK) return rc;
            if (!found) return cc_result_error(CC_ERR_STORAGE, "Completed tool result is missing from history");
        }
        checkpoint->next_call++;
        rc = cc_run_checkpoint_save(runtime->run_checkpoints, checkpoint);
        if (rc.code != CC_OK) return rc;
    }
    if (checkpoint->step == UINT_MAX)
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Run step overflow");
    cc_tool_call_list_cleanup(&checkpoint->planned_calls);
    free(checkpoint->reasoning_content);
    checkpoint->reasoning_content = NULL;
    checkpoint->phase = CC_RUN_CHECKPOINT_MODEL;
    checkpoint->next_call = 0;
    checkpoint->step++;
    return cc_run_checkpoint_save(runtime->run_checkpoints, checkpoint);
}

/**
 * @brief provider 返回后核对所有工具完整性，再按 checkpoint 或普通路径依次执行。
 * @param[in,out] ctx 有效的本轮上下文，至少有一个工具槽，工具副作用只在 provider 回调结束后发生。
 * @return 成功为 CC_OK，空上下文或无工具为 CC_ERR_INVALID_ARGUMENT，未结束槽或重复 ID 为 CC_ERR_MODEL，参数解析、执行及持久化错误原样返回。
 * @note 参数缺失时采用 {}，使用完整消费 JSON 入口并要求顶层对象。
 * checkpoint 路径统一生成稳定的本地工具 ID 并先保存计划。执行路径结束时清工具槽，前置校验失败由外层统一清理。
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
        if (slot->id && slot->id[0]) {
            for (size_t j = 0; j < i; j++) {
                const stream_tool_slot_t *prior = &ctx->tools[j];
                if (prior->used && prior->id && prior->id[0] &&
                    strcmp(prior->id, slot->id) == 0) {
                    return cc_result_error(CC_ERR_MODEL,
                        "Provider reused a tool-call id within one response");
                }
            }
        }
        const char *arguments = cc_string_builder_cstr(&slot->arguments);
        if (!arguments || !arguments[0]) arguments = "{}";
        cc_json_value_t *json = NULL;
        cc_result_t valid = cc_json_parse_strict(arguments, &json);
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
    if (ctx->checkpoint) {
        cc_run_checkpoint_t *checkpoint = ctx->checkpoint;
        if (checkpoint->phase != CC_RUN_CHECKPOINT_MODEL || checkpoint->planned_calls.count)
            return cc_result_error(CC_ERR_INVALID_STATE, "Run checkpoint is not awaiting a model result");
        checkpoint->reasoning_content = cc_copy_string(thinking ? thinking : "");
        if (!checkpoint->reasoning_content)
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy checkpoint reasoning");
        for (size_t i = 0; i < CC_STREAM_MAX_TOOL_CALLS && rc.code == CC_OK; i++) {
            stream_tool_slot_t *slot = &ctx->tools[i];
            if (!slot->used) continue;
            char *id = NULL;
            rc = cc_id_generate_uuid_v4("tool_", &id);
            if (rc.code == CC_OK) {
                const char *arguments = cc_string_builder_cstr(&slot->arguments);
                rc = cc_tool_call_list_append_values(&checkpoint->planned_calls, id, slot->name,
                                                      arguments && arguments[0] ? arguments : "{}");
            }
            free(id);
        }
        if (rc.code == CC_OK) {
            checkpoint->phase = CC_RUN_CHECKPOINT_TOOLS;
            checkpoint->next_call = 0;
            rc = cc_run_checkpoint_save(ctx->runtime->run_checkpoints, checkpoint);
        }
        if (rc.code == CC_OK) rc = checkpoint_execute_tools(ctx);
        stream_tool_slots_cleanup(ctx);
        return rc;
    }
    for (size_t i = 0; i < CC_STREAM_MAX_TOOL_CALLS && rc.code == CC_OK; i++) {
        stream_tool_slot_t *slot = &ctx->tools[i];
        if (!slot->used) continue;
        const char *arguments = cc_string_builder_cstr(&slot->arguments);
        if (!arguments || !arguments[0]) arguments = "{}";
        char *generated_tool_id = NULL;
        const char *tool_id = slot->id;
        if (!tool_id || !tool_id[0]) {
            rc = cc_id_generate_uuid_v4("tool_", &generated_tool_id);
            if (rc.code != CC_OK) break;
            cc_result_free(&rc);
            tool_id = generated_tool_id;
        }
        cc_tool_call_t call = {
            .id = (char *)tool_id,
            .name = slot->name,
            .arguments_json = (char *)arguments,
        };
        rc = runtime_execute_tool_step(
            ctx->runtime, ctx->session_id, &call,
            thinking && thinking[0] ? thinking : NULL, ctx->cancel_token,
            ctx->deadline_ms,NULL,NULL,stream_run_id(ctx),ctx->step);
        free(generated_tool_id);
        if (rc.code == CC_ERR_CANCELLED) ctx->cancelled = 1;
        runtime_publish_observability_with_run(ctx->runtime, CC_OBS_EVENT_STREAM_TOOL_END,
            ctx->session_id, stream_run_id(ctx), ctx->step, rc.code == CC_OK ? "ok" : "error",
            rc.message, rc.code == CC_OK ? NULL : &rc, NULL);
    }
    stream_tool_slots_cleanup(ctx);
    return rc;
}


/**
 * @brief 通过应用回调同步报告本地 ERROR 片段。
 * @param[in] ctx 借用上下文，NULL 或无应用回调时成功跳过。
 * @param[in] message 可选借用错误正文，NULL 使用 stream error。
 * @return 应用回调结果原样返回，无回调为 CC_OK，栈上片段及正文只在回调期借用。
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


/**
 * @brief 核对内容字节预算后同步转发片段，超限时标记并报告本地错误。
 * @param[in,out] ctx 本轮上下文，累计转发内容字节，零预算表示不限。
 * @param[in] chunk 本次借用片段，content 长度按 NUL 前字节计算。
 * @return 参数非法为 CC_ERR_INVALID_ARGUMENT，超限为 CC_ERR_LIMIT_EXCEEDED，应用回调错误原样返回，其余为 CC_OK。
 * @note content 在无应用回调时也计入预算，超限会置 limit_exceeded/finished，错误回调失败可先返回其错误。
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


/**
 * @brief 串行消费 provider 片段，合并用量、转发内容并更新文本和工具槽状态。
 * @param[in] chunk 回调期间借用片段，跨片段所需数据复制到上下文拥有缓冲。
 * @param[in,out] user_data 必需的 stream_loop_ctx_t，provider 返回前保持有效且串行访问。
 * @return 成功为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，取消为 CC_ERR_CANCELLED，恢复屏障为 CC_ERR_INVALID_STATE，模型片段冲突、预算、分配或应用回调错误原样返回。
 * @note 有效 USAGE 合并到本步骤视图并直接返回，其他片段先计预算及转发再更新状态。
 * TOOL_END 只标记完成，实际工具执行在 provider 返回后进行。结束标志后的 usage 仍可用于本步骤观测。
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

    /* provider 即使收到空 schema 也可能发工具片段，恢复屏障在应用转发前拒绝该类片段。 */
    if (cc_agent_runtime_recovery_blocks_tools(ctx->runtime) &&
        (chunk->type == CC_STREAM_CHUNK_TOOL_START || chunk->type == CC_STREAM_CHUNK_TOOL_DELTA ||
         chunk->type == CC_STREAM_CHUNK_TOOL_END)) {
        const char *reason = "Model requested a tool while operation recovery is unavailable";
        cc_result_t emit_rc = stream_loop_emit_error(ctx, reason);
        cc_result_free(&emit_rc);
        return cc_result_error(CC_ERR_INVALID_STATE, reason);
    }

    if (chunk->type == CC_STREAM_CHUNK_USAGE) {
        if (chunk->usage.valid) {
            /* 输入与缓存计数可先到、累计输出随后到，按字段合并为本步骤的一次用量观测。 */
            ctx->usage_event_seen = 1;
            ctx->observed_usage.valid = 1;
            if (chunk->usage.prompt_tokens) ctx->observed_usage.prompt_tokens = chunk->usage.prompt_tokens;
            if (chunk->usage.completion_tokens) ctx->observed_usage.completion_tokens = chunk->usage.completion_tokens;
            if (chunk->usage.cached_tokens_reported || chunk->usage.cached_tokens_valid) {
                ctx->observed_usage.cached_tokens_reported = 1;
                ctx->observed_usage.cached_tokens_valid = chunk->usage.cached_tokens_valid;
                ctx->observed_usage.cached_tokens = chunk->usage.cached_tokens;
            }
            if (chunk->usage.cache_write_tokens_reported || chunk->usage.cache_write_tokens_valid) {
                ctx->observed_usage.cache_write_tokens_reported = 1;
                ctx->observed_usage.cache_write_tokens_valid = chunk->usage.cache_write_tokens_valid;
                ctx->observed_usage.cache_write_tokens = chunk->usage.cache_write_tokens;
            }
        }
        return cc_result_ok();
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
                ctx->session_id, stream_run_id(ctx), ctx->step, "delta", chunk->content);
        }
        break;

    case CC_STREAM_CHUNK_TEXT:


        if (chunk->content && strlen(chunk->content) > 0) {
            rc = cc_string_builder_append(&ctx->text_builder, chunk->content);
            if (rc.code != CC_OK) return rc;
            runtime_publish_stream_content(ctx->runtime, CC_OBS_EVENT_STREAM_TEXT,
                ctx->session_id, stream_run_id(ctx), ctx->step, "delta", chunk->content);
        }
        break;

    case CC_STREAM_CHUNK_TOOL_START:
    {
        bool new_tool = false;
        rc = stream_start_tool(ctx, chunk, &new_tool);
        if (rc.code != CC_OK) return rc;

        if (new_tool) {
            cc_json_value_t *attrs = cc_json_create_object();
            if (attrs) {
                cc_json_object_set_owned(attrs, "tool_name",
                    cc_json_create_string(chunk->tool_name ? chunk->tool_name : ""));
                cc_json_object_set_owned(attrs, "tool_id",
                    cc_json_create_string(chunk->tool_id ? chunk->tool_id : ""));
                char *attrs_json = cc_json_stringify_unformatted(attrs);
                cc_json_destroy(attrs);
                if (attrs_json) {
                    runtime_publish_observability_with_run(ctx->runtime,
                        CC_OBS_EVENT_STREAM_TOOL_START, ctx->session_id,
                        stream_run_id(ctx), ctx->step, "started", NULL, NULL, attrs_json);
                    free(attrs_json);
                }
            }
        }
        break;
    }

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
                ctx->session_id, stream_run_id(ctx), ctx->step, "delta", chunk->content);
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
            if (open == 0) {
                return cc_result_error(CC_ERR_MODEL,
                    "Provider ended tool calls that were never open");
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
                ctx->session_id, stream_run_id(ctx), ctx->step, "artifact", chunk->content);
        }
        break;

    case CC_STREAM_CHUNK_PROVIDER_WARNING:
        if (chunk->content) {
            runtime_publish_stream_content(ctx->runtime,
                CC_OBS_EVENT_STREAM_PROVIDER_WARNING, ctx->session_id,
                stream_run_id(ctx), ctx->step, "warning", chunk->content);
        }
        break;

    case CC_STREAM_CHUNK_ERROR:
        if (chunk->content) {
            runtime_publish_stream_content(ctx->runtime, CC_OBS_EVENT_STREAM_ERROR,
                ctx->session_id, stream_run_id(ctx), ctx->step, "error", chunk->content);
        }
        break;
    case CC_STREAM_CHUNK_USAGE:
        /* USAGE 已由前置分支处理，此处保留枚举覆盖。 */
        break;
    }
    return cc_result_ok();
}


/**
 * @brief 将同步响应按思考、正文、工具、产物及 FINISHED 顺序适配为统一片段。
 * @param[in,out] ctx 本轮串行上下文，转换时使用相同预算和应用回调。
 * @param[in] response 借用的完整同步响应，工具数组及产物列表须满足 provider 响应契约。
 * @return 转换完成为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，工具超过八个为 CC_ERR_LIMIT_EXCEEDED，片段和产物序列化错误原样返回。
 * @note 回调仅累积工具，执行由外层完成，response 在整个转换期保持有效。
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


/**
 * @brief 优先同步调用 chat_stream，否则用 chat 的完整响应适配统一片段。
 * @param[in] runtime 借用实例，须有 provider vtable。
 * @param[in,out] request 借用请求，将 stream 改为实际传输模式，所有消息及媒体视图覆盖 provider 调用期。
 * @param[in,out] ctx 借用的本次回调状态，provider 及其回调返回前保持有效。
 * @return 参数非法为 CC_ERR_INVALID_ARGUMENT，无可用传输为 CC_ERR_UNSUPPORTED，初始化、provider 或转换错误原样返回。
 * @note 同步 chat 成功后复制 response.usage，响应在转换完成后释放，原生流由 usage 事件合并。
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
        ctx->usage_event_seen = 1;
        ctx->observed_usage = response.usage;
        rc = stream_loop_consume_sync_response(ctx, &response);
    }
    cc_llm_response_free(&response);
    return rc;
}


/**
 * @brief 在已取得准入及可选检查点 claim 的调用中推进模型、工具与最终答复提交。
 * @param[in,out] runtime 借用实例，所有依赖和 generation 租约覆盖同步 provider、工具及应用回调。
 * @param[in] session_id 必需借用会话标识，同会话写入由 claim 或外层串行化。
 * @param[in] user_input 必需借用原输入，checkpoint 路径要求与保留输入逐字节一致。
 * @param[in] options 可选借用同步选项，token 用于取消及本次调用截止时间检查。
 * @param[in] stream_options 可选借用流式选项，未提供同步 token 时采用其中 token，应用上下文覆盖回调。
 * @param[in,out] checkpoint 可选借用的已 claim 检查点，恢复可跳过已保存输入、工具结果或最终答复。
 * @param[out] out_response 基础参数合法后先置 NULL，成功取得拥有答复，provider 错误时也可能取得拥有诊断正文，调用者均须 free。
 * @return 成功答复或达到步数上限为 CC_OK，输入/输出/流预算、取消、超时、模型、存储、工具和检查点错误原样返回。
 * @note 每步使用独立文本/思考及工具槽，流字节预算在本次运行内累计，usage 按模型步骤发布。
 * 最终答复先分配交付副本，再写历史、删除检查点并执行可选记忆钩子。失败可保留已落库用户输入、意图或工具结果供核对。
 * 达到步数上限时 checkpoint 路径保存稳定 final，普通路径交付停止提示，外层负责 claim 释放与准入归还。
 */
static cc_result_t cc_agent_runtime_handle_message_internal(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    const cc_agent_runtime_run_options_t *options,
    const cc_agent_runtime_stream_options_t *stream_options,
    cc_run_checkpoint_t *checkpoint,
    char **out_response
)
{
    if (!runtime || !session_id || !user_input || !out_response) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "NULL argument");
    }
    const char *run_id = checkpoint ? checkpoint->run_id : NULL;
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

    char final_message_id[64] = {0};
    if (checkpoint) {
        if (checkpoint->recovered_from_backup || checkpoint->recovered_from_temporary)
            return cc_result_error(CC_ERR_STORAGE, "Run checkpoint requires manual recovery");
        if (strcmp(checkpoint->session_id, session_id) != 0 ||
            strcmp(checkpoint->user_input, user_input) != 0)
            return cc_result_error(CC_ERR_STORAGE, "Run checkpoint does not match the request");
        int written = snprintf(final_message_id, sizeof(final_message_id), "%s-final", checkpoint->run_id);
        if (written < 0 || (size_t)written >= sizeof(final_message_id))
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Run final message id is too long");
        cc_message_t final = {0};
        int found = 0;
        cc_result_t rc = runtime->run_history_find(&runtime->store, session_id,
            CC_RUN_HISTORY_MESSAGE_ID, final_message_id, &final, &found);
        if (rc.code != CC_OK) { cc_message_cleanup(&final); return rc; }
        if (found) {
            if (final.role != CC_ROLE_ASSISTANT) {
                cc_message_cleanup(&final);
                return cc_result_error(CC_ERR_STORAGE, "Run final message has an invalid role");
            }
            rc = cc_message_get_text_summary(&final, out_response);
            cc_message_cleanup(&final);
            if (rc.code != CC_OK) return rc;
            rc = cc_run_checkpoint_remove(runtime->run_checkpoints, checkpoint->run_id);
            if (rc.code != CC_OK) { free(*out_response); *out_response = NULL; }
            return rc;
        }
        cc_message_cleanup(&final);
    }

    cc_tool_call_t pending_call;
    memset(&pending_call, 0, sizeof(pending_call));
    int has_pending_call = 0;
    cc_result_t pending_rc = checkpoint && checkpoint->phase == CC_RUN_CHECKPOINT_TOOLS ?
        cc_result_ok() : runtime_find_pending_tool_call(
            runtime, session_id, NULL, 1, &pending_call, &has_pending_call);
    if (pending_rc.code != CC_OK) return pending_rc;
    cc_result_free(&pending_rc);
    if (has_pending_call) {
        cc_json_value_t *attrs = cc_json_create_object();
        if (attrs) {
            cc_json_object_set_owned(attrs, "tool_call_id",
                cc_json_create_string(pending_call.id ? pending_call.id : ""));
            cc_json_object_set_owned(attrs, "tool_name",
                cc_json_create_string(pending_call.name ? pending_call.name : ""));
            char *attrs_json = cc_json_stringify_unformatted(attrs);
            cc_json_destroy(attrs);
            if (attrs_json) {
                runtime_publish_observability_with_run(runtime,
                    CC_OBS_EVENT_TOOL_RECOVERY_REQUIRED, session_id, run_id, 0,
                    "pending", "Interrupted tool call requires reconciliation",
                    NULL, attrs_json);
                free(attrs_json);
            }
        }
        cc_tool_call_cleanup(&pending_call);
        return cc_result_error(CC_ERR_INVALID_STATE,
            "Session has an unresolved tool call; reconcile its side effect before continuing");
    }
    cc_tool_call_cleanup(&pending_call);


    cc_message_t *user_msg = NULL;
    char *msg_id = NULL;
    int user_exists = 0;
    cc_result_t rc = cc_result_ok();
    if (checkpoint) {
        cc_message_t previous = {0};
        rc = runtime->run_history_find(&runtime->store, session_id, CC_RUN_HISTORY_MESSAGE_ID,
                                       checkpoint->user_message_id, &previous, &user_exists);
        if (rc.code == CC_OK && user_exists) {
            char *existing_text = NULL;
            if (previous.role != CC_ROLE_USER)
                rc = cc_result_error(CC_ERR_STORAGE, "Run input message has an invalid role");
            else {
                rc = cc_message_get_text_summary(&previous, &existing_text);
                if (rc.code == CC_OK && (!existing_text || strcmp(existing_text, user_input) != 0))
                    rc = cc_result_error(CC_ERR_STORAGE, "Run input message does not match checkpoint");
            }
            free(existing_text);
        }
        cc_message_cleanup(&previous);
        if (rc.code != CC_OK) return rc;
        msg_id = cc_copy_string(checkpoint->user_message_id);
        if (!msg_id) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy run input id");
    } else {
        rc = generate_id(&msg_id);
    }
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    if (!user_exists)
        rc = cc_message_create_text(msg_id, session_id, CC_ROLE_USER, user_input, NULL, &user_msg);
    free(msg_id);

    if (rc.code != CC_OK) return rc;

    if (!user_exists) rc = runtime_append_message(runtime, user_msg);
    if (rc.code != CC_OK) {
        cc_message_destroy(user_msg);
        return rc;
    }
    cc_message_destroy(user_msg);



    stream_loop_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.runtime = runtime;
    ctx.session_id = session_id;
    ctx.checkpoint = checkpoint;
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

    if (checkpoint && checkpoint->phase == CC_RUN_CHECKPOINT_TOOLS) {
        ctx.step = (int)(checkpoint->step <= INT_MAX ? checkpoint->step : INT_MAX);
        rc = checkpoint_execute_tools(&ctx);
        if (rc.code != CC_OK) { stream_loop_ctx_cleanup(&ctx); return rc; }
    }
    int max_steps = runtime_effective_max_steps(runtime);
    int start_step = checkpoint ? (checkpoint->step > (unsigned)max_steps ? max_steps :
                                   (int)checkpoint->step - 1) : 0;
    for (int step = start_step; step < max_steps; step++) {
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
        ctx.usage_event_seen = 0;
        memset(&ctx.observed_usage, 0, sizeof(ctx.observed_usage));
        cc_string_builder_clear(&ctx.text_builder);
        cc_string_builder_clear(&ctx.thinking_builder);



        cc_message_t *messages = NULL;
        size_t message_count = 0;
        long stage_start_ms = runtime_clock_ms();
        rc = cc_context_builder_build_messages_for_run(runtime, session_id, run_id,
            runtime->config.system_prompt, ctx.cancel_token, &messages, &message_count);
        runtime_publish_llm_stage(runtime, CC_OBS_EVENT_LLM_CONTEXT_BUILD,
            session_id, run_id, step, rc.code == CC_OK ? "ok" : "error",
            runtime_clock_ms() - stage_start_ms, message_count,
            "message_count", 0, NULL, rc.code == CC_OK ? NULL : &rc);
        if (rc.code != CC_OK) {
            cc_result_t emit_rc = stream_loop_emit_error(
                &ctx, rc.message ? rc.message : cc_error_string(rc.code));
            cc_result_free(&emit_rc);
            runtime_publish_observability_with_run(runtime, CC_OBS_EVENT_LLM_REQUEST_START,
                session_id, run_id, step, "error", rc.message, &rc, NULL);
            stream_loop_ctx_cleanup(&ctx);
            return rc;
        }



        char *tools_json = NULL;
        stage_start_ms = runtime_clock_ms();
        if (cc_agent_runtime_recovery_blocks_tools(runtime))
            rc = cc_result_ok();
        else
            rc = cc_tool_registry_build_schema_json(runtime->tool_registry, &tools_json);
        runtime_publish_llm_stage(runtime, CC_OBS_EVENT_LLM_TOOLS_SCHEMA_BUILD,
            session_id, run_id, step, rc.code == CC_OK ? "ok" : "error",
            runtime_clock_ms() - stage_start_ms,
            tools_json ? strlen(tools_json) : 0, "tools_json_bytes",
            0, NULL, rc.code == CC_OK ? NULL : &rc);
        if (rc.code != CC_OK) {
            cc_result_t emit_rc = stream_loop_emit_error(
                &ctx, rc.message ? rc.message : cc_error_string(rc.code));
            cc_result_free(&emit_rc);
            runtime_publish_observability_with_run(runtime, CC_OBS_EVENT_LLM_REQUEST_START,
                session_id, run_id, step, "error", rc.message, &rc, NULL);
            for (size_t mi = 0; mi < message_count; mi++) cc_message_cleanup(&messages[mi]);
            free(messages);
            free(tools_json);
            stream_loop_ctx_cleanup(&ctx);
            return rc;
        }



        runtime_publish_observability_with_run(runtime, CC_OBS_EVENT_LLM_REQUEST_START,
            session_id, run_id, step, "started", NULL, NULL, NULL);



        cc_llm_chat_request_t req;
        memset(&req, 0, sizeof(req));
        req.size = sizeof(req);
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
        /* 原生流按内容预算计量，传输层响应字节还含 SSE 帧，故此处只给同步响应设置传输预算。 */
        req.max_response_bytes = req.stream ? 0 : runtime->config.limits.max_output_bytes;
        req.event_bus = runtime->event_bus;
        req.session_id = session_id;
        req.run_id = run_id;
        req.step = step;

        if (tools_json && strlen(tools_json) > 2) {
            req.tools_json = tools_json;
        }

        {
            char *request_attrs = runtime_build_request_config_attributes(
                runtime, &req, tools_json ? strlen(tools_json) : 0);
            runtime_publish_observability_with_run(runtime,
                CC_OBS_EVENT_LLM_REQUEST_CONFIG, session_id, run_id, step,
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
        if (ctx.usage_event_seen || rc.code == CC_OK)
            cc_agent_runtime_publish_llm_usage_with_identity(runtime,session_id,run_id,NULL,
                ctx.step,req.model,"agent",ctx.usage_event_seen ? &ctx.observed_usage : NULL);
        if (rc.code == CC_OK && !ctx.has_tool_call &&
            !cc_string_builder_cstr(&ctx.text_builder)[0]) {
            rc = cc_result_error(CC_ERR_MODEL,
                "Model finished without answer text or tool calls; "
                "reasoning may have exhausted model.max_tokens");
        }
        runtime_publish_llm_stage(runtime, CC_OBS_EVENT_LLM_PROVIDER_SUBMIT,
            session_id, run_id, step, rc.code == CC_OK ? "ok" : "error",
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

        runtime_publish_observability_with_run(runtime, CC_OBS_EVENT_LLM_RESPONSE_FINISH,
            session_id, run_id, step, rc.code == CC_OK ? "ok" : "error",
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

            /* 先准备调用者答复副本，再提交 final 和删除检查点，分配失败时仍保留可恢复入口。 */
            char *answer = cc_copy_string(final_text ? final_text : "");
            if (!answer) {
                stream_loop_ctx_cleanup(&ctx);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy final response");
            }

            rc = checkpoint ? runtime_store_assistant_text_with_id(
                runtime, session_id, final_message_id, final_text, thinking) :
                cc_agent_runtime_store_assistant_text(runtime, session_id, final_text, thinking);
            if (rc.code != CC_OK) {
                free(answer);
                stream_loop_ctx_cleanup(&ctx);
                return rc;
            }
            if (checkpoint) {
                rc = cc_run_checkpoint_remove(runtime->run_checkpoints, checkpoint->run_id);
                if (rc.code != CC_OK) { free(answer); stream_loop_ctx_cleanup(&ctx); return rc; }
            }
            active_memory_after_run(runtime, session_id, user_input, final_text);

            *out_response = answer;
            runtime_publish_llm_stage(runtime, CC_OBS_EVENT_LLM_STREAM_FINISH,
                session_id, run_id, step, "finished", 0,
                (size_t)ctx.chunk_count, "chunk_count",
                final_text ? strlen(final_text) : 0, "text_bytes", NULL);
            stream_loop_ctx_cleanup(&ctx);

            runtime_publish_observability_with_run(runtime, CC_OBS_EVENT_STREAM_FINISHED,
                session_id, run_id, step, "finished", NULL, NULL, NULL);
            runtime_publish_observability_with_run(runtime, CC_OBS_EVENT_RUN_FINISHED,
                session_id, run_id, step, "ok", NULL, NULL, NULL);
            return cc_result_ok();
        }

        /* 恢复屏障在工具意图及派发前再次检查，覆盖忽略空工具 schema 的 provider。 */
        if (cc_agent_runtime_recovery_blocks_tools(runtime)) {
            cc_result_t blocked = cc_result_error(CC_ERR_INVALID_STATE,
                "Model requested a tool while operation recovery is unavailable");
            cc_result_t emit_rc = stream_loop_emit_error(&ctx, blocked.message);
            cc_result_free(&emit_rc);
            stream_loop_ctx_cleanup(&ctx);
            return blocked;
        }

        /* 工具执行必须发生在 provider 返回之后，不能占用 HTTP body callback。 */
        rc = execute_pending_tools(&ctx);
        if (rc.code != CC_OK) {
            stream_loop_ctx_cleanup(&ctx);
            return rc;
        }



    }



    stream_loop_ctx_cleanup(&ctx);

    const char *max_steps_text = "Agent stopped: max steps reached.";
    char *answer = cc_copy_string(max_steps_text);
    if (!answer) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy final response");
    if (checkpoint) {
        rc = runtime_store_assistant_text_with_id(runtime, session_id, final_message_id, max_steps_text, NULL);
        if (rc.code == CC_OK) rc = cc_run_checkpoint_remove(runtime->run_checkpoints, checkpoint->run_id);
        if (rc.code != CC_OK) { free(answer); return rc; }
    }
    *out_response = answer;
    runtime_publish_llm_stage(runtime, CC_OBS_EVENT_LLM_STREAM_FINISH,
        session_id, run_id, runtime_effective_max_steps(runtime), "max_steps_reached",
        0, 0, "chunk_count", 0, "text_bytes", NULL);
    runtime_publish_observability_with_run(runtime, CC_OBS_EVENT_STREAM_FINISHED,
        session_id, run_id, runtime_effective_max_steps(runtime), "finished", NULL, NULL, NULL);
    runtime_publish_observability_with_run(runtime, CC_OBS_EVENT_RUN_FINISHED,
        session_id, run_id, runtime_effective_max_steps(runtime), "max_steps_reached",
        "max_steps_reached", NULL, "{\"reason\":\"max_steps_reached\"}");
    return cc_result_ok();
}

/**
 * @brief 构造本次新运行的检查点身份和输入，取得会话 claim 后保存 MODEL 起点。
 * @param[in] runtime 有效借用实例，未配置 checkpoint 时仅允许未指定 ID 的普通调用。
 * @param[in] session_id 借用会话标识，checkpoint 启用时要求非空。
 * @param[in] user_input 借用输入，checkpoint 启用时要求非空并满足输入预算。
 * @param[in] requested_run_id 可选借用 run-UUID，存在时派生输入 ID 并核对检查点、历史输入及 final 均未使用。
 * @param[out] checkpoint 必需空资源输出，进入后清零，本次创建字段归外层 cleanup。
 * @param[out] claimed 必需输出，进入后置零，claim 成功后置一，后续失败也须由外层释放。
 * @return 成功或普通无检查点调用为 CC_OK，非法身份为 CC_ERR_INVALID_ARGUMENT，重复身份或缺少能力为 CC_ERR_INVALID_STATE，预算、分配、claim 和保存错误原样返回。
 */
static cc_result_t runtime_start_checkpoint(cc_agent_runtime_t *runtime,
                                             const char *session_id, const char *user_input,
                                             const char *requested_run_id,
                                             cc_run_checkpoint_t *checkpoint, int *claimed)
{
    *claimed = 0;
    memset(checkpoint, 0, sizeof(*checkpoint));
    if (!runtime->run_checkpoints)
        return requested_run_id ? cc_result_error(CC_ERR_INVALID_STATE, "Checkpointed runs are unavailable") : cc_result_ok();
    if (!session_id || !session_id[0] || !user_input || !user_input[0])
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Checkpointed run requires a session and input");
    cc_result_t rc = check_input_limit(runtime, user_input);
    if (rc.code == CC_OK && requested_run_id) {
        if (strlen(requested_run_id)!=40U)
            rc=cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid requested run id");
        else {
            checkpoint->run_id=cc_copy_string(requested_run_id);
            if (!checkpoint->run_id) rc=cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot copy requested run id");
        }
    } else if (rc.code == CC_OK) rc = cc_id_generate_uuid_v4("run-", &checkpoint->run_id);
    if (rc.code == CC_OK) rc = requested_run_id ?
        cc_run_checkpoint_derive_user_message_id(checkpoint->run_id,&checkpoint->user_message_id) :
        generate_id(&checkpoint->user_message_id);
    if (rc.code == CC_OK) {
        checkpoint->session_id = cc_copy_string(session_id);
        checkpoint->user_input = cc_copy_string(user_input);
        if (!checkpoint->session_id || !checkpoint->user_input)
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy checkpointed run input");
    }
    checkpoint->step = 1;
    checkpoint->phase = CC_RUN_CHECKPOINT_MODEL;
    if (rc.code == CC_OK) {
        rc = cc_run_checkpoint_claim(runtime->run_checkpoints, checkpoint->run_id, session_id);
        if (rc.code == CC_OK) *claimed = 1;
    }
    if (rc.code == CC_OK && requested_run_id) {
        cc_run_checkpoint_t existing={0};
        rc=cc_run_checkpoint_load(runtime->run_checkpoints,checkpoint->run_id,&existing);
        cc_run_checkpoint_cleanup(&existing);
        if (rc.code==CC_OK) {
            rc=cc_result_error(CC_ERR_INVALID_STATE,"Requested run id already has a checkpoint");
        } else if (rc.code==CC_ERR_NOT_FOUND) {
            cc_result_free(&rc);
            char final_id[64];
            int n=snprintf(final_id,sizeof(final_id),"%s-final",checkpoint->run_id);
            if (n<0 || (size_t)n>=sizeof(final_id))
                rc=cc_result_error(CC_ERR_LIMIT_EXCEEDED,"Run final message id is too long");
            else {
                cc_message_t final={0}; int found=0;
                rc=runtime->run_history_find(&runtime->store,session_id,
                    CC_RUN_HISTORY_MESSAGE_ID,final_id,&final,&found);
                cc_message_cleanup(&final);
                if (rc.code==CC_OK && found) {
                    cc_result_free(&rc);
                    rc=cc_result_error(CC_ERR_INVALID_STATE,"Requested run id already has a final answer");
                }
                if (rc.code==CC_OK) {
                    cc_message_t prior_input={0}; found=0;
                    rc=runtime->run_history_find(&runtime->store,session_id,
                        CC_RUN_HISTORY_MESSAGE_ID,checkpoint->user_message_id,&prior_input,&found);
                    cc_message_cleanup(&prior_input);
                    if (rc.code==CC_OK && found) {
                        cc_result_free(&rc);
                        rc=cc_result_error(CC_ERR_INVALID_STATE,"Requested run id already has an input message");
                    }
                }
            }
        }
    }
    if (rc.code == CC_OK) rc = cc_run_checkpoint_save(runtime->run_checkpoints, checkpoint);
    return rc;
}

/**
 * @brief 释放本次取得的检查点 claim，再清理本地字段并决定最终返回结果。
 * @param[in] runtime 借用实例，claimed 非零时须有检查点存储。
 * @param[in,out] checkpoint 借用的本次拥有检查点，清理后字段归零。
 * @param[in] claimed 非零表示需要按 checkpoint->run_id 归还 claim。
 * @param[in] result 按值移交的当前操作结果，已有错误优先保留。
 * @return 已有错误原样返回，原成功但释放失败时返回释放错误，其余返回原成功结果，未选用的释放消息由内部清理。
 */
static cc_result_t runtime_release_checkpoint(cc_agent_runtime_t *runtime,
                                               cc_run_checkpoint_t *checkpoint,
                                               int claimed, cc_result_t result)
{
    if (claimed) {
        cc_result_t release = cc_run_checkpoint_release(runtime->run_checkpoints, checkpoint->run_id);
        if (result.code == CC_OK && release.code != CC_OK) result = release;
        else cc_result_free(&release);
    }
    cc_run_checkpoint_cleanup(checkpoint);
    return result;
}

/**
 * @brief 为流式调用取得准入、新检查点和 claim，执行统一引擎后配对归还。
 * @param[in,out] runtime 必需借用实例，准入计数由本函数配对管理。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] user_input 必需借用输入。
 * @param[in] requested_run_id 可选借用预先生成的运行 ID，交检查点起点校验。
 * @param[in] options 可选借用流选项，token 和应用上下文覆盖整个同步调用。
 * @param[out] out_response 基础参数合法后置空，可能拥有成功答复或错误诊断，即使后续 claim 释放失败也由调用者清理。
 * @return 参数非法为 CC_ERR_INVALID_ARGUMENT，其余准入、检查点、引擎及释放结果原样返回。
 */
static cc_result_t runtime_handle_message_stream_cb(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    const char *requested_run_id,
    const cc_agent_runtime_stream_options_t *options,
    char **out_response
)
{
    if (!runtime || !session_id || !user_input || !out_response)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid streaming run request");
    *out_response = NULL;
    cc_agent_runtime_run_options_t run_options;
    memset(&run_options, 0, sizeof(run_options));
    run_options.size = sizeof(run_options);
    if (options) run_options.cancel_token = options->cancel_token;
    cc_result_t enter_rc = runtime_enter_run(runtime);
    if (enter_rc.code != CC_OK) return enter_rc;
    cc_result_free(&enter_rc);
    cc_run_checkpoint_t checkpoint;
    int claimed = 0;
    cc_result_t rc = runtime_start_checkpoint(runtime, session_id, user_input, requested_run_id, &checkpoint, &claimed);
    if (rc.code == CC_OK) {
        rc = cc_agent_runtime_handle_message_internal(
            runtime, session_id, user_input, options ? &run_options : NULL,
            options, runtime->run_checkpoints ? &checkpoint : NULL, out_response);
    }
    rc = runtime_release_checkpoint(runtime, &checkpoint, claimed, rc);
    runtime_leave_run(runtime);
    return rc;
}

/**
 * @brief 使用自动运行 ID 委托流式同步调用包装器。
 * @param[in,out] runtime 借用实例及其依赖，外层寿命覆盖本次调用。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] user_input 必需借用原输入。
 * @param[in] options 可选借用流选项，应用回调接收临时借用片段。
 * @param[out] out_response 无已有资源输出槽，初始化沿内部包装器，非 NULL 正文均由调用者 free。
 * @return 流式运行包装器结果原样返回。
 */
cc_result_t cc_agent_runtime_handle_message_stream_cb(
    cc_agent_runtime_t *runtime, const char *session_id, const char *user_input,
    const cc_agent_runtime_stream_options_t *options, char **out_response)
{
    return runtime_handle_message_stream_cb(runtime,session_id,user_input,NULL,options,out_response);
}

/**
 * @brief 以调用者预分配的运行 ID 启动 checkpointed 流式调用。
 * @param[in,out] runtime 借用实例，须配置 checkpoint/history 依赖才能使用指定 ID。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] user_input 必需借用原输入。
 * @param[in] run_id 必需且非空的借用 run-UUID，进一步形状和重复检查由检查点路径执行。
 * @param[in] options 可选借用流选项。
 * @param[out] out_response 非 NULL 时先置空，后续拥有正文由调用者 free。
 * @return 空 ID 为 CC_ERR_INVALID_ARGUMENT，其他结果沿流式运行包装器。
 */
cc_result_t cc_agent_runtime_handle_message_with_run_id(
    cc_agent_runtime_t *runtime, const char *session_id, const char *user_input,
    const char *run_id, const cc_agent_runtime_stream_options_t *options, char **out_response)
{
    if (out_response) *out_response=NULL;
    if (!run_id || !run_id[0]) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid requested run id");
    return runtime_handle_message_stream_cb(runtime,session_id,user_input,run_id,options,out_response);
}

/**
 * @brief 加载旧检查点，claim 后重新读取并按保留原输入继续运行。
 * @param[in,out] runtime 借用实例，须具备 checkpoint 和完整历史查询，配对管理运行准入。
 * @param[in] run_id 必需借用运行 ID。
 * @param[in] options 可选借用流选项，token 和回调上下文覆盖本次恢复调用。
 * @param[out] out_response 基础参数合法后置空，非 NULL 拥有正文由调用者 free，后续释放失败也遵守此规则。
 * @return 缺少能力或参数为 CC_ERR_INVALID_ARGUMENT，会话变化为 CC_ERR_STORAGE，其余加载、claim、引擎及释放错误原样返回。
 * @note claim 后重读排除会话已变化的状态，存在 UNKNOWN 工具时保留核对屏障，检查点及 claim 在本次结束时清理或归还。
 */
cc_result_t cc_agent_runtime_resume_run(
    cc_agent_runtime_t *runtime, const char *run_id,
    const cc_agent_runtime_stream_options_t *options, char **out_response)
{
    if (!runtime || !run_id || !out_response || !runtime->run_checkpoints || !runtime->run_history_find)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Run recovery is unavailable");
    *out_response = NULL;
    cc_result_t rc = runtime_enter_run(runtime);
    if (rc.code != CC_OK) return rc;
    cc_run_checkpoint_t checkpoint = {0};
    int claimed = 0;
    rc = cc_run_checkpoint_load(runtime->run_checkpoints, run_id, &checkpoint);
    if (rc.code == CC_OK) {
        rc = cc_run_checkpoint_claim(runtime->run_checkpoints, checkpoint.run_id, checkpoint.session_id);
        if (rc.code == CC_OK) claimed = 1;
    }
    if (rc.code == CC_OK) {
        cc_run_checkpoint_t current = {0};
        rc = cc_run_checkpoint_load(runtime->run_checkpoints, run_id, &current);
        if (rc.code == CC_OK && strcmp(current.session_id, checkpoint.session_id) != 0)
            rc = cc_result_error(CC_ERR_STORAGE, "Run checkpoint session changed");
        if (rc.code == CC_OK) {
            cc_run_checkpoint_cleanup(&checkpoint);
            checkpoint = current;
            memset(&current, 0, sizeof(current));
        }
        cc_run_checkpoint_cleanup(&current);
    }
    if (rc.code == CC_OK) {
        cc_agent_runtime_run_options_t run_options = {0};
        run_options.size = sizeof(run_options);
        run_options.cancel_token = options ? options->cancel_token : NULL;
        rc = cc_agent_runtime_handle_message_internal(runtime, checkpoint.session_id,
            checkpoint.user_input, &run_options, options, &checkpoint, out_response);
    }
    rc = runtime_release_checkpoint(runtime, &checkpoint, claimed, rc);
    runtime_leave_run(runtime);
    return rc;
}

/**
 * @brief 委托检查点存储列举持久运行 ID。
 * @param[in] runtime 借用实例，须配置 checkpoint 存储，读取期间保持有效。
 * @param[out] out_run_ids 必需输出，初始化与失败规则沿 cc_run_checkpoint_list，成功字符串及数组由调用者逐项 free。
 * @param[out] out_count 必需输出，由检查点列表入口填写条数。
 * @return 参数或能力缺失为 CC_ERR_INVALID_ARGUMENT，列表结果原样返回。
 */
cc_result_t cc_agent_runtime_list_interrupted_runs(
    cc_agent_runtime_t *runtime, char ***out_run_ids, size_t *out_count)
{
    if (!runtime || !runtime->run_checkpoints || !out_run_ids || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Run recovery is unavailable");
    return cc_run_checkpoint_list(runtime->run_checkpoints, out_run_ids, out_count);
}

/**
 * @brief 委托检查点存储读取一个持久运行快照。
 * @param[in] runtime 借用实例，须配置检查点存储。
 * @param[in] run_id 必需借用运行 ID。
 * @param[out] out_checkpoint 必需的无已有资源结构，初始化沿底层 load，返回字段由调用者 cleanup。
 * @return 参数或能力缺失为 CC_ERR_INVALID_ARGUMENT，其余读取、恢复来源及校验结果原样返回。
 */
cc_result_t cc_agent_runtime_load_interrupted_run(
    cc_agent_runtime_t *runtime, const char *run_id, cc_run_checkpoint_t *out_checkpoint)
{
    if (!runtime || !runtime->run_checkpoints || !run_id || !out_checkpoint)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Run recovery is unavailable");
    return cc_run_checkpoint_load(runtime->run_checkpoints, run_id, out_checkpoint);
}

/**
 * @brief claim 目标会话并重读确认后移除指定检查点。
 * @param[in] runtime 借用实例，须有 checkpoint 存储。
 * @param[in] run_id 必需借用运行 ID。
 * @return 参数或能力缺失为 CC_ERR_INVALID_ARGUMENT，重读会话改变为 CC_ERR_STORAGE，加载、claim、删除及释放错误原样返回。
 * @note 本次仅移除检查点并释放局部字段，设备账本的未知工具仍由独立核对入口管理。
 */
cc_result_t cc_agent_runtime_abandon_run(cc_agent_runtime_t *runtime, const char *run_id)
{
    if (!runtime || !runtime->run_checkpoints || !run_id)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Run recovery is unavailable");
    cc_run_checkpoint_t checkpoint = {0};
    cc_result_t rc = cc_run_checkpoint_load(runtime->run_checkpoints, run_id, &checkpoint);
    int claimed = 0;
    if (rc.code == CC_OK) {
        rc = cc_run_checkpoint_claim(runtime->run_checkpoints, checkpoint.run_id, checkpoint.session_id);
        if (rc.code == CC_OK) claimed = 1;
    }
    if (rc.code == CC_OK) {
        cc_run_checkpoint_t current = {0};
        rc = cc_run_checkpoint_load(runtime->run_checkpoints, run_id, &current);
        if (rc.code == CC_OK && strcmp(current.session_id, checkpoint.session_id) != 0)
            rc = cc_result_error(CC_ERR_STORAGE, "Run checkpoint session changed");
        cc_run_checkpoint_cleanup(&current);
        if (rc.code == CC_OK) rc = cc_run_checkpoint_remove(runtime->run_checkpoints, run_id);
    }
    return runtime_release_checkpoint(runtime, &checkpoint, claimed, rc);
}

/**
 * @brief 从可选设备账本查找待核对工具调用。
 * @param[in] runtime 有效借用实例，无 journal 时成功返回未找到。
 * @param[in] session_id 借用会话标识。
 * @param[in] tool_call_id 可选借用调用 ID，具体选择顺序沿账本接口。
 * @param[out] out_call 可选空资源输出，由账本复制拥有字段。
 * @param[out] out_operation_id 可选空资源输出，进入后置 NULL，命中时由调用者 free。
 * @param[out] out_found 必需输出，进入后置零，命中由账本入口填写。
 * @return 无账本为 CC_OK，其他结果沿 cc_operation_journal_find_unknown，返回码决定输出是否可使用。
 */
static cc_result_t runtime_find_journaled_call(
    cc_agent_runtime_t *runtime, const char *session_id, const char *tool_call_id,
    cc_tool_call_t *out_call, char **out_operation_id, int *out_found)
{
    *out_found = 0;
    if (out_operation_id) *out_operation_id = NULL;
    if (!runtime->operation_journal) return cc_result_ok();
    return cc_operation_journal_find_unknown(runtime->operation_journal, session_id,
                                              tool_call_id, out_call, out_operation_id, out_found);
}

/**
 * @brief 取得运行准入后先查设备账本，再查历史未配对工具意图。
 * @param[in,out] runtime 借用实例，准入计数在返回前归还。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] tool_call_id 可选借用调用 ID，NULL 按各来源的待处理顺序选取。
 * @param[out] out_call 必需空资源输出，参数合法后清零，成功拥有字段由调用者 cc_tool_call_cleanup。
 * @return 成功为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，未找到为 CC_ERR_NOT_FOUND，准入、账本或历史错误原样返回，错误时清理输出。
 */
cc_result_t cc_agent_runtime_get_pending_tool_call(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *tool_call_id,
    cc_tool_call_t *out_call
)
{
    if (!runtime || !session_id || !out_call) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,
            "Invalid pending tool lookup request");
    }
    memset(out_call, 0, sizeof(*out_call));
    cc_result_t rc = runtime_enter_run(runtime);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);

    int found = 0;
    rc = runtime_find_journaled_call(runtime, session_id, tool_call_id,
                                     out_call, NULL, &found);
    if (rc.code == CC_OK && !found) {
        cc_result_free(&rc);
        rc = runtime_find_pending_tool_call(
            runtime, session_id, tool_call_id, 0, out_call, &found);
    }
    if (rc.code == CC_OK && !found) {
        rc = cc_result_error(CC_ERR_NOT_FOUND,
            "Session has no unresolved tool call");
    }
    if (rc.code != CC_OK) cc_tool_call_cleanup(out_call);
    runtime_leave_run(runtime);
    return rc;
}

/**
 * @brief 遍历检查点定位唯一的当前工具节点，claim 后重读核对完整调用。
 * @param[in] runtime 借用实例，无 checkpoint 存储时成功返回空输出。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] call 借用待核对调用，ID、名称和参数均已验证。
 * @param[out] out_checkpoint 必需空资源输出，进入后清零，成功匹配时接管拥有字段。
 * @param[out] out_claimed 必需输出，进入后置零，成功取得且保留 claim 时置一，调用者负责配对释放。
 * @return 成功匹配或无匹配为 CC_OK，重复归属、恢复旁路或身份冲突为 CC_ERR_STORAGE，claim 后变化为 CC_ERR_INVALID_STATE，其余列表、加载和 claim 错误原样返回。
 * @note 失败清理本地输出，并尝试释放已取得 claim，列表 ID 及所有未移交候选始终清理。
 */
static cc_result_t runtime_claim_checkpoint_for_call(
    cc_agent_runtime_t *runtime, const char *session_id, const cc_tool_call_t *call,
    cc_run_checkpoint_t *out_checkpoint, int *out_claimed)
{
    memset(out_checkpoint, 0, sizeof(*out_checkpoint));
    *out_claimed = 0;
    if (!runtime->run_checkpoints) return cc_result_ok();
    char **ids = NULL;
    size_t count = 0;
    cc_result_t rc = cc_run_checkpoint_list(runtime->run_checkpoints, &ids, &count);
    for (size_t i = 0; rc.code == CC_OK && i < count; i++) {
        cc_run_checkpoint_t candidate = {0};
        rc = cc_run_checkpoint_load(runtime->run_checkpoints, ids[i], &candidate);
        if (rc.code == CC_OK && strcmp(candidate.session_id, session_id) == 0 &&
            candidate.phase == CC_RUN_CHECKPOINT_TOOLS &&
            candidate.next_call < candidate.planned_calls.count &&
            strcmp(candidate.planned_calls.items[candidate.next_call].id, call->id) == 0) {
            if (out_checkpoint->run_id) {
                rc = cc_result_error(CC_ERR_STORAGE, "Tool call belongs to multiple run checkpoints");
            } else if (candidate.recovered_from_backup || candidate.recovered_from_temporary) {
                rc = cc_result_error(CC_ERR_STORAGE, "Run checkpoint requires manual recovery");
            } else if (!runtime_tool_call_equal(&candidate.planned_calls.items[candidate.next_call], call)) {
                rc = cc_result_error(CC_ERR_STORAGE, "Run checkpoint conflicts with pending tool operation");
            } else {
                *out_checkpoint = candidate;
                memset(&candidate, 0, sizeof(candidate));
            }
        }
        cc_run_checkpoint_cleanup(&candidate);
    }
    for (size_t i = 0; i < count; i++) free(ids[i]);
    free(ids);
    if (rc.code == CC_OK && out_checkpoint->run_id) {
        rc = cc_run_checkpoint_claim(runtime->run_checkpoints,
                                      out_checkpoint->run_id, out_checkpoint->session_id);
        if (rc.code == CC_OK) {
            *out_claimed = 1;
            cc_run_checkpoint_t current = {0};
            rc = cc_run_checkpoint_load(runtime->run_checkpoints, out_checkpoint->run_id, &current);
            if (rc.code == CC_OK && (current.recovered_from_backup || current.recovered_from_temporary ||
                strcmp(current.session_id, session_id) != 0 ||
                current.phase != CC_RUN_CHECKPOINT_TOOLS ||
                current.next_call >= current.planned_calls.count ||
                !runtime_tool_call_equal(&current.planned_calls.items[current.next_call], call))) {
                rc = cc_result_error(CC_ERR_INVALID_STATE, "Run checkpoint changed while reconciling");
            }
            if (rc.code == CC_OK) {
                cc_run_checkpoint_cleanup(out_checkpoint);
                *out_checkpoint = current;
                memset(&current, 0, sizeof(current));
            }
            cc_run_checkpoint_cleanup(&current);
            if (rc.code != CC_OK) {
                cc_result_t release = cc_run_checkpoint_release(runtime->run_checkpoints, out_checkpoint->run_id);
                cc_result_free(&release);
                *out_claimed = 0;
            }
        }
    }
    if (rc.code != CC_OK) cc_run_checkpoint_cleanup(out_checkpoint);
    return rc;
}

/** @brief 完整历史核对的借用期上下文，计数和命中标志只由串行 visitor 更新。 */
typedef struct runtime_reconciliation_scan {
    const char *operation_id; /**< 借用的目标账本 ID。 */
    const cc_tool_call_t *call; /**< 借用的预期完整工具调用。 */
    const char *note_text; /**< 借用的本次人工决定完整正文。 */
    cc_message_tool_outcome_t outcome; /**< 本次已确认的执行或未执行结论。 */
    const char *artifact_id; /**< 借用的稳定产物消息 ID。 */
    const cc_message_t *artifact; /**< 可选借用的预期产物观察消息。 */
    int note_found; /**< 已找到内容及结果标志一致的决定说明。 */
    int artifact_found; /**< 已找到唯一且完整一致的产物消息。 */
    int intent_count; /**< 同调用 ID 的完整一致意图条数，超过一即报错。 */
    int tool_result_count; /**< 同调用 ID 的 tool role 结果数，超过一即报错。 */
} runtime_reconciliation_scan_t;

/**
 * @brief 对完整历史的单条消息核对意图唯一性、已有工具结果及人工决定和产物一致性。
 * @param[in] message 有效借用消息，身份及工具数组满足 history visitor 契约。
 * @param[in,out] context 有效的 runtime_reconciliation_scan_t，引用字段覆盖遍历期，命中计数及标志累加。
 * @return 一致或无关消息为 CC_OK，历史意图/结果重复或矛盾为 CC_ERR_STORAGE，已持久决定或产物与本次不同为 CC_ERR_INVALID_STATE，序列化和摘要错误原样返回。
 * @note 比对产物使用完整消息序列化，本函数释放本次临时正文，原历史及期望消息保持借用。
 */
static cc_result_t runtime_visit_reconciliation_message(const cc_message_t *message, void *context)
{
    runtime_reconciliation_scan_t *scan = context;
    if (message->role == CC_ROLE_ASSISTANT) {
        for (size_t i = 0; i < message->tool_calls.count; i++) {
            if (message->tool_calls.items[i].id &&
                strcmp(message->tool_calls.items[i].id, scan->call->id) == 0) {
                if (!runtime_tool_call_equal(&message->tool_calls.items[i], scan->call))
                    return cc_result_error(CC_ERR_STORAGE, "Historical intent conflicts with pending tool operation");
                if (++scan->intent_count > 1)
                    return cc_result_error(CC_ERR_STORAGE, "Repeated historical tool call id");
            }
        }
    }
    if (message->role == CC_ROLE_TOOL && message->tool_call_id &&
        strcmp(message->tool_call_id, scan->call->id) == 0 &&
        ++scan->tool_result_count > 1)
        return cc_result_error(CC_ERR_STORAGE, "Repeated historical tool result id");

    if (message->id && strcmp(message->id, scan->artifact_id) == 0) {
        if (!scan->artifact || scan->artifact_found || message->role != CC_ROLE_USER)
            return cc_result_error(CC_ERR_INVALID_STATE, "Conflicting reconciliation artifacts");
        char *expected = NULL, *actual = NULL;
        cc_result_t rc = cc_message_to_json(scan->artifact, 1, &expected);
        if (rc.code == CC_OK) rc = cc_message_to_json(message, 1, &actual);
        if (rc.code == CC_OK && strcmp(expected, actual) != 0)
            rc = cc_result_error(CC_ERR_INVALID_STATE, "Reconciliation artifacts changed on retry");
        free(expected); free(actual);
        if (rc.code != CC_OK) return rc;
        scan->artifact_found = 1;
    }
    if (message->role != CC_ROLE_USER || !message->tool_call_id) return cc_result_ok();

    char *text = NULL;
    cc_result_t rc = cc_message_get_text_summary(message, &text);
    if (rc.code != CC_OK) return rc;
    if (text && strcmp(text, runtime_non_execution_note) == 0 &&
        strcmp(message->tool_call_id, scan->call->id) == 0 &&
        message->tool_outcome == CC_MESSAGE_TOOL_OUTCOME_RECONCILED_NOT_EXECUTED) {
        if (scan->outcome == CC_MESSAGE_TOOL_OUTCOME_RECONCILED_NOT_EXECUTED)
            scan->note_found = 1;
        else rc = cc_result_error(CC_ERR_INVALID_STATE, "Tool invocation was durably recorded as not executed");
        free(text);
        return rc;
    }
    static const char prefix[] = "Operator reconciliation for operation ";
    size_t prefix_length = sizeof(prefix) - 1U;
    size_t operation_length = strlen(scan->operation_id);
    int matches = text && strncmp(text, prefix, prefix_length) == 0 &&
        strncmp(text + prefix_length, scan->operation_id, operation_length) == 0 &&
        strncmp(text + prefix_length + operation_length, ", tool ", 7) == 0;
    if (matches) {
        if (strcmp(message->tool_call_id, scan->call->id) != 0 ||
            strcmp(text, scan->note_text) != 0 ||
            message->tool_outcome != scan->outcome) {
            rc = cc_result_error(CC_ERR_INVALID_STATE,
                "A different reconciliation decision is already durable; inspect the pending operation");
        } else {
            scan->note_found = 1;
        }
    }
    free(text);
    return rc;
}

/**
 * @brief 优先以有界 visitor 遍历完整历史，否则加载 limit=-1 的全量消息进行核对。
 * @param[in] runtime 借用实例，须有完整 visitor 或支持完整加载的 store。
 * @param[in] session_id 借用会话标识。
 * @param[in,out] scan 本次核对状态，按历史消息累计命中及计数。
 * @return visitor 或完整读取、逐项校验结果原样返回，缺少加载入口为 CC_ERR_STORAGE。
 * @note 加载路径始终清理消息数组，整体历史视图及同会话并发由调用者 claim 或外层协调。
 */
static cc_result_t runtime_scan_reconciliation(
    cc_agent_runtime_t *runtime, const char *session_id, runtime_reconciliation_scan_t *scan)
{
    if (runtime->run_history_visit)
        return runtime->run_history_visit(&runtime->store, session_id,
                                         runtime_visit_reconciliation_message, scan);
    if (!runtime->store.vtable || !runtime->store.vtable->load_messages)
        return cc_result_error(CC_ERR_STORAGE, "Session store cannot scan reconciliation history");
    cc_message_t *messages = NULL;
    size_t count = 0;
    cc_result_t rc = runtime->store.vtable->load_messages(runtime->store.self, session_id,
                                                          -1, &messages, &count);
    for (size_t i = 0; rc.code == CC_OK && i < count; i++)
        rc = runtime_visit_reconciliation_message(&messages[i], scan);
    for (size_t i = 0; i < count; i++) cc_message_cleanup(&messages[i]);
    free(messages);
    return rc;
}

/**
 * @brief claim 设备操作及关联会话，核对并原子保存人工决定，再推进检查点并结束账本项。
 * @param[in] runtime 借用实例，journal 必需，可选 checkpoint 用于会话互斥和工具节点推进。
 * @param[in] session_id 借用的已核对会话标识。
 * @param[in] call 借用的完整待核对工具调用。
 * @param[in] operation_id 借用账本操作 ID，用于 claim 和稳定产物 ID。
 * @param[in] disposition 已验证的 EXECUTED 或 NOT_EXECUTED，由外部实际核对结果决定。
 * @param[in] observed_result EXECUTED 时必需的借用实测结果，NOT_EXECUTED 时内部生成失败结果。
 * @return 完整决定提交和账本结束为 CC_OK，历史或重试冲突、持久化、分配及 claim 错误原样返回。
 * @note 同一决定重试先核对已有持久说明与产物，缺工具结果时补齐同批记录。NOT_EXECUTED 为后续恢复生成新调用 ID。
 * 失败可已有决定或检查点推进，保留账本并释放核对 claim 供重试，所有本次拥有消息及字段在返回前清理。
 */
static cc_result_t runtime_resolve_journaled_tool(
    cc_agent_runtime_t *runtime, const char *session_id, const cc_tool_call_t *call,
    const char *operation_id, cc_tool_recovery_disposition_t disposition,
    const cc_tool_result_t *observed_result)
{
    cc_result_t rc = cc_operation_journal_claim_reconciliation(runtime->operation_journal, operation_id);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);

    cc_tool_result_t not_executed = {0};
    cc_run_checkpoint_t checkpoint = {0};
    int checkpoint_claimed = 0;
    char *session_claim_id = NULL;
    int session_claimed = 0;
    rc = runtime_claim_checkpoint_for_call(runtime, session_id, call,
                                           &checkpoint, &checkpoint_claimed);
    if (rc.code != CC_OK) goto done;
    if (runtime->run_checkpoints && !checkpoint_claimed) {
        rc = cc_id_generate_uuid_v4("run-", &session_claim_id);
        if (rc.code == CC_OK) {
            rc = cc_run_checkpoint_claim(runtime->run_checkpoints, session_claim_id, session_id);
            if (rc.code == CC_OK) session_claimed = 1;
        }
        if (rc.code != CC_OK) goto done;
    }

    const cc_tool_result_t *resolution = observed_result;
    if (disposition == CC_TOOL_RECOVERY_NOT_EXECUTED) {
        not_executed.ok = 0;
        not_executed.error = cc_copy_string("The operator verified that this action was not executed.");
        if (!not_executed.error) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate recovery result");
            goto done;
        }
        resolution = &not_executed;
    }

    const char *result_text = resolution->ok ? resolution->text : resolution->error;
    cc_string_builder_t note;
    rc = cc_string_builder_init(&note);
    if (rc.code != CC_OK) goto done;
    rc = cc_string_builder_appendf(&note,
        "Operator reconciliation for operation %s, tool %s: %s. Observed result: %s",
        operation_id, call->name,
        disposition == CC_TOOL_RECOVERY_EXECUTED ? "executed" : "not executed",
        result_text ? result_text : "");
    if (rc.code != CC_OK) { cc_string_builder_deinit(&note); goto done; }
    char *note_text = cc_string_builder_take(&note);
    if (!note_text) { rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate recovery note"); goto done; }

    cc_message_t *note_message = NULL;
    cc_message_t *tool_message = NULL;
    cc_message_t *artifact_message = NULL;
    char *message_id = NULL;
    size_t artifact_id_size = strlen(operation_id) + sizeof("-artifacts");
    char *artifact_id = malloc(artifact_id_size);
    if (!artifact_id) {
        free(note_text);
        rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate reconciliation artifact id");
        goto messages_done;
    }
    snprintf(artifact_id, artifact_id_size, "%s-artifacts", operation_id);
    rc = runtime_create_artifact_observation(runtime, session_id, resolution,
                                              artifact_id, call->id, &artifact_message);
    runtime_reconciliation_scan_t scan = {0};
    scan.operation_id = operation_id;
    scan.call = call;
    scan.note_text = note_text;
    scan.outcome = disposition == CC_TOOL_RECOVERY_EXECUTED ?
        CC_MESSAGE_TOOL_OUTCOME_RECONCILED_EXECUTED : CC_MESSAGE_TOOL_OUTCOME_RECONCILED_NOT_EXECUTED;
    scan.artifact_id = artifact_id;
    scan.artifact = artifact_message;
    if (rc.code == CC_OK) rc = runtime_scan_reconciliation(runtime, session_id, &scan);
    if (rc.code == CC_OK && scan.artifact_found && !scan.note_found)
        rc = cc_result_error(CC_ERR_STORAGE, "Reconciliation artifacts have no durable decision");
    if (rc.code != CC_OK) { free(note_text); goto messages_done; }
    rc = generate_id(&message_id);
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = cc_message_create_text(message_id, session_id, CC_ROLE_USER,
                                    note_text, call->id, &note_message);
    }
    free(message_id);
    free(note_text);
    if (rc.code != CC_OK) goto messages_done;
    note_message->tool_outcome = scan.outcome;

    /* 原意图尚无 tool role 配对时同批补齐结果，已有 UNKNOWN 占位结果继续保留供决定说明关联。 */
    int has_unpaired_call = scan.intent_count && !scan.tool_result_count;
    if (scan.note_found && has_unpaired_call) {
        rc = cc_result_error(CC_ERR_STORAGE, "Durable reconciliation has an unpaired tool call");
        goto messages_done;
    }
    if (has_unpaired_call) {
        message_id = NULL;
        rc = generate_id(&message_id);
        if (rc.code == CC_OK) {
            cc_result_free(&rc);
            rc = cc_message_create_text(message_id, session_id, CC_ROLE_TOOL,
                                        result_text ? result_text : "", call->id, &tool_message);
        }
        free(message_id);
        if (rc.code != CC_OK) goto messages_done;
    }
    cc_session_record_t records[5];
    memset(records, 0, sizeof(records));
    size_t record_count = 0;
    if (has_unpaired_call) {
        records[record_count].type = CC_SESSION_RECORD_TOOL_CALL;
        records[record_count].session_id = session_id;
        records[record_count++].data.tool_call = call;
        records[record_count].type = CC_SESSION_RECORD_TOOL_RESULT;
        records[record_count].session_id = session_id;
        records[record_count].data.tool_result.tool_call_id = call->id;
        records[record_count++].data.tool_result.result = resolution;
        records[record_count].type = CC_SESSION_RECORD_MESSAGE;
        records[record_count].session_id = session_id;
        records[record_count++].data.message = tool_message;
    }
    if (!scan.note_found) {
        records[record_count].type = CC_SESSION_RECORD_MESSAGE;
        records[record_count].session_id = session_id;
        records[record_count++].data.message = note_message;
    }
    if (artifact_message && !scan.artifact_found) {
        records[record_count].type = CC_SESSION_RECORD_MESSAGE;
        records[record_count].session_id = session_id;
        records[record_count++].data.message = artifact_message;
    }
    if (record_count) rc = runtime_append_records(runtime, records, record_count);
    if (rc.code == CC_OK && checkpoint_claimed) {
        if (disposition == CC_TOOL_RECOVERY_EXECUTED) {
            checkpoint.next_call++;
        } else {
            char *retry_id = NULL;
            char *prior_id = cc_copy_string(call->id);
            if (!prior_id) rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot preserve reconciled tool id");
            if (rc.code == CC_OK) rc = cc_id_generate_uuid_v4("tool_", &retry_id);
            if (rc.code == CC_OK) {
                free(checkpoint.planned_calls.items[checkpoint.next_call].id);
                checkpoint.planned_calls.items[checkpoint.next_call].id = retry_id;
                free(checkpoint.retry_after_call_id);
                checkpoint.retry_after_call_id = prior_id;
            } else {
                free(retry_id);
                free(prior_id);
            }
        }
        if (rc.code == CC_OK) rc = cc_run_checkpoint_save(runtime->run_checkpoints, &checkpoint);
    }
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = cc_operation_journal_finish(runtime->operation_journal, operation_id);
    }
    if (rc.code == CC_OK) {
        cc_json_value_t *attrs = cc_json_create_object();
        if (attrs) {
            cc_json_object_set_owned(attrs, "operation_id", cc_json_create_string(operation_id));
            cc_json_object_set_owned(attrs, "tool_call_id", cc_json_create_string(call->id));
            cc_json_object_set_owned(attrs, "tool_name", cc_json_create_string(call->name));
            char *json = cc_json_stringify_unformatted(attrs);
            cc_json_destroy(attrs);
            if (json) {
                runtime_publish_observability_with_run(runtime, CC_OBS_EVENT_TOOL_RECOVERY_RESOLVED,
                    session_id, checkpoint_claimed ? checkpoint.run_id : NULL, 0, disposition == CC_TOOL_RECOVERY_EXECUTED ?
                    "executed" : "not_executed", NULL, NULL, json);
                free(json);
            }
        }
    }
messages_done:
    free(artifact_id);
    cc_message_destroy(note_message);
    cc_message_destroy(tool_message);
    cc_message_destroy(artifact_message);
done:
    if (rc.code != CC_OK) {
        cc_result_t release_rc = cc_operation_journal_release_reconciliation(
            runtime->operation_journal, operation_id);
        cc_result_free(&release_rc);
    }
    if (checkpoint_claimed) {
        cc_result_t release_rc = cc_run_checkpoint_release(runtime->run_checkpoints, checkpoint.run_id);
        if (rc.code == CC_OK && release_rc.code != CC_OK) rc = release_rc;
        else cc_result_free(&release_rc);
    }
    if (session_claimed) {
        cc_result_t release_rc = cc_run_checkpoint_release(runtime->run_checkpoints, session_claim_id);
        if (rc.code == CC_OK && release_rc.code != CC_OK) rc = release_rc;
        else cc_result_free(&release_rc);
    }
    free(session_claim_id);
    cc_run_checkpoint_cleanup(&checkpoint);
    cc_tool_result_cleanup(&not_executed);
    return rc;
}

/**
 * @brief 以外部核对结果解决待处理工具，优先走设备账本的可重试决定提交流程。
 * @param[in,out] runtime 借用实例，调用期占用并配对归还运行准入名额。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] tool_call_id 必需且非空的借用调用 ID。
 * @param[in] disposition 必须是 EXECUTED 或 NOT_EXECUTED，实际副作用由调用者先核实。
 * @param[in] observed_result EXECUTED 时必需借用结果，NOT_EXECUTED 时忽略，原拥有字段仍归调用者。
 * @return 参数非法为 CC_ERR_INVALID_ARGUMENT，未找到为 CC_ERR_NOT_FOUND，准入、账本、历史、分配及提交错误原样返回。
 * @note 本入口只提交核对证据，工具的下一次执行由后续运行推进。无账本的兼容路径补写工具结果与产物观察。
 */
cc_result_t cc_agent_runtime_resolve_pending_tool(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *tool_call_id,
    cc_tool_recovery_disposition_t disposition,
    const cc_tool_result_t *observed_result
)
{
    if (!runtime || !session_id || !tool_call_id || !tool_call_id[0]) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,
            "Invalid pending tool recovery request");
    }
    if (disposition != CC_TOOL_RECOVERY_EXECUTED &&
        disposition != CC_TOOL_RECOVERY_NOT_EXECUTED) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,
            "Unknown pending tool recovery disposition");
    }
    if (disposition == CC_TOOL_RECOVERY_EXECUTED && !observed_result) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,
            "Observed result is required for a completed tool action");
    }

    cc_result_t rc = runtime_enter_run(runtime);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);

    cc_tool_call_t pending_call;
    memset(&pending_call, 0, sizeof(pending_call));
    int found = 0;
    char *operation_id = NULL;
    rc = runtime_find_journaled_call(runtime, session_id, tool_call_id,
                                     &pending_call, &operation_id, &found);
    if (rc.code != CC_OK) {
        runtime_leave_run(runtime);
        return rc;
    }
    if (found) {
        cc_result_free(&rc);
        rc = runtime_resolve_journaled_tool(runtime, session_id, &pending_call,
                                            operation_id, disposition, observed_result);
        free(operation_id);
        cc_tool_call_cleanup(&pending_call);
        runtime_leave_run(runtime);
        return rc;
    }
    cc_result_free(&rc);
    rc = runtime_find_pending_tool_call(
        runtime, session_id, tool_call_id, 0, &pending_call, &found);
    if (rc.code != CC_OK || !found) {
        if (rc.code == CC_OK) {
            rc = cc_result_error(CC_ERR_NOT_FOUND,
                "No unresolved tool call matches the supplied id");
        }
        cc_tool_call_cleanup(&pending_call);
        runtime_leave_run(runtime);
        return rc;
    }
    cc_result_free(&rc);

    cc_tool_result_t not_executed_result;
    memset(&not_executed_result, 0, sizeof(not_executed_result));
    const cc_tool_result_t *resolution = observed_result;
    if (disposition == CC_TOOL_RECOVERY_NOT_EXECUTED) {
        not_executed_result.ok = 0;
        not_executed_result.error = cc_copy_string(
            "The interrupted action was reconciled as not executed; no automatic retry was performed.");
        if (!not_executed_result.error) {
            cc_tool_call_cleanup(&pending_call);
            runtime_leave_run(runtime);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY,
                "Failed to allocate interrupted tool result");
        }
        resolution = &not_executed_result;
    }

    cc_message_t *tool_msg = NULL;
    char *message_id = NULL;
    rc = generate_id(&message_id);
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        const char *text = resolution->ok ? resolution->text : resolution->error;
        rc = cc_message_create_text(message_id, session_id, CC_ROLE_TOOL,
            text ? text : "", tool_call_id, &tool_msg);
    }
    free(message_id);

    if (rc.code == CC_OK) {
        cc_session_record_t records[3];
        memset(records, 0, sizeof(records));
        records[0].type = CC_SESSION_RECORD_TOOL_CALL;
        records[0].session_id = session_id;
        records[0].data.tool_call = &pending_call;
        records[1].type = CC_SESSION_RECORD_TOOL_RESULT;
        records[1].session_id = session_id;
        records[1].data.tool_result.tool_call_id = tool_call_id;
        records[1].data.tool_result.result = resolution;
        records[2].type = CC_SESSION_RECORD_MESSAGE;
        records[2].session_id = session_id;
        records[2].data.message = tool_msg;
        rc = runtime_append_records(runtime, records, 3);
    }

    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = cc_agent_runtime_append_artifact_observation(
            runtime, session_id, pending_call.id, resolution);
    }
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        cc_json_value_t *attrs = cc_json_create_object();
        if (attrs) {
            cc_json_object_set_owned(attrs, "tool_call_id",
                cc_json_create_string(tool_call_id));
            cc_json_object_set_owned(attrs, "tool_name",
                cc_json_create_string(pending_call.name ? pending_call.name : ""));
            char *attrs_json = cc_json_stringify_unformatted(attrs);
            cc_json_destroy(attrs);
            if (attrs_json) {
                runtime_publish_observability(runtime,
                    CC_OBS_EVENT_TOOL_RECOVERY_RESOLVED, session_id, 0,
                    disposition == CC_TOOL_RECOVERY_EXECUTED ? "executed" : "not_executed",
                    NULL, NULL, attrs_json);
                free(attrs_json);
            }
        }
        rc = cc_result_ok();
    }

    cc_message_destroy(tool_msg);
    cc_tool_result_cleanup(&not_executed_result);
    cc_tool_call_cleanup(&pending_call);
    runtime_leave_run(runtime);
    return rc;
}

/**
 * @brief 释放 runtime 拥有的配置、provider 身份、摘要热缓存和实例锁。
 * @pre 外层已关闭新入口，排空全部运行、provider/工具/应用回调、直接借用及锁等待者。
 * @param[in,out] runtime 待销毁实例，可为 NULL。
 * @post 实例及拥有内存已释放，注入依赖继续归其原拥有者，原 runtime 指针由调用者自行清空。
 */
void cc_agent_runtime_destroy(cc_agent_runtime_t *runtime)
{
    if (!runtime) return;
    cc_mutex_lock(runtime->mutex);
    free(runtime->config.system_prompt);
    free(runtime->config.workspace_dir);
    free(runtime->config.model);
    free(runtime->config.model_extra_body_json);
    free(runtime->config.active_memory_category);
    free(runtime->provider_name);
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


/**
 * @brief 在实例锁内设置后续请求读取的 thinking 标志。
 * @param[in,out] runtime 借用实例，NULL 时跳过。
 * @param[in] enabled 非零归一为一，零保持零。
 * @post 有效实例的标志已更新，实际 provider 行为由其能力和请求实现决定。
 */
void cc_agent_runtime_set_thinking_mode(cc_agent_runtime_t *runtime, int enabled)
{
    if (!runtime) return;
    cc_mutex_lock(runtime->mutex);
    runtime->thinking_mode = enabled ? 1 : 0;
    cc_mutex_unlock(runtime->mutex);
}


/**
 * @brief 在实例锁内配对替换后续审批使用的函数和上下文。
 * @param[in,out] runtime 借用实例，NULL 时跳过。
 * @param[in] approve_tool_call 可选借用审批函数，NULL 清除当前配置。
 * @param[in] user_data 借用上下文，寿命须覆盖已取得该配对快照的执行调用，旧上下文由原拥有者在借用结束后释放。
 * @post 后续审批可取得新配对，已经取得旧快照的调用继续使用旧上下文，替换操作只保存借用关系。
 */
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


/**
 * @brief 在实例锁内读取 thinking 标志。
 * @param[in] runtime 借用实例，可为 NULL。
 * @return 当前保存标志，NULL 实例为零，创建初值沿 options，setter 更新后为零或一。
 */
int cc_agent_runtime_get_thinking_mode(cc_agent_runtime_t *runtime)
{
    if (!runtime) return 0;
    cc_mutex_lock(runtime->mutex);
    int enabled = runtime->thinking_mode;
    cc_mutex_unlock(runtime->mutex);
    return enabled;
}

/**
 * @brief 取得创建时注入的事件总线借用指针。
 * @param[in] runtime 借用实例，可为 NULL。
 * @return 注入的 event_bus，空实例或未注入为 NULL，寿命与销毁权限由原拥有者协调。
 */
cc_event_bus_t *cc_agent_runtime_event_bus(cc_agent_runtime_t *runtime)
{
    return runtime ? runtime->event_bus : NULL;
}

/**
 * @brief 取得创建时注入的工具注册表借用指针。
 * @param[in] runtime 借用实例，可为 NULL。
 * @return tool_registry，空实例或未注入为 NULL，表本体及工具的拥有和冻结状态沿 registry 契约。
 */
cc_tool_registry_t *cc_agent_runtime_tool_registry(cc_agent_runtime_t *runtime)
{
    return runtime ? runtime->tool_registry : NULL;
}

/**
 * @brief 取得创建时注入的工具执行池借用指针。
 * @param[in] runtime 借用实例，可为 NULL。
 * @return tool_pool，空实例或未注入为 NULL，池寿命及关闭由原拥有者负责。
 */
cc_tool_executor_pool_t *cc_agent_runtime_tool_pool(cc_agent_runtime_t *runtime)
{
    return runtime ? runtime->tool_pool : NULL;
}

/**
 * @brief 取得 runtime 内嵌 SessionStore 端口描述的借用地址。
 * @param[in] runtime 借用实例，可为 NULL。
 * @return 内嵌端口地址，NULL 实例为 NULL，地址寿命止于 runtime 销毁，端口 self/vtable 继续归依赖拥有者管理。
 */
cc_session_store_t *cc_agent_runtime_session_store(cc_agent_runtime_t *runtime)
{
    return runtime ? &runtime->store : NULL;
}

/**
 * @brief 按字节比较本 generation 创建时复制的 workspace 字符串。
 * @param[in] runtime 借用实例，可为 NULL，配置字符串创建后保持固定。
 * @param[in] workspace 借用候选路径，可为 NULL。
 * @return 两路径存在且 strcmp 相等为一，其余为零，比较使用原路径字符串。
 */
int cc_agent_runtime_workspace_matches(const cc_agent_runtime_t *runtime, const char *workspace)
{
    return runtime && workspace && runtime->config.workspace_dir &&
           strcmp(runtime->config.workspace_dir,workspace)==0;
}


/**
 * @brief 检查当前 provider 是否提供原生 chat_stream 函数。
 * @param[in] runtime 借用实例，可为 NULL。
 * @return 存在原生流函数为非零，其余为零，具体能力仍由 provider 实现返回结果解释。
 */
int cc_agent_runtime_supports_stream(cc_agent_runtime_t *runtime)
{
    return runtime && runtime->llm.vtable && runtime->llm.vtable->chat_stream;
}


/**
 * @brief 调用可选 SessionStore 会话创建入口，提供会话 workspace。
 * @param[in] runtime 借用实例，调用期依赖保持有效。
 * @param[in] session_id 必需借用会话标识，具体非空及重复规则由 store 校验。
 * @param[in] workspace_dir 可选借用路径，NULL 使用 runtime 配置路径。
 * @return 空必需参数为 CC_ERR_INVALID_ARGUMENT，无 create_session 入口为 CC_OK，其他结果沿后端。
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




/**
 * @brief 取得运行准入和可选新检查点后，以同步答复接口执行统一模型循环。
 * @param[in,out] runtime 必需借用实例，准入计数由本函数配对管理。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] user_input 必需借用原输入，checkpoint 路径进一步要求非空。
 * @param[in] options 可选借用同步选项，token 生命周期覆盖整个调用。
 * @param[out] out_response 基础参数合法后置空，成功答复或错误诊断均可能拥有，调用者在所有返回路径 free。
 * @return 参数、准入、检查点、统一引擎及 claim 释放结果原样返回。
 */
cc_result_t cc_agent_runtime_handle_message_with_options(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    const cc_agent_runtime_run_options_t *options,
    char **out_response
)
{
    if (!runtime || !session_id || !user_input || !out_response)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run request");
    *out_response = NULL;
    cc_result_t enter_rc = runtime_enter_run(runtime);
    if (enter_rc.code != CC_OK) return enter_rc;
    cc_result_free(&enter_rc);
    cc_run_checkpoint_t checkpoint;
    int claimed = 0;
    cc_result_t rc = runtime_start_checkpoint(runtime, session_id, user_input, NULL, &checkpoint, &claimed);
    if (rc.code == CC_OK) {
        rc = cc_agent_runtime_handle_message_internal(
            runtime, session_id, user_input, options, NULL,
            runtime->run_checkpoints ? &checkpoint : NULL, out_response);
    }
    rc = runtime_release_checkpoint(runtime, &checkpoint, claimed, rc);
    runtime_leave_run(runtime);
    return rc;
}

/**
 * @brief 以缺省同步选项委托消息处理入口。
 * @param[in,out] runtime 必需借用实例，外层保持其依赖和 generation 租约有效。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] user_input 必需借用原输入。
 * @param[out] out_response 必需空资源输出槽，规则沿带选项入口，任何非 NULL 正文归调用者 free。
 * @return 带选项消息处理入口的结果原样返回。
 */
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
