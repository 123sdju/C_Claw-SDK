



#include "cc/adapters/cc_http_llm_provider.h"
#include "cc/core/cc_observability.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_json.h"
#include "cc/util/cc_redaction.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>

#ifndef CC_HTTP_LLM_STREAM_ERROR_PREVIEW_BYTES
#define CC_HTTP_LLM_STREAM_ERROR_PREVIEW_BYTES 4096
#endif

/* 单个尚未形成完整 SSE/NDJSON 事件的最大缓存，防止异常端点持续输出无分隔数据。 */
#ifndef CC_HTTP_LLM_STREAM_MAX_PENDING_FRAME_BYTES
#define CC_HTTP_LLM_STREAM_MAX_PENDING_FRAME_BYTES (64 * 1024)
#endif

/*
 * 通用 HTTP LLM provider 私有状态。
 *
 * provider 只保存 base_url/api_key/model 和协议 vtable；OpenAI/Anthropic/Ollama 的差异
 * 放在 cc_llm_protocol_t 中。这样 HTTP 传输、错误分类和 stream framing 可以复用。
 */
typedef struct cc_http_llm_provider {
    cc_http_client_t *http_client;
    char *base_url;
    char *api_key;
    char *model;
    cc_llm_protocol_t protocol;
} cc_http_llm_provider_t;

typedef struct cc_http_llm_metrics_ctx {
    cc_event_bus_t *event_bus;
    const char *session_id;
    int step;
    const int *stream_saw_done;
    const int *stream_partial;
} cc_http_llm_metrics_ctx_t;

/*
 * 记录 HTTP 请求尝试的耗时指标（连接、首字节、总耗时等），并通过 event_bus 发布为可观测事件。
 * 参数: metrics - HTTP 尝试指标数据, user_data - 内含 event_bus 和上下文信息
 */
static void http_attempt_metrics_observer(const cc_http_attempt_metrics_t *metrics, void *user_data)
{
    cc_http_llm_metrics_ctx_t *ctx = (cc_http_llm_metrics_ctx_t *)user_data;
    if (!metrics || !ctx || !ctx->event_bus) {
        return;
    }

    cc_json_value_t *attrs = cc_json_create_object();
    if (!attrs) {
        return;
    }
    cc_json_object_set(attrs, "stage", cc_json_create_string(metrics->stage ? metrics->stage : ""));
    cc_json_object_set(attrs, "err", cc_json_create_string(metrics->err ? metrics->err : ""));
    cc_json_object_set(attrs, "http_status", cc_json_create_number((double)metrics->status));
    cc_json_object_set(attrs, "attempt_id", cc_json_create_number((double)metrics->attempt_id));
    cc_json_object_set(attrs, "attempt_index", cc_json_create_number((double)metrics->attempt_index));
    cc_json_object_set(attrs, "final_attempt", cc_json_create_bool(metrics->final_attempt != 0));
    cc_json_object_set(attrs, "total_ms", cc_json_create_number((double)metrics->total_ms));
    cc_json_object_set(attrs, "connect_ms", cc_json_create_number((double)metrics->connect_ms));
    cc_json_object_set(attrs, "header_ms", cc_json_create_number((double)metrics->header_ms));
    cc_json_object_set(attrs, "first_body_ms", cc_json_create_number((double)metrics->first_body_ms));
    cc_json_object_set(attrs, "saw_body", cc_json_create_bool(metrics->saw_body != 0));
    /*
     * 连接复用诊断字段用于区分 TCP/TLS 建连慢和 provider 首包慢，
     * 长测控制台可据此统计复用率与连接重建次数。
     */
    cc_json_object_set(attrs, "reused_connection", cc_json_create_bool(metrics->reused_connection != 0));
    cc_json_object_set(attrs, "connection_generation", cc_json_create_number((double)metrics->connection_generation));
    cc_json_object_set(attrs, "origin", cc_json_create_string(metrics->origin ? metrics->origin : ""));
    cc_json_object_set(attrs, "connection_pool", cc_json_create_string(metrics->connection_pool ? metrics->connection_pool : ""));
    cc_json_object_set(attrs, "reuse_count", cc_json_create_number((double)metrics->reuse_count));
    cc_json_object_set(attrs, "idle_ms_before_reuse", cc_json_create_number((double)metrics->idle_ms_before_reuse));
    cc_json_object_set(attrs, "max_reuse_reached", cc_json_create_bool(metrics->max_reuse_reached != 0));
    cc_json_object_set(attrs, "connection_discard_reason", cc_json_create_string(metrics->connection_discard_reason ? metrics->connection_discard_reason : ""));
    cc_json_object_set(attrs, "retry_allowed", cc_json_create_bool(metrics->retry_allowed != 0));
    cc_json_object_set(attrs, "retry_performed", cc_json_create_bool(metrics->retry_performed != 0));
    cc_json_object_set(attrs, "partial_stream", cc_json_create_bool(metrics->partial_stream != 0 || (ctx->stream_partial && *ctx->stream_partial != 0)));
    cc_json_object_set(attrs, "saw_done", cc_json_create_bool(metrics->saw_done != 0 || (ctx->stream_saw_done && *ctx->stream_saw_done != 0)));
    char *attrs_json = cc_json_stringify_unformatted(attrs);
    cc_json_destroy(attrs);
    if (!attrs_json) {
        return;
    }

    const int ok_stage = metrics->stage && strcmp(metrics->stage, "ok") == 0;

    cc_observability_event_t event;
    memset(&event, 0, sizeof(event));
    event.size = sizeof(event);
    event.event = CC_OBS_EVENT_LLM_HTTP_ATTEMPT;
    event.session_id = ctx->session_id;
    event.step = ctx->step;
    event.status = ok_stage ? "ok" : "error";
    event.attributes_json = attrs_json;
    cc_result_t rc = cc_observability_publish(ctx->event_bus, &event);
    cc_result_free(&rc);
    if (ok_stage && metrics->saw_body && metrics->first_body_ms >= 0) {
        event.event = CC_OBS_EVENT_LLM_FIRST_BODY;
        event.status = "ok";
        rc = cc_observability_publish(ctx->event_bus, &event);
        cc_result_free(&rc);
    }
    free(attrs_json);
}

