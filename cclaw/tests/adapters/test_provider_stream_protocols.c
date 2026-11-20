#include "cc/adapters/cc_llm_providers.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_json.h"
#include "../../core/vendor/cjson/cJSON.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/** @brief 串行 JSON 故障记录，所有测试在单线程恢复钩子后退出。 */
static void *json_allocations[4096];
static size_t json_attempts, json_live, json_fail_at = SIZE_MAX;
#ifdef CC_TEST_PROVIDER_OOM
static size_t fail_copy_bytes;
static int fail_copy_once, inside_json_allocator;
/** @brief 绕过链接包装的真实 malloc。 @param[in] bytes 分配字节数。 @return 标准分配结果。 */
void *__real_malloc(size_t bytes);
/**
 * @brief 在 cJSON 分配器之外拒绝一次指定长度副本。
 * @param[in] bytes 本次分配字节数。
 * @return 故障命中为 NULL，其他交真实分配器。
 */
void *__wrap_malloc(size_t bytes)
{
    if (fail_copy_once && !inside_json_allocator && bytes == fail_copy_bytes) { fail_copy_once = 0; return NULL; }
    return __real_malloc(bytes);
}
#endif

/**
 * @brief 逐项拒绝 cJSON 真实分配并记录活跃地址。
 * @param[in] bytes 本次字节数。
 * @return 命中故障返回 NULL，正常返回标准拥有地址，记录容量不足由断言报告。
 */
static void *json_test_allocate(size_t bytes)
{
    if (json_attempts++ == json_fail_at) return NULL;
#ifdef CC_TEST_PROVIDER_OOM
    inside_json_allocator = 1;
#endif
    void *result = malloc(bytes);
#ifdef CC_TEST_PROVIDER_OOM
    inside_json_allocator = 0;
#endif
    if (!result) return NULL;
    for (size_t i = 0; i < sizeof(json_allocations) / sizeof(json_allocations[0]); i++) {
        if (!json_allocations[i]) { json_allocations[i] = result; json_live++; return result; }
    }
    assert(!"JSON allocation tracker is full");
    free(result);
    return NULL;
}

/**
 * @brief 归还已登记 cJSON 地址。
 * @param[in,out] pointer 可空消费地址，其他地址须来自本测试钩子。
 * @post 清空记录并 free，未知地址由断言终止。
 */
static void json_test_free(void *pointer)
{
    if (!pointer) return;
    for (size_t i = 0; i < sizeof(json_allocations) / sizeof(json_allocations[0]); i++) {
        if (json_allocations[i] == pointer) { json_allocations[i] = NULL; json_live--; free(pointer); return; }
    }
    assert(!"JSON released an unknown address");
}

/** @brief 同步 fixture 借用响应，独占保存可选请求副本。 */
typedef struct fake_http_state {
    const char *payload; /**< 覆盖一次 provider 调用的响应文本借用。 */
    size_t split_at; /**< 首段字节数，零或超长采用一次交付。 */
    int capture_request; /**< 非零深拷贝本次请求体。 */
    char *request_body; /**< 测试拥有的最近请求副本，最终 free。 */
    size_t calls, json_allocations_before_http; /**< HTTP 次数和进入传输前的 JSON 分配次数。 */
} fake_http_state_t;

/** @brief 单线程回调独占的块数量、值快照与完成顺序。 */
typedef struct stream_observation {
    int text;
    int tool_start;
    int tool_delta;
    int tool_end;
    int finished;
    int saw_hello;
    int usage_count;
    cc_llm_usage_t usage;
    int sequence;
    int usage_sequence;
    int finished_sequence;
    int owned_success; /**< 非零时返回带拥有诊断的 CC_OK，供 sanitizer 验证释放。 */
} stream_observation_t;

/**
 * @brief 捕获可选请求并以整体正文或最多两段回调返回合成响应。
 * @param[in,out] self 有效 fixture 借用，payload 非空并覆盖同步调用。
 * @param[in] request 有效 HTTP 请求借用，全部嵌套回调在返回前完成。
 * @param[out] out_response 有效空响应槽，成功整体正文交调用者释放，流式不保留正文。
 * @return 参数、请求/正文分配或 body 回调结果，fixture 拥有请求副本留给测试清理。
 */
static cc_result_t fake_http_perform(
    void *self,
    const cc_http_request_t *request,
    cc_http_response_t *out_response)
{
    fake_http_state_t *state = (fake_http_state_t *)self;
    if (!state || !state->payload || !request || !out_response) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid fake HTTP request");
    }
    state->calls++;
    state->json_allocations_before_http = json_attempts;
    if (state->capture_request) {
        free(state->request_body);
        state->request_body=cc_copy_string(request->body);
        if (!state->request_body) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot capture request");
    }

    memset(out_response, 0, sizeof(*out_response));
    out_response->status_code = 200;
    size_t length = strlen(state->payload);
    if (!request->on_body) {
        out_response->body=malloc(length+1U);
        if (!out_response->body) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot copy fake response");
        memcpy(out_response->body,state->payload,length+1U); out_response->body_size=length;
        return cc_result_ok();
    }
    size_t split = state->split_at;
    if (split == 0 || split >= length) split = length;

    cc_result_t rc = request->on_body(state->payload, split, request->user_data);
    if (rc.code != CC_OK || split == length) return rc;
    cc_result_free(&rc);
    return request->on_body(state->payload + split, length - split, request->user_data);
}

