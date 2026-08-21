



#include "cc/ports/cc_tool_registry.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_thread.h"
#include "cc/internal/cc_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 4
#define LOOPS 1000


/* dummy 工具名，用于 registry 查找和 schema 构建测试。 */
static const char *dummy_name(void *self) { (void)self; return "dummy"; }

/* dummy 工具说明。 */
static const char *dummy_desc(void *self) { (void)self; return "Dummy tool"; }

/* dummy 工具 schema，最小 object schema。 */
static const char *dummy_schema(void *self) { (void)self; return "{\"type\":\"object\",\"properties\":{}}"; }

/* dummy 调用函数：返回成功空 JSON，验证 vtable 结构完整。 */
static cc_result_t dummy_call(void *self, const char *args, const cc_tool_context_t *ctx, cc_tool_result_t *out)
{
    (void)self; (void)args; (void)ctx;
    memset(out, 0, sizeof(*out));
    out->ok = 1;
    out->text = cc_copy_string("{}");
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

/* 冻结后多线程反复 find/build schema，验证 registry 读路径线程安全。 */
static void *reader(void *arg)
{
    read_ctx_t *ctx = (read_ctx_t *)arg;
    for (int i = 0; i < LOOPS; i++) {
        cc_tool_t tool;
        char *schema = NULL;
        if (cc_tool_registry_find(ctx->registry, "dummy", &tool).code != CC_OK) ctx->failed = 1;
        if (cc_tool_registry_build_schema_json(ctx->registry, &schema).code != CC_OK || !schema) ctx->failed = 1;
        free(schema);
    }
    return NULL;
}

/*
 * 验证 tool registry freeze 契约。
 *
 * freeze 后 registry 只读，新增工具必须失败；并发读应该稳定成功。
 */
int main(void)
{
    cc_tool_registry_t *registry = NULL;
    if (cc_tool_registry_create(&registry).code != CC_OK) return 1;


    cc_tool_t tool = {0};
    tool.vtable = &dummy_vtable;
    if (cc_tool_registry_add(registry, tool).code != CC_OK) return 1;


    if (cc_tool_registry_freeze(registry).code != CC_OK) return 1;
    if (!cc_tool_registry_is_frozen(registry)) return 1;
    if (cc_tool_registry_add(registry, tool).code == CC_OK) return 1;


    read_ctx_t ctx = { registry, 0 };
    cc_thread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++) cc_thread_create(reader, &ctx, &threads[i]);
    for (int i = 0; i < THREADS; i++) cc_thread_join(threads[i]);

    cc_tool_registry_destroy(registry);
    return ctx.failed ? 1 : 0;
}
