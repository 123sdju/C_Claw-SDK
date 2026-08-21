

#include "cc/app/cc_context_builder.h"
#include "cc_agent_runtime_internal.h"
#include "cc/app/cc_memory_context.h"
#include "cc/core/cc_message.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"
#include "cc/util/cc_token_counter.h"
#include "cc/ports/cc_platform.h"
#include "cc/ports/cc_tool_registry.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define MAX_LOAD_MESSAGES 500

/* context builder 内部使用的动态 message 数组。 */
typedef struct message_vec {
    cc_message_t *items;
    size_t count;
    size_t capacity;
} message_vec_t;


/* 清理 message_vec 中每条深拷贝消息和数组缓冲。 */
static void message_vec_cleanup(message_vec_t *vec)
{
    if (!vec) return;
    for (size_t i = 0; i < vec->count; i++) cc_message_cleanup(&vec->items[i]);
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

/* 向 message_vec 追加 message 深拷贝。 */
static cc_result_t message_vec_append_copy(message_vec_t *vec, const cc_message_t *message)
{
    if (!vec || !message) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message vector append");
    }
    if (vec->count == vec->capacity) {
        size_t next_cap = vec->capacity ? vec->capacity * 2 : 8;
        cc_message_t *next = realloc(vec->items, next_cap * sizeof(cc_message_t));
        if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow message vector");
        memset(next + vec->capacity, 0, (next_cap - vec->capacity) * sizeof(cc_message_t));
        vec->items = next;
        vec->capacity = next_cap;
    }
    cc_result_t rc = cc_message_copy(message, &vec->items[vec->count]);
    if (rc.code != CC_OK) return rc;
    vec->count++;
    return cc_result_ok();
}


/*
 * 创建一条临时文本 message 并追加到 vector。
 *
 * 用于 system prompt、memory block 和 summary 这类构造出来的上下文消息。
 */
static cc_result_t message_vec_append_text(
    message_vec_t *vec,
    cc_message_role_t role,
    const char *text
)
{
    cc_message_t *msg = NULL;
    cc_result_t rc = cc_message_create_text(NULL, NULL, role, text ? text : "", NULL, &msg);
    if (rc.code != CC_OK) return rc;
    rc = message_vec_append_copy(vec, msg);
    cc_message_destroy(msg);
    return rc;
}


/* 释放 session store 加载出的 message 数组。 */
static void free_loaded_messages(cc_message_t *messages, size_t count)
{
    for (size_t i = 0; i < count; i++) cc_message_cleanup(&messages[i]);
    free(messages);
}

/*
 * 估算一组消息的 token 数。
 *
 * 先序列化成 JSON，再用轻量 token 估算器计算；失败时返回 0，让调用方倾向于不压缩。
 */
static int messages_token_estimate(const cc_message_t *messages, size_t count)
{
    char *json = NULL;


    cc_result_t rc = cc_messages_to_json(messages, count, 1, &json);
    if (rc.code != CC_OK || !json) {
        cc_result_free(&rc);
        return 0;
    }
    int tokens = cc_token_estimate_json_messages(json);
    free(json);
    cc_result_free(&rc);
    return tokens;
}

static cc_result_t build_history_suffix_costs(
    const cc_message_t *messages,
    size_t count,
    int **out_suffix
)
{
    if (!out_suffix) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null token cost output");
    *out_suffix = NULL;
    if (count > (SIZE_MAX / sizeof(int)) - 1) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "History token index is too large");
    }
    int *suffix = calloc(count + 1, sizeof(int));
    if (!suffix) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate history token index");
    for (size_t i = count; i > 0; i--) {
        int item_cost = messages_token_estimate(&messages[i - 1], 1);
        if (item_cost < 0) item_cost = 0;
        if (suffix[i] > INT_MAX - item_cost) {
            free(suffix);
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "History token estimate overflow");
        }
        suffix[i - 1] = suffix[i] + item_cost;
    }
    *out_suffix = suffix;
    return cc_result_ok();
}

