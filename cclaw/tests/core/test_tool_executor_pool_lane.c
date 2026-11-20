

#include "cc/app/cc_tool_executor_pool.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/ports/cc_thread.h"
#include "cc/ports/cc_platform.h"

#include <string.h>

/* lane 并发测试状态。 */
typedef struct pool_state {
    cc_tool_executor_pool_t *pool;
    cc_mutex_t mutex;
    int active;
    int violation;
} pool_state_t;

/**
 * @brief 获取同一工具 lane 票据并统计临界区并发量。
 * @param[in,out] arg pool_state_t 借用，pool/mutex 覆盖线程期。
 * @return NULL，成功票据在退出前归还，结果诊断逐次消费，失败记入锁内 violation。
 * @note active/violation 由状态锁保护，主线程 join 后读取。
 */
static void *pool_worker(void *arg)
{
    pool_state_t *state = (pool_state_t *)arg;
    cc_tool_executor_pool_ticket_t ticket;
    cc_result_t rc = cc_tool_executor_pool_acquire(state->pool, "plugin.echo", &ticket);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        cc_mutex_lock(state->mutex);
        state->violation = 1;
        cc_mutex_unlock(state->mutex);
        return NULL;
    }
    cc_result_free(&rc);

    cc_mutex_lock(state->mutex);
    state->active++;
    if (state->active > 1) state->violation = 1;
    cc_mutex_unlock(state->mutex);

    cc_mutex_lock(state->mutex);
    state->active--;
    cc_mutex_unlock(state->mutex);
    cc_tool_executor_pool_release(state->pool, ticket);
    return NULL;
}

typedef struct cancel_worker_state {
    cc_tool_executor_pool_t *pool;
    cc_cancel_token_t *token;
    cc_error_code_t result;
} cancel_worker_state_t;

/**
 * @brief 等待共享 lane 票据并记录取消结果。
 * @param[in,out] arg 独占 cancel_worker_state_t 借用，pool/token 覆盖 join。
 * @return NULL，若意外成功仍归还票据，诊断在本线程释放，主线程 join 后读 result。
 */
static void *cancel_worker(void *arg)
{
    cancel_worker_state_t *state = arg;
    cc_tool_executor_pool_ticket_t ticket;
    cc_result_t rc = cc_tool_executor_pool_acquire_with_cancel(
        state->pool, "plugin.echo", state->token, &ticket);
    state->result = rc.code;
    if (rc.code == CC_OK) cc_tool_executor_pool_release(state->pool, ticket);
    cc_result_free(&rc);
    return NULL;
}

/**
 * @brief 在五秒墙钟内等待池指标观察到指定数量的等待者。
 * @param[in] pool 存活池借用，指标接口内部同步。
 * @param[in] expected 期望最少等待者数量。
 * @param[in] mutex 调用期借用的独立夹具锁，供短暂条件等待使用。
 * @param[in] cond 调用期借用的独立条件对象，用一毫秒等待让出调度。
 * @return 满足期望一，预算耗尽零，指标诊断每轮消费。
 */
static int wait_for_waiters(cc_tool_executor_pool_t *pool, int expected,
                            cc_mutex_t mutex, cc_cond_t cond)
{
    uint64_t start = cc_platform_monotonic_ms();
    while (cc_platform_monotonic_ms() - start < 5000U) {
        cc_tool_executor_pool_metrics_t metrics;
        cc_result_t rc = cc_tool_executor_pool_get_metrics(pool, &metrics, NULL, 0);
        int ready = rc.code == CC_OK && metrics.total_waiting >= expected;
        cc_result_free(&rc);
        if (ready) return 1;
        cc_mutex_lock(mutex);
        cc_cond_timedwait(cond, mutex, 1);
        cc_mutex_unlock(mutex);
    }
    return 0;
}

/**
 * @brief 消费操作结果并报告是否成功。
 * @param[in] result 按值移交的拥有结果。
 * @return 成功一，失败零，不保留诊断。
 */
static int succeeded(cc_result_t result)
{
    int good = result.code == CC_OK;
    cc_result_free(&result);
    return good;
}

/**
 * @brief 验证 lane 并发与取消，以及配置空策略和指标数组为空的参数边界。
 * @return 全部断言通过返回零，任一失败返回一，成功路径排空工作线程及票据后销毁池。
 */
