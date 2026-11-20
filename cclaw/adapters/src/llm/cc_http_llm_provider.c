



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

/* 保留的预览兼容宏，当前实现没有读取点，实际响应预算沿 HTTP 请求和端口处理。 */
#ifndef CC_HTTP_LLM_STREAM_ERROR_PREVIEW_BYTES
#define CC_HTTP_LLM_STREAM_ERROR_PREVIEW_BYTES 4096
#endif

/** @brief 未成帧缓冲的字节上限，含结尾 NUL；单次 HTTP 大块会分段追加并穿插解析。 */
#ifndef CC_HTTP_LLM_STREAM_MAX_PENDING_FRAME_BYTES
#define CC_HTTP_LLM_STREAM_MAX_PENDING_FRAME_BYTES (64 * 1024)
#endif

/** @brief 拥有配置副本与已交接协议，借用外层 HTTP 端口的 provider。 */
typedef struct cc_http_llm_provider {
    cc_http_client_t *http_client; /**< 覆盖全部调用期的端口借用。 */
    char *base_url; /**< 拥有的地址副本。 */
    char *api_key; /**< 可选拥有凭据副本。 */
    char *model; /**< 拥有的默认模型副本。 */
    cc_llm_protocol_t protocol; /**< 成功创建后交接的协议记录，析构时调用可选 destroy。 */
} cc_http_llm_provider_t;

/** @brief 只在同步 HTTP 调用栈中存活的指标上下文。 */
typedef struct cc_http_llm_metrics_ctx {
    cc_event_bus_t *event_bus; /**< 可选事件总线借用。 */
    const char *session_id; /**< 本次请求会话标识借用。 */
    const char *run_id; /**< 本次请求 run 标识借用。 */
    int step; /**< 当前模型步骤。 */
    const int *stream_saw_done; /**< 可选栈上流完成标志借用。 */
    const int *stream_partial; /**< 可选栈上部分流标志借用。 */
} cc_http_llm_metrics_ctx_t;

/**
 * @brief 把一次 HTTP 尝试指标复制为事件属性，并按条件追加首包事件。
 * @param[in] metrics 回调期间借用指标，NULL 时跳过。
 * @param[in,out] user_data 同步 HTTP 调用栈中的指标上下文借用，NULL 或缺总线时跳过。
 * @post 临时 JSON、序列化文本与发布结果均清理；分配失败跳过本次观测，业务请求继续执行。
 * @note 所有回调须在 perform 返回前退出，事件中引用的字段在 publish 期间稳定。
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
    if (!cc_json_object_set_owned(attrs, "stage", cc_json_create_string(metrics->stage ? metrics->stage : "")) ||
        !cc_json_object_set_owned(attrs, "err", cc_json_create_string(metrics->err ? metrics->err : "")) ||
        !cc_json_object_set_owned(attrs, "http_status", cc_json_create_number((double)metrics->status)) ||
        !cc_json_object_set_owned(attrs, "attempt_id", cc_json_create_number((double)metrics->attempt_id)) ||
        !cc_json_object_set_owned(attrs, "attempt_index", cc_json_create_number((double)metrics->attempt_index)) ||
        !cc_json_object_set_owned(attrs, "final_attempt", cc_json_create_bool(metrics->final_attempt != 0)) ||
        !cc_json_object_set_owned(attrs, "total_ms", cc_json_create_number((double)metrics->total_ms)) ||
        !cc_json_object_set_owned(attrs, "connect_ms", cc_json_create_number((double)metrics->connect_ms)) ||
        !cc_json_object_set_owned(attrs, "header_ms", cc_json_create_number((double)metrics->header_ms)) ||
        !cc_json_object_set_owned(attrs, "first_body_ms", cc_json_create_number((double)metrics->first_body_ms)) ||
        !cc_json_object_set_owned(attrs, "saw_body", cc_json_create_bool(metrics->saw_body != 0)) ||
        !cc_json_object_set_owned(attrs, "reused_connection", cc_json_create_bool(metrics->reused_connection != 0)) ||
        !cc_json_object_set_owned(attrs, "connection_generation", cc_json_create_number((double)metrics->connection_generation)) ||
        !cc_json_object_set_owned(attrs, "origin", cc_json_create_string(metrics->origin ? metrics->origin : "")) ||
        !cc_json_object_set_owned(attrs, "connection_pool", cc_json_create_string(metrics->connection_pool ? metrics->connection_pool : "")) ||
        !cc_json_object_set_owned(attrs, "reuse_count", cc_json_create_number((double)metrics->reuse_count)) ||
        !cc_json_object_set_owned(attrs, "idle_ms_before_reuse", cc_json_create_number((double)metrics->idle_ms_before_reuse)) ||
        !cc_json_object_set_owned(attrs, "max_reuse_reached", cc_json_create_bool(metrics->max_reuse_reached != 0)) ||
        !cc_json_object_set_owned(attrs, "connection_discard_reason", cc_json_create_string(metrics->connection_discard_reason ? metrics->connection_discard_reason : "")) ||
        !cc_json_object_set_owned(attrs, "retry_allowed", cc_json_create_bool(metrics->retry_allowed != 0)) ||
        !cc_json_object_set_owned(attrs, "retry_performed", cc_json_create_bool(metrics->retry_performed != 0)) ||
        !cc_json_object_set_owned(attrs, "partial_stream", cc_json_create_bool(metrics->partial_stream != 0 || (ctx->stream_partial && *ctx->stream_partial != 0))) ||
        !cc_json_object_set_owned(attrs, "saw_done", cc_json_create_bool(metrics->saw_done != 0 || (ctx->stream_saw_done && *ctx->stream_saw_done != 0)))) {
        cc_json_destroy(attrs);
        return;
    }
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
    event.run_id = ctx->run_id;
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

/**
 * @brief 将失败 HTTP 状态映射成稳定业务错误类别。
 * @param[in] status_code 响应状态，429 映射限流，500 至 599 映射网络错误。
 * @return 其他状态均为 MODEL，此函数不重试请求。
 */
