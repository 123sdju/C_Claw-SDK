



#include "cc/app/cc_run_queue.h"
#include "cc/core/cc_id.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_thread.h"
#include "cc/ports/cc_platform.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>


#define CC_RUN_QUEUE_LANE_COUNT 4
#define CC_RUN_QUEUE_MAX_ACTIVE_SESSIONS 128

/** @brief 作业在队列锁内从等待转为运行或完成，collect 只消费已完成项。 */
typedef enum cc_run_job_state {
    CC_RUN_JOB_PENDING = 0,
    CC_RUN_JOB_RUNNING = 1,
    CC_RUN_JOB_COMPLETED = 2
} cc_run_job_state_t;

/**
 * @brief 队列拥有的会话键及在途计数，由队列 mutex 保护。
 * generation 在会话中断时递增，当前调度与结果收集没有使用它作代际验证。
 */
typedef struct cc_run_queue_session_state {
    char *key;
    int in_flight;

    unsigned long generation;
} cc_run_queue_session_state_t;

/**
 * @brief 队列链表拥有的作业、取消源和结果，worker 在运行阶段借用。
 * 普通任务的适配包装可由 owns_user_data 标记交给作业，包装内业务上下文继续借用。
 * result 在 worker 返回并发布完成后交给唯一 collector，或由放弃/过期/销毁清理。
 */
typedef struct cc_run_queue_job {
    cc_run_id_t id;
    char *session_key;
    cc_run_queue_lane_t lane;
    cc_run_queue_action_t action;
    cc_run_queue_task_fn task;
    cc_run_queue_task_with_token_fn task_with_token;
    void *user_data;
    int owns_user_data;
    cc_cancel_source_t *cancel_source;
    cc_result_t result;
    cc_run_job_state_t state;
    uint64_t completed_at_ms;
    int abandoned;
    struct cc_run_queue_job *next;
} cc_run_queue_job_t;

/**
 * @brief 队列拥有作业链、活动会话表、worker 及同步资源。
 * mutex 保护作业发布、状态及统计，cond 供 worker、collector 和 shutdown 重新检查谓词。
 * worker 在锁外执行任务并填写尚未发布的结果，回锁后发布完成供其他调用读取。
 */
struct cc_run_queue {
    cc_run_queue_config_t config;
    int lane_in_flight[CC_RUN_QUEUE_LANE_COUNT];
    cc_run_queue_session_state_t sessions[CC_RUN_QUEUE_MAX_ACTIVE_SESSIONS];
    size_t session_count;
    size_t total_in_flight;
    size_t total_pending;
    size_t total_completed;
    size_t total_jobs;
    size_t pending_high_water;
    size_t total_high_water;
    unsigned long long submitted_total;
    unsigned long long completed_total;
    unsigned long long cancelled_total;
    unsigned long long abandoned_total;
    int shutting_down;
    cc_run_queue_job_t *jobs;
    cc_run_queue_job_t *tail;
    cc_thread_t *workers;
    size_t worker_count;
    cc_mutex_t mutex;
    cc_cond_t cond;
};

/**
 * @brief 创建带指定说明的取消结果。
 * @param[in] message 借用的说明文本，NULL 时采用默认说明。
 * @return 拥有的 CC_ERR_CANCELLED 结果，接收者用 cc_result_free 清理消息。
 */
static cc_result_t cancelled_result(const char *message)
{
    return cc_result_error(CC_ERR_CANCELLED, message ? message : "Run cancelled");
}

/**
 * @brief 清理已经摘除或尚未发布的作业及其拥有资源。
 * @pre 作业已无 worker、collector 和取消 token 借用，或尚未交给队列。
 * @param[in] job 消费拥有的作业，允许 NULL，普通包装内的业务上下文保持借用。
 * @post 作业、键、结果、取消源及被标记拥有的适配包装均已释放。
 */
static void free_job(cc_run_queue_job_t *job)
{
    if (!job) return;


    free(job->session_key);
    cc_result_free(&job->result);
    cc_cancel_source_destroy(job->cancel_source);
    if (job->owns_user_data) free(job->user_data);
    free(job);
}

/**
 * @brief 返回四类 lane 与会话各并发一、总任务最多 256 项的默认配置。
 * @return 按值配置，同会话最多等待两项，完成结果默认保留 300000 毫秒。
 */
cc_run_queue_config_t cc_run_queue_default_config(void)
{
    cc_run_queue_config_t config;
    config.main_concurrency = 1;
    config.subagent_concurrency = 1;
    config.plugin_concurrency = 1;
    config.mcp_concurrency = 1;
    config.per_session_concurrency = 1;
    config.max_pending_per_session = 2;
    config.max_total_tasks = 256;
    config.completed_ttl_ms = 300000;
    return config;
}

/**
 * @brief 将非正并发值归一为一，保留正值。
 * @param[in] value 配置中的整数并发值。
 * @return 至少为一的并发上限。
 */
static int normalized_limit(int value)
{
    return value <= 0 ? 1 : value;
}

/**
 * @brief 将四种 lane 映射到内部数组下标。
 * @param[in] lane 待检查的 lane 值。
 * @return 有效值返回零到三，其他值返回负一。
 */
static int lane_index(cc_run_queue_lane_t lane)
{
    switch (lane) {
        case CC_RUN_QUEUE_LANE_MAIN: return 0;
        case CC_RUN_QUEUE_LANE_SUBAGENT: return 1;
        case CC_RUN_QUEUE_LANE_PLUGIN: return 2;
        case CC_RUN_QUEUE_LANE_MCP: return 3;
        default: return -1;
    }
}

/**
 * @brief 从不可变配置读取并归一化某个 lane 的并发上限。
 * @param[in] queue 借用的存活队列。
 * @param[in] lane 内部 lane 下标，零到三选择对应配置。
 * @return 正数上限，未知下标返回一。
 */