/* 将 HTTP 状态码映射成 SDK 稳定错误码，供上层 retry/backoff 策略判断。 */
static cc_error_code_t classify_http_status(long status_code)
{
    if (status_code == 429) return CC_ERR_RATE_LIMIT;
    if (status_code >= 500 && status_code <= 599) return CC_ERR_NETWORK;
    if (status_code >= 400 && status_code <= 499) return CC_ERR_MODEL;
    return CC_ERR_MODEL;
}

/* ASCII 大小写不敏感比较，用于 HTTP header 名称匹配。 */
static int ascii_case_equal(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (tolower(ca) != tolower(cb)) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/*
 * 查找响应头。
 *
 * 返回值是 response 内部借用指针，调用方不能释放；主要用于读取 Retry-After。
 */
static void build_header_profile(
    const char *method,
    const cc_http_header_t *headers,
    size_t header_count,
    char *out,
    size_t out_size
)
{
    uint32_t name_hash = 2166136261u;
    if (!out || out_size == 0) return;
    for (size_t i = 0; i < header_count; i++) {
        const char *name = headers[i].name ? headers[i].name : "";
        while (*name) {
            name_hash ^= (uint32_t)tolower((unsigned char)*name++);
            name_hash *= 16777619u;
        }
        name_hash ^= 0xffu;
        name_hash *= 16777619u;
    }
    snprintf(out, out_size, "method=%s;headers=%u;names=%08x",
             method ? method : "GET",
             (unsigned)header_count,
             (unsigned)name_hash);
}

static const char *response_header_value(
    const cc_http_response_t *response,
    const char *name
)
{
    if (!response || !name) return NULL;
    for (size_t i = 0; i < response->header_count; i++) {
        if (ascii_case_equal(response->headers[i].name, name)) {
            return response->headers[i].value;
        }
    }
    return NULL;
}

/*
 * 解析 Retry-After 秒数格式。
 *
 * 这里只支持数字秒，不解析 HTTP-date；返回毫秒，解析失败返回 0。上限限制为 24 小时，
 * 避免 provider 返回异常大值影响应用策略。
 */
static int parse_retry_after_ms(const char *value)
{
    if (!value || !*value) return 0;
    int seconds = 0;
    while (*value && isspace((unsigned char)*value)) value++;
    while (*value && isdigit((unsigned char)*value)) {
        seconds = seconds * 10 + (*value - '0');
        if (seconds > 86400) return 86400000;
        value++;
    }
    while (*value && isspace((unsigned char)*value)) value++;
    return *value == '\0' ? seconds * 1000 : 0;
}

/*
 * 把非 2xx HTTP 响应转换成结构化 cc_result_t。
 *
 * detail 中包含 http_status、retry_after_ms、recoverable 和脱敏后的 body。SDK 不自动重试，
 * 但把恢复语义暴露给下游应用决定 backoff。
 */
static cc_result_t http_response_status_error(
    const char *label,
    const cc_http_response_t *response
)
{
    cc_error_code_t code = classify_http_status(response ? response->status_code : 0);
    cc_error_detail_t detail;
    memset(&detail, 0, sizeof(detail));
    detail.size = sizeof(detail);
    detail.http_status = response ? response->status_code : 0;
    detail.retry_after_ms = parse_retry_after_ms(response_header_value(response, "Retry-After"));
    detail.recoverable = (detail.http_status == 429 || detail.http_status >= 500) ? 1 : 0;
    detail.error_code = (char *)cc_error_code_name(code);
    detail.raw_redacted_body = cc_redact_secrets(response && response->body ? response->body : "");
    const char *safe_body = detail.raw_redacted_body ? detail.raw_redacted_body : "empty response body";
    cc_result_t rc = cc_result_errf(code,
        "%s HTTP %ld: %s",
        label ? label : "LLM",
        detail.http_status,
        safe_body && *safe_body ? safe_body : "empty response body");
    cc_result_t with_detail = cc_result_with_detail(rc.code, rc.message, &detail);
    cc_result_free(&rc);
    free(detail.raw_redacted_body);
    return with_detail;
}

/*
 * 释放协议层构造出的 HTTP 请求。
 *
 * url/api_key/body_json/headers 都由 cc_llm_http_request_t 拥有；provider 在发送完成后调用
 * 本函数，避免协议实现和 HTTP 传输层的所有权混淆。
 */
void cc_llm_http_request_cleanup(cc_llm_http_request_t *request)
{
    if (!request) return;
    free(request->url);
    free(request->api_key);
    free(request->body_json);
    for (size_t i = 0; i < request->header_count; i++) {
        free((char *)request->headers[i].name);
        free((char *)request->headers[i].value);
    }
    free(request->headers);
    memset(request, 0, sizeof(*request));
}

/*
 * 发送同步 JSON POST。
 *
 * 函数只负责 HTTP 传输和状态码处理；响应 body 的协议解析由 protocol vtable 完成。
 * out_response 成功后由调用方 free。
 */
static cc_result_t http_post_json_with_headers(
    cc_http_client_t *http_client,
    const char *url,
    const cc_http_header_t *headers,
    size_t header_count,
    const char *body_json,
    const cc_llm_chat_request_t *llm_request,
    cc_cancel_token_t *cancel_token,
    int timeout_ms,
    char **out_response
)
{
    cc_http_llm_metrics_ctx_t metrics_ctx = {
        .event_bus = llm_request ? llm_request->event_bus : NULL,
        .session_id = llm_request ? llm_request->session_id : NULL,
        .step = llm_request ? llm_request->step : 0,
    };
    char header_profile[96];
    build_header_profile("POST", headers, header_count, header_profile, sizeof(header_profile));
    cc_http_request_t http_request;
    memset(&http_request, 0, sizeof(http_request));
    http_request.method = "POST";
    http_request.url = url;
    http_request.headers = headers;
    http_request.header_count = header_count;
    http_request.body = body_json;
    http_request.timeout_ms = timeout_ms > 0 ? timeout_ms : 30000;
    http_request.deadline_ms = llm_request ? llm_request->deadline_ms : 0;
    http_request.max_response_bytes = llm_request && llm_request->max_response_bytes > 0 ?
        llm_request->max_response_bytes : 32U * 1024U;
    http_request.cancel_token = cancel_token;
    http_request.connection_pool = "llm_sync";
    http_request.header_profile = header_profile;
    if (metrics_ctx.event_bus) {
        http_request.on_attempt_metrics = http_attempt_metrics_observer;
        http_request.attempt_metrics_user_data = &metrics_ctx;
    }

    cc_http_response_t response;
    memset(&response, 0, sizeof(response));
    cc_result_t rc = cc_http_client_perform(http_client, &http_request, &response);
    if (rc.code != CC_OK) return rc;

    if (response.status_code < 200 || response.status_code >= 300) {
        cc_result_t err = http_response_status_error("LLM", &response);
        cc_http_response_free(&response);
        return err;
    }

    *out_response = response.body ? response.body : cc_copy_string("");
    response.body = NULL;
    cc_http_response_free(&response);
    return cc_result_ok();
}

/*
 * HTTP 流式解析上下文。
 *
 * buffer 保存还未组成完整 SSE/NDJSON 事件的尾部数据；on_chunk 是 runtime 传入的回调，
 * 不由 provider 拥有。finished 防止重复发送 FINISHED chunk。
 */
typedef struct cc_http_llm_stream_ctx {
    cc_llm_protocol_t protocol;
    cc_llm_stream_kind_t stream_kind;
    cc_llm_stream_callback_fn on_chunk;
    void *user_data;
    char *buffer;
    size_t buf_len;
    size_t buf_cap;
    size_t total_bytes;
    int finished;
    int saw_event;
    int saw_text;
    int saw_tool;
    int saw_done;
    int partial_stream;
} cc_http_llm_stream_ctx_t;

/*
 * 向流式缓冲追加 HTTP body 数据。
 *
 * HTTP client 可能按任意大小回调 body，provider 必须自己缓存半个 SSE/NDJSON 事件。
 */
static cc_result_t stream_ctx_append(
    cc_http_llm_stream_ctx_t *ctx,
    const char *data,
    size_t len
)
{
    if (len == 0) return cc_result_ok();
    if (len > SIZE_MAX - ctx->buf_len - 1) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Stream frame size overflow");
    }
    size_t required = ctx->buf_len + len + 1;
    if (required > CC_HTTP_LLM_STREAM_MAX_PENDING_FRAME_BYTES) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Stream frame exceeded pending buffer limit");
    }
    if (required > ctx->buf_cap) {
        size_t new_cap = ctx->buf_cap ? ctx->buf_cap * 2 : 4096;
        while (new_cap < required) {
            if (new_cap > CC_HTTP_LLM_STREAM_MAX_PENDING_FRAME_BYTES / 2) {
                new_cap = CC_HTTP_LLM_STREAM_MAX_PENDING_FRAME_BYTES;
                break;
            }
            new_cap *= 2;
        }
        char *new_buf = realloc(ctx->buffer, new_cap);
        if (!new_buf) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Stream buffer realloc failed");
        ctx->buffer = new_buf;
        ctx->buf_cap = new_cap;
    }
    memcpy(ctx->buffer + ctx->buf_len, data, len);
    ctx->buf_len += len;
    ctx->buffer[ctx->buf_len] = '\0';
    return cc_result_ok();
}

