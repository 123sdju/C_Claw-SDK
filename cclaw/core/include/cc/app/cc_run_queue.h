

#ifndef CC_RUN_QUEUE_H
#define CC_RUN_QUEUE_H

#include "cc/core/cc_result.h"
#include <stdint.h>
#include "cc/app/cc_cancel_token.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 拥有共享 worker、作业及取消源的队列，入口和销毁寿命由外层 owner 协调。 */
typedef struct cc_run_queue cc_run_queue_t;

/** @brief 队列内唯一的非零运行 ID，用于收集、中断与放弃当前仍保留的作业。 */
typedef uint64_t cc_run_id_t;

/**
 * @brief 四类独立并发预算，共享最多 32 个 worker。
 * lane 限制该类任务的在途数量，实际运行还受共享 worker 和会话并发约束。
 * subagent、plugin 和 MCP 的业务含义由提交者实现。
 */
typedef enum cc_run_queue_lane {
    CC_RUN_QUEUE_LANE_MAIN = 0,
    CC_RUN_QUEUE_LANE_SUBAGENT = 1,
    CC_RUN_QUEUE_LANE_PLUGIN = 2,
    CC_RUN_QUEUE_LANE_MCP = 3
} cc_run_queue_lane_t;

/**
 * @brief 新提交对同会话旧作业的影响，所有 action 仍会提交本次任务。
 * STEER 与 INTERRUPT 取消旧等待项并向运行项发信号，COLLECT 只取消旧等待项，
 * FOLLOWUP 保留旧任务继续排队。取得已有结果由独立 collect API 完成。
 */
typedef enum cc_run_queue_action {
    CC_RUN_QUEUE_ACTION_STEER = 0,
    CC_RUN_QUEUE_ACTION_FOLLOWUP = 1,
    CC_RUN_QUEUE_ACTION_COLLECT = 2,
    CC_RUN_QUEUE_ACTION_INTERRUPT = 3
} cc_run_queue_action_t;

/**
 * @brief 创建时按值复制的并发、背压与结果保留配置。
 * 并发值小于等于零时按一处理。等待上限小于等于零、总量或 TTL 为零时停用对应限制。
 * 总量包含等待、运行和已完成作业，TTL 清理由后续提交及状态查询触发。
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

/** @brief 锁内复制的队列状态，各累计值统计接纳、完成、取消及成功放弃调用。 */
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

/**
 * @brief 提交调用期间借用的元数据，成功后队列拥有会话键副本。
 * session_key 按字节区分会话，允许空字符串，lane/action 使用下方枚举语义。
 */
typedef struct cc_run_queue_request {
    const char *session_key;
    cc_run_queue_lane_t lane;
    cc_run_queue_action_t action;
} cc_run_queue_request_t;

/**
 * @brief worker 锁外调用的普通任务。
 * @param[in] user_data 提交方拥有的借用上下文，允许 NULL，至少保持到任务结束。
 * @return 拥有的结果交给队列，最终由 collector 或清理路径归还消息。
 * @note 开始执行前检查取消，执行中途的退出由回调自身实现。
 */
typedef cc_result_t (*cc_run_queue_task_fn)(void *user_data);

/**
 * @brief worker 锁外调用并提供取消 token 的任务。
 * @param[in] user_data 提交方拥有的借用上下文，允许 NULL，保持到回调返回。
 * @param[in] cancel_token 本次调用期间借用的 token，由作业持有取消源。
 * @return 拥有的执行结果交给队列，观察到取消时可返回 CC_ERR_CANCELLED。
 * @note 取消检查与退出延迟由具体任务决定，回调返回后结束 token 借用。
 */
typedef cc_result_t (*cc_run_queue_task_with_token_fn)(
    void *user_data,
    cc_cancel_token_t *cancel_token
);

/**
 * @brief 取得四个 lane 和会话各并发一的默认配置。
 * @return 按值配置，等待上限每会话两项，总量 256 项，结果 TTL 300000 毫秒。
 */
cc_run_queue_config_t cc_run_queue_default_config(void);

/**
 * @brief 复制配置并创建同步资源和共享 worker。
 * @param[in] config 调用期间借用的配置，NULL 使用默认值。
 * @param[out] out_queue 必填，成功写拥有队列，失败保留原槽值。
 * @return 成功为 CC_OK，参数、分配或线程端口错误向上传递，部分创建失败时清理本次资源。
 */
