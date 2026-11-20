

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

/**
 * @brief 按 agent ID 路由消息，并通过运行队列协调会话任务的管理器。
 * 路由表、当前 ID、待收集链表和会话重置 gate 由内部锁保护，runtime 及注册源上下文由外部拥有者管理。
 * 拥有者须关闭入口并排空 API 调用后销毁，回调中不能同步等待自身或销毁所在管理器。
 */
typedef struct cc_agent_manager cc_agent_manager_t;

/**
 * @brief 在管理器锁外取得本次任务使用的 runtime 与租约。
 * @param[in,out] context 注册时借用的源上下文，允许 NULL，由源自行同步。
 * @param[out] out_runtime 必填，成功提供覆盖租约使用期的借用 runtime。
 * @param[out] out_lease 必填，成功提供交给 release 的租约，NULL 表示无需归还。
 * @return 成功为 CC_OK，失败须自行清理本次资源，管理器直接传回拥有的错误结果。
 * @note 上下文与函数指针须覆盖已开始的 acquire、任务使用及 release，替换注册不撤销旧借用。
 */
typedef cc_result_t (*cc_agent_manager_runtime_acquire_fn)(
    void *context,
    cc_agent_runtime_t **out_runtime,
    void **out_lease);

/**
 * @brief 在管理器锁外归还一次成功取得的非 NULL 租约。
 * @param[in,out] context 对应 acquire 的借用上下文，存活至本次回调返回。
 * @param[in] lease 消费 acquire 交付的非 NULL 租约。
 * @post 该任务结束对租约和 runtime 的使用，源负责释放或递减其资源引用。
 */
typedef void (*cc_agent_manager_runtime_release_fn)(
    void *context,
    void *lease);

/** @brief 创建期间借用的选项，字符串会复制，runtime 和外部队列按字段约定管理。 */
typedef struct cc_agent_manager_options {
    cc_agent_runtime_t *default_runtime; /**< 可选初始 runtime，管理器借用，须覆盖接纳任务的使用期。 */
    cc_run_queue_t *queue; /**< NULL 创建自有默认队列，否则保持外部队列到管理器销毁完成。 */
    int owns_queue; /**< 非零表示成功创建管理器后接管外部队列，内部新建队列始终自有。 */
    const char *default_agent_id; /**< 初始 runtime 的 ID，NULL 使用 default，无初始 runtime 时忽略。 */
    cc_run_queue_action_t default_action; /**< 默认队列 action，未知值归一化为 STEER。 */
} cc_agent_manager_options_t;

/** @brief 调用期间借用的提交选项，覆盖默认 action 与 MAIN lane。 */
typedef struct cc_agent_manager_submit_options {
    cc_run_queue_action_t action; /**< 本次对同会话旧任务的处理方式，未知值按 STEER。 */
    cc_run_queue_lane_t lane; /**< 本次并发预算类别，非法值由队列拒绝。 */
} cc_agent_manager_submit_options_t;

/**
 * @brief 创建路由表、内部锁及默认运行队列，可同时注册初始 runtime。
 * @param[in] options 可选借用配置，NULL 创建空路由表和自有默认队列。
 * @param[out] out_manager 必填，成功写拥有的管理器，失败保留原槽值。
 * @return CC_OK 或分配、线程端口、队列及初始注册错误，失败清理本次已接管资源。
 * @note 外部队列一旦接管，后续初始注册失败也会销毁它。
 */
cc_result_t cc_agent_manager_create(
    const cc_agent_manager_options_t *options,
    cc_agent_manager_t **out_manager
);

/**
 * @brief 释放管理器的任务结果、输入副本和路由表，自有队列同时销毁。
 * @pre 拥有者关闭入口并排空 API 调用，外部借用队列保持到本次销毁返回。
 * @param[in] manager 消费拥有的管理器，允许 NULL，借用 runtime 不在此销毁。
 * @post 管理器及其返回的借用 ID 失效，尚未收集的响应和租约被释放。
 * @note 自有队列销毁或借用队列逐项取消/收集都会等待任务退出，当前线程须位于 worker 之外。
 * 外部已消费、放弃或清理的队列结果按缺失处理，此时管理器仍负责释放自身任务包装。
 */
