



#include "cc/ports/cc_event_bus.h"
#include "cc/ports/cc_thread.h"
#include "cc/ports/cc_platform.h"

#define THREADS 4
#define LOOPS 200

/* 测试上下文：共享 event bus、计数锁和收到的事件总数。 */
typedef struct {
    cc_event_bus_t *bus;
    cc_mutex_t mutex;
    int count;
    cc_cond_t cond;
    int entered;
    int release;
} event_ctx_t;

/**
 * @brief 计数每次同步回调，并为 root 事件嵌套发布 nested。
 * @param[in] event_type 回调期间借用主题，首字节 r 触发嵌套。
 * @param[in] event_json 回调期间借用正文，此测试忽略。
 * @param[in,out] user_data 必填 event_ctx_t，总线与计数锁覆盖所有发布线程。
 * @post 计数在上下文锁内增加，嵌套发布位于该锁外。
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

/**
 * @brief 在线程内重复发布 root 事件，配合同步嵌套回调检查并发计数。
 * @param[in,out] arg 必填借用 event_ctx_t，主线程保管至 join。
 * @return 固定返回 NULL，主线程通过总计数检查全部发布。
 */
static void *publisher(void *arg)
{
    event_ctx_t *ctx = (event_ctx_t *)arg;
    for (int i = 0; i < LOOPS; i++) {
        cc_event_bus_publish(ctx->bus, "root", "{}");
    }
    return NULL;
}

/**
 * @brief 登记回调已开始，并保持在途直到主线程释放屏障。
 * @param[in] type 回调借用主题，此测试忽略。
 * @param[in] json 回调借用正文，此测试忽略。
 * @param[in,out] arg 必填 event_ctx_t，其 mutex/cond 覆盖回调退出。
 * @post entered 置一，返回时已观察到 release。
 */
static void blocking_handler(const char *type, const char *json, void *arg)
{
    (void)type;
    (void)json;
    event_ctx_t *ctx = arg;
    cc_mutex_lock(ctx->mutex);
    ctx->entered = 1;
    cc_cond_broadcast(ctx->cond);
    while (!ctx->release) cc_cond_wait(ctx->cond, ctx->mutex);
    cc_mutex_unlock(ctx->mutex);
}

/**
 * @brief 发布一个 blocked 事件，允许主线程观察回调仍在途。
 * @param[in,out] arg 必填借用 event_ctx_t，覆盖线程 join。
 * @return NULL，发布结果消息在本线程释放。
 */
static void *publish_once(void *arg)
{
    event_ctx_t *ctx = arg;
    cc_result_t rc = cc_event_bus_publish(ctx->bus, "blocked", "{}");
    cc_result_free(&rc);
    return NULL;
}

/**
 * @brief 检查同步回调未退出时 shutdown 超时，释放回调后可再次排空。
 * @return 两次关闭及发布线程 join 满足预期为一，失败为零，正常收尾销毁上下文。
 */
static int test_shutdown_waits_for_sync_callback(void)
{
    event_ctx_t ctx = {0};
    if (cc_event_bus_create(&ctx.bus).code != CC_OK ||
        cc_mutex_create(&ctx.mutex).code != CC_OK ||
        cc_cond_create(&ctx.cond).code != CC_OK) return 0;
    cc_event_subscription_t subscription = {0};
    if (cc_event_bus_subscribe(ctx.bus, "blocked", blocking_handler, &ctx, NULL,
                              &subscription).code != CC_OK) return 0;
    cc_thread_t worker;
    if (cc_thread_create(publish_once, &ctx, &worker).code != CC_OK) return 0;
    cc_mutex_lock(ctx.mutex);
    while (!ctx.entered) cc_cond_wait(ctx.cond, ctx.mutex);
    cc_mutex_unlock(ctx.mutex);
    cc_result_t rc = cc_event_bus_shutdown(ctx.bus, 1);
    int ok = rc.code == CC_ERR_TIMEOUT;
    cc_result_free(&rc);
    cc_mutex_lock(ctx.mutex);
    ctx.release = 1;
    cc_cond_broadcast(ctx.cond);
    cc_mutex_unlock(ctx.mutex);
    rc = cc_thread_join(worker);
    ok = ok && rc.code == CC_OK;
    cc_result_free(&rc);
    rc = cc_event_bus_shutdown(ctx.bus, 1000);
    ok = ok && rc.code == CC_OK;
    cc_result_free(&rc);
    cc_event_bus_destroy(ctx.bus);
    cc_cond_destroy(ctx.cond);
    cc_mutex_destroy(ctx.mutex);
    return ok;
}

