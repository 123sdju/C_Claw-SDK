

#include "cc/ports/cc_event_bus.h"
#include "cc/ports/cc_thread.h"
#include "cc/ports/cc_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 慢 handler 隔离测试状态：慢订阅者阻塞时，快订阅者仍应由其它 worker 执行。 */
typedef struct {
    cc_event_bus_t *bus;
    cc_mutex_t mutex;
    cc_cond_t cond;
    int slow_started;
    int release_slow;
    int fast_count;
} isolation_state_t;

/**
 * @brief 占住一个异步 worker，通知主线程后等待释放。
 * @param[in] event_type 借用主题，此测试忽略。
 * @param[in] event_json 借用正文，此测试忽略。
 * @param[in,out] user_data 必填 isolation_state_t，其同步资源覆盖回调结束。
 * @post slow_started 置一，返回时已观察到 release_slow。
 */
static void slow_handler(const char *event_type, const char *event_json, void *user_data)
{
    (void)event_type;
    (void)event_json;
    isolation_state_t *state = (isolation_state_t *)user_data;
    cc_mutex_lock(state->mutex);
    state->slow_started = 1;
    cc_cond_broadcast(state->cond);
    while (!state->release_slow) {
        cc_cond_wait(state->cond, state->mutex);
    }
    cc_mutex_unlock(state->mutex);
}

/**
 * @brief 在另一 worker 上记录快速订阅投递。
 * @param[in] event_type 借用主题，此测试忽略。
 * @param[in] event_json 借用正文，此测试忽略。
 * @param[in,out] user_data 必填 isolation_state_t，锁内增加 fast_count 并广播。
 * @post 本次快速回调完成，无正文保留。
 */
static void fast_handler(const char *event_type, const char *event_json, void *user_data)
{
    (void)event_type;
    (void)event_json;
    isolation_state_t *state = (isolation_state_t *)user_data;
    cc_mutex_lock(state->mutex);
    state->fast_count++;
    cc_cond_broadcast(state->cond);
    cc_mutex_unlock(state->mutex);
}

/**
 * @brief 检查两 worker 下慢订阅阻塞期间快速订阅仍能完成。
 * @return 隔离与排空符合预期为一，失败为零，正常收尾释放总线及同步资源。
 */
static int test_slow_handler_does_not_block_fast_handler(void)
{
    isolation_state_t state = {0};
    if (cc_mutex_create(&state.mutex).code != CC_OK) return 0;
    if (cc_cond_create(&state.cond).code != CC_OK) return 0;

    cc_event_bus_config_t config = cc_event_bus_default_config();
    config.mode = CC_EVENT_BUS_MODE_ASYNC;
    config.worker_count = 2;
    config.mailbox_capacity = 8;
    if (cc_event_bus_create_with_config(&config, &state.bus).code != CC_OK) return 0;
    cc_event_subscription_t slow_subscription = {0};
    cc_event_subscription_t fast_subscription = {0};
    if (cc_event_bus_subscribe(state.bus, "slow", slow_handler, &state, NULL,
            &slow_subscription).code != CC_OK) return 0;
    if (cc_event_bus_subscribe(state.bus, "fast", fast_handler, &state, NULL,
            &fast_subscription).code != CC_OK) return 0;

    if (cc_event_bus_publish(state.bus, "slow", "{}").code != CC_OK) return 0;
    cc_mutex_lock(state.mutex);
    while (!state.slow_started) cc_cond_wait(state.cond, state.mutex);
    cc_mutex_unlock(state.mutex);

    if (cc_event_bus_publish(state.bus, "fast", "{}").code != CC_OK) return 0;
    cc_mutex_lock(state.mutex);
    while (state.fast_count == 0) cc_cond_wait(state.cond, state.mutex);
    int ok = state.slow_started && state.fast_count == 1;
    state.release_slow = 1;
    cc_cond_broadcast(state.cond);
    cc_mutex_unlock(state.mutex);

    ok = ok && cc_event_bus_flush(state.bus, 30000).code == CC_OK;
    cc_event_bus_destroy(state.bus);
    cc_cond_destroy(state.cond);
    cc_mutex_destroy(state.mutex);
    return ok;
}