static cc_error_code_t classify_http_status(long status_code)
{
    if (status_code == 429) return CC_ERR_RATE_LIMIT;
    if (status_code >= 500 && status_code <= 599) return CC_ERR_NETWORK;
    if (status_code >= 400 && status_code <= 499) return CC_ERR_MODEL;
    return CC_ERR_MODEL;
}

/**
 * @brief 按字符大小写折叠比较 HTTP 的 ASCII 头名称。
 * @param[in] a 可为 NULL 的借用字符串。
 * @param[in] b 可为 NULL 的借用字符串。
 * @return 两个非 NULL 字符串折叠后完全相等为一，其他为零。
 */
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

/**
 * @brief 用方法、头数量和头名称散列生成连接诊断标识。
 * @param[in] method 借用方法名，NULL 使用 GET。
 * @param[in] headers 含 header_count 项的借用数组，零项时允许 NULL。
 * @param[in] header_count 数组项数，散列只读取 name。
 * @param[out] out 可写输出，NULL 时跳过。
 * @param[in] out_size 输出字节容量，含 NUL，零时跳过。
 * @post 有效非零输出得到 NUL 结尾标识，超长按字节截断；头值未进入该标识。
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

/**
 * @brief 忽略头名称大小写查找首个匹配的响应字段。
 * @param[in] response 借用响应，可为 NULL；已有头数量须对应有效数组。
 * @param[in] name 借用目标名称，可为 NULL。
 * @return 借用头值或 NULL，寿命止于响应释放。
 */
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

/**
 * @brief 读取两端可带空白的非负十进制秒数，并转换为有上限的毫秒值。
 * @param[in] value 借用头值，可为 NULL。
 * @return 有效数值返回毫秒，饱和到 86400000；缺失、日期或带其他字符返回零。
 */
static int parse_retry_after_ms(const char *value)
{
    if (!value || !*value) return 0;
    int seconds = 0;
    while (*value && isspace((unsigned char)*value)) value++;
    while (*value && isdigit((unsigned char)*value)) {
        if (seconds <= 86400) seconds = seconds * 10 + (*value - '0');
        value++;
    }
    while (*value && isspace((unsigned char)*value)) value++;
    return *value == '\0' ? (seconds > 86400 ? 86400000 : seconds * 1000) : 0;
}