cc_result_t cc_run_queue_create(
    const cc_run_queue_config_t *config,
    cc_run_queue_t **out_queue
);

/**
 * @brief 无限等待运行回调、join worker 并释放队列及剩余结果。
 * @pre 拥有者先关闭外部入口并排空其他 API 调用，当前线程不是本队列 worker。
 * @param[in] queue 消费拥有的队列，允许 NULL，业务上下文保持到回调退出。
 * @post 所有队列借用失效，拥有者清空自己的句柄并禁止后续调用。
 * @note 不响应取消的任务可能延长销毁等待，外层先 shutdown 可使用有限预算协调。
 */
void cc_run_queue_destroy(cc_run_queue_t *queue);


/**
 * @brief 接纳普通任务并管理适配包装，成功时交付运行 ID。
 * @pre 队列在本次 API 期间存活，业务上下文覆盖已接纳任务的使用期。
 * @param[in,out] queue 借用的队列。
 * @param[in] request 有效的借用请求，复制 session_key，lane 必须有效。
 * @param[in] task 非空普通回调，在 worker 上执行。
 * @param[in] user_data 业务方拥有的借用上下文，允许 NULL，队列不释放。
 * @param[out] out_run_id 必填，成功写非零 ID，失败保留原值。
 * @return CC_OK 表示接纳，其他情况返回参数、关闭、背压或资源错误。
 * @note 普通任务在开始前可取消，执行中途退出由其自身处理，容量检查先于 action。
 */
cc_result_t cc_run_queue_submit(
    cc_run_queue_t *queue,
    const cc_run_queue_request_t *request,
    cc_run_queue_task_fn task,
    void *user_data,
    cc_run_id_t *out_run_id
);


/**
 * @brief 接纳带取消 token 的任务，并为作业持有独立取消源。
 * @pre 队列在本次 API 期间存活，业务上下文保持到任务结束。
 * @param[in,out] queue 借用的队列。
 * @param[in] request 有效的借用元数据，复制 session_key。
 * @param[in] task 非空 worker 回调。
 * @param[in] user_data 借用的业务上下文，允许 NULL，队列不释放。
 * @param[out] out_run_id 必填，成功写运行 ID，失败保留原值。
 * @return 成功为 CC_OK，非法参数、关闭、容量及资源错误分别向上传递。
 */
cc_result_t cc_run_queue_submit_with_token(
    cc_run_queue_t *queue,
    const cc_run_queue_request_t *request,
    cc_run_queue_task_with_token_fn task,
    void *user_data,
    cc_run_id_t *out_run_id
);


/**
 * @brief 无限等待指定作业完成并消费其结果。
 * @pre 外层保持队列存活，等待不会形成对当前回调自身完成的依赖。
 * @param[in,out] queue 借用的队列。
 * @param[in] run_id 非零运行 ID。
 * @return 原任务拥有的结果，或参数/未找到错误，调用者统一 cc_result_free。
 */
cc_result_t cc_run_queue_collect(
    cc_run_queue_t *queue,
    cc_run_id_t run_id
);

/**
 * @brief 在预算内等待并收集结果，超时保留作业。
 * @pre 外层保持队列存活并排除销毁，同一 ID 最多一个 collector 成功消费。
 * @param[in,out] queue 借用的队列。
 * @param[in] run_id 非零运行 ID。
 * @param[in] timeout_ms 毫秒预算，UINT_MAX 无限等待，零只接受已完成项。
 * @return 原任务结果或参数/超时/未找到错误，返回的结果消息归调用者释放。
 * @note TTL 清理、放弃及竞争收集都可能使 ID 缺失。
 */
cc_result_t cc_run_queue_collect_timeout(
    cc_run_queue_t *queue,
    cc_run_id_t run_id,
    unsigned timeout_ms
);

/**
 * @brief 无限等待指定作业完成，保留作业和结果供之后收集。
 * @pre 外层保持队列存活，等待不能依赖当前回调自身完成。
 * @param[in,out] queue 借用的队列。
 * @param[in] run_id 非零运行 ID，等待期间可能被其他调用收集、放弃或清理。
 * @return 完成项存在为 CC_OK，参数非法或 ID 缺失返回对应错误，任务自身结果继续由队列拥有。
 * @note 多个等待者可观察完成，同一结果仍仅由一个成功 collector 消费。
 */