static cc_result_t fixed_context_cost(
    cc_agent_runtime_t *runtime,
    const char *headers,
    int *out_tokens
)
{
    if (!runtime || !out_tokens) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid context cost request");
    int total = cc_token_estimate(headers ? headers : "");
    char *tools_json = NULL;
    cc_result_t rc = cc_tool_registry_build_schema_json(runtime->tool_registry, &tools_json);
    if (rc.code != CC_OK) return rc;
    int tool_tokens = cc_token_estimate(tools_json ? tools_json : "[]");
    free(tools_json);
    if (tool_tokens > INT_MAX - total) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Fixed context token estimate overflow");
    }
    *out_tokens = total + tool_tokens;
    return cc_result_ok();
}


/*
 * 把 message 转为摘要 prompt 中的人类可读文本。
 *
 * 文本内容通过 message summary 获取；tool_calls 追加 JSON，确保压缩历史时不丢失工具调用。
 */
static cc_result_t append_message_plaintext(cc_string_builder_t *sb, const cc_message_t *msg)
{
    char *summary = NULL;
    cc_result_t rc = cc_message_get_text_summary(msg, &summary);
    if (rc.code != CC_OK) {
        free(summary);
        return rc;
    }
    if (summary) {
        rc = cc_string_builder_append(sb, summary);
    }
    free(summary);
    if (rc.code == CC_OK && msg->tool_calls.count > 0) {
        char *tool_calls = NULL;
        rc = cc_tool_call_list_to_json(&msg->tool_calls, &tool_calls);
        if (rc.code == CC_OK && tool_calls) {
            rc = cc_string_builder_append(sb, "\n");
            if (rc.code == CC_OK) rc = cc_string_builder_append(sb, tool_calls);
        }
        free(tool_calls);
    }
    return rc;
}

static uint64_t fnv1a64(const void *data, size_t length)
{
    const unsigned char *bytes = (const unsigned char *)data;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int history_fingerprint(
    const cc_message_t *messages,
    size_t count,
    uint64_t *out_fingerprint
)
{
    char *json = NULL;
    cc_result_t rc = cc_messages_to_json(messages, count, 1, &json);
    if (rc.code != CC_OK || !json) {
        cc_result_free(&rc);
        free(json);
        return 0;
    }
    *out_fingerprint = fnv1a64(json, strlen(json));
    free(json);
    cc_result_free(&rc);
    return 1;
}

static uint64_t summary_configuration_fingerprint(const cc_agent_runtime_t *runtime)
{
    const char *model = runtime && runtime->config.model ? runtime->config.model : "";
    const char *prompt = runtime && runtime->config.system_prompt ?
        runtime->config.system_prompt : "";
    uint64_t hash = fnv1a64(model, strlen(model));
    hash ^= fnv1a64(prompt, strlen(prompt));
    hash *= UINT64_C(1099511628211);
    return hash;
}

static int summary_persistent_key(
    const cc_agent_runtime_t *runtime,
    const char *session_id,
    uint64_t history_hash,
    size_t summarized_count,
    char *out,
    size_t out_size)
{
    uint64_t session_hash = fnv1a64(
        session_id ? session_id : "", session_id ? strlen(session_id) : 0);
    int written = snprintf(
        out,
        out_size,
        "__cclaw_context_summary_v1_%016llx_%016llx_%016llx_%llu",
        (unsigned long long)session_hash,
        (unsigned long long)summary_configuration_fingerprint(runtime),
        (unsigned long long)history_hash,
        (unsigned long long)summarized_count);
    return written > 0 && (size_t)written < out_size;
}

static char *summary_persistent_lookup(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    uint64_t history_hash,
    size_t summarized_count)
{
    if (!runtime || !runtime->memory_store) return NULL;
    char key[160];
    if (!summary_persistent_key(
            runtime, session_id, history_hash, summarized_count, key, sizeof(key))) {
        return NULL;
    }
    cc_memory_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    cc_result_t rc = cc_memory_store_get(runtime->memory_store, key, &entry);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return NULL;
    }
    char *summary = NULL;
    if (entry.value &&
        entry.category && strcmp(entry.category, "context_summary") == 0 &&
        entry.session_id && strcmp(entry.session_id, session_id) == 0) {
        summary = cc_copy_string(entry.value);
    }
    cc_memory_entry_free(&entry);
    cc_result_free(&rc);
    return summary;
}

