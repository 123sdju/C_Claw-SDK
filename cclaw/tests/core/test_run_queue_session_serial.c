

#include "cc/app/cc_run_queue.h"
#include "cc/ports/cc_thread.h"
#include <stdio.h>

/** @brief 测试拥有的会话并发计数，两个提交线程及任务通过 mutex 共享。 */
typedef struct serial_state {
    cc_run_queue_t *queue;
    cc_mutex_t mutex;
    int active;
    int violation;
} serial_state_t;

/**
 * @brief 在任务执行期间登记并发数，发现同会话重叠时记录违反标志。
 * @param[in,out] user_data 借用的 serial_state_t，由测试保持到两个提交线程及队列退出。
 * @return CC_OK，测试通过共享计数另外判断并发契约。
 */
static cc_result_t queued_task(void *user_data)
{
    serial_state_t *state = (serial_state_t *)user_data;
    cc_mutex_lock(state->mutex);
    state->active++;
    if (state->active > 1) state->violation = 1;
    cc_mutex_unlock(state->mutex);

    for (volatile int i = 0; i < 5000000; i++) {
    }

    cc_mutex_lock(state->mutex);
    state->active--;
    cc_mutex_unlock(state->mutex);
    return cc_result_ok();
}

/**
 * @brief 向同一会话提交 STEER 任务并同步等待，清理取得的结果。
 * @param[in,out] arg 借用的测试状态，由主测试保持到线程 join。
 * @return NULL，串行性由共享 violation 标志判断。
 */
static void *worker(void *arg)
{
    serial_state_t *state = (serial_state_t *)arg;
    cc_run_queue_request_t request;
    request.session_key = "same-session";
    request.lane = CC_RUN_QUEUE_LANE_MAIN;
    request.action = CC_RUN_QUEUE_ACTION_STEER;
    cc_result_t rc = cc_run_queue_run(state->queue, &request, queued_task, state);
    cc_result_free(&rc);
    return NULL;
}

/**
 * @brief 创建两个提交线程，检查 lane 并发二时同会话仍按一执行。
 * @return 成功为零，端口错误或发现并发违反为一。
 */
static int test_serial_session(void)
{
    cc_run_queue_config_t config = cc_run_queue_default_config();
    config.main_concurrency = 2;
    config.per_session_concurrency = 1;

    serial_state_t state;
    state.queue = NULL;
    state.mutex = NULL;
    state.active = 0;
    state.violation = 0;

    if (cc_mutex_create(&state.mutex).code != CC_OK) return 1;
    if (cc_run_queue_create(&config, &state.queue).code != CC_OK) return 1;

    cc_thread_t a;
    cc_thread_t b;
    if (cc_thread_create(worker, &state, &a).code != CC_OK) return 1;
    if (cc_thread_create(worker, &state, &b).code != CC_OK) return 1;
    if (cc_thread_join(a).code != CC_OK) return 1;
    if (cc_thread_join(b).code != CC_OK) return 1;

    int ok = state.violation == 0 && cc_run_queue_in_flight(state.queue) == 0;
    cc_run_queue_destroy(state.queue);
    cc_mutex_destroy(state.mutex);
    return ok ? 0 : 1;
}

/** @brief 测试持有的运行闸门，使主 lane 保持占用直到核对完等待项。 */
typedef struct blocked_lane_state {
    cc_mutex_t mutex;
    cc_cond_t cond;
    int started;
    int released;
} blocked_lane_state_t;

/**
 * @brief 通知测试任务已运行，并在条件变量上等待闸门开启。
 * @param[in,out] user_data 借用的闸门，测试保证到队列销毁后再释放同步资源。
 * @return 闸门开启后返回 CC_OK。
 */
static cc_result_t blocked_lane_task(void *user_data)
{
    blocked_lane_state_t *state = user_data;
    cc_mutex_lock(state->mutex);
    state->started = 1;
    cc_cond_broadcast(state->cond);
    while (!state->released) cc_cond_wait(state->cond, state->mutex);
    cc_mutex_unlock(state->mutex);
    return cc_result_ok();
}

/**
 * @brief 提供可立即完成的作业，供等待会话与其他 lane 标记共用。
 * @param[in] user_data 未使用的借用上下文，允许 NULL。
 * @return CC_OK，收集到取消错误可识别任务在执行前被错误拒绝。
 */
static cc_result_t completed_task(void *user_data)
{
    (void)user_data;
    return cc_result_ok();
}

/**
 * @brief 占满主 lane 后排入 160 个不同会话，检查等待项没有消耗活动会话槽。
 * @return 所有等待项保留且随后正常完成为零，创建、接纳、计数或结果异常为一。
 * @note 其他 lane 的标记只能在扫描过先前等待链后运行，提供确定的扫描完成依据。
 */
