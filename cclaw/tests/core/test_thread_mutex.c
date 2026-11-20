



#include "cc/ports/cc_thread.h"
#include "cc/ports/cc_platform.h"

#include <stdio.h>
#include <stdlib.h>

#define THREADS 8
#define LOOPS 10000

/*
 * 测试用共享计数器。
 *
 * value 是故意共享的临界资源，mutex 用来验证平台 thread port 的互斥语义。
 */
typedef struct {
    cc_mutex_t mutex;
    int value;
} counter_t;
static counter_t once_counter;

/**
 * @brief 并发取得一次性静态锁并递增进程期计数器。
 * @param[in] arg 测试未使用的借用上下文，允许 NULL。
 * @return 正常循环完成或初始化失败均返回 NULL，主线程通过最终计数检查失败。
 * @note 返回结果消息逐次释放，once_counter.mutex 按 init_once 契约持续到进程退出。
 */
static void *once_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < LOOPS; i++) {
        cc_result_t rc = cc_mutex_init_once(&once_counter.mutex);
        int ready = rc.code == CC_OK;
        cc_result_free(&rc);
        if (!ready) return NULL;
        cc_mutex_lock(once_counter.mutex);
        once_counter.value++;
        cc_mutex_unlock(once_counter.mutex);
    }
    return NULL;
}

/**
 * @brief 重复在显式互斥锁内递增共享计数器。
 * @param[in,out] arg 必填 counter_t，主线程保管至本线程 join，mutex 已创建。
 * @return 循环完成返回 NULL，无独立堆资源。
 */
static void *worker(void *arg)
{
    counter_t *counter = (counter_t *)arg;
    for (int i = 0; i < LOOPS; i++) {
        cc_mutex_lock(counter->mutex);
        counter->value++;
        cc_mutex_unlock(counter->mutex);
    }
    return NULL;
}

/** @brief 自 join 错误回归的借用上下文，两个同步资源由主线程保管。 */
typedef struct {
    cc_mutex_t mutex;
    cc_cond_t cond;
    cc_thread_t thread;
    int ready, attempted, result_code, identity_matches;
} self_join_state_t;

/**
 * @brief 等待自身句柄发布后尝试自 join，并记录保留句柄的错误结果。
 * @param[in,out] arg 必填 self_join_state_t，主线程保管至随后成功 join。
 * @return NULL，自 join 返回的拥有错误消息在本线程清理。
 */
static void *self_join_worker(void *arg)
{
    self_join_state_t *state = arg;
    cc_mutex_lock(state->mutex);
    while (!state->ready) cc_cond_wait(state->cond, state->mutex);
    cc_thread_t thread = state->thread;
    cc_mutex_unlock(state->mutex);
    int identity_matches = cc_thread_is_current(thread);
    cc_result_t rc = cc_thread_join(thread);
    cc_mutex_lock(state->mutex);
    state->identity_matches = identity_matches;
    state->result_code = rc.code;
    state->attempted = 1;
    cc_cond_broadcast(state->cond);
    cc_mutex_unlock(state->mutex);
    cc_result_free(&rc);
    return NULL;
}

/**
 * @brief 检查自 join 失败保留包装句柄，外部拥有者随后仍可正常 join。
 * @return 自身份、PLATFORM 错误和后续成功 join 均满足为一，失败为零。
 * @note 主线程等待自 join 调用返回后才执行外部 join，外部 join 意外失败时终止进程，避免释放仍被线程借用的夹具。
 */
static int test_failed_join_keeps_handle(void)
{
    self_join_state_t state = {0};
    cc_result_t rc = cc_mutex_create(&state.mutex);
    int ok = rc.code == CC_OK;
    cc_result_free(&rc);
    if (!ok) return 0;
    rc = cc_cond_create(&state.cond);
    ok = rc.code == CC_OK;
    cc_result_free(&rc);
    if (!ok) { cc_mutex_destroy(state.mutex); return 0; }
    rc = cc_thread_create(self_join_worker, &state, &state.thread);
    ok = rc.code == CC_OK;
    cc_result_free(&rc);
    if (ok) {
        cc_mutex_lock(state.mutex);
        state.ready = 1;
        cc_cond_broadcast(state.cond);
        while (!state.attempted) cc_cond_wait(state.cond, state.mutex);
        cc_mutex_unlock(state.mutex);
        rc = cc_thread_join(state.thread);
        int joined = rc.code == CC_OK;
        cc_result_free(&rc);
        if (!joined) abort();
        ok = state.result_code == CC_ERR_PLATFORM && state.identity_matches;
    }
    cc_cond_destroy(state.cond); cc_mutex_destroy(state.mutex);
    return ok;
}

/** @brief 条件广播测试拥有的同步资源和锁内谓词。 */
typedef struct condition_state {
    cc_mutex_t mutex;
    cc_cond_t condition;
    cc_cond_t ready;
    int registered;
    int release;
    int notified;
} condition_state_t;

/**
 * @brief 登记就绪后在谓词循环中等待广播，记录放行时的成功通知数。
 * @param[in,out] arg 借用 condition_state_t，主线程保管至 join。
 * @return NULL，所有计数在 mutex 内更新，有限等待使漏广播表现为断言失败。
 */