void cc_agent_manager_destroy(cc_agent_manager_t *manager);

/**
 * @brief 复制 ID 注册裸借用 runtime，已有同名项会替换其后续提交来源。
 * @param[in,out] manager 存活管理器。
 * @param[in] agent_id 必填借用 C 字符串，允许空字符串，成功后管理器拥有副本。
 * @param[in] runtime 必填借用 runtime，外部保证覆盖已接纳任务的实际使用期。
 * @return CC_OK 或参数、分配错误，失败保留已有注册。
 * @note 已接纳任务继续使用原 runtime，替换不取消它们。
 */
cc_result_t cc_agent_manager_add_agent(
    cc_agent_manager_t *manager,
    const char *agent_id,
    cc_agent_runtime_t *runtime
);

/**
 * @brief 注册运行时租约源，每次消息提交或重置在锁外取得独立租约。
 * @param[in,out] manager 存活管理器。
 * @param[in] agent_id 必填借用 ID，复制新 ID 或替换同名源。
 * @param[in,out] context 借用源上下文，允许 NULL，寿命覆盖 acquire 及对应 release。
 * @param[in] acquire 必填获取函数，在管理器锁外调用。
 * @param[in] release 必填归还函数，仅对非 NULL 成功租约调用。
 * @return CC_OK 或参数、分配错误，失败保留已有来源。
 * @note 源负责自身并发安全，替换来源后，旧上下文仍须覆盖先前已开始的操作。
 */
cc_result_t cc_agent_manager_add_agent_lease(
    cc_agent_manager_t *manager,
    const char *agent_id,
    void *context,
    cc_agent_manager_runtime_acquire_fn acquire,
    cc_agent_manager_runtime_release_fn release
);

/**
 * @brief 使用默认提交选项接纳消息并无限等待收集。
 * @param[in,out] manager 存活管理器，覆盖整个调用。
 * @param[in] agent_id 可选借用 ID，NULL 使用当前 ID，再回退 default。
 * @param[in] session_id 必填借用会话 ID，接纳时复制，允许空字符串。
 * @param[in] user_input 必填借用输入，接纳时复制，允许空字符串。
 * @param[out] out_response 必填，收集开始后置 NULL，成功交付需 free 的响应，允许成功为空。
 * @return 提交错误或拥有的执行结果，调用者用 cc_result_free 清理，执行失败释放响应。
 * @note 提交失败保留输出槽值，同步等待须避免与当前 worker 或会话任务形成依赖环。
 */
cc_result_t cc_agent_manager_handle_message(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    char **out_response
);

/**
 * @brief 按指定 lane/action 接纳消息并无限等待收集。
 * @param[in,out] manager 存活管理器。
 * @param[in] agent_id 可选借用 ID，NULL 使用当前或 default。
 * @param[in] session_id 必填借用会话 ID，提交时复制。
 * @param[in] user_input 必填借用输入，提交时复制。
 * @param[in] options 可选借用选项，NULL 使用管理器默认 action 和 MAIN lane。
 * @param[out] out_response 必填，提交失败保留原值，进入收集后置 NULL，成功移交需 free 的响应。
 * @return 提交或执行结果归调用者 cc_result_free，失败时响应被释放。
 * @note 外层保证同步等待的资源依赖可完成，响应是否存在取决于 runtime。
 */
cc_result_t cc_agent_manager_handle_message_with_options(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    const cc_agent_manager_submit_options_t *options,
    char **out_response
);

