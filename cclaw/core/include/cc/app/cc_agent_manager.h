

#ifndef CC_AGENT_MANAGER_H
#define CC_AGENT_MANAGER_H

#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_runtime_controller.h"
#include "cc/app/cc_run_queue.h"
#include "cc/core/cc_result.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * agent manager 不透明句柄。
 *
 * manager 在多个 agent runtime 之间做路由，并把 run queue 用作并发/串行化调度器。
 * 它仍是 SDK 组件，不提供 CLI/Web gateway。
 */
typedef struct cc_agent_manager cc_agent_manager_t;

typedef cc_result_t (*cc_agent_manager_runtime_acquire_fn)(
    void *context,
    cc_agent_runtime_t **out_runtime,
    void **out_lease);

typedef void (*cc_agent_manager_runtime_release_fn)(
    void *context,
    void *lease);

/*
 * agent manager 创建选项。
 *
 * default_runtime 是初始 agent；queue 可由外部传入或由 manager 创建，owns_queue 控制
 * destroy 时是否释放。default_agent_id 只在创建期间借用并由 manager 复制。
 */
typedef struct cc_agent_manager_options {
    cc_agent_runtime_t *default_runtime;
    cc_run_queue_t *queue;
    int owns_queue;
    const char *default_agent_id;
    cc_run_queue_action_t default_action;
} cc_agent_manager_options_t;

/* 单次提交选项，用于覆盖默认 action/lane。 */
typedef struct cc_agent_manager_submit_options {
    cc_run_queue_action_t action;
    cc_run_queue_lane_t lane;
} cc_agent_manager_submit_options_t;

/* 创建 agent manager；成功后调用方用 cc_agent_manager_destroy()。 */
cc_result_t cc_agent_manager_create(
    const cc_agent_manager_options_t *options,
    cc_agent_manager_t **out_manager
);

/* 销毁 manager；是否销毁 queue 取决于 owns_queue。 */
void cc_agent_manager_destroy(cc_agent_manager_t *manager);

/* 注册一个 agent runtime；manager 不拥有 runtime，只保存指针并按 agent_id 路由。 */
cc_result_t cc_agent_manager_add_agent(
    cc_agent_manager_t *manager,
    const char *agent_id,
    cc_agent_runtime_t *runtime
);

/* Register a runtime source whose lease remains held while a queued task can use it. */
cc_result_t cc_agent_manager_add_agent_lease(
    cc_agent_manager_t *manager,
    const char *agent_id,
    void *context,
    cc_agent_manager_runtime_acquire_fn acquire,
    cc_agent_manager_runtime_release_fn release
);

/* 同步处理消息，使用默认提交选项；out_response 由调用方 free()。 */
cc_result_t cc_agent_manager_handle_message(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    char **out_response
);

/* 同步处理消息并指定 lane/action。 */
cc_result_t cc_agent_manager_handle_message_with_options(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    const cc_agent_manager_submit_options_t *options,
    char **out_response
);

/* 同步 streaming 处理消息；chunk 通过 on_stream 携带 agent/session/run 元数据返回。 */
cc_result_t cc_agent_manager_handle_message_stream_with_options(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    const cc_agent_manager_submit_options_t *options,
    cc_runtime_controller_stream_fn on_stream,
    void *user_data,
    char **out_response
);


/* 异步提交消息处理任务；返回 run_id 供 collect/interrupt。 */
cc_result_t cc_agent_manager_submit(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    cc_run_id_t *out_run_id
);

/* 带提交选项的异步提交。 */
cc_result_t cc_agent_manager_submit_with_options(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    const cc_agent_manager_submit_options_t *options,
    cc_run_id_t *out_run_id
);

/* 收集异步 run 的结果；out_response 成功后由调用方 free()。 */
cc_result_t cc_agent_manager_collect(
    cc_agent_manager_t *manager,
    cc_run_id_t run_id,
    char **out_response
);

/* 设置当前 agent；后续 agent_id 为 NULL 的调用可使用当前 agent。 */
cc_result_t cc_agent_manager_set_current_agent(
    cc_agent_manager_t *manager,
    const char *agent_id
);

/* 切换当前 agent；语义与 set_current_agent 相同，便于上层表达用户操作。 */
cc_result_t cc_agent_manager_switch_agent(
    cc_agent_manager_t *manager,
    const char *agent_id
);

/* 返回当前 agent id；返回指针由 manager 拥有，调用方不能释放。 */
const char *cc_agent_manager_current_agent(cc_agent_manager_t *manager);

/* 中断指定 agent/session 的任务；通过 run queue cancel token 传播。 */
cc_result_t cc_agent_manager_interrupt(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id
);


/* 重置指定 session；通常清理 session store 中该 session 的历史。 */
cc_result_t cc_agent_manager_reset_session(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id
);

/* 构造 agent_id + session_id 的队列 session key；返回字符串由调用方 free()。 */
cc_result_t cc_agent_manager_make_session_key(
    const char *agent_id,
    const char *session_id,
    char **out_key
);

/* 列出已注册 agent id；返回数组和字符串由调用方释放。 */
cc_result_t cc_agent_manager_list_agents(
    cc_agent_manager_t *manager,
    char ***out_ids,
    size_t *out_count
);

#ifdef __cplusplus
}
#endif

#endif