#ifdef CC_TEST_EVENT_LIFETIME_BARRIERS
enum { ROLE_NONE, ROLE_UNSUBSCRIBE, ROLE_FLUSH, ROLE_SHUTDOWN, ROLE_DESTROY, ROLE_PUBLISH };

/** @brief Linux 链接屏障，只控制当前测试创建的等待线程和 worker join 调用。 */
typedef struct {
    cc_event_bus_t *bus;
    cc_event_subscription_t subscription;
    cc_mutex_t mutex;
    cc_cond_t cond;
    int role, paused, release, api_code;
    int destroy_waiting, destroy_done;
    int join_pause, join_entered, join_release, join_failures, join_attempts;
    int fake_clock, fake_waits, wait_budgets[2];
    uint64_t fake_ms;
} lifetime_barrier_t;

static lifetime_barrier_t *g_lifetime_barrier;
static _Thread_local int g_event_role;
static _Thread_local int g_wait_paused;

extern int __real_cc_cond_timedwait(cc_cond_t cond, cc_mutex_t mutex, int timeout_ms);
extern cc_result_t __real_cc_thread_join(cc_thread_t thread);
extern uint64_t __real_cc_platform_monotonic_ms(void);

/**
 * @brief 仅为预算用例的专用发布线程提供可控单调时钟。
 * @return 显式启用时返回 fixture 毫秒值，其余线程与场景使用真实平台时钟。
 */
uint64_t __wrap_cc_platform_monotonic_ms(void)
{
    lifetime_barrier_t *barrier = g_lifetime_barrier;
    if (barrier && barrier->fake_clock && g_event_role == ROLE_PUBLISH) return barrier->fake_ms;
    return __real_cc_platform_monotonic_ms();
}

/**
 * @brief 在生产 wait 返回后临时放开总线锁，保留已登记调用并强制目标交错。
 * @param[in] cond 原生产条件变量，fixture 自身等待直接透传。
 * @param[in] mutex 原生产已持有锁，暂停前解锁，返回前恢复持有。
 * @param[in] timeout_ms 原生产毫秒预算，专用发布屏障用一毫秒等待制造提前唤醒，其余透传。
 * @return 真实端口等待结果，屏障仅影响调度。
 */
int __wrap_cc_cond_timedwait(cc_cond_t cond, cc_mutex_t mutex, int timeout_ms)
{
    lifetime_barrier_t *barrier = g_lifetime_barrier;
    if (barrier && barrier->fake_clock && g_event_role == ROLE_PUBLISH && cond != barrier->cond) {
        int index = barrier->fake_waits++;
        if (index < 2) barrier->wait_budgets[index] = timeout_ms;
        barrier->fake_ms += index == 0 ? 90U : 10U;
        return 0;
    }
    if (barrier && cond != barrier->cond && g_event_role == ROLE_DESTROY) {
        cc_mutex_lock(barrier->mutex);
        barrier->destroy_waiting = 1;
        cc_cond_broadcast(barrier->cond);
        cc_mutex_unlock(barrier->mutex);
    }
    int wait_budget = barrier && g_event_role == ROLE_PUBLISH && !g_wait_paused ? 1 : timeout_ms;
    int result = __real_cc_cond_timedwait(cond, mutex, wait_budget);
    if (barrier && cond != barrier->cond && g_event_role == barrier->role &&
        !g_wait_paused && (g_event_role == ROLE_UNSUBSCRIBE || g_event_role == ROLE_FLUSH ||
                          g_event_role == ROLE_PUBLISH)) {
        g_wait_paused = 1;
        cc_mutex_unlock(mutex);
        cc_mutex_lock(barrier->mutex);
        barrier->paused = 1;
        cc_cond_broadcast(barrier->cond);
        while (!barrier->release) cc_cond_wait(barrier->cond, barrier->mutex);
        cc_mutex_unlock(barrier->mutex);
        cc_mutex_lock(mutex);
    }
    return result;
}