/* FIFO 测试状态：expected 记录同一 handler 应看到的下一个序号。 */
typedef struct {
    cc_mutex_t mutex;
    int expected;
    int violation;
} fifo_state_t;

/**
 * @brief 核对同一订阅按序看到数字正文，并记录序列违例。
 * @param[in] event_type 借用主题，此测试忽略。
 * @param[in] event_json 借用的一位数字正文，空正文视为非法序号。
 * @param[in,out] user_data 必填 fifo_state_t，expected 和 violation 由 mutex 保护。
 * @post 期望序号加一，观察到次序不符时置 violation。
 */
static void fifo_handler(const char *event_type, const char *event_json, void *user_data)
{
    (void)event_type;
    fifo_state_t *state = (fifo_state_t *)user_data;
    int value = event_json && event_json[0] ? event_json[0] - '0' : -1;
    cc_mutex_lock(state->mutex);
    if (value != state->expected) state->violation = 1;
    state->expected++;
    cc_mutex_unlock(state->mutex);
}

/**
 * @brief 在四 worker 下发布五个序号，检查单订阅串行 FIFO。
 * @return 回调条数、顺序及 flush 满足预期为一，失败为零，正常收尾归还资源。
 */
static int test_same_handler_fifo(void)
{
    fifo_state_t state = {0};
    if (cc_mutex_create(&state.mutex).code != CC_OK) return 0;

    cc_event_bus_config_t config = cc_event_bus_default_config();
    config.mode = CC_EVENT_BUS_MODE_ASYNC;
    config.worker_count = 4;
    config.mailbox_capacity = 16;
    cc_event_bus_t *bus = NULL;
    if (cc_event_bus_create_with_config(&config, &bus).code != CC_OK) return 0;
    cc_event_subscription_t subscription = {0};
    if (cc_event_bus_subscribe(bus, "seq", fifo_handler, &state, NULL,
            &subscription).code != CC_OK) return 0;

    const char *values[] = {"0", "1", "2", "3", "4"};
    for (int i = 0; i < 5; i++) {
        if (cc_event_bus_publish(bus, "seq", values[i]).code != CC_OK) return 0;
    }
    int ok = cc_event_bus_flush(bus, 30000).code == CC_OK;
    cc_mutex_lock(state.mutex);
    ok = ok && state.expected == 5 && state.violation == 0;
    cc_mutex_unlock(state.mutex);
    cc_event_bus_destroy(bus);
    cc_mutex_destroy(state.mutex);
    return ok;
}

/* 背压测试状态：用阻塞 handler 填满 worker 和队列，再观察第三次 publish 是否阻塞。 */
typedef struct {
    cc_event_bus_t *bus;
    cc_mutex_t mutex;
    cc_cond_t cond;
    int started;
    int release;
    int publish_thread_started;
    int publish_thread_done;
    int publish_code;
} backpressure_state_t;

/**
 * @brief 阻塞目标订阅回调，供邮箱背压和路由换代测试控制进度。
 * @param[in] event_type 借用主题，此测试忽略。
 * @param[in] event_json 借用正文，此测试忽略。
 * @param[in,out] user_data 必填 backpressure_state_t，主线程维持至总线排空。
 * @post started 置一并广播，观察到 release 后返回。
 */
static void blocking_handler(const char *event_type, const char *event_json, void *user_data)
{
    (void)event_type;
    (void)event_json;
    backpressure_state_t *state = (backpressure_state_t *)user_data;
    cc_mutex_lock(state->mutex);
    state->started = 1;
    cc_cond_broadcast(state->cond);
    while (!state->release) cc_cond_wait(state->cond, state->mutex);
    cc_mutex_unlock(state->mutex);
}

