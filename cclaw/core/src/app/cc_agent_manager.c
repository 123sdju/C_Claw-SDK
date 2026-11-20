



#include "cc/app/cc_agent_manager.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_thread.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/** @brief 拥有 ID 副本的路由项，保存裸 runtime 或一组借用的租约源回调。 */
typedef struct cc_agent_manager_entry {
    char *id;
    cc_agent_runtime_t *runtime;
    void *lease_context;
    cc_agent_manager_runtime_acquire_fn acquire_runtime;
    cc_agent_manager_runtime_release_fn release_runtime;
} cc_agent_manager_entry_t;

/* 消息包装由待收集项拥有，队列 worker 只借用该指针。 */
typedef struct cc_agent_manager_message_task cc_agent_manager_message_task_t;

/** @brief 待收集项，拥有任务包装及 worker 交付的响应，run_id 在队列发布任务前写入。 */
typedef struct cc_agent_manager_pending_run {
    cc_run_id_t run_id;
    int collecting;
    char *response;
    cc_agent_manager_message_task_t *task;
    struct cc_agent_manager_pending_run *next;
} cc_agent_manager_pending_run_t;

/** @brief 活动 reset 栈帧提供的存储会话 gate，身份为地址数值，session 借用至移除节点。 */
typedef struct cc_agent_manager_reset_gate {
    uintptr_t store_self_id;
    uintptr_t store_vtable_id;
    const char *session_id;
    struct cc_agent_manager_reset_gate *next;
} cc_agent_manager_reset_gate_t;

/**
 * @brief 管理器拥有的路由、当前 ID 与待收集链表，内部 mutex 保护其结构和活动 reset gate。
 * queue 按 owns_queue 决定拥有权。任务响应借助队列完成/收集的同步关系发布。
 * 销毁时外层已关闭入口并排空 API，裸 runtime 与租约源上下文均由外部管理。
 */
struct cc_agent_manager {
    cc_agent_manager_entry_t *entries;
    size_t entry_count;
    size_t entry_capacity;
    cc_run_queue_t *queue;
    int owns_queue;
    char *current_agent_id;
    cc_run_queue_action_t default_action;
    cc_agent_manager_pending_run_t *pending_runs;
    cc_agent_manager_reset_gate_t *reset_gates;
    cc_mutex_t mutex;
};

/**
 * @brief 拥有输入副本和可选租约的任务包装，回调结束或清理时归还租约。
 * store 地址在 runtime 借用有效时转为数值，完成后仅比较身份，不解引用旧 runtime 或端口。
 */
struct cc_agent_manager_message_task {
    cc_agent_runtime_t *runtime;
    uintptr_t store_self_id;
    uintptr_t store_vtable_id;
    void *runtime_lease;
    void *lease_context;
    cc_agent_manager_runtime_release_fn release_runtime;
    char *agent_id;
    char *session_id;
    char *user_input;
    cc_agent_manager_pending_run_t *pending_run;
    cc_runtime_controller_stream_fn on_stream;
    void *stream_user_data;
    int stream;
};

/**
 * @brief 归还任务保存的非空 runtime 租约，并清空本地租约槽。
 * @param[in,out] task 借用任务，允许 NULL，调用方排除对同一任务的并发释放。
 * @post 非空租约至多归还一次，源回调在管理器锁外执行。
 */
static void release_task_runtime(cc_agent_manager_message_task_t *task)
{
    if (!task || !task->release_runtime || !task->runtime_lease) return;
    task->release_runtime(task->lease_context, task->runtime_lease);
    task->runtime_lease = NULL;
}

/** @brief worker 栈上的流式桥接上下文，runtime 同步调用期间借用。 */
typedef struct cc_agent_manager_stream_ctx {
    cc_agent_manager_message_task_t *task;
    cc_cancel_token_t *cancel_token;
} cc_agent_manager_stream_ctx_t;

/* 同步流式入口与异步入口共用的任务接纳函数，完整契约见下方定义。 */
static cc_result_t submit_message_internal(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    const cc_agent_manager_submit_options_t *options,
    int stream,
    cc_runtime_controller_stream_fn on_stream,
    void *stream_user_data,
    cc_run_id_t *out_run_id
);

/**
 * @brief 检查队列取消状态后，附加任务路由元数据并转发流块。
 * @param[in] chunk runtime 借用的非空流块，仅本次回调期间有效。
 * @param[in,out] user_data 借用的有效 cc_agent_manager_stream_ctx_t 栈上下文。
 * @return 转发成功为 CC_OK，缺少上下文/回调为参数错误，已取消为 CC_ERR_CANCELLED。
 * @note 用户回调在 worker 上锁外执行，事件及其指针不能跨回调保存，generation 固定为零。
 */
static cc_result_t manager_stream_cb(const cc_stream_chunk_t *chunk, void *user_data)
{
    cc_agent_manager_stream_ctx_t *ctx = (cc_agent_manager_stream_ctx_t *)user_data;
    if (!chunk || !ctx || !ctx->task || !ctx->task->on_stream) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid manager stream callback");
    }
    if (cc_cancel_token_is_cancelled(ctx->cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "Agent manager stream cancelled");
    }
    cc_runtime_controller_stream_event_t event;
    memset(&event, 0, sizeof(event));
    event.size = sizeof(event);
    event.chunk = chunk;
    event.agent_id = ctx->task->agent_id;
    event.session_id = ctx->task->session_id;
    event.run_id = ctx->task->pending_run ? ctx->task->pending_run->run_id : 0;
    event.runtime_generation = 0;
    ctx->task->on_stream(&event, ctx->task->stream_user_data);
    return cc_result_ok();
}