/**
 * @brief 生成含状态、重试等待和脱敏正文的拥有错误结果。
 * @param[in] label 借用错误类别标签，NULL 使用 LLM。
 * @param[in] response 借用响应，可为 NULL，缺失状态按零分类。
 * @return 拥有结果，调用者 cc_result_free；429/5xx 标记可恢复，临时脱敏副本在返回前释放。
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
    detail.recoverable = (code == CC_ERR_RATE_LIMIT || code == CC_ERR_NETWORK) ? 1 : 0;
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

/**
 * @brief 释放完整或部分协议请求的拥有字段，然后清零。
 * @param[in,out] request 已清零或初始化的请求，可为 NULL。
 * @post URL、凭据、正文和各头字符串及数组已释放，原字段借用结束，结构体由调用者继续保管。
 */
void cc_llm_http_request_cleanup(cc_llm_http_request_t *request)
{
    if (!request) return;
    free(request->url);
    free(request->api_key);
    free(request->body_json);
    for (size_t i = 0; request->headers && i < request->header_count; i++) {
        free((char *)request->headers[i].name);
        free((char *)request->headers[i].value);
    }
    free(request->headers);
    memset(request, 0, sizeof(*request));
}

/**
 * @brief 将显式长度的拥有 HTTP 正文补成安全 C 字符串，兼容旧零长度文本端口。
 * @param[in,out] response 有效拥有响应，body_size 非零时 body 至少可读对应字节且可 realloc/free。
 * @param[in] json_body 非零要求成功 JSON 正文无原始 NUL，零时把错误正文内 NUL 转为空格供完整脱敏。
 * @return CC_OK、长度溢出的 LIMIT_EXCEEDED、非法正文 JSON 或扩容 OOM，错误时仍由调用者释放响应。
 * @note body_size 为零且 body 非空时沿旧接口按已有 NUL 结尾文本处理；成功扩容保持原始长度字段。
 */
static cc_result_t ensure_http_response_text(cc_http_response_t *response, int json_body)
{
    if (!response->body_size) return cc_result_ok();
    if (response->body_size == SIZE_MAX)
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "HTTP response text size overflow");
    if (!response->body)
        return cc_result_error(CC_ERR_JSON, "HTTP response has a size without a body");
    if (json_body && memchr(response->body, '\0', response->body_size))
        return cc_result_error(CC_ERR_JSON, "HTTP JSON response contains a raw NUL");
    char *text = realloc(response->body, response->body_size + 1);
    if (!text) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to terminate HTTP response text");
    response->body = text;
    if (!json_body) {
        for (size_t i = 0; i < response->body_size; i++) if (!text[i]) text[i] = ' ';
    }
    text[response->body_size] = '\0';
    return cc_result_ok();
}