/* 只发送一次 FINISHED chunk，避免 provider [DONE] 和 HTTP 结束重复通知 runtime。 */
static cc_result_t stream_forward_chunk(const cc_stream_chunk_t *chunk, void *user_data)
{
    cc_http_llm_stream_ctx_t *ctx = (cc_http_llm_stream_ctx_t *)user_data;
    if (!ctx || !chunk || !ctx->on_chunk) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid stream callback state");
    }
    if (chunk->type == CC_STREAM_CHUNK_TEXT || chunk->type == CC_STREAM_CHUNK_THINKING) {
        ctx->saw_text = 1;
    } else if (chunk->type == CC_STREAM_CHUNK_TOOL_START ||
               chunk->type == CC_STREAM_CHUNK_TOOL_DELTA ||
               chunk->type == CC_STREAM_CHUNK_TOOL_END) {
        ctx->saw_tool = 1;
    } else if (chunk->type == CC_STREAM_CHUNK_FINISHED) {
        ctx->saw_done = 1;
        ctx->finished = 1;
    }
    return ctx->on_chunk(chunk, ctx->user_data);
}

static cc_result_t emit_finished_once(cc_http_llm_stream_ctx_t *ctx)
{
    if (ctx->finished) return cc_result_ok();
    ctx->finished = 1;
    ctx->saw_done = 1;
    cc_stream_chunk_t chunk = {
        .type = CC_STREAM_CHUNK_FINISHED,
        .tool_index = -1,
    };
    return ctx->on_chunk(&chunk, ctx->user_data);
}

