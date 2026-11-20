#include "cc/app/cc_agent_runtime.h"
#include "cc/core/cc_observability.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief 创建夹具拥有的内存会话端口。
 * @param[out] out_store 新端口槽，成功后调用 vtable.destroy 回收。
 * @return 构造状态与拥有诊断，调用者消费。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

/* stream callback 捕获状态，模拟实时 UI 收到的 chunk。 */
typedef struct callback_state {
    int text_seen;
    int finished_seen;
    int error_seen;
    char text[64];
} callback_state_t;

/* event bus 捕获状态，用来验证 observability 事件。 */
typedef struct event_capture {
    int text_events;
    int finished_events;
    char last_payload[512];
} event_capture_t;

/**
 * @brief 记录流观测事件计数及最后一条有界 JSON 副本。
 * @param[in] event_type 同步回调期事件名借用。
 * @param[in] event_json 可 NULL 的回调期正文借用。
 * @param[in,out] user_data event_capture_t 借用，可 NULL，主线程在 flush 后读取。
 * @return 无返回值，无拥有对象逸出。
 * @note 只写调用者持有的固定数组，事件总线排空后再结束上下文寿命。
 */
static void on_event(const char *event_type, const char *event_json, void *user_data)
{
    event_capture_t *capture = (event_capture_t *)user_data;
    if (!capture) return;
    if (strcmp(event_type, CC_OBS_EVENT_STREAM_TEXT) == 0) capture->text_events++;
    if (strcmp(event_type, CC_OBS_EVENT_STREAM_FINISHED) == 0) capture->finished_events++;
    snprintf(capture->last_payload, sizeof(capture->last_payload), "%s",
        event_json ? event_json : "");
}

/**
 * @brief 生成同步回退答案。
 * @param[in] self 未使用状态借用。
 * @param[in] request 未使用请求借用。
 * @param[out] out 新响应槽，取得独立文本副本，由 runtime 清理。
 * @return 文本构造结果，OOM 沿真实响应函数上报。
 */
static cc_result_t fake_chat(void *self, const cc_llm_chat_request_t *request, cc_llm_response_t *out)
{
    (void)self;
    (void)request;
    cc_llm_response_init(out);
    return cc_llm_response_set_text(out, "sync");
}

/**
 * @brief 顺序同步发出文本和结束块。
 * @param[in] self 调用期文本借用，覆盖回调期。
 * @param[in] request 未使用请求借用。
 * @param[in] on_chunk 同步块接收函数。
 * @param[in,out] user_data 原样传递的上下文借用。
 * @return 首个失败或末次回调拥有结果，中间成功诊断消费，块自身为栈借用。
 */
static cc_result_t fake_chat_stream(
    void *self,
    const cc_llm_chat_request_t *request,
    cc_llm_stream_callback_fn on_chunk,
    void *user_data
)
{
    const char *text = (const char *)self;
    (void)request;
    cc_stream_chunk_t chunk = {
        .type = CC_STREAM_CHUNK_TEXT,
        .content = (char *)text,
        .tool_index = -1,
    };
    cc_stream_chunk_t done = {
        .type = CC_STREAM_CHUNK_FINISHED,
        .tool_index = -1,
    };
    cc_result_t rc = on_chunk(&chunk, user_data);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    return on_chunk(&done, user_data);
}

/* fake provider vtable，支持同步和流式。 */
static cc_llm_provider_vtable_t fake_vtable = {
    fake_chat,
    fake_chat_stream,
    NULL,
    NULL
};

/* 只实现同步 chat，用来验证 Runtime 的同步响应 -> chunk 兼容适配。 */
static cc_llm_provider_vtable_t sync_only_vtable = {
    fake_chat,
    NULL,
    NULL,
    NULL
};

/**
 * @brief 收集实际 runtime 发出的文本、结束和错误块。
 * @param[in] chunk 回调期块借用，NULL 返回参数错误。
 * @param[in,out] user_data callback_state_t 借用，NULL 返回参数错误，文本按固定容量截断。
 * @return 无拥有字段的成功或拥有参数错误诊断，调用者消费，不保留输入。
 */
static cc_result_t on_chunk(const cc_stream_chunk_t *chunk, void *user_data)
{
    callback_state_t *state = (callback_state_t *)user_data;
    if (!chunk || !state) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid test callback input");
    }
    if (chunk->type == CC_STREAM_CHUNK_TEXT) {
        state->text_seen++;
        if (chunk->content) {
            strncat(state->text, chunk->content,
                sizeof(state->text) - strlen(state->text) - 1);
        }
    } else if (chunk->type == CC_STREAM_CHUNK_FINISHED) {
        state->finished_seen++;
    } else if (chunk->type == CC_STREAM_CHUNK_ERROR) {
        state->error_seen++;
    }
    return cc_result_ok();
}