/**
 * @brief 同步发送 JSON POST，映射失败状态并转移成功响应正文。
 * @param[in,out] http_client 有效 HTTP 端口借用，perform 和全部回调结束后才能释放。
 * @param[in] url 有效借用目标 URL。
 * @param[in] headers 含 header_count 项的借用头数组，零项允许 NULL。
 * @param[in] header_count 请求头数量。
 * @param[in] body_json 借用请求体，保持到 perform 返回。
 * @param[in] llm_request 可选统一请求借用，提供观测、绝对期限和响应字节预算。
 * @param[in] cancel_token 可选取消令牌借用，覆盖同步请求与回调。
 * @param[in] timeout_ms 正值为毫秒超时，非正使用 30000。
 * @param[out] out_response 有效空槽，成功接收须 free 的正文；无正文时复制空串。
 * @return 传输、HTTP 状态或空正文分配错误；所有响应资源在错误返回前释放。
 * @note perform 失败优先原样返回；仅成功 perform 的响应进入正文补尾和非 2xx 状态分类。
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
        .run_id = cc_llm_chat_request_run_id(llm_request),
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
    if (rc.code != CC_OK) {
        cc_http_response_free(&response);
        return rc;
    }
    cc_result_free(&rc);
    rc = ensure_http_response_text(&response, response.status_code >= 200 && response.status_code < 300);
    if (rc.code != CC_OK) {
        cc_http_response_free(&response);
        return rc;
    }
    cc_result_free(&rc);

    if (response.status_code < 200 || response.status_code >= 300) {
        cc_result_t err = http_response_status_error("LLM", &response);
        cc_http_response_free(&response);
        return err;
    }

    *out_response = response.body ? response.body : cc_copy_string("");
    response.body = NULL;
    cc_http_response_free(&response);
    if (!*out_response)
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate empty HTTP response");
    return cc_result_ok();
}

/** @brief 每次同步流请求独占的栈上状态，拥有未成帧缓冲，借用协议与上层回调。 */
typedef struct cc_http_llm_stream_ctx {
    cc_llm_protocol_t protocol; /**< 借用 provider 保管的协议。 */
    cc_llm_stream_kind_t stream_kind; /**< 本次流分帧方式。 */
    cc_llm_stream_callback_fn on_chunk; /**< 同步上层回调借用。 */
    void *user_data; /**< 上层回调上下文借用。 */
    char *buffer; /**< 拥有的未成帧字节缓冲，保证 NUL 结尾。 */
    size_t buf_len; /**< 有效尾部字节数，不含 NUL。 */
    size_t buf_cap; /**< 分配字节容量，含 NUL。 */
    size_t total_bytes; /**< 本次已接收原始字节计数，检查 size_t 溢出。 */
    int finished; /**< 已交付最终 FINISHED，后续 body 调用跳过。 */
    int defer_finished_until_end; /**< 语义完成后保留 usage 接收期。 */
    int protocol_finished; /**< 已语义完成，后续仅接纳 usage 或终止标记。 */
    int saw_event; /**< 已处理非空协议事件。 */
    int saw_text; /**< 已观察文本或思考块。 */
    int saw_tool; /**< 已观察工具块。 */
    int saw_done; /**< 已观察语义完成或传输完成。 */
    int partial_stream; /**< 错误发生前已交付部分有效流。 */
} cc_http_llm_stream_ctx_t;

/**
 * @brief 在未成帧预算内追加字节，并按需扩容拥有缓冲。
 * @param[in,out] ctx 有效独占流状态。
 * @param[in] data 至少含 len 字节的借用片段，零长度时允许 NULL。
 * @param[in] len 追加字节数。
 * @return 完成返回 CC_OK；尺寸溢出、帧预算或 realloc 失败返回错误，分配失败保留旧缓冲。
 * @note 成功补结尾 NUL，调用者负责输入中原始 NUL 的校验及后续分帧。
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
        size_t new_cap = ctx->buf_cap ? ctx->buf_cap : 4096;
        if (new_cap > CC_HTTP_LLM_STREAM_MAX_PENDING_FRAME_BYTES)
            new_cap = CC_HTTP_LLM_STREAM_MAX_PENDING_FRAME_BYTES;
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

/**
 * @brief 记录流块状态，延迟语义完成并同步转发上层回调。
 * @param[in] chunk 回调期间借用的流块，不能为空。
 * @param[in,out] user_data 有效流上下文借用。
 * @return 回调结果或非法完成顺序错误；重复 FINISHED 成功跳过，延迟模式先登记完成而保留 usage 阶段。
 * @note chunk 及字段的借用仅覆盖上层回调，错误返回前可能已有部分块交付。
 */
static cc_result_t stream_forward_chunk(const cc_stream_chunk_t *chunk, void *user_data)
{
    cc_http_llm_stream_ctx_t *ctx = (cc_http_llm_stream_ctx_t *)user_data;
    if (!ctx || !chunk || !ctx->on_chunk) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid stream callback state");
    }
    if (chunk->type == CC_STREAM_CHUNK_FINISHED && (ctx->finished || ctx->protocol_finished))
        return cc_result_ok();
    if (ctx->finished)
        return cc_result_error(CC_ERR_JSON, "Stream content followed the transport terminator");
    if (ctx->protocol_finished && chunk->type!=CC_STREAM_CHUNK_USAGE)
        return cc_result_error(CC_ERR_JSON,"Stream content followed the final response");
    if (chunk->type == CC_STREAM_CHUNK_TEXT || chunk->type == CC_STREAM_CHUNK_THINKING) {
        ctx->saw_text = 1;
    } else if (chunk->type == CC_STREAM_CHUNK_TOOL_START ||
               chunk->type == CC_STREAM_CHUNK_TOOL_DELTA ||
               chunk->type == CC_STREAM_CHUNK_TOOL_END) {
        ctx->saw_tool = 1;
    } else if (chunk->type == CC_STREAM_CHUNK_FINISHED) {
        ctx->saw_done = 1;
        if (ctx->defer_finished_until_end) {
            ctx->protocol_finished = 1;
            return cc_result_ok();
        }
        ctx->finished = 1;
    }
    return ctx->on_chunk(chunk, ctx->user_data);
}

