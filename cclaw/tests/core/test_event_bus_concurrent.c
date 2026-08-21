



#include "cc/ports/cc_event_bus.h"
#include "cc/ports/cc_thread.h"

#define THREADS 4
#define LOOPS 200

/* 测试上下文：共享 event bus、计数锁和收到的事件总数。 */
typedef struct {
    cc_event_bus_t *bus;
    cc_mutex_t mutex;
    int count;
} event_ctx_t;

/*
 * 事件处理器。
 *
 * root 事件里再次 publish nested 事件，用来验证 event bus 在 handler 中重入发布不会死锁。
 */
static void handler(const char *event_type, const char *event_json, void *user_data)
{
    (void)event_json;
    event_ctx_t *ctx = (event_ctx_t *)user_data;
    cc_mutex_lock(ctx->mutex);
    ctx->count++;
    cc_mutex_unlock(ctx->mutex);
    if (event_type && event_type[0] == 'r') {
        cc_event_bus_publish(ctx->bus, "nested", "{}");
    }
}

/* 并发发布 root 事件的 worker。 */
static void *publisher(void *arg)
{
    event_ctx_t *ctx = (event_ctx_t *)arg;
    for (int i = 0; i < LOOPS; i++) {
        cc_event_bus_publish(ctx->bus, "root", "{}");
    }
    return NULL;
}

/*
 * 验证同步 event bus 的并发发布和重入发布。
 *
 * 每个 root 事件都会触发一个 nested 事件，所以最终计数应为 THREADS * LOOPS * 2。
 */
int main(void)
{
    event_ctx_t ctx = {0};
    if (cc_event_bus_create(&ctx.bus).code != CC_OK) return 1;
    if (cc_mutex_create(&ctx.mutex).code != CC_OK) return 1;

    cc_event_subscription_t subscription = {0};
    if (cc_event_bus_subscribe(ctx.bus, NULL, handler, &ctx, NULL,
            &subscription).code != CC_OK) return 1;

    cc_thread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++) cc_thread_create(publisher, &ctx, &threads[i]);
    for (int i = 0; i < THREADS; i++) cc_thread_join(threads[i]);


    int ok = ctx.count == THREADS * LOOPS * 2;
    cc_mutex_destroy(ctx.mutex);
    cc_event_bus_destroy(ctx.bus);
    return ok ? 0 : 1;
}