/**
 * @brief 在线程发布 block 的第三个事件，记录接纳前和返回后的状态。
 * @param[in,out] arg 必填 backpressure_state_t，bus 与同步资源覆盖 join。
 * @return NULL，拥有的 publish 结果消息在本线程释放。
 */
static void *third_publish_thread(void *arg)
{
    backpressure_state_t *state = (backpressure_state_t *)arg;
    cc_mutex_lock(state->mutex);
    state->publish_thread_started = 1;
    cc_cond_broadcast(state->cond);
    cc_mutex_unlock(state->mutex);

    cc_result_t rc = cc_event_bus_publish(state->bus, "block", "3");

    cc_mutex_lock(state->mutex);
    state->publish_code = rc.code;
    state->publish_thread_done = 1;
    cc_cond_broadcast(state->cond);
    cc_mutex_unlock(state->mutex);
    cc_result_free(&rc);
    return NULL;
}

/**
 * @brief 填满单项邮箱，检查第三个外部发布者等待到回调释放后成功入队。
 * @return 背压、返回码及排空满足预期为一，失败为零，正常收尾 join 发布者并释放资源。
 */
static int test_queue_full_blocks_publisher(void)
{
    backpressure_state_t state = {0};
    if (cc_mutex_create(&state.mutex).code != CC_OK) return 0;
    if (cc_cond_create(&state.cond).code != CC_OK) return 0;

    cc_event_bus_config_t config = cc_event_bus_default_config();
    config.mode = CC_EVENT_BUS_MODE_ASYNC;
    config.worker_count = 1;
    config.mailbox_capacity = 1;
    if (cc_event_bus_create_with_config(&config, &state.bus).code != CC_OK) return 0;
    cc_event_subscription_options_t options = {
        .size = sizeof(options),
        .overflow_policy = CC_EVENT_BLOCK_WITH_TIMEOUT,
        .mailbox_capacity = 1,
        .block_timeout_ms = 30000,
    };
    cc_event_subscription_t subscription = {0};
    if (cc_event_bus_subscribe(state.bus, "block", blocking_handler, &state, &options,
            &subscription).code != CC_OK) return 0;

    if (cc_event_bus_publish(state.bus, "block", "1").code != CC_OK) return 0;
    cc_mutex_lock(state.mutex);
    while (!state.started) cc_cond_wait(state.cond, state.mutex);
    cc_mutex_unlock(state.mutex);
    if (cc_event_bus_publish(state.bus, "block", "2").code != CC_OK) return 0;

    cc_thread_t thread;
    if (cc_thread_create(third_publish_thread, &state, &thread).code != CC_OK) return 0;
    cc_mutex_lock(state.mutex);
    while (!state.publish_thread_started) cc_cond_wait(state.cond, state.mutex);
    int woke = cc_cond_timedwait(state.cond, state.mutex, 50);
    int ok = (!woke || !state.publish_thread_done) && state.publish_thread_done == 0;
    state.release = 1;
    cc_cond_broadcast(state.cond);
    cc_mutex_unlock(state.mutex);

    cc_thread_join(thread);
    ok = ok && state.publish_code == CC_OK && cc_event_bus_flush(state.bus, 30000).code == CC_OK;
    cc_event_bus_destroy(state.bus);
    cc_cond_destroy(state.cond);
    cc_mutex_destroy(state.mutex);
    return ok;
}

/* destroy drain 测试状态。 */
typedef struct {
    cc_mutex_t mutex;
    int count;
} count_state_t;

/**
 * @brief 对一次异步投递递增受锁保护的计数。
 * @param[in] event_type 借用主题，此测试忽略。
 * @param[in] event_json 借用正文，此测试忽略。
 * @param[in,out] user_data 必填 count_state_t，mutex 覆盖全部回调。
 * @post count 加一，无事件正文保留。
 */
static void count_handler(const char *event_type, const char *event_json, void *user_data)
{
    (void)event_type;
    (void)event_json;
    count_state_t *state = (count_state_t *)user_data;
    cc_mutex_lock(state->mutex);
    state->count++;
    cc_mutex_unlock(state->mutex);
}