/**
 * @brief 为终止标记或成功 EOF 最多发出一次最终完成块。
 * @param[in,out] ctx 有效独占流状态，须有有效 on_chunk。
 * @return 已完成返回 CC_OK，首次发送返回上层回调结果，状态在回调前标记完成。
 */
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

/**
 * @brief 处理传输终止标记，或将事件交给协议解析并协调语义完成时点。
 * @param[in,out] ctx 有效流状态与协议操作表。
 * @param[in] event_json 可选借用事件，NULL 或空串跳过。
 * @return 终止标记、协议或下游回调结果；缺少解析器返回平台错误，延迟模式继续接受 usage。
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
    cc_result_free(&rc);
    if (finished && ctx->defer_finished_until_end) {
        ctx->protocol_finished=1;
        ctx->saw_done=1;
        return cc_result_ok();
    }
    if (finished) return emit_finished_once(ctx);
    return cc_result_ok();
}

/**
 * @brief 逐行收集 SSE data 字段，以换行合并后分发协议正文。
 * @param[in,out] ctx 有效流上下文。
 * @param[in,out] event_text 有效可写 NUL 结尾事件，拆分时覆盖行结束符。
 * @return 字符串构建、协议解析或下游回调结果，临时 builder 始终清理。
 * @note 支持 LF、CRLF 和裸 CR；event/id 等其他行按当前协议策略跳过。
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
        char *line_end = strpbrk(line, "\r\n");
        char *next_line = NULL;
        if (line_end) {
            next_line = line_end + ((*line_end == '\r' && line_end[1] == '\n') ? 2 : 1);
            *line_end = '\0';
        }
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\r') line[len - 1] = '\0';
        if (strncmp(line, "data:", 5) == 0) {
            const char *data = line + 5;
            if (*data == ' ') data++;
            if (data_json.length > 0) {
                rc = cc_string_builder_append(&data_json, "\n");
                if (rc.code != CC_OK) {
                    cc_string_builder_deinit(&data_json);
                    return rc;
                }
                cc_result_free(&rc);
            }
            rc = cc_string_builder_append(&data_json, data);
            if (rc.code != CC_OK) {
                cc_string_builder_deinit(&data_json);
                return rc;
            }
        }
        if (!line_end) break;
        line = next_line;
    }

    rc = dispatch_stream_event(ctx, cc_string_builder_cstr(&data_json));
    cc_string_builder_deinit(&data_json);
    return rc;
}

/**
 * @brief 在有界字节范围内识别一处 SSE 行结束符。
 * @param[in] at 借用当前位置，可为 NULL。
 * @param[in] end 同一缓冲的尾后地址，at 有效时须可与其比较。
 * @return CRLF 为二，单独 CR/LF 为一，NULL、到尾或普通字节为零。
 */
static size_t sse_line_ending_length(const char *at, const char *end)
{
    if (!at || at >= end) return 0;
    if (*at == '\r') return at + 1 < end && at[1] == '\n' ? 2U : 1U;
    return *at == '\n' ? 1U : 0U;
}

/**
 * @brief 在有界未消费数据中查找空行分隔的事件末尾。
 * @param[in] start 有效缓冲起点借用。
 * @param[in] end 同一缓冲尾后地址，须不小于 start。
 * @param[out] separator_length 有效槽，仅找到分隔符时写入其总字节数，失败保留原值。
 * @return 借用的第一段行结束符地址，未形成完整空行时返回 NULL。
 */
