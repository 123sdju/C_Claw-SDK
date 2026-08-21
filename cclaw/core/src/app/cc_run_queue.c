



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

/* run job 状态机：等待、运行、完成。collect 只会取走 COMPLETED job。 */
typedef enum cc_run_job_state {
    CC_RUN_JOB_PENDING = 0,
    CC_RUN_JOB_RUNNING = 1,
    CC_RUN_JOB_COMPLETED = 2
} cc_run_job_state_t;

/*
 * 单个 session 的并发状态。
 *
 * key 是 agent/session 组合键；in_flight 记录该 session 正在运行的任务数；generation
 * 预留给中断/热切换时识别旧任务。
 */
typedef struct cc_run_queue_session_state {
    char *key;
    int in_flight;

    unsigned long generation;
} cc_run_queue_session_state_t;

/*
 * 队列中的一个 run job。
 *
 * task 与 task_with_token 二选一；cancel_source 由 job 拥有；result 在 worker 完成后
 * 由 collect 转移给调用方。
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

/*
 * run queue 内部状态。
 *
 * jobs 是待处理/运行/已完成 job 链表；lane_in_flight 和 sessions 共同限制全局 lane 并发
 * 与同 session 并发。mutex/cond 保护所有共享状态。
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

/* 构造取消结果，统一 CC_ERR_CANCELLED 的消息。 */
static cc_result_t cancelled_result(const char *message)
{
    return cc_result_error(CC_ERR_CANCELLED, message ? message : "Run cancelled");
}

/* 释放 job 及其拥有的 session_key、result、cancel_source 和可选 user_data。 */
static void free_job(cc_run_queue_job_t *job)
{
    if (!job) return;


    free(job->session_key);
    cc_result_free(&job->result);
    cc_cancel_source_destroy(job->cancel_source);
    if (job->owns_user_data) free(job->user_data);
    free(job);
}

/* 返回默认队列配置。 */
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

/* 将非法并发值归一化为 1，防止配置导致 lane 永远不可运行。 */
static int normalized_limit(int value)
{
    return value <= 0 ? 1 : value;
}

/* lane enum 到数组索引的映射。 */
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

/* 获取指定 lane 的并发上限。 */
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

/* 获取同 session 并发上限。 */
static int session_limit(const cc_run_queue_t *queue)
{
    return normalized_limit(queue->config.per_session_concurrency);
}

/*
 * 根据 lane 并发推导 worker 数。
 *
 * worker 总数不会无限增长，最大限制为 32，避免桌面配置误填导致线程爆炸。
 */
static size_t desired_worker_count(const cc_run_queue_config_t *config)
{
    int main_limit = normalized_limit(config->main_concurrency);
    int subagent_limit = normalized_limit(config->subagent_concurrency);
    int plugin_limit = normalized_limit(config->plugin_concurrency);
    int mcp_limit = normalized_limit(config->mcp_concurrency);
    int total = main_limit + subagent_limit + plugin_limit + mcp_limit;
    if (total < 1) total = 1;


    if (total > 32) total = 32;
    return (size_t)total;
}