static char *summary_persistent_predecessor(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_message_t *history,
    size_t target_count,
    size_t *out_summarized_count)
{
    if (out_summarized_count) *out_summarized_count = 0;
    if (!runtime || !runtime->memory_store || target_count < 3) return NULL;
    cc_memory_query_t query = {
        .size = sizeof(query),
        .query = "cclaw_context_summary_v1",
        .category = "context_summary",
        .session_id = session_id,
        .limit = 32,
    };
    cc_memory_search_result_t *results = NULL;
    size_t result_count = 0;
    cc_result_t rc = cc_memory_store_query(
        runtime->memory_store, &query, &results, &result_count);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return NULL;
    }
    cc_result_free(&rc);

    uint64_t expected_session = fnv1a64(
        session_id ? session_id : "", session_id ? strlen(session_id) : 0);
    uint64_t expected_config = summary_configuration_fingerprint(runtime);
    char *best = NULL;
    size_t best_count = 0;
    for (size_t i = 0; i < result_count; i++) {
        unsigned long long session_hash = 0;
        unsigned long long config_hash = 0;
        unsigned long long history_hash = 0;
        unsigned long long summarized_count = 0;
        int matched = sscanf(
            results[i].entry.key ? results[i].entry.key : "",
            "__cclaw_context_summary_v1_%llx_%llx_%llx_%llu",
            &session_hash, &config_hash, &history_hash, &summarized_count);
        if (matched != 4 ||
            session_hash != (unsigned long long)expected_session ||
            config_hash != (unsigned long long)expected_config ||
            summarized_count < 3 || summarized_count >= target_count ||
            summarized_count <= best_count) {
            continue;
        }
        uint64_t actual_history_hash = 0;
        if (!history_fingerprint(
                history, (size_t)summarized_count, &actual_history_hash) ||
            actual_history_hash != (uint64_t)history_hash) {
            continue;
        }
        char *candidate = cc_copy_string(results[i].entry.value);
        if (!candidate) continue;
        free(best);
        best = candidate;
        best_count = (size_t)summarized_count;
    }
    cc_memory_search_result_free_array(results, result_count);
    if (best && out_summarized_count) *out_summarized_count = best_count;
    return best;
}

static void summary_persistent_store(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    uint64_t history_hash,
    size_t summarized_count,
    const char *summary)
{
    if (!runtime || !runtime->memory_store || !summary || !summary[0]) return;
    char key[160];
    if (!summary_persistent_key(
            runtime, session_id, history_hash, summarized_count, key, sizeof(key))) {
        return;
    }
    cc_result_t rc = cc_memory_store_set(
        runtime->memory_store, key, summary, "context_summary", session_id);
    /* Derived cache persistence is best-effort and never advances authoritative history. */
    cc_result_free(&rc);
}

static char *summary_cache_lookup(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    uint64_t fingerprint,
    size_t summarized_count
)
{
    char *result = NULL;
    cc_mutex_lock(runtime->mutex);
    for (size_t i = 0; i < CC_CONTEXT_SUMMARY_CACHE_SLOTS; i++) {
        cc_context_summary_cache_entry_t *entry = &runtime->summary_cache[i];
        if (entry->session_id && entry->summary &&
            entry->history_fingerprint == fingerprint &&
            entry->summarized_count == summarized_count &&
            strcmp(entry->session_id, session_id) == 0) {
            result = cc_copy_string(entry->summary);
            entry->last_used = ++runtime->summary_cache_clock;
            break;
        }
    }
    cc_mutex_unlock(runtime->mutex);
    return result;
}

