#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_cache_metrics.h"
#include "cc/app/cc_context_builder.h"
#include "cc/app/cc_runtime_builder.h"
#include "cc/core/cc_message.h"
#include "cc/core/cc_observability.h"
#include "cc/util/cc_json.h"

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief 创建夹具拥有的内存会话端口。
 * @param[out] out_store 新输出槽，成功以 vtable.destroy 回收。
 * @return 构造状态与拥有诊断，由调用者消费。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);
/**
 * @brief 创建夹具持久摘要测试使用的内存记忆后端。
 * @param[out] out_store 新输出槽，成功由 cc_memory_store_destroy 回收。
 * @return 构造状态与拥有诊断，由调用者消费。
 */
extern cc_result_t cc_memory_store_create_inmem(cc_memory_store_t *out_store);

/* fake LLM 记录 runtime 传入 request 的 max_tokens/temperature。 */
typedef struct {
    int sync_seen;
    int sync_max_tokens;
    double sync_temperature;
    int stream_seen;
    int stream_max_tokens;
    double stream_temperature;
    int summary_seen;
    int summary_prefix_seen;
    int summary_history_items;
    int summary_max_tokens;
    int summary_usage_enabled;
    int summary_fail;
    double summary_temperature;
    const char *summary_response;
} fake_llm_state_t;

/**
 * @brief 按百万分之一绝对容差比较两个配置数值。
 * @param[in] a 第一个数值。
 * @param[in] b 第二个数值。
 * @return 绝对差小于容差为一，否则零，无分配和共享状态。
 */
static int double_eq(double a, double b)
{
    double d = a - b;
    if (d < 0.0) d = -d;
    return d < 0.000001;
}

/**
 * @brief 检查实际请求内容并模拟摘要成功、无效摘要或摘要网络失败。
 * @param[in,out] self 独占 fake_llm_state_t 借用，记录请求参数及摘要历史项数。
 * @param[in] request 调用期请求借用，可 NULL，摘要模式要求包含消息。
 * @param[out] out 新响应槽，取得拥有答案及可选按值 usage，由 runtime 清理。
 * @return 真实序列化/响应构造错误或注入网络错误，临时文本和成功诊断在本层消费。
 * @note 同一夹具状态只由串行测试调用，正文和请求不保留借用。
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
        cc_result_free(&json_rc);
    }

    if (messages_text &&
        strstr(messages_text, "Summarize the following conversation")) {
        state->summary_seen++;
        char *summary_prompt = NULL;
        cc_result_t prompt_rc = cc_message_get_text_summary(&request->messages[0], &summary_prompt);
        state->summary_history_items = 0;
        if (prompt_rc.code == CC_OK && summary_prompt) {
            if (strstr(summary_prompt, "[previous verified summary]")) state->summary_prefix_seen++;
            const char *cursor = summary_prompt;
            while ((cursor = strstr(cursor, "[user]: ")) != NULL) {
                state->summary_history_items++;
                cursor += strlen("[user]: ");
            }
        }
        cc_result_free(&prompt_rc);
        free(summary_prompt);
        state->summary_max_tokens = request->max_tokens;
        state->summary_temperature = request->temperature;
        cc_result_t rc = cc_llm_response_set_text(
            out, state->summary_response ? state->summary_response : "summary-ok");
        free(messages_text);
        if (rc.code != CC_OK) return rc;
        cc_result_free(&rc);
        if (state->summary_usage_enabled) {
            out->usage.valid=1;
            out->usage.prompt_tokens=100;
            out->usage.cached_tokens_valid=1;
            out->usage.cached_tokens=0;
        }
        if (state->summary_fail) return cc_result_error(CC_ERR_NETWORK,"injected summary failure");
    } else {
        state->sync_seen = 1;
        state->sync_max_tokens = request ? request->max_tokens : -1;
        state->sync_temperature = request ? request->temperature : -1.0;
        cc_result_t rc = cc_llm_response_set_text(out, "sync-ok");
        free(messages_text);
        if (rc.code != CC_OK) return rc;
        cc_result_free(&rc);
    }

    out->finished = 1;
    return cc_result_ok();
}

/**
 * @brief 记录流请求 token/temperature 配置并同步发出文本与结束块。
 * @param[in,out] self 独占 provider 夹具状态借用。
 * @param[in] request 可 NULL 的调用期请求借用。
 * @param[in] on_chunk 同步块接收函数。
 * @param[in,out] user_data 原样传给回调的上下文借用。
 * @return 首个失败或末次回调拥有结果，中间成功诊断消费，栈上块不逸出。
 */
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
    cc_result_free(&rc);
    return on_chunk(&done, user_data);
}

/* fake provider vtable，只实现 chat/chat_stream。 */
static cc_llm_provider_vtable_t fake_vtable = {
    fake_chat,
    fake_chat_stream,
    NULL
};

/**
 * @brief 检查成功并消费操作诊断。
 * @param[in] result 按值移交的拥有结果。
 * @return 成功一，失败零。
 */
static int succeeded(cc_result_t result)
{
    int good = result.code == CC_OK;
    cc_result_free(&result);
    return good;
}