/**
 * @brief 提交流式消息，在队列 worker 上转发数据块并同步收集完整响应。
 * @param[in,out] manager 覆盖同步调用的存活管理器。
 * @param[in] agent_id 可选借用 ID，NULL 使用当前或 default。
 * @param[in] session_id 必填会话 ID，提交时复制。
 * @param[in] user_input 必填输入，提交时复制。
 * @param[in] options 可选借用提交配置。
 * @param[in] on_stream 数据块回调，NULL 时转发桥接返回参数错误。
 * @param[in,out] user_data 回调借用上下文，允许 NULL，保持至同步调用返回。
 * @param[out] out_response 必填，提交失败保留原值，收集时置 NULL，成功交付需 free 的响应。
 * @return 参数、提交或执行结果，拥有的错误消息由调用者清理。
 * @note 事件、chunk 和元数据仅回调期间借用，runtime_generation 为零。回调可发中断，
 * 不能等待本任务、同步提交依赖同一队列预算的工作或销毁管理器。
 */
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


/**
 * @brief 使用默认选项复制输入并异步接纳消息。
 * @param[in,out] manager 存活管理器。
 * @param[in] agent_id 可选借用 ID，NULL 使用当前或 default。
 * @param[in] session_id 必填借用会话 ID，接纳时复制。
 * @param[in] user_input 必填借用输入，接纳时复制。
 * @param[out] out_run_id 必填，成功写非零队列 ID，失败保留原值。
 * @return CC_OK 表示已接纳，执行结果由后续 collect 取得，其他错误原样返回。
 */
cc_result_t cc_agent_manager_submit(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    cc_run_id_t *out_run_id
);

/**
 * @brief 按选项复制输入并异步接纳消息，runtime 租约覆盖任务使用期。
 * @param[in,out] manager 存活管理器。
 * @param[in] agent_id 可选借用 ID，NULL 使用当前或 default。
 * @param[in] session_id 必填借用会话 ID，允许空字符串，提交时复制。
 * @param[in] user_input 必填借用输入，允许空字符串，提交时复制。
 * @param[in] options 可选借用配置，NULL 使用默认 action 和 MAIN lane。
 * @param[out] out_run_id 必填，成功写非零 ID，失败保留原值。
 * @return 接纳为 CC_OK，所选存储端口和会话正在重置为 CC_ERR_INVALID_STATE，其他错误向上传递。
 * @note 接纳后任务可能立即执行，调用方须最终 collect 或按销毁约定清理管理器。
 */
cc_result_t cc_agent_manager_submit_with_options(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    const cc_agent_manager_submit_options_t *options,
    cc_run_id_t *out_run_id
);

/**
 * @brief 无限等待指定已提交任务，并消费管理器保存的响应与任务包装。
 * @param[in,out] manager 存活管理器，调用期间禁止销毁。
 * @param[in] run_id 非零队列 ID，由本管理器提交。
 * @param[out] out_response 必填，参数有效后置 NULL，仅执行成功时移交需 free 的响应。
 * @return 拥有的执行结果或参数/未找到错误，重复在途收集为 CC_ERR_INVALID_STATE，统一 cc_result_free。
 * @note 内部锁保证同一项只有一个在途 collector。外部队列消费、放弃或 TTL 清理可使结果缺失，
 * 此时释放管理器包装并返回 CC_ERR_NOT_FOUND，禁止从任务回调等待自身。
 */
cc_result_t cc_agent_manager_collect(
    cc_agent_manager_t *manager,
    cc_run_id_t run_id,
    char **out_response
);

/**
 * @brief 校验目标已注册后复制并替换默认路由 ID。
 * @param[in,out] manager 存活管理器。
 * @param[in] agent_id 必填借用 ID，成功后管理器拥有副本。
 * @return CC_OK 或参数、未找到、分配错误，失败保留当前 ID。
 */
cc_result_t cc_agent_manager_set_current_agent(
    cc_agent_manager_t *manager,
    const char *agent_id
);

/**
 * @brief 调用 set_current_agent 切换后续省略 ID 的路由目标。
 * @param[in,out] manager 存活管理器。
 * @param[in] agent_id 必填已注册 ID，调用期间借用。
 * @return 与 set_current_agent 相同，失败保留当前目标。
 */