/**
 * @brief 在 worker 上同步执行消息，将响应留在待收集项并归还 runtime 租约。
 * @param[in,out] user_data 队列借用的非空任务包装，由管理器保持到任务结束。
 * @param[in] cancel_token 本次执行期借用的队列 token，传递给 runtime。
 * @return 拥有的 runtime 执行结果或开始前取消错误，交给队列保存。
 * @note 响应的释放/移交在 collect 或 destroy 中进行，失败响应也由该路径清理。
 */
static cc_result_t run_message_task(void *user_data, cc_cancel_token_t *cancel_token)
{
    cc_agent_manager_message_task_t *task = (cc_agent_manager_message_task_t *)user_data;
    cc_result_t rc;
    if (cc_cancel_token_is_cancelled(cancel_token)) {
        rc = cc_result_error(CC_ERR_CANCELLED, "Agent run cancelled before execution");
    } else if (task->stream) {
        cc_agent_runtime_stream_options_t options;
        memset(&options, 0, sizeof(options));
        options.size = sizeof(options);
        options.cancel_token = cancel_token;
        cc_agent_manager_stream_ctx_t stream_ctx;
        memset(&stream_ctx, 0, sizeof(stream_ctx));
        stream_ctx.task = task;
        stream_ctx.cancel_token = cancel_token;
        options.on_chunk = manager_stream_cb;
        options.user_data = &stream_ctx;
        rc = cc_agent_runtime_handle_message_stream_cb(
            task->runtime,
            task->session_id,
            task->user_input,
            &options,
            &task->pending_run->response
        );
    } else {
        cc_agent_runtime_run_options_t options;
        memset(&options, 0, sizeof(options));
        options.cancel_token = cancel_token;
        rc = cc_agent_runtime_handle_message_with_options(
            task->runtime,
            task->session_id,
            task->user_input,
            &options,
            &task->pending_run->response
        );
    }
    release_task_runtime(task);
    return rc;
}

/**
 * @brief 保留四种已知 action，将其余值归一化为 STEER。
 * @param[in] action 提交方提供的枚举值。
 * @return 队列可识别的 action。
 */
static cc_run_queue_action_t normalize_action(cc_run_queue_action_t action)
{
    switch (action) {
        case CC_RUN_QUEUE_ACTION_STEER:
        case CC_RUN_QUEUE_ACTION_FOLLOWUP:
        case CC_RUN_QUEUE_ACTION_COLLECT:
        case CC_RUN_QUEUE_ACTION_INTERRUPT:
            return action;
        default:
            return CC_RUN_QUEUE_ACTION_STEER;
    }
}

/**
 * @brief 在路由表已满时扩容，并将新增槽位置零。
 * @pre 调用方持有 manager->mutex。
 * @param[in,out] manager 非空管理器，成功时可能移动 entries 数组。
 * @return 存在空槽为一，分配失败为零且保留原数组。
 */
static int ensure_entry_capacity(cc_agent_manager_t *manager)
{
    if (manager->entry_count < manager->entry_capacity) return 1;
    size_t next_capacity = manager->entry_capacity ? manager->entry_capacity * 2 : 4;
    cc_agent_manager_entry_t *next = realloc(manager->entries, next_capacity * sizeof(*next));
    if (!next) return 0;
    memset(next + manager->entry_capacity, 0, (next_capacity - manager->entry_capacity) * sizeof(*next));
    manager->entries = next;
    manager->entry_capacity = next_capacity;
    return 1;
}

/**
 * @brief 按有效 ID 查找路由，NULL ID 使用当前 ID 或 default。
 * @pre 调用方持有 manager->mutex。
 * @param[in] manager 非空借用管理器。
 * @param[in] agent_id 可选借用 ID，比较期间有效。
 * @return 锁内借用的路由项，缺失返回 NULL，扩容或解锁后不得解引用旧表项。
 */
static cc_agent_manager_entry_t *find_entry(
    cc_agent_manager_t *manager,
    const char *agent_id)
{
    const char *effective_id = agent_id ? agent_id :
        (manager->current_agent_id ? manager->current_agent_id : "default");
    for (size_t i = 0; i < manager->entry_count; i++) {
        if (manager->entries[i].id && strcmp(manager->entries[i].id, effective_id) == 0) {
            return &manager->entries[i];
        }
    }
    return NULL;
}

/**
 * @brief 按存储端口身份与原会话 ID 检查活动重置，覆盖共享端口的路由别名。
 * @pre 调用方持有 manager->mutex。
 * @param[in] manager 存活借用管理器。
 * @param[in] store_self_id 租约有效时快照的端口 self 地址数值，仅用于身份比较。
 * @param[in] store_vtable_id 同时快照的 vtable 地址数值，不解引用。
 * @param[in] session_id 非空借用会话 ID。
 * @return 存在活动 gate 为一，否则为零。
 */
static int session_resetting_locked(cc_agent_manager_t *manager,
                                    uintptr_t store_self_id, uintptr_t store_vtable_id,
                                    const char *session_id)
{
    for (cc_agent_manager_reset_gate_t *gate = manager->reset_gates; gate; gate = gate->next) {
        if (gate->store_self_id == store_self_id && gate->store_vtable_id == store_vtable_id &&
            strcmp(gate->session_id, session_id) == 0) return 1;
    }
    return 0;
}

/**
 * @brief 将 agent 的冒号和百分号转义后拼接 session，生成无歧义队列键。
 * @param[in] agent_id 借用 ID，NULL 使用 default。
 * @param[in] session_id 借用会话 ID，NULL 使用空字符串。
 * @param[out] out_key 必填，成功写调用者需 free 的字符串，失败保留原值。
 * @return CC_OK 或输出参数、尺寸溢出、分配错误，失败保留输出原值。
 * @note agent 的冒号编码为 %3A、百分号编码为 %25，session 原文保持，普通 ID 仍为 agent:session。
 */