int main(void)
{
    cc_tool_executor_pool_policy_t policy;
    policy.name = "plugin.echo";
    policy.concurrency = 1;
    policy.timeout_ms = 12345;

    cc_tool_executor_pool_config_t config = cc_tool_executor_pool_default_config();
    config.default_concurrency = 4;
    config.policies = &policy;
    config.policy_count = 1;

    pool_state_t state;
    state.pool = NULL;
    state.mutex = NULL;
    state.active = 0;
    state.violation = 0;

    if (!succeeded(cc_mutex_create(&state.mutex))) return 1;
    if (!succeeded(cc_tool_executor_pool_create(&config, &state.pool))) {
        cc_mutex_destroy(state.mutex); return 1;
    }
    cc_tool_executor_pool_config_t invalid_config = config;
    invalid_config.policies = NULL;
    cc_tool_executor_pool_t *retained_pool = state.pool;
    cc_result_t invalid_rc = cc_tool_executor_pool_create(&invalid_config, &retained_pool);
    int invalid_ok = invalid_rc.code == CC_ERR_INVALID_ARGUMENT && retained_pool == state.pool;
    cc_result_free(&invalid_rc);
    if (!invalid_ok) return 1;
    cc_tool_executor_pool_metrics_t query_metrics;
    cc_result_t query_rc = cc_tool_executor_pool_get_metrics(state.pool, &query_metrics, NULL, 4);
    int query_ok = query_rc.code == CC_OK && query_metrics.lane_count == 1 &&
        query_metrics.reported_lane_count == 0 && query_metrics.lanes == NULL;
    cc_result_free(&query_rc);
    if (!query_ok) return 1;
    if (cc_tool_executor_pool_timeout_ms(state.pool, "plugin.echo") != 12345) return 1;

    cc_cond_t cond = NULL;
    if (!succeeded(cc_cond_create(&cond))) return 1;
    cc_tool_executor_pool_ticket_t held;
    if (!succeeded(cc_tool_executor_pool_acquire(state.pool, "plugin.echo", &held))) return 1;
    cc_thread_t a;
    cc_thread_t b;
    if (!succeeded(cc_thread_create(pool_worker, &state, &a))) return 1;
    if (!succeeded(cc_thread_create(pool_worker, &state, &b))) return 1;
    int observed_waiters = wait_for_waiters(state.pool, 2, state.mutex, cond);
    cc_tool_executor_pool_release(state.pool, held);
    if (!succeeded(cc_thread_join(a))) return 1;
    if (!succeeded(cc_thread_join(b))) return 1;

    if (!succeeded(cc_tool_executor_pool_acquire(state.pool, "plugin.echo", &held))) return 1;
    cc_cancel_source_t *source = NULL;
    if (!succeeded(cc_cancel_source_create(&source))) return 1;
    cancel_worker_state_t cancellation = {state.pool, cc_cancel_source_token(source), CC_OK};
    cc_thread_t waiter;
    if (!succeeded(cc_thread_create(cancel_worker, &cancellation, &waiter))) return 1;
    int observed_cancellable_waiter = wait_for_waiters(state.pool, 1, state.mutex, cond);
    cc_cancel_source_cancel(source);
    if (!succeeded(cc_thread_join(waiter))) return 1;
    cc_tool_executor_pool_ticket_t cancelled_ticket;
    cc_result_t cancelled_rc = cc_tool_executor_pool_acquire_with_cancel(
        state.pool,
        "plugin.echo",
        cc_cancel_source_token(source),
        &cancelled_ticket);
    int cancel_ok = cancelled_rc.code == CC_ERR_CANCELLED && cancellation.result == CC_ERR_CANCELLED;
    cc_result_free(&cancelled_rc);
    cc_cancel_source_destroy(source);
    cc_tool_executor_pool_release(state.pool, held);

    int ok = state.violation == 0 && cancel_ok && observed_waiters && observed_cancellable_waiter;
    cc_tool_executor_pool_lane_metrics_t lane_metrics[4];
    cc_tool_executor_pool_metrics_t metrics;
    memset(&metrics, 0, sizeof(metrics));
    if (!succeeded(cc_tool_executor_pool_get_metrics(state.pool,
                                          &metrics,
                                          lane_metrics,
                                          sizeof(lane_metrics) / sizeof(lane_metrics[0])))) {
        ok = 0;
    } else if (metrics.lane_count < 1 ||
        metrics.acquired != 4 ||
        metrics.released != 4 ||
        metrics.cancelled != 2 ||
        metrics.max_total_waiting < 2) {
        ok = 0;
    }
    cc_tool_executor_pool_destroy(state.pool);
    cc_cond_destroy(cond);
    cc_mutex_destroy(state.mutex);
    return ok ? 0 : 1;
}