/*
 * 分发单个协议事件。
 *
 * SSE/NDJSON framing 层只拆出 event_json；真正把 provider JSON 翻译成 text/tool/error
 * chunk 的工作交给 protocol->parse_stream_event。
 */
static cc_result_t dispatch_stream_event(
    cc_http_llm_stream_ctx_t *ctx,
    const char *event_json
)
{
    if (!event_json || !*event_json) return cc_result_ok();
    ctx->saw_event = 1;
    if (strcmp(event_json, "[DONE]") == 0) {
        return emit_finished_once(ctx);
    }
    if (!ctx->protocol.vtable->parse_stream_event) {
        return cc_result_error(CC_ERR_PLATFORM, "LLM protocol does not parse stream events");
    }

    int finished = 0;
    cc_result_t rc = ctx->protocol.vtable->parse_stream_event(
        ctx->protocol.self,
        event_json,
        stream_forward_chunk,
        ctx,
        &finished);
    if (rc.code != CC_OK) return rc;
    if (finished) return emit_finished_once(ctx);
    return cc_result_ok();
}

/*
 * 处理一个完整 SSE event。
 *
 * SSE event 可能包含多行 data:，这里把多行 data 用换行拼成协议 JSON，再交给分发函数。
 * 其它字段如 event/id 当前忽略。
 */