/**
 * @brief 对当前总线的 shutdown 线程注入一次 join 错误或 join 拥有者暂停。
 * @param[in] thread 真实生产 worker 句柄，故障分支保持借用以供下一次 join 重试。
 * @return 故障为拥有消息的 PLATFORM 错误，其余透传真实 join 的消费契约。
 */
cc_result_t __wrap_cc_thread_join(cc_thread_t thread)
{
    lifetime_barrier_t *barrier = g_lifetime_barrier;
    if (barrier && (g_event_role == ROLE_SHUTDOWN || g_event_role == ROLE_DESTROY)) {
        cc_mutex_lock(barrier->mutex);
        barrier->join_attempts++;
        if (barrier->join_failures > 0) {
            barrier->join_failures--;
            cc_mutex_unlock(barrier->mutex);
            return cc_result_error(CC_ERR_PLATFORM, "injected worker join failure");
        }
        if (barrier->join_pause && g_event_role == ROLE_SHUTDOWN) {
            barrier->join_entered = 1;
            cc_cond_broadcast(barrier->cond);
            while (!barrier->join_release) cc_cond_wait(barrier->cond, barrier->mutex);
        }
        cc_mutex_unlock(barrier->mutex);
    }
    return __real_cc_thread_join(thread);
}

/**
 * @brief 初始化当前用例的屏障锁和条件变量。
 * @param[out] barrier 空资源输出，进入时清零，成功后调用方销毁两个同步资源。
 * @return 成功为一，失败清理已建资源并返回零。
 */
static int lifetime_barrier_init(lifetime_barrier_t *barrier)
{
    *barrier = (lifetime_barrier_t){0};
    cc_result_t rc = cc_mutex_create(&barrier->mutex);
    int ok = rc.code == CC_OK;
    cc_result_free(&rc);
    if (!ok) return 0;
    rc = cc_cond_create(&barrier->cond);
    ok = rc.code == CC_OK;
    cc_result_free(&rc);
    if (!ok) cc_mutex_destroy(barrier->mutex);
    return ok;
}

/**
 * @brief 在受控线程执行一个已登记等待 API，并把返回码交回主线程。
 * @param[in,out] arg 借用屏障，覆盖线程 join，role 决定退订、flush 或 shutdown。
 * @return NULL，拥有的返回消息在本线程释放。
 */
static void *lifecycle_waiter_thread(void *arg)
{
    lifetime_barrier_t *barrier = arg;
    g_event_role = barrier->role;
    cc_result_t rc;
    if (barrier->role == ROLE_UNSUBSCRIBE)
        rc = cc_event_bus_unsubscribe(barrier->bus, barrier->subscription, 3000);
    else if (barrier->role == ROLE_FLUSH)
        rc = cc_event_bus_flush(barrier->bus, 3000);
    else if (barrier->role == ROLE_PUBLISH)
        rc = cc_event_bus_publish(barrier->bus, "blocked", "third");
    else
        rc = cc_event_bus_shutdown(barrier->bus, 3000);
    cc_mutex_lock(barrier->mutex);
    barrier->api_code = rc.code;
    cc_cond_broadcast(barrier->cond);
    cc_mutex_unlock(barrier->mutex);
    cc_result_free(&rc);
    return NULL;
}

/**
 * @brief 在唯一 owner 线程销毁总线，并通知测试观察是否等待登记调用。
 * @param[in,out] arg 借用屏障，覆盖线程 join，bus 只在本线程消费。
 * @return NULL，返回前在屏障锁内记录 destroy_done。
 */