/** @brief 提供无连接状态的成功重置。 @param[in] self 未使用，可为 NULL。 @return 无拥有消息的 CC_OK。 */
static cc_result_t fake_http_reset(void *self)
{
    (void)self;
    return cc_result_ok();
}

/** @brief 结束端口借用，fixture 仍归测试栈。 @param[in] self 未使用，可为 NULL。 @post 没有拥有资源需要释放。 */
static void fake_http_destroy(void *self)
{
    (void)self;
}

static const cc_http_client_vtable_t s_fake_http_vtable = {
    fake_http_perform,
    fake_http_reset,
    fake_http_destroy,
};

/**
 * @brief 按值记录块种类、hello 文本和 usage/完成的相对顺序。
 * @param[in] chunk 有效同步块借用，字段不跨回调保存。
 * @param[in,out] user_data 有效独占观测槽。
 * @return 参数错误或 CC_OK，usage 按值复制，字符串只在回调内比较。
 */
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
            observation->finished_sequence=++observation->sequence;
            break;
        case CC_STREAM_CHUNK_USAGE:
            observation->usage_count++;
            observation->usage=chunk->usage;
            observation->usage_sequence=++observation->sequence;
            break;
        default:
            break;
    }
    return observation->owned_success ? cc_result_error(CC_OK, "synthetic successful callback") : cc_result_ok();
}

/**
 * @brief 用统一空消息请求运行一次真实流入口。
 * @param[in,out] provider 已创建 provider 借用，覆盖同步请求与回调。
 * @param[in,out] observation 有效独占观测槽，调用者预先清零。
 * @return provider 的拥有结果，调用者释放诊断。
 */
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

/**
 * @brief 消费已退出调用的 provider 并清空外部记录。
 * @param[in,out] provider 可选初始化记录，无有效析构时仅清零。
 * @post provider 拥有状态释放，HTTP 与 fixture 继续由测试拥有。
 */
static void destroy_provider(cc_llm_provider_t *provider)
{
    if (provider && provider->vtable && provider->vtable->destroy) {
        provider->vtable->destroy(provider->self);
    }
    if (provider) memset(provider, 0, sizeof(*provider));
}

/** @brief 检查同一事件中文本与完整工具块均交付。 @return 通过为零，失败为一，provider 和结果已清理。 */
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

