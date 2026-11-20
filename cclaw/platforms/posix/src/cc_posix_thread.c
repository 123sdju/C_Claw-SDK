#include "cc/ports/cc_thread.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/** @brief 拥有 POSIX 条件变量及创建时实际选定的计时钟。 */
typedef struct cc_posix_cond {
    pthread_cond_t native; /**< 配合调用者 mutex 使用的原生条件变量。 */
    clockid_t clock; /**< MONOTONIC 可用时优先采用，否则保留 REALTIME。 */
} cc_posix_cond_t;

/**
 * @brief 在进程期静态门锁内安装一次递归 mutex，失败允许后续重试。
 * @param[in,out] slot 必需的静态零初始化槽，所有并发初始化均通过本入口。
 * @return 已有或创建成功为 CC_OK，NULL 为 INVALID_ARGUMENT，创建错误原样返回。
 * @note 安装后的锁及槽持续到进程退出，调用者不得并发重置或销毁。
 */
cc_result_t cc_mutex_init_once(cc_mutex_t *slot)
{
    if (!slot) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null mutex slot");
    static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&gate);
    cc_result_t rc = *slot ? cc_result_ok() : cc_mutex_create(slot);
    pthread_mutex_unlock(&gate);
    return rc;
}

/**
 * @brief 创建可 join 的 POSIX 线程及其堆上 pthread_t 包装。
 * @param[in] fn 必需入口，可能在本函数返回前执行，返回指针由本端口忽略。
 * @param[in,out] arg 同步交给新线程的借用上下文，拥有者保持到线程入口结束。
 * @param[out] out_thread 必需空资源槽，基础参数合法后置 NULL，成功交付拥有句柄。
 * @return 成功为 CC_OK，空入口或输出为 INVALID_ARGUMENT，分配失败为 OUT_OF_MEMORY，pthread_create 失败为 PLATFORM。
 * @note 创建失败释放本次包装，成功后由唯一拥有者 join 并结束句柄借用。
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

/**
 * @brief 等待 POSIX 线程退出，成功后消费其堆上 pthread_t 句柄。
 * @param[in,out] thread create 交付且尚未成功 join 的拥有句柄，NULL 返回参数错误。
 * @return 成功为 CC_OK 并释放句柄，pthread_join 失败为 CC_ERR_PLATFORM 并保留句柄供拥有者重试。
 * @note 外层串行化同一线程的 join，并在目标线程之外调用。
 */
cc_result_t cc_thread_join(cc_thread_t thread)
{
    if (!thread) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null thread");
    }
    if (cc_thread_is_current(thread)) {
        return cc_result_error(CC_ERR_PLATFORM, "Cannot join the current thread");
    }

    pthread_t *pthread = (pthread_t *)thread;
    int rc = pthread_join(*pthread, NULL);

    if (rc != 0) {
        return cc_result_error(CC_ERR_PLATFORM, "Failed to join thread");
    }

    free(pthread);
    return cc_result_ok();
}

/**
 * @brief 比较当前 pthread 身份和未成功 join 的目标句柄。
 * @param[in] thread 可选借用句柄，使用期由拥有者排除并发成功 join。
 * @return 同一线程为非零，NULL 或不同线程为零，无资源交接。
 */
int cc_thread_is_current(cc_thread_t thread)
{
    return thread && pthread_equal(pthread_self(), *(pthread_t *)thread);
}

/**
 * @brief 创建堆上递归 pthread mutex。
 * @param[out] out_mutex 必需空资源槽，进入后置 NULL，成功取得拥有句柄供 destroy。
 * @return 成功为 CC_OK，NULL 为 INVALID_ARGUMENT，分配失败为 OUT_OF_MEMORY，属性或 mutex 初始化失败为 PLATFORM。
 * @note 本次属性和失败包装均清理，递归持有次数须配对归还，条件等待时仅持有一次。
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

/**
 * @brief 销毁并释放拥有的 POSIX mutex 包装。
 * @pre 没有持锁者、等待者或正在开始使用该句柄的调用。
 * @param[in,out] mutex 消费拥有句柄，NULL 跳过。
 * @post 句柄及借用失效，原生 destroy 的状态当前无返回通道。
 */
void cc_mutex_destroy(cc_mutex_t mutex)
{
    if (!mutex) return;
    pthread_mutex_destroy((pthread_mutex_t *)mutex);
    free(mutex);
}

/**
 * @brief 阻塞取得 POSIX 递归 mutex。
 * @param[in,out] mutex 存活借用锁，NULL 跳过，同线程重复取得需对应次数解锁。
 * @post 正常返回后增加一次持有，原生错误当前由 void 接口忽略。
 */
void cc_mutex_lock(cc_mutex_t mutex)
{
    if (!mutex) return;
    pthread_mutex_lock((pthread_mutex_t *)mutex);
}

/**
 * @brief 归还当前线程的一次递归持有。
 * @param[in,out] mutex 当前线程持有的存活锁，NULL 跳过。
 * @post 正常解锁后持有深度减一，零深度允许其他线程取得，原生错误未传回。
 */