static void *lifecycle_destroy_thread(void *arg)
{
    lifetime_barrier_t *barrier = arg;
    g_event_role = ROLE_DESTROY;
    cc_event_bus_destroy(barrier->bus);
    cc_mutex_lock(barrier->mutex);
    barrier->destroy_done = 1;
    cc_cond_broadcast(barrier->cond);
    cc_mutex_unlock(barrier->mutex);
    return NULL;
}

/**
 * @brief 提供不保留正文的替换订阅，供槽位代际循环使用。
 * @param[in] type 回调借用主题，此测试忽略。
 * @param[in] json 回调借用正文，此测试忽略。
 * @param[in] arg 可选借用上下文，此测试忽略。
 * @post 无状态变化。
 */
static void lifetime_noop_handler(const char *type, const char *json, void *arg)
{
    (void)type; (void)json; (void)arg;
}

/**
 * @brief 记录后续订阅投递次数并通知夹具，不保留正文。
 * @param[in] type 同步借用主题，此测试忽略。
 * @param[in] json 同步借用正文，此测试忽略。
 * @param[in,out] arg 借用 event_ctx_t，计数由其 mutex 保护。
 * @post count 增加并广播 cond。
 */
static void lifetime_count_handler(const char *type, const char *json, void *arg)
{
    (void)type; (void)json;
    event_ctx_t *ctx = arg;
    cc_mutex_lock(ctx->mutex);
    ctx->count++;
    cc_cond_broadcast(ctx->cond);
    cc_mutex_unlock(ctx->mutex);
}

/**
 * @brief 填满首订阅邮箱，核对重复唤醒预算或关闭后后续槽的入队拒绝。
 * @param[in] timeout_case 非零使用合成时钟核对 100/10 毫秒剩余预算，零暂停发布并关闭总线。
 * @return 预期返回码、投递数量和排空均满足为一，失败为零。
 */