/**
 * @brief 构造独立运行时、会话库及冻结注册表，失败回收候选。
 * @param[in,out] state 调用者拥有的 provider 状态借用，覆盖 runtime 寿命。
 * @param[in] config 按值配置视图，runtime 构造按其契约复制。
 * @param[in] event_bus 可 NULL 的借用事件总线，由外层拥有。
 * @param[out] out_registry 新拥有注册表槽，成功由 destroy_runtime 回收。
 * @param[out] out_store 新拥有存储槽，成功由 destroy_runtime 回收。
 * @param[out] out_runtime 新拥有运行时槽，保持依赖借用直至销毁。
 * @return 完整成功零，失败一且输出为空，构造串行，诊断逐次消费。
 */
static int create_runtime(
    fake_llm_state_t *state,
    cc_agent_runtime_config_t config,
    cc_event_bus_t *event_bus,
    cc_tool_registry_t **out_registry,
    cc_session_store_t *out_store,
    cc_agent_runtime_t **out_runtime
)
{
    *out_registry = NULL;
    memset(out_store, 0, sizeof(*out_store));
    *out_runtime = NULL;
    if (!succeeded(cc_tool_registry_create(out_registry))) goto failed;
    if (!succeeded(cc_tool_registry_freeze(*out_registry))) goto failed;
    if (!succeeded(cc_memory_session_store_create(out_store))) goto failed;

    cc_agent_runtime_deps_t deps = {0};
    deps.llm.self = state;
    deps.llm.vtable = &fake_vtable;
    deps.tool_registry = *out_registry;
    deps.store = *out_store;
    deps.event_bus = event_bus;

    cc_agent_runtime_options_t options = {0};
    options.config = config;
    if (succeeded(cc_agent_runtime_create(&deps, &options, out_runtime))) return 0;
failed:
    cc_agent_runtime_destroy(*out_runtime);
    *out_runtime = NULL;
    if (out_store->vtable && out_store->vtable->destroy) out_store->vtable->destroy(out_store->self);
    memset(out_store, 0, sizeof(*out_store));
    cc_tool_registry_destroy(*out_registry);
    *out_registry = NULL;
    return 1;
}

/**
 * @brief 在运行调用退出后按依赖顺序消费夹具拥有对象。
 * @param[in] registry 拥有的冻结注册表，可 NULL。
 * @param[in,out] store 拥有存储视图借用，消费 self 后调用者不重复销毁。
 * @param[in] runtime 拥有的运行时，可 NULL，先结束对存储和注册表的借用。
 * @return 无返回值，依赖拥有对象被消费。
 * @note 外层串行调用，event_bus 与 provider 状态仍由调用者拥有。
 */
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

/**
 * @brief 创建并同步追加用户消息，制造实际可压缩历史。
 * @param[in,out] store 存活的会话端口借用，夹具串行写入。
 * @param[in] session_id 会话 ID 借用。
 * @param[in] id 消息 ID 借用。
 * @param[in] content 文本正文借用，仅覆盖构造调用。
 * @return 成功零，失败一，创建消息和全部诊断在返回前释放，下层追加复制输入。
 */