/** @brief 检查 null 工具字段、仅 index 与分段名称/参数兼容。 @return 通过为零，失败为一，provider 和结果已清理。 */
static int test_openai_null_optional_tool_fields(void)
{
    static const char payload[] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hello\",\"tool_calls\":null}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":null,\"function\":{\"name\":\"clock.now\",\"arguments\":null}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"name\":null,\"arguments\":\"{}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    fake_http_state_t state = {payload, 29};
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

/** @brief 验证空 choices 元数据不终止后续文本和完成。 @return 通过为零，失败为一，全部资源归还。 */
static int test_openai_metadata_event_is_ignored(void)
{
    static const char payload[] =
        "data: {\"id\":\"meta\",\"choices\":[]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    fake_http_state_t state = {payload, 17};
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
        observation.finished != 1;
    cc_result_free(&rc);
    destroy_provider(&provider);
    return failed;
}

/** @brief 对照合法用量和负数/小数，检查缓存 reported 与 valid 分开。 @return 通过为零，失败为一，结果和 provider 已清理。 */
static int test_openai_usage_validation(void)
{
    static const char valid[] =
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":200,\"completion_tokens\":25,"
        "\"prompt_tokens_details\":{\"cached_tokens\":120,\"cache_write_tokens\":30}}}\n\n"
        "data: [DONE]\n\n";
    static const char invalid[] =
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":200.5,\"completion_tokens\":10,"
        "\"prompt_tokens_details\":{\"cached_tokens\":-1,\"cache_write_tokens\":1.5}}}\n\n"
        "data: [DONE]\n\n";
    fake_http_state_t state={valid,31};
    cc_http_client_t client={&state,&s_fake_http_vtable,sizeof(cc_http_client_t),CC_HTTP_CAP_STREAM_BODY};
    cc_llm_provider_t provider={0}; cc_result_t rc=cc_openai_provider_create(&client,NULL,"test-key","test-model",&provider);
    if (rc.code!=CC_OK) { cc_result_free(&rc); return 1; }
    stream_observation_t observed={0}; rc=run_stream(&provider,&observed);
    int failed=rc.code!=CC_OK || observed.usage_count!=1 || observed.usage.prompt_tokens!=200 ||
        observed.usage.completion_tokens!=25 || !observed.usage.cached_tokens_valid || observed.usage.cached_tokens!=120 ||
        !observed.usage.cache_write_tokens_valid || observed.usage.cache_write_tokens!=30;
    cc_result_free(&rc); state.payload=invalid; memset(&observed,0,sizeof(observed));
    rc=run_stream(&provider,&observed);
    failed |= rc.code!=CC_OK || observed.usage_count!=1 || observed.usage.prompt_tokens!=0 ||
        observed.usage.completion_tokens!=10 || observed.usage.cached_tokens_valid || observed.usage.cache_write_tokens_valid ||
        !observed.usage.cached_tokens_reported || !observed.usage.cache_write_tokens_reported;
    cc_result_free(&rc);
    state.payload="data: {\"choices\":[],\"usage\":{\"prompt_tokens\":0,\"completion_tokens\":0,\"prompt_tokens_details\":{\"cached_tokens\":0,\"cache_write_tokens\":0}}}\n\ndata: [DONE]\n\n";
    memset(&observed,0,sizeof(observed)); rc=run_stream(&provider,&observed);
    failed |= rc.code!=CC_OK || observed.usage_count!=1 || !observed.usage.valid ||
        !observed.usage.cached_tokens_reported || !observed.usage.cached_tokens_valid || observed.usage.cached_tokens ||
        !observed.usage.cache_write_tokens_reported || !observed.usage.cache_write_tokens_valid || observed.usage.cache_write_tokens;
    cc_result_free(&rc);
    state.payload="data: {\"choices\":[],\"usage\":{}}\n\ndata: [DONE]\n\n";
    memset(&observed,0,sizeof(observed)); rc=run_stream(&provider,&observed);
    failed |= rc.code!=CC_OK || observed.usage_count!=0;
    cc_result_free(&rc); destroy_provider(&provider); return failed;
}

/**
 * @brief 从实际发送请求读取 include_usage 布尔值。
 * @param[in] body 有效捕获请求文本借用。
 * @return 解析失败为负二，缺失或非布尔为负一，显式布尔为零或一，临时树清理。
 */
static int request_usage_option(const char *body)
{
    cc_json_value_t *root=NULL;
    cc_result_t rc=cc_json_parse_strict(body,&root);
    if (rc.code!=CC_OK) { cc_result_free(&rc); return -2; }
    cc_json_value_t *options=cc_json_object_get(root,"stream_options");
    cc_json_value_t *include=cc_json_object_get(options,"include_usage");
    int result=cc_json_is_bool(include) ? cc_json_bool_value(include) : -1;
    cc_json_destroy(root); cc_result_free(&rc); return result;
}

/**
 * @brief 检查官方自动请求用量、兼容地址显式选择与完成后 usage 顺序。
 * @return 通过为零，失败为一，覆盖 DONE/干净 EOF/不完整尾部/非法后续内容并归还捕获请求。
 */
static int test_openai_stream_usage_request(void)
{
    static const char full[]=
        "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":200,\"completion_tokens\":25,"
        "\"prompt_tokens_details\":{\"cached_tokens\":120}}}\n\n"
        "data: [DONE]\n\n";
    static const char no_done[]=
        "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n";
    static const char no_done_usage[]=
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":200,\"completion_tokens\":25,"
        "\"prompt_tokens_details\":{\"cached_tokens\":120}}}\n\n";
    static const char partial_usage[]=
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":200}}";
    static const char invalid_tail[]=
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"late\"}}]}\n\n"
        "data: [DONE]\n\n";
    fake_http_state_t state={.payload=full,.split_at=90,.capture_request=1};
    cc_http_client_t client={&state,&s_fake_http_vtable,sizeof(cc_http_client_t),CC_HTTP_CAP_STREAM_BODY};
    cc_llm_provider_t provider={0};
    cc_result_t rc=cc_openai_provider_create(&client,NULL,"test-key","test-model",&provider);
    if (rc.code!=CC_OK) { cc_result_free(&rc); return 1; }
    stream_observation_t observed={0}; rc=run_stream(&provider,&observed);
    int failed=rc.code!=CC_OK || request_usage_option(state.request_body)!=1 || observed.usage_count!=1 ||
        !observed.usage.cached_tokens_valid || observed.usage.cached_tokens!=120 || observed.finished!=1 ||
        observed.usage_sequence>=observed.finished_sequence;
    cc_result_free(&rc);
    state.payload=no_done; memset(&observed,0,sizeof(observed)); rc=run_stream(&provider,&observed);
    failed |= rc.code!=CC_OK || observed.finished!=1 || observed.usage_count!=0; cc_result_free(&rc);
    state.payload=no_done_usage; memset(&observed,0,sizeof(observed)); rc=run_stream(&provider,&observed);
    failed |= rc.code!=CC_OK || observed.finished!=1 || observed.usage_count!=1 ||
        observed.usage.cached_tokens!=120 || observed.usage_sequence>=observed.finished_sequence;
    cc_result_free(&rc);
    state.payload=partial_usage; memset(&observed,0,sizeof(observed)); rc=run_stream(&provider,&observed);
    failed |= rc.code!=CC_OK || observed.finished!=1 || observed.usage_count!=0; cc_result_free(&rc);
    state.payload=invalid_tail; memset(&observed,0,sizeof(observed)); rc=run_stream(&provider,&observed);
    failed |= rc.code!=CC_ERR_JSON || observed.finished!=0; cc_result_free(&rc);

    state.payload=full; cc_llm_chat_request_t request={0}; request.size=sizeof(request);
    request.stream=1; request.extra_body_json="{\"stream_options\":{\"include_usage\":false}}";
    memset(&observed,0,sizeof(observed));
    rc=provider.vtable->chat_stream(provider.self,&request,observe_chunk,&observed);
    failed |= rc.code!=CC_OK || request_usage_option(state.request_body)!=0 || observed.usage_count!=0;
    cc_result_free(&rc);
    state.payload="{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}";
    memset(&request,0,sizeof(request)); request.size=sizeof(request); cc_llm_response_t response={0};
    rc=provider.vtable->chat(provider.self,&request,&response);
    failed |= rc.code!=CC_OK || request_usage_option(state.request_body)!=-1;
    cc_result_free(&rc); cc_llm_response_free(&response); destroy_provider(&provider);

    state.payload=full; provider=(cc_llm_provider_t){0};
    rc=cc_openai_provider_create(&client,"https://compatible.example","test-key","test-model",&provider);
    if (rc.code!=CC_OK) { cc_result_free(&rc); free(state.request_body); return 1; }
    memset(&observed,0,sizeof(observed)); rc=run_stream(&provider,&observed);
    failed |= rc.code!=CC_OK || request_usage_option(state.request_body)!=-1 || observed.usage_count!=0;
    cc_result_free(&rc);
    memset(&request,0,sizeof(request)); request.size=sizeof(request); request.stream=1;
    request.extra_body_json="{\"stream_options\":{\"include_usage\":true}}";
    memset(&observed,0,sizeof(observed)); rc=provider.vtable->chat_stream(provider.self,&request,observe_chunk,&observed);
    failed |= rc.code!=CC_OK || request_usage_option(state.request_body)!=1 || observed.usage_count!=1 ||
        observed.usage_sequence>=observed.finished_sequence;
    cc_result_free(&rc); destroy_provider(&provider); free(state.request_body); return failed;
}

/** @brief 验证同步 tool_calls 对象不能当数组接纳。 @return JSON 错误为零，其他为一，响应和 provider 已清理。 */
static int test_openai_invalid_sync_tool_type(void)
{
    fake_http_state_t state={"{\"choices\":[{\"message\":{\"tool_calls\":{}}}]}",0};
    cc_http_client_t client={&state,&s_fake_http_vtable,sizeof(cc_http_client_t),CC_HTTP_CAP_STREAM_BODY};
    cc_llm_provider_t provider={0}; cc_result_t rc=cc_openai_provider_create(&client,NULL,"test-key","test-model",&provider);
    if (rc.code!=CC_OK) { cc_result_free(&rc); return 1; }
    cc_llm_chat_request_t request={0}; request.size=sizeof(request); request.max_tokens=10;
    cc_llm_response_t response={0}; rc=provider.vtable->chat(provider.self,&request,&response);
    int failed=rc.code!=CC_ERR_JSON;
    cc_result_free(&rc); cc_llm_response_free(&response); destroy_provider(&provider); return failed;
}

/** @brief 验证文本块结束不生成工具完成，message 结束只交付一次 FINISHED。 @return 通过为零，失败为一，资源归还。 */
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

/**
 * @brief 以固定合成凭据建立待测 provider。
 * @param[in,out] client 借用 HTTP，覆盖 provider 寿命。
 * @param[out] provider 必填空拥有输出，失败保持原值。
 * @return 对应构造器拥有结果，由调用者释放。
 */
typedef cc_result_t (*provider_create_fn)(cc_http_client_t *client, cc_llm_provider_t *provider);

/** @brief 创建 OpenAI fixture。 @param[in,out] client HTTP 借用。 @param[out] provider 空拥有槽。 @return 通用创建结果，诊断交调用者。 */
static cc_result_t create_openai(cc_http_client_t *client, cc_llm_provider_t *provider)
{
    return cc_openai_provider_create(client, NULL, "test-key", "test-model", provider);
}

/** @brief 创建 Anthropic fixture。 @param[in,out] client HTTP 借用。 @param[out] provider 空拥有槽。 @return 通用创建结果，诊断交调用者。 */
static cc_result_t create_anthropic(cc_http_client_t *client, cc_llm_provider_t *provider)
{
    return cc_anthropic_provider_create(client, NULL, "test-key", "test-model", provider);
}

/** @brief 创建 Ollama fixture。 @param[in,out] client HTTP 借用。 @param[out] provider 空拥有槽。 @return 通用创建结果，诊断交调用者。 */
static cc_result_t create_ollama(cc_http_client_t *client, cc_llm_provider_t *provider)
{
    return cc_ollama_provider_create(client, NULL, "test-model", provider);
}

/**
 * @brief 通过真实 provider 入口确认合成损坏事件返回 JSON 错误。
 * @param[in] create_provider 有效同步创建回调。
 * @param[in] payload 覆盖请求期间的合成响应文本借用。
 * @return 通过为零，失败为一，拥有结果和 provider 均清理。
 */
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

/**
 * @brief 核对 API 错误保持 MODEL 及合成业务说明。
 * @param[in] create 有效同步创建回调，HTTP 借用覆盖 provider。
 * @param[in] payload 调用期合成错误文本借用。
 * @return 通过为零，失败为一，结果与 provider 在返回前清理。
 */
static int test_stream_error_preserves_message(
    cc_result_t (*create)(cc_http_client_t *, cc_llm_provider_t *),
    const char *payload)
{
    fake_http_state_t state = {payload, 19};
    cc_http_client_t client = {
        &state, &s_fake_http_vtable, sizeof(cc_http_client_t), CC_HTTP_CAP_STREAM_BODY};
    cc_llm_provider_t provider = {0};
    cc_result_t rc = create(&client, &provider);
    if (rc.code != CC_OK) { cc_result_free(&rc); return 1; }
    stream_observation_t observation = {0};
    rc = run_stream(&provider, &observation);
    int failed = rc.code != CC_ERR_MODEL || !rc.message ||
        strcmp(rc.message, "No user query found in messages.") != 0;
    cc_result_free(&rc);
    destroy_provider(&provider);
    return failed;
}

/** @brief 无堆拥有资源的请求样本，字符串和数组只在同步 provider 调用中借用。 */
typedef struct model_fixture {
    cc_content_part_t system, user[4], assistant, results[2];
    cc_tool_call_t calls[2];
    cc_message_t messages[5];
    cc_llm_chat_request_t request;
} model_fixture_t;

/**
 * @brief 准备 system、混合媒体、双工具调用及两个原文结果。
 * @param[out] fixture 有效独占栈对象，入口清零，内部指针只借用自身数组或静态文字。
 * @post 请求在 fixture 保持原地址期间有效，调用者不对这些借用消息执行 cleanup。
 */
static void model_fixture_init(model_fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->system.kind = CC_MEDIA_TEXT; fixture->system.text = "stable system";
    fixture->user[0].kind = CC_MEDIA_TEXT; fixture->user[0].text = "question";
    fixture->user[1].kind = CC_MEDIA_IMAGE;
    fixture->user[1].artifact = (cc_media_artifact_t){.kind=CC_MEDIA_IMAGE,.mime="image/png",.data_base64="aW1hZ2U="};
    fixture->user[2].kind = CC_MEDIA_AUDIO;
    fixture->user[2].artifact = (cc_media_artifact_t){.kind=CC_MEDIA_AUDIO,.mime="audio/wav",.data_base64="YXVkaW8="};
    fixture->user[3].kind = CC_MEDIA_FILE;
    fixture->user[3].artifact = (cc_media_artifact_t){.kind=CC_MEDIA_FILE,.id="file-1",.path="fixture.txt",.bytes=12};
    fixture->assistant.kind = CC_MEDIA_TEXT; fixture->assistant.text = "before tools";
    fixture->results[0].kind = CC_MEDIA_TEXT; fixture->results[0].text = "exact result one";
    fixture->results[1].kind = CC_MEDIA_TEXT; fixture->results[1].text = "failure: synthetic result two";
    fixture->calls[0] = (cc_tool_call_t){.id="call-1",.name="first",.arguments_json="{\"x\":1}"};
    fixture->calls[1] = (cc_tool_call_t){.id="call-2",.name="second",.arguments_json="{\"y\":2}"};
    fixture->messages[0] = (cc_message_t){.role=CC_ROLE_SYSTEM,.content={&fixture->system,1,1}};
    fixture->messages[1] = (cc_message_t){.role=CC_ROLE_USER,.content={fixture->user,4,4}};
    fixture->messages[2] = (cc_message_t){.role=CC_ROLE_ASSISTANT,.content={&fixture->assistant,1,1},.tool_calls={fixture->calls,2,2}};
    fixture->messages[3] = (cc_message_t){.role=CC_ROLE_TOOL,.tool_call_id="call-1",.content={&fixture->results[0],1,1}};
    fixture->messages[4] = (cc_message_t){.role=CC_ROLE_TOOL,.tool_call_id="call-2",.content={&fixture->results[1],1,1}};
    fixture->request.size = sizeof(fixture->request);
    fixture->request.messages = fixture->messages; fixture->request.message_count = 5;
    fixture->request.model = "qwen-fixture"; fixture->request.thinking_mode = 1; fixture->request.max_tokens = 64;
    fixture->request.tools_json = "[{\"type\":\"function\",\"function\":{\"name\":\"first\",\"parameters\":{\"type\":\"object\"}}},"
        "{\"type\":\"function\",\"function\":{\"name\":\"second\",\"parameters\":{\"type\":\"object\"}}}]";
    fixture->request.extra_body_json = "{\"custom_option\":{\"enabled\":true}}";
}

/**
 * @brief 核对原生 tool_use/tool_result 保留双调用身份、输入、文本及顺序。
 * @return 通过为零，断言失败终止，JSON 树、响应、provider 和捕获请求均清理。
 */
static int test_anthropic_native_tool_history(void)
{
    model_fixture_t fixture; model_fixture_init(&fixture);
    fake_http_state_t state = {.payload="{\"content\":[],\"stop_reason\":\"end_turn\"}",.capture_request=1};
    cc_http_client_t client = {&state,&s_fake_http_vtable,sizeof(client),CC_HTTP_CAP_STREAM_BODY};
    cc_llm_provider_t provider = {0}; cc_result_t rc = create_anthropic(&client,&provider);
    assert(rc.code == CC_OK); cc_result_free(&rc);
    cc_llm_response_t response = {0}; rc = provider.vtable->chat(provider.self,&fixture.request,&response);
    assert(rc.code == CC_OK); cc_result_free(&rc); cc_llm_response_free(&response);
    cc_json_value_t *root = NULL; rc = cc_json_parse_strict(state.request_body,&root);
    assert(rc.code == CC_OK); cc_result_free(&rc);
    cc_json_value_t *messages = cc_json_object_get(root,"messages");
    assert(cc_json_array_size(messages) == 3);
    cc_json_value_t *assistant = cc_json_array_get(messages,1), *results = cc_json_array_get(messages,2);
    assert(strcmp(cc_json_string_value(cc_json_object_get(assistant,"role")),"assistant") == 0);
    cc_json_value_t *blocks = cc_json_object_get(assistant,"content");
    assert(cc_json_array_size(blocks) == 3 && strcmp(cc_json_string_value(cc_json_object_get(cc_json_array_get(blocks,0),"text")),"before tools") == 0);
    cc_json_value_t *result_blocks = cc_json_object_get(results,"content");
    assert(strcmp(cc_json_string_value(cc_json_object_get(results,"role")),"user") == 0 && cc_json_array_size(result_blocks) == 2);
    for (int i = 0; i < 2; i++) {
        cc_json_value_t *call = cc_json_array_get(blocks,i+1), *result = cc_json_array_get(result_blocks,i);
        assert(strcmp(cc_json_string_value(cc_json_object_get(call,"type")),"tool_use") == 0 &&
            strcmp(cc_json_string_value(cc_json_object_get(call,"id")),fixture.calls[i].id) == 0 &&
            strcmp(cc_json_string_value(cc_json_object_get(call,"name")),fixture.calls[i].name) == 0);
        assert(cc_json_int_value(cc_json_object_get(cc_json_object_get(call,"input"),i ? "y" : "x")) == i+1);
        assert(strcmp(cc_json_string_value(cc_json_object_get(result,"type")),"tool_result") == 0 &&
            strcmp(cc_json_string_value(cc_json_object_get(result,"tool_use_id")),fixture.calls[i].id) == 0 &&
            strcmp(cc_json_string_value(cc_json_object_get(result,"content")),fixture.results[i].text) == 0 &&
            !cc_json_object_get(result,"is_error"));
    }
    cc_json_destroy(root); destroy_provider(&provider); free(state.request_body);
    return 0;
}

/**
 * @brief 逐请求 JSON 分配拒绝，确认完整请求失败前不进入 HTTP，全部节点归还。
 * @return 三协议通过为零，断言失败终止，钩子逐次恢复默认且拥有响应清理。
 */
static int test_request_json_allocation_failures(void)
{
    const provider_create_fn factories[] = {create_openai,create_anthropic,create_ollama};
    const char *payloads[] = {"{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}","{\"content\":[]}","{\"message\":{\"content\":\"ok\"},\"done\":true}"};
    const char *streams[] = {"data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n",
        "data: {\"type\":\"message_stop\"}\n\n", "{\"done\":true}\n"};
    cJSON_Hooks hooks = {json_test_allocate,json_test_free};
    for (size_t p = 0; p < 3; p++) {
        model_fixture_t fixture; model_fixture_init(&fixture);
        fake_http_state_t state = {.payload=payloads[p]};
        cc_http_client_t client = {&state,&s_fake_http_vtable,sizeof(client),CC_HTTP_CAP_STREAM_BODY};
        cc_llm_provider_t provider = {0}; cc_result_t rc = factories[p](&client,&provider);
        assert(rc.code == CC_OK); cc_result_free(&rc);
        for (int stream = 0; stream < 2; stream++) {
            state.payload = stream ? streams[p] : payloads[p]; fixture.request.stream = stream;
            json_attempts = 0; json_fail_at = SIZE_MAX; cJSON_InitHooks(&hooks);
            cc_llm_response_t response = {0}; stream_observation_t observed = {0};
            rc = stream ? provider.vtable->chat_stream(provider.self,&fixture.request,observe_chunk,&observed) :
                provider.vtable->chat(provider.self,&fixture.request,&response);
            cJSON_InitHooks(NULL);
            assert(rc.code == CC_OK && !json_live); cc_result_free(&rc); cc_llm_response_free(&response);
            size_t required = state.json_allocations_before_http;
            assert(required > 0);
            for (size_t failure = 0; failure < required; failure++) {
                size_t calls = state.calls;
                json_attempts = 0; json_fail_at = failure; cJSON_InitHooks(&hooks);
                rc = stream ? provider.vtable->chat_stream(provider.self,&fixture.request,observe_chunk,&observed) :
                    provider.vtable->chat(provider.self,&fixture.request,&response);
                cJSON_InitHooks(NULL);
                assert(rc.code != CC_OK && state.calls == calls && !json_live);
                cc_result_free(&rc); cc_llm_response_free(&response);
            }
        }
        json_fail_at = SIZE_MAX;
        destroy_provider(&provider);
    }
    return 0;
}

/**
 * @brief 确认三协议损坏工具 JSON 和非法消息数组均拒绝整次请求。
 * @return 全部通过为零，断言失败终止，调用次数保持且诊断逐次归还。
 */
static int test_invalid_request_documents(void)
{
    const provider_create_fn factories[] = {create_openai,create_anthropic,create_ollama};
    const char *invalid[] = {"", "[", "[] trailing", "{}", "[\"\\u0000\"]", "[\"\\u12x4\"]"};
    for (size_t p = 0; p < 3; p++) {
        fake_http_state_t state = {.payload="{}"};
        cc_http_client_t client = {&state,&s_fake_http_vtable,sizeof(client),CC_HTTP_CAP_STREAM_BODY};
        cc_llm_provider_t provider = {0}; cc_result_t rc = factories[p](&client,&provider);
        assert(rc.code == CC_OK); cc_result_free(&rc);
        cc_llm_chat_request_t request = {0}; cc_llm_response_t response = {0};
        for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
            /* DTO 字段类型为 char *，本次同步构造只读取该静态合成文本。 */
            request.tools_json = (char *)invalid[i];
            rc = provider.vtable->chat(provider.self,&request,&response);
            assert(rc.code != CC_OK && !state.calls); cc_result_free(&rc); cc_llm_response_free(&response);
            stream_observation_t observed = {0};
            rc = provider.vtable->chat_stream(provider.self,&request,observe_chunk,&observed);
            assert(rc.code != CC_OK && !state.calls && !observed.finished); cc_result_free(&rc);
        }
        request.tools_json = NULL; request.message_count = 1;
        rc = provider.vtable->chat(provider.self,&request,&response);
        assert(rc.code == CC_ERR_INVALID_ARGUMENT && !state.calls); cc_result_free(&rc); cc_llm_response_free(&response);
        destroy_provider(&provider);
    }
    return 0;
}