static int test_blocked_publish_boundaries(int timeout_case)
{
    event_ctx_t ctx = {0};
    lifetime_barrier_t barrier;
    if (!lifetime_barrier_init(&barrier)) return 0;
    if (cc_mutex_create(&ctx.mutex).code != CC_OK ||
        cc_cond_create(&ctx.cond).code != CC_OK) return 0;
    cc_event_bus_config_t config = cc_event_bus_default_config();
    config.mode = CC_EVENT_BUS_MODE_ASYNC; config.worker_count = 2; config.mailbox_capacity = 1;
    if (cc_event_bus_create_with_config(&config, &ctx.bus).code != CC_OK) return 0;
    cc_event_subscription_options_t options = {0};
    options.overflow_policy = CC_EVENT_BLOCK_WITH_TIMEOUT;
    options.mailbox_capacity = 1; options.block_timeout_ms = timeout_case ? 100 : 3000;
    cc_event_subscription_t slow = {0}, fast = {0};
    if (cc_event_bus_subscribe(ctx.bus, "blocked", blocking_handler, &ctx, &options,
                              &slow).code != CC_OK ||
        cc_event_bus_subscribe(ctx.bus, "blocked", lifetime_count_handler, &ctx, NULL,
                              &fast).code != CC_OK) return 0;
    if (cc_event_bus_publish(ctx.bus, "blocked", "first").code != CC_OK) return 0;
    cc_mutex_lock(ctx.mutex);
    while (!ctx.entered) cc_cond_wait(ctx.cond, ctx.mutex);
    cc_mutex_unlock(ctx.mutex);
    if (cc_event_bus_publish(ctx.bus, "blocked", "second").code != CC_OK) return 0;
    cc_mutex_lock(ctx.mutex);
    while (ctx.count < 2) cc_cond_wait(ctx.cond, ctx.mutex);
    cc_mutex_unlock(ctx.mutex);
    barrier.bus = ctx.bus; barrier.role = ROLE_PUBLISH;
    g_lifetime_barrier = &barrier;
    int ok = 1;
    cc_thread_t publisher_thread = NULL;
    cc_result_t rc;
    if (timeout_case) {
        barrier.fake_clock = 1; barrier.fake_ms = 1000;
        g_event_role = ROLE_PUBLISH;
        rc = cc_event_bus_publish(ctx.bus, "blocked", "third");
        g_event_role = ROLE_NONE;
        barrier.fake_clock = 0;
        ok = rc.code == CC_ERR_TIMEOUT && barrier.fake_waits == 2 &&
            barrier.wait_budgets[0] == 100 && barrier.wait_budgets[1] == 10;
        cc_result_free(&rc);
    } else {
        if (cc_thread_create(lifecycle_waiter_thread, &barrier, &publisher_thread).code != CC_OK) return 0;
        cc_mutex_lock(barrier.mutex);
        while (!barrier.paused) cc_cond_wait(barrier.cond, barrier.mutex);
        cc_mutex_unlock(barrier.mutex);
        rc = cc_event_bus_shutdown(ctx.bus, 0);
        ok = rc.code == CC_ERR_TIMEOUT;
        cc_result_free(&rc);
    }
    cc_mutex_lock(ctx.mutex);
    ctx.release = 1;
    cc_cond_broadcast(ctx.cond);
    cc_mutex_unlock(ctx.mutex);
    if (!timeout_case) {
        uint64_t started_ms = cc_platform_monotonic_ms();
        cc_event_bus_metrics_t metrics = {0};
        do {
            cc_event_bus_get_metrics(ctx.bus, &metrics);
            if (metrics.delivered >= 4) break;
            cc_mutex_lock(ctx.mutex);
            (void)cc_cond_timedwait(ctx.cond, ctx.mutex, 10);
            cc_mutex_unlock(ctx.mutex);
        } while (cc_platform_monotonic_ms() - started_ms < 3000U);
        ok = ok && metrics.delivered == 4;
        cc_mutex_lock(barrier.mutex);
        barrier.release = 1;
        cc_cond_broadcast(barrier.cond);
        cc_mutex_unlock(barrier.mutex);
        rc = cc_thread_join(publisher_thread);
        ok = ok && rc.code == CC_OK && barrier.api_code == CC_ERR_CANCELLED;
        cc_result_free(&rc);
    }
    rc = cc_event_bus_shutdown(ctx.bus, 3000);
    ok = ok && rc.code == CC_OK;
    cc_result_free(&rc);
    cc_mutex_lock(ctx.mutex);
    ok = ok && ctx.count == (timeout_case ? 3 : 2);
    cc_mutex_unlock(ctx.mutex);
    cc_event_bus_destroy(ctx.bus);
    g_lifetime_barrier = NULL;
    cc_cond_destroy(ctx.cond); cc_mutex_destroy(ctx.mutex);
    cc_cond_destroy(barrier.cond); cc_mutex_destroy(barrier.mutex);
    return ok;
}

/**
 * @brief 暂停已登记等待者，核对 owner 销毁屏障或公开代际回绕后的旧等待隔离。
 * @param[in] role ROLE_UNSUBSCRIBE 或 ROLE_FLUSH，分别覆盖两类条件等待。
 * @param[in] reuse_slot 非零时做 65535 代际回绕并保留新回调，零时由 owner 并行销毁。
 * @return 全部检查通过为一，设置或断言失败为零，正常收尾 join 全部测试线程并归还上下文。
 */
