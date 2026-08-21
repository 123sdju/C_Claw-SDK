



#include "cc/app/cc_tool_executor_pool.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_thread.h"
#include "cc/ports/cc_platform.h"

#include <stdlib.h>
#include <string.h>

/*
 * 单个工具执行 lane。
 *
 * name 是 lane 名，concurrency 是最大并发，timeout_ms 是该 lane 默认工具超时，
 * in_flight 在 mutex 保护下记录当前占用。
 */
typedef struct cc_tool_executor_lane {
    char *name;
    int concurrency;
    int timeout_ms;
    int in_flight;
    int waiting;
    int max_waiting;
    unsigned long acquired;
    unsigned long released;
    unsigned long cancelled;
} cc_tool_executor_lane_t;

/* 工具执行池内部状态；mutex/cond 保护 lane 数组和等待者。 */
struct cc_tool_executor_pool {
    int default_concurrency;
    int default_timeout_ms;
    cc_tool_executor_lane_t *lanes;
    size_t lane_count;
    size_t lane_capacity;
    int total_waiting;
    int max_total_waiting;
    unsigned long acquired;
    unsigned long released;
    unsigned long cancelled;
    cc_mutex_t mutex;
    cc_cond_t cond;
};

/* 默认工具执行池配置。 */
cc_tool_executor_pool_config_t cc_tool_executor_pool_default_config(void)
{
    cc_tool_executor_pool_config_t config;
    config.default_concurrency = 4;
    config.default_timeout_ms = 30000;
    config.policies = NULL;
    config.policy_count = 0;
    return config;
}

/* 将非法或 0 并发归一化为 1，避免配置错误导致 lane 永远不可用。 */
static int normalized_concurrency(int value)
{
    return value <= 0 ? 1 : value;
}