cc_result_t cc_agent_manager_make_session_key(
    const char *agent_id,
    const char *session_id,
    char **out_key
)
{
    if (!out_key) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null session key output");
    const char *aid = agent_id ? agent_id : "default";
    const char *sid = session_id ? session_id : "";
    size_t session_length = strlen(sid);
    if (session_length > SIZE_MAX - 2U) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Session key size overflow");
    }
    size_t len = session_length + 2U;
    for (const char *cursor = aid; *cursor; cursor++) {
        size_t encoded_length = (*cursor == ':' || *cursor == '%') ? 3U : 1U;
        if (len > SIZE_MAX - encoded_length) {
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Session key size overflow");
        }
        len += encoded_length;
    }
    char *key = malloc(len);
    if (!key) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate session key");
    char *output = key;
    for (const char *cursor = aid; *cursor; cursor++) {
        if (*cursor == ':' || *cursor == '%') {
            memcpy(output, *cursor == ':' ? "%3A" : "%25", 3U);
            output += 3U;
        } else {
            *output++ = *cursor;
        }
    }
    *output++ = ':';
    memcpy(output, sid, session_length + 1U);
    *out_key = key;
    return cc_result_ok();
}

/**
 * @brief 归还尚存租约并释放任务拥有的三份字符串及包装。
 * @pre 队列已结束对任务的使用，或任务提交失败且从未交付。
 * @param[in] task 消费任务包装，允许 NULL，不释放其 pending_run 或响应。
 * @post 任务指针和输入副本均失效。
 */
static void free_message_task(cc_agent_manager_message_task_t *task)
{
    if (!task) return;
    release_task_runtime(task);
    free(task->agent_id);
    free(task->session_id);
    free(task->user_input);
    free(task);
}

/**
 * @brief 在待收集链表查找指定队列 ID。
 * @pre 调用方持有 manager->mutex。
 * @param[in] manager 非空借用管理器。
 * @param[in] run_id 待匹配队列 ID。
 * @return 锁内借用项，未找到为 NULL。
 */
static cc_agent_manager_pending_run_t *find_pending_run_locked(
    cc_agent_manager_t *manager,
    cc_run_id_t run_id
)
{
    for (cc_agent_manager_pending_run_t *run = manager->pending_runs; run; run = run->next) {
        if (run->run_id == run_id) return run;
    }
    return NULL;
}

/**
 * @brief 摘除指定待收集项并清空其 next 连接。
 * @pre 调用方持有 manager->mutex，队列任务已结束才可释放返回项。
 * @param[in,out] manager 非空管理器，可能更新链表头或前驱。
 * @param[in] run_id 待摘除的队列 ID。
 * @return 调用方取得拥有的项，未找到为 NULL。
 */
static cc_agent_manager_pending_run_t *take_pending_run_locked(
    cc_agent_manager_t *manager,
    cc_run_id_t run_id
)
{
    cc_agent_manager_pending_run_t *prev = NULL;
    cc_agent_manager_pending_run_t *run = manager->pending_runs;
    while (run) {
        if (run->run_id == run_id) {
            if (prev) prev->next = run->next;
            else manager->pending_runs = run->next;
            run->next = NULL;
            return run;
        }
        prev = run;
        run = run->next;
    }
    return NULL;
}

/**
 * @brief 创建内部锁与队列接线，可注册初始 runtime 并复制当前 ID。
 * @param[in] options 可选借用配置，NULL 使用自有默认队列和空路由表。
 * @param[out] out_manager 必填，成功写拥有的管理器，失败保留原槽值。
 * @return CC_OK 或资源、队列、初始注册错误，清理本次已创建/接管资源。
 */
cc_result_t cc_agent_manager_create(
    const cc_agent_manager_options_t *options,
    cc_agent_manager_t **out_manager
)
{
    if (!out_manager) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null out_manager");
    cc_agent_manager_t *manager = calloc(1, sizeof(cc_agent_manager_t));
    if (!manager) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create agent manager");

    cc_result_t lock_rc = cc_mutex_create(&manager->mutex);
    if (lock_rc.code != CC_OK) {
        free(manager);
        return lock_rc;
    }
    manager->default_action = options ?
        normalize_action(options->default_action) : CC_RUN_QUEUE_ACTION_STEER;

    if (options && options->queue) {
        manager->queue = options->queue;
        manager->owns_queue = options->owns_queue ? 1 : 0;
    } else {
        cc_run_queue_config_t queue_config = cc_run_queue_default_config();
        cc_result_t rc = cc_run_queue_create(&queue_config, &manager->queue);
        if (rc.code != CC_OK) {
            cc_mutex_destroy(manager->mutex);
            free(manager);
            return rc;
        }
        manager->owns_queue = 1;
    }

    if (options && options->default_runtime) {
        const char *default_id = options->default_agent_id ? options->default_agent_id : "default";
        cc_result_t rc = cc_agent_manager_add_agent(manager, default_id, options->default_runtime);
        if (rc.code != CC_OK) {
            cc_agent_manager_destroy(manager);
            return rc;
        }
        manager->current_agent_id = cc_copy_string(default_id);
        if (!manager->current_agent_id) {
            cc_agent_manager_destroy(manager);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy current agent id");
        }
    }

    *out_manager = manager;
    return cc_result_ok();
}

