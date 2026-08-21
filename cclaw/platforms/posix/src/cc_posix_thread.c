#include "cc/ports/cc_thread.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

/*
 * 创建 POSIX 线程。
 *
 * public API 中的 cc_thread_t 是不透明句柄；POSIX 实现把 pthread_t 放在堆上，
 * join 后释放。失败时不会写出半初始化句柄。
 */
cc_result_t cc_thread_create(cc_thread_fn_t fn, void *arg, cc_thread_t *out_thread)
{
    if (!fn || !out_thread) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid thread argument");
    }

    *out_thread = NULL;

    pthread_t *thread = malloc(sizeof(*thread));
    if (!thread) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate thread");
    }

    int rc = pthread_create(thread, NULL, fn, arg);
    if (rc != 0) {
        free(thread);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create thread");
    }

    *out_thread = thread;
    return cc_result_ok();
}

/*
 * 等待线程结束并释放线程句柄。
 *
 * 该接口是 join-once 语义：无论 pthread_join 成功或失败，包装对象都会释放。
 */
cc_result_t cc_thread_join(cc_thread_t thread)
{
    if (!thread) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null thread");
    }

    pthread_t *pthread = (pthread_t *)thread;
    int rc = pthread_join(*pthread, NULL);
    free(pthread);

    if (rc != 0) {
        return cc_result_error(CC_ERR_PLATFORM, "Failed to join thread");
    }

    return cc_result_ok();
}

/*
 * 创建递归 mutex。
 *
 * runtime 内部存在同线程重入锁保护对象的路径，因此 POSIX 端使用
 * PTHREAD_MUTEX_RECURSIVE。attr 初始化、settype 或 mutex 初始化失败都会清理资源。
 */
cc_result_t cc_mutex_create(cc_mutex_t *out_mutex)
{
    if (!out_mutex) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null mutex output");
    }

    *out_mutex = NULL;

    pthread_mutex_t *mutex = malloc(sizeof(*mutex));
    if (!mutex) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate mutex");
    }

    pthread_mutexattr_t attr;
    int attr_initialized = 0;
    int rc = pthread_mutexattr_init(&attr);
    if (rc != 0) {
        free(mutex);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to initialize mutex attributes");
    }
    attr_initialized = 1;

    rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (rc == 0) {
        rc = pthread_mutex_init(mutex, &attr);
    }

    if (attr_initialized) {
        pthread_mutexattr_destroy(&attr);
    }

    if (rc != 0) {
        free(mutex);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to initialize mutex");
    }

    *out_mutex = mutex;
    return cc_result_ok();
}

/* 销毁 POSIX mutex；调用方必须保证没有线程仍持有或等待该锁。 */
void cc_mutex_destroy(cc_mutex_t mutex)
{
    if (!mutex) return;
    pthread_mutex_destroy((pthread_mutex_t *)mutex);
    free(mutex);
}

/* 加锁；空 mutex 视为 no-op，便于裁剪 profile 中的防御式调用。 */
void cc_mutex_lock(cc_mutex_t mutex)
{
    if (!mutex) return;
    pthread_mutex_lock((pthread_mutex_t *)mutex);
}

/* 解锁；必须由持锁线程调用。 */
void cc_mutex_unlock(cc_mutex_t mutex)
{
    if (!mutex) return;
    pthread_mutex_unlock((pthread_mutex_t *)mutex);
}

/*
 * 创建条件变量。
 *
 * 条件变量和 mutex 配合用于 run queue、event bus 等等待通知场景。
 */
cc_result_t cc_cond_create(cc_cond_t *out_cond)
{
    if (!out_cond) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null condition output");
    }

    *out_cond = NULL;

    pthread_cond_t *cond = malloc(sizeof(*cond));
    if (!cond) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate condition");
    }

    int rc = pthread_cond_init(cond, NULL);
    if (rc != 0) {
        free(cond);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to initialize condition");
    }

    *out_cond = cond;
    return cc_result_ok();
}

/* 销毁条件变量；调用方必须保证没有线程仍在 wait。 */
void cc_cond_destroy(cc_cond_t cond)
{
    if (!cond) return;
    pthread_cond_destroy((pthread_cond_t *)cond);
    free(cond);
}

/* 无限等待条件变量；调用方进入前必须已经持有 mutex。 */
void cc_cond_wait(cc_cond_t cond, cc_mutex_t mutex)
{
    if (!cond || !mutex) return;
    pthread_cond_wait((pthread_cond_t *)cond, (pthread_mutex_t *)mutex);
}

/*
 * 带超时等待条件变量。
 *
 * timeout_ms <= 0 表示无限等待并返回 1；正数使用 CLOCK_REALTIME 构造绝对 deadline。
 * 返回 0 只表示超时或无法构造 deadline，其它唤醒返回 1。
 */
int cc_cond_timedwait(cc_cond_t cond, cc_mutex_t mutex, int timeout_ms)
{
    if (!cond || !mutex) return 0;
    if (timeout_ms <= 0) {
        pthread_cond_wait((pthread_cond_t *)cond, (pthread_mutex_t *)mutex);
        return 1;
    }

    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return 0;
    }

    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += deadline.tv_nsec / 1000000000L;
        deadline.tv_nsec %= 1000000000L;
    }

    int rc = pthread_cond_timedwait(
        (pthread_cond_t *)cond,
        (pthread_mutex_t *)mutex,
        &deadline);
    return rc != ETIMEDOUT;
}

/* 唤醒一个等待线程。 */
void cc_cond_signal(cc_cond_t cond)
{
    if (!cond) return;
    pthread_cond_signal((pthread_cond_t *)cond);
}

/* 唤醒所有等待线程，用于 shutdown、flush、cancel 等全局状态变化。 */
void cc_cond_broadcast(cc_cond_t cond)
{
    if (!cond) return;
    pthread_cond_broadcast((pthread_cond_t *)cond);
}