/* 在持锁状态下按 name 查找 lane。 */
static int find_lane_locked(cc_tool_executor_pool_t *pool, const char *name)
{
    for (size_t i = 0; i < pool->lane_count; i++) {
        if (pool->lanes[i].name && strcmp(pool->lanes[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* 在持锁状态下确保 lane 数组有可写空间。 */
static int ensure_capacity_locked(cc_tool_executor_pool_t *pool)
{
    if (pool->lane_count < pool->lane_capacity) return 1;
    size_t next_capacity = pool->lane_capacity ? pool->lane_capacity * 2 : 8;
    cc_tool_executor_lane_t *next = realloc(pool->lanes, next_capacity * sizeof(*next));
    if (!next) return 0;
    memset(next + pool->lane_capacity, 0, (next_capacity - pool->lane_capacity) * sizeof(*next));
    pool->lanes = next;
    pool->lane_capacity = next_capacity;
    return 1;
}

/* 在持锁状态下新增 lane，并应用并发/timeout 默认值。 */
static int add_lane_locked(
    cc_tool_executor_pool_t *pool,
    const char *name,
    int concurrency,
    int timeout_ms
)
{
    if (!ensure_capacity_locked(pool)) return -1;
    cc_tool_executor_lane_t *lane = &pool->lanes[pool->lane_count];
    lane->name = cc_copy_string(name);
    if (!lane->name) return -1;
    lane->concurrency = normalized_concurrency(concurrency);
    lane->timeout_ms = timeout_ms > 0 ? timeout_ms : pool->default_timeout_ms;
    lane->in_flight = 0;
    pool->lane_count++;
    return (int)(pool->lane_count - 1);
}

/*
 * 获取 lane 索引，不存在则按默认策略创建。
 *
 * 动态创建 lane 允许工具名直接成为隔离维度，不要求所有工具都提前出现在配置中。
 */
static int lane_index_locked(cc_tool_executor_pool_t *pool, const char *name)
{
    int index = find_lane_locked(pool, name);
    if (index >= 0) return index;
    return add_lane_locked(pool, name, pool->default_concurrency, pool->default_timeout_ms);
}

/*
 * 创建工具执行池。
 *
 * 先创建 mutex/cond，再按配置预置 lane。任一 policy 添加失败都会销毁已创建 pool，避免
 * 留下部分可用的并发控制对象。
 */
cc_result_t cc_tool_executor_pool_create(
    const cc_tool_executor_pool_config_t *config,
    cc_tool_executor_pool_t **out_pool
)
{
    if (!out_pool) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null out_pool");
    cc_tool_executor_pool_config_t defaults = cc_tool_executor_pool_default_config();
    const cc_tool_executor_pool_config_t *effective = config ? config : &defaults;

    cc_tool_executor_pool_t *pool = calloc(1, sizeof(cc_tool_executor_pool_t));
    if (!pool) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create tool executor pool");
    pool->default_concurrency = normalized_concurrency(effective->default_concurrency);
    pool->default_timeout_ms = effective->default_timeout_ms > 0 ? effective->default_timeout_ms : 30000;

    cc_result_t rc = cc_mutex_create(&pool->mutex);
    if (rc.code != CC_OK) {
        free(pool);
        return rc;
    }
    rc = cc_cond_create(&pool->cond);
    if (rc.code != CC_OK) {
        cc_mutex_destroy(pool->mutex);
        free(pool);
        return rc;
    }

    cc_mutex_lock(pool->mutex);
    for (size_t i = 0; i < effective->policy_count; i++) {
        const cc_tool_executor_pool_policy_t *policy = &effective->policies[i];
        if (policy->name && add_lane_locked(pool, policy->name, policy->concurrency, policy->timeout_ms) < 0) {
            cc_mutex_unlock(pool->mutex);
            cc_tool_executor_pool_destroy(pool);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to add tool pool policy");
        }
    }
    cc_mutex_unlock(pool->mutex);

    *out_pool = pool;
    return cc_result_ok();
}

/*
 * 销毁工具执行池。
 *
 * 调用方应保证没有未 release 的 ticket，也没有线程正在 acquire；本函数只释放内部资源。
 */
void cc_tool_executor_pool_destroy(cc_tool_executor_pool_t *pool)
{
    if (!pool) return;
    for (size_t i = 0; i < pool->lane_count; i++) {
        free(pool->lanes[i].name);
    }
    free(pool->lanes);
    cc_cond_destroy(pool->cond);
    cc_mutex_destroy(pool->mutex);
    free(pool);
}

/* 不带取消 token 的 acquire 快捷入口。 */
cc_result_t cc_tool_executor_pool_acquire(
    cc_tool_executor_pool_t *pool,
    const char *lane_name,
    cc_tool_executor_pool_ticket_t *out_ticket
)
{
    return cc_tool_executor_pool_acquire_with_cancel(pool, lane_name, NULL, out_ticket);
}

/*
 * 获取 lane 执行许可。
 *
 * 当 lane 已满时在条件变量上短超时等待，并周期性检查 cancel token。成功后 in_flight++
 * 并返回 ticket，调用方必须 release。
 */
cc_result_t cc_tool_executor_pool_acquire_with_cancel(
    cc_tool_executor_pool_t *pool,
    const char *lane_name,
    cc_cancel_token_t *cancel_token,
    cc_tool_executor_pool_ticket_t *out_ticket
)
{
    return cc_tool_executor_pool_acquire_with_deadline(
        pool, lane_name, cancel_token, 0, out_ticket);
}

cc_result_t cc_tool_executor_pool_acquire_with_deadline(
    cc_tool_executor_pool_t *pool,
    const char *lane_name,
    cc_cancel_token_t *cancel_token,
    uint64_t deadline_ms,
    cc_tool_executor_pool_ticket_t *out_ticket
)
{
    if (!pool || !lane_name || !out_ticket) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid tool executor pool acquire");
    }
    if (cc_cancel_token_is_cancelled(cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "Tool executor pool acquire cancelled");
    }
    if (deadline_ms > 0 && cc_platform_monotonic_ms() >= deadline_ms) {
        return cc_result_error(CC_ERR_TIMEOUT, "Tool executor pool acquire deadline exceeded");
    }

    cc_mutex_lock(pool->mutex);
    int index = lane_index_locked(pool, lane_name);
    if (index < 0) {
        cc_mutex_unlock(pool->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create tool executor lane");
    }

    cc_tool_executor_lane_t *lane = &pool->lanes[index];
    while (lane->in_flight >= lane->concurrency) {
        lane->waiting++;
        pool->total_waiting++;
        if (lane->waiting > lane->max_waiting) {
            lane->max_waiting = lane->waiting;
        }
        if (pool->total_waiting > pool->max_total_waiting) {
            pool->max_total_waiting = pool->total_waiting;
        }
        cc_cond_timedwait(pool->cond, pool->mutex, 50);
        lane = &pool->lanes[index];
        if (lane->waiting > 0) {
            lane->waiting--;
        }
        if (pool->total_waiting > 0) {
            pool->total_waiting--;
        }
        if (cc_cancel_token_is_cancelled(cancel_token)) {
            lane->cancelled++;
            pool->cancelled++;
            cc_mutex_unlock(pool->mutex);
            return cc_result_error(CC_ERR_CANCELLED, "Tool executor pool acquire cancelled");
        }
        if (deadline_ms > 0 && cc_platform_monotonic_ms() >= deadline_ms) {
            lane->cancelled++;
            pool->cancelled++;
            cc_mutex_unlock(pool->mutex);
            return cc_result_error(CC_ERR_TIMEOUT,
                "Tool executor pool acquire deadline exceeded");
        }
    }
    lane->in_flight++;
    lane->acquired++;
    pool->acquired++;
    out_ticket->lane_index = (size_t)index;
    cc_mutex_unlock(pool->mutex);
    return cc_result_ok();
}

/* 释放 lane 执行许可并广播唤醒等待者。 */
void cc_tool_executor_pool_release(
    cc_tool_executor_pool_t *pool,
    cc_tool_executor_pool_ticket_t ticket
)
{
    if (!pool) return;
    cc_mutex_lock(pool->mutex);
    if (ticket.lane_index < pool->lane_count && pool->lanes[ticket.lane_index].in_flight > 0) {
        pool->lanes[ticket.lane_index].in_flight--;
        pool->lanes[ticket.lane_index].released++;
        pool->released++;
    }
    cc_cond_broadcast(pool->cond);
    cc_mutex_unlock(pool->mutex);
}

/* 查询 lane timeout；未知 lane 会按默认策略创建后返回默认 timeout。 */
int cc_tool_executor_pool_timeout_ms(
    cc_tool_executor_pool_t *pool,
    const char *lane_name
)
{
    if (!pool || !lane_name) return 0;
    cc_mutex_lock(pool->mutex);
    int index = lane_index_locked(pool, lane_name);
    int timeout_ms = index >= 0 ? pool->lanes[index].timeout_ms : pool->default_timeout_ms;
    cc_mutex_unlock(pool->mutex);
    return timeout_ms;
}

/*
 * 获取工具执行池的运行指标。
 *
 * 参数:
 *   pool             - 工具执行池实例
 *   out_metrics      - 输出参数，接收总指标
 *   lane_buffer      - 输出参数，接收各 lane 指标数组
 *   lane_buffer_count - lane_buffer 数组容量
 * 返回: CC_OK 成功，CC_ERR_INVALID_ARGUMENT 参数无效。
 *
 * 用于可观测性和诊断，返回各 lane 的并发、等待、超时等统计信息。
 */
cc_result_t cc_tool_executor_pool_get_metrics(
    cc_tool_executor_pool_t *pool,
    cc_tool_executor_pool_metrics_t *out_metrics,
    cc_tool_executor_pool_lane_metrics_t *lane_buffer,
    size_t lane_buffer_count
)
{
    if (!pool || !out_metrics) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid tool executor pool metrics request");
    }

    memset(out_metrics, 0, sizeof(*out_metrics));
    cc_mutex_lock(pool->mutex);
    out_metrics->default_concurrency = pool->default_concurrency;
    out_metrics->default_timeout_ms = pool->default_timeout_ms;
    out_metrics->lane_count = pool->lane_count;
    out_metrics->reported_lane_count = lane_buffer_count < pool->lane_count ?
        lane_buffer_count : pool->lane_count;
    out_metrics->max_total_waiting = pool->max_total_waiting;
    out_metrics->acquired = pool->acquired;
    out_metrics->released = pool->released;
    out_metrics->cancelled = pool->cancelled;
    out_metrics->lanes = lane_buffer;
    for (size_t i = 0; i < pool->lane_count; i++) {
        cc_tool_executor_lane_t *lane = &pool->lanes[i];
        out_metrics->total_in_flight += lane->in_flight;
        out_metrics->total_waiting += lane->waiting;
        if (!lane_buffer || i >= lane_buffer_count) {
            continue;
        }
        lane_buffer[i].name = lane->name;
        lane_buffer[i].concurrency = lane->concurrency;
        lane_buffer[i].timeout_ms = lane->timeout_ms;
        lane_buffer[i].in_flight = lane->in_flight;
        lane_buffer[i].waiting = lane->waiting;
        lane_buffer[i].max_waiting = lane->max_waiting;
        lane_buffer[i].acquired = lane->acquired;
        lane_buffer[i].released = lane->released;
        lane_buffer[i].cancelled = lane->cancelled;
    }
    cc_mutex_unlock(pool->mutex);
    return cc_result_ok();
}
