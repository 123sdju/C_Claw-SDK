#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_cache_metrics.h"
#include "cc/app/cc_runtime_builder.h"
#include "cc/core/cc_observability.h"
#include "cc/util/cc_json.h"

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 创建夹具拥有的内存会话端口。
 * @param[out] out_store 新输出槽，成功后由持有者 vtable.destroy 回收。
 * @return 构造状态及拥有诊断，由调用者消费。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "check failed at line %d\n", __LINE__); return 1; } } while (0)
/**
 * @brief 判断成功并消费传入结果。
 * @param[in] rc 按值移交的拥有诊断结果。
 * @return 成功一，失败零，返回前释放动态成员。
 */
static int ok(cc_result_t rc) { int good = rc.code == CC_OK; cc_result_free(&rc); return good; }
typedef struct provider_state { int report; } provider_state_t;
/**
 * @brief 按夹具模式返回使用量及固定答案。
 * @param[in] self 调用期 provider_state_t 借用，主线程在运行调用之间更新模式。
 * @param[in] request 未使用请求借用。
 * @param[out] out 已初始化的独占响应借用，取得答案副本及按值 usage。
 * @return 文本设置结果，拥有诊断交调用者，响应按 provider 契约清理。
 */
static cc_result_t chat(void *self, const cc_llm_chat_request_t *request, cc_llm_response_t *out)
{
    (void)request;
    provider_state_t *state = self;
    if (state->report) {
        out->usage.valid = 1;
        out->usage.prompt_tokens = 100;
        out->usage.cached_tokens_valid = 1;
        out->usage.cached_tokens = 20;
        if (state->report == 2) {
            out->usage.cached_tokens_valid = 0;
            out->usage.cached_tokens_reported = 1;
            out->usage.cache_write_tokens_reported = 1;
        } else if (state->report == 3) out->usage.cached_tokens = 0;
    }
    return cc_llm_response_set_text(out, "done");
}
static const cc_llm_provider_vtable_t provider_vtable = {chat, NULL, NULL, NULL};
static provider_state_t reload_state={1};
/**
 * @brief 为 builder 交付借用常驻状态的 provider 视图。
 * @param[in] config 未使用配置借用。
 * @param[in] http 未使用 HTTP 借用。
 * @param[out] out 必填新端口槽，self 指向常驻 reload_state，无 destroy 责任。
 * @return 成功结果，生成代并发只读取夹具状态。
 */
static cc_result_t create_reload_provider(const cc_config_t *config,cc_http_client_t *http,cc_llm_provider_t *out)
{ (void)config; (void)http; out->self=&reload_state; out->vtable=&provider_vtable; return cc_result_ok(); }
/**
 * @brief 为 generation 创建独立拥有的内存会话库。
 * @param[in] config 未使用配置借用。
 * @param[out] out 新端口槽，成功后 generation 负责 destroy。
 * @return 真实构造结果及拥有诊断，沿调用链消费。
 */
static cc_result_t create_reload_store(const cc_config_t *config,cc_session_store_t *out)
{ (void)config; return cc_memory_session_store_create(out); }
/**
 * @brief 创建无动态资源的空策略视图。
 * @param[in] config 未使用配置借用。
 * @param[out] out 必填独占端口槽，清零后交付。
 * @return 成功结果，无拥有资源。
 */
static cc_result_t create_reload_policy(const cc_config_t *config,cc_policy_engine_t *out)
{ (void)config; memset(out,0,sizeof(*out)); return cc_result_ok(); }
/**
 * @brief 将使用量观测事件送入指标器并消费诊断。
 * @param[in] event_type 同步回调期事件名称借用。
 * @param[in] event_json 同步回调期 JSON 借用。
 * @param[in,out] user_data 指标器借用，保持到 unsubscribe/flush 完成。
 * @return 无返回值，观测诊断在本回调消费。
 * @note 指标器内部同步状态，回调不保留事件文本。
 */
static void on_usage(const char *event_type, const char *event_json, void *user_data)
{
    cc_result_t rc = cc_cache_metrics_observe(user_data, event_type, event_json);
    cc_result_free(&rc);
}