/**
 * @brief 检查成功并消费一个结果，供夹具各构造和订阅步骤使用。
 * @param[in] result 按值移交的拥有结果。
 * @return 成功一，失败零，无保留诊断。
 */
static int success(cc_result_t result)
{
    int good = result.code == CC_OK;
    cc_result_free(&result);
    return good;
}

/**
 * @brief 创建最小 runtime 及由调用者持有的依赖，失败回收全部候选。
 * @param[in] stream_text 调用期文本借用，覆盖 runtime 寿命。
 * @param[in] provider_vtable 常驻 provider 函数表借用。
 * @param[in] max_stream_bytes 流正文预算，零采用 runtime 默认行为。
 * @param[out] out_registry 必填新注册表槽，成功由 destroy_runtime 回收。
 * @param[out] out_store 必填新存储槽，成功由 destroy_runtime 回收。
 * @param[out] out_bus 可 NULL，非 NULL 时创建拥有事件总线。
 * @param[out] out_runtime 必填新运行时槽，成功后保持其他依赖借用。
 * @return 完整创建零，失败一且输出清空，全部步骤结果诊断消费，构造串行执行。
 */
static int create_runtime(
    const char *stream_text,
    const cc_llm_provider_vtable_t *provider_vtable,
    size_t max_stream_bytes,
    cc_tool_registry_t **out_registry,
    cc_session_store_t *out_store,
    cc_event_bus_t **out_bus,
    cc_agent_runtime_t **out_runtime
)
{
    *out_registry = NULL;
    memset(out_store, 0, sizeof(*out_store));
    if (out_bus) *out_bus = NULL;
    *out_runtime = NULL;
    if (!success(cc_tool_registry_create(out_registry))) goto failed;
    if (!success(cc_tool_registry_freeze(*out_registry))) goto failed;
    if (!success(cc_memory_session_store_create(out_store))) goto failed;
    if (out_bus && !success(cc_event_bus_create(out_bus))) goto failed;

    cc_agent_runtime_deps_t deps = {0};
    deps.llm.self = (void *)stream_text;
    deps.llm.vtable = provider_vtable;
    deps.tool_registry = *out_registry;
    deps.store = *out_store;
    deps.event_bus = out_bus ? *out_bus : NULL;

    cc_agent_runtime_options_t options = {0};
    options.config.max_steps = 1;
    options.config.workspace_dir = ".";
    options.config.limits.max_stream_bytes = max_stream_bytes;
    if (success(cc_agent_runtime_create(&deps, &options, out_runtime))) return 0;
failed:
    cc_agent_runtime_destroy(*out_runtime);
    *out_runtime = NULL;
    if (out_bus) { cc_event_bus_destroy(*out_bus); *out_bus = NULL; }
    if (out_store->vtable && out_store->vtable->destroy) out_store->vtable->destroy(out_store->self);
    memset(out_store, 0, sizeof(*out_store));
    cc_tool_registry_destroy(*out_registry);
    *out_registry = NULL;
    return 1;
}

/**
 * @brief 排空并销毁夹具创建的运行时及依赖。
 * @param[in] registry 注册表拥有指针，可 NULL。
 * @param[in,out] store 存储拥有视图借用，本函数消费其 self，调用者不重复销毁。
 * @param[in] bus 事件总线拥有指针，可 NULL，销毁排空回调。
 * @param[in] runtime 运行时拥有指针，可 NULL，先结束其依赖使用。
 * @return 无返回值，所有输入拥有对象在本函数消费。
 * @note 主线程串行调用，所有 stream 同步回调已退出。
 */
static void destroy_runtime(
    cc_tool_registry_t *registry,
    cc_session_store_t *store,
    cc_event_bus_t *bus,
    cc_agent_runtime_t *runtime
)
{
    cc_agent_runtime_destroy(runtime);
    cc_event_bus_destroy(bus);
    if (store->vtable && store->vtable->destroy) store->vtable->destroy(store->self);
    cc_tool_registry_destroy(registry);
}

/**
 * @brief 验证流回调、观测事件、输出上限及同步 provider 的流式兼容输出。
 * @return 所有断言通过零，失败一，正常路径消费结果/答案并销毁各组依赖。
 * @note 观测状态在事件 flush 后读取，stream 回调在对应 runtime 调用内同步借用。
 */