static char *find_sse_event_end(char *start, char *end, size_t *separator_length)
{
    char *line = start;
    while (line < end) {
        char *line_end = line;
        while (line_end < end && *line_end != '\r' && *line_end != '\n') line_end++;
        if (line_end == end) return NULL;
        size_t first_ending = sse_line_ending_length(line_end, end);
        char *next_line = line_end + first_ending;
        if (next_line >= end) return NULL;
        size_t second_ending = sse_line_ending_length(next_line, end);
        if (second_ending > 0) {
            *separator_length = first_ending + second_ending;
            return line_end;
        }
        line = next_line;
    }
    return NULL;
}

/**
 * @brief 顺序解析已有完整 SSE 事件，并压缩保留未成帧尾部。
 * @param[in,out] ctx 有效且已分配缓冲的独占上下文。
 * @return 处理完成返回 CC_OK，首个事件错误原样返回；错误前可能已交付块，缓冲随后由请求清理。
 */
static cc_result_t process_sse_buffer(cc_http_llm_stream_ctx_t *ctx)
{
    char *p = ctx->buffer;
    char *buf_end = ctx->buffer + ctx->buf_len;

    while (!ctx->finished && p < buf_end) {
        size_t sep_len = 0;
        char *end = find_sse_event_end(p, buf_end, &sep_len);
        if (!end) break;

        char saved = *end;
        *end = '\0';
        cc_result_t rc = process_sse_event(ctx, p);
        *end = saved;
        if (rc.code != CC_OK) return rc;
        cc_result_free(&rc);
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

/**
 * @brief 按 LF 拆出 NDJSON 事件，移除行尾 CR 并保留未完成尾行。
 * @param[in,out] ctx 有效且已分配缓冲的独占上下文。
 * @return 处理完成返回 CC_OK，事件或回调错误向上传递，部分块可能已交付。
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
        cc_result_free(&rc);
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

/**
 * @brief 接收任意大小 HTTP 字节块，按未成帧预算分段追加并穿插解析。
 * @param[in] data 至少含 len 字节的借用数据，零长度允许 NULL；原始 NUL 会返回 JSON 错误。
 * @param[in] len 本次字节数，先累计到有溢出检查的原始流计数。
 * @param[in,out] user_data 同步 perform 调用栈上的有效独占流状态。
 * @return 追加、分帧或上层回调结果；已完成或空片段返回 CC_OK，失败前可已交付部分块。
 */
static cc_result_t stream_body_callback(const char *data, size_t len, void *user_data)
{
    cc_http_llm_stream_ctx_t *ctx = (cc_http_llm_stream_ctx_t *)user_data;
    if (!ctx || (len && !data))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid HTTP stream body callback");
    if (ctx->finished || !len) return cc_result_ok();
    if (ctx->stream_kind != CC_LLM_STREAM_SSE && ctx->stream_kind != CC_LLM_STREAM_NDJSON)
        return cc_result_error(CC_ERR_PLATFORM, "Unsupported LLM stream framing");
    if (memchr(data, '\0', len))
        return cc_result_error(CC_ERR_JSON, "LLM stream contains a raw NUL");
    if (len > SIZE_MAX - ctx->total_bytes) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Stream byte count overflow");
    }
    ctx->total_bytes += len;

    size_t consumed = 0;
    while (consumed < len && !ctx->finished) {
        size_t limit = CC_HTTP_LLM_STREAM_MAX_PENDING_FRAME_BYTES;
        if (limit <= 1 || ctx->buf_len >= limit - 1)
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Stream frame exceeded pending buffer limit");
        size_t available = limit - ctx->buf_len - 1;
        size_t fragment = len - consumed;
        if (fragment > available) fragment = available;
        cc_result_t rc = stream_ctx_append(ctx, data + consumed, fragment);
        if (rc.code != CC_OK) return rc;
        consumed += fragment;
        if (ctx->stream_kind == CC_LLM_STREAM_SSE) rc = process_sse_buffer(ctx);
        else rc = process_ndjson_buffer(ctx);
        if (rc.code != CC_OK) return rc;
        cc_result_free(&rc);
    }
    return cc_result_ok();
}

/**
 * @brief 在成功传输结束后分发 NDJSON 最后一条无换行事件。
 * @param[in,out] ctx 有效流上下文，SSE 尾部继续保持完整事件分隔规则。
 * @return 无待处理尾行返回 CC_OK，否则交付协议结果；NDJSON 尾部在尝试分发后清空。
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

/**
 * @brief 同步执行流式 POST，分帧转发并在成功终止后确认最终完成。
 * @param[in,out] http_client 有效借用端口，全部 body/metrics 回调须在 perform 返回前退出。
 * @param[in] http_req 有效借用协议请求，URL、正文与头覆盖同步调用。
 * @param[in] llm_request 可选统一请求借用，提供观测与绝对期限。
 * @param[in,out] protocol 本次使用的协议借用，self 与操作表覆盖全部解析回调。
 * @param[in] cancel_token 可选取消令牌借用。
 * @param[in] timeout_ms 正值毫秒超时，非正使用 300000。
 * @param[in] on_chunk 有效同步输出回调。
 * @param[in,out] user_data 上层回调上下文借用，覆盖全部回调和返回。
 * @return 传输、状态、解析或回调结果；缺少语义/传输完成时返回 NETWORK，所有临时响应与缓冲始终清理。
 * @note 错误前可有部分块，延迟完成模式先接 usage，成功 EOF 后补最终完成。
 * perform 的传输或 body 回调错误优先保留，仅 perform 及尾部解析成功后对非 2xx 做 HTTP 状态分类。
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
        .run_id = cc_llm_chat_request_run_id(llm_request),
        .step = llm_request ? llm_request->step : 0,
    };
    char header_profile[96];
    build_header_profile("POST", http_req->headers, http_req->header_count, header_profile, sizeof(header_profile));
    cc_http_llm_stream_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.protocol = protocol;
    ctx.stream_kind = http_req->stream_kind;
    ctx.defer_finished_until_end = http_req->defer_finished_until_end;
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
    /* 原始 SSE/NDJSON 传输采用零整体预算；分帧层限制未完成事件，内容预算和绝对期限由各自层执行。 */
    http_request.max_response_bytes = 0;
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
        cc_result_free(&rc);
        rc = ensure_http_response_text(&response, response.status_code >= 200 && response.status_code < 300);
    }
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = flush_stream_tail(&ctx);
    }

    if (rc.code == CC_OK && (response.status_code < 200 || response.status_code >= 300)) {
        cc_result_free(&rc);
        rc = http_response_status_error("LLM stream", &response);
    }

    /* 语义完成后允许 usage 继续到达，干净 EOF 也可触发最终完成。 */
    if (rc.code == CC_OK && !ctx.finished && ctx.protocol_finished) {
        cc_result_free(&rc);
        rc=emit_finished_once(&ctx);
    }

    if (rc.code == CC_OK && !ctx.saw_done) {
        cc_result_free(&rc);
        if (ctx.saw_event || ctx.saw_text || ctx.saw_tool) {
            ctx.partial_stream = 1;
        }
        rc = cc_result_error(CC_ERR_NETWORK, "stream_ended_without_done");
    }

    cc_http_response_free(&response);
    free(ctx.buffer);
    return rc;
}