static void summary_cache_store(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    uint64_t fingerprint,
    size_t summarized_count,
    const char *summary
)
{
    char *session_copy = cc_copy_string(session_id);
    char *summary_copy = cc_copy_string(summary);
    if (!session_copy || !summary_copy) {
        free(session_copy);
        free(summary_copy);
        return;
    }

    cc_mutex_lock(runtime->mutex);
    size_t victim = 0;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; i < CC_CONTEXT_SUMMARY_CACHE_SLOTS; i++) {
        cc_context_summary_cache_entry_t *entry = &runtime->summary_cache[i];
        if (entry->session_id &&
            entry->history_fingerprint == fingerprint &&
            entry->summarized_count == summarized_count &&
            strcmp(entry->session_id, session_id) == 0) {
            victim = i;
            oldest = 0;
            break;
        }
        if (!entry->session_id) {
            victim = i;
            oldest = 0;
            break;
        }
        if (entry->last_used < oldest) {
            oldest = entry->last_used;
            victim = i;
        }
    }
    cc_context_summary_cache_entry_t *entry = &runtime->summary_cache[victim];
    free(entry->session_id);
    free(entry->summary);
    entry->session_id = session_copy;
    entry->summary = summary_copy;
    entry->history_fingerprint = fingerprint;
    entry->summarized_count = summarized_count;
    entry->last_used = ++runtime->summary_cache_clock;
    cc_mutex_unlock(runtime->mutex);
}

static int summary_fits_history_budget(
    const char *summary,
    int recent_history_tokens,
    int history_budget
)
{
    if (!summary || history_budget <= 0 || recent_history_tokens < 0) return 0;
    int label_tokens = cc_token_estimate("Earlier conversation summary:");
    int summary_tokens = cc_token_estimate(summary);
    if (label_tokens < 0 || summary_tokens < 0 ||
        label_tokens > INT_MAX - summary_tokens) {
        return 0;
    }
    int total_summary_tokens = label_tokens + summary_tokens;
    return total_summary_tokens <= history_budget &&
           recent_history_tokens <= history_budget - total_summary_tokens;
}

/*
 * 尝试用 LLM 压缩历史消息。
 *
 * 只压缩较早历史，保留近期消息原文。压缩失败时返回 0，调用方会回退到截断策略，这样
 * provider 错误不会阻塞主请求。
 */
static int try_compress_history(
    cc_agent_runtime_t *runtime,
    cc_message_t *messages,
    int start_idx,
    int end_idx,
    const char *prior_summary,
    cc_cancel_token_t *cancel_token,
    char **out_summary
)
{
    *out_summary = NULL;
    if (!runtime || !runtime->llm.vtable || !runtime->llm.vtable->chat) return 0;
    if ((!prior_summary || !prior_summary[0]) && end_idx - start_idx <= 2) return 0;



    cc_string_builder_t sb;
    if (cc_string_builder_init(&sb).code != CC_OK) return 0;
    cc_result_t rc = cc_string_builder_append(&sb,
        "Summarize the following conversation into a concise paragraph. "
        "Preserve key facts, decisions, tool results, file paths, artifact ids, "
        "and explicit user preferences. Output ONLY the summary paragraph.\n\n");
    if (rc.code == CC_OK && prior_summary && prior_summary[0]) {
        rc = cc_string_builder_append(
            &sb, "[previous verified summary]: ");
        if (rc.code == CC_OK) rc = cc_string_builder_append(&sb, prior_summary);
        if (rc.code == CC_OK) rc = cc_string_builder_append(&sb, "\n");
    }
    for (int i = start_idx; rc.code == CC_OK && i < end_idx; i++) {
        rc = cc_string_builder_append(&sb, "[");
        if (rc.code == CC_OK) {
            rc = cc_string_builder_append(&sb, cc_message_role_string(messages[i].role));
        }
        if (rc.code == CC_OK) rc = cc_string_builder_append(&sb, "]: ");
        if (rc.code == CC_OK) rc = append_message_plaintext(&sb, &messages[i]);
        if (rc.code == CC_OK) rc = cc_string_builder_append(&sb, "\n");
    }
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        cc_string_builder_deinit(&sb);
        return 0;
    }
    char *prompt = cc_string_builder_take(&sb);
    if (!prompt) return 0;

    cc_message_t *prompt_msg = NULL;
    rc = cc_message_create_text(
        "summary_prompt", "summary", CC_ROLE_USER, prompt, NULL, &prompt_msg);
    free(prompt);
    if (rc.code != CC_OK) return 0;

    cc_llm_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.messages = prompt_msg;
    req.message_count = 1;
    req.model = runtime->config.model;
    req.max_tokens = runtime->config.summary_max_tokens;
    req.temperature = runtime->config.summary_temperature;
    req.stream = 0;
    req.cancel_token = cancel_token;
    req.timeout_ms = runtime->config.limits.provider_timeout_ms;
    req.max_response_bytes = runtime->config.limits.max_output_bytes;
    if (req.timeout_ms > 0) {
        req.deadline_ms = cc_platform_monotonic_ms() + (uint64_t)req.timeout_ms;
    }



    cc_llm_response_t resp;
    cc_llm_response_init(&resp);
    rc = runtime->llm.vtable->chat(runtime->llm.self, &req, &resp);
    cc_message_destroy(prompt_msg);
    if (rc.code != CC_OK || !resp.has_text || !resp.text) {
        cc_result_free(&rc);
        cc_llm_response_free(&resp);
        return 0;
    }
    *out_summary = cc_copy_string(resp.text);
    cc_result_free(&rc);
    cc_llm_response_free(&resp);
    return *out_summary != NULL;
}