/**
 * @brief 发布后直接销毁总线，检查已接纳事件在返回前得到处理。
 * @return 事件计数为一时返回一，失败为零，计数锁在总线销毁后释放。
 */
static int test_destroy_drains_pending_events(void)
{
    count_state_t state = {0};
    if (cc_mutex_create(&state.mutex).code != CC_OK) return 0;

    cc_event_bus_config_t config = cc_event_bus_default_config();
    config.mode = CC_EVENT_BUS_MODE_ASYNC;
    config.worker_count = 1;
    config.mailbox_capacity = 8;
    cc_event_bus_t *bus = NULL;
    if (cc_event_bus_create_with_config(&config, &bus).code != CC_OK) return 0;
    cc_event_subscription_t subscription = {0};
    if (cc_event_bus_subscribe(bus, "count", count_handler, &state, NULL,
            &subscription).code != CC_OK) return 0;
    if (cc_event_bus_publish(bus, "count", "{}").code != CC_OK) return 0;
    cc_event_bus_destroy(bus);

    cc_mutex_lock(state.mutex);
    int ok = state.count == 1;
    cc_mutex_unlock(state.mutex);
    cc_mutex_destroy(state.mutex);
    return ok;
}

/* 嵌套发布测试状态。 */
typedef struct {
    cc_event_bus_t *bus;
    cc_mutex_t mutex;
    int root_count;
    int nested_count;
} nested_state_t;

/**
 * @brief root 回调锁外嵌套发布 nested，两类回调分别累计。
 * @param[in] event_type 借用主题，root 触发嵌套，nested 只计数。
 * @param[in] event_json 借用正文，此测试忽略。
 * @param[in,out] user_data 必填 nested_state_t，覆盖嵌套事件和最终 flush。
 * @post 对应计数在 mutex 内增加，嵌套发布位于该锁外。
 */
static void nested_handler(const char *event_type, const char *event_json, void *user_data)
{
    (void)event_json;
    nested_state_t *state = (nested_state_t *)user_data;
    cc_mutex_lock(state->mutex);
    if (strcmp(event_type, "root") == 0) {
        state->root_count++;
        cc_mutex_unlock(state->mutex);
        cc_event_bus_publish(state->bus, "nested", "{}");
        return;
    }
    if (strcmp(event_type, "nested") == 0) state->nested_count++;
    cc_mutex_unlock(state->mutex);
}

/**
 * @brief 检查两 worker 通配订阅的嵌套发布在 flush 后全部完成。
 * @return root/nested 均一次且排空成功为一，失败为零，正常收尾归还资源。
 */
static int test_nested_publish_does_not_deadlock(void)
{
    nested_state_t state = {0};
    if (cc_mutex_create(&state.mutex).code != CC_OK) return 0;

    cc_event_bus_config_t config = cc_event_bus_default_config();
    config.mode = CC_EVENT_BUS_MODE_ASYNC;
    config.worker_count = 2;
    config.mailbox_capacity = 8;
    if (cc_event_bus_create_with_config(&config, &state.bus).code != CC_OK) return 0;
    cc_event_subscription_t subscription = {0};
    if (cc_event_bus_subscribe(state.bus, NULL, nested_handler, &state, NULL,
            &subscription).code != CC_OK) return 0;
    if (cc_event_bus_publish(state.bus, "root", "{}").code != CC_OK) return 0;

    int ok = cc_event_bus_flush(state.bus, 30000).code == CC_OK;
    cc_mutex_lock(state.mutex);
    ok = ok && state.root_count == 1 && state.nested_count == 1;
    cc_mutex_unlock(state.mutex);
    cc_event_bus_destroy(state.bus);
    cc_mutex_destroy(state.mutex);
    return ok;
}

typedef struct {
    cc_event_bus_t *bus;
    int first_code;
    int second_code;
    int delivered;
} self_publish_state_t;

