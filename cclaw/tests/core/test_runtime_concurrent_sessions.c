



#include "cc/app/cc_agent_runtime.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_thread.h"
#include "cc/internal/cc_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 4


/*
 * 函数 cc_memory_session_store_create：声明 cclaw/tests/core/test_runtime_concurrent_sessions.c 中的 memory session store create 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

/* fake LLM：直接返回文本，避免测试依赖真实 provider。 */
static cc_result_t fake_chat(void *self, const cc_llm_chat_request_t *request, cc_llm_response_t *out)
{
    (void)self;
    memset(out, 0, sizeof(*out));
    out->has_text = 1;
    out->finished = 1;
    out->text = cc_copy_string(request->thinking_mode ? "thinking-ok" : "ok");
    return cc_result_ok();
}

/* fake provider destroy，无私有状态。 */
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

/*
 * 并发 worker。
 *
 * 每个线程创建自己的 session 并执行一次 handle_message，用来验证 runtime/store 的多会话
 * 并发边界。
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

/*
 * 验证 runtime 可以并发处理多个 session。
 *
 * 重点覆盖共享 runtime、共享 tool registry 和内存 session store 在多线程下不会串话或崩溃。
 */
int main(void)
{
    cc_tool_registry_t *registry = NULL;
    cc_session_store_t store;
    cc_agent_runtime_t *runtime = NULL;


    if (cc_tool_registry_create(&registry).code != CC_OK) return 1;
    cc_tool_registry_freeze(registry);
    if (cc_memory_session_store_create(&store).code != CC_OK) return 1;


    cc_llm_provider_t llm = { NULL, &fake_vtable };
    cc_policy_engine_t policy = {0};
    cc_sandbox_t sandbox = {0};
    cc_agent_runtime_config_t config = {
        .max_steps = 2,
        .system_prompt = "system",
        .workspace_dir = ".",
        .model = "fake"
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
    if (cc_agent_runtime_create(&deps, &options, &runtime).code != CC_OK) return 1;


    runtime_ctx_t ctx[THREADS];
    cc_thread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++) {
        ctx[i].runtime = runtime;
        ctx[i].index = i;
        ctx[i].failed = 0;
        cc_thread_create(worker, &ctx[i], &threads[i]);
    }
    for (int i = 0; i < THREADS; i++) cc_thread_join(threads[i]);


    int failed = 0;
    for (int i = 0; i < THREADS; i++) failed |= ctx[i].failed;

    cc_agent_runtime_destroy(runtime);
    store.vtable->destroy(store.self);
    cc_tool_registry_destroy(registry);
    return failed ? 1 : 0;
}