static cc_result_t process_sse_event(
    cc_http_llm_stream_ctx_t *ctx,
    char *event_text
)
{
    cc_string_builder_t data_json;
    cc_result_t rc = cc_string_builder_init(&data_json);
    if (rc.code != CC_OK) return rc;

    char *line = event_text;
    while (*line) {
        char *line_end = strchr(line, '\n');
        if (line_end) *line_end = '\0';
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\r') line[len - 1] = '\0';
        if (strncmp(line, "data:", 5) == 0) {
            const char *data = line + 5;
            if (*data == ' ') data++;
            if (data_json.length > 0) cc_string_builder_append(&data_json, "\n");
            rc = cc_string_builder_append(&data_json, data);
            if (rc.code != CC_OK) {
                cc_string_builder_deinit(&data_json);
                return rc;
            }
        }
        if (!line_end) break;
        line = line_end + 1;
    }

    rc = dispatch_stream_event(ctx, cc_string_builder_cstr(&data_json));
    cc_string_builder_deinit(&data_json);
    return rc;
}

/*
 * 从缓冲中尽可能拆出完整 SSE event。
 *
 * 支持 \n\n 和 \r\n\r\n 两种分隔符；未完成的尾部数据会 memmove 到缓冲开头，等待下一次
 * HTTP body 回调补齐。
 */
static cc_result_t process_sse_buffer(cc_http_llm_stream_ctx_t *ctx)
{
    char *p = ctx->buffer;
    char *buf_end = ctx->buffer + ctx->buf_len;

    while (!ctx->finished && p < buf_end) {
        char *end = strstr(p, "\n\n");
        size_t sep_len = 2;
        if (!end) {
            end = strstr(p, "\r\n\r\n");
            sep_len = 4;
        }
        if (!end) break;

        char saved = *end;
        *end = '\0';
        cc_result_t rc = process_sse_event(ctx, p);
        *end = saved;
        if (rc.code != CC_OK) return rc;
        p = end + sep_len;
    }

    if (p > ctx->buffer) {
        size_t remaining = ctx->buf_len - (size_t)(p - ctx->buffer);
        if (remaining > 0) memmove(ctx->buffer, p, remaining);
        ctx->buf_len = remaining;
        ctx->buffer[ctx->buf_len] = '\0';
    }
    return cc_result_ok();
}

/*
 * 从缓冲中拆出 NDJSON 行。
 *
 * 每一行都是一个 provider stream event；未读完的最后半行留在缓冲中，HTTP 结束时由
 * flush_stream_tail 处理。
 */
