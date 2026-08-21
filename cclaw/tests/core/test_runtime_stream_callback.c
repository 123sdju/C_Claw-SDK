#include "cc/app/cc_agent_runtime.h"
#include "cc/core/cc_observability.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*
 * 函数 cc_memory_session_store_create：声明 cclaw/tests/core/test_runtime_stream_callback.c 中的 memory session store create 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
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

/* 验证 stream callback 与 event bus 是两条输出路径：callback 给实时 UI，event bus 给观测。 */
static void on_event(const char *event_type, const char *event_json, void *user_data)
{
    event_capture_t *capture = (event_capture_t *)user_data;
    if (!capture) return;
    if (strcmp(event_type, CC_OBS_EVENT_STREAM_TEXT) == 0) capture->text_events++;
    if (strcmp(event_type, CC_OBS_EVENT_STREAM_FINISHED) == 0) capture->finished_events++;
    snprintf(capture->last_payload, sizeof(capture->last_payload), "%s",
        event_json ? event_json : "");
}

/* fake 同步 chat，只为 provider vtable 完整性提供 fallback。 */
static cc_result_t fake_chat(void *self, const cc_llm_chat_request_t *request, cc_llm_response_t *out)
{
    (void)self;
    (void)request;
    cc_llm_response_init(out);
    return cc_llm_response_set_text(out, "sync");
}

/* fake stream provider：发出一个 TEXT chunk 和一个 FINISHED chunk。 */
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

/* runtime stream callback：记录 text/finished/error chunk 的顺序相关计数。 */
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

/* 构造最小 runtime，可选择 event bus 和 stream 字节上限。 */
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
    if (cc_tool_registry_create(out_registry).code != CC_OK) return 1;
    cc_tool_registry_freeze(*out_registry);
    if (cc_memory_session_store_create(out_store).code != CC_OK) return 1;
    if (out_bus && cc_event_bus_create(out_bus).code != CC_OK) return 1;

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
    return cc_agent_runtime_create(&deps, &options, out_runtime).code == CC_OK ? 0 : 1;
}

/* 销毁 create_runtime 创建的依赖对象。 */
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

/*
 * 验证正式 stream callback API。
 *
 * 第一段确认 callback 实时输出和 event bus observability 同时工作；第二段确认
 * max_stream_bytes 超限会发 error chunk 并返回 CC_ERR_LIMIT_EXCEEDED。
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
    cc_event_bus_subscribe(bus, CC_OBS_EVENT_STREAM_TEXT, on_event, &events, NULL,
        &text_subscription);
    cc_event_bus_subscribe(bus, CC_OBS_EVENT_STREAM_FINISHED, on_event, &events, NULL,
        &finished_subscription);
    cc_agent_runtime_stream_options_t options = {0};
    options.on_chunk = on_chunk;
    options.user_data = &state;
    cc_agent_runtime_create_session(runtime, "stream_cb", ".");
    cc_result_t rc = cc_agent_runtime_handle_message_stream_cb(
        runtime, "stream_cb", "hi", &options, &response);
    if (rc.code != CC_OK || !response || strcmp(response, "hello") != 0) failed = 1;
    if (state.text_seen != 1 || state.finished_seen != 1 ||
        state.error_seen != 0 || strcmp(state.text, "hello") != 0) failed = 1;
    if (events.text_events != 1 || events.finished_events != 1) failed = 1;
    if (!strstr(events.last_payload, "\"schema_version\":1")) failed = 1;
    if (!strstr(events.last_payload, "\"status\":\"finished\"")) failed = 1;
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
    cc_agent_runtime_create_session(runtime, "stream_limit", ".");
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
    cc_agent_runtime_create_session(runtime, "sync_adapted_stream", ".");
    rc = cc_agent_runtime_handle_message_stream_cb(
        runtime, "sync_adapted_stream", "hi", &options, &response);
    if (rc.code != CC_OK || !response || strcmp(response, "sync") != 0) failed = 1;
    if (state.text_seen != 1 || state.finished_seen != 1 ||
        state.error_seen != 0 || strcmp(state.text, "sync") != 0) failed = 1;
    cc_result_free(&rc);
    free(response);

    response = NULL;
    cc_agent_runtime_create_session(runtime, "sync_adapted_buffered", ".");
    rc = cc_agent_runtime_handle_message(
        runtime, "sync_adapted_buffered", "hi", &response);
    if (rc.code != CC_OK || !response || strcmp(response, "sync") != 0) failed = 1;
    cc_result_free(&rc);
    free(response);
    destroy_runtime(registry, &store, NULL, runtime);

    return failed ? 1 : 0;
}