/**
 * @brief 等待队列结束对本管理器任务的借用，再释放响应、租约与路由资源。
 * @pre 外层关闭入口并排空管理器 API，借用队列仍存活，当前线程位于队列 worker 之外。
 * @param[in] manager 消费拥有的管理器，允许 NULL。
 * @post 自有队列已销毁，借用队列只取消并收集本管理器任务，所有管理器借用失效。
 * @note 外部已消费、放弃或按 TTL 清理的队列结果可能缺失，此时回调已退出，可安全释放包装。
 */
void cc_agent_manager_destroy(cc_agent_manager_t *manager)
{
    if (!manager) return;
    if (manager->owns_queue) {
        cc_run_queue_destroy(manager->queue);
    } else {
        for (cc_agent_manager_pending_run_t *run = manager->pending_runs; run; run = run->next) {
            cc_result_t rc = cc_run_queue_interrupt_run(manager->queue, run->run_id);
            cc_result_free(&rc);
        }
        for (cc_agent_manager_pending_run_t *run = manager->pending_runs; run; run = run->next) {
            cc_result_t rc = cc_run_queue_collect(manager->queue, run->run_id);
            cc_result_free(&rc);
        }
    }
    cc_agent_manager_pending_run_t *run = manager->pending_runs;
    while (run) {
        cc_agent_manager_pending_run_t *next = run->next;
        free(run->response);
        free_message_task(run->task);
        free(run);
        run = next;
    }
    for (size_t i = 0; i < manager->entry_count; i++) {
        free(manager->entries[i].id);
    }
    free(manager->current_agent_id);
    free(manager->entries);
    cc_mutex_destroy(manager->mutex);
    free(manager);
}

/**
 * @brief 在锁内复制新 ID 或替换已有 ID 的裸 runtime 来源。
 * @param[in,out] manager 存活管理器。
 * @param[in] agent_id 必填借用 ID，允许空字符串。
 * @param[in] runtime 必填借用 runtime，外层保证覆盖已接纳任务。
 * @return CC_OK 或参数、扩容/复制分配错误，替换只影响后续取得来源的操作。
 */
cc_result_t cc_agent_manager_add_agent(
    cc_agent_manager_t *manager,
    const char *agent_id,
    cc_agent_runtime_t *runtime
)
{
    if (!manager || !agent_id || !runtime) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid agent registration");
    }
    cc_mutex_lock(manager->mutex);
    for (size_t i = 0; i < manager->entry_count; i++) {
        if (strcmp(manager->entries[i].id, agent_id) == 0) {
            manager->entries[i].runtime = runtime;
            manager->entries[i].lease_context = NULL;
            manager->entries[i].acquire_runtime = NULL;
            manager->entries[i].release_runtime = NULL;
            cc_mutex_unlock(manager->mutex);
            return cc_result_ok();
        }
    }
    if (!ensure_entry_capacity(manager)) {
        cc_mutex_unlock(manager->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow agent table");
    }
    manager->entries[manager->entry_count].id = cc_copy_string(agent_id);
    if (!manager->entries[manager->entry_count].id) {
        cc_mutex_unlock(manager->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy agent id");
    }
    manager->entries[manager->entry_count].runtime = runtime;
    manager->entry_count++;
    cc_mutex_unlock(manager->mutex);
    return cc_result_ok();
}

/**
 * @brief 在锁内注册租约来源，替换不回收旧上下文或已取得的租约。
 * @param[in,out] manager 存活管理器。
 * @param[in] agent_id 必填借用 ID，新注册时复制。
 * @param[in,out] context 外部拥有的源上下文，允许 NULL，覆盖旧操作退出和 release。
 * @param[in] acquire 必填获取函数，在管理器锁外调用。
 * @param[in] release 必填归还函数，在管理器锁外调用。
 * @return CC_OK 或参数、分配错误，失败保留原注册。
 */
cc_result_t cc_agent_manager_add_agent_lease(
    cc_agent_manager_t *manager,
    const char *agent_id,
    void *context,
    cc_agent_manager_runtime_acquire_fn acquire,
    cc_agent_manager_runtime_release_fn release)
{
    if (!manager || !agent_id || !acquire || !release) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid leased agent registration");
    }
    cc_mutex_lock(manager->mutex);
    cc_agent_manager_entry_t *entry = find_entry(manager, agent_id);
    if (!entry) {
        if (!ensure_entry_capacity(manager)) {
            cc_mutex_unlock(manager->mutex);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow agent table");
        }
        entry = &manager->entries[manager->entry_count];
        entry->id = cc_copy_string(agent_id);
        if (!entry->id) {
            cc_mutex_unlock(manager->mutex);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy agent id");
        }
        manager->entry_count++;
    }
    entry->runtime = NULL;
    entry->lease_context = context;
    entry->acquire_runtime = acquire;
    entry->release_runtime = release;
    cc_mutex_unlock(manager->mutex);
    return cc_result_ok();
}

/**
 * @brief 转交默认选项的同步接纳与收集路径。
 * @param[in,out] manager 存活管理器。
 * @param[in] agent_id 可选借用 ID，NULL 使用当前或 default。
 * @param[in] session_id 必填会话 ID，提交时复制。
 * @param[in] user_input 必填输入，提交时复制。
 * @param[out] out_response 必填，提交失败保留原值，收集后移交需 free 的成功响应。
 * @return 拥有的提交或执行结果，交由调用者 cc_result_free。
 */
cc_result_t cc_agent_manager_handle_message(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    char **out_response
)
{
    return cc_agent_manager_handle_message_with_options(
        manager, agent_id, session_id, user_input, NULL, out_response);
}