/*
 * 追加上下文头部消息。
 *
 * system_prompt 作为 system 消息放在最前；active memory 检索到的 block 也作为 system
 * 消息注入。memory 注入失败不会吞掉非 OK rc，保证真正的构造错误可返回给调用方。
 */
static cc_result_t append_header_block(
    cc_string_builder_t *sb,
    const char *label,
    const char *text
)
{
    if (!sb || !text || !text[0]) return cc_result_ok();
    if (sb->length > 0) {
        cc_result_t rc = cc_string_builder_append(sb, "\n\n");
        if (rc.code != CC_OK) return rc;
    }
    if (label && label[0]) {
        cc_result_t rc = cc_string_builder_append(sb, label);
        if (rc.code != CC_OK) return rc;
        rc = cc_string_builder_append(sb, "\n");
        if (rc.code != CC_OK) return rc;
    }
    return cc_string_builder_append(sb, text);
}

/*
 * 收集基础头信息：将系统提示词和 memory_store 检索到的活跃记忆注入到 headers 中。
 * 先追加 system_prompt，再从 runtime 的 memory_store 检索相关记忆并追加。
 * 参数: runtime       - Agent 运行时实例
 *       system_prompt - 系统提示词
 *       headers       - 字符串构建器（累积输出）
 * 返回: cc_result_t
 */
static cc_result_t collect_base_headers(
    cc_agent_runtime_t *runtime,
    const char *system_prompt,
    cc_string_builder_t *headers
)
{
    cc_result_t rc = append_header_block(headers, NULL, system_prompt);
    if (rc.code != CC_OK) return rc;
    if (runtime && runtime->memory_store) {
        char *mem_block = NULL;
        rc = cc_memory_context_inject(runtime->memory_store, system_prompt, &mem_block);
        if (rc.code == CC_OK) {
            rc = append_header_block(headers, "Active memory:", mem_block);
        }
        free(mem_block);
        if (rc.code != CC_OK) return rc;
    }
    return cc_result_ok();
}

/*
 * 从历史消息范围中收集 system 角色消息：遍历 [start, end) 区间，提取每条 system
 * 消息的文本摘要并追加到 headers 中，标注为 "Session system context:"。
 * 参数: headers - 字符串构建器（累积输出）
 *       history - 历史消息数组
 *       start   - 起始索引
 *       end     - 结束索引（不包含）
 * 返回: cc_result_t
 */