int main(void)
{
    int failed = 0;
    cc_tool_registry_t *registry = NULL;
    cc_session_store_t store = {0};
    cc_event_bus_t *bus = NULL;
    cc_agent_runtime_t *runtime = NULL;
    callback_state_t state = {0};
    event_capture_t events = {0};
    char *response = NULL;

    if (create_runtime("hello", &fake_vtable, 0,
        &registry, &store, &bus, &runtime)) return 1;
    cc_event_subscription_t text_subscription = {0};
    cc_event_subscription_t finished_subscription = {0};
    if (!success(cc_event_bus_subscribe(bus, CC_OBS_EVENT_STREAM_TEXT, on_event, &events, NULL,
        &text_subscription))) failed = 1;
    if (!success(cc_event_bus_subscribe(bus, CC_OBS_EVENT_STREAM_FINISHED, on_event, &events, NULL,
        &finished_subscription))) failed = 1;
    cc_agent_runtime_stream_options_t options = {0};
    options.on_chunk = on_chunk;
    options.user_data = &state;
    if (!success(cc_agent_runtime_create_session(runtime, "stream_cb", "."))) failed = 1;
    cc_result_t rc = cc_agent_runtime_handle_message_stream_cb(
        runtime, "stream_cb", "hi", &options, &response);
    if (rc.code != CC_OK || !response || strcmp(response, "hello") != 0) failed = 1;
    if (state.text_seen != 1 || state.finished_seen != 1 ||
        state.error_seen != 0 || strcmp(state.text, "hello") != 0) failed = 1;
    if (!success(cc_event_bus_flush(bus, 1000))) failed = 1;
    else {
        if (events.text_events != 1 || events.finished_events != 1) failed = 1;
        if (!strstr(events.last_payload, "\"schema_version\":1")) failed = 1;
        if (!strstr(events.last_payload, "\"status\":\"finished\"")) failed = 1;
    }
    cc_result_free(&rc);
    free(response);
    destroy_runtime(registry, &store, bus, runtime);

    registry = NULL;
    memset(&store, 0, sizeof(store));
    bus = NULL;
    runtime = NULL;
    memset(&state, 0, sizeof(state));
    response = NULL;
    if (create_runtime("too-long", &fake_vtable, 3,
        &registry, &store, NULL, &runtime)) return 1;
    options.user_data = &state;
    if (!success(cc_agent_runtime_create_session(runtime, "stream_limit", "."))) failed = 1;
    rc = cc_agent_runtime_handle_message_stream_cb(
        runtime, "stream_limit", "hi", &options, &response);
    if (rc.code != CC_ERR_LIMIT_EXCEEDED) failed = 1;
    if (state.error_seen != 1) failed = 1;
    cc_result_free(&rc);
    free(response);
    destroy_runtime(registry, &store, NULL, runtime);

    /* 同步-only provider 也进入同一 step engine，并可由流式 API 接收标准 chunk。 */
    registry = NULL;
    memset(&store, 0, sizeof(store));
    runtime = NULL;
    memset(&state, 0, sizeof(state));
    response = NULL;
    if (create_runtime(NULL, &sync_only_vtable, 0,
        &registry, &store, NULL, &runtime)) return 1;
    if (cc_agent_runtime_supports_stream(runtime) != 0) failed = 1;
    options.user_data = &state;
    if (!success(cc_agent_runtime_create_session(runtime, "sync_adapted_stream", "."))) failed = 1;
    rc = cc_agent_runtime_handle_message_stream_cb(
        runtime, "sync_adapted_stream", "hi", &options, &response);
    if (rc.code != CC_OK || !response || strcmp(response, "sync") != 0) failed = 1;
    if (state.text_seen != 1 || state.finished_seen != 1 ||
        state.error_seen != 0 || strcmp(state.text, "sync") != 0) failed = 1;
    cc_result_free(&rc);
    free(response);

    response = NULL;
    if (!success(cc_agent_runtime_create_session(runtime, "sync_adapted_buffered", "."))) failed = 1;
    rc = cc_agent_runtime_handle_message(
        runtime, "sync_adapted_buffered", "hi", &response);
    if (rc.code != CC_OK || !response || strcmp(response, "sync") != 0) failed = 1;
    cc_result_free(&rc);
    free(response);
    destroy_runtime(registry, &store, NULL, runtime);

    return failed ? 1 : 0;
}