/**
 * @brief 构建并发送同步请求，再将成功正文交协议解析。
 * @param[in,out] self 存活 provider 借用，不能为空。
 * @param[in] request 有效统一请求借用，覆盖全部协议与 HTTP 调用。
 * @param[out] out_response 有效空目标，入口清零；解析交付的完整或部分字段由调用者 cc_llm_response_free。
 * @return 构建、HTTP 或解析结果，JSON 错误替换成固定诊断；失败也可能有部分响应字段。
 * @note 所有完整或部分 HTTP 请求与正文在返回前清理，外层协调 provider 的并发和销毁。
 */
static cc_result_t http_llm_chat(
    void *self,
    const cc_llm_chat_request_t *request,
    cc_llm_response_t *out_response
)
{
    cc_http_llm_provider_t *provider = (cc_http_llm_provider_t *)self;
    if (!provider || !request || !out_response)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid HTTP LLM chat request");
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
    if (rc.code != CC_OK) {
        cc_llm_http_request_cleanup(&http_req);
        return rc;
    }
    cc_result_free(&rc);

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

/**
 * @brief 构建流式请求并同步交付协议块，按本次分帧方式检查流支持。
 * @param[in,out] self 存活 provider 借用，不能为空。
 * @param[in] request 有效统一请求，覆盖全部协议与传输调用。
 * @param[in] on_chunk 有效同步输出回调。
 * @param[in,out] user_data 可选上层上下文借用，至少保持到本次请求及回调结束。
 * @return 构建、流支持、传输或回调结果；失败前可能已交付部分块，HTTP 请求字段始终清理。
 */
static cc_result_t http_llm_chat_stream(
    void *self,
    const cc_llm_chat_request_t *request,
    cc_llm_stream_callback_fn on_chunk,
    void *user_data
)
{
    cc_http_llm_provider_t *provider = (cc_http_llm_provider_t *)self;
    if (!provider || !request || !on_chunk)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid HTTP LLM stream request");
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
    if (rc.code != CC_OK) {
        cc_llm_http_request_cleanup(&http_req);
        return rc;
    }
    cc_result_free(&rc);

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

/**
 * @brief 释放 provider 拥有配置并调用成功接管协议的可选析构。
 * @param[in,out] self 消费拥有 provider，允许 NULL。
 * @post provider 与协议拥有状态释放，HTTP 端口继续由外层保管。
 * @pre owner 已关闭入口并排空所有同步请求、协议和上层回调。
 */
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

/**
 * @brief 输出通用文本/工具能力，按协议名称补模态标记，并据解析回调声明流支持。
 * @param[in] self provider 借用，可为 NULL 时仅得到基线能力且 streaming 为零。
 * @param[out] out_capabilities 有效目标，先清零后完整填充，固定产物数量 8、单产物 10 MiB。
 * @return 正常返回 CC_OK，NULL 输出返回参数错误；未知或空协议名保留基线标记。
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
    out_capabilities->size = sizeof(*out_capabilities);
    out_capabilities->text_input = 1;
    out_capabilities->text_output = 1;
    out_capabilities->tool_calling = 1;
    out_capabilities->reasoning = 1;
    cc_http_llm_provider_t *provider = (cc_http_llm_provider_t *)self;
    out_capabilities->streaming = provider && provider->protocol.vtable &&
        provider->protocol.vtable->parse_stream_event != NULL;
    out_capabilities->max_artifacts = 8;
    out_capabilities->max_artifact_bytes = 10u * 1024u * 1024u;

    const char *name = provider && provider->protocol.vtable && provider->protocol.vtable->name ?
        provider->protocol.vtable->name(provider->protocol.self) : "";
    if (!name) name = "";
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

/* 静态操作表覆盖全部 provider 实例寿命，绑定同步/流式请求、销毁和能力查询。 */
static cc_llm_provider_vtable_t http_llm_vtable = {
    http_llm_chat,
    http_llm_chat_stream,
    http_llm_destroy,
    http_llm_capabilities
};

/**
 * @brief 先复制全部配置，成功后才交接协议并发布拥有 provider。
 * @param[in,out] http_client 有效端口借用，覆盖 provider 全部请求与销毁。
 * @param[in] base_url 调用期间借用地址，NULL 复制为空串。
 * @param[in] api_key 可选调用期凭据借用，非 NULL 时复制失败返回 OOM。
 * @param[in] model 调用期间借用默认模型，NULL 复制为空串。
 * @param[in,out] protocol 须有 build_request/parse_response 的协议，成功接管，失败保持外层所有权。
 * @param[out] out_provider 必填空资源槽，成功发布 self/vtable，失败保留原值。
 * @return CC_OK、参数错误或 OOM，失败清理本次副本，协议 self 和借用 HTTP 保持归原 owner。
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
    if (!provider->base_url || (api_key && !provider->api_key) || !provider->model) {
        http_llm_destroy(provider);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy HTTP LLM provider config");
    }
    provider->protocol = protocol;

    out_provider->self = provider;
    out_provider->vtable = &http_llm_vtable;
    return cc_result_ok();
}