/**
 * @brief 验证旧布局访问、报告零与缺失/无效使用量，以及重载后旧 generation 的 provider 身份。
 * @return 断言通过零，失败一，成功路径 flush/unsubscribe 后释放指标和全部 generation 租约。
 * @note 输入状态覆盖 runtime 调用期，旧代租约保持到迟到使用量事件处理结束。
 */
int main(void)
{
    /* 旧请求只分配原布局长度，新追踪字段查询须尊重 size 边界。 */
    size_t old_size=offsetof(cc_llm_chat_request_t,run_id);
    cc_llm_chat_request_t *old_request=calloc(1,old_size); CHECK(old_request);
    old_request->size=old_size;
    CHECK(!cc_llm_chat_request_run_id(old_request) && !cc_llm_chat_request_origin_turn_id(old_request));
    free(old_request);
    cc_llm_chat_request_t partial={0}; partial.size=offsetof(cc_llm_chat_request_t,origin_turn_id);
    partial.run_id="run-observed";
    CHECK(strcmp(cc_llm_chat_request_run_id(&partial),"run-observed")==0 &&
          !cc_llm_chat_request_origin_turn_id(&partial));

    cc_cache_metrics_t *metrics = NULL;
    CHECK(ok(cc_cache_metrics_create(&metrics)));
    CHECK(ok(cc_cache_metrics_set_provider(metrics, "fake-provider")));
    cc_event_bus_t *bus = NULL;
    CHECK(ok(cc_event_bus_create(&bus)));
    cc_event_subscription_t subscription = {0};
    CHECK(ok(cc_event_bus_subscribe(bus, CC_OBS_EVENT_LLM_USAGE, on_usage, metrics, NULL, &subscription)));
    cc_session_store_t store = {0};
    CHECK(ok(cc_memory_session_store_create(&store)));
    CHECK(ok(store.vtable->create_session(store.self, "s1", ".")));
    cc_tool_registry_t *registry = NULL;
    CHECK(ok(cc_tool_registry_create(&registry)));
    CHECK(ok(cc_tool_registry_freeze(registry)));
    provider_state_t state = {1};
    cc_agent_runtime_deps_t deps = {0};
    deps.store = store;
    deps.tool_registry = registry;
    deps.llm.self = &state;
    deps.llm.vtable = &provider_vtable;
    deps.event_bus = bus;
    cc_agent_runtime_options_t options = {0};
    options.config.model = "fake-model";
    options.config.max_steps = 1;
    cc_agent_runtime_t *runtime = NULL;
    CHECK(ok(cc_agent_runtime_create(&deps, &options, &runtime)));
    char *response = NULL;
    CHECK(ok(cc_agent_runtime_handle_message(runtime, "s1", "first", &response)));
    free(response);
    state.report = 0;
    response = NULL;
    CHECK(ok(cc_agent_runtime_handle_message(runtime, "s1", "second", &response)));
    free(response);
    state.report = 2;
    response = NULL;
    CHECK(ok(cc_agent_runtime_handle_message(runtime, "s1", "invalid cache counts", &response)));
    free(response);
    state.report = 3;
    response = NULL;
    CHECK(ok(cc_agent_runtime_handle_message(runtime, "s1", "reported zero cache count", &response)));
    free(response);
    CHECK(ok(cc_event_bus_flush(bus, 1000)));
    char *snapshot = NULL;
    CHECK(ok(cc_cache_metrics_snapshot(metrics, &snapshot)));
    cc_json_value_t *json = NULL;
    CHECK(ok(cc_json_parse(snapshot, &json)));
    cc_json_value_t *models = cc_json_object_get(json, "models");
    CHECK(cc_json_array_size(models) == 1);
    cc_json_value_t *model = cc_json_array_get(models, 0);
    CHECK(cc_json_int_value(cc_json_object_get(model, "usage_events")) == 4);
    CHECK(cc_json_int_value(cc_json_object_get(model, "cache_measured")) == 2);
    CHECK(cc_json_int_value(cc_json_object_get(model, "cache_unreported")) == 1);
    CHECK(cc_json_int_value(cc_json_object_get(model, "cache_unusable")) == 1);
    CHECK(cc_json_int_value(cc_json_object_get(model, "cache_write_unusable")) == 1);
    CHECK(cc_json_int_value(cc_json_object_get(model, "cache_hit_basis_points")) == 1000);
    cc_json_destroy(json);
    free(snapshot);
    cc_agent_runtime_destroy(runtime);
    CHECK(ok(cc_event_bus_unsubscribe(bus, subscription, 1000)));
    cc_event_bus_destroy(bus);
    cc_tool_registry_destroy(registry);
    store.vtable->destroy(store.self);
    cc_cache_metrics_destroy(metrics);

    /* 重载后旧代租约仍可完成，其使用量保留创建该代时捕获的 provider 身份。 */
    metrics=NULL; CHECK(ok(cc_cache_metrics_create(&metrics)));
    const cc_llm_provider_descriptor_t providers[]={
        {"provider-a",1,create_reload_provider},{"provider-b",1,create_reload_provider}};
    cc_runtime_feature_set_t features={0}; features.llm_providers=providers; features.llm_provider_count=2;
    features.create_session_store=create_reload_store; features.create_policy_engine=create_reload_policy;
    cc_config_t config={0}; config.provider="provider-a"; config.model="same-model";
    config.data_dir="."; config.workspace_path="."; config.max_steps=1;
    cc_runtime_builder_t *builder=NULL; CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)));
    cc_runtime_generation_t *old_generation=NULL,*new_generation=NULL;
    CHECK(ok(cc_runtime_builder_acquire_generation(builder,&old_generation)));
    cc_agent_runtime_t *old_runtime=cc_runtime_generation_runtime(old_generation);
    bus=cc_agent_runtime_event_bus(old_runtime); memset(&subscription,0,sizeof(subscription));
    CHECK(ok(cc_event_bus_subscribe(bus,CC_OBS_EVENT_LLM_USAGE,on_usage,metrics,NULL,&subscription)));
    CHECK(ok(cc_agent_runtime_create_session(old_runtime,"old-generation",".")));
    config.provider="provider-b";
    CHECK(ok(cc_runtime_builder_reload(builder,&config)) && ok(cc_cache_metrics_set_provider(metrics,"provider-b")));
    CHECK(ok(cc_runtime_builder_acquire_generation(builder,&new_generation)));
    cc_agent_runtime_t *new_runtime=cc_runtime_generation_runtime(new_generation);
    CHECK(ok(cc_agent_runtime_create_session(new_runtime,"new-generation",".")));
    response=NULL; CHECK(ok(cc_agent_runtime_handle_message(old_runtime,"old-generation","old input",&response))); free(response);
    response=NULL; CHECK(ok(cc_agent_runtime_handle_message(new_runtime,"new-generation","new input",&response))); free(response);
    CHECK(ok(cc_event_bus_flush(bus,1000)));
    snapshot=NULL; CHECK(ok(cc_cache_metrics_snapshot(metrics,&snapshot)));
    json=NULL; CHECK(ok(cc_json_parse(snapshot,&json))); free(snapshot);
    models=cc_json_object_get(json,"models"); CHECK(cc_json_array_size(models)==2);
    cc_json_value_t *old_model=cc_json_array_get(models,0),*new_model=cc_json_array_get(models,1);
    CHECK(strcmp(cc_json_string_value(cc_json_object_get(old_model,"provider")),"provider-a")==0 &&
          strcmp(cc_json_string_value(cc_json_object_get(new_model,"provider")),"provider-b")==0 &&
          cc_json_int_value(cc_json_object_get(old_model,"usage_events"))==1 &&
          cc_json_int_value(cc_json_object_get(new_model,"usage_events"))==1);
    cc_json_destroy(json); CHECK(ok(cc_event_bus_unsubscribe(bus,subscription,1000)));
    cc_runtime_generation_release(new_generation); cc_runtime_generation_release(old_generation);
    CHECK(ok(cc_runtime_builder_destroy(builder,1000))); cc_cache_metrics_destroy(metrics);
    return 0;
}