/**
 * @brief 首次回调连续向自身邮箱发布两项，制造 worker 自依赖背压。
 * @param[in] event_type 借用主题，此测试忽略。
 * @param[in] event_json 借用正文，值为 1 时触发两次嵌套发布。
 * @param[in,out] user_data 必填 self_publish_state_t，同订阅串行回调，flush 后由主线程读取。
 * @post delivered 加一，记录两次发布返回码并释放结果消息。
 */
static void self_publish_handler(const char *event_type, const char *event_json, void *user_data)
{
    (void)event_type;
    self_publish_state_t *state = (self_publish_state_t *)user_data;
    state->delivered++;
    if (strcmp(event_json, "1") != 0) return;
    cc_result_t first = cc_event_bus_publish(state->bus, "self", "2");
    state->first_code = first.code;
    cc_result_free(&first);
    cc_result_t second = cc_event_bus_publish(state->bus, "self", "3");
    state->second_code = second.code;
    cc_result_free(&second);
}

/**
 * @brief 检查唯一 worker 的第二次自发布在满邮箱时返回 QUEUE_FULL。
 * @return 首次自发布成功、第二次拒绝且共投递两项为一，失败为零，正常收尾销毁总线。
 */
static int test_self_publish_full_mailbox_rejected(void)
{
    cc_event_bus_config_t config = cc_event_bus_default_config();
    config.mode = CC_EVENT_BUS_MODE_ASYNC;
    config.worker_count = 1;
    config.mailbox_capacity = 1;
    self_publish_state_t state = {0};
    if (cc_event_bus_create_with_config(&config, &state.bus).code != CC_OK) return 0;
    cc_event_subscription_options_t options = { .size = sizeof(options),
        .overflow_policy = CC_EVENT_BLOCK_WITH_TIMEOUT, .mailbox_capacity = 1,
        .block_timeout_ms = 500 };
    cc_event_subscription_t subscription = {0};
    if (cc_event_bus_subscribe(state.bus, "self", self_publish_handler,
            &state, &options, &subscription).code != CC_OK) return 0;
    if (cc_event_bus_publish(state.bus, "self", "1").code != CC_OK) return 0;
    int ok = cc_event_bus_flush(state.bus, 1000).code == CC_OK &&
        state.first_code == CC_OK && state.second_code == CC_ERR_QUEUE_FULL &&
        state.delivered == 2;
    cc_event_bus_destroy(state.bus);
    return ok;
}

/**
 * @brief 为主题复用和分配失败测试提供无状态订阅回调。
 * @param[in] event_type 借用主题，此测试忽略。
 * @param[in] event_json 借用正文，此测试忽略。
 * @param[in] user_data 可选借用上下文，此测试忽略。
 * @post 无状态变化或正文保留。
 */
static void noop_handler(const char *event_type, const char *event_json, void *user_data)
{
    (void)event_type;
    (void)event_json;
    (void)user_data;
}

/**
 * @brief 循环建立并退订二百五十六个主题，检查墓碑复用及旧值句柄拒绝。
 * @return 循环成功、旧句柄 NOT_FOUND 且活动计数归零为一，失败为零。
 */
static int test_topic_slots_reused_and_stale_handles_rejected(void)
{
    cc_event_bus_t *bus = NULL;
    if (cc_event_bus_create(&bus).code != CC_OK) return 0;

    cc_event_subscription_t first = {0};
    for (int i = 0; i < 256; i++) {
        char topic[32];
        snprintf(topic, sizeof(topic), "topic.%d", i);
        cc_event_subscription_t subscription = {0};
        if (cc_event_bus_subscribe(bus, topic, noop_handler, NULL, NULL,
                &subscription).code != CC_OK) {
            cc_event_bus_destroy(bus);
            return 0;
        }
        if (i == 0) first = subscription;
        if (cc_event_bus_publish(bus, topic, "{}").code != CC_OK ||
            cc_event_bus_unsubscribe(bus, subscription, 0).code != CC_OK) {
            cc_event_bus_destroy(bus);
            return 0;
        }
    }

    cc_result_t stale_rc = cc_event_bus_unsubscribe(bus, first, 0);
    int ok = stale_rc.code == CC_ERR_NOT_FOUND;
    cc_result_free(&stale_rc);
    cc_event_bus_metrics_t metrics = {0};
    cc_event_bus_get_metrics(bus, &metrics);
    ok = ok && metrics.active_subscribers == 0 && metrics.active_topics == 0;
    cc_event_bus_destroy(bus);
    return ok;
}

