#include "cc/adapters/cc_llm_providers.h"
#include "cc/app/cc_app_features.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_config.h"
#include "cc/util/cc_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief 单次检查并在失败时报告源码行。 @param[in] x 条件表达式，只求值一次。 */
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)
/** @brief 按值消费拥有结果并报告合成错误。 @param[in,out] rc 待消费结果副本，原调用者不再释放。 @return 成功为一，错误为零。 */
static int ok(cc_result_t rc) { int good=rc.code==CC_OK; if (!good) fprintf(stderr,"%s\n",rc.message ? rc.message : "error"); cc_result_free(&rc); return good; }

/** @brief 同步 HTTP fixture，借用响应并拥有最后请求体副本。 */
typedef struct fake_http { const char *payload; char *last_body; size_t calls; } fake_http_t;
/**
 * @brief 捕获拥有请求副本，并以两段流回调或整体拥有正文返回合成响应。
 * @param[in,out] self 有效 fixture 借用，payload 覆盖本次调用。
 * @param[in] request 有效同步请求借用，嵌套回调在返回前结束。
 * @param[out] response 有效空响应槽，整体正文成功移交给调用者释放。
 * @return 分配或 body 回调结果，捕获请求留给测试最终 free。
 */
static cc_result_t perform(void *self,const cc_http_request_t *request,cc_http_response_t *response)
{
    fake_http_t *fake=self;
    char *body=cc_copy_string(request->body);
    if (!body) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"cannot capture fake request");
    free(fake->last_body); fake->last_body=body; fake->calls++;
    memset(response,0,sizeof(*response)); response->status_code=200;
    if (request->on_body) {
        size_t len=strlen(fake->payload),split=len/2U;
        cc_result_t rc=request->on_body(fake->payload,split,request->user_data);
        if (rc.code != CC_OK) return rc;
        cc_result_free(&rc);
        return request->on_body(fake->payload+split,len-split,request->user_data);
    }
    response->body=cc_copy_string(fake->payload);
    if (!response->body) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"cannot copy fake response");
    response->body_size=strlen(response->body); return cc_result_ok();
}
static const cc_http_client_vtable_t http_vtable={.perform=perform};
/**
 * @brief 从实际发送正文读取顶层缓存模式。
 * @param[in] body 有效捕获文本借用。
 * @return 缺字段为零，五分钟为一，一小时为二，解析或字段错误为负一，临时树归还。
 */
static int cache_mode(const char *body)
{
    cc_json_value_t *root=NULL;
    cc_result_t rc=cc_json_parse_strict(body,&root);
    if (rc.code!=CC_OK) { cc_result_free(&rc); return -1; }
    cc_json_value_t *control=cc_json_object_get(root,"cache_control");
    int result=0;
    if (control) {
        const char *type=cc_json_string_value(cc_json_object_get(control,"type"));
        cc_json_value_t *ttl_value=cc_json_object_get(control,"ttl");
        const char *ttl=cc_json_string_value(ttl_value);
        if (!cc_json_is_object(control) || !type || strcmp(type,"ephemeral")!=0) result=-1;
        else if (!ttl_value || (ttl && strcmp(ttl,"5m")==0)) result=1;
        else if (ttl && strcmp(ttl,"1h")==0) result=2;
        else result=-1;
    }
    cc_json_destroy(root); return result;
}
/**
 * @brief 检查一个对象的缓存断点与 TTL。
 * @param[in] object 调用期间借用对象，缺 cache_control 时返回零。
 * @return 五分钟为一，一小时为二，错误为负一，直接借用源树不分配。
 */
static int marker_mode(const cc_json_value_t *object)
{
    cc_json_value_t *control=cc_json_object_get(object,"cache_control");
    if (!control) return 0;
    const char *type=cc_json_string_value(cc_json_object_get(control,"type"));
    cc_json_value_t *ttl_value=cc_json_object_get(control,"ttl");
    const char *ttl=cc_json_string_value(ttl_value);
    if (!cc_json_is_object(control) || !type || strcmp(type,"ephemeral")!=0) return -1;
    if (!ttl_value || (ttl && strcmp(ttl,"5m")==0)) return 1;
    return ttl && strcmp(ttl,"1h")==0 ? 2 : -1;
}
/**
 * @brief 验证工具末项和 system 的稳定断点模式与内容。
 * @param[in] body 有效捕获请求借用。
 * @param[in] expected_mode 预期零、五分钟一或一小时二。
 * @return 断点和文本均匹配为一，失败为零，解析树归还。
 */