/* 在持锁状态下查找 session 状态。 */
static int find_session_locked(const cc_run_queue_t *queue, const char *session_key)
{
    for (size_t i = 0; i < queue->session_count; i++) {
        if (queue->sessions[i].key && strcmp(queue->sessions[i].key, session_key) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* 在持锁状态下创建 session 状态；达到固定上限时失败。 */
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

/* 删除没有 in-flight 任务的 session 状态，保持 session 表紧凑。 */
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

/* 统计某个 session 的 pending job 数，用于 max_pending_per_session 背压。 */
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

/*
 * 把 pending job 标记为已取消完成。
 *
 * 这种 job 尚未被 worker 取走，因此可以直接写入取消 result 并减少 pending 计数。
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

/*
 * 应用提交 action 对旧任务的影响。
 *
 * STEER/INTERRUPT 会取消运行中任务，STEER/COLLECT/INTERRUPT 会取消 pending 任务；FOLLOWUP
 * 保留旧任务并排队。
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

/* 在持锁状态下按 run_id 查找 job。 */
static cc_run_queue_job_t *find_job_locked(cc_run_queue_t *queue, cc_run_id_t run_id)
{
    for (cc_run_queue_job_t *job = queue->jobs; job; job = job->next) {
        if (job->id == run_id) return job;
    }
    return NULL;
}

/* 在持锁状态下从链表摘除 job，所有权转移给调用方。 */
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

/*
 * 查找当前可运行 job。
 *
 * 只有同时满足 lane 并发和 session 并发限制的 pending job 才会进入 RUNNING。无效 lane
 * 或无法创建 session 状态的 job 会直接转成取消完成。
 */
static cc_run_queue_job_t *find_runnable_job_locked(cc_run_queue_t *queue)
{
    for (cc_run_queue_job_t *job = queue->jobs; job; job = job->next) {
        if (job->state != CC_RUN_JOB_PENDING) continue;
        int lane = lane_index(job->lane);
        if (lane < 0) {
            complete_pending_job_locked(queue, job, "Invalid run queue lane");
            continue;
        }
        int session_index = find_session_locked(queue, job->session_key);
        if (session_index < 0) {
            session_index = create_session_locked(queue, job->session_key);
            if (session_index < 0) {
                complete_pending_job_locked(queue, job, "Failed to track session state");
                continue;
            }
        }
        cc_run_queue_session_state_t *session = &queue->sessions[session_index];
        if (session->in_flight < session_limit(queue) &&
            queue->lane_in_flight[lane] < lane_limit(queue, lane)) {
            session->in_flight++;
            queue->lane_in_flight[lane]++;
            queue->total_in_flight++;
            if (queue->total_pending > 0) queue->total_pending--;
            job->state = CC_RUN_JOB_RUNNING;
            return job;
        }
    }
    return NULL;
}

/*
 * 完成运行中 job 的资源计数回收。
 *
 * 减少 lane/session/total in-flight，必要时移除 idle session，并把 job 标记 COMPLETED。
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

/*
 * worker 主循环。
 *
 * worker 在条件变量上等待 runnable job；取到 job 后释放锁执行用户任务，避免任务执行时
 * 阻塞提交/中断/collect。完成后重新加锁更新状态并唤醒等待者。
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

/*
 * 创建 run queue。
 *
 * 初始化 mutex/cond/worker 数组并启动 worker。任一 worker 启动失败都会设置 shutdown、
 * join 已启动线程并释放所有资源。
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

/*
 * 销毁 run queue。
 *
 * 先设置 shutting_down 并取消未完成 job，再 join worker，最后释放 job/session/同步原语。
 * 调用方应停止提交新任务后再销毁。
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

/*
 * 提交带 cancel token 的任务。
 *
 * 创建 job 和 cancel source，在锁内检查 shutdown、pending 上限、应用 action，然后追加到
 * FIFO 链表并唤醒 worker。
 */
cc_result_t cc_run_queue_submit_with_token(
    cc_run_queue_t *queue,
    const cc_run_queue_request_t *request,
    cc_run_queue_task_with_token_fn task,
    void *user_data,
    cc_run_id_t *out_run_id
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

/* 普通 task 的包装结构，用于复用带 token 提交流程。 */
typedef struct cc_run_queue_plain_task {
    cc_run_queue_task_fn task;
    void *user_data;
} cc_run_queue_plain_task_t;

/* 忽略 cancel token 执行普通 task。 */
static cc_result_t run_plain_task_with_token(void *user_data, cc_cancel_token_t *cancel_token)
{
    (void)cancel_token;
    cc_run_queue_plain_task_t *plain = (cc_run_queue_plain_task_t *)user_data;
    return plain->task(plain->user_data);
}

/*
 * 提交不接收 cancel token 的任务。
 *
 * 通过小 wrapper 适配到 submit_with_token；提交成功后 job owns_user_data=1，由 free_job
 * 释放 wrapper。
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
    cc_result_t rc = cc_run_queue_submit_with_token(
        queue,
        request,
        run_plain_task_with_token,
        plain,
        out_run_id
    );
    if (rc.code != CC_OK) free(plain);
    else {
        cc_mutex_lock(queue->mutex);
        cc_run_queue_job_t *job = find_job_locked(queue, *out_run_id);
        if (job) job->owns_user_data = 1;
        cc_mutex_unlock(queue->mutex);
    }
    return rc;
}

/*
 * 等待并收集 run 结果。
 *
 * collect 会阻塞到 job COMPLETED，然后从链表摘除 job，把 result 按值返回给调用方，并
 * 清空 job->result 避免 free_job 重复释放。
 */
cc_result_t cc_run_queue_collect(
    cc_run_queue_t *queue,
    cc_run_id_t run_id
)
{
    return cc_run_queue_collect_timeout(queue, run_id, UINT_MAX);
}

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
    cc_run_queue_job_t *job = find_job_locked(queue, run_id);
    while (job && job->state != CC_RUN_JOB_COMPLETED) {
        if (timeout_ms == UINT_MAX) {
            cc_cond_wait(queue->cond, queue->mutex);
        } else {
            uint64_t elapsed = cc_platform_monotonic_ms() - started;
            if (elapsed >= timeout_ms) {
                cc_mutex_unlock(queue->mutex);
                return cc_result_error(CC_ERR_TIMEOUT, "Timed out waiting for run result");
            }
            unsigned remaining = timeout_ms - (unsigned)elapsed;
            int slice = remaining > 50U ? 50 : (int)remaining;
            if (slice <= 0) slice = 1;
            (void)cc_cond_timedwait(queue->cond, queue->mutex, slice);
        }
        job = find_job_locked(queue, run_id);
    }
    if (!job) {
        cc_mutex_unlock(queue->mutex);
        return cc_result_error(CC_ERR_NOT_FOUND, "Run id not found");
    }
    job = take_job_locked(queue, run_id);
    cc_mutex_unlock(queue->mutex);

    cc_result_t result = job->result;
    job->result = cc_result_ok();
    free_job(job);
    return result;
}

/* 同步运行任务：submit 后立即 collect。 */
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

/*
 * 中断整个 session 的任务。
 *
 * pending job 转成取消完成，running job 触发 cancel_source；真正退出取决于任务是否检查
 * cancel token。
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

/* 中断指定 run_id 的任务。 */
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

/* 读取当前 in-flight 总数。 */
size_t cc_run_queue_in_flight(cc_run_queue_t *queue)
{
    if (!queue) return 0;
    cc_mutex_lock(queue->mutex);
    size_t value = queue->total_in_flight;
    cc_mutex_unlock(queue->mutex);
    return value;
}

/* 读取当前 pending 总数。 */
size_t cc_run_queue_pending(cc_run_queue_t *queue)
{
    if (!queue) return 0;
    cc_mutex_lock(queue->mutex);
    size_t value = queue->total_pending;
    cc_mutex_unlock(queue->mutex);
    return value;
}

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