static cc_result_t collect_history_system_headers(
    cc_string_builder_t *headers,
    const cc_message_t *history,
    size_t start,
    size_t end
)
{
    for (size_t i = start; i < end; i++) {
        if (history[i].role != CC_ROLE_SYSTEM) continue;
        char *summary = NULL;
        cc_result_t rc = cc_message_get_text_summary(&history[i], &summary);
        if (rc.code != CC_OK) {
            free(summary);
            return rc;
        }
        rc = append_header_block(headers, "Session system context:", summary);
        free(summary);
        if (rc.code != CC_OK) return rc;
    }
    return cc_result_ok();
}

/*
 * 按 tool_call_id 在 assistant 消息的 tool_calls 数组中查找索引：
 * 线性遍历，匹配 id 字符串相等则返回对应位置。
 * 参数: assistant    - assistant 角色消息
 *       tool_call_id - 要查找的 tool call ID
 * 返回: 找到返回索引（≥0），找不到或参数无效返回 -1
 */
static int tool_call_index_by_id(const cc_message_t *assistant, const char *tool_call_id)
{
    if (!assistant || !tool_call_id || !tool_call_id[0]) return -1;
    for (size_t i = 0; i < assistant->tool_calls.count; i++) {
        const char *id = assistant->tool_calls.items[i].id;
        if (id && strcmp(id, tool_call_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/*
 * 判断消息是否为控制命令：提取 user 消息的文本摘要，跳过前导空白后检查是否以 '/' 开头。
 * 控制命令消息在构建上下文时会被过滤，不送入 LLM。
 * 参数: message - 待检查的消息
 * 返回: 1 表示是控制命令，0 表示不是或参数无效
 */
static int message_is_control_command(const cc_message_t *message)
{
    if (!message || message->role != CC_ROLE_USER) return 0;
    char *summary = NULL;
    cc_result_t rc = cc_message_get_text_summary(message, &summary);
    if (rc.code != CC_OK || !summary) {
        cc_result_free(&rc);
        free(summary);
        return 0;
    }
    char *text = summary;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }
    int is_command = *text == '/';
    free(summary);
    cc_result_free(&rc);
    return is_command;
}

/*
 * 追加有效的 tool 调用组到输出列表：验证 assistant 消息的每个 tool_call 是否在其后
 * 均有对应的 tool 角色响应消息。仅当全部 tool_call 匹配时才将 assistant + tool 消息
 * 整体追加到 out 中，不完整则跳过该组。
 * 参数: out             - 输出消息列表
 *       history         - 历史消息数组
 *       assistant_index - assistant 消息在 history 中的索引
 *       end             - 历史消息结束索引（不包含）
 *       out_next_index  - 输出处理后的下一个索引位置
 * 返回: cc_result_t
 */
static cc_result_t append_valid_tool_group(
    message_vec_t *out,
    const cc_message_t *history,
    size_t assistant_index,
    size_t end,
    size_t *out_next_index
)
{
    const cc_message_t *assistant = &history[assistant_index];
    size_t call_count = assistant->tool_calls.count;
    *out_next_index = assistant_index + 1;
    if (call_count == 0) {
        return message_vec_append_copy(out, assistant);
    }

    bool *matched = calloc(call_count, sizeof(bool));
    if (!matched) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to validate tool message group");
    }

    size_t matched_count = 0;
    size_t group_end = assistant_index + 1;
    while (group_end < end && history[group_end].role == CC_ROLE_TOOL) {
        int index = tool_call_index_by_id(assistant, history[group_end].tool_call_id);
        if (index >= 0 && !matched[index]) {
            matched[index] = true;
            matched_count++;
        }
        group_end++;
    }

    if (matched_count != call_count) {
        free(matched);
        *out_next_index = group_end;
        return cc_result_ok();
    }

    memset(matched, 0, call_count * sizeof(bool));
    cc_result_t rc = message_vec_append_copy(out, assistant);
    for (size_t i = assistant_index + 1; rc.code == CC_OK && i < group_end; i++) {
        int index = tool_call_index_by_id(assistant, history[i].tool_call_id);
        if (index < 0 || matched[index]) {
            continue;
        }
        matched[index] = true;
        rc = message_vec_append_copy(out, &history[i]);
    }
    free(matched);
    *out_next_index = group_end;
    return rc;
}

/*
 * 过滤并追加指定范围的历史消息：遍历 [start, end)，跳过 system/tool/控制命令消息，
 * 对含 tool_calls 的 assistant 消息调用 append_valid_tool_group 验证后追加，
 * 其余消息直接深拷贝追加到 out。
 * 参数: out     - 输出消息列表
 *       history - 历史消息数组
 *       start   - 起始索引
 *       end     - 结束索引（不包含）
 * 返回: cc_result_t
 */
static cc_result_t append_history_range_sanitized(
    message_vec_t *out,
    const cc_message_t *history,
    size_t start,
    size_t end
)
{
    size_t i = start;
    while (i < end) {
        const cc_message_t *msg = &history[i];
        if (msg->role == CC_ROLE_SYSTEM ||
            msg->role == CC_ROLE_TOOL ||
            message_is_control_command(msg)) {
            i++;
            continue;
        }
        if (msg->role == CC_ROLE_ASSISTANT && msg->tool_calls.count > 0) {
            size_t next = i + 1;
            cc_result_t rc = append_valid_tool_group(out, history, i, end, &next);
            if (rc.code != CC_OK) return rc;
            i = next;
            continue;
        }
        cc_result_t rc = message_vec_append_copy(out, msg);
        if (rc.code != CC_OK) return rc;
        i++;
    }
    return cc_result_ok();
}


/*
 * 构建一次 LLM 请求的消息数组。
 *
 * 流程：加载 session 历史、追加 system/memory 头、估算 token、超过阈值时尝试 LLM 摘要，
 * 摘要失败则按预算保留最近消息。返回数组由调用方逐项 cleanup 后 free。
 */
cc_result_t cc_context_builder_build_messages(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *system_prompt,
    cc_cancel_token_t *cancel_token,
    cc_message_t **out_messages,
    size_t *out_count
)
{
    if (!runtime || !session_id || !out_messages || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null context builder argument");
    }
    *out_messages = NULL;
    *out_count = 0;
    if (cancel_token && cc_cancel_token_is_cancelled(cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "Context build cancelled");
    }

    cc_message_t *history = NULL;
    size_t history_count = 0;
    cc_result_t rc = runtime->store.vtable->load_messages(
        runtime->store.self, session_id, MAX_LOAD_MESSAGES, &history, &history_count);
    if (rc.code != CC_OK) return rc;

    cc_string_builder_t headers;
    rc = cc_string_builder_init(&headers);
    if (rc.code != CC_OK) {
        free_loaded_messages(history, history_count);
        return rc;
    }
    rc = collect_base_headers(runtime, system_prompt, &headers);
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&headers);
        free_loaded_messages(history, history_count);
        return rc;
    }

    int keep_recent = runtime->config.context_keep_recent > 0 ?
        runtime->config.context_keep_recent : 20;
    int budget = runtime->config.context_window_tokens;
    double threshold = runtime->config.context_compress_threshold;
    int *history_suffix_tokens = NULL;
    rc = build_history_suffix_costs(history, history_count, &history_suffix_tokens);
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&headers);
        free_loaded_messages(history, history_count);
        return rc;
    }
    int fixed_tokens = 0;
    rc = fixed_context_cost(runtime, cc_string_builder_cstr(&headers), &fixed_tokens);
    if (rc.code != CC_OK) {
        free(history_suffix_tokens);
        cc_string_builder_deinit(&headers);
        free_loaded_messages(history, history_count);
        return rc;
    }
    int output_reserve = runtime->config.max_tokens > 0 ? runtime->config.max_tokens : 0;
    int history_budget = budget;
    if (history_budget > 0) {
        int reserved = fixed_tokens > INT_MAX - output_reserve
            ? INT_MAX : fixed_tokens + output_reserve;
        history_budget = reserved >= history_budget ? 0 : history_budget - reserved;
    }
    int history_tokens = history_suffix_tokens[0];

    int compressed = 0;
    size_t history_start = 0;
    size_t history_end = history_count;


    if (history_budget > 0 && threshold > 0.0 &&
        history_tokens > (int)(history_budget * threshold) &&
        history_count > (size_t)(keep_recent + 2)) {
        int end_idx = (int)history_count - keep_recent;
        char *summary = NULL;
        uint64_t fingerprint = 0;
        int cacheable = history_fingerprint(history, (size_t)end_idx, &fingerprint);
        if (cacheable) {
            summary = summary_cache_lookup(
                runtime, session_id, fingerprint, (size_t)end_idx);
        }
        if (!summary && cacheable) {
            summary = summary_persistent_lookup(
                runtime, session_id, fingerprint, (size_t)end_idx);
            if (summary) {
                summary_cache_store(
                    runtime, session_id, fingerprint, (size_t)end_idx, summary);
            }
        }
        if (!summary) {
            size_t predecessor_count = 0;
            char *predecessor = cacheable ? summary_persistent_predecessor(
                runtime, session_id, history, (size_t)end_idx, &predecessor_count) : NULL;
            (void)try_compress_history(
                runtime,
                history,
                (int)predecessor_count,
                end_idx,
                predecessor,
                cancel_token,
                &summary);
            free(predecessor);
            if (summary && cacheable &&
                (!cancel_token || !cc_cancel_token_is_cancelled(cancel_token))) {
                summary_cache_store(
                    runtime, session_id, fingerprint, (size_t)end_idx, summary);
                summary_persistent_store(
                    runtime, session_id, fingerprint, (size_t)end_idx, summary);
            }
        }
        if (summary && !summary_fits_history_budget(
                summary, history_suffix_tokens[end_idx], history_budget)) {
            free(summary);
            summary = NULL;
        }
        if (summary) {
            rc = append_header_block(&headers, "Earlier conversation summary:", summary);
            free(summary);
            summary = NULL;
            if (rc.code != CC_OK) {
                free(history_suffix_tokens);
                cc_string_builder_deinit(&headers);
                free_loaded_messages(history, history_count);
                return rc;
            }
            history_start = (size_t)end_idx;
            compressed = 1;
        }
        if (cancel_token && cc_cancel_token_is_cancelled(cancel_token)) {
            free(summary);
            free(history_suffix_tokens);
            cc_string_builder_deinit(&headers);
            free_loaded_messages(history, history_count);
            return cc_result_error(CC_ERR_CANCELLED, "Context summary cancelled");
        }
    }

    if (!compressed) {
        if (budget > 0 && history_tokens > history_budget && history_count > 0) {
            history_start = history_count;
            while (history_start > 0 &&
                   history_suffix_tokens[history_start - 1] <= history_budget) {
                history_start--;
            }
        }
    }

    rc = collect_history_system_headers(&headers, history, history_start, history_end);
    if (rc.code != CC_OK) {
        free(history_suffix_tokens);
        cc_string_builder_deinit(&headers);
        free_loaded_messages(history, history_count);
        return rc;
    }

    message_vec_t out;
    memset(&out, 0, sizeof(out));
    const char *header_text = cc_string_builder_cstr(&headers);
    if (header_text && header_text[0]) {
        rc = message_vec_append_text(&out, CC_ROLE_SYSTEM, header_text);
    }
    if (rc.code == CC_OK) {
        rc = append_history_range_sanitized(&out, history, history_start, history_end);
    }
    cc_string_builder_deinit(&headers);
    free(history_suffix_tokens);
    free_loaded_messages(history, history_count);
    if (rc.code != CC_OK) {
        message_vec_cleanup(&out);
        return rc;
    }
    *out_messages = out.items;
    *out_count = out.count;
    return cc_result_ok();
}
