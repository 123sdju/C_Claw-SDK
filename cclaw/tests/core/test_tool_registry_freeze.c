



#include "cc/ports/cc_tool_registry.h"
#include "cc/ports/cc_thread.h"
#include "cc/internal/cc_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 4
#define LOOPS 1000


/**
 * @brief 提供常驻工具名称。
 * @param[in] self 未使用借用，可 NULL。
 * @return 常驻名称借用，无分配，可并发读取。
 */
static const char *dummy_name(void *self) { (void)self; return "dummy"; }

/**
 * @brief 提供常驻工具描述。
 * @param[in] self 未使用借用，可 NULL。
 * @return 常驻描述借用，无分配，可并发读取。
 */
static const char *dummy_desc(void *self) { (void)self; return "Dummy tool"; }

/**
 * @brief 提供最小对象参数 schema。
 * @param[in] self 未使用借用，可 NULL。
 * @return 常驻 schema 借用，无分配，可并发读取。
 */
static const char *dummy_schema(void *self) { (void)self; return "{\"type\":\"object\",\"properties\":{}}"; }

/**
 * @brief 构造拥有空 JSON 文本的工具结果。
 * @param[in] self 未使用借用，可 NULL。
 * @param[in] args 未使用参数借用。
 * @param[in] ctx 未使用上下文借用。
 * @param[out] out 新独占结果槽，文本交调用者 cleanup。
 * @return 文本分配成功返回成功，OOM 返回内存错误，无共享可变状态。
 */
static cc_result_t dummy_call(void *self, const char *args, const cc_tool_context_t *ctx, cc_tool_result_t *out)
{
    (void)self; (void)args; (void)ctx;
    memset(out, 0, sizeof(*out));
    out->ok = 1;
    out->text = cc_copy_string("{}");
    if (!out->text) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate dummy result");
    return cc_result_ok();
}

/* dummy tool vtable，destroy 为空因为没有 self 状态。 */
static cc_tool_vtable_t dummy_vtable = {
    dummy_name, dummy_desc, dummy_schema, dummy_call, NULL
};


/* 并发读取测试上下文。 */
typedef struct {
    cc_tool_registry_t *registry;
    int failed;
} read_ctx_t;

/**
 * @brief 并发反复查工具并生成独立 schema 输出。
 * @param[in,out] arg 独占 read_ctx_t 借用，registry 在 join 前存活且已冻结。
 * @return NULL，输出 schema 和诊断每轮释放，failed 仅本线程写并在 join 后读取。
 */
static void *reader(void *arg)
{
    read_ctx_t *ctx = (read_ctx_t *)arg;
    for (int i = 0; i < LOOPS; i++) {
        cc_tool_t tool;
        char *schema = NULL;
        cc_result_t rc = cc_tool_registry_find(ctx->registry, "dummy", &tool);
        if (rc.code != CC_OK) ctx->failed = 1;
        cc_result_free(&rc);
        rc = cc_tool_registry_build_schema_json(ctx->registry, &schema);
        if (rc.code != CC_OK || !schema) ctx->failed = 1;
        cc_result_free(&rc);
        free(schema);
    }
    return NULL;
}

/**
 * @brief 验证冻结后拒绝新增工具并允许四线程反复读取。
 * @return 构造、冻结、拒绝和线程读断言全部通过零，否则一，排空已启动线程后销毁注册表。
 * @note 工具无 self 资源，常驻函数表覆盖 registry 寿命，线程结果各自独占。
 */
int main(void)
{
    cc_tool_registry_t *registry = NULL;
    cc_result_t rc = cc_tool_registry_create(&registry);
    int failed = rc.code != CC_OK;
    cc_result_free(&rc);
    if (failed) return 1;


    cc_tool_t tool = {0};
    tool.vtable = &dummy_vtable;
    rc = cc_tool_registry_add(registry, tool);
    failed = rc.code != CC_OK;
    cc_result_free(&rc);
    if (failed) { cc_tool_registry_destroy(registry); return 1; }


    rc = cc_tool_registry_freeze(registry);
    failed = rc.code != CC_OK;
    cc_result_free(&rc);
    if (failed || !cc_tool_registry_is_frozen(registry)) { cc_tool_registry_destroy(registry); return 1; }
    cc_result_t rejected = cc_tool_registry_add(registry, tool);
    int rejected_ok = rejected.code != CC_OK;
    cc_result_free(&rejected);
    if (!rejected_ok) { cc_tool_registry_destroy(registry); return 1; }


    read_ctx_t contexts[THREADS] = {0};
    cc_thread_t threads[THREADS];
    int started = 0;
    for (int i = 0; i < THREADS; i++) {
        contexts[i].registry = registry;
        rc = cc_thread_create(reader, &contexts[i], &threads[i]);
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

    cc_tool_registry_destroy(registry);
    for (int i = 0; i < started; i++) failed |= contexts[i].failed;
    return failed ? 1 : 0;
}