/**
 * @brief 检查三协议同步响应拒绝尾部和解码 NUL，空推理字符串仍可接纳。
 * @return 全部通过为零，断言失败终止，每次响应清理，provider 最终销毁。
 */
static int test_response_documents(void)
{
    const provider_create_fn factories[] = {create_openai,create_anthropic,create_ollama};
    const char *invalid[][2] = {
        {"{\"choices\":[]} trailing", "{\"choices\":[{\"message\":{\"content\":\"cut\\u0000tail\"}}]}"},
        {"{\"content\":[]} trailing", "{\"content\":[{\"type\":\"text\",\"text\":\"cut\\u0000tail\"}]}"},
        {"{\"message\":{}} trailing", "{\"message\":{\"content\":\"cut\\u0000tail\"}}"}
    };
    const char *literal[] = {
        "{\"choices\":[{\"message\":{\"content\":\"literal\\\\u0000\"}}]}",
        "{\"content\":[{\"type\":\"text\",\"text\":\"literal\\\\u0000\"}]}",
        "{\"message\":{\"content\":\"literal\\\\u0000\"}}"
    };
    for (size_t p = 0; p < 3; p++) {
        fake_http_state_t state = {0};
        cc_http_client_t client = {&state,&s_fake_http_vtable,sizeof(client),CC_HTTP_CAP_STREAM_BODY};
        cc_llm_provider_t provider = {0}; cc_result_t rc = factories[p](&client,&provider);
        assert(rc.code == CC_OK); cc_result_free(&rc);
        cc_llm_chat_request_t request = {0}; cc_llm_response_t response = {0};
        for (size_t i = 0; i < 2; i++) {
            state.payload = invalid[p][i]; rc = provider.vtable->chat(provider.self,&request,&response);
            assert(rc.code == CC_ERR_JSON); cc_result_free(&rc); cc_llm_response_free(&response);
        }
        state.payload = literal[p]; rc = provider.vtable->chat(provider.self,&request,&response);
        assert(rc.code == CC_OK && response.text && strcmp(response.text,"literal\\u0000") == 0);
        cc_result_free(&rc); cc_llm_response_free(&response);
        if (!p) {
            state.payload = "{\"choices\":[{\"message\":{\"content\":\"ok\",\"reasoning_content\":\"\"}}]}";
            rc = provider.vtable->chat(provider.self,&request,&response);
            assert(rc.code == CC_OK); cc_result_free(&rc); cc_llm_response_free(&response);
            state.payload = "data: {\"choices\":[{\"delta\":{\"content\":\"hello\",\"reasoning_content\":\"\"}}]}\n\n"
                "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
            stream_observation_t observed = {0}; rc = run_stream(&provider,&observed);
            assert(rc.code == CC_OK && observed.text == 1 && observed.finished == 1); cc_result_free(&rc);
        }
        destroy_provider(&provider);
    }
    return 0;
}

