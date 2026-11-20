#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_cache_metrics.h"
#include "cc/core/cc_observability.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 创建夹具拥有的内存会话后端。
 * @param[out] out_store 新端口槽，成功后由主函数 destroy。
 * @return 构造结果及拥有诊断，调用者消费。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)
/**
 * @brief 检查成功并释放结果诊断。
 * @param[in] rc 按值移交的拥有结果。
 * @return 成功一，失败零，无残留动态字段。
 */
static int ok(cc_result_t rc) { int good=rc.code==CC_OK; cc_result_free(&rc); return good; }

typedef struct fake_provider { int fail_after_usage; } fake_provider_t;
/**
 * @brief 同步转发栈上流块到真实 runtime 回调。
 * @param[in] callback 非 NULL 调用地址。
 * @param[in,out] user 回调上下文借用，覆盖本次调用。
 * @param[in] chunk 调用期块借用，接收者不得异步保留。
 * @return 回调拥有结果原样转交，当前层不重复释放。
 */
static cc_result_t emit(cc_llm_stream_callback_fn callback,void *user,cc_stream_chunk_t *chunk)
{ return callback(chunk,user); }
/**
 * @brief 发出增量 usage、文本与结束块，或模拟 usage 后断流。
 * @param[in] self 串行使用的 fake_provider_t 借用，故障开关覆盖流调用。
 * @param[in] request 未使用请求借用。
 * @param[in] callback 同步块接收函数。
 * @param[in,out] user 接收上下文借用，不保留。
 * @return 首个失败或末次回调结果，内部成功诊断消费，栈上块无拥有资源。
 */
static cc_result_t chat_stream(void *self,const cc_llm_chat_request_t *request,
                               cc_llm_stream_callback_fn callback,void *user)
{
    (void)request; fake_provider_t *provider=self;
    cc_stream_chunk_t usage={0}; usage.type=CC_STREAM_CHUNK_USAGE; usage.tool_index=-1;
    usage.usage.valid=1; usage.usage.prompt_tokens=200;
    usage.usage.cached_tokens_valid=1; usage.usage.cached_tokens=120;
    usage.usage.cache_write_tokens_valid=1; usage.usage.cache_write_tokens=30;
    cc_result_t rc=emit(callback,user,&usage); if (rc.code!=CC_OK) return rc;
    cc_result_free(&rc);
    if (provider->fail_after_usage) return cc_result_error(CC_ERR_NETWORK,"injected stream interruption");
    cc_stream_chunk_t text={0}; text.type=CC_STREAM_CHUNK_TEXT; text.tool_index=-1; text.content="done";
    rc=emit(callback,user,&text); if (rc.code!=CC_OK) return rc;
    cc_result_free(&rc);
    memset(&usage,0,sizeof(usage)); usage.type=CC_STREAM_CHUNK_USAGE; usage.tool_index=-1;
    usage.usage.valid=1; usage.usage.completion_tokens=8;
    rc=emit(callback,user,&usage); if (rc.code!=CC_OK) return rc;
    cc_result_free(&rc);
    usage.usage.completion_tokens=25;
    rc=emit(callback,user,&usage); if (rc.code!=CC_OK) return rc;
    cc_result_free(&rc);
    cc_stream_chunk_t finished={0}; finished.type=CC_STREAM_CHUNK_FINISHED; finished.tool_index=-1;
    return emit(callback,user,&finished);
}
static const cc_llm_provider_vtable_t provider_vtable={.chat_stream=chat_stream};

typedef struct observations { cc_cache_metrics_t *metrics; unsigned events; char *last; } observations_t;
/**
 * @brief 复制最新使用量事件并更新真实指标器。
 * @param[in] event_type 回调期间事件名借用。
 * @param[in] event_json 回调期间 JSON 借用，复制后保存。
 * @param[in,out] user_data observations_t 借用，拥有 last 副本，主线程在 flush 后读取。
 * @return 无返回值，副本保留给观察状态拥有者，观测诊断已消费。
 * @note OOM 令 last 为空并由主线程断言发现，观测诊断在回调内消费，unsubscribe 后才释放状态。
 */
static void on_usage(const char *event_type,const char *event_json,void *user_data)
{
    observations_t *observed=user_data; observed->events++;
    free(observed->last); observed->last=cc_copy_string(event_json);
    cc_result_t rc=cc_cache_metrics_observe(observed->metrics,event_type,event_json); cc_result_free(&rc);
}

/**
 * @brief 验证 usage 增量合并、断流计量、provider 名称深拷贝及旧 options 布局。
 * @return 全部断言通过零，失败一，成功路径排空观测并回收 runtime、指标、事件和依赖。
 * @note provider 状态只在同步调用之间改动，事件字段仅在 flush 后读取。
 */