/**
 * @brief 校验响应槽后复用 submit 与 collect，同步等待本次队列任务。
 * @param[in,out] manager 存活管理器，外层排除销毁。
 * @param[in] agent_id 可选借用 ID，NULL 使用当前或 default。
 * @param[in] session_id 必填借用会话 ID。
 * @param[in] user_input 必填借用输入。
 * @param[in] options 可选借用提交配置，NULL 采用默认配置。
 * @param[out] out_response 必填，提交失败保留原值，收集时清零，成功移交需 free 的响应。
 * @return 参数/提交错误或任务拥有的执行结果，执行失败时响应被释放。
 * @note 等待无限期，调用方不得占用本次任务必须取得的 worker 或会话资源。
 */
cc_result_t cc_agent_manager_handle_message_with_options(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    const cc_agent_manager_submit_options_t *options,
    char **out_response
)
{
    if (!out_response) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null response output");
    }
    cc_run_id_t run_id = 0;
    cc_result_t rc = cc_agent_manager_submit_with_options(
        manager,
        agent_id,
        session_id,
        user_input,
        options,
        &run_id
    );
    if (rc.code != CC_OK) return rc;
    rc = cc_agent_manager_collect(manager, run_id, out_response);
    return rc;
}

/**
 * @brief 提交流式任务并无限等待收集，数据块在 worker 上转发。
 * @param[in,out] manager 存活管理器，覆盖本次同步调用。
 * @param[in] agent_id 可选借用 ID，NULL 使用当前或 default。
 * @param[in] session_id 必填会话 ID，提交时复制。
 * @param[in] user_input 必填输入，提交时复制。
 * @param[in] options 可选借用提交配置。
 * @param[in] on_stream 用户流回调，桥接层收到数据块时要求非 NULL。
 * @param[in,out] user_data 借用回调上下文，允许 NULL，覆盖同步调用。
 * @param[out] out_response 必填，提交失败保留原值，收集时置 NULL，成功响应交调用方 free。
 * @return 参数、提交或拥有的执行结果，失败收集会释放 runtime 返回的响应。
 * @note 用户回调仅借用事件，不能等待自身或销毁本队列/管理器。
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
)
{
    if (!out_response) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null streaming response output");
    }
    cc_run_id_t run_id = 0;
    cc_result_t rc = submit_message_internal(
        manager,
        agent_id,
        session_id,
        user_input,
        options,
        1,
        on_stream,
        user_data,
        &run_id);
    if (rc.code != CC_OK) return rc;
    return cc_agent_manager_collect(manager, run_id, out_response);
}

/**
 * @brief 使用默认 action 和 MAIN lane 异步接纳消息。
 * @param[in,out] manager 存活管理器。
 * @param[in] agent_id 可选借用 ID，NULL 使用当前或 default。
 * @param[in] session_id 必填借用会话 ID，提交时复制。
 * @param[in] user_input 必填借用输入，提交时复制。
 * @param[out] out_run_id 必填，成功写非零 ID，失败保留原值。
 * @return 接纳为 CC_OK，其余为提交错误，执行结果需另行 collect。
 */
cc_result_t cc_agent_manager_submit(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    cc_run_id_t *out_run_id
)
{
    return cc_agent_manager_submit_with_options(
        manager, agent_id, session_id, user_input, NULL, out_run_id);
}

/**
 * @brief 快照路由来源、取得租约并复制输入，将任务交给队列后挂入待收集链表。
 * @param[in,out] manager 存活管理器，本次 API 期间外层排除销毁。
 * @param[in] agent_id 可选借用 ID，NULL 在锁内选择当前或 default。
 * @param[in] session_id 必填借用会话 ID，复制入任务和队列键。
 * @param[in] user_input 必填借用输入，复制入任务。
 * @param[in] options 可选借用配置，NULL 使用默认 action 和 MAIN lane。
 * @param[in] stream 非零启用流式路径。
 * @param[in] on_stream 流式路径使用的回调，普通路径传 NULL。
 * @param[in,out] stream_user_data 借用流式上下文，覆盖已接纳任务的回调，允许 NULL。
 * @param[out] out_run_id 必填，成功写 ID，失败保留原值。
 * @return CC_OK 或参数、路由、租约、分配及队列错误，目标底层存储会话正在重置返回 INVALID_STATE。
 * 失败归还本次已取得租约和副本。
 * @note 队列在自身锁内写 pending->run_id 后唤醒 worker，避免首块事件读取未发布 ID。
 * 取得 runtime 后快照 store.self/vtable 的 uintptr_t 身份，再在同一准入临界区完成 gate
 * 检查、队列发布和 pending 登记，reset 不会漏掉共享端口的别名路由任务。
 */