/**
 * @brief 验证延迟 usage 和带拥有成功诊断的块回调，覆盖三个协议的主要块类型。
 * @return 全部通过为零，断言失败终止，sanitizer 检查中间成功诊断归还。
 */
static int test_owned_callbacks_and_late_usage(void)
{
    const provider_create_fn factories[] = {create_openai,create_anthropic,create_ollama};
    const char *payloads[] = {
        "data: {\"choices\":[{\"delta\":{\"content\":\"hello\",\"reasoning_content\":\"thinking\",\"tool_calls\":[{\"index\":0,\"id\":\"c\",\"function\":{\"name\":\"f\",\"arguments\":\"{}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\ndata: {\"choices\":[],\"usage\":{\"prompt_tokens\":0}}\n\ndata: [DONE]\n\n",
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"c\",\"name\":\"f\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{}\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"hello\"}}\n\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"}}\n\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":null},\"usage\":{\"output_tokens\":0}}\n\ndata: {\"type\":\"message_stop\"}\n\n",
        "{\"message\":{\"content\":\"hello\",\"reasoning_content\":\"thinking\",\"tool_calls\":[{\"id\":\"c\",\"function\":{\"name\":\"f\",\"arguments\":{}}}]},\"done\":true}\n"
    };
    for (size_t p = 0; p < 3; p++) {
        fake_http_state_t state = {.payload=payloads[p],.split_at=17};
        cc_http_client_t client = {&state,&s_fake_http_vtable,sizeof(client),CC_HTTP_CAP_STREAM_BODY};
        cc_llm_provider_t provider = {0}; cc_result_t rc = factories[p](&client,&provider);
        assert(rc.code == CC_OK); cc_result_free(&rc);
        stream_observation_t observed = {.owned_success=1}; rc = run_stream(&provider,&observed);
        assert(rc.code == CC_OK && observed.finished == 1 && observed.tool_start == 1 && observed.tool_delta == 1 && observed.tool_end == 1);
        if (p < 2) assert(observed.usage_count == 1 && observed.usage.valid && observed.usage_sequence < observed.finished_sequence);
        cc_result_free(&rc); destroy_provider(&provider);
    }
    return 0;
}