static cc_result_t process_ndjson_buffer(cc_http_llm_stream_ctx_t *ctx)
{
    char *p = ctx->buffer;
    char *buf_end = ctx->buffer + ctx->buf_len;

    while (!ctx->finished && p < buf_end) {
        char *end = strchr(p, '\n');
        if (!end) break;
        *end = '\0';
        size_t len = strlen(p);
        if (len > 0 && p[len - 1] == '\r') p[len - 1] = '\0';
        cc_result_t rc = dispatch_stream_event(ctx, p);
        *end = '\n';
        if (rc.code != CC_OK) return rc;
        p = end + 1;
    }

    if (p > ctx->buffer) {
        size_t remaining = ctx->buf_len - (size_t)(p - ctx->buffer);
        if (remaining > 0) memmove(ctx->buffer, p, remaining);
        ctx->buf_len = remaining;
        ctx->buffer[ctx->buf_len] = '\0';
    }
    return cc_result_ok();
}

/*
 * HTTP client body 回调。
 *
 * 该函数运行在 HTTP 传输层回调上下文中，只做追加缓冲和 framing 解析，最终通过
 * runtime 提供的 on_chunk 把实时 chunk 交回上层。
 */
static cc_result_t stream_body_callback(const char *data, size_t len, void *user_data)
{
    cc_http_llm_stream_ctx_t *ctx = (cc_http_llm_stream_ctx_t *)user_data;
    if (ctx->finished) return cc_result_ok();
    if (len > SIZE_MAX - ctx->total_bytes) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Stream byte count overflow");
    }
    ctx->total_bytes += len;

    cc_result_t rc = stream_ctx_append(ctx, data, len);
    if (rc.code != CC_OK) return rc;

    if (ctx->stream_kind == CC_LLM_STREAM_SSE) {
        return process_sse_buffer(ctx);
    }
    if (ctx->stream_kind == CC_LLM_STREAM_NDJSON) {
        return process_ndjson_buffer(ctx);
    }
    return cc_result_error(CC_ERR_PLATFORM, "Unsupported LLM stream framing");
}

/*
 * HTTP 结束后处理流式尾部。
 *
 * NDJSON 允许最后一行没有换行符，因此需要在响应完成后尝试分发剩余 buffer；SSE 则只在
 * 完整 event 分隔符后处理。
 */
static cc_result_t flush_stream_tail(cc_http_llm_stream_ctx_t *ctx)
{
    if (ctx->finished || ctx->buf_len == 0) return cc_result_ok();
    if (ctx->stream_kind == CC_LLM_STREAM_NDJSON) {
        cc_result_t rc = dispatch_stream_event(ctx, ctx->buffer);
        ctx->buf_len = 0;
        if (ctx->buffer) ctx->buffer[0] = '\0';
        return rc;
    }
    return cc_result_ok();
}

/*
 * 发送流式 JSON POST。
 *
 * HTTP body 不整体缓存在内存，而是通过 on_body 增量解析 SSE/NDJSON。非 2xx 响应仍映射
 * 成结构化错误；成功结束时保证发送 FINISHED chunk。
 */
