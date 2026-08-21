#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_context_builder.h"
#include "cc/core/cc_message.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*
 * 函数 cc_memory_session_store_create：声明 cclaw/tests/core/test_runtime_request_config.c 中的 memory session store create 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

/* fake LLM 记录 runtime 传入 request 的 max_tokens/temperature。 */
typedef struct {
    int sync_seen;
    int sync_max_tokens;
    double sync_temperature;
    int stream_seen;
    int stream_max_tokens;
    double stream_temperature;
    int summary_seen;
    int summary_max_tokens;
    double summary_temperature;
    const char *summary_response;
} fake_llm_state_t;

/* 浮点比较 helper。 */
static int double_eq(double a, double b)
{
    double d = a - b;
    if (d < 0.0) d = -d;
    return d < 0.000001;
}

/*
 * fake 同步 LLM。
 *
 * 普通 run 返回 sync-ok；如果上下文 builder 触发摘要 prompt，则记录 summary 配置并返回
 * summary-ok，用来验证 summary_max_tokens/summary_temperature 透传。
 */
static cc_result_t fake_chat(void *self, const cc_llm_chat_request_t *request, cc_llm_response_t *out)
{
    fake_llm_state_t *state = (fake_llm_state_t *)self;
    cc_llm_response_init(out);

    char *messages_text = NULL;
    if (request) {
        cc_result_t json_rc = cc_messages_to_json(
            request->messages, request->message_count, 1, &messages_text);
        if (json_rc.code != CC_OK) return json_rc;
    }

    if (messages_text &&
        strstr(messages_text, "Summarize the following conversation")) {
        state->summary_seen++;
        state->summary_max_tokens = request->max_tokens;
        state->summary_temperature = request->temperature;
        cc_result_t rc = cc_llm_response_set_text(
            out, state->summary_response ? state->summary_response : "summary-ok");
        free(messages_text);
        if (rc.code != CC_OK) return rc;
    } else {
        state->sync_seen = 1;
        state->sync_max_tokens = request ? request->max_tokens : -1;
        state->sync_temperature = request ? request->temperature : -1.0;
        cc_result_t rc = cc_llm_response_set_text(out, "sync-ok");
        free(messages_text);
        if (rc.code != CC_OK) return rc;
    }

    out->finished = 1;
    return cc_result_ok();
}

/* fake stream LLM：记录 stream request 配置并发出 text + finished chunk。 */
static cc_result_t fake_chat_stream(
    void *self,
    const cc_llm_chat_request_t *request,
    cc_llm_stream_callback_fn on_chunk,
    void *user_data
)
{
    fake_llm_state_t *state = (fake_llm_state_t *)self;
    state->stream_seen = 1;
    state->stream_max_tokens = request ? request->max_tokens : -1;
    state->stream_temperature = request ? request->temperature : -1.0;

    cc_stream_chunk_t text = {
        .type = CC_STREAM_CHUNK_TEXT,
        .content = "stream-ok",
        .tool_index = -1,
    };
    cc_stream_chunk_t done = {
        .type = CC_STREAM_CHUNK_FINISHED,
        .tool_index = -1,
    };
    cc_result_t rc = on_chunk(&text, user_data);
    if (rc.code != CC_OK) return rc;
    return on_chunk(&done, user_data);
}

/* fake provider vtable，只实现 chat/chat_stream。 */
static cc_llm_provider_vtable_t fake_vtable = {
    fake_chat,
    fake_chat_stream,
    NULL
};

/* 创建一套最小 runtime 依赖，供多个测试段复用。 */
static int create_runtime(
    fake_llm_state_t *state,
    cc_agent_runtime_config_t config,
    cc_tool_registry_t **out_registry,
    cc_session_store_t *out_store,
    cc_agent_runtime_t **out_runtime
)
{
    if (cc_tool_registry_create(out_registry).code != CC_OK) return 1;
    if (cc_tool_registry_freeze(*out_registry).code != CC_OK) return 1;
    if (cc_memory_session_store_create(out_store).code != CC_OK) return 1;

    cc_agent_runtime_deps_t deps = {0};
    deps.llm.self = state;
    deps.llm.vtable = &fake_vtable;
    deps.tool_registry = *out_registry;
    deps.store = *out_store;

    cc_agent_runtime_options_t options = {0};
    options.config = config;
    return cc_agent_runtime_create(&deps, &options, out_runtime).code == CC_OK ? 0 : 1;
}

/* 销毁 create_runtime 创建的 runtime/store/registry。 */
static void destroy_runtime(
    cc_tool_registry_t *registry,
    cc_session_store_t *store,
    cc_agent_runtime_t *runtime
)
{
    cc_agent_runtime_destroy(runtime);
    if (store->vtable && store->vtable->destroy) store->vtable->destroy(store->self);
    cc_tool_registry_destroy(registry);
}

/* 向 session store 追加 user message，用来制造需要压缩的长历史。 */
static int append_user_message(cc_session_store_t *store, const char *session_id, const char *id, const char *content)
{
    cc_message_t *msg = NULL;
    cc_result_t rc = cc_message_create_text(id, session_id, CC_ROLE_USER, content, NULL, &msg);
    if (rc.code != CC_OK) return 1;
    cc_session_record_t record = {
        .type = CC_SESSION_RECORD_MESSAGE,
        .session_id = session_id,
        .data.message = msg,
    };
    rc = store->vtable->append_records(store->self, &record, 1);
    cc_message_destroy(msg);
    int failed = rc.code != CC_OK;
    cc_result_free(&rc);
    return failed;
}