/**
 * @brief 在 JSON 分配器之外拒绝同步文本和推理副本，确认 OOM 沿 provider 返回。
 * @return 通过为零，Linux 断言验证故障被消费及 JSON 节点归零，其他平台保留正常专项覆盖。
 */
static int test_response_copy_failures(void)
{
#ifdef CC_TEST_PROVIDER_OOM
    const provider_create_fn factories[] = {create_openai,create_anthropic,create_ollama};
    const char *text = "response-copy-unique-1234567890123456789012345678901234567890";
    const char *payloads[] = {
        "{\"choices\":[{\"message\":{\"content\":\"response-copy-unique-1234567890123456789012345678901234567890\"}}]}",
        "{\"content\":[{\"type\":\"text\",\"text\":\"response-copy-unique-1234567890123456789012345678901234567890\"}]}",
        "{\"message\":{\"content\":\"response-copy-unique-1234567890123456789012345678901234567890\"}}"
    };
    const char *reasoning[] = {
        "{\"choices\":[{\"message\":{\"content\":\"ok\",\"reasoning_content\":\"response-copy-unique-1234567890123456789012345678901234567890\"}}]}",
        NULL,
        "{\"message\":{\"content\":\"ok\",\"reasoning_content\":\"response-copy-unique-1234567890123456789012345678901234567890\"}}"
    };
    cJSON_Hooks hooks = {json_test_allocate,json_test_free};
    for (size_t p = 0; p < 3; p++) {
        fake_http_state_t state = {0};
        cc_http_client_t client = {&state,&s_fake_http_vtable,sizeof(client),CC_HTTP_CAP_STREAM_BODY};
        cc_llm_provider_t provider = {0}; cc_result_t rc = factories[p](&client,&provider);
        assert(rc.code == CC_OK); cc_result_free(&rc);
        for (size_t copy = 0; copy < 2; copy++) {
            if (copy && !reasoning[p]) continue;
            state.payload = copy ? reasoning[p] : payloads[p];
            cc_llm_chat_request_t request = {0}; cc_llm_response_t response = {0};
            json_fail_at = SIZE_MAX; fail_copy_bytes = strlen(text) + 1; fail_copy_once = 1;
            cJSON_InitHooks(&hooks);
            rc = provider.vtable->chat(provider.self,&request,&response);
            cJSON_InitHooks(NULL);
            assert(rc.code == CC_ERR_OUT_OF_MEMORY && !fail_copy_once && !json_live);
            cc_result_free(&rc); cc_llm_response_free(&response);
        }
        destroy_provider(&provider);
    }
#endif
    return 0;
}