typedef struct {
    cc_mutex_t mutex;
    cc_cond_t cond;
    int started;
    int release;
} unsubscribe_state_t;

/**
 * @brief 保持回调在途直到主线程释放，观察退订超时后的自动回收。
 * @param[in] event_type 借用主题，此测试忽略。
 * @param[in] event_json 借用正文，此测试忽略。
 * @param[in,out] user_data 必填 unsubscribe_state_t，同步资源保持至回调返回。
 * @post started 置一并广播，等待 release 后返回。
 */
static void unsubscribe_blocking_handler(
    const char *event_type,
    const char *event_json,
    void *user_data)
{
    (void)event_type;
    (void)event_json;
    unsubscribe_state_t *state = (unsubscribe_state_t *)user_data;
    cc_mutex_lock(state->mutex);
    state->started = 1;
    cc_cond_broadcast(state->cond);
    while (!state->release) cc_cond_wait(state->cond, state->mutex);
    cc_mutex_unlock(state->mutex);
}

/**
 * @brief 检查退订超时保留在途槽，回调退出后自动回收并使原句柄过期。
 * @return 保留槽、自动回收、旧句柄及复用代际全部满足为一，失败为零。
 */
static int test_unsubscribe_timeout_reclaims_automatically(void)
{
    unsubscribe_state_t state = {0};
    if (cc_mutex_create(&state.mutex).code != CC_OK) return 0;
    if (cc_cond_create(&state.cond).code != CC_OK) return 0;

    cc_event_bus_config_t config = cc_event_bus_default_config();
    config.mode = CC_EVENT_BUS_MODE_ASYNC;
    config.worker_count = 1;
    config.mailbox_capacity = 2;
    cc_event_bus_t *bus = NULL;
    if (cc_event_bus_create_with_config(&config, &bus).code != CC_OK) return 0;

    cc_event_subscription_t original = {0};
    if (cc_event_bus_subscribe(bus, "blocked", unsubscribe_blocking_handler,
            &state, NULL, &original).code != CC_OK) return 0;
    if (cc_event_bus_publish(bus, "blocked", "{}").code != CC_OK) return 0;

    cc_mutex_lock(state.mutex);
    while (!state.started) cc_cond_wait(state.cond, state.mutex);
    cc_mutex_unlock(state.mutex);

    cc_result_t timeout_rc = cc_event_bus_unsubscribe(bus, original, 0);
    int ok = timeout_rc.code == CC_ERR_TIMEOUT;
    cc_result_free(&timeout_rc);

    cc_event_subscription_t while_pending = {0};
    cc_result_t subscribe_rc = cc_event_bus_subscribe(
        bus, "replacement", noop_handler, NULL, NULL, &while_pending);
    ok = ok && subscribe_rc.code == CC_OK && while_pending.slot != original.slot;
    cc_result_free(&subscribe_rc);

    cc_mutex_lock(state.mutex);
    state.release = 1;
    cc_cond_broadcast(state.cond);
    cc_mutex_unlock(state.mutex);
    ok = ok && cc_event_bus_flush(bus, 30000).code == CC_OK;

    cc_result_t retry_rc = cc_event_bus_unsubscribe(bus, original, 30000);
    ok = ok && retry_rc.code == CC_ERR_NOT_FOUND;
    cc_result_free(&retry_rc);

    cc_event_subscription_t reused = {0};
    cc_result_t reused_rc = cc_event_bus_subscribe(
        bus, "reused", noop_handler, NULL, NULL, &reused);
    ok = ok && reused_rc.code == CC_OK && reused.slot == original.slot &&
        reused.generation != original.generation;
    cc_result_free(&reused_rc);

    cc_event_bus_destroy(bus);
    cc_cond_destroy(state.cond);
    cc_mutex_destroy(state.mutex);
    return ok;
}