void cc_mutex_unlock(cc_mutex_t mutex)
{
    if (!mutex) return;
    pthread_mutex_unlock((pthread_mutex_t *)mutex);
}

/**
 * @brief 创建条件变量包装并记录实际使用的计时钟。
 * @param[out] out_cond 必需空资源槽，进入后置 NULL，成功由调用者 destroy。
 * @return 成功为 CC_OK，NULL 为 INVALID_ARGUMENT，分配失败为 OUT_OF_MEMORY，属性或条件变量初始化失败为 PLATFORM。
 * @note 平台声明支持 clock selection 且设置成功时采用 MONOTONIC，其余保留 REALTIME，失败清理本次资源。
 */
cc_result_t cc_cond_create(cc_cond_t *out_cond)
{
    if (!out_cond) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null condition output");
    }

    *out_cond = NULL;

    cc_posix_cond_t *cond = malloc(sizeof(*cond));
    if (!cond) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate condition");
    }

    pthread_condattr_t attr;
    int rc = pthread_condattr_init(&attr);
    if (rc != 0) {
        free(cond);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to initialize condition attributes");
    }
    cond->clock = CLOCK_REALTIME;
#if defined(_POSIX_CLOCK_SELECTION) && _POSIX_CLOCK_SELECTION >= 0 && defined(CLOCK_MONOTONIC)
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0) cond->clock = CLOCK_MONOTONIC;
#endif
    rc = pthread_cond_init(&cond->native, &attr);
    pthread_condattr_destroy(&attr);
    if (rc != 0) {
        free(cond);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to initialize condition");
    }

    *out_cond = cond;
    return cc_result_ok();
}

/**
 * @brief 销毁条件变量及其计时钟包装。
 * @pre 无等待、通知或正在开始使用此句柄的调用。
 * @param[in,out] cond 消费拥有句柄，NULL 跳过。
 * @post 包装及原生条件变量失效，原生 destroy 状态当前无返回通道。
 */
void cc_cond_destroy(cc_cond_t cond)
{
    if (!cond) return;
    pthread_cond_destroy(&((cc_posix_cond_t *)cond)->native);
    free(cond);
}

/**
 * @brief 原子释放一次持有的 mutex 并等待通知，返回时重新持锁。
 * @pre 当前线程恰好持有 mutex 一次，所有同时等待者使用同一谓词锁。
 * @param[in,out] cond 存活借用条件变量，NULL 跳过。
 * @param[in,out] mutex 存活借用锁，NULL 跳过。
 * @post 正常等待返回时重新持锁，可能伪唤醒，调用者循环检查谓词，原生错误未传回。
 */
void cc_cond_wait(cc_cond_t cond, cc_mutex_t mutex)
{
    if (!cond || !mutex) return;
    pthread_cond_wait(&((cc_posix_cond_t *)cond)->native, (pthread_mutex_t *)mutex);
}

/**
 * @brief 以创建时选定的时钟计算一次条件等待预算。
 * @pre 当前线程恰好持有 mutex 一次，返回后仍需在锁内重查谓词。
 * @param[in,out] cond 存活借用条件变量，NULL 返回零。
 * @param[in,out] mutex 存活借用锁，NULL 返回零。
 * @param[in] timeout_ms 毫秒预算，非正数无限等待，正数形成原生绝对截止时间。
 * @return 原生等待成功为一，超时、计时或等待错误为零，正常等待返回时重新持锁。
 * @note REALTIME 回退会受墙钟调整影响，重新取得 mutex 的时间额外取决于持锁者。
 */
int cc_cond_timedwait(cc_cond_t cond, cc_mutex_t mutex, int timeout_ms)
{
    if (!cond || !mutex) return 0;
    cc_posix_cond_t *state = cond;
    if (timeout_ms <= 0) {
        return pthread_cond_wait(&state->native, (pthread_mutex_t *)mutex) == 0;
    }

    struct timespec deadline;
    if (clock_gettime(state->clock, &deadline) != 0) {
        return 0;
    }

    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += deadline.tv_nsec / 1000000000L;
        deadline.tv_nsec %= 1000000000L;
    }

    int rc = pthread_cond_timedwait(
        &state->native,
        (pthread_mutex_t *)mutex,
        &deadline);
    return rc == 0;
}

/**
 * @brief 请求唤醒一个已等待线程。
 * @param[in,out] cond 存活借用条件变量，NULL 跳过，通知不保存为未来许可。
 * @post 原生 signal 已调用，具体唤醒及伪唤醒由 pthread 实现。
 */
void cc_cond_signal(cc_cond_t cond)
{
    if (!cond) return;
    pthread_cond_signal(&((cc_posix_cond_t *)cond)->native);
}

/**
 * @brief 请求唤醒当时全部等待线程。
 * @param[in,out] cond 存活借用条件变量，NULL 跳过。
 * @post 原生 broadcast 已调用，等待者重新争用谓词锁后自行检查状态。
 */
void cc_cond_broadcast(cc_cond_t cond)
{
    if (!cond) return;
    pthread_cond_broadcast(&((cc_posix_cond_t *)cond)->native);
}
