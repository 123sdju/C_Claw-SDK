

#include "cc/app/cc_run_queue.h"
#include "cc/ports/cc_thread.h"

/* 同 session 串行化测试状态。 */
typedef struct serial_state {
    cc_run_queue_t *queue;
    cc_mutex_t mutex;
    int active;
    int violation;
} serial_state_t;

/*
 * 队列任务：进入时 active++，退出时 active--。
 *
 * 如果 per_session_concurrency=1 失效，同一 session 的两个任务会同时 active>1。
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

/* 并发提交同一个 session_key 的 run_queue 任务。 */
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

/* 验证同一 session 即使 lane 并发为 2，也会被 per-session 限制串行执行。 */
int main(void)
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