/**
 * @brief 在首邮箱背压期间复用后续路由槽，检查旧发布快照不会投递到新主题。
 * @return 路由隔离和新主题成功对照均满足为一，失败为零，正常收尾排空并释放全部上下文。
 */
static int test_publish_snapshot_keeps_subscription_epoch(void)
{
    backpressure_state_t state = {0};
    count_state_t replacement = {0};
    if (cc_mutex_create(&state.mutex).code != CC_OK ||
        cc_cond_create(&state.cond).code != CC_OK ||
        cc_mutex_create(&replacement.mutex).code != CC_OK) return 0;
    cc_event_bus_config_t config = cc_event_bus_default_config();
    config.mode = CC_EVENT_BUS_MODE_ASYNC;
    config.worker_count = 2; config.mailbox_capacity = 1;
    if (cc_event_bus_create_with_config(&config, &state.bus).code != CC_OK) return 0;
    cc_event_subscription_t first = {0}, original = {0}, next = {0};
    if (cc_event_bus_subscribe(state.bus, "block", blocking_handler, &state, NULL,
                              &first).code != CC_OK ||
        cc_event_bus_subscribe(state.bus, "block", noop_handler, NULL, NULL,
                              &original).code != CC_OK) return 0;
    if (cc_event_bus_publish(state.bus, "block", "1").code != CC_OK) return 0;
    cc_mutex_lock(state.mutex);
    while (!state.started) cc_cond_wait(state.cond, state.mutex);
    cc_mutex_unlock(state.mutex);
    if (cc_event_bus_publish(state.bus, "block", "2").code != CC_OK) return 0;
    cc_event_bus_metrics_t before = {0};
    cc_event_bus_get_metrics(state.bus, &before);
    cc_thread_t publisher_thread;
    if (cc_thread_create(third_publish_thread, &state, &publisher_thread).code != CC_OK) return 0;
    uint64_t started_ms = cc_platform_monotonic_ms();
    int publishing_waits = 0;
    while (cc_platform_monotonic_ms() - started_ms < 3000U) {
        cc_event_bus_metrics_t metrics = {0};
        cc_event_bus_get_metrics(state.bus, &metrics);
        if (metrics.matched > before.matched) { publishing_waits = 1; break; }
        cc_mutex_lock(state.mutex);
        (void)cc_cond_timedwait(state.cond, state.mutex, 10);
        cc_mutex_unlock(state.mutex);
    }
    cc_result_t rc = cc_event_bus_unsubscribe(state.bus, original, 1000);
    int ok = publishing_waits && rc.code == CC_OK;
    cc_result_free(&rc);
    rc = cc_event_bus_subscribe(state.bus, "replacement", count_handler, &replacement, NULL, &next);
    ok = ok && rc.code == CC_OK && next.slot == original.slot && next.generation != original.generation;
    cc_result_free(&rc);
    cc_mutex_lock(state.mutex);
    state.release = 1;
    cc_cond_broadcast(state.cond);
    cc_mutex_unlock(state.mutex);
    rc = cc_thread_join(publisher_thread);
    ok = ok && rc.code == CC_OK && state.publish_code == CC_OK;
    cc_result_free(&rc);
    rc = cc_event_bus_flush(state.bus, 3000);
    ok = ok && rc.code == CC_OK;
    cc_result_free(&rc);
    cc_mutex_lock(replacement.mutex);
    ok = ok && replacement.count == 0;
    cc_mutex_unlock(replacement.mutex);
    rc = cc_event_bus_publish(state.bus, "replacement", "{}");
    ok = ok && rc.code == CC_OK;
    cc_result_free(&rc);
    rc = cc_event_bus_flush(state.bus, 3000);
    ok = ok && rc.code == CC_OK;
    cc_result_free(&rc);
    cc_mutex_lock(replacement.mutex);
    ok = ok && replacement.count == 1;
    cc_mutex_unlock(replacement.mutex);
    cc_event_bus_destroy(state.bus);
    cc_mutex_destroy(replacement.mutex);
    cc_cond_destroy(state.cond); cc_mutex_destroy(state.mutex);
    return ok;
}