static int lane_limit(const cc_run_queue_t *queue, int lane)
{
    switch (lane) {
        case 0: return normalized_limit(queue->config.main_concurrency);
        case 1: return normalized_limit(queue->config.subagent_concurrency);
        case 2: return normalized_limit(queue->config.plugin_concurrency);
        case 3: return normalized_limit(queue->config.mcp_concurrency);
        default: return 1;
    }
}

/**
 * @brief 读取并归一化同会话在途任务上限。
 * @param[in] queue 借用的存活队列。
 * @return 至少为一的会话并发上限。
 */
static int session_limit(const cc_run_queue_t *queue)
{
    return normalized_limit(queue->config.per_session_concurrency);
}

/**
 * @brief 用宽整数合计归一化并发，得到最多 32 个共享 worker。
 * @param[in] config 借用的有效配置，四个 int 上限先转换为 uint64_t 再相加。
 * @return 四到 32 之间的 worker 数，各 lane 共享这些 worker。
 */
static size_t desired_worker_count(const cc_run_queue_config_t *config)
{
    int main_limit = normalized_limit(config->main_concurrency);
    int subagent_limit = normalized_limit(config->subagent_concurrency);
    int plugin_limit = normalized_limit(config->plugin_concurrency);
    int mcp_limit = normalized_limit(config->mcp_concurrency);
    uint64_t total = (uint64_t)main_limit + (uint64_t)subagent_limit +
        (uint64_t)plugin_limit + (uint64_t)mcp_limit;
    return total > 32U ? 32U : (size_t)total;
}

/**
 * @brief 按字节查找已登记的会话状态。
 * @pre 调用者持有队列 mutex。
 * @param[in] queue 借用的队列。
 * @param[in] session_key 有效的借用会话键。
 * @return 匹配项下标，缺失返回负一，借用仅在保持锁时有效。
 */