static cc_result_t submit_message_internal(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    const cc_agent_manager_submit_options_t *options,
    int stream,
    cc_runtime_controller_stream_fn on_stream,
    void *stream_user_data,
    cc_run_id_t *out_run_id
)
{
    if (!manager || !session_id || !user_input || !out_run_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid agent submit request");
    }

    cc_mutex_lock(manager->mutex);
    const char *effective_agent_id = agent_id ? agent_id :
        (manager->current_agent_id ? manager->current_agent_id : "default");
    char *agent_id_copy = cc_copy_string(effective_agent_id);
    cc_agent_manager_entry_t *entry = find_entry(manager, effective_agent_id);
    cc_agent_runtime_t *runtime = entry ? entry->runtime : NULL;
    void *lease_context = entry ? entry->lease_context : NULL;
    cc_agent_manager_runtime_acquire_fn acquire_runtime = entry ? entry->acquire_runtime : NULL;
    cc_agent_manager_runtime_release_fn release_runtime = entry ? entry->release_runtime : NULL;
    cc_mutex_unlock(manager->mutex);
    if (!entry) {
        free(agent_id_copy);
        return cc_result_error(CC_ERR_NOT_FOUND, "Agent runtime not found");
    }
    if (!agent_id_copy) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy agent id");

    void *runtime_lease = NULL;
    if (acquire_runtime) {
        cc_result_t acquire_rc = acquire_runtime(lease_context, &runtime, &runtime_lease);
        if (acquire_rc.code != CC_OK) {
            free(agent_id_copy);
            return acquire_rc;
        }
    }
    if (!runtime) {
        if (release_runtime && runtime_lease) release_runtime(lease_context, runtime_lease);
        free(agent_id_copy);
        return cc_result_error(CC_ERR_INVALID_STATE, "Agent runtime lease is unavailable");
    }

    char *session_key = NULL;
    cc_result_t rc = cc_agent_manager_make_session_key(agent_id_copy, session_id, &session_key);
    if (rc.code != CC_OK) {
        if (release_runtime && runtime_lease) release_runtime(lease_context, runtime_lease);
        free(agent_id_copy);
        return rc;
    }

    cc_agent_manager_pending_run_t *pending = calloc(1, sizeof(*pending));
    cc_agent_manager_message_task_t *task = calloc(1, sizeof(*task));
    if (!pending || !task) {
        free(pending);
        free(task);
        free(session_key);
        if (release_runtime && runtime_lease) release_runtime(lease_context, runtime_lease);
        free(agent_id_copy);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate agent run task");
    }
    task->runtime = runtime;
    cc_session_store_t *store = cc_agent_runtime_session_store(runtime);
    task->store_self_id = (uintptr_t)store->self;
    task->store_vtable_id = (uintptr_t)store->vtable;
    task->runtime_lease = runtime_lease;
    task->lease_context = lease_context;
    task->release_runtime = release_runtime;
    task->agent_id = agent_id_copy;
    task->session_id = cc_copy_string(session_id);
    task->user_input = cc_copy_string(user_input);
    task->pending_run = pending;
    task->stream = stream ? 1 : 0;
    task->on_stream = on_stream;
    task->stream_user_data = stream_user_data;
    pending->task = task;
    if (!task->agent_id || !task->session_id || !task->user_input) {
        free_message_task(task);
        free(pending);
        free(session_key);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy agent run input");
    }

    cc_run_queue_request_t request;
    request.session_key = session_key;
    request.lane = options ? options->lane : CC_RUN_QUEUE_LANE_MAIN;
    request.action = options ? normalize_action(options->action) : manager->default_action;
    cc_mutex_lock(manager->mutex);
    if (session_resetting_locked(
            manager, task->store_self_id, task->store_vtable_id, task->session_id)) {
        cc_mutex_unlock(manager->mutex);
        free(session_key);
        free_message_task(task);
        free(pending);
        return cc_result_error(CC_ERR_INVALID_STATE, "Agent session is being reset");
    }
    rc = cc_run_queue_submit_with_token(
        manager->queue, &request, run_message_task, task, &pending->run_id);
    if (rc.code != CC_OK) {
        cc_mutex_unlock(manager->mutex);
        free(session_key);
        free_message_task(task);
        free(pending);
        return rc;
    }
    pending->next = manager->pending_runs;
    manager->pending_runs = pending;
    *out_run_id = pending->run_id;
    cc_mutex_unlock(manager->mutex);
    free(session_key);
    return cc_result_ok();
}

/**
 * @brief 以非流式模式调用共用接纳路径，成功后由 collect 消费结果。
 * @param[in,out] manager 存活管理器。
 * @param[in] agent_id 可选借用 ID，NULL 使用当前或 default。
 * @param[in] session_id 必填借用会话 ID，提交时复制。
 * @param[in] user_input 必填借用输入，提交时复制。
 * @param[in] options 可选借用提交配置。
 * @param[out] out_run_id 必填，成功写非零 ID，失败保留原值。
 * @return 接纳为 CC_OK，其他错误原样返回，拥有的结果消息由调用者清理。
 */
cc_result_t cc_agent_manager_submit_with_options(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    const cc_agent_manager_submit_options_t *options,
    cc_run_id_t *out_run_id
)
{
    return submit_message_internal(
        manager,
        agent_id,
        session_id,
        user_input,
        options,
        0,
        NULL,
        NULL,
        out_run_id);
}

/**
 * @brief 等待队列结果后摘除待收集项，成功移交响应，其余路径释放响应。
 * @pre 调用方保持管理器存活，不能从本任务回调等待自身。
 * @param[in,out] manager 存活管理器，调用期禁止销毁。
 * @param[in] run_id 本管理器成功提交的非零 ID。
 * @param[out] out_response 必填，参数有效后置 NULL，成功时取得需 free 的响应。
 * @return 拥有的队列结果或参数/未找到错误，重复在途收集返回 CC_ERR_INVALID_STATE。
 * @note 队列完成同步保证响应已写完，不能从本任务回调收集自身。
 */