#ifdef CC_TEST_EVENT_ALLOCATION_FAILURES
static int g_topic_copy_attempts, g_fail_topic_copy;
extern void *__real_malloc(size_t size);

/**
 * @brief 在唯一测试主题第二次复制时返回 NULL，其他分配交真实 malloc。
 * @param[in] size 请求字节数，仅同步订阅测试显式启用故障。
 * @return 命中目录主题复制时为 NULL，其余为调用者拥有的真实分配。
 */
void *__wrap_malloc(size_t size)
{
    if (g_fail_topic_copy && size == sizeof("oom.directory") && ++g_topic_copy_attempts == 2) {
        g_fail_topic_copy = 0;
        return NULL;
    }
    return __real_malloc(size);
}

/**
 * @brief 检查目录主题副本 OOM 返回真实错误并清理槽副本，可随后重试订阅。
 * @return OOM、空输出、零计数及成功重试全部满足为一，失败为零。
 */
static int test_topic_copy_allocation_failure(void)
{
    cc_event_bus_t *bus = NULL;
    if (cc_event_bus_create(&bus).code != CC_OK) return 0;
    cc_event_subscription_t subscription = {UINT16_MAX, UINT16_MAX};
    g_topic_copy_attempts = 0; g_fail_topic_copy = 1;
    cc_result_t rc = cc_event_bus_subscribe(bus, "oom.directory", noop_handler, NULL,
                                           NULL, &subscription);
    g_fail_topic_copy = 0;
    int ok = rc.code == CC_ERR_OUT_OF_MEMORY && g_topic_copy_attempts == 2 &&
        subscription.slot == 0 && subscription.generation == 0;
    cc_result_free(&rc);
    cc_event_bus_metrics_t metrics = {0};
    cc_event_bus_get_metrics(bus, &metrics);
    ok = ok && metrics.active_subscribers == 0 && metrics.active_topics == 0;
    rc = cc_event_bus_subscribe(bus, "oom.directory", noop_handler, NULL, NULL, &subscription);
    ok = ok && rc.code == CC_OK;
    cc_result_free(&rc);
    rc = cc_event_bus_unsubscribe(bus, subscription, 0);
    ok = ok && rc.code == CC_OK;
    cc_result_free(&rc);
    cc_event_bus_destroy(bus);
    return ok;
}
#endif

/**
 * @brief 顺序运行异步隔离、FIFO、背压、嵌套、槽回收及路由代际回归。
 * @return 所有用例通过为零，任一失败打印用例名并返回一。
 * @note Linux 分配 wrapper 额外检查目录主题复制 OOM，其他平台运行公共 API 集成用例。
 */
int main(void)
{
#define RUN_TEST(test) do { if (!(test)()) { fprintf(stderr, "%s failed\n", #test); return 1; } } while (0)
    RUN_TEST(test_slow_handler_does_not_block_fast_handler);
    RUN_TEST(test_same_handler_fifo);
    RUN_TEST(test_queue_full_blocks_publisher);
    RUN_TEST(test_destroy_drains_pending_events);
    RUN_TEST(test_nested_publish_does_not_deadlock);
    RUN_TEST(test_self_publish_full_mailbox_rejected);
    RUN_TEST(test_topic_slots_reused_and_stale_handles_rejected);
    RUN_TEST(test_unsubscribe_timeout_reclaims_automatically);
    RUN_TEST(test_publish_snapshot_keeps_subscription_epoch);
#ifdef CC_TEST_EVENT_ALLOCATION_FAILURES
    RUN_TEST(test_topic_copy_allocation_failure);
#endif
    return 0;
}