static void *condition_worker(void *arg)
{
    condition_state_t *state = arg;
    cc_mutex_lock(state->mutex);
    state->registered++;
    cc_cond_signal(state->ready);
    while (!state->release) {
        int signalled = cc_cond_timedwait(state->condition, state->mutex, 2000);
        if (state->release && signalled) state->notified++;
    }
    cc_mutex_unlock(state->mutex);
    return NULL;
}

/**
 * @brief 核对四个已登记等待者在同一广播后各自重新持有谓词锁。
 * @return 全部成功通知且线程收尾完成为一，创建、等待或计数失败为零。
 * @note ready 计数在实际条件登记前更新，但主线程取得同一 mutex 时该等待已释放锁并登记。
 *       join 意外失败时终止进程，避免在借用未排空时销毁同步对象。
 */
static int test_condition_broadcast(void)
{
    enum { WAITERS = 4 };
    condition_state_t state = {0};
    cc_thread_t threads[WAITERS] = {0};
    size_t created = 0;
    cc_result_t rc = cc_mutex_create(&state.mutex);
    int ok = rc.code == CC_OK;
    cc_result_free(&rc);
    if (!ok) return 0;
    rc = cc_cond_create(&state.condition);
    ok = rc.code == CC_OK;
    cc_result_free(&rc);
    if (!ok) goto cleanup;
    rc = cc_cond_create(&state.ready);
    ok = rc.code == CC_OK;
    cc_result_free(&rc);
    if (!ok) goto cleanup;
    for (size_t i = 0; i < WAITERS; i++) {
        rc = cc_thread_create(condition_worker, &state, &threads[i]);
        ok = rc.code == CC_OK;
        cc_result_free(&rc);
        if (!ok) break;
        created++;
    }
    cc_mutex_lock(state.mutex);
    while (ok && state.registered < WAITERS) cc_cond_wait(state.ready, state.mutex);
    state.release = 1;
    cc_cond_broadcast(state.condition);
    cc_mutex_unlock(state.mutex);
    for (size_t i = 0; i < created; i++) {
        rc = cc_thread_join(threads[i]);
        int joined = rc.code == CC_OK;
        cc_result_free(&rc);
        if (!joined) abort();
    }
    ok = ok && state.notified == WAITERS;

cleanup:
    cc_cond_destroy(state.ready);
    cc_cond_destroy(state.condition);
    cc_mutex_destroy(state.mutex);
    return ok;
}

/**
 * @brief 检查各创建入口的 NULL 参数返回，并保留非法线程入口的原输出槽。
 * @return 全部返回 INVALID_ARGUMENT 为一，其他为零，拥有诊断逐次释放。
 */
static int test_invalid_creation(void)
{
    cc_thread_t thread = (cc_thread_t)&thread;
    cc_result_t rc = cc_thread_create(NULL, NULL, &thread);
    int ok = rc.code == CC_ERR_INVALID_ARGUMENT && thread == (cc_thread_t)&thread;
    cc_result_free(&rc);
    rc = cc_thread_create(worker, NULL, NULL);
    ok = ok && rc.code == CC_ERR_INVALID_ARGUMENT;
    cc_result_free(&rc);
    rc = cc_mutex_create(NULL);
    ok = ok && rc.code == CC_ERR_INVALID_ARGUMENT;
    cc_result_free(&rc);
    rc = cc_cond_create(NULL);
    ok = ok && rc.code == CC_ERR_INVALID_ARGUMENT;
    cc_result_free(&rc);
    rc = cc_mutex_init_once(NULL);
    ok = ok && rc.code == CC_ERR_INVALID_ARGUMENT;
    cc_result_free(&rc);
    return ok;
}

/**
 * @brief 检查显式互斥、一次性静态互斥、条件广播与失败 join 的句柄寿命。
 * @return 全部计数和可选自 join 回归通过为零，创建、join 或断言失败为一。
 * @note 正常收尾 join 全部 worker 并释放显式计数锁，静态 once 锁按进程期契约保留。
 */
int main(void)
{
    if (!test_invalid_creation()) return 1;
    counter_t counter = {0};

    if (cc_mutex_create(&counter.mutex).code != CC_OK) return 1;


    cc_thread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++) {
        if (cc_thread_create(worker, &counter, &threads[i]).code != CC_OK) return 1;
    }


    for (int i = 0; i < THREADS; i++) {
        if (cc_thread_join(threads[i]).code != CC_OK) return 1;
    }


    cc_mutex_destroy(counter.mutex);


    if (counter.value != THREADS * LOOPS) {
        fprintf(stderr, "counter=%d\n", counter.value);
        return 1;
    }
    for (int i = 0; i < THREADS; i++)
        if (cc_thread_create(once_worker, NULL, &threads[i]).code != CC_OK) return 1;
    for (int i = 0; i < THREADS; i++)
        if (cc_thread_join(threads[i]).code != CC_OK) return 1;
    if (once_counter.value != THREADS * LOOPS) return 1;
    if (!test_failed_join_keeps_handle()) return 1;
    if (!test_condition_broadcast()) return 1;
    return 0;
}