int main(void)
{
    observations_t observed={0}; CHECK(ok(cc_cache_metrics_create(&observed.metrics)));
    CHECK(ok(cc_cache_metrics_set_provider(observed.metrics,"fallback-provider")));
    cc_event_bus_t *bus=NULL; CHECK(ok(cc_event_bus_create(&bus)));
    cc_event_subscription_t subscription={0};
    CHECK(ok(cc_event_bus_subscribe(bus,CC_OBS_EVENT_LLM_USAGE,on_usage,&observed,NULL,&subscription)));
    cc_session_store_t sessions={0}; CHECK(ok(cc_memory_session_store_create(&sessions)));
    CHECK(ok(sessions.vtable->create_session(sessions.self,"stream-session",".")));
    cc_tool_registry_t *registry=NULL; CHECK(ok(cc_tool_registry_create(&registry)) && ok(cc_tool_registry_freeze(registry)));
    fake_provider_t provider={0}; cc_agent_runtime_deps_t deps={0};
    deps.store=sessions; deps.tool_registry=registry; deps.event_bus=bus;
    deps.llm.self=&provider; deps.llm.vtable=&provider_vtable;
    char provider_name[]="anthropic";
    cc_agent_runtime_options_t options={0}; options.size=sizeof(options); options.config.model="fake-anthropic"; options.config.max_steps=1;
    options.provider_name=provider_name;
    cc_agent_runtime_t *runtime=NULL; CHECK(ok(cc_agent_runtime_create(&deps,&options,&runtime)));
    provider_name[0]='X'; /* runtime 持有不可变副本，迟到事件继续使用原 provider 名称。 */
    char *answer=NULL; CHECK(ok(cc_agent_runtime_handle_message(runtime,"stream-session","hello",&answer)));
    CHECK(answer && strcmp(answer,"done")==0); free(answer);
    CHECK(ok(cc_event_bus_flush(bus,1000)) && observed.events==1 && observed.last);
    cc_json_value_t *root=NULL; CHECK(ok(cc_json_parse(observed.last,&root)));
    cc_json_value_t *attrs=cc_json_object_get(root,"attributes");
    CHECK(cc_json_int_value(cc_json_object_get(attrs,"prompt_tokens"))==200 &&
          cc_json_int_value(cc_json_object_get(attrs,"cached_tokens"))==120 &&
          cc_json_int_value(cc_json_object_get(attrs,"cache_write_tokens"))==30 &&
          cc_json_int_value(cc_json_object_get(attrs,"completion_tokens"))==25);
    cc_json_destroy(root);
    char *snapshot=NULL; CHECK(ok(cc_cache_metrics_snapshot(observed.metrics,&snapshot)));
    CHECK(ok(cc_json_parse(snapshot,&root))); free(snapshot);
    cc_json_value_t *model=cc_json_array_get(cc_json_object_get(root,"models"),0);
    CHECK(strcmp(cc_json_string_value(cc_json_object_get(model,"provider")),"anthropic")==0 &&
          cc_json_int_value(cc_json_object_get(model,"usage_events"))==1 &&
          cc_json_int_value(cc_json_object_get(model,"cache_measured"))==1 &&
          cc_json_int_value(cc_json_object_get(model,"cache_write_reported"))==1 &&
          cc_json_int_value(cc_json_object_get(model,"cache_write_tokens"))==30 &&
          cc_json_int_value(cc_json_object_get(model,"cache_hit_basis_points"))==6000);
    cc_json_destroy(root);
    provider.fail_after_usage=1; answer=NULL;
    cc_result_t rc=cc_agent_runtime_handle_message(runtime,"stream-session","retry",&answer);
    CHECK(rc.code==CC_ERR_NETWORK); cc_result_free(&rc); free(answer);
    CHECK(ok(cc_event_bus_flush(bus,1000)) && observed.events==2);
    root=NULL; CHECK(ok(cc_json_parse(observed.last,&root))); attrs=cc_json_object_get(root,"attributes");
    CHECK(cc_json_int_value(cc_json_object_get(attrs,"prompt_tokens"))==200 &&
          cc_json_object_get(attrs,"completion_tokens")==NULL);
    cc_json_destroy(root);
    cc_agent_runtime_destroy(runtime);
    /* 旧 options 布局不得读取尾部 provider_name 字段。 */
    options.size=offsetof(cc_agent_runtime_options_t,provider_name);
    options.provider_name="must-not-appear"; provider.fail_after_usage=0; runtime=NULL;
    CHECK(ok(cc_agent_runtime_create(&deps,&options,&runtime)));
    answer=NULL; CHECK(ok(cc_agent_runtime_handle_message(runtime,"stream-session","legacy",&answer))); free(answer);
    CHECK(ok(cc_event_bus_flush(bus,1000)));
    root=NULL; CHECK(ok(cc_json_parse(observed.last,&root)));
    attrs=cc_json_object_get(root,"attributes"); CHECK(cc_json_object_get(attrs,"provider")==NULL);
    cc_json_destroy(root); cc_agent_runtime_destroy(runtime);
    CHECK(ok(cc_event_bus_unsubscribe(bus,subscription,1000)));
    cc_event_bus_destroy(bus); cc_tool_registry_destroy(registry); sessions.vtable->destroy(sessions.self);
    cc_cache_metrics_destroy(observed.metrics); free(observed.last); return 0;
}