static int stable_breakpoints(const char *body,int expected_mode)
{
    cc_json_value_t *root=NULL;
    cc_result_t rc=cc_json_parse_strict(body,&root);
    if (rc.code!=CC_OK) { cc_result_free(&rc); return 0; }
    cc_json_value_t *system=cc_json_object_get(root,"system");
    cc_json_value_t *tools=cc_json_object_get(root,"tools");
    cc_json_value_t *first=cc_json_array_get(tools,0);
    cc_json_value_t *last=cc_json_array_get(tools,1);
    const char *system_text=expected_mode ?
        cc_json_string_value(cc_json_object_get(cc_json_array_get(system,0),"text")) :
        cc_json_string_value(system);
    int good=system_text && strcmp(system_text,"stable system instructions")==0 &&
        cc_json_array_size(tools)==2 && marker_mode(first)==0 && marker_mode(last)==expected_mode &&
        (!expected_mode || (cc_json_array_size(system)==1 &&
            marker_mode(cc_json_array_get(system,0))==expected_mode));
    cc_json_destroy(root); return good;
}
/** @brief 独占 usage 值快照，容量四项，并记录最终完成次数。 */
typedef struct observations { cc_llm_usage_t usage[4]; size_t count; int finished; } observations_t;
/**
 * @brief 复制至多四个 usage 值并计数完成块。
 * @param[in] chunk 有效同步块借用，字符串不跨回调保存。
 * @param[in,out] user_data 有效独占观测槽。
 * @return 无拥有消息的 CC_OK。
 */
static cc_result_t observe(const cc_stream_chunk_t *chunk,void *user_data)
{
    observations_t *out=user_data;
    if (chunk->type==CC_STREAM_CHUNK_USAGE && out->count<4U) out->usage[out->count++]=chunk->usage;
    if (chunk->type==CC_STREAM_CHUNK_FINISHED) out->finished++;
    return cc_result_ok();
}

/**
 * @brief 顺序验证缓存默认/覆盖、稳定断点、reported/valid 和流式增量用量。
 * @return 通过返回零，失败由 CHECK 返回一，成功路径归还消息、响应、provider 及捕获请求。
 */