static int append_user_message(cc_session_store_t *store, const char *session_id, const char *id, const char *content)
{
    cc_message_t *msg = NULL;
    cc_result_t rc = cc_message_create_text(id, session_id, CC_ROLE_USER, content, NULL, &msg);
    if (rc.code != CC_OK) { cc_result_free(&rc); return 1; }
    cc_result_free(&rc);
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

/**
 * @brief 新建 runtime 后构造真实上下文并核对持久摘要及请求次数。
 * @param[in] deps 调用期依赖视图借用，底层实例由调用者拥有。
 * @param[in] options 调用期选项借用。
 * @param[in] state 存活 provider 状态借用，调用完成后读取累计次数。
 * @param[in] expected_calls 期望摘要 provider 累计调用次数。
 * @return 断言通过零，失败一，临时 runtime、消息数组、JSON 和诊断统一回收。
 */
static int build_persistent_summary(cc_agent_runtime_deps_t *deps,cc_agent_runtime_options_t *options,
                                    fake_llm_state_t *state,int expected_calls)
{
    cc_agent_runtime_t *runtime=NULL;
    cc_result_t rc=cc_agent_runtime_create(deps,options,&runtime);
    if (rc.code!=CC_OK) { cc_result_free(&rc); return 1; }
    cc_result_free(&rc);
    cc_message_t *messages=NULL; size_t count=0;
    rc=cc_context_builder_build_messages(runtime,"cache-session",options->config.system_prompt,NULL,&messages,&count);
    char *json=NULL;
    if (rc.code==CC_OK) { cc_result_free(&rc); rc=cc_messages_to_json(messages,count,1,&json); }
    int failed=rc.code!=CC_OK || !json || !strstr(json,"summary-ok") || state->summary_seen!=expected_calls;
    free(json); for (size_t i=0;i<count;i++) cc_message_cleanup(&messages[i]); free(messages);
    cc_result_free(&rc); cc_agent_runtime_destroy(runtime); return failed;
}

/**
 * @brief 验证摘要缓存键覆盖 token、temperature、provider、model/prompt 和 endpoint 命名空间。
 * @return 断言通过零，否则一，正常及构造失败路径释放拥有的记忆、会话和注册表。
 * @note 多个 runtime 串行创建销毁并共享借用后端，旧布局或缺命名空间时验证重新计算。
 */
static int test_persistent_summary_configuration(void)
{
    fake_llm_state_t state={0}; cc_tool_registry_t *registry=NULL;
    cc_session_store_t store={0}; cc_memory_store_t memory={0};
    int failed=1;
    cc_result_t rc=cc_tool_registry_create(&registry);
    if (rc.code!=CC_OK) { cc_result_free(&rc); goto cleanup; } cc_result_free(&rc);
    rc=cc_tool_registry_freeze(registry); if (rc.code!=CC_OK) { cc_result_free(&rc); goto cleanup; } cc_result_free(&rc);
    rc=cc_memory_session_store_create(&store); if (rc.code!=CC_OK) { cc_result_free(&rc); goto cleanup; } cc_result_free(&rc);
    rc=cc_memory_store_create_inmem(&memory); if (rc.code!=CC_OK) { cc_result_free(&rc); goto cleanup; } cc_result_free(&rc);
    rc=store.vtable->create_session(store.self,"cache-session",".");
    if (rc.code!=CC_OK) { cc_result_free(&rc); goto cleanup; } cc_result_free(&rc);
    const char *long_text=
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    for (int i=0;i<8;i++) {
        char id[16]; snprintf(id,sizeof(id),"cache-%d",i);
        if (append_user_message(&store,"cache-session",id,long_text)) goto cleanup;
    }
    cc_agent_runtime_deps_t deps={0}; deps.size=sizeof(deps); deps.llm.self=&state; deps.llm.vtable=&fake_vtable;
    deps.tool_registry=registry; deps.store=store; deps.memory_store=&memory;
    cc_agent_runtime_options_t options={0}; options.size=sizeof(options); options.provider_name="provider-a";
    options.summary_cache_namespace="https://endpoint-a.example";
    options.config.max_steps=1; options.config.system_prompt="system"; options.config.workspace_dir=".";
    options.config.model="model-a"; options.config.context_window_tokens=240;
    options.config.context_compress_threshold=0.8; options.config.context_keep_recent=1;
    options.config.summary_max_tokens=321; options.config.summary_temperature=0.15;
    failed=build_persistent_summary(&deps,&options,&state,1) ||
               build_persistent_summary(&deps,&options,&state,1);
    options.config.summary_max_tokens=222;
    failed |= build_persistent_summary(&deps,&options,&state,2);
    failed |= build_persistent_summary(&deps,&options,&state,2);
    options.config.summary_temperature=0.35;
    failed |= build_persistent_summary(&deps,&options,&state,3);
    options.provider_name="provider-b";
    failed |= build_persistent_summary(&deps,&options,&state,4);
    options.config.model="one"; options.config.system_prompt="two";
    failed |= build_persistent_summary(&deps,&options,&state,5);
    options.config.model="two"; options.config.system_prompt="one";
    failed |= build_persistent_summary(&deps,&options,&state,6);
    options.summary_cache_namespace="https://endpoint-b.example";
    failed |= build_persistent_summary(&deps,&options,&state,7);
    failed |= build_persistent_summary(&deps,&options,&state,7);
    options.summary_cache_namespace="https://endpoint-a.example";
    failed |= build_persistent_summary(&deps,&options,&state,7);
    /* 缺命名空间或使用旧 options 布局时，摘要重新计算以保持 provider 边界。 */
    options.summary_cache_namespace=NULL;
    failed |= build_persistent_summary(&deps,&options,&state,8);
    failed |= build_persistent_summary(&deps,&options,&state,9);
    options.summary_cache_namespace="https://endpoint-a.example";
    options.size=offsetof(cc_agent_runtime_options_t,summary_cache_namespace);
    failed |= build_persistent_summary(&deps,&options,&state,10);
cleanup:
    cc_memory_store_destroy(&memory);
    if (store.vtable && store.vtable->destroy) store.vtable->destroy(store.self);
    cc_tool_registry_destroy(registry);
    return failed;
}

/**
 * @brief 构造真实上下文并核对摘要标记是否出现。
 * @param[in,out] runtime 存活运行时借用，可能更新派生缓存。
 * @param[in] session_id 会话 ID 借用。
 * @param[in] expected 是否期望存在摘要标记。
 * @return 结果及标记匹配零，否则一，消息、JSON 与诊断全部回收，调用串行。
 */
static int expect_context_summary(cc_agent_runtime_t *runtime, const char *session_id, bool expected)
{
    cc_message_t *messages=NULL; size_t count=0; char *json=NULL;
    cc_result_t rc=cc_context_builder_build_messages(runtime,session_id,"system",NULL,&messages,&count);
    if (rc.code==CC_OK) { cc_result_free(&rc); rc=cc_messages_to_json(messages,count,1,&json); }
    int failed=rc.code!=CC_OK || !json ||
        ((strstr(json,"Earlier conversation summary:")!=NULL) != expected);
    free(json);
    for (size_t i=0;i<count;i++) cc_message_cleanup(&messages[i]);
    free(messages); cc_result_free(&rc); return failed;
}

/**
 * @brief 将缓存/使用量事件送入指标器。
 * @param[in] event_type 回调期名称借用。
 * @param[in] event_json 回调期 JSON 借用。
 * @param[in,out] user_data 指标器借用，存活到 unsubscribe 完成。
 * @return 无返回值，回调消费观测结果诊断。
 * @note 指标器内部同步，诊断在本回调消费，外层 flush 后读取快照。
 */
static void observe_summary_cache(const char *event_type, const char *event_json, void *user_data)
{
    cc_result_t rc=cc_cache_metrics_observe(user_data,event_type,event_json);
    cc_result_free(&rc);
}

/**
 * @brief 验证摘要前缀复用、历史重写失效、空/超限/失败摘要拒绝及观测计数。
 * @return 全部上下文和指标断言通过零，否则一，成功路径排空事件并释放所有依赖和快照。
 * @note 同一 provider 状态串行使用，订阅回调借用指标器直到 unsubscribe，检查快照前 flush。
 */
static int test_generation_summary_prefix(void)
{
    fake_llm_state_t state={0}; state.summary_usage_enabled=1; cc_tool_registry_t *registry=NULL;
    cc_session_store_t store={0}; cc_agent_runtime_t *runtime=NULL;
    cc_agent_runtime_config_t config={0}; config.max_steps=1; config.system_prompt="system";
    config.workspace_dir="."; config.model="fake"; config.context_window_tokens=240;
    config.context_compress_threshold=0.8; config.context_keep_recent=1;
    config.summary_max_tokens=321; config.summary_temperature=0.15;
    cc_cache_metrics_t *metrics=NULL; cc_event_bus_t *event_bus=NULL;
    cc_event_subscription_t subscription={0}, usage_subscription={0};
    cc_result_t rc=cc_cache_metrics_create(&metrics);
    if (rc.code!=CC_OK) { cc_result_free(&rc); return 1; } cc_result_free(&rc);
    rc=cc_event_bus_create(&event_bus);
    if (rc.code!=CC_OK) { cc_result_free(&rc); cc_cache_metrics_destroy(metrics); return 1; } cc_result_free(&rc);
    rc=cc_event_bus_subscribe(event_bus,CC_OBS_EVENT_CONTEXT_SUMMARY_CACHE,observe_summary_cache,
                              metrics,NULL,&subscription);
    if (rc.code!=CC_OK) { cc_result_free(&rc); cc_event_bus_destroy(event_bus); cc_cache_metrics_destroy(metrics); return 1; }
    cc_result_free(&rc);
    rc=cc_event_bus_subscribe(event_bus,CC_OBS_EVENT_LLM_USAGE,observe_summary_cache,
                              metrics,NULL,&usage_subscription);
    if (rc.code!=CC_OK) {
        cc_result_free(&rc); rc=cc_event_bus_unsubscribe(event_bus,subscription,1000); cc_result_free(&rc);
        cc_event_bus_destroy(event_bus); cc_cache_metrics_destroy(metrics); return 1;
    }
    cc_result_free(&rc);
    if (create_runtime(&state,config,event_bus,&registry,&store,&runtime)) {
        rc=cc_event_bus_unsubscribe(event_bus,subscription,1000); cc_result_free(&rc);
        rc=cc_event_bus_unsubscribe(event_bus,usage_subscription,1000); cc_result_free(&rc);
        cc_event_bus_destroy(event_bus); cc_cache_metrics_destroy(metrics); return 1;
    }
    const char *long_text=
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    rc=store.vtable->create_session(store.self,"prefix", ".");
    int failed=rc.code!=CC_OK; cc_result_free(&rc);
    for (int i=0;i<8 && !failed;i++) {
        char id[16]; snprintf(id,sizeof(id),"p%d",i);
        failed=append_user_message(&store,"prefix",id,long_text);
    }
    if (!failed) failed=expect_context_summary(runtime,"prefix",true) || state.summary_seen!=1 ||
                        state.summary_prefix_seen!=0 || state.summary_history_items!=7;
    if (!failed) failed=append_user_message(&store,"prefix","p8",long_text);
    if (!failed) failed=expect_context_summary(runtime,"prefix",true) || state.summary_seen!=2 ||
                        state.summary_prefix_seen!=1 || state.summary_history_items!=1;
    if (!failed) failed=expect_context_summary(runtime,"prefix",true) || state.summary_seen!=2;

    /* 同会话重写早期正文后，即使候选条数仍能作为前缀，也必须失效。 */
    if (!failed) { rc=store.vtable->clear_session(store.self,"prefix"); failed=rc.code!=CC_OK; cc_result_free(&rc); }
    char altered[800]; snprintf(altered,sizeof(altered),"changed:%s",long_text);
    for (int i=0;i<9 && !failed;i++) {
        char id[16]; snprintf(id,sizeof(id),"p%d",i);
        failed=append_user_message(&store,"prefix",id,i==0 ? altered : long_text);
    }
    if (!failed) failed=expect_context_summary(runtime,"prefix",true) || state.summary_seen!=3 ||
                        state.summary_prefix_seen!=1 || state.summary_history_items!=8;

    /* 空、纯空白及超预算摘要不安装，后续请求可以重新调用 provider。 */
    if (!failed) { rc=store.vtable->create_session(store.self,"blank","."); failed=rc.code!=CC_OK; cc_result_free(&rc); }
    for (int i=0;i<8 && !failed;i++) {
        char id[16]; snprintf(id,sizeof(id),"b%d",i);
        failed=append_user_message(&store,"blank",id,long_text);
    }
    state.summary_usage_enabled=0; state.summary_response=" \t\n";
    if (!failed) failed=expect_context_summary(runtime,"blank",false) || state.summary_seen!=4;
    if (!failed) failed=expect_context_summary(runtime,"blank",false) || state.summary_seen!=5;
    state.summary_usage_enabled=1; state.summary_response="summary-ok";
    if (!failed) failed=expect_context_summary(runtime,"blank",true) || state.summary_seen!=6;
    if (!failed) failed=expect_context_summary(runtime,"blank",true) || state.summary_seen!=6;
    if (!failed) { rc=store.vtable->create_session(store.self,"oversized","."); failed=rc.code!=CC_OK; cc_result_free(&rc); }
    for (int i=0;i<8 && !failed;i++) {
        char id[16]; snprintf(id,sizeof(id),"o%d",i);
        failed=append_user_message(&store,"oversized",id,long_text);
    }
    state.summary_response=long_text;
    if (!failed) failed=expect_context_summary(runtime,"oversized",false) || state.summary_seen!=7;
    state.summary_response="summary-ok";
    if (!failed) failed=expect_context_summary(runtime,"oversized",true) || state.summary_seen!=8;
    if (!failed) failed=expect_context_summary(runtime,"oversized",true) || state.summary_seen!=8;

    if (!failed) { rc=store.vtable->create_session(store.self,"failed-prefix","."); failed=rc.code!=CC_OK; cc_result_free(&rc); }
    for (int i=0;i<8 && !failed;i++) {
        char id[16]; snprintf(id,sizeof(id),"f%d",i);
        failed=append_user_message(&store,"failed-prefix",id,long_text);
    }
    if (!failed) failed=expect_context_summary(runtime,"failed-prefix",true) || state.summary_seen!=9;
    if (!failed) failed=append_user_message(&store,"failed-prefix","f8",long_text);
    state.summary_fail=1;
    if (!failed) failed=expect_context_summary(runtime,"failed-prefix",false) || state.summary_seen!=10;
    state.summary_fail=0; state.summary_usage_enabled=0; state.summary_response=" \t";
    if (!failed) failed=expect_context_summary(runtime,"failed-prefix",false) || state.summary_seen!=11;
    state.summary_usage_enabled=1; state.summary_response="summary-ok";
    if (!failed) failed=expect_context_summary(runtime,"failed-prefix",true) || state.summary_seen!=12;
    if (!failed) failed=expect_context_summary(runtime,"failed-prefix",true) || state.summary_seen!=12;
    if (!failed) {
        rc=cc_event_bus_flush(event_bus,1000); failed=rc.code!=CC_OK; cc_result_free(&rc);
    }
    char *snapshot=NULL; cc_json_value_t *root=NULL;
    if (!failed) { rc=cc_cache_metrics_snapshot(metrics,&snapshot); failed=rc.code!=CC_OK; cc_result_free(&rc); }
    if (!failed) { rc=cc_json_parse(snapshot,&root); failed=rc.code!=CC_OK; cc_result_free(&rc); }
    if (!failed) {
        cc_json_value_t *summary=cc_json_object_get(root,"summary_cache");
        failed=!summary || cc_json_int_value(cc_json_object_get(summary,"hit"))!=4 ||
               cc_json_int_value(cc_json_object_get(summary,"prefix_hit"))!=2 ||
               cc_json_int_value(cc_json_object_get(summary,"miss"))!=9 ||
               cc_json_int_value(cc_json_object_get(summary,"rejected"))!=1 ||
               cc_json_int_value(cc_json_object_get(summary,"model_requests"))!=12 ||
               cc_json_int_value(cc_json_object_get(summary,"usage_reported"))!=9 ||
               cc_json_int_value(cc_json_object_get(summary,"usage_unreported"))!=3;
        cc_json_value_t *models=cc_json_object_get(root,"models");
        cc_json_value_t *model=cc_json_array_get(models,0);
        failed |= cc_json_array_size(models)!=1 || !model ||
                  cc_json_int_value(cc_json_object_get(model,"usage_events"))!=12 ||
                  cc_json_int_value(cc_json_object_get(model,"cache_measured"))!=9 ||
                  cc_json_int_value(cc_json_object_get(model,"cache_unreported"))!=3 ||
                  cc_json_int_value(cc_json_object_get(model,"prompt_tokens"))!=900 ||
                  cc_json_int_value(cc_json_object_get(model,"cached_tokens"))!=0;
    }
    cc_json_destroy(root); free(snapshot);
    destroy_runtime(registry,&store,runtime);
    rc=cc_event_bus_unsubscribe(event_bus,subscription,1000); failed |= rc.code!=CC_OK; cc_result_free(&rc);
    rc=cc_event_bus_unsubscribe(event_bus,usage_subscription,1000); failed |= rc.code!=CC_OK; cc_result_free(&rc);
    cc_event_bus_destroy(event_bus); cc_cache_metrics_destroy(metrics); return failed;
}

/**
 * @brief 核对较新持久摘要优先于旧 RAM 前缀，并拒绝持久超长候选。
 * @return 全部调用数与历史后缀断言通过零，否则一，三个 runtime 在后端销毁前释放。
 * @note store/memory 由函数拥有并供多个 runtime 串行借用，查询副本和超长输入统一回收。
 */
static int test_summary_prefix_source_priority(void)
{
    fake_llm_state_t state={0}; cc_tool_registry_t *registry=NULL;
    cc_session_store_t store={0}; cc_memory_store_t memory={0};
    cc_agent_runtime_t *older=NULL,*newer=NULL,*fresh=NULL;
    cc_result_t rc=cc_tool_registry_create(&registry);
    if (rc.code!=CC_OK) { cc_result_free(&rc); return 1; } cc_result_free(&rc);
    rc=cc_tool_registry_freeze(registry);
    if (rc.code!=CC_OK) { cc_result_free(&rc); cc_tool_registry_destroy(registry); return 1; } cc_result_free(&rc);
    rc=cc_memory_session_store_create(&store);
    if (rc.code!=CC_OK) { cc_result_free(&rc); cc_tool_registry_destroy(registry); return 1; } cc_result_free(&rc);
    rc=cc_memory_store_create_inmem(&memory);
    if (rc.code!=CC_OK) { cc_result_free(&rc); store.vtable->destroy(store.self); cc_tool_registry_destroy(registry); return 1; }
    cc_result_free(&rc);
    cc_agent_runtime_deps_t deps={0}; deps.size=sizeof(deps); deps.llm.self=&state; deps.llm.vtable=&fake_vtable;
    deps.tool_registry=registry; deps.store=store; deps.memory_store=&memory;
    cc_agent_runtime_options_t options={0}; options.size=sizeof(options); options.provider_name="fake";
    options.summary_cache_namespace="https://cache.example"; options.config.max_steps=1;
    options.config.system_prompt="system"; options.config.workspace_dir="."; options.config.model="fake";
    options.config.context_window_tokens=240; options.config.context_compress_threshold=0.8;
    options.config.context_keep_recent=1; options.config.summary_max_tokens=321;
    options.config.summary_temperature=0.15;
    rc=cc_agent_runtime_create(&deps,&options,&older);
    int failed=rc.code!=CC_OK; cc_result_free(&rc);
    if (!failed) { rc=cc_agent_runtime_create(&deps,&options,&newer); failed=rc.code!=CC_OK; cc_result_free(&rc); }
    if (!failed) { rc=store.vtable->create_session(store.self,"source-priority","."); failed=rc.code!=CC_OK; cc_result_free(&rc); }
    const char *text=
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    for (int i=0;i<8 && !failed;i++) {
        char id[16]; snprintf(id,sizeof(id),"s%d",i);
        failed=append_user_message(&store,"source-priority",id,text);
    }
    if (!failed) failed=expect_context_summary(older,"source-priority",true) || state.summary_seen!=1;
    if (!failed) failed=append_user_message(&store,"source-priority","s8",text) ||
                        append_user_message(&store,"source-priority","s9",text);
    if (!failed) failed=expect_context_summary(newer,"source-priority",true) || state.summary_seen!=2 ||
                        state.summary_prefix_seen!=1 || state.summary_history_items!=2;
    if (!failed) failed=append_user_message(&store,"source-priority","s10",text);
    /* older 保留七条 RAM 前缀，九条持久前缀应胜出，只需摘要一条新消息。 */
    if (!failed) failed=expect_context_summary(older,"source-priority",true) || state.summary_seen!=3 ||
                        state.summary_prefix_seen!=2 || state.summary_history_items!=1;
    /* 注入旧实现遗留的超长条目，较短可用持久前驱应继续成为候选。 */
    cc_memory_query_t query={0}; query.size=sizeof(query); query.category="context_summary";
    query.session_id="source-priority"; query.limit=32;
    cc_memory_search_result_t *entries=NULL; size_t entry_count=0; char *large_key=NULL;
    if (!failed) {
        rc=cc_memory_store_query(&memory,&query,&entries,&entry_count);
        failed=rc.code!=CC_OK; cc_result_free(&rc);
    }
    for (size_t i=0;i<entry_count && !large_key;i++) {
        const char *key=entries[i].entry.key;
        const char *suffix=key ? strrchr(key,'_') : NULL;
        if (suffix && strcmp(suffix,"_10")==0) {
            large_key=malloc(strlen(key)+1U);
            if (large_key) strcpy(large_key,key);
        }
    }
    cc_memory_search_result_free_array(entries,entry_count);
    if (!failed && !large_key) failed=1;
    char *huge=NULL;
    if (!failed) {
        huge=malloc(10001U);
        if (!huge) failed=1;
        else { memset(huge,'z',10000U); huge[10000]='\0'; }
    }
    if (!failed) {
        rc=cc_memory_store_set(&memory,large_key,huge,"context_summary","source-priority");
        failed=rc.code!=CC_OK; cc_result_free(&rc);
    }
    free(huge); free(large_key);
    if (!failed) failed=append_user_message(&store,"source-priority","s11",text);
    if (!failed) { rc=cc_agent_runtime_create(&deps,&options,&fresh); failed=rc.code!=CC_OK; cc_result_free(&rc); }
    if (!failed) failed=expect_context_summary(fresh,"source-priority",true) || state.summary_seen!=4 ||
                        state.summary_prefix_seen!=3 || state.summary_history_items!=2;
    if (older) cc_agent_runtime_destroy(older);
    if (newer) cc_agent_runtime_destroy(newer);
    if (fresh) cc_agent_runtime_destroy(fresh);
    cc_memory_store_destroy(&memory); store.vtable->destroy(store.self); cc_tool_registry_destroy(registry);
    return failed;
}

static fake_llm_state_t *builder_test_state;

/**
 * @brief 为 generation 交付借用当前夹具状态的 provider。
 * @param[in] config 未使用配置借用。
 * @param[in] http 未使用 HTTP 借用。
 * @param[out] out 新端口槽，借用静态函数表及 builder_test_state，无 destroy 资源。
 * @return 无动态字段的成功结果，外层保持状态到 builder 销毁。
 */
static cc_result_t builder_test_provider(const cc_config_t *config, cc_http_client_t *http,
                                         cc_llm_provider_t *out)
{
    (void)config; (void)http;
    out->self=builder_test_state; out->vtable=&fake_vtable;
    return cc_result_ok();
}

/**
 * @brief 创建 builder 拥有的内存会话后端。
 * @param[in] config 未使用配置借用。
 * @param[out] out 新端口槽，成功移交后端拥有权。
 * @return 真实创建结果及拥有诊断，由 builder 消费。
 */
static cc_result_t builder_test_sessions(const cc_config_t *config, cc_session_store_t *out)
{ (void)config; return cc_memory_session_store_create(out); }
/**
 * @brief 创建 builder 拥有的内存记忆后端。
 * @param[in] config 未使用配置借用。
 * @param[out] out 新端口槽，成功移交拥有权。
 * @return 真实创建结果及拥有诊断，由 builder 消费。
 */
static cc_result_t builder_test_memory(const cc_config_t *config, cc_memory_store_t *out)
{ (void)config; return cc_memory_store_create_inmem(out); }
/**
 * @brief 交付无私有状态的空策略视图。
 * @param[in] config 未使用配置借用。
 * @param[out] out 必填独占新槽，清零。
 * @return 无拥有字段的成功结果。
 */
static cc_result_t builder_test_policy(const cc_config_t *config, cc_policy_engine_t *out)
{ (void)config; memset(out,0,sizeof(*out)); return cc_result_ok(); }

/**
 * @brief 取得 generation 租约、构造摘要上下文并检查累计 provider 请求数。
 * @param[in,out] builder 存活 builder 借用，函数只临时持有当前 generation。
 * @param[in] expected_calls 全局夹具状态期望的摘要累计调用数。
 * @return 全部断言通过零，否则一，消息/JSON/诊断清理后释放 generation 租约。
 * @note builder_test_state 在外层串行测试设置，覆盖整个调用。
 */
static int builder_test_summary(cc_runtime_builder_t *builder, int expected_calls)
{
    cc_runtime_generation_t *generation=NULL;
    cc_result_t rc=cc_runtime_builder_acquire_generation(builder,&generation);
    if (rc.code!=CC_OK) { cc_result_free(&rc); return 1; }
    cc_result_free(&rc);
    cc_message_t *messages=NULL; size_t count=0;
    rc=cc_context_builder_build_messages(cc_runtime_generation_runtime(generation),
                                        "builder-cache-session","system",NULL,&messages,&count);
    char *json=NULL;
    if (rc.code==CC_OK) { cc_result_free(&rc); rc=cc_messages_to_json(messages,count,1,&json); }
    int failed=rc.code!=CC_OK || !json || !strstr(json,"summary-ok") ||
               builder_test_state->summary_seen!=expected_calls;
    free(json); for (size_t i=0;i<count;i++) cc_message_cleanup(&messages[i]); free(messages);
    cc_result_free(&rc); cc_runtime_generation_release(generation); return failed;
}

/**
 * @brief 经真实 builder 重载验证 endpoint 命名空间和不稳定路径的摘要复用策略。
 * @return 全部摘要调用数断言及销毁成功零，否则一，释放 generation 后才销毁 builder。
 * @note 全局 provider 状态仅在当前测试借用栈对象，退出时清空。
 */
static int test_builder_summary_namespace(void)
{
    fake_llm_state_t state={0}; builder_test_state=&state;
    const cc_llm_provider_descriptor_t providers[]={{"fake",1,builder_test_provider}};
    cc_runtime_feature_set_t features={0};
    features.llm_providers=providers; features.llm_provider_count=1;
    features.create_session_store=builder_test_sessions;
    features.create_memory_store=builder_test_memory; features.create_policy_engine=builder_test_policy;
    cc_config_t config={0}; config.provider="fake"; config.model="model"; config.base_url="https://a.example";
    config.data_dir="."; config.workspace_path="."; config.system_prompt="system";
    config.context_window_tokens=240; config.context_compress_threshold=0.8; config.context_keep_recent=1;
    config.summary_max_tokens=321; config.summary_temperature=0.15;
    cc_runtime_builder_t *builder=NULL;
    cc_result_t rc=cc_runtime_builder_create(&config,&features,&builder);
    if (rc.code!=CC_OK) { cc_result_free(&rc); builder_test_state=NULL; return 1; }
    cc_result_free(&rc);
    cc_runtime_generation_t *generation=NULL;
    rc=cc_runtime_builder_acquire_generation(builder,&generation);
    if (rc.code!=CC_OK) {
        cc_result_free(&rc); rc=cc_runtime_builder_destroy(builder,1000); cc_result_free(&rc);
        builder_test_state=NULL; return 1;
    }
    cc_result_free(&rc);
    cc_session_store_t *store=cc_agent_runtime_session_store(cc_runtime_generation_runtime(generation));
    rc=store->vtable->create_session(store->self,"builder-cache-session",".");
    int failed=rc.code!=CC_OK; cc_result_free(&rc);
    const char *long_text=
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    for (int i=0;i<8 && !failed;i++) {
        char id[16]; snprintf(id,sizeof(id),"builder-%d",i);
        failed=append_user_message(store,"builder-cache-session",id,long_text);
    }
    cc_runtime_generation_release(generation);
    if (!failed) failed=builder_test_summary(builder,1);
    if (!failed) { rc=cc_runtime_builder_reload(builder,&config); failed=rc.code!=CC_OK; cc_result_free(&rc); }
    if (!failed) failed=builder_test_summary(builder,1);
    config.base_url="https://b.example";
    if (!failed) { rc=cc_runtime_builder_reload(builder,&config); failed=rc.code!=CC_OK; cc_result_free(&rc); }
    if (!failed) failed=builder_test_summary(builder,2);
    config.base_url="https://a.example/private-path";
    if (!failed) { rc=cc_runtime_builder_reload(builder,&config); failed=rc.code!=CC_OK; cc_result_free(&rc); }
    if (!failed) failed=builder_test_summary(builder,3);
    if (!failed) { rc=cc_runtime_builder_reload(builder,&config); failed=rc.code!=CC_OK; cc_result_free(&rc); }
    if (!failed) failed=builder_test_summary(builder,4);
    config.base_url="https://a.example";
    if (!failed) { rc=cc_runtime_builder_reload(builder,&config); failed=rc.code!=CC_OK; cc_result_free(&rc); }
    if (!failed) failed=builder_test_summary(builder,4);
    rc=cc_runtime_builder_destroy(builder,1000); failed |= rc.code!=CC_OK; cc_result_free(&rc);
    builder_test_state=NULL; return failed;
}

/**
 * @brief 验证缓冲/实时流请求配置、摘要独立配置、缓存失效和 builder 代际命名空间。
 * @return 所有场景通过零，失败一，成功路径逐组清理答案、上下文、runtime 和依赖。
 * @note provider 与缓存夹具串行运行，事件场景通过 flush 和 unsubscribe 排空借用。
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
    if (create_runtime(&state, request_config, NULL, &registry, &store, &runtime)) return 1;

    if (!succeeded(store.vtable->create_session(store.self, "sync", "."))) failed = 1;
    char *response = NULL;
    cc_result_t rc = cc_agent_runtime_handle_message(runtime, "sync", "hello", &response);
    if (rc.code != CC_OK || !response || strcmp(response, "stream-ok") != 0) failed = 1;
    if (!state.stream_seen || state.stream_max_tokens != 1234 ||
        !double_eq(state.stream_temperature, 0.25)) failed = 1;
    cc_result_free(&rc);
    free(response);

    if (!succeeded(store.vtable->create_session(store.self, "stream", "."))) failed = 1;
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
        /* 保证当前用户输入可进入预算，同时迫使旧历史触发摘要压缩。 */
        .context_window_tokens = 240,
        .context_compress_threshold = 0.8,
        .context_keep_recent = 1,
        .summary_max_tokens = 321,
        .summary_temperature = 0.15
    };
    if (create_runtime(&state, summary_config, NULL, &registry, &store, &runtime)) return 1;

    if (!succeeded(store.vtable->create_session(store.self, "summary", "."))) failed = 1;
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
        cc_result_free(&rc);
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

    if (test_persistent_summary_configuration() || test_builder_summary_namespace() ||
        test_generation_summary_prefix() || test_summary_prefix_source_priority()) failed=1;
    return failed ? 1 : 0;
}
