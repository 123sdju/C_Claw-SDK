



#include "cc/app/cc_tool_executor_pool.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_thread.h"
#include "cc/ports/cc_platform.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief 单个命名 lane 的拥有配置及锁内统计。
 *
 * name 独立分配，池销毁时释放，数组 realloc 保持名称地址。
 * concurrency 限制同时持有许可数，timeout_ms 由外层 executor 用作工具执行默认值。
 */
typedef struct cc_tool_executor_lane {
    char *name;
    int concurrency;
    int timeout_ms;
    int in_flight;
    int waiting; /**< 当前在条件变量片段中等待的调用数。 */
    int max_waiting; /**< 该 lane 历史等待峰值。 */
    unsigned long acquired;
    unsigned long released;
    unsigned long cancelled; /**< 取消和截止时间到达的合计次数。 */
} cc_tool_executor_lane_t;

/** @brief 池拥有的 lane 数组及同步资源，计数和扩展操作由 mutex 保护。 */
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

/**
 * @brief 构造默认配置。
 * @return 按值返回并发四、工具超时 30000 毫秒、空策略数组和零条目配置。
 */
cc_tool_executor_pool_config_t cc_tool_executor_pool_default_config(void)
{
    cc_tool_executor_pool_config_t config;
    config.default_concurrency = 4;
    config.default_timeout_ms = 30000;
    config.policies = NULL;
    config.policy_count = 0;
    return config;
}

/**
 * @brief 将非正并发配置归一为一。
 * @param[in] value 调用者提供的并发值。
 * @return 正值保持原值，零或负数返回一。
 */
static int normalized_concurrency(int value)
{
    return value <= 0 ? 1 : value;
}

/**
 * @brief 按名称字节比较查找最先匹配的 lane。
 * @pre 调用者持有 pool->mutex。
 * @param[in] pool 非空池，调用期间借用其 lane 数组。
 * @param[in] name 非空指针的借用名称，空字符串可匹配。
 * @return 找到返回内部索引，缺失返回 -1，重复配置使用最先匹配项。
 */