cc_result_t cc_run_queue_wait(cc_run_queue_t *queue, cc_run_id_t run_id);

/**
 * @brief 放弃结果，清理等待/完成项，或将运行项标记为结束后自清理。
 * @param[in,out] queue 借用且在调用期存活的队列。
 * @param[in] run_id 非零运行 ID。
 * @return 已找到为 CC_OK，缺失或参数非法分别返回对应错误。
 * @note 运行项仍可能使用业务上下文，成功返回只表明放弃请求已登记。
 */
cc_result_t cc_run_queue_abandon(cc_run_queue_t *queue, cc_run_id_t run_id);
/**
 * @brief 关闭新接纳，取消等待及运行项，并等待在途任务归零。
 * @pre 外层保持队列存活，当前线程不是待退出的本队列 worker。
 * @param[in,out] queue 借用的队列，超时后仍保持关闭，可再次等待。
 * @param[in] timeout_ms 毫秒预算，UINT_MAX 无限等待，零只检查当前是否排空。
 * @return 排空为 CC_OK，超时或参数非法返回对应错误。
 * @note 成功后仍有 worker 句柄和完成结果，由后续 destroy join 并清理。
 */
cc_result_t cc_run_queue_shutdown(cc_run_queue_t *queue, unsigned timeout_ms);
/**
 * @brief 清理已过 TTL 的结果后复制统计快照。
 * @param[in,out] queue 借用的存活队列，NULL 时只提供空状态。
 * @param[out] out_status 借用输出，NULL 直接返回，其他情况清零并设置 size。
 * @post 结果为一次锁内快照，过期完成作业可能已经从队列删除。
 */
void cc_run_queue_get_status(cc_run_queue_t *queue, cc_run_queue_status_t *out_status);


/**
 * @brief 接纳普通任务后立即无限等待收集结果。
 * @pre 队列及业务上下文覆盖同步调用，回调的等待依赖允许取得所需资源。
 * @param[in,out] queue 借用的队列。
 * @param[in] request 调用期间借用的请求。
 * @param[in] task 非空普通回调。
 * @param[in] user_data 借用的业务上下文，允许 NULL。
 * @return 提交错误或拥有的任务结果，调用者统一 cc_result_free。
 */
cc_result_t cc_run_queue_run(
    cc_run_queue_t *queue,
    const cc_run_queue_request_t *request,
    cc_run_queue_task_fn task,
    void *user_data
);


/**
 * @brief 取消同会话等待项，并向运行项发信号。
 * @param[in,out] queue 借用且在调用期存活的队列。
 * @param[in] session_key 非空指针的借用会话键。
 * @return 参数有效为 CC_OK，包括无匹配项，参数非法返回对应错误。
 * @note 运行项退出取决于回调检查，等待项保留取消结果供 collect。
 */
cc_result_t cc_run_queue_interrupt_session(
    cc_run_queue_t *queue,
    const char *session_key
);

/**
 * @brief 取消指定作业或保留其已经完成的结果。
 * @param[in,out] queue 借用且在调用期存活的队列。
 * @param[in] run_id 非零运行 ID。
 * @return 已找到为 CC_OK，缺失或参数非法返回对应错误。
 * @note 成功返回后运行回调仍可能在执行，资源寿命需等退出后再结束。
 */
cc_result_t cc_run_queue_interrupt_run(
    cc_run_queue_t *queue,
    cc_run_id_t run_id
);

/**
 * @brief 读取当前运行任务数量。
 * @param[in] queue 借用的存活队列，允许 NULL。
 * @return 一次锁内计数快照，NULL 返回零。
 */
size_t cc_run_queue_in_flight(cc_run_queue_t *queue);

/**
 * @brief 读取当前等待任务数量。
 * @param[in] queue 借用的存活队列，允许 NULL。
 * @return 一次锁内计数快照，NULL 返回零。
 */
size_t cc_run_queue_pending(cc_run_queue_t *queue);

#ifdef __cplusplus
}
#endif

#endif