static cc_result_t http_post_json_stream_with_headers(
    cc_http_client_t *http_client,
    const cc_llm_http_request_t *http_req,
    const cc_llm_chat_request_t *llm_request,
    cc_llm_protocol_t protocol,
    cc_cancel_token_t *cancel_token,
    int timeout_ms,
    cc_llm_stream_callback_fn on_chunk,
    void *user_data
)
{
    cc_http_llm_metrics_ctx_t metrics_ctx = {
        .event_bus = llm_request ? llm_request->event_bus : NULL,
        .session_id = llm_request ? llm_request->session_id : NULL,
        .step = llm_request ? llm_request->step : 0,
    };
    char header_profile[96];
    build_header_profile("POST", http_req->headers, http_req->header_count, header_profile, sizeof(header_profile));
    cc_http_llm_stream_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.protocol = protocol;
    ctx.stream_kind = http_req->stream_kind;
    ctx.on_chunk = on_chunk;
    ctx.user_data = user_data;
    metrics_ctx.stream_saw_done = &ctx.saw_done;
    metrics_ctx.stream_partial = &ctx.partial_stream;

    cc_http_request_t http_request;
    memset(&http_request, 0, sizeof(http_request));
    http_request.method = "POST";
    http_request.url = http_req->url;
    http_request.headers = http_req->headers;
    http_request.header_count = http_req->header_count;
    http_request.body = http_req->body_json;
    http_request.timeout_ms = timeout_ms > 0 ? timeout_ms : 300000;
    http_request.deadline_ms = llm_request ? llm_request->deadline_ms : 0;
    http_request.max_response_bytes = llm_request && llm_request->max_response_bytes > 0 ?
        llm_request->max_response_bytes : 32U * 1024U;
    http_request.on_body = stream_body_callback;
    http_request.user_data = &ctx;
    http_request.cancel_token = cancel_token;
    http_request.connection_pool = "llm_stream";
    http_request.header_profile = header_profile;
    if (metrics_ctx.event_bus) {
        http_request.on_attempt_metrics = http_attempt_metrics_observer;
        http_request.attempt_metrics_user_data = &metrics_ctx;
    }

    cc_http_response_t response;
    memset(&response, 0, sizeof(response));
    cc_result_t rc = cc_http_client_perform(http_client, &http_request, &response);
    if (rc.code != CC_OK && (ctx.saw_text || ctx.saw_tool)) {
        ctx.partial_stream = 1;
    }
    if (rc.code == CC_OK) {
        rc = flush_stream_tail(&ctx);
    }

    if (rc.code == CC_OK && (response.status_code < 200 || response.status_code >= 300)) {
        rc = http_response_status_error("LLM stream", &response);
    }

    if (rc.code == CC_OK && !ctx.saw_done) {
        if (ctx.saw_event || ctx.saw_text || ctx.saw_tool) {
            ctx.partial_stream = 1;
        }
        rc = cc_result_error(CC_ERR_NETWORK, "stream_ended_without_done");
    }

    cc_http_response_free(&response);
    free(ctx.buffer);
    return rc;
}

/*
 * 同步 chat vtable 实现。
 *
 * 协议层先构造 HTTP 请求，通用 provider 发送 POST，最后协议层解析响应 JSON 到
 * cc_llm_response_t。JSON 解析失败统一映射为稳定提示，避免泄漏 provider 原始细节。
 */
static cc_result_t http_llm_chat(
    void *self,
    const cc_llm_chat_request_t *request,
    cc_llm_response_t *out_response
)
{
    cc_http_llm_provider_t *provider = (cc_http_llm_provider_t *)self;
    memset(out_response, 0, sizeof(*out_response));

    cc_llm_http_request_t http_req;
    memset(&http_req, 0, sizeof(http_req));
    cc_result_t rc = provider->protocol.vtable->build_request(
        provider->protocol.self,
        provider->base_url,
        provider->api_key,
        provider->model,
        request,
        0,
        &http_req
    );
    if (rc.code != CC_OK) return rc;

    char *response_json = NULL;
    rc = http_post_json_with_headers(
        provider->http_client,
        http_req.url,
        http_req.headers,
        http_req.header_count,
        http_req.body_json,
        request,
        request->cancel_token,
        request->timeout_ms,
        &response_json
    );
    cc_llm_http_request_cleanup(&http_req);
    if (rc.code != CC_OK) return rc;

    rc = provider->protocol.vtable->parse_response(
        provider->protocol.self,
        response_json,
        out_response
    );
    if (rc.code == CC_ERR_JSON) {
        cc_result_free(&rc);
        rc = cc_result_error(CC_ERR_JSON, "Provider returned invalid response JSON");
    }
    free(response_json);
    return rc;
}

/*
 * 流式 chat vtable 实现。
 *
 * 只有协议声明 stream_kind 且实现 parse_stream_event 才允许流式；否则 fail-fast，避免
 * runtime 以为有实时输出但 provider 静默退回同步模式。
 */
static cc_result_t http_llm_chat_stream(
    void *self,
    const cc_llm_chat_request_t *request,
    cc_llm_stream_callback_fn on_chunk,
    void *user_data
)
{
    cc_http_llm_provider_t *provider = (cc_http_llm_provider_t *)self;
    cc_llm_http_request_t http_req;
    memset(&http_req, 0, sizeof(http_req));
    cc_result_t rc = provider->protocol.vtable->build_request(
        provider->protocol.self,
        provider->base_url,
        provider->api_key,
        provider->model,
        request,
        1,
        &http_req
    );
    if (rc.code != CC_OK) return rc;

    if (http_req.stream_kind == CC_LLM_STREAM_NONE ||
        !provider->protocol.vtable->parse_stream_event) {
        cc_llm_http_request_cleanup(&http_req);
        return cc_result_error(CC_ERR_PLATFORM, "LLM protocol does not support streaming");
    }

    rc = http_post_json_stream_with_headers(
        provider->http_client,
        &http_req,
        request,
        provider->protocol,
        request->cancel_token,
        request->timeout_ms,
        on_chunk,
        user_data);
    cc_llm_http_request_cleanup(&http_req);
    return rc;
}

