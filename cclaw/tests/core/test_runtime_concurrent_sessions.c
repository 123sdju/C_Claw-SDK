



#include "cc/app/cc_agent_runtime.h"
#include "cc/ports/cc_thread.h"
#include "cc/internal/cc_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 4


/**
 * @brief 创建主线程拥有的内存会话库。
 * @param[out] out_store 新端口槽，成功以 vtable.destroy 回收。
 * @return 构造状态及拥有诊断，调用者消费。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

/**
 * @brief 按请求思考模式生成独立文本响应。
 * @param[in] self 未使用状态借用。
 * @param[in] request 同步请求借用，各线程独立请求。
 * @param[out] out 独占新响应槽，文本副本交 runtime 清理。
 * @return 文本构造状态，OOM 传播，无共享可变状态。
 */
static cc_result_t fake_chat(void *self, const cc_llm_chat_request_t *request, cc_llm_response_t *out)
{
    (void)self;
    cc_llm_response_init(out);
    out->finished = 1;
    return cc_llm_response_set_text(out, request->thinking_mode ? "thinking-ok" : "ok");
}

/**
 * @brief 接收无私有资源 provider 的销毁调用。
 * @param[in] self 未使用借用，可 NULL，不释放外部状态。
 * @return 无返回值，无动态对象。
 */
static void fake_destroy(void *self) { (void)self; }

/* fake provider vtable，只需要同步 chat。 */
static cc_llm_provider_vtable_t fake_vtable = {
    fake_chat,
    NULL,
    fake_destroy
};

/* 每个线程独立 session 的执行上下文。 */
typedef struct {
    cc_agent_runtime_t *runtime;
    int index;
    int failed;
} runtime_ctx_t;

/**
 * @brief 在共享 runtime 创建独立会话并核对一次答案。
 * @param[in,out] arg 独占 runtime_ctx_t 借用，runtime 覆盖线程期，failed 仅本线程写。
 * @return NULL，答案与结果诊断在本线程释放，主线程 join 后读取 failed。
 */
static void *worker(void *arg)
{
    runtime_ctx_t *ctx = (runtime_ctx_t *)arg;
    char sid[32];
    char *response = NULL;
    snprintf(sid, sizeof(sid), "ses_%d", ctx->index);
    cc_result_t rc = cc_agent_runtime_create_session(ctx->runtime, sid, ".");
    if (rc.code != CC_OK) ctx->failed = 1;
    cc_result_free(&rc);
    rc = cc_agent_runtime_handle_message(ctx->runtime, sid, "hello", &response);
    if (rc.code != CC_OK || !response || strcmp(response, "ok") != 0) ctx->failed = 1;
    cc_result_free(&rc);
    free(response);
    return NULL;
}

/**
 * @brief 启动四个会话线程，核对每个答案并在排空后销毁共享依赖。
 * @return 全部构造、线程操作及答案检查成功零，否则一，成功/部分启动路径回收已建资源。
 * @note registry 先冻结，线程各有结果槽，runtime/store 保持到已启动线程全部 join。
 */
int main(void)
{
    cc_tool_registry_t *registry = NULL;
    cc_session_store_t store = {0};
    cc_agent_runtime_t *runtime = NULL;


    cc_result_t rc = cc_tool_registry_create(&registry);
    int failed = rc.code != CC_OK;
    cc_result_free(&rc);
    if (failed) return 1;
    rc = cc_tool_registry_freeze(registry);
    failed = rc.code != CC_OK;
    cc_result_free(&rc);
    if (failed) { cc_tool_registry_destroy(registry); return 1; }
    rc = cc_memory_session_store_create(&store);
    failed = rc.code != CC_OK;
    cc_result_free(&rc);
    if (failed) { cc_tool_registry_destroy(registry); return 1; }


    cc_llm_provider_t llm = { NULL, &fake_vtable };
    cc_policy_engine_t policy = {0};
    cc_sandbox_t sandbox = {0};
    cc_agent_runtime_config_t config = {
        .max_steps = 2,
        .system_prompt = "system",
        .workspace_dir = ".",
        .model = "fake",
        .limits = { .size = sizeof(cc_runtime_limits_t), .max_concurrency = THREADS }
    };
    cc_agent_runtime_deps_t deps;
    memset(&deps, 0, sizeof(deps));
    deps.llm = llm;
    deps.tool_registry = registry;
    deps.store = store;
    deps.policy = policy;
    deps.sandbox = sandbox;
    cc_agent_runtime_options_t options;
    memset(&options, 0, sizeof(options));
    options.config = config;
    rc = cc_agent_runtime_create(&deps, &options, &runtime);
    failed = rc.code != CC_OK;
    cc_result_free(&rc);
    if (failed) { store.vtable->destroy(store.self); cc_tool_registry_destroy(registry); return 1; }


    runtime_ctx_t ctx[THREADS];
    cc_thread_t threads[THREADS];
    int started = 0;
    for (int i = 0; i < THREADS; i++) {
        ctx[i].runtime = runtime;
        ctx[i].index = i;
        ctx[i].failed = 0;
        rc = cc_thread_create(worker, &ctx[i], &threads[i]);
        failed = rc.code != CC_OK;
        cc_result_free(&rc);
        if (failed) break;
        started++;
    }
    for (int i = 0; i < started; i++) {
        rc = cc_thread_join(threads[i]);
        int joined = rc.code == CC_OK;
        cc_result_free(&rc);
        if (!joined) return 1;
    }


    for (int i = 0; i < started; i++) failed |= ctx[i].failed;

    cc_agent_runtime_destroy(runtime);
    store.vtable->destroy(store.self);
    cc_tool_registry_destroy(registry);
    return failed ? 1 : 0;
}