static int test_waiting_sessions_do_not_consume_active_slots(void)
{
    enum { WAITING_COUNT = 160 };
    blocked_lane_state_t gate = {0};
    cc_run_queue_t *queue = NULL;
    cc_run_id_t blocked_id = 0;
    cc_run_id_t waiting_ids[WAITING_COUNT] = {0};
    size_t submitted = 0;
    int failed = 1;
    cc_result_t rc = cc_mutex_create(&gate.mutex);
    if (rc.code != CC_OK) { cc_result_free(&rc); return 1; }
    cc_result_free(&rc);
    rc = cc_cond_create(&gate.cond);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        cc_mutex_destroy(gate.mutex);
        return 1;
    }
    cc_result_free(&rc);

    cc_run_queue_config_t config = cc_run_queue_default_config();
    config.max_total_tasks = 192;
    config.completed_ttl_ms = 0;
    rc = cc_run_queue_create(&config, &queue);
    if (rc.code != CC_OK) { cc_result_free(&rc); goto cleanup; }
    cc_result_free(&rc);
    cc_run_queue_request_t request = {
        .session_key = "blocked", .lane = CC_RUN_QUEUE_LANE_MAIN,
        .action = CC_RUN_QUEUE_ACTION_FOLLOWUP
    };
    rc = cc_run_queue_submit(queue, &request, blocked_lane_task, &gate, &blocked_id);
    if (rc.code != CC_OK) { cc_result_free(&rc); goto cleanup; }
    cc_result_free(&rc);
    cc_mutex_lock(gate.mutex);
    while (!gate.started) cc_cond_wait(gate.cond, gate.mutex);
    cc_mutex_unlock(gate.mutex);

    for (size_t i = 0; i < WAITING_COUNT; i++) {
        char session_key[32];
        snprintf(session_key, sizeof(session_key), "waiting-%zu", i);
        request.session_key = session_key;
        rc = cc_run_queue_submit(queue, &request, completed_task, NULL, &waiting_ids[i]);
        if (rc.code != CC_OK) { cc_result_free(&rc); goto cleanup; }
        cc_result_free(&rc);
        submitted++;
    }
    request.session_key = "scan-marker";
    request.lane = CC_RUN_QUEUE_LANE_PLUGIN;
    cc_run_id_t marker_id = 0;
    rc = cc_run_queue_submit(queue, &request, completed_task, NULL, &marker_id);
    if (rc.code != CC_OK) { cc_result_free(&rc); goto cleanup; }
    cc_result_free(&rc);
    rc = cc_run_queue_collect_timeout(queue, marker_id, 2000U);
    if (rc.code != CC_OK) { cc_result_free(&rc); goto cleanup; }
    cc_result_free(&rc);
    cc_run_queue_status_t status;
    cc_run_queue_get_status(queue, &status);
    failed = status.pending != WAITING_COUNT || status.in_flight != 1 ||
        status.cancelled_total != 0;

cleanup:
    if (queue) {
        cc_mutex_lock(gate.mutex);
        gate.released = 1;
        cc_cond_broadcast(gate.cond);
        cc_mutex_unlock(gate.mutex);
        if (blocked_id) {
            rc = cc_run_queue_collect(queue, blocked_id);
            if (rc.code != CC_OK) failed = 1;
            cc_result_free(&rc);
        }
        for (size_t i = 0; i < submitted; i++) {
            rc = cc_run_queue_collect(queue, waiting_ids[i]);
            if (rc.code != CC_OK) failed = 1;
            cc_result_free(&rc);
        }
        cc_run_queue_destroy(queue);
    }
    cc_cond_destroy(gate.cond);
    cc_mutex_destroy(gate.mutex);
    return failed;
}

/**
 * @brief 检查非消费等待保留结果，并核对缺失及非法 ID 的返回。
 * @return 等待后仍可收集同一结果为零，资源或返回语义异常为一。
 */
static int test_wait_preserves_result(void)
{
    cc_run_queue_t *queue = NULL;
    cc_result_t rc = cc_run_queue_create(NULL, &queue);
    if (rc.code != CC_OK) { cc_result_free(&rc); return 1; }
    cc_result_free(&rc);
    cc_run_queue_request_t request = {
        .session_key = "wait-result", .lane = CC_RUN_QUEUE_LANE_MAIN,
        .action = CC_RUN_QUEUE_ACTION_FOLLOWUP
    };
    cc_run_id_t run_id = 0;
    int failed = 0;
    rc = cc_run_queue_submit(queue, &request, completed_task, NULL, &run_id);
    if (rc.code != CC_OK) { cc_result_free(&rc); cc_run_queue_destroy(queue); return 1; }
    cc_result_free(&rc);
    rc = cc_run_queue_wait(queue, run_id);
    if (rc.code != CC_OK) failed = 1;
    cc_result_free(&rc);
    cc_run_queue_status_t status;
    cc_run_queue_get_status(queue, &status);
    if (status.completed != 1 || status.total_tasks != 1) failed = 1;
    rc = cc_run_queue_collect(queue, run_id);
    if (rc.code != CC_OK) failed = 1;
    cc_result_free(&rc);
    rc = cc_run_queue_wait(queue, run_id);
    if (rc.code != CC_ERR_NOT_FOUND) failed = 1;
    cc_result_free(&rc);
    rc = cc_run_queue_wait(queue, 0);
    if (rc.code != CC_ERR_INVALID_ARGUMENT) failed = 1;
    cc_result_free(&rc);
    cc_run_queue_destroy(queue);
    return failed;
}

/**
 * @brief 顺序执行会话串行化、拥塞等待槽与非消费等待回归。
 * @return 全部通过为零，任一断言路径失败为一。
 */
int main(void)
{
    if (test_serial_session()) return 1;
    if (test_waiting_sessions_do_not_consume_active_slots()) return 1;
    return test_wait_preserves_result();
}
