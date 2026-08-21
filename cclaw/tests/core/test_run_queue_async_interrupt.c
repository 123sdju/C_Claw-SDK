

#include "cc/app/cc_run_queue.h"
#include "cc/ports/cc_thread.h"

/* interrupt 测试状态，用 cond 等待任务真正开始后再取消。 */
typedef struct interrupt_state {
    cc_mutex_t mutex;
    cc_cond_t cond;
    int started;
} interrupt_state_t;

/*
 * 可取消任务。
 *
 * 任务启动后循环检查 cancel token；收到中断后返回 CC_ERR_CANCELLED，验证取消能传播到
 * 正在运行的 job。
 */
static cc_result_t cancellable_task(void *user_data, cc_cancel_token_t *token)
{
    interrupt_state_t *state = (interrupt_state_t *)user_data;
    cc_mutex_lock(state->mutex);
    state->started = 1;
    cc_cond_broadcast(state->cond);
    cc_mutex_unlock(state->mutex);

    while (!cc_cancel_token_is_cancelled(token)) {
        for (volatile int i = 0; i < 10000; i++) {
        }
    }
    return cc_result_error(CC_ERR_CANCELLED, "Task observed cancellation");
}

/*
 * 验证异步提交后按 run_id interrupt。
 *
 * collect 应收到 CC_ERR_CANCELLED，并且 queue 的 in_flight/pending 计数归零。
 */
int main(void)
{
    cc_run_queue_config_t config = cc_run_queue_default_config();
    config.main_concurrency = 1;
    config.per_session_concurrency = 1;

    interrupt_state_t state;
    state.mutex = NULL;
    state.cond = NULL;
    state.started = 0;

    if (cc_mutex_create(&state.mutex).code != CC_OK) return 1;
    if (cc_cond_create(&state.cond).code != CC_OK) return 1;

    cc_run_queue_t *queue = NULL;
    if (cc_run_queue_create(&config, &queue).code != CC_OK) return 1;

    cc_run_queue_request_t request;
    request.session_key = "agent:session";
    request.lane = CC_RUN_QUEUE_LANE_MAIN;
    request.action = CC_RUN_QUEUE_ACTION_FOLLOWUP;

    cc_run_id_t run_id = 0;
    cc_result_t rc = cc_run_queue_submit_with_token(
        queue,
        &request,
        cancellable_task,
        &state,
        &run_id
    );
    if (rc.code != CC_OK) return 1;
    cc_result_free(&rc);

    cc_mutex_lock(state.mutex);
    while (!state.started) {
        cc_cond_wait(state.cond, state.mutex);
    }
    cc_mutex_unlock(state.mutex);

    rc = cc_run_queue_interrupt_run(queue, run_id);
    if (rc.code != CC_OK) return 1;
    cc_result_free(&rc);

    rc = cc_run_queue_collect(queue, run_id);
    int ok = rc.code == CC_ERR_CANCELLED &&
        cc_run_queue_in_flight(queue) == 0 &&
        cc_run_queue_pending(queue) == 0;
    cc_result_free(&rc);

    cc_run_queue_destroy(queue);
    cc_cond_destroy(state.cond);
    cc_mutex_destroy(state.mutex);
    return ok ? 0 : 1;
}
