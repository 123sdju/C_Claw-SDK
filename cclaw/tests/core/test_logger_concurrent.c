



#include "cc/ports/cc_logger.h"
#include "cc/ports/cc_thread.h"

#define THREADS 4
#define LOOPS 200

/* 并发日志测试上下文，共享同一个 logger。 */
typedef struct {
    cc_logger_t *logger;
} log_ctx_t;

/**
 * @brief 向共享 logger 连续提交格式化日志。
 * @param[in] arg 存活到 join 的 log_ctx_t 借用，logger 由主线程拥有。
 * @return NULL，无输出拥有对象，logger 内部同步并发格式化和写入。
 */
static void *worker(void *arg)
{
    log_ctx_t *ctx = (log_ctx_t *)arg;
    for (int i = 0; i < LOOPS; i++) {
        cc_logger_log(ctx->logger, CC_LOG_INFO, "thread log %d", i);
    }
    return NULL;
}

/**
 * @brief 创建共享日志器并排空四个线程后销毁，核对线程创建和 join 的实际结果。
 * @return 全部线程建立并退出为零，创建或 join 失败为一，诊断逐次释放。
 * @note 部分线程创建失败仍等待此前已启动线程，logger 仅在全部已启动线程 join 后释放。
 */
int main(void)
{
    cc_logger_t *logger = NULL;

    cc_result_t rc = cc_logger_create("test", CC_LOG_INFO, &logger);
    int failed = rc.code != CC_OK;
    cc_result_free(&rc);
    if (failed) return 1;


    log_ctx_t ctx = { logger };
    cc_thread_t threads[THREADS];
    int started = 0;
    for (; started < THREADS; started++) {
        rc = cc_thread_create(worker, &ctx, &threads[started]);
        failed = rc.code != CC_OK;
        cc_result_free(&rc);
        if (failed) break;
    }
    for (int i = 0; i < started; i++) {
        rc = cc_thread_join(threads[i]);
        int joined = rc.code == CC_OK;
        cc_result_free(&rc);
        if (!joined) return 1;
    }


    cc_logger_destroy(logger);
    return failed ? 1 : 0;
}
