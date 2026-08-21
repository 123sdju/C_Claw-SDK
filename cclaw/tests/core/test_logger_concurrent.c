



#include "cc/ports/cc_logger.h"
#include "cc/ports/cc_thread.h"

#define THREADS 4
#define LOOPS 200

/* 并发日志测试上下文，共享同一个 logger。 */
typedef struct {
    cc_logger_t *logger;
} log_ctx_t;

/* 多线程反复写日志，验证 logger 内部锁和格式化路径不会崩溃。 */
static void *worker(void *arg)
{
    log_ctx_t *ctx = (log_ctx_t *)arg;
    for (int i = 0; i < LOOPS; i++) {
        cc_logger_log(ctx->logger, CC_LOG_INFO, "thread log %d", i);
    }
    return NULL;
}

/* 验证 logger 可被多个线程同时使用并安全销毁。 */
int main(void)
{
    cc_logger_t *logger = NULL;

    if (cc_logger_create("test", CC_LOG_INFO, &logger).code != CC_OK) return 1;


    log_ctx_t ctx = { logger };
    cc_thread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++) cc_thread_create(worker, &ctx, &threads[i]);
    for (int i = 0; i < THREADS; i++) cc_thread_join(threads[i]);


    cc_logger_destroy(logger);
    return 0;
}