int main(void)
{
    fake_http_t fake={0}; cc_http_client_t http={&fake,&http_vtable,sizeof(http),CC_HTTP_CAP_STREAM_BODY};
    cc_llm_provider_t provider={0}; CHECK(ok(cc_anthropic_provider_create(&http,NULL,"test-key","test-model",&provider)));
    cc_llm_chat_request_t request={0}; request.size=sizeof(request); request.max_tokens=100;
    cc_message_t *system_message=NULL, *user_message=NULL;
    CHECK(ok(cc_message_create_text("system","s",CC_ROLE_SYSTEM,"stable system instructions",NULL,&system_message)));
    CHECK(ok(cc_message_create_text("user","s",CC_ROLE_USER,"changing user message",NULL,&user_message)));
    cc_message_t messages[2]={*system_message,*user_message};
    request.messages=messages; request.message_count=2;
    request.tools_json="[{\"type\":\"function\",\"function\":{\"name\":\"first\",\"description\":\"a\",\"parameters\":{\"type\":\"object\"}}},"
        "{\"type\":\"function\",\"function\":{\"name\":\"second\",\"description\":\"b\",\"parameters\":{\"type\":\"object\"}}}]";
    cc_llm_response_t response={0};
    fake.payload="{\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],\"stop_reason\":\"end_turn\","
        "\"usage\":{\"input_tokens\":50,\"cache_read_input_tokens\":120,\"cache_creation_input_tokens\":30,\"output_tokens\":25}}";
    CHECK(ok(provider.vtable->chat(provider.self,&request,&response)) && cache_mode(fake.last_body)==1 &&
          stable_breakpoints(fake.last_body,1) && response.usage.valid &&
          response.usage.prompt_tokens==200 && response.usage.cached_tokens_valid && response.usage.cached_tokens==120 &&
          response.usage.cache_write_tokens_valid && response.usage.cache_write_tokens==30 && response.usage.completion_tokens==25);
    cc_llm_response_free(&response);
    request.extra_body_json="{\"cache_control\":null}";
    fake.payload="{\"content\":[],\"usage\":{\"input_tokens\":50,\"cache_read_input_tokens\":120,\"output_tokens\":25}}";
    memset(&response,0,sizeof(response));
    CHECK(ok(provider.vtable->chat(provider.self,&request,&response)) && cache_mode(fake.last_body)==0 &&
          stable_breakpoints(fake.last_body,0) && response.usage.valid &&
          response.usage.prompt_tokens==0 && response.usage.cached_tokens_valid &&
          !response.usage.cache_write_tokens_valid && response.usage.completion_tokens==25);
    cc_llm_response_free(&response);
    request.extra_body_json=NULL;
    fake.payload="{\"content\":[],\"usage\":{\"input_tokens\":50,\"cache_read_input_tokens\":-1,"
        "\"cache_creation_input_tokens\":30,\"output_tokens\":2.5}}";
    memset(&response,0,sizeof(response));
    CHECK(ok(provider.vtable->chat(provider.self,&request,&response)) && response.usage.valid &&
          response.usage.prompt_tokens==0 && !response.usage.cached_tokens_valid &&
          response.usage.cache_write_tokens_valid && response.usage.completion_tokens==0);
    cc_llm_response_free(&response);
    fake.payload="{\"content\":[],\"usage\":{\"input_tokens\":0,\"cache_read_input_tokens\":0,\"cache_creation_input_tokens\":0,\"output_tokens\":0}}";
    CHECK(ok(provider.vtable->chat(provider.self,&request,&response)) && response.usage.valid &&
          response.usage.cached_tokens_reported && response.usage.cached_tokens_valid && !response.usage.cached_tokens &&
          response.usage.cache_write_tokens_reported && response.usage.cache_write_tokens_valid && !response.usage.cache_write_tokens);
    cc_llm_response_free(&response);
    fake.payload="{\"content\":[],\"usage\":{}}";
    CHECK(ok(provider.vtable->chat(provider.self,&request,&response)) && !response.usage.valid &&
          !response.usage.cached_tokens_reported && !response.usage.cache_write_tokens_reported);
    cc_llm_response_free(&response);
    fake.payload="data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":50,"
        "\"cache_read_input_tokens\":120,\"cache_creation_input_tokens\":30,\"output_tokens\":1}}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"ok\"}}\n\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":null},\"usage\":{\"output_tokens\":8}}\n\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":25}}\n\n";
    observations_t observed={0}; request.stream=1;
    CHECK(ok(provider.vtable->chat_stream(provider.self,&request,observe,&observed)) &&
          cache_mode(fake.last_body)==1 && stable_breakpoints(fake.last_body,1) && observed.count==3 && observed.finished==1);
    CHECK(observed.usage[0].prompt_tokens==200 && observed.usage[0].cached_tokens==120 &&
          observed.usage[0].cache_write_tokens==30 && observed.usage[0].completion_tokens==0 &&
          observed.usage[1].completion_tokens==8 && observed.usage[2].completion_tokens==25);
    provider.vtable->destroy(provider.self);

    fake.payload="{\"content\":[],\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}";
    memset(&provider,0,sizeof(provider));
    CHECK(ok(cc_anthropic_provider_create_configured(&http,NULL,"test-key","test-model",
        "{\"cache_control\":null}",&provider)));
    request.stream=0; request.extra_body_json=NULL;
    memset(&response,0,sizeof(response));
    CHECK(ok(provider.vtable->chat(provider.self,&request,&response)) && cache_mode(fake.last_body)==0 &&
          stable_breakpoints(fake.last_body,0));
    cc_llm_response_free(&response);
    /* 实例默认禁用后，请求可显式重新启用五分钟缓存。 */
    request.extra_body_json="{\"cache_control\":{\"type\":\"ephemeral\"}}";
    memset(&response,0,sizeof(response));
    CHECK(ok(provider.vtable->chat(provider.self,&request,&response)) && cache_mode(fake.last_body)==1 &&
          stable_breakpoints(fake.last_body,1));
    cc_llm_response_free(&response);
    provider.vtable->destroy(provider.self);

    /* 通过默认 feature 工厂创建实例，核对无请求级 extra body 的摘要/记忆式请求也沿实例模式。 */
    const cc_runtime_feature_set_t *features=cc_app_default_features();
    const cc_llm_provider_descriptor_t *anthropic=NULL;
    for (size_t i=0;features && i<features->llm_provider_count;i++)
        if (features->llm_providers[i].name && strcmp(features->llm_providers[i].name,"anthropic")==0)
            anthropic=&features->llm_providers[i];
    CHECK(anthropic && anthropic->compiled && anthropic->create);
    cc_config_t config={0}; config.model="test-model"; config.api_key="test-key";
    config.model_extra_body_json="{\"cache_control\":null}";
    memset(&provider,0,sizeof(provider)); CHECK(ok(anthropic->create(&config,&http,&provider)));
    request.extra_body_json=NULL;
    memset(&response,0,sizeof(response));
    CHECK(ok(provider.vtable->chat(provider.self,&request,&response)) && cache_mode(fake.last_body)==0 &&
          stable_breakpoints(fake.last_body,0));
    cc_llm_response_free(&response); provider.vtable->destroy(provider.self);

    config.model_extra_body_json="{\"cache_control\":{\"type\":\"ephemeral\",\"ttl\":\"1h\"}}";
    memset(&provider,0,sizeof(provider)); CHECK(ok(anthropic->create(&config,&http,&provider)));
    memset(&response,0,sizeof(response));
    CHECK(ok(provider.vtable->chat(provider.self,&request,&response)) && cache_mode(fake.last_body)==2 &&
          stable_breakpoints(fake.last_body,2));
    cc_llm_response_free(&response);
    size_t calls_before=fake.calls;
    request.extra_body_json="{\"cache_control\":{\"type\":\"ephemeral\",\"ttl\":\"bad\"}}";
    cc_result_t rc=provider.vtable->chat(provider.self,&request,&response);
    CHECK(rc.code==CC_ERR_INVALID_ARGUMENT && fake.calls==calls_before); cc_result_free(&rc);
    provider.vtable->destroy(provider.self);
    config.model_extra_body_json="{\"cache_control\":null,\"cache_control\":{\"type\":\"ephemeral\"}}";
    memset(&provider,0,sizeof(provider)); rc=anthropic->create(&config,&http,&provider);
    CHECK(rc.code==CC_ERR_INVALID_ARGUMENT && !provider.self && fake.calls==calls_before); cc_result_free(&rc);
    cc_message_destroy(system_message); cc_message_destroy(user_message);
    free(fake.last_body); return 0;
}