static int test_registered_waiter_lifetime(int role, int reuse_slot)
{
    event_ctx_t ctx = {0}, replacement = {0};
    lifetime_barrier_t barrier;
    if (!lifetime_barrier_init(&barrier)) return 0;
    if (cc_event_bus_create(&ctx.bus).code != CC_OK ||
        cc_mutex_create(&ctx.mutex).code != CC_OK ||
        cc_cond_create(&ctx.cond).code != CC_OK ||
        cc_mutex_create(&replacement.mutex).code != CC_OK ||
        cc_cond_create(&replacement.cond).code != CC_OK) return 0;
    barrier.bus = ctx.bus;
    barrier.role = role;
    if (cc_event_bus_subscribe(ctx.bus, "blocked", blocking_handler, &ctx, NULL,
                              &barrier.subscription).code != CC_OK) return 0;
    cc_thread_t publisher_thread, waiter_thread, final_thread;
    if (cc_thread_create(publish_once, &ctx, &publisher_thread).code != CC_OK) return 0;
    cc_mutex_lock(ctx.mutex);
    while (!ctx.entered) cc_cond_wait(ctx.cond, ctx.mutex);
    cc_mutex_unlock(ctx.mutex);
    g_lifetime_barrier = &barrier;
    if (cc_thread_create(lifecycle_waiter_thread, &barrier, &waiter_thread).code != CC_OK) return 0;
    cc_mutex_lock(barrier.mutex);
    while (!barrier.paused) cc_cond_wait(barrier.cond, barrier.mutex);
    cc_mutex_unlock(barrier.mutex);
    cc_mutex_lock(ctx.mutex);
    ctx.release = 1;
    cc_cond_broadcast(ctx.cond);
    cc_mutex_unlock(ctx.mutex);
    cc_result_t rc = cc_thread_join(publisher_thread);
    int ok = rc.code == CC_OK;
    cc_result_free(&rc);

    if (reuse_slot) {
        for (size_t i = 0; i < UINT16_MAX - 1U; i++) {
            cc_event_subscription_t temporary = {0};
            rc = cc_event_bus_subscribe(ctx.bus, "replacement", lifetime_noop_handler, NULL,
                                        NULL, &temporary);
            ok = ok && rc.code == CC_OK;
            cc_result_free(&rc);
            rc = cc_event_bus_unsubscribe(ctx.bus, temporary, 0);
            ok = ok && rc.code == CC_OK;
            cc_result_free(&rc);
        }
        cc_event_subscription_t next = {0};
        rc = cc_event_bus_subscribe(ctx.bus, "blocked", blocking_handler, &replacement, NULL, &next);
        ok = ok && rc.code == CC_OK && next.slot == barrier.subscription.slot &&
            next.generation == barrier.subscription.generation;
        cc_result_free(&rc);
        replacement.bus = ctx.bus;
        if (cc_thread_create(publish_once, &replacement, &final_thread).code != CC_OK) return 0;
        cc_mutex_lock(replacement.mutex);
        while (!replacement.entered) cc_cond_wait(replacement.cond, replacement.mutex);
        cc_mutex_unlock(replacement.mutex);
    } else {
        if (cc_thread_create(lifecycle_destroy_thread, &barrier, &final_thread).code != CC_OK) return 0;
        cc_mutex_lock(barrier.mutex);
        while (!barrier.destroy_waiting && !barrier.destroy_done)
            cc_cond_wait(barrier.cond, barrier.mutex);
        ok = ok && barrier.destroy_waiting && !barrier.destroy_done;
        cc_mutex_unlock(barrier.mutex);
    }
    cc_mutex_lock(barrier.mutex);
    barrier.release = 1;
    cc_cond_broadcast(barrier.cond);
    cc_mutex_unlock(barrier.mutex);
    rc = cc_thread_join(waiter_thread);
    ok = ok && rc.code == CC_OK && barrier.api_code == CC_OK;
    cc_result_free(&rc);
    if (reuse_slot) {
        cc_mutex_lock(replacement.mutex);
        replacement.release = 1;
        cc_cond_broadcast(replacement.cond);
        cc_mutex_unlock(replacement.mutex);
    }
    rc = cc_thread_join(final_thread);
    ok = ok && rc.code == CC_OK;
    cc_result_free(&rc);
    if (reuse_slot) cc_event_bus_destroy(ctx.bus);
    g_lifetime_barrier = NULL;
    cc_cond_destroy(replacement.cond); cc_mutex_destroy(replacement.mutex);
    cc_cond_destroy(ctx.cond); cc_mutex_destroy(ctx.mutex);
    cc_cond_destroy(barrier.cond); cc_mutex_destroy(barrier.mutex);
    return ok;
}