/* 销毁 HTTP LLM provider，并级联销毁协议私有对象。 */
static void http_llm_destroy(void *self)
{
    cc_http_llm_provider_t *provider = (cc_http_llm_provider_t *)self;
    if (!provider) return;
    free(provider->base_url);
    free(provider->api_key);
    free(provider->model);
    if (provider->protocol.vtable && provider->protocol.vtable->destroy) {
        provider->protocol.vtable->destroy(provider->protocol.self);
    }
    free(provider);
}

/*
 * 返回 provider 能力矩阵。
 *
 * 通用 HTTP provider 默认支持文本、工具、reasoning 和 streaming；多模态能力按协议名称
 * 做保守标记。runtime create 阶段会用该矩阵校验配置，避免静默降级。
 */
static cc_result_t http_llm_capabilities(
    void *self,
    cc_llm_provider_capabilities_t *out_capabilities
)
{
    if (!out_capabilities) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null provider capabilities");
    }
    memset(out_capabilities, 0, sizeof(*out_capabilities));
    out_capabilities->text_input = 1;
    out_capabilities->text_output = 1;
    out_capabilities->tool_calling = 1;
    out_capabilities->reasoning = 1;
    out_capabilities->streaming = 1;
    out_capabilities->max_artifacts = 8;
    out_capabilities->max_artifact_bytes = 10u * 1024u * 1024u;

    cc_http_llm_provider_t *provider = (cc_http_llm_provider_t *)self;
    const char *name = provider && provider->protocol.vtable && provider->protocol.vtable->name ?
        provider->protocol.vtable->name(provider->protocol.self) : "";
    if (strcmp(name, "openai") == 0) {
        out_capabilities->image_input = 1;
        out_capabilities->audio_input = 1;
        out_capabilities->image_output = 1;
        out_capabilities->audio_output = 1;
        out_capabilities->file_input = 1;
        out_capabilities->file_output = 1;
    } else if (strcmp(name, "anthropic") == 0) {
        out_capabilities->image_input = 1;
        out_capabilities->file_input = 1;
    } else if (strcmp(name, "ollama") == 0) {
        out_capabilities->image_input = 1;
    }
    return cc_result_ok();
}

/* HTTP LLM provider vtable，绑定同步 chat、流式 chat、destroy 和 capability 查询。 */
static cc_llm_provider_vtable_t http_llm_vtable = {
    http_llm_chat,
    http_llm_chat_stream,
    http_llm_destroy,
    http_llm_capabilities
};

/*
 * 创建通用 HTTP LLM provider。
 *
 * 成功后 out_provider 持有 self/vtable；base_url/api_key/model 被深拷贝，protocol self 的
 * 所有权转移给 provider，destroy 时会调用 protocol.vtable->destroy。
 */
cc_result_t cc_http_llm_provider_create(
    cc_http_client_t *http_client,
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_protocol_t protocol,
    cc_llm_provider_t *out_provider
)
{
    if (!http_client || !http_client->vtable ||
        !protocol.vtable || !protocol.vtable->build_request ||
        !protocol.vtable->parse_response || !out_provider) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid HTTP LLM provider argument");
    }

    cc_http_llm_provider_t *provider = calloc(1, sizeof(*provider));
    if (!provider) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create HTTP LLM provider");
    }

    provider->http_client = http_client;
    provider->base_url = base_url ? cc_copy_string(base_url) : cc_copy_string("");
    provider->api_key = api_key ? cc_copy_string(api_key) : NULL;
    provider->model = model ? cc_copy_string(model) : cc_copy_string("");
    provider->protocol = protocol;

    if (!provider->base_url || !provider->model) {
        http_llm_destroy(provider);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy HTTP LLM provider config");
    }

    out_provider->self = provider;
    out_provider->vtable = &http_llm_vtable;
    return cc_result_ok();
}