cc_result_t cc_agent_manager_switch_agent(
    cc_agent_manager_t *manager,
    const char *agent_id
);

/**
 * @brief 借用当前路由 ID，尚未选择时返回静态 default 字符串。
 * @pre 外层排除并发切换当前 ID，借用期覆盖到下次切换或销毁前。
 * @param[in] manager 借用管理器，允许 NULL。
 * @return NULL 管理器返回 NULL，其余为只读借用字符串，调用者不可释放。
 */
const char *cc_agent_manager_current_agent(cc_agent_manager_t *manager);

/**
 * @brief 取消指定路由会话的等待任务并通知运行任务。
 * @param[in,out] manager 存活管理器。
 * @param[in] agent_id 可选借用 ID，NULL 使用当前或 default，无需已注册。
 * @param[in] session_id 必填借用会话 ID。
 * @return 参数有效且资源充足时 CC_OK，包括无匹配任务，其他错误向上传递。
 * @note 成功表示取消已发布，运行回调退出取决于 runtime，结果仍待 collect。
 */
cc_result_t cc_agent_manager_interrupt(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id
);


/**
 * @brief 拒绝同底层存储会话的新提交，取消并等待共享该会话的旧任务退出后清理历史。
 * @pre 当前线程位于目标任务的流式或 runtime 回调之外，外层协调直接操作 runtime/存储的入口。
 * @param[in,out] manager 存活管理器。
 * @param[in] agent_id 可选借用已注册 ID，NULL 使用当前或 default。
 * @param[in] session_id 必填借用会话 ID，传给队列与存储后端。
 * @return CC_OK 或参数、未找到、租约、分配、队列及存储错误，同存储会话重复 reset 返回 INVALID_STATE。
 * @note 以 store.self/vtable 两地址数值与原 session_id 匹配 gate，覆盖取消、无限等待及 clear_session。
 * 同 runtime 的别名、或不同 runtime 共享同端口的提交均被拒绝，其他端口或会话继续接纳。
 * 任务在 runtime 借用有效时快照 uintptr_t 身份，旧 pending 结果保留供 collect，重置只比较数值。
 * 不同端口包装共用物理目录由外层 owner 协调，已发布取消与存储副作用不会回滚。
 * 全部路径解除本次 gate 并归还租约，不支持 clear_session 时返回 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t cc_agent_manager_reset_session(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id
);

/**
 * @brief 转义 agent 的冒号和百分号，再拼接原 session，生成无歧义队列键。
 * @param[in] agent_id 借用 ID，NULL 使用 default。
 * @param[in] session_id 借用会话 ID，NULL 使用空字符串。
 * @param[out] out_key 必填，成功写需 free 的字符串，失败保留原值。
 * @return CC_OK 或参数、尺寸溢出、分配错误，溢出返回 CC_ERR_LIMIT_EXCEEDED。
 * @note agent 中冒号与百分号分别编码为 %3A 与 %25，首个原始冒号分隔 session。
 * session 原文保留，普通无特殊字节的 ID 仍输出 agent:session。
 */
cc_result_t cc_agent_manager_make_session_key(
    const char *agent_id,
    const char *session_id,
    char **out_key
);

/**
 * @brief 在锁内复制已注册 ID，按注册顺序提供独立快照。
 * @param[in] manager 存活管理器。
 * @param[out] out_ids 必填，参数有效后置 NULL，成功返回拥有的字符串数组。
 * @param[out] out_count 必填，参数有效后置零，成功写数组条数。
 * @return CC_OK 或参数、分配错误，空表成功返回 NULL/零，分配失败清理部分副本。
 * @note 调用者逐个 free 字符串后 free 数组。
 */
cc_result_t cc_agent_manager_list_agents(
    cc_agent_manager_t *manager,
    char ***out_ids,
    size_t *out_count
);

#ifdef __cplusplus
}
#endif

#endif
