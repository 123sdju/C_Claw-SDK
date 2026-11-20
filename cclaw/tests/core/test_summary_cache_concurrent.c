#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_context_builder.h"
#include "cc/core/cc_message.h"
#include "cc/ports/cc_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief 创建两个并发工作线程共用的内存会话存储。
 * @param[out] out_store 非 NULL 空接口，成功后由主线程销毁。
 * @return 创建结果，动态诊断由测试消费。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at line %d\n",__LINE__); return 1; } } while (0)

/** @brief 消费结果的动态消息并判定调用是否成功。
 * @param[in] rc 按值取得并释放的调用结果。
 * @return CC_OK 返回 1，其余返回 0。
 */
static int ok(cc_result_t rc) { int good=rc.code==CC_OK; cc_result_free(&rc); return good; }

/** @brief 用提示中的会话标记生成假摘要，并检测跨会话污染。
 * @param[in] self 未使用的 provider 上下文。
 * @param[in] request 同步借用的摘要请求，应只含一条摘要提示。
 * @param[out] out 借用响应，成功时取得由响应对象拥有的摘要文本。
 * @return 请求不符或标记混合返回错误，否则返回响应文本设置结果。
 * @note 两个线程可并行调用，本回调只使用本次调用的局部数据。
 */
static cc_result_t summary_chat(void *self, const cc_llm_chat_request_t *request, cc_llm_response_t *out)
{
    (void)self;
    if (!request || request->message_count!=1)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"摘要请求应只包含一条提示");
    char *prompt=NULL;
    cc_result_t rc=cc_message_get_text_summary(&request->messages[0],&prompt);
    if (rc.code!=CC_OK) return rc;
    cc_result_free(&rc);
    int left=prompt && strstr(prompt,"LEFT_MARKER")!=NULL;
    int right=prompt && strstr(prompt,"RIGHT_MARKER")!=NULL;
    free(prompt);
    if (left==right) return cc_result_error(CC_ERR_INVALID_STATE,"摘要会话标记缺失或混合");
    return cc_llm_response_set_text(out,left ? "LEFT_MARKER summary" : "RIGHT_MARKER summary");
}

static const cc_llm_provider_vtable_t provider={.chat=summary_chat};

typedef struct worker {
    cc_agent_runtime_t *runtime; /**< 借用同一个运行时及其共享摘要缓存。 */
    cc_session_store_t *store; /**< 借用线程安全会话存储。 */
    const char *session; /**< 本线程独占修改的会话标识。 */
    const char *own_marker; /**< 本会话摘要必须包含的静态标记。 */
    const char *other_marker; /**< 其他会话的静态标记，输出中不得出现。 */
    int failed; /**< 仅本工作线程写，主线程 join 后读取。 */
} worker_t;

/** @brief 为本线程会话追加一条带专属标记的长用户消息。
 * @param[in,out] worker 借用线程状态，存储回调复制本次临时消息。
 * @param[in] number 用于生成消息标识的序号。
 * @return 创建及追加成功返回 1，否则返回 0，并消费结果及销毁临时消息。
 */
static int append_message(worker_t *worker, int number)
{
    char id[32], text[720];
    snprintf(id,sizeof(id),"%s-%d",worker->session,number);
    int length=snprintf(text,sizeof(text),"%s:",worker->own_marker);
    if (length<0 || (size_t)length>=sizeof(text)) return 0;
    memset(text+length,'x',sizeof(text)-(size_t)length-1U);
    text[sizeof(text)-1U]='\0';
    cc_message_t *message=NULL;
    cc_result_t rc=cc_message_create_text(id,worker->session,CC_ROLE_USER,text,NULL,&message);
    if (rc.code!=CC_OK) { cc_result_free(&rc); return 0; }
    cc_result_free(&rc);
    cc_session_record_t record={.type=CC_SESSION_RECORD_MESSAGE,.session_id=worker->session,.data.message=message};
    rc=worker->store->vtable->append_records(worker->store->self,&record,1);
    cc_message_destroy(message); return ok(rc);
}

/** @brief 连续构建摘要上下文并追加历史，检查共享缓存中的会话隔离。
 * @param[in,out] argument 非 NULL worker_t 借用指针，须存活至线程被 join。
 * @return 始终返回 NULL，失败通过所属 worker.failed 记录。
 * @post 每轮释放上下文和序列化文本，本线程仅修改自己的会话与失败字段。
 */
static void *build_repeatedly(void *argument)
{
    worker_t *worker=argument;
    for (int i=0;i<24 && !worker->failed;i++) {
        cc_message_t *messages=NULL; size_t count=0; char *json=NULL;
        cc_result_t rc=cc_context_builder_build_messages(worker->runtime,worker->session,"system",NULL,&messages,&count);
        if (rc.code==CC_OK) { cc_result_free(&rc); rc=cc_messages_to_json(messages,count,1,&json); }
        if (rc.code!=CC_OK || !json || !strstr(json,"Earlier conversation summary:") ||
            !strstr(json,worker->own_marker) || strstr(json,worker->other_marker)) worker->failed=1;
        cc_result_free(&rc); free(json);
        for (size_t n=0;n<count;n++) cc_message_cleanup(&messages[n]);
        free(messages);
        if (!worker->failed && !append_message(worker,i+8)) worker->failed=1;
    }
    return NULL;
}

/** @brief 并行运行两会话的摘要读写并在 join 后销毁共享依赖。
 * @return 全部断言通过返回 0，否则返回 1。第二线程创建失败时先等待首线程退出。
 */
int main(void)
{
    cc_tool_registry_t *registry=NULL; cc_session_store_t store={0}; cc_agent_runtime_t *runtime=NULL;
    CHECK(ok(cc_tool_registry_create(&registry)) && ok(cc_tool_registry_freeze(registry)) &&
          ok(cc_memory_session_store_create(&store)) &&
          ok(store.vtable->create_session(store.self,"left",".")) &&
          ok(store.vtable->create_session(store.self,"right",".")));
    cc_agent_runtime_deps_t deps={0}; deps.size=sizeof(deps); deps.llm.vtable=&provider;
    deps.store=store; deps.tool_registry=registry;
    cc_agent_runtime_options_t options={0}; options.size=sizeof(options);
    options.config.max_steps=1; options.config.model="fake"; options.config.system_prompt="system";
    options.config.context_window_tokens=260; options.config.context_compress_threshold=0.8;
    options.config.context_keep_recent=1; options.config.summary_max_tokens=100;
    CHECK(ok(cc_agent_runtime_create(&deps,&options,&runtime)));
    worker_t left={runtime,&store,"left","LEFT_MARKER","RIGHT_MARKER",0};
    worker_t right={runtime,&store,"right","RIGHT_MARKER","LEFT_MARKER",0};
    for (int i=0;i<8;i++) CHECK(append_message(&left,i) && append_message(&right,i));
    cc_thread_t a=NULL,b=NULL;
    CHECK(ok(cc_thread_create(build_repeatedly,&left,&a)));
    cc_result_t rc=cc_thread_create(build_repeatedly,&right,&b);
    if (rc.code!=CC_OK) {
        cc_result_free(&rc);
        rc=cc_thread_join(a);
        cc_result_free(&rc);
        cc_agent_runtime_destroy(runtime); store.vtable->destroy(store.self); cc_tool_registry_destroy(registry);
        return 1;
    }
    cc_result_free(&rc);
    CHECK(ok(cc_thread_join(a)) && ok(cc_thread_join(b)) && !left.failed && !right.failed);
    cc_agent_runtime_destroy(runtime); store.vtable->destroy(store.self); cc_tool_registry_destroy(registry);
    return 0;
}
