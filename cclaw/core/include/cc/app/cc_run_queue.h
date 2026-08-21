

#ifndef CC_RUN_QUEUE_H
#define CC_RUN_QUEUE_H

#include "cc/core/cc_result.h"
#include <stdint.h>
#include "cc/app/cc_cancel_token.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* run queue 不透明句柄；内部管理 worker、pending 队列、session 串行化和取消源。 */
typedef struct cc_run_queue cc_run_queue_t;

/* 单次提交的运行 id，供 collect/interrupt 精确定位任务。 */
typedef uint64_t cc_run_id_t;

/*
 * run queue lane。
 *
 * 不同 lane 对应不同并发预算：主 Agent、subagent、plugin、MCP 可以互相隔离，避免某类
 * 任务耗尽所有 worker。核心 SDK 只提供 lane，不内置业务 subagent。
 */
typedef enum cc_run_queue_lane {
    CC_RUN_QUEUE_LANE_MAIN = 0,
    CC_RUN_QUEUE_LANE_SUBAGENT = 1,
    CC_RUN_QUEUE_LANE_PLUGIN = 2,
    CC_RUN_QUEUE_LANE_MCP = 3
} cc_run_queue_lane_t;

/*
 * run queue action。
 *
 * STEER/FOLLOWUP/COLLECT/INTERRUPT 描述任务意图，队列可据此决定是否打断同 session 的
 * 旧任务或只是排队等待。
 */
typedef enum cc_run_queue_action {
    CC_RUN_QUEUE_ACTION_STEER = 0,
    CC_RUN_QUEUE_ACTION_FOLLOWUP = 1,
    CC_RUN_QUEUE_ACTION_COLLECT = 2,
    CC_RUN_QUEUE_ACTION_INTERRUPT = 3
} cc_run_queue_action_t;

/*
 * run queue 并发配置。
 *
 * 各 lane 并发和 per_session_concurrency 共同限制资源占用；max_pending_per_session 防止
 * 单个 session 无限堆积任务。
 */
typedef struct cc_run_queue_config {
    int main_concurrency;
    int subagent_concurrency;
    int plugin_concurrency;
    int mcp_concurrency;
    int per_session_concurrency;
    int max_pending_per_session;
    size_t max_total_tasks;
    unsigned completed_ttl_ms;
} cc_run_queue_config_t;

typedef struct cc_run_queue_status {
    size_t size;
    size_t pending;
    size_t in_flight;
    size_t completed;
    size_t total_tasks;
    size_t pending_high_water;
    size_t total_high_water;
    unsigned long long submitted;
    unsigned long long completed_total;
    unsigned long long cancelled_total;
    unsigned long long abandoned_total;
    int shutting_down;
} cc_run_queue_status_t;

/*
 * 提交一次 run 的元数据。
 *
 * session_key 是借用字符串，用于同会话串行化和中断；lane/action 决定调度策略。
 */
typedef struct cc_run_queue_request {
    const char *session_key;
    cc_run_queue_lane_t lane;
    cc_run_queue_action_t action;
} cc_run_queue_request_t;

/* 无取消 token 的任务回调；user_data 由提交方拥有。 */
typedef cc_result_t (*cc_run_queue_task_fn)(void *user_data);

/* 带取消 token 的任务回调；长任务应定期查询 token 并尽快返回 CC_ERR_CANCELLED。 */
typedef cc_result_t (*cc_run_queue_task_with_token_fn)(
    void *user_data,
    cc_cancel_token_t *cancel_token
);

/* 返回默认队列配置；适合作为 profile 配置的基础值。 */
cc_run_queue_config_t cc_run_queue_default_config(void);

/* 创建 run queue；config 为 NULL 时使用默认配置。 */
cc_result_t cc_run_queue_create(
    const cc_run_queue_config_t *config,
    cc_run_queue_t **out_queue
);

/* 销毁队列；应在没有外部线程继续提交任务后调用。 */
void cc_run_queue_destroy(cc_run_queue_t *queue);


/* 提交无 token 任务；返回 run_id，调用方可 collect 或 interrupt。 */
cc_result_t cc_run_queue_submit(
    cc_run_queue_t *queue,
    const cc_run_queue_request_t *request,
    cc_run_queue_task_fn task,
    void *user_data,
    cc_run_id_t *out_run_id
);


/* 提交带 token 任务；队列为每个 run 管理取消源。 */
cc_result_t cc_run_queue_submit_with_token(
    cc_run_queue_t *queue,
    const cc_run_queue_request_t *request,
    cc_run_queue_task_with_token_fn task,
    void *user_data,
    cc_run_id_t *out_run_id
);


/* 等待指定 run 完成并取得其结果。 */
cc_result_t cc_run_queue_collect(
    cc_run_queue_t *queue,
    cc_run_id_t run_id
);

cc_result_t cc_run_queue_collect_timeout(
    cc_run_queue_t *queue,
    cc_run_id_t run_id,
    unsigned timeout_ms
);

cc_result_t cc_run_queue_abandon(cc_run_queue_t *queue, cc_run_id_t run_id);
cc_result_t cc_run_queue_shutdown(cc_run_queue_t *queue, unsigned timeout_ms);
void cc_run_queue_get_status(cc_run_queue_t *queue, cc_run_queue_status_t *out_status);


/* 同步运行任务；内部可复用 submit + collect 语义。 */
cc_result_t cc_run_queue_run(
    cc_run_queue_t *queue,
    const cc_run_queue_request_t *request,
    cc_run_queue_task_fn task,
    void *user_data
);


/* 中断某个 session 下的运行中/待处理任务。 */
cc_result_t cc_run_queue_interrupt_session(
    cc_run_queue_t *queue,
    const char *session_key
);

/* 中断指定 run；实现应触发 cancel token 并让任务在边界处退出。 */
cc_result_t cc_run_queue_interrupt_run(
    cc_run_queue_t *queue,
    cc_run_id_t run_id
);

/* 返回当前运行中的任务数量，用于测试、监控和背压判断。 */
size_t cc_run_queue_in_flight(cc_run_queue_t *queue);

/* 返回等待队列中的任务数量。 */
size_t cc_run_queue_pending(cc_run_queue_t *queue);

#ifdef __cplusplus
}
#endif

#endif