static int find_lane_locked(cc_tool_executor_pool_t *pool, const char *name)
{
    for (size_t i = 0; i < pool->lane_count; i++) {
        if (pool->lanes[i].name && strcmp(pool->lanes[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief 数组已满时从八槽起倍增扩容，并清零新增空间。
 * @pre 调用者持有 pool->mutex。
 * @param[in,out] pool 非空池，成功更新拥有的数组地址与容量。
 * @return 容量足够或扩容成功返回一，分配失败返回零并保留原数组。
 */
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

/**
 * @brief 复制名称并初始化新 lane，完整准备后才递增条目数。
 * @pre 调用者持有 pool->mutex。
 * @param[in,out] pool 非空池，拥有新增的名称及槽位。
 * @param[in] name 非空指针的借用名称，调用期间有效。
 * @param[in] concurrency lane 并发值，非正归一为一。
 * @param[in] timeout_ms 工具超时毫秒数，非正使用池默认值。
 * @return 成功返回新索引，数组或名称分配失败返回 -1，失败也可能已扩容数组。
 */
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

/**
 * @brief 找到最先匹配的 lane，缺失时按池默认配置创建。
 * @pre 调用者持有 pool->mutex。
 * @param[in,out] pool 非空池，可能新增 lane 并扩展拥有数组。
 * @param[in] name 非空指针的借用名称，调用期间有效。
 * @return 成功返回既有或新索引，创建失败返回 -1。
 */
static int lane_index_locked(cc_tool_executor_pool_t *pool, const char *name)
{
    int index = find_lane_locked(pool, name);
    if (index >= 0) return index;
    return add_lane_locked(pool, name, pool->default_concurrency, pool->default_timeout_ms);
}

/**
 * @brief 创建同步资源并按策略预置 lane，失败清理本次池。
 * @param[in] config 可选借用配置，NULL 使用默认值，数量非零须提供策略数组。
 * 名称逐项复制，NULL 名称跳过，重复名称按原顺序保存。
 * @param[out] out_pool 必填拥有对象输出，成功填写，失败保持原值。
 * @return 参数缺失返回 CC_ERR_INVALID_ARGUMENT，池或名称分配失败返回 CC_ERR_OUT_OF_MEMORY，
 * 同步原语创建错误向上传递，成功返回 CC_OK。
 */
cc_result_t cc_tool_executor_pool_create(
    const cc_tool_executor_pool_config_t *config,
    cc_tool_executor_pool_t **out_pool
)
{
    if (!out_pool) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null out_pool");
    cc_tool_executor_pool_config_t defaults = cc_tool_executor_pool_default_config();
    const cc_tool_executor_pool_config_t *effective = config ? config : &defaults;
    if (effective->policy_count && !effective->policies)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool pool policies");

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

/**
 * @brief 释放池拥有的所有名称、数组及同步原语。
 * @pre owner 已关闭入口、排空所有调用并归还全部许可，当前没有条件变量等待者。
 * @param[in,out] pool 待销毁对象，可为 NULL。
 * @post 池和借出的名称已释放，调用者停止使用原句柄与票据。
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

/**
 * @brief 通过统一入口无期限等待 lane 许可。
 * @param[in,out] pool 非空池，调用及持有票据期间保持有效。
 * @param[in] lane_name 非空指针的借用名称，未知名称按默认值建立。
 * @param[out] out_ticket 必填独立输出，成功拥有一次许可，失败保持原值。
 * @return 原样返回带取消入口结果，成功后须向原池归还一次。
 */
cc_result_t cc_tool_executor_pool_acquire(
    cc_tool_executor_pool_t *pool,
    const char *lane_name,
    cc_tool_executor_pool_ticket_t *out_ticket
)
{
    return cc_tool_executor_pool_acquire_with_cancel(pool, lane_name, NULL, out_ticket);
}

/**
 * @brief 使用取消令牌通过统一入口等待许可，截止时间为零。
 * @param[in,out] pool 非空池，调用及持有票据期间保持有效。
 * @param[in] lane_name 非空指针的借用名称，空字符串也可使用。
 * @param[in] cancel_token 可选借用取消令牌，调用期间有效。
 * @param[out] out_ticket 必填独立输出，成功填写 lane 索引，失败保持原值。
 * @return 原样返回带截止时间入口结果，成功后须向原池归还一次。
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

/**
 * @brief 锁内核对取消、绝对截止时间与容量，满载时分片等待并重取数组元素。
 *
 * 首先取得或建立 lane，错误退出也可能保留新 lane。等待期间条件变量释放 mutex，
 * 其他调用可能扩容数组，醒来后按稳定索引重新取得 lane 指针。
 * @param[in,out] pool 非空池，许可使用期间由外层保持有效。
 * @param[in] lane_name 非空指针的名称，调用期间借用并保持稳定。
 * @param[in] cancel_token 可选借用取消令牌。
 * @param[in] deadline_ms 单调时钟的绝对毫秒截止时间，零表示无限期。
 * @param[out] out_ticket 必填独立输出，成功时填写索引，失败保持原值。
 * @return 成功占用许可返回 CC_OK，参数或 lane 分配失败返回对应错误，
 * 取消返回 CC_ERR_CANCELLED，到期返回 CC_ERR_TIMEOUT，两者均累计 cancelled 指标。
 */
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
    cc_mutex_lock(pool->mutex);
    int index = lane_index_locked(pool, lane_name);
    if (index < 0) {
        cc_mutex_unlock(pool->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create tool executor lane");
    }

    cc_tool_executor_lane_t *lane = &pool->lanes[index];
    for (;;) {
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
        if (lane->in_flight < lane->concurrency) break;
        lane->waiting++;
        pool->total_waiting++;
        if (lane->waiting > lane->max_waiting) lane->max_waiting = lane->waiting;
        if (pool->total_waiting > pool->max_total_waiting)
            pool->max_total_waiting = pool->total_waiting;
        cc_cond_timedwait(pool->cond, pool->mutex, 50);
        /* 等待释放过锁，lane 数组可能已经 realloc，索引保持稳定。 */
        lane = &pool->lanes[index];
        lane->waiting--;
        pool->total_waiting--;
    }
    lane->in_flight++;
    lane->acquired++;
    pool->acquired++;
    out_ticket->lane_index = (size_t)index;
    cc_mutex_unlock(pool->mutex);
    return cc_result_ok();
}

/**
 * @brief 持锁归还一个 lane 占用并广播所有等待者。
 * @param[in,out] pool 票据原所属池，可为 NULL。
 * @param[in] ticket 成功取得且尚未归还的原池票据，外层保证一次归还。
 * @post 索引有效且仍有占用时递减占用并累计 released，非空池均广播，NULL 直接返回。
 */
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

/**
 * @brief 持锁读取工具超时，未知名称按默认值创建 lane。
 * @param[in,out] pool 待查询池，可为 NULL，查询可能新增 lane。
 * @param[in] lane_name 借用名称，可为 NULL，空字符串可用。
 * @return 参数为空返回零，找到或创建成功返回 lane 超时毫秒数，创建失败回退池默认超时。
 */
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

/**
 * @brief 一次持锁遍历复制总量和指定容量内的 lane 明细。
 * @param[in] pool 非空借用池，查询及名称使用期间保持存活。
 * @param[out] out_metrics 必填独立输出，参数有效后清零，lanes 借用调用者数组。
 * @param[out] lane_buffer 可选明细缓冲区，仅前 reported_lane_count 项被写入，名称借用池至销毁。
 * @param[in] lane_buffer_count 缓冲区元素数，NULL 缓冲区时实际报告条目数为零。
 * @return 参数缺失返回 CC_ERR_INVALID_ARGUMENT 并保持输出原值，成功返回 CC_OK。
 * 总 lane 数与总计数始终覆盖全部 lane，名称副本保持池拥有，generation 使用者须延续租约至读取结束。
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
    out_metrics->reported_lane_count = lane_buffer ?
        (lane_buffer_count < pool->lane_count ? lane_buffer_count : pool->lane_count) : 0;
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