/*
 * 验证 runtime request 配置透传。
 *
 * 覆盖 buffered/live 调用共用的 stream step engine 对 max_tokens/temperature 的透传，
 * 以及 context 压缩摘要请求使用独立的 summary 配置。
 */
int main(void)
{
    int failed = 0;
    fake_llm_state_t state = {0};
    cc_tool_registry_t *registry = NULL;
    cc_session_store_t store = {0};
    cc_agent_runtime_t *runtime = NULL;

    cc_agent_runtime_config_t request_config = {
        .max_steps = 1,
        .system_prompt = "system",
        .workspace_dir = ".",
        .model = "fake",
        .max_tokens = 1234,
        .temperature = 0.25
    };
    if (create_runtime(&state, request_config, &registry, &store, &runtime)) return 1;

    store.vtable->create_session(store.self, "sync", ".");
    char *response = NULL;
    cc_result_t rc = cc_agent_runtime_handle_message(runtime, "sync", "hello", &response);
    if (rc.code != CC_OK || !response || strcmp(response, "stream-ok") != 0) failed = 1;
    if (!state.stream_seen || state.stream_max_tokens != 1234 ||
        !double_eq(state.stream_temperature, 0.25)) failed = 1;
    cc_result_free(&rc);
    free(response);

    store.vtable->create_session(store.self, "stream", ".");
    response = NULL;
    rc = cc_agent_runtime_handle_message_stream_cb(runtime, "stream", "hello", NULL, &response);
    if (rc.code != CC_OK || !response || strcmp(response, "stream-ok") != 0) failed = 1;
    if (!state.stream_seen || state.stream_max_tokens != 1234 ||
        !double_eq(state.stream_temperature, 0.25)) failed = 1;
    cc_result_free(&rc);
    free(response);
    destroy_runtime(registry, &store, runtime);

    memset(&state, 0, sizeof(state));
    registry = NULL;
    memset(&store, 0, sizeof(store));
    runtime = NULL;
    cc_agent_runtime_config_t summary_config = {
        .max_steps = 1,
        .system_prompt = "system",
        .workspace_dir = ".",
        .model = "fake",
        .context_window_tokens = 80,
        .context_compress_threshold = 0.8,
        .context_keep_recent = 1,
        .summary_max_tokens = 321,
        .summary_temperature = 0.15
    };
    if (create_runtime(&state, summary_config, &registry, &store, &runtime)) return 1;

    store.vtable->create_session(store.self, "summary", ".");
    const char *long_text =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    for (int i = 0; i < 8; i++) {
        char id[16];
        snprintf(id, sizeof(id), "m%d", i);
        if (append_user_message(&store, "summary", id, long_text)) failed = 1;
    }

    cc_message_t *messages = NULL;
    size_t message_count = 0;
    rc = cc_context_builder_build_messages(runtime, "summary", "system", NULL,
                                            &messages, &message_count);
    char *messages_text = NULL;
    if (rc.code == CC_OK) {
        rc = cc_messages_to_json(messages, message_count, 1, &messages_text);
    }
    if (rc.code != CC_OK || !messages_text) failed = 1;
    if (!state.summary_seen || state.summary_max_tokens != 321 ||
        !double_eq(state.summary_temperature, 0.15)) failed = 1;
    if (messages_text && !strstr(messages_text, "summary-ok")) failed = 1;
    free(messages_text);
    for (size_t i = 0; i < message_count; i++) cc_message_cleanup(&messages[i]);
    free(messages);
    cc_result_free(&rc);

    /* 相同 session 历史再次构建应命中摘要缓存，不重复调用 provider。 */
    messages = NULL;
    message_count = 0;
    rc = cc_context_builder_build_messages(runtime, "summary", "system", NULL,
                                            &messages, &message_count);
    if (rc.code != CC_OK || state.summary_seen != 1) failed = 1;
    for (size_t i = 0; i < message_count; i++) cc_message_cleanup(&messages[i]);
    free(messages);
    cc_result_free(&rc);

    /*
     * 新消息会改变待摘要前缀及其 fingerprint，缓存必须失效并重新生成摘要，
     * 不能把旧摘要错误地用于更新后的历史。
     */
    if (append_user_message(&store, "summary", "m8", long_text)) failed = 1;
    state.summary_response = long_text;
    messages = NULL;
    message_count = 0;
    rc = cc_context_builder_build_messages(runtime, "summary", "system", NULL,
                                            &messages, &message_count);
    if (rc.code != CC_OK || state.summary_seen != 2) failed = 1;
    messages_text = NULL;
    if (rc.code == CC_OK) {
        cc_result_t json_rc = cc_messages_to_json(
            messages, message_count, 1, &messages_text);
        if (json_rc.code != CC_OK ||
            (messages_text && strstr(messages_text, "Earlier conversation summary"))) {
            failed = 1;
        }
        cc_result_free(&json_rc);
    }
    free(messages_text);
    for (size_t i = 0; i < message_count; i++) cc_message_cleanup(&messages[i]);
    free(messages);
    cc_result_free(&rc);
    destroy_runtime(registry, &store, runtime);

    return failed ? 1 : 0;
}