cc_result_t cc_agent_manager_collect(
    cc_agent_manager_t *manager,
    cc_run_id_t run_id,
    char **out_response
)
{
    if (!manager || !out_response || run_id == 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid agent collect request");
    }
    *out_response = NULL;
    cc_mutex_lock(manager->mutex);
    cc_agent_manager_pending_run_t *pending = find_pending_run_locked(manager, run_id);
    if (pending && pending->collecting) {
        cc_mutex_unlock(manager->mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Agent run is already being collected");
    }
    if (pending) pending->collecting = 1;
    cc_mutex_unlock(manager->mutex);
    if (!pending) return cc_result_error(CC_ERR_NOT_FOUND, "Agent run id not found");

    cc_result_t rc = cc_run_queue_collect(manager->queue, run_id);

    cc_mutex_lock(manager->mutex);
    pending = take_pending_run_locked(manager, run_id);
    cc_mutex_unlock(manager->mutex);
    if (pending) {
        if (rc.code == CC_OK) {
            *out_response = pending->response;
            pending->response = NULL;
        }
        free(pending->response);
        free_message_task(pending->task);
        free(pending);
    }
    return rc;
}

/**
 * @brief 在锁内确认目标存在，再复制并替换当前 ID。
 * @param[in,out] manager 存活管理器。
 * @param[in] agent_id 必填借用的已注册 ID。
 * @return CC_OK 或参数、未找到、分配错误，失败保留旧 ID。
 */
cc_result_t cc_agent_manager_set_current_agent(
    cc_agent_manager_t *manager,
    const char *agent_id
)
{
    if (!manager || !agent_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid current agent request");
    }
    cc_mutex_lock(manager->mutex);
    if (!find_entry(manager, agent_id)) {
        cc_mutex_unlock(manager->mutex);
        return cc_result_error(CC_ERR_NOT_FOUND, "Agent runtime not found");
    }
    char *copy = cc_copy_string(agent_id);
    if (!copy) {
        cc_mutex_unlock(manager->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy agent id");
    }
    free(manager->current_agent_id);
    manager->current_agent_id = copy;
    cc_mutex_unlock(manager->mutex);
    return cc_result_ok();
}

/**
 * @brief 转交 set_current_agent，切换后续省略 ID 的路由目标。
 * @param[in,out] manager 存活管理器。
 * @param[in] agent_id 必填借用的已注册 ID。
 * @return 与 set_current_agent 相同。
 */
cc_result_t cc_agent_manager_switch_agent(
    cc_agent_manager_t *manager,
    const char *agent_id
)
{
    return cc_agent_manager_set_current_agent(manager, agent_id);
}

/**
 * @brief 返回当前 ID 的直接借用，未设置时使用静态 default。
 * @pre 调用者排除并发切换，返回借用仅保持至下次切换或销毁。
 * @param[in] manager 借用管理器，允许 NULL。
 * @return NULL 管理器返回 NULL，其余指针不可释放。
 */
const char *cc_agent_manager_current_agent(cc_agent_manager_t *manager)
{
    if (!manager) return NULL;
    return manager->current_agent_id ? manager->current_agent_id : "default";
}

/**
 * @brief 锁内复制有效 agent ID，在锁外生成键并发布会话取消。
 * @param[in,out] manager 存活管理器。
 * @param[in] agent_id 可选借用 ID，NULL 使用当前或 default，不校验注册状态。
 * @param[in] session_id 必填借用会话 ID。
 * @return CC_OK 或参数、分配及队列错误，没有匹配任务也成功。
 * @note 取消发布后运行任务仍可使用 runtime 和输入，等待/运行结果继续由 collect 清理。
 */
cc_result_t cc_agent_manager_interrupt(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id
)
{
    if (!manager || !session_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid agent interrupt request");
    }
    cc_mutex_lock(manager->mutex);
    const char *effective_agent_id = agent_id ? agent_id :
        (manager->current_agent_id ? manager->current_agent_id : "default");
    char *agent_id_copy = cc_copy_string(effective_agent_id);
    cc_mutex_unlock(manager->mutex);
    if (!agent_id_copy) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy agent id");
    char *session_key = NULL;
    cc_result_t rc = cc_agent_manager_make_session_key(agent_id_copy, session_id, &session_key);
    free(agent_id_copy);
    if (rc.code != CC_OK) return rc;
    rc = cc_run_queue_interrupt_session(manager->queue, session_key);
    free(session_key);
    return rc;
}

/**
 * @brief 按底层存储端口和会话建立 gate，取消并等待所有别名旧任务退出后清理历史。
 * @pre 调用线程位于目标会话任务回调之外，外层协调直接访问 runtime/存储的入口。
 * @param[in,out] manager 存活管理器。
 * @param[in] agent_id 可选借用 ID，NULL 使用当前或 default，须存在对应注册。
 * @param[in] session_id 必填借用会话 ID，覆盖本次同步调用。
 * @return CC_OK 或参数、未找到、分配、租约、队列及存储错误，同存储会话已在重置为 INVALID_STATE。
 * @note 用租约有效时保存的 uintptr_t 地址身份比较 self/vtable，不解引用已完成任务的旧 runtime。
 * 等待不消费 pending 结果，gate 期间共享同端口和会话的提交为 INVALID_STATE，独立端口继续接纳。
 * 逐运行 ID 取消，避免按路由队列键遗漏别名或影响新来源。不同端口包装共用目录由外层 owner 协调。
 * 所有退出路径解除本次 gate 并归还租约，已经发布的取消与存储副作用不会回滚。
 */
cc_result_t cc_agent_manager_reset_session(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id
)
{
    if (!manager || !session_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid agent reset request");
    }

    cc_mutex_lock(manager->mutex);
    const char *effective_agent_id = agent_id ? agent_id :
        (manager->current_agent_id ? manager->current_agent_id : "default");
    char *agent_id_copy = cc_copy_string(effective_agent_id);
    cc_agent_manager_entry_t *entry = find_entry(manager, effective_agent_id);
    cc_agent_runtime_t *runtime = entry ? entry->runtime : NULL;
    void *lease_context = entry ? entry->lease_context : NULL;
    cc_agent_manager_runtime_acquire_fn acquire_runtime = entry ? entry->acquire_runtime : NULL;
    cc_agent_manager_runtime_release_fn release_runtime = entry ? entry->release_runtime : NULL;
    cc_mutex_unlock(manager->mutex);
    if (!entry) {
        free(agent_id_copy);
        return cc_result_error(CC_ERR_NOT_FOUND, "Agent runtime not found");
    }
    if (!agent_id_copy) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy agent id");

    void *runtime_lease = NULL;
    if (acquire_runtime) {
        cc_result_t acquire_rc = acquire_runtime(lease_context, &runtime, &runtime_lease);
        if (acquire_rc.code != CC_OK) {
            free(agent_id_copy);
            return acquire_rc;
        }
    }
    if (!runtime) {
        if (release_runtime && runtime_lease) release_runtime(lease_context, runtime_lease);
        free(agent_id_copy);
        return cc_result_error(CC_ERR_INVALID_STATE, "Agent runtime lease is unavailable");
    }

    cc_session_store_t *store = cc_agent_runtime_session_store(runtime);
    uintptr_t store_self_id = (uintptr_t)store->self;
    uintptr_t store_vtable_id = (uintptr_t)store->vtable;
    cc_result_t rc = cc_result_ok();
    cc_agent_manager_reset_gate_t gate = { store_self_id, store_vtable_id, session_id, NULL };
    cc_run_id_t *run_ids = NULL;
    size_t run_count = 0;
    cc_mutex_lock(manager->mutex);
    if (session_resetting_locked(manager, store_self_id, store_vtable_id, session_id)) {
        cc_mutex_unlock(manager->mutex);
        rc = cc_result_error(CC_ERR_INVALID_STATE, "Agent session is already being reset");
        goto cleanup;
    }
    for (cc_agent_manager_pending_run_t *run = manager->pending_runs; run; run = run->next) {
        if (run->task->store_self_id == store_self_id &&
            run->task->store_vtable_id == store_vtable_id &&
            strcmp(run->task->session_id, session_id) == 0) run_count++;
    }
    if (run_count) {
        run_ids = calloc(run_count, sizeof(*run_ids));
        if (!run_ids) {
            cc_mutex_unlock(manager->mutex);
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy session run IDs");
            goto cleanup;
        }
        size_t index = 0;
        for (cc_agent_manager_pending_run_t *run = manager->pending_runs; run; run = run->next) {
            if (run->task->store_self_id == store_self_id &&
                run->task->store_vtable_id == store_vtable_id &&
                strcmp(run->task->session_id, session_id) == 0) run_ids[index++] = run->run_id;
        }
    }
    gate.next = manager->reset_gates;
    manager->reset_gates = &gate;
    cc_mutex_unlock(manager->mutex);

    for (size_t i = 0; rc.code == CC_OK && i < run_count; i++) {
        cc_result_t cancel_rc = cc_run_queue_interrupt_run(manager->queue, run_ids[i]);
        if (cancel_rc.code == CC_OK || cancel_rc.code == CC_ERR_NOT_FOUND) {
            cc_result_free(&cancel_rc);
        } else {
            cc_result_free(&rc);
            rc = cancel_rc;
        }
    }
    for (size_t i = 0; rc.code == CC_OK && i < run_count; i++) {
        cc_result_t wait_rc = cc_run_queue_wait(manager->queue, run_ids[i]);
        if (wait_rc.code == CC_OK || wait_rc.code == CC_ERR_NOT_FOUND) {
            cc_result_free(&wait_rc);
        } else {
            cc_result_free(&rc);
            rc = wait_rc;
        }
    }

    if (rc.code == CC_OK) {
        if (!store || !store->vtable || !store->vtable->clear_session) {
            rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Session store does not support reset");
        } else {
            rc = store->vtable->clear_session(store->self, session_id);
        }
    }

    cc_mutex_lock(manager->mutex);
    cc_agent_manager_reset_gate_t **link = &manager->reset_gates;
    while (*link && *link != &gate) link = &(*link)->next;
    if (*link) *link = gate.next;
    cc_mutex_unlock(manager->mutex);

cleanup:
    free(run_ids);
    free(agent_id_copy);
    if (release_runtime && runtime_lease) release_runtime(lease_context, runtime_lease);
    return rc;
}

/**
 * @brief 持锁复制全部已注册 ID，失败清理部分副本。
 * @param[in] manager 存活管理器。
 * @param[out] out_ids 必填，参数有效后置 NULL，成功交付拥有的字符串数组。
 * @param[out] out_count 必填，参数有效后置零，成功写数组条数。
 * @return CC_OK 或参数、分配错误，空表成功返回 NULL/零。
 * @note 调用者逐个 free 字符串，再 free 数组，输出快照不依赖管理器后续寿命。
 */
cc_result_t cc_agent_manager_list_agents(
    cc_agent_manager_t *manager,
    char ***out_ids,
    size_t *out_count
)
{
    if (!manager || !out_ids || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid agent list request");
    }
    *out_ids = NULL;
    *out_count = 0;
    cc_mutex_lock(manager->mutex);
    if (manager->entry_count == 0) {
        cc_mutex_unlock(manager->mutex);
        return cc_result_ok();
    }
    char **ids = calloc(manager->entry_count, sizeof(char *));
    if (!ids) {
        cc_mutex_unlock(manager->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate agent list");
    }
    for (size_t i = 0; i < manager->entry_count; i++) {
        ids[i] = cc_copy_string(manager->entries[i].id ? manager->entries[i].id : "");
        if (!ids[i]) {
            for (size_t j = 0; j < i; j++) free(ids[j]);
            free(ids);
            cc_mutex_unlock(manager->mutex);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy agent id");
        }
    }
    *out_ids = ids;
    *out_count = manager->entry_count;
    cc_mutex_unlock(manager->mutex);
    return cc_result_ok();
}