/** @brief 顺序运行三协议的分帧、字段、构造故障及用量契约组。 @return 全部通过为零，任一失败为一。 */
int main(void)
{
    int failed = 0;
    failed |= test_stream_error_preserves_message(create_openai,
        "data: {\"error\":{\"message\":\"No user query found in messages.\"}}\n\n");
    failed |= test_stream_error_preserves_message(create_ollama,
        "{\"error\":\"No user query found in messages.\"}\n");
    failed |= test_openai_mixed_delta();
    failed |= test_openai_null_optional_tool_fields();
    failed |= test_openai_metadata_event_is_ignored();
    failed |= test_openai_usage_validation();
    failed |= test_openai_stream_usage_request();
    failed |= test_openai_invalid_sync_tool_type();
    failed |= test_anthropic_text_stop_is_not_tool_stop();
    failed |= test_anthropic_native_tool_history();
    failed |= test_request_json_allocation_failures();
    failed |= test_invalid_request_documents();
    failed |= test_response_documents();
    failed |= test_owned_callbacks_and_late_usage();
    failed |= test_response_copy_failures();
    failed |= test_malformed_stream(
        create_openai,
        "data: {\"choices\":[{\"delta\":{\"content\":7}}]}\n\n");
    failed |= test_malformed_stream(
        create_openai,
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":{}}}]}\n\n");
    failed |= test_malformed_stream(
        create_anthropic,
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":7}}\n\n");
    failed |= test_malformed_stream(
        create_ollama,
        "{\"message\":{\"content\":7},\"done\":false}\n");
    failed |= test_malformed_stream(create_openai, "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0.5}]}}]}\n\n");
    failed |= test_malformed_stream(create_openai, "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":2147483648}]}}]}\n\n");
    failed |= test_malformed_stream(create_anthropic, "data: {\"type\":\"content_block_stop\",\"index\":0.5}\n\n");
    failed |= test_malformed_stream(create_anthropic, "data: {\"type\":\"content_block_stop\",\"index\":2147483648}\n\n");
    failed |= test_malformed_stream(create_openai, "data: {\"choices\":[]} trailing\n\n");
    failed |= test_malformed_stream(create_anthropic, "data: {\"type\":\"ping\",\"text\":\"cut\\u0000tail\"}\n\n");
    failed |= test_malformed_stream(create_ollama, "{\"done\":true} trailing\n");
    return failed ? 1 : 0;
}
