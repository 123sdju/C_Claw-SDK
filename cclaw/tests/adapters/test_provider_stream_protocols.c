#include "cc/adapters/cc_llm_providers.h"

#include <string.h>

typedef struct fake_http_state {
    const char *payload;
    size_t split_at;
} fake_http_state_t;

typedef struct stream_observation {
    int text;
    int tool_start;
    int tool_delta;
    int tool_end;
    int finished;
    int saw_hello;
} stream_observation_t;

static cc_result_t fake_http_perform(
    void *self,
    const cc_http_request_t *request,
    cc_http_response_t *out_response)
{
    fake_http_state_t *state = (fake_http_state_t *)self;
    if (!state || !state->payload || !request || !request->on_body || !out_response) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid fake HTTP request");
    }

    memset(out_response, 0, sizeof(*out_response));
    out_response->status_code = 200;
    size_t length = strlen(state->payload);
    size_t split = state->split_at;
    if (split == 0 || split >= length) split = length;

    cc_result_t rc = request->on_body(state->payload, split, request->user_data);
    if (rc.code != CC_OK || split == length) return rc;
    return request->on_body(state->payload + split, length - split, request->user_data);
}

static cc_result_t fake_http_reset(void *self)
{
    (void)self;
    return cc_result_ok();
}

static void fake_http_destroy(void *self)
{
    (void)self;
}

static const cc_http_client_vtable_t s_fake_http_vtable = {
    fake_http_perform,
    fake_http_reset,
    fake_http_destroy,
};

static cc_result_t observe_chunk(const cc_stream_chunk_t *chunk, void *user_data)
{
    stream_observation_t *observation = (stream_observation_t *)user_data;
    if (!chunk || !observation) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid stream observation");
    }

    switch (chunk->type) {
        case CC_STREAM_CHUNK_TEXT:
            observation->text++;
            if (chunk->content && strcmp(chunk->content, "hello") == 0) {
                observation->saw_hello = 1;
            }
            break;
        case CC_STREAM_CHUNK_TOOL_START:
            observation->tool_start++;
            break;
        case CC_STREAM_CHUNK_TOOL_DELTA:
            observation->tool_delta++;
            break;
        case CC_STREAM_CHUNK_TOOL_END:
            observation->tool_end++;
            break;
        case CC_STREAM_CHUNK_FINISHED:
            observation->finished++;
            break;
        default:
            break;
    }
    return cc_result_ok();
}

static cc_result_t run_stream(
    cc_llm_provider_t *provider,
    stream_observation_t *observation)
{
    cc_llm_chat_request_t request;
    memset(&request, 0, sizeof(request));
    request.size = sizeof(request);
    request.stream = 1;
    request.timeout_ms = 1000;
    request.max_response_bytes = 32U * 1024U;
    return provider->vtable->chat_stream(
        provider->self,
        &request,
        observe_chunk,
        observation);
}

static void destroy_provider(cc_llm_provider_t *provider)
{
    if (provider && provider->vtable && provider->vtable->destroy) {
        provider->vtable->destroy(provider->self);
    }
    if (provider) memset(provider, 0, sizeof(*provider));
}

static int test_openai_mixed_delta(void)
{
    static const char payload[] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hello\",\"tool_calls\":[{\"index\":0,\"id\":\"call-1\",\"function\":{\"name\":\"clock.now\",\"arguments\":\"{}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    fake_http_state_t state = {payload, 37};
    cc_http_client_t client = {
        &state, &s_fake_http_vtable, sizeof(cc_http_client_t), CC_HTTP_CAP_STREAM_BODY};
    cc_llm_provider_t provider = {0};
    cc_result_t rc = cc_openai_provider_create(&client, NULL, "test-key", "test-model", &provider);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return 1;
    }

    stream_observation_t observation = {0};
    rc = run_stream(&provider, &observation);
    int failed = rc.code != CC_OK || observation.text != 1 || !observation.saw_hello ||
        observation.tool_start != 1 || observation.tool_delta != 1 ||
        observation.tool_end != 1 || observation.finished != 1;
    cc_result_free(&rc);
    destroy_provider(&provider);
    return failed;
}

static int test_anthropic_text_stop_is_not_tool_stop(void)
{
    static const char payload[] =
        "data: {\"type\":\"message_start\"}\n\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"hello\"}}\n\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n";
    fake_http_state_t state = {payload, 51};
    cc_http_client_t client = {
        &state, &s_fake_http_vtable, sizeof(cc_http_client_t), CC_HTTP_CAP_STREAM_BODY};
    cc_llm_provider_t provider = {0};
    cc_result_t rc = cc_anthropic_provider_create(&client, NULL, "test-key", "test-model", &provider);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return 1;
    }

    stream_observation_t observation = {0};
    rc = run_stream(&provider, &observation);
    int failed = rc.code != CC_OK || observation.text != 1 || !observation.saw_hello ||
        observation.tool_end != 0 || observation.finished != 1;
    cc_result_free(&rc);
    destroy_provider(&provider);
    return failed;
}

typedef cc_result_t (*provider_create_fn)(cc_http_client_t *, cc_llm_provider_t *);

static cc_result_t create_openai(cc_http_client_t *client, cc_llm_provider_t *provider)
{
    return cc_openai_provider_create(client, NULL, "test-key", "test-model", provider);
}

static cc_result_t create_anthropic(cc_http_client_t *client, cc_llm_provider_t *provider)
{
    return cc_anthropic_provider_create(client, NULL, "test-key", "test-model", provider);
}

static cc_result_t create_ollama(cc_http_client_t *client, cc_llm_provider_t *provider)
{
    return cc_ollama_provider_create(client, NULL, "test-model", provider);
}

static int test_malformed_stream(
    provider_create_fn create_provider,
    const char *payload)
{
    fake_http_state_t state = {payload, 0};
    cc_http_client_t client = {
        &state, &s_fake_http_vtable, sizeof(cc_http_client_t), CC_HTTP_CAP_STREAM_BODY};
    cc_llm_provider_t provider = {0};
    cc_result_t rc = create_provider(&client, &provider);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return 1;
    }

    stream_observation_t observation = {0};
    rc = run_stream(&provider, &observation);
    int failed = rc.code != CC_ERR_JSON;
    cc_result_free(&rc);
    destroy_provider(&provider);
    return failed;
}

int main(void)
{
    int failed = 0;
    failed |= test_openai_mixed_delta();
    failed |= test_anthropic_text_stop_is_not_tool_stop();
    failed |= test_malformed_stream(
        create_openai,
        "data: {\"choices\":[{\"delta\":{\"content\":7}}]}\n\n");
    failed |= test_malformed_stream(
        create_anthropic,
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":7}}\n\n");
    failed |= test_malformed_stream(
        create_ollama,
        "{\"message\":{\"content\":7},\"done\":false}\n");
    return failed ? 1 : 0;
}