static int find_session_locked(const cc_run_queue_t *queue, const char *session_key)
{
    for (size_t i = 0; i < queue->session_count; i++) {
        if (queue->sessions[i].key && strcmp(queue->sessions[i].key, session_key) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief 为即将运行的首个会话作业复制键并登记零在途状态。
 * @pre 调用者持有队列 mutex，已确认键不存在并准备立即增加在途计数。
 * @param[in,out] queue 借用的队列，成功增加 session_count。
 * @param[in] session_key 有效的借用会话键，成功后队列拥有独立副本。
 * @return 新项下标，达到 128 项上限或分配失败返回负一且不增加条数。
 */
static int create_session_locked(cc_run_queue_t *queue, const char *session_key)
{
    if (queue->session_count >= CC_RUN_QUEUE_MAX_ACTIVE_SESSIONS) return -1;
    cc_run_queue_session_state_t *state = &queue->sessions[queue->session_count];
    state->key = cc_copy_string(session_key);
    if (!state->key) return -1;
    state->in_flight = 0;
    state->generation = 0;
    queue->session_count++;
    return (int)(queue->session_count - 1);
}

/**
 * @brief 释放零在途会话的键，用末项填补空位并减少条数。
 * @pre 调用者持有队列 mutex，后续按键重新查找会话，避免缓存被移动的下标。
 * @param[in,out] queue 借用的队列。
 * @param[in] index 待检查的下标，负数、越界或仍有在途作业时保持原状态。
 * @post 可移除项已释放并从有效数组删除，其他项下标可能变化。
 */
static void remove_idle_session_locked(cc_run_queue_t *queue, int index)
{
    if (index < 0 || (size_t)index >= queue->session_count) return;
    if (queue->sessions[index].in_flight != 0) return;
    free(queue->sessions[index].key);
    size_t last = queue->session_count - 1;
    if ((size_t)index != last) {
        queue->sessions[index] = queue->sessions[last];
    }
    memset(&queue->sessions[last], 0, sizeof(queue->sessions[last]));
    queue->session_count--;
}

/**
 * @brief 扫描链表统计指定会话的等待作业。
 * @pre 调用者持有队列 mutex。
 * @param[in] queue 借用的队列。
 * @param[in] session_key 有效的借用会话键。
 * @return 等待状态且键匹配的作业数，运行及已完成作业不计入。
 */
static size_t pending_count_for_session_locked(
    const cc_run_queue_t *queue,
    const char *session_key
)
{
    size_t count = 0;
    for (cc_run_queue_job_t *job = queue->jobs; job; job = job->next) {
        if (job->state == CC_RUN_JOB_PENDING &&
            job->session_key &&
            strcmp(job->session_key, session_key) == 0) {
            count++;
        }
    }
    return count;
}

/**
 * @brief 将尚未执行的等待作业转为取消完成并更新统计。
 * @pre 调用者持有队列 mutex，并负责在本次状态更新后广播条件变量。
 * @param[in,out] queue 借用的队列，减少等待数并增加完成及取消累计数。
 * @param[in,out] job 队列拥有的作业，NULL 或非等待状态时保持原状。
 * @param[in] message 借用的取消说明，由结果复制保存。
 * @post 有效等待项拥有取消结果、完成时刻及已置位取消源，任务回调不会再执行。
 */
static void complete_pending_job_locked(cc_run_queue_t *queue, cc_run_queue_job_t *job, const char *message)
{
    if (!job || job->state != CC_RUN_JOB_PENDING) return;
    job->state = CC_RUN_JOB_COMPLETED;
    job->result = cancelled_result(message);
    cc_cancel_source_cancel(job->cancel_source);
    if (queue->total_pending > 0) queue->total_pending--;
    queue->total_completed++;
    queue->completed_total++;
    queue->cancelled_total++;
    job->completed_at_ms = cc_platform_monotonic_ms();
}

/**
 * @brief 按新请求的 action 更新同会话旧任务的取消状态。
 * @pre 调用者持有队列 mutex，在发布新作业后统一广播条件变量。
 * @param[in,out] queue 借用的队列，NULL 时直接返回。
 * @param[in] request 借用的请求，NULL 或会话键为空指针时直接返回。
 * @post STEER/INTERRUPT 向运行项发取消，STEER/COLLECT/INTERRUPT 将等待项取消完成，其他值保留旧任务。
 */
static void apply_submit_action_locked(
    cc_run_queue_t *queue,
    const cc_run_queue_request_t *request
)
{
    if (!queue || !request || !request->session_key) return;
    int cancel_running = request->action == CC_RUN_QUEUE_ACTION_STEER ||
        request->action == CC_RUN_QUEUE_ACTION_INTERRUPT;
    int cancel_pending = request->action == CC_RUN_QUEUE_ACTION_STEER ||
        request->action == CC_RUN_QUEUE_ACTION_COLLECT ||
        request->action == CC_RUN_QUEUE_ACTION_INTERRUPT;

    for (cc_run_queue_job_t *job = queue->jobs; job; job = job->next) {
        if (!job->session_key || strcmp(job->session_key, request->session_key) != 0) continue;
        if (cancel_pending && job->state == CC_RUN_JOB_PENDING) {
            complete_pending_job_locked(queue, job, "Run superseded by newer input");
        } else if (cancel_running && job->state == CC_RUN_JOB_RUNNING) {
            cc_cancel_source_cancel(job->cancel_source);
        }
    }
}

/**
 * @brief 按运行 ID 查找队列仍保留的作业。
 * @pre 调用者持有队列 mutex。
 * @param[in] queue 借用的队列。
 * @param[in] run_id 待查运行 ID。
 * @return 锁内借用的作业地址，缺失返回 NULL，解锁后需由状态及拥有协议保护。
 */
static cc_run_queue_job_t *find_job_locked(cc_run_queue_t *queue, cc_run_id_t run_id)
{
    for (cc_run_queue_job_t *job = queue->jobs; job; job = job->next) {
        if (job->id == run_id) return job;
    }
    return NULL;
}

/**
 * @brief 按 ID 从链表摘除作业并调整总数及完成数。
 * @pre 调用者持有队列 mutex，目标为可安全移交的完成项，等待或运行计数由外层负责。
 * @param[in,out] queue 借用的队列。
 * @param[in] run_id 待摘除的运行 ID。
 * @return 拥有的孤立作业，由调用者 free_job，缺失返回 NULL。
 */
static cc_run_queue_job_t *take_job_locked(cc_run_queue_t *queue, cc_run_id_t run_id)
{
    cc_run_queue_job_t *prev = NULL;
    cc_run_queue_job_t *job = queue->jobs;
    while (job) {
        if (job->id == run_id) {
            if (prev) prev->next = job->next;
            else queue->jobs = job->next;
            if (queue->tail == job) queue->tail = prev;
            if (queue->total_jobs > 0) queue->total_jobs--;
            if (job->state == CC_RUN_JOB_COMPLETED && queue->total_completed > 0) {
                queue->total_completed--;
            }
            job->next = NULL;
            return job;
        }
        prev = job;
        job = job->next;
    }
    return NULL;
}

/**
 * @brief 删除完成时间达到 TTL 的作业并清理其结果和包装。
 * @pre 非空队列的 mutex 已由调用者持有，完成项的任务及 token 借用已结束。
 * @param[in,out] queue 借用的队列，NULL 或 TTL 为零时直接返回。
 * @post 过期完成项已删除，之后 collect 对这些 ID 返回未找到。
 */
static void purge_expired_completed_locked(cc_run_queue_t *queue)
{
    if (!queue || queue->config.completed_ttl_ms == 0) return;
    uint64_t now = cc_platform_monotonic_ms();
    cc_run_queue_job_t *job = queue->jobs;
    cc_run_queue_job_t *prev = NULL;
    while (job) {
        cc_run_queue_job_t *next = job->next;
        int expired = job->state == CC_RUN_JOB_COMPLETED &&
            now - job->completed_at_ms >= queue->config.completed_ttl_ms;
        if (expired) {
            if (prev) prev->next = next;
            else queue->jobs = next;
            if (queue->tail == job) queue->tail = prev;
            if (queue->total_jobs > 0) queue->total_jobs--;
            if (queue->total_completed > 0) queue->total_completed--;
            job->next = NULL;
            free_job(job);
        } else {
            prev = job;
        }
        job = next;
    }
}

/**
 * @brief 按链表顺序选取同时满足 lane 和会话预算的等待作业。
 * @pre 调用者持有队列 mutex。
 * @param[in,out] queue 借用的队列，先检查 lane 预算，再为即将运行的会话分配状态。
 * @return 借给当前 worker 的运行作业，增加在途并减少等待计数，无可运行项返回 NULL。
 * @note 无效 lane 或会话状态分配失败会取消完成该项，两条返回路径均广播这些完成变化。
 */
static cc_run_queue_job_t *find_runnable_job_locked(cc_run_queue_t *queue)
{
    cc_run_queue_job_t *runnable = NULL;
    int completed_pending = 0;
    for (cc_run_queue_job_t *job = queue->jobs; job; job = job->next) {
        if (job->state != CC_RUN_JOB_PENDING) continue;
        int lane = lane_index(job->lane);
        if (lane < 0) {
            complete_pending_job_locked(queue, job, "Invalid run queue lane");
            completed_pending = 1;
            continue;
        }
        if (queue->lane_in_flight[lane] >= lane_limit(queue, lane)) continue;
        int session_index = find_session_locked(queue, job->session_key);
        if (session_index < 0) {
            session_index = create_session_locked(queue, job->session_key);
            if (session_index < 0) {
                complete_pending_job_locked(queue, job, "Failed to track session state");
                completed_pending = 1;
                continue;
            }
        }
        cc_run_queue_session_state_t *session = &queue->sessions[session_index];
        if (session->in_flight < session_limit(queue)) {
            session->in_flight++;
            queue->lane_in_flight[lane]++;
            queue->total_in_flight++;
            if (queue->total_pending > 0) queue->total_pending--;
            job->state = CC_RUN_JOB_RUNNING;
            runnable = job;
            break;
        }
    }
    if (completed_pending) cc_cond_broadcast(queue->cond);
    return runnable;
}

/**
 * @brief 在任务返回后回收 lane、会话及总在途计数并发布完成。
 * @pre 调用者持有队列 mutex，job 为本 worker 独占执行且已经填妥结果的运行项。
 * @param[in,out] queue 借用的队列。
 * @param[in,out] job 队列拥有的运行作业。
 * @post 作业为带完成时间的完成项，零在途会话移除，worker 继续处理取消计数及唤醒。
 */
static void finish_running_job_locked(cc_run_queue_t *queue, cc_run_queue_job_t *job)
{
    int lane = lane_index(job->lane);
    if (lane >= 0 && queue->lane_in_flight[lane] > 0) {
        queue->lane_in_flight[lane]--;
    }
    int session_index = find_session_locked(queue, job->session_key);
    if (session_index >= 0) {
        if (queue->sessions[session_index].in_flight > 0) {
            queue->sessions[session_index].in_flight--;
        }
        remove_idle_session_locked(queue, session_index);
    }
    if (queue->total_in_flight > 0) queue->total_in_flight--;
    job->state = CC_RUN_JOB_COMPLETED;
    job->completed_at_ms = cc_platform_monotonic_ms();
    queue->total_completed++;
    queue->completed_total++;
}

/**
 * @brief 在条件变量上等待可运行项，锁外执行任务并回锁发布结果。
 * @param[in,out] arg 借用的队列地址，由创建者保持到所有 worker join 结束。
 * @return 队列关闭后返回 NULL，线程资源由拥有者 join 回收。
 * @note 回调借用上下文及 token，仅在本次调用内有效，放弃项在完成发布后摘除并清理。
 */
static void *worker_main(void *arg)
{
    cc_run_queue_t *queue = (cc_run_queue_t *)arg;
    for (;;) {
        cc_mutex_lock(queue->mutex);
        cc_run_queue_job_t *job = NULL;
        while (!queue->shutting_down && !(job = find_runnable_job_locked(queue))) {
            cc_cond_wait(queue->cond, queue->mutex);
        }
        if (queue->shutting_down && !job) {
            cc_mutex_unlock(queue->mutex);
            return NULL;
        }
        cc_mutex_unlock(queue->mutex);

        if (cc_cancel_token_is_cancelled(cc_cancel_source_token(job->cancel_source))) {
            job->result = cancelled_result("Run cancelled before execution");
        } else if (job->task_with_token) {
            job->result = job->task_with_token(
                job->user_data,
                cc_cancel_source_token(job->cancel_source)
            );
        } else if (job->task) {
            job->result = job->task(job->user_data);
        } else {
            job->result = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Run queue job has no task");
        }

        cc_mutex_lock(queue->mutex);
        finish_running_job_locked(queue, job);
        if (job->result.code == CC_ERR_CANCELLED) queue->cancelled_total++;
        cc_run_queue_job_t *discard = job->abandoned
            ? take_job_locked(queue, job->id) : NULL;
        cc_cond_broadcast(queue->cond);
        cc_mutex_unlock(queue->mutex);
        free_job(discard);
    }
}

/**
 * @brief 复制配置、创建同步资源并启动有上限的共享 worker 池。
 * @param[in] config 调用期间借用的配置，NULL 使用默认值，队列保存按值副本。
 * @param[out] out_queue 必填，成功写入拥有的队列，失败保留原槽值。
 * @return 成功为 CC_OK，空输出为 CC_ERR_INVALID_ARGUMENT，分配和线程端口错误向上传递。
 * @note 部分 worker 创建失败时先关闭并 join 已启动线程，再释放本次资源。
 */
cc_result_t cc_run_queue_create(
    const cc_run_queue_config_t *config,
    cc_run_queue_t **out_queue
)
{
    if (!out_queue) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null out_queue");
    cc_run_queue_t *queue = calloc(1, sizeof(cc_run_queue_t));
    if (!queue) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create run queue");
    queue->config = config ? *config : cc_run_queue_default_config();
    queue->worker_count = desired_worker_count(&queue->config);

    cc_result_t rc = cc_mutex_create(&queue->mutex);
    if (rc.code != CC_OK) {
        free(queue);
        return rc;
    }
    rc = cc_cond_create(&queue->cond);
    if (rc.code != CC_OK) {
        cc_mutex_destroy(queue->mutex);
        free(queue);
        return rc;
    }
    queue->workers = calloc(queue->worker_count, sizeof(cc_thread_t));
    if (!queue->workers) {
        cc_cond_destroy(queue->cond);
        cc_mutex_destroy(queue->mutex);
        free(queue);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate run queue workers");
    }
    for (size_t i = 0; i < queue->worker_count; i++) {
        rc = cc_thread_create(worker_main, queue, &queue->workers[i]);
        if (rc.code != CC_OK) {
            cc_mutex_lock(queue->mutex);
            queue->shutting_down = 1;
            cc_cond_broadcast(queue->cond);
            cc_mutex_unlock(queue->mutex);
            for (size_t j = 0; j < i; j++) {
                cc_result_t join_rc = cc_thread_join(queue->workers[j]);
                cc_result_free(&join_rc);
            }
            free(queue->workers);
            cc_cond_destroy(queue->cond);
            cc_mutex_destroy(queue->mutex);
            free(queue);
            return rc;
        }
    }
    *out_queue = queue;
    return cc_result_ok();
}

/**
 * @brief 关闭接纳、等待运行回调并 join 所有 worker，随后释放整个队列。
 * @pre 拥有者已关闭外部入口并排空其他 API 调用，当前线程不是本队列 worker，业务上下文保持到任务退出。
 * @param[in] queue 消费拥有的队列，允许 NULL。
 * @post 队列、剩余结果、包装、会话键及同步资源已释放，销毁后所有借用句柄失效。
 * @note 普通任务及不检查取消的回调可能使等待持续，UINT_MAX 在本接口表示无限等待。
 */
void cc_run_queue_destroy(cc_run_queue_t *queue)
{
    if (!queue) return;
    cc_result_t shutdown_rc = cc_run_queue_shutdown(queue, UINT_MAX);
    cc_result_free(&shutdown_rc);

    for (size_t i = 0; i < queue->worker_count; i++) {
        if (queue->workers[i]) {
            cc_result_t rc = cc_thread_join(queue->workers[i]);
            cc_result_free(&rc);
        }
    }
    free(queue->workers);

    cc_run_queue_job_t *job = queue->jobs;
    while (job) {
        cc_run_queue_job_t *next = job->next;
        free_job(job);
        job = next;
    }
    for (size_t i = 0; i < queue->session_count; i++) {
        free(queue->sessions[i].key);
    }
    cc_cond_destroy(queue->cond);
    cc_mutex_destroy(queue->mutex);
    free(queue);
}

/**
 * @brief 准备作业并在锁内验证容量、处理旧任务，再发布给 worker。
 * @pre 调用期间 queue 仍存活，user_data 的借用期覆盖回调及已接纳作业的结束。
 * @param[in,out] queue 借用的存活队列。
 * @param[in] request 调用期间借用的请求，复制非空 session_key，lane 必须有效。
 * @param[in] task 在 worker 上调用的非空函数。
 * @param[in] user_data 回调上下文，owns_user_data 非零时只在成功发布后消费该指针。
 * @param[out] out_run_id 必填，成功在发布锁内写入运行 ID，失败保留原值。
 * @param[in] owns_user_data 成功后由作业负责 free 的适配包装标志，失败仍归调用者。
 * @return 成功为 CC_OK，非法参数为 CC_ERR_INVALID_ARGUMENT，关闭为 CC_ERR_CANCELLED，容量为 CC_ERR_QUEUE_FULL，分配和 ID 错误向上传递。
 * @note 背压检查先于 action。TTL 清理可能已发生，成功接纳时拥有标志先于作业可见性写入。
 */
static cc_result_t run_queue_submit_with_token_owned(
    cc_run_queue_t *queue,
    const cc_run_queue_request_t *request,
    cc_run_queue_task_with_token_fn task,
    void *user_data,
    cc_run_id_t *out_run_id,
    int owns_user_data
)
{
    if (!queue || !request || !request->session_key || !task || !out_run_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run queue submit request");
    }
    if (lane_index(request->lane) < 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run queue lane");
    }

    cc_run_queue_job_t *job = calloc(1, sizeof(cc_run_queue_job_t));
    if (!job) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate run queue job");
    job->session_key = cc_copy_string(request->session_key);
    if (!job->session_key) {
        free(job);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy session key");
    }
    cc_result_t rc = cc_cancel_source_create(&job->cancel_source);
    if (rc.code != CC_OK) {
        free(job->session_key);
        free(job);
        return rc;
    }
    job->lane = request->lane;
    job->action = request->action;
    job->task_with_token = task;
    job->user_data = user_data;
    job->state = CC_RUN_JOB_PENDING;
    rc = cc_id_generate_u64(&job->id);
    if (rc.code != CC_OK) {
        free_job(job);
        return rc;
    }
    cc_result_free(&rc);

    cc_mutex_lock(queue->mutex);
    if (queue->shutting_down) {
        cc_mutex_unlock(queue->mutex);
        free_job(job);
        return cc_result_error(CC_ERR_CANCELLED, "Run queue is shutting down");
    }
    purge_expired_completed_locked(queue);
    if (queue->config.max_total_tasks > 0 &&
        queue->total_jobs >= queue->config.max_total_tasks) {
        cc_mutex_unlock(queue->mutex);
        free_job(job);
        return cc_result_error(CC_ERR_QUEUE_FULL, "Run queue task limit reached");
    }
    if (request->action != CC_RUN_QUEUE_ACTION_STEER &&
        request->action != CC_RUN_QUEUE_ACTION_COLLECT &&
        queue->config.max_pending_per_session > 0 &&
        pending_count_for_session_locked(queue, request->session_key) >=
            (size_t)queue->config.max_pending_per_session) {
        cc_mutex_unlock(queue->mutex);
        free_job(job);
        return cc_result_error(CC_ERR_QUEUE_FULL, "Too many pending runs for session");
    }
    for (int attempt = 0; attempt < 8 && find_job_locked(queue, job->id); attempt++) {
        cc_run_id_t candidate = 0;
        rc = cc_id_generate_u64(&candidate);
        if (rc.code != CC_OK) {
            cc_mutex_unlock(queue->mutex);
            free_job(job);
            return rc;
        }
        cc_result_free(&rc);
        job->id = candidate;
    }
    if (find_job_locked(queue, job->id)) {
        cc_mutex_unlock(queue->mutex);
        free_job(job);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to allocate a unique secure run ID");
    }
    apply_submit_action_locked(queue, request);
    /* 先交接适配包装的拥有权，再把作业发布到 worker 可见链表。 */
    job->owns_user_data = owns_user_data;
    if (!queue->jobs) {
        queue->jobs = queue->tail = job;
    } else {
        queue->tail->next = job;
        queue->tail = job;
    }
    queue->total_pending++;
    queue->total_jobs++;
    queue->submitted_total++;
    if (queue->total_pending > queue->pending_high_water) {
        queue->pending_high_water = queue->total_pending;
    }
    if (queue->total_jobs > queue->total_high_water) {
        queue->total_high_water = queue->total_jobs;
    }
    *out_run_id = job->id;
    cc_cond_broadcast(queue->cond);
    cc_mutex_unlock(queue->mutex);
    return cc_result_ok();
}

/**
 * @brief 借用业务上下文，提交可在回调内检查取消 token 的任务。
 * @pre 拥有者保持队列及业务上下文覆盖本次 API 和已接纳任务的寿命。
 * @param[in,out] queue 借用的队列。
 * @param[in] request 调用期间借用的元数据，成功复制会话键。
 * @param[in] task 非空 worker 回调，结果拥有权交给队列。
 * @param[in] user_data 借用的业务上下文，允许 NULL，队列不释放。
 * @param[out] out_run_id 必填，成功写运行 ID，失败保留原值。
 * @return 统一提交路径的接纳、参数、背压、关闭或资源错误。
 */
cc_result_t cc_run_queue_submit_with_token(
    cc_run_queue_t *queue,
    const cc_run_queue_request_t *request,
    cc_run_queue_task_with_token_fn task,
    void *user_data,
    cc_run_id_t *out_run_id)
{
    return run_queue_submit_with_token_owned(
        queue, request, task, user_data, out_run_id, 0);
}

/** @brief 队列成功接纳后拥有的普通任务适配包装，业务 user_data 继续借用。 */
typedef struct cc_run_queue_plain_task {
    cc_run_queue_task_fn task;
    void *user_data;
} cc_run_queue_plain_task_t;

/**
 * @brief 将带 token 的 worker 调用适配到普通任务回调。
 * @param[in] user_data 借用的有效普通任务包装，由作业保持到回调退出。
 * @param[in] cancel_token 调用期间借用的取消 token，本适配层不检查。
 * @return 普通回调交付的拥有结果，由队列保存后交给 collector 或清理。
 */
static cc_result_t run_plain_task_with_token(void *user_data, cc_cancel_token_t *cancel_token)
{
    (void)cancel_token;
    cc_run_queue_plain_task_t *plain = (cc_run_queue_plain_task_t *)user_data;
    return plain->task(plain->user_data);
}

/**
 * @brief 创建适配包装并提交普通任务，成功后包装交给作业拥有。
 * @pre 队列和业务上下文由外层保持到本次 API 及已接纳任务结束。
 * @param[in,out] queue 借用的队列。
 * @param[in] request 调用期间借用的请求，成功复制会话键。
 * @param[in] task 非空普通回调，其执行中途由回调自身决定退出。
 * @param[in] user_data 借用的业务上下文，允许 NULL，队列仅释放外层包装。
 * @param[out] out_run_id 必填，成功写运行 ID，失败保留原值。
 * @return 成功为 CC_OK，参数、包装分配及统一提交路径错误向上传递，失败归还本次包装。
 */
cc_result_t cc_run_queue_submit(
    cc_run_queue_t *queue,
    const cc_run_queue_request_t *request,
    cc_run_queue_task_fn task,
    void *user_data,
    cc_run_id_t *out_run_id
)
{
    if (!task) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null run queue task");
    cc_run_queue_plain_task_t *plain = malloc(sizeof(*plain));
    if (!plain) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate run queue task wrapper");
    plain->task = task;
    plain->user_data = user_data;
    cc_result_t rc = run_queue_submit_with_token_owned(
        queue,
        request,
        run_plain_task_with_token,
        plain,
        out_run_id,
        1
    );
    if (rc.code != CC_OK) free(plain);
    return rc;
}

/**
 * @brief 无限等待指定作业完成并消费其结果。
 * @pre 队列存活，同一结果由单个成功 collector 消费，回调不得等待依赖自身完成的作业。
 * @param[in,out] queue 借用的队列。
 * @param[in] run_id 非零运行 ID。
 * @return 原任务交付的拥有结果，或参数/未找到错误，接收者统一用 cc_result_free 清理。
 */
cc_result_t cc_run_queue_collect(
    cc_run_queue_t *queue,
    cc_run_id_t run_id
)
{
    return cc_run_queue_collect_timeout(queue, run_id, UINT_MAX);
}

/**
 * @brief 在持锁状态下等待指定作业完成，唤醒后重新定位作业。
 * @pre 调用者持有队列 mutex，并保持队列存活，输出地址有效。
 * @param[in,out] queue 借用的队列，条件等待期间暂时释放并重新取得同一锁。
 * @param[in] run_id 待查运行 ID。
 * @param[in] timeout_ms 等待预算，UINT_MAX 无限等待。
 * @param[in] started 有限预算的调用起始毫秒，包含进入 mutex 前的耗时，无限等待时忽略。
 * @param[out] out_job 写入锁内借用项，成功时为完成作业，失败时不能使用其值。
 * @return 已完成为 CC_OK，超时或作业缺失返回对应错误，成功后仍由队列拥有结果。
 */
static cc_result_t wait_for_job_locked(cc_run_queue_t *queue, cc_run_id_t run_id,
                                       unsigned timeout_ms, uint64_t started,
                                       cc_run_queue_job_t **out_job)
{
    *out_job = find_job_locked(queue, run_id);
    while (*out_job && (*out_job)->state != CC_RUN_JOB_COMPLETED) {
        if (timeout_ms == UINT_MAX) {
            cc_cond_wait(queue->cond, queue->mutex);
        } else {
            uint64_t elapsed = cc_platform_monotonic_ms() - started;
            if (elapsed >= timeout_ms) {
                return cc_result_error(CC_ERR_TIMEOUT, "Timed out waiting for run result");
            }
            unsigned remaining = timeout_ms - (unsigned)elapsed;
            int slice = remaining > 50U ? 50 : (int)remaining;
            if (slice <= 0) slice = 1;
            (void)cc_cond_timedwait(queue->cond, queue->mutex, slice);
        }
        *out_job = find_job_locked(queue, run_id);
    }
    if (!*out_job) return cc_result_error(CC_ERR_NOT_FOUND, "Run id not found");
    return cc_result_ok();
}

/**
 * @brief 按调用起始预算等待完成，再摘除作业并转移结果拥有权。
 * @pre 外层保持队列存活并排除销毁，同一 ID 的竞争收集者最多一个取得结果。
 * @param[in,out] queue 借用的队列。
 * @param[in] run_id 非零运行 ID，已放弃、清理或被其他 collector 消费时可能缺失。
 * @param[in] timeout_ms 等待毫秒预算，UINT_MAX 无限等待，零值只接受当前已完成项。
 * @return 原任务结果或 CC_ERR_INVALID_ARGUMENT/CC_ERR_TIMEOUT/CC_ERR_NOT_FOUND，超时保留作业供后续处理。
 * @note 条件等待后重新按 ID 查找，归还的 cc_result_t 消息由调用者清理，锁取得耗时计入预算。
 */
cc_result_t cc_run_queue_collect_timeout(
    cc_run_queue_t *queue,
    cc_run_id_t run_id,
    unsigned timeout_ms
)
{
    if (!queue || run_id == 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run queue collect request");
    }
    uint64_t started = cc_platform_monotonic_ms();
    cc_mutex_lock(queue->mutex);
    cc_run_queue_job_t *job = NULL;
    cc_result_t rc = wait_for_job_locked(queue, run_id, timeout_ms, started, &job);
    if (rc.code != CC_OK) {
        cc_mutex_unlock(queue->mutex);
        return rc;
    }
    cc_result_free(&rc);
    job = take_job_locked(queue, run_id);
    cc_mutex_unlock(queue->mutex);

    cc_result_t result = job->result;
    job->result = cc_result_ok();
    free_job(job);
    return result;
}

/**
 * @brief 无限等待作业完成，保留原结果和作业供之后 collect。
 * @pre 外层保持队列存活，调用线程不依赖自身任务完成，成功前仍允许其他调用收集或清理作业。
 * @param[in,out] queue 借用的队列。
 * @param[in] run_id 非零运行 ID。
 * @return 找到完成作业为 CC_OK，参数非法或 ID 已缺失返回对应错误，不返回任务自身的结果码。
 * @note 缺失本身不证明外部动作状态，已知接纳 ID 的拥有者结合收集、放弃和 TTL 协议解释。
 */
cc_result_t cc_run_queue_wait(cc_run_queue_t *queue, cc_run_id_t run_id)
{
    if (!queue || run_id == 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run queue wait request");
    }
    cc_mutex_lock(queue->mutex);
    cc_run_queue_job_t *job = NULL;
    cc_result_t rc = wait_for_job_locked(queue, run_id, UINT_MAX, 0, &job);
    cc_mutex_unlock(queue->mutex);
    return rc;
}

/**
 * @brief 提交普通任务并立即无限等待收集其结果。
 * @pre 队列及业务上下文覆盖本次同步调用，回调的依赖关系允许取得所需 worker 和会话预算。
 * @param[in,out] queue 借用的队列。
 * @param[in] request 调用期间借用的元数据。
 * @param[in] task 非空普通回调。
 * @param[in] user_data 借用的回调上下文，允许 NULL。
 * @return 提交错误或拥有的任务结果，接收者用 cc_result_free 清理。
 */
cc_result_t cc_run_queue_run(
    cc_run_queue_t *queue,
    const cc_run_queue_request_t *request,
    cc_run_queue_task_fn task,
    void *user_data
)
{
    cc_run_id_t run_id = 0;
    cc_result_t rc = cc_run_queue_submit(queue, request, task, user_data, &run_id);
    if (rc.code != CC_OK) return rc;
    return cc_run_queue_collect(queue, run_id);
}

/**
 * @brief 取消指定会话的等待项，并向运行项发取消信号。
 * @param[in,out] queue 借用且在整个调用期存活的队列。
 * @param[in] session_key 非空指针的借用会话键，按字节匹配。
 * @return 有效参数为 CC_OK，包括没有匹配项，空参数为 CC_ERR_INVALID_ARGUMENT。
 * @note 等待项保留取消结果供收集，运行项退出取决于回调检查，成功返回不等待退出。
 */
cc_result_t cc_run_queue_interrupt_session(
    cc_run_queue_t *queue,
    const char *session_key
)
{
    if (!queue || !session_key) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run queue interrupt");
    }
    cc_mutex_lock(queue->mutex);
    int index = find_session_locked(queue, session_key);
    if (index >= 0) queue->sessions[index].generation++;
    for (cc_run_queue_job_t *job = queue->jobs; job; job = job->next) {
        if (!job->session_key || strcmp(job->session_key, session_key) != 0) continue;
        if (job->state == CC_RUN_JOB_PENDING) {
            complete_pending_job_locked(queue, job, "Run interrupted before execution");
        } else if (job->state == CC_RUN_JOB_RUNNING) {
            cc_cancel_source_cancel(job->cancel_source);
        }
    }
    cc_cond_broadcast(queue->cond);
    cc_mutex_unlock(queue->mutex);
    return cc_result_ok();
}

/**
 * @brief 定位作业并取消等待项或向运行项发布取消。
 * @param[in,out] queue 借用且在调用期存活的队列。
 * @param[in] run_id 非零运行 ID。
 * @return 匹配项为 CC_OK，包括已经完成项，缺失为 CC_ERR_NOT_FOUND，非法参数为 CC_ERR_INVALID_ARGUMENT。
 * @note 已完成结果保留，取消发布与运行回调退出分别判断。
 */
cc_result_t cc_run_queue_interrupt_run(
    cc_run_queue_t *queue,
    cc_run_id_t run_id
)
{
    if (!queue || run_id == 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run queue interrupt request");
    }
    cc_mutex_lock(queue->mutex);
    cc_run_queue_job_t *job = find_job_locked(queue, run_id);
    if (!job) {
        cc_mutex_unlock(queue->mutex);
        return cc_result_error(CC_ERR_NOT_FOUND, "Run id not found");
    }
    if (job->state == CC_RUN_JOB_PENDING) {
        complete_pending_job_locked(queue, job, "Run interrupted before execution");
    } else if (job->state == CC_RUN_JOB_RUNNING) {
        cc_cancel_source_cancel(job->cancel_source);
    }
    cc_cond_broadcast(queue->cond);
    cc_mutex_unlock(queue->mutex);
    return cc_result_ok();
}

/**
 * @brief 在队列锁内读取当前运行作业总数。
 * @param[in] queue 借用且存活的队列，允许 NULL。
 * @return 锁内数值快照，NULL 返回零，不触发 TTL 清理。
 */
size_t cc_run_queue_in_flight(cc_run_queue_t *queue)
{
    if (!queue) return 0;
    cc_mutex_lock(queue->mutex);
    size_t value = queue->total_in_flight;
    cc_mutex_unlock(queue->mutex);
    return value;
}

/**
 * @brief 在队列锁内读取当前等待作业总数。
 * @param[in] queue 借用且存活的队列，允许 NULL。
 * @return 锁内数值快照，NULL 返回零，不触发 TTL 清理。
 */
size_t cc_run_queue_pending(cc_run_queue_t *queue)
{
    if (!queue) return 0;
    cc_mutex_lock(queue->mutex);
    size_t value = queue->total_pending;
    cc_mutex_unlock(queue->mutex);
    return value;
}

/**
 * @brief 放弃作业结果，立即删除可清理项或标记运行项结束后自清理。
 * @param[in,out] queue 借用且在调用期存活的队列。
 * @param[in] run_id 非零运行 ID。
 * @return 已找到为 CC_OK，缺失为 CC_ERR_NOT_FOUND，非法参数为 CC_ERR_INVALID_ARGUMENT。
 * @note 运行项仅标记放弃并发取消，业务上下文需继续有效到任务结束，每次成功调用增加放弃累计数。
 */
cc_result_t cc_run_queue_abandon(cc_run_queue_t *queue, cc_run_id_t run_id)
{
    if (!queue || run_id == 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run queue abandon request");
    }
    cc_mutex_lock(queue->mutex);
    cc_run_queue_job_t *job = find_job_locked(queue, run_id);
    if (!job) {
        cc_mutex_unlock(queue->mutex);
        return cc_result_error(CC_ERR_NOT_FOUND, "Run id not found");
    }
    queue->abandoned_total++;
    cc_run_queue_job_t *discard = NULL;
    if (job->state == CC_RUN_JOB_PENDING) {
        complete_pending_job_locked(queue, job, "Run abandoned before execution");
        discard = take_job_locked(queue, run_id);
    } else if (job->state == CC_RUN_JOB_COMPLETED) {
        discard = take_job_locked(queue, run_id);
    } else {
        job->abandoned = 1;
        cc_cancel_source_cancel(job->cancel_source);
    }
    cc_cond_broadcast(queue->cond);
    cc_mutex_unlock(queue->mutex);
    free_job(discard);
    return cc_result_ok();
}

/**
 * @brief 永久关闭新接纳、取消等待及运行项，并按预算等待在途回调归零。
 * @pre 外层保持队列存活，当前线程不能等待自身所在的 worker 回调退出。
 * @param[in,out] queue 借用的队列，成功或超时后均保持关闭状态。
 * @param[in] timeout_ms 等待毫秒预算，UINT_MAX 无限等待，零值只检查是否已排空。
 * @return 在途归零为 CC_OK，未在预算内归零为 CC_ERR_TIMEOUT，NULL 为 CC_ERR_INVALID_ARGUMENT。
 * @note 返回成功后仍保留作业结果与 worker 句柄，销毁阶段继续 join，外部 API 调用由拥有者协调排空。
 */
cc_result_t cc_run_queue_shutdown(cc_run_queue_t *queue, unsigned timeout_ms)
{
    if (!queue) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null run queue");
    uint64_t started = cc_platform_monotonic_ms();
    cc_mutex_lock(queue->mutex);
    if (!queue->shutting_down) {
        queue->shutting_down = 1;
        for (cc_run_queue_job_t *job = queue->jobs; job; job = job->next) {
            if (job->state == CC_RUN_JOB_PENDING) {
                complete_pending_job_locked(queue, job, "Run queue is shutting down");
            } else if (job->state == CC_RUN_JOB_RUNNING) {
                cc_cancel_source_cancel(job->cancel_source);
            }
        }
        cc_cond_broadcast(queue->cond);
    }
    while (queue->total_in_flight > 0) {
        if (timeout_ms != UINT_MAX) {
            uint64_t elapsed = cc_platform_monotonic_ms() - started;
            if (elapsed >= timeout_ms) {
                cc_mutex_unlock(queue->mutex);
                return cc_result_error(CC_ERR_TIMEOUT, "Run queue shutdown timed out");
            }
            unsigned remaining = timeout_ms - (unsigned)elapsed;
            int slice = remaining > 50U ? 50 : (int)remaining;
            if (slice <= 0) slice = 1;
            (void)cc_cond_timedwait(queue->cond, queue->mutex, slice);
        } else {
            cc_cond_wait(queue->cond, queue->mutex);
        }
    }
    cc_mutex_unlock(queue->mutex);
    return cc_result_ok();
}

/**
 * @brief 清理过期完成结果后复制一次队列状态快照。
 * @param[in,out] queue 借用且存活的队列，允许 NULL，非空时可能执行 TTL 删除。
 * @param[out] out_status 借用的输出结构，NULL 直接返回，其他情况先清零并设置 size。
 * @post 非空队列的计数与关闭状态在同一次锁内复制，NULL 队列得到仅含 size 的空快照。
 */
void cc_run_queue_get_status(cc_run_queue_t *queue, cc_run_queue_status_t *out_status)
{
    if (!out_status) return;
    memset(out_status, 0, sizeof(*out_status));
    out_status->size = sizeof(*out_status);
    if (!queue) return;
    cc_mutex_lock(queue->mutex);
    purge_expired_completed_locked(queue);
    out_status->pending = queue->total_pending;
    out_status->in_flight = queue->total_in_flight;
    out_status->completed = queue->total_completed;
    out_status->total_tasks = queue->total_jobs;
    out_status->pending_high_water = queue->pending_high_water;
    out_status->total_high_water = queue->total_high_water;
    out_status->submitted = queue->submitted_total;
    out_status->completed_total = queue->completed_total;
    out_status->cancelled_total = queue->cancelled_total;
    out_status->abandoned_total = queue->abandoned_total;
    out_status->shutting_down = queue->shutting_down;
    cc_mutex_unlock(queue->mutex);
}
