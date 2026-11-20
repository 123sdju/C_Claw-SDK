

#include "cc/app/cc_run_queue.h"
#include "cc/ports/cc_thread.h"

/* interrupt 测试状态，用 cond 等待任务真正开始后再取消。 */
typedef struct interrupt_state {
    cc_mutex_t mutex;
    cc_cond_t cond;
    int started;
} interrupt_state_t;

/**
 * @brief 发布已启动条件后轮询取消令牌并返回取消结果。
 * @param[in,out] user_data interrupt_state_t 借用，mutex/cond 覆盖作业与 collect 期。
 * @param[in] token 队列拥有的取消令牌借用，只在任务执行期间查询。
 * @return 拥有取消诊断的结果，交队列保存并由 collect 调用者消费。
 * @note started 在状态锁内写并广播，主线程观察启动后发起取消，任务无额外拥有资源。
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

/**
 * @brief 检查成功并消费临时操作结果。
 * @param[in] result 按值移交的拥有结果。
 * @return CC_OK 时一，否则零。
 */
static int succeeded(cc_result_t result)
{
    int good = result.code == CC_OK;
    cc_result_free(&result);
    return good;
}

/**
 * @brief 等待任务真正启动后按 run_id 取消，再 collect 并核对队列计数归零。
 * @return 全部创建、取消及终态断言通过零，否则一，正常路径排空队列后释放条件和锁。
 * @note 栈上下文覆盖整个作业，启动条件由 mutex/cond 同步，令牌由队列持有。
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

    if (!succeeded(cc_mutex_create(&state.mutex))) return 1;
    if (!succeeded(cc_cond_create(&state.cond))) { cc_mutex_destroy(state.mutex); return 1; }

    cc_run_queue_t *queue = NULL;
    if (!succeeded(cc_run_queue_create(&config, &queue))) {
        cc_cond_destroy(state.cond); cc_mutex_destroy(state.mutex); return 1;
    }

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
    if (rc.code != CC_OK) {
        cc_result_free(&rc); cc_run_queue_destroy(queue);
        cc_cond_destroy(state.cond); cc_mutex_destroy(state.mutex); return 1;
    }
    cc_result_free(&rc);

    cc_mutex_lock(state.mutex);
    while (!state.started) {
        cc_cond_wait(state.cond, state.mutex);
    }
    cc_mutex_unlock(state.mutex);

    rc = cc_run_queue_interrupt_run(queue, run_id);
    if (rc.code != CC_OK) {
        cc_result_free(&rc); cc_run_queue_destroy(queue);
        cc_cond_destroy(state.cond); cc_mutex_destroy(state.mutex); return 1;
    }
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