/**
 * @brief 检查 shutdown 的失败 join 可重试，以及并发 owner 销毁只 join 同一 worker 一次。
 * @return 两个场景全部通过为一，正常收尾释放总线、线程和 fixture 同步资源。
 */
static int test_shutdown_join_ownership(void)
{
    lifetime_barrier_t barrier;
    if (!lifetime_barrier_init(&barrier)) return 0;
    cc_event_bus_config_t config = cc_event_bus_default_config();
    config.mode = CC_EVENT_BUS_MODE_ASYNC; config.worker_count = 1;
    if (cc_event_bus_create_with_config(&config, &barrier.bus).code != CC_OK) return 0;
    g_lifetime_barrier = &barrier;
    barrier.join_failures = 1;
    g_event_role = ROLE_SHUTDOWN;
    cc_result_t rc = cc_event_bus_shutdown(barrier.bus, 1000);
    int ok = rc.code == CC_ERR_PLATFORM;
    cc_result_free(&rc);
    rc = cc_event_bus_shutdown(barrier.bus, 1000);
    ok = ok && rc.code == CC_OK && barrier.join_attempts == 2;
    cc_result_free(&rc);
    g_event_role = ROLE_NONE;
    cc_event_bus_destroy(barrier.bus);

    barrier.role = ROLE_SHUTDOWN; barrier.join_pause = 1; barrier.join_attempts = 0;
    if (cc_event_bus_create_with_config(&config, &barrier.bus).code != CC_OK) return 0;
    cc_thread_t shutdown_thread, destroy_thread;
    if (cc_thread_create(lifecycle_waiter_thread, &barrier, &shutdown_thread).code != CC_OK) return 0;
    cc_mutex_lock(barrier.mutex);
    while (!barrier.join_entered) cc_cond_wait(barrier.cond, barrier.mutex);
    cc_mutex_unlock(barrier.mutex);
    if (cc_thread_create(lifecycle_destroy_thread, &barrier, &destroy_thread).code != CC_OK) return 0;
    cc_mutex_lock(barrier.mutex);
    while (!barrier.destroy_waiting && !barrier.destroy_done)
        cc_cond_wait(barrier.cond, barrier.mutex);
    ok = ok && barrier.destroy_waiting && !barrier.destroy_done;
    barrier.join_release = 1;
    cc_cond_broadcast(barrier.cond);
    cc_mutex_unlock(barrier.mutex);
    rc = cc_thread_join(shutdown_thread);
    ok = ok && rc.code == CC_OK;
    cc_result_free(&rc);
    rc = cc_thread_join(destroy_thread);
    ok = ok && rc.code == CC_OK && barrier.api_code == CC_OK && barrier.join_attempts == 1;
    cc_result_free(&rc);
    g_lifetime_barrier = NULL;
    cc_cond_destroy(barrier.cond); cc_mutex_destroy(barrier.mutex);
    return ok;
}
#endif

/**
 * @brief 运行同步并发/嵌套发布，以及关闭、登记等待者和 join 所有权回归。
 * @return 全部断言满足为零，创建或任一回归失败为一。
 * @note 基础计数应为 THREADS * LOOPS * 2；Linux 屏障只在对应测试宏启用时参与。
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
    ok = ok && test_shutdown_waits_for_sync_callback();
#ifdef CC_TEST_EVENT_LIFETIME_BARRIERS
    ok = ok && test_registered_waiter_lifetime(ROLE_UNSUBSCRIBE, 0);
    ok = ok && test_registered_waiter_lifetime(ROLE_FLUSH, 0);
    ok = ok && test_registered_waiter_lifetime(ROLE_UNSUBSCRIBE, 1);
    ok = ok && test_shutdown_join_ownership();
    ok = ok && test_blocked_publish_boundaries(1);
    ok = ok && test_blocked_publish_boundaries(0);
#endif
    return ok ? 0 : 1;
}
