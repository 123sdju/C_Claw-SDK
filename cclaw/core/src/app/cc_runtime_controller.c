#include "cc/app/cc_runtime_controller.h"

#include "cc/app/cc_cancel_token.h"
#include "cc/app/cc_runtime_builder.h"
#include "cc/core/cc_id.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_platform.h"
#include "cc/ports/cc_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cc_runtime_controller {
    cc_runtime_builder_t *builder; /**< 固定借用，寿命覆盖本对象。 */
    char *session_base; /**< 拥有的会话前缀副本。 */
    char *workspace_dir; /**< 拥有的工作区路径副本。 */
    char session_id[96]; /**< 当前选择，修改受 mutex 保护。 */
    unsigned long session_generation; /**< 本对象内会话选择版本。 */
    unsigned long active_runtime_generation; /**< 最近成功取得的运行时代际 ID。 */
    cc_runtime_controller_run_id_t active_run_id; /**< 已接纳运行 ID，零表示没有占用。 */
    int drop_late_events; /**< 是否按运行及代际身份过滤流事件。 */
    int fresh_session_on_start; /**< 创建时固定的 UUID 会话模式。 */
    int session_needs_create; /**< 当前选择尚待底层建立会话。 */
    int session_change_in_progress; /**< 会话创建或恢复正在锁外访问依赖。 */
    cc_runtime_controller_lifecycle_t lifecycle; /**< 锁内维护的接纳和排空状态。 */
    cc_cancel_source_t *active_cancel; /**< 已接纳运行拥有的取消源，锁内借出使用。 */
    cc_mutex_t mutex; /**< 保护生命周期、运行及会话状态。 */
    cc_cond_t state_changed; /**< 通知关闭等待者运行或会话变更已退出。 */
};

/**
 * @brief 直接比较创建时固定的 builder 借用身份。
 * @param[in] controller 借用控制器，可为 NULL，调用期间须存活。
 * @param[in] builder 待比较 builder，可为 NULL，调用期间须存活。
 * @return 两者均非空且身份一致返回非零，否则返回零，固定身份读取无需状态锁。
 */
int cc_runtime_controller_uses_builder(const cc_runtime_controller_t *controller,
                                       const cc_runtime_builder_t *builder)
{ return controller && builder && controller->builder==builder; }

typedef struct controller_stream_ctx {
    cc_runtime_controller_t *controller; /**< 同步运行期间借用的控制器。 */
    cc_runtime_controller_stream_fn on_stream; /**< 提交者提供的回调。 */
    void *user_data; /**< 提交者借出的回调上下文。 */
    cc_runtime_controller_run_id_t run_id; /**< 本次接纳运行 ID。 */
    unsigned long runtime_generation; /**< 本次持有的运行时代际 ID。 */
    char session_id[96]; /**< 本次运行固定的会话 ID 副本。 */
    cc_cancel_token_t *cancel_token; /**< 本次取消源中的借用令牌。 */
} controller_stream_ctx_t;

/**
 * @brief 选择非空会话前缀，缺省时使用静态 default 字符串。
 * @param[in] value 借用前缀，可为 NULL 或空字符串。
 * @return 输入的非空借用字符串或静态默认字符串，调用者保持输入寿命。
 */
static const char *session_base_or_default(const char *value)
{
    return value && value[0] ? value : "default";
}

/**
 * @brief 先复制字符串，成功后释放并替换原拥有字段。
 * @param[in,out] field 非空字段地址，原字段须为可 free 的拥有字符串或 NULL。
 * @param[in] value 调用期间借用的源字符串，NULL 按空串复制，可引用原字段。
 * @return 成功返回 CC_OK，分配失败返回 CC_ERR_OUT_OF_MEMORY 并保留原字段。
 */
static cc_result_t replace_owned_string(char **field, const char *value)
{
    char *copy = cc_copy_string(value ? value : "");
    if (!copy) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy controller config");
    free(*field);
    *field = copy;
    return cc_result_ok();
}

/**
 * @brief 保留完整版本后缀，按剩余容量选择会话前缀并拼接 ID。
 * @pre 调用者持有 mutex，或对象尚未发布给其他调用者。
 * @param[in,out] controller 非空对象，读取其前缀与版本并覆盖 session_id。
 * @post ID 为 NUL 结尾的前缀加完整版本号文本，过长前缀在 UTF-8 字符边界缩短。
 */
static void update_session_id_locked(cc_runtime_controller_t *controller)
{
    char suffix[sizeof(controller->session_id)];
    snprintf(suffix, sizeof(suffix), "-%lu", controller->session_generation);
    size_t suffix_length = strlen(suffix);
    size_t prefix_limit = sizeof(controller->session_id) - suffix_length - 1U;
    const char *base = session_base_or_default(controller->session_base);
    size_t prefix_length = strlen(base);
    if (prefix_length > prefix_limit) prefix_length = prefix_limit;
    while (prefix_length && (((unsigned char)base[prefix_length] & 0xc0U) == 0x80U))
        prefix_length--;
    memcpy(controller->session_id, base, prefix_length);
    memcpy(controller->session_id + prefix_length, suffix, suffix_length + 1U);
}

/**
 * @brief 生成 UUID 并保留其完整尾部，按剩余容量截取会话前缀。
 * @param[in] base 调用期间借用的前缀，NULL 或空串使用 default。
 * @param[out] out 非空输出缓冲区，失败后内容仅在返回 CC_OK 时可用。
 * @param[in] out_size 缓冲区总字节数，包含 NUL，须容纳 UUID、分隔符与结尾。
 * @return 返回 UUID 生成结果，容量或格式化失败返回 CC_ERR_LIMIT_EXCEEDED，临时 UUID 始终在本函数释放。
 */
static cc_result_t generate_fresh_session_id(const char *base, char *out, size_t out_size)
{
    char *uuid = NULL;
    cc_result_t rc = cc_id_generate_uuid_v4(NULL, &uuid);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    size_t uuid_len = strlen(uuid);
    if (out_size <= uuid_len + 1U) {
        free(uuid);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Session id buffer is too small");
    }
    size_t prefix_limit = out_size - uuid_len - 2U;
    int written = snprintf(out, out_size, "%.*s-%s", (int)prefix_limit,
        session_base_or_default(base), uuid);
    free(uuid);
    if (written < 0 || (size_t)written >= out_size) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Failed to format session id");
    }
    return cc_result_ok();
}

/**
 * @brief 使用已持有代际中的 runtime 建立会话。
 * @pre 对象尚未发布，或当前已登记运行或会话变更以保持 workspace_dir 稳定。
 * @param[in] controller 提供工作区副本的非空对象。
 * @param[in] generation 调用者已持有的代际，借用至本次调用结束。
 * @param[in] session_id 待建立会话的 ID，调用期间借用。
 * @return 无 runtime 返回 CC_ERR_INVALID_STATE，否则返回底层会话创建结果，空工作区让 runtime 使用默认配置。
 */
static cc_result_t create_session_on_generation(
    cc_runtime_controller_t *controller,
    cc_runtime_generation_t *generation,
    const char *session_id)
{
    cc_agent_runtime_t *runtime = cc_runtime_generation_runtime(generation);
    if (!runtime) return cc_result_error(CC_ERR_INVALID_STATE, "Runtime generation is unavailable");
    return cc_agent_runtime_create_session(runtime, session_id,
        controller->workspace_dir && controller->workspace_dir[0] ? controller->workspace_dir : NULL);
}

/**
 * @brief 分配同步状态与配置副本，取得初始代际后交出运行中的控制器。
 *
 * UUID 模式只选择未保存会话，普通模式立即调用会话创建。创建阶段尚无外部并发访问。
 * @param[in] builder 非空借用 builder，寿命覆盖本对象。
 * @param[in] config 可选配置，前缀和工作区复制，尾字段按 size 判断可用性。
 * @param[out] out_controller 必填，builder 与输出均有效时先置 NULL，成功取得拥有对象。
 * @return 返回分配、同步原语、ID、代际或会话创建结果。失败释放本次对象，底层已写会话由存储管理。
 */
cc_result_t cc_runtime_controller_create(
    cc_runtime_builder_t *builder,
    const cc_runtime_controller_config_t *config,
    cc_runtime_controller_t **out_controller)
{
    if (!builder || !out_controller) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid runtime controller create request");
    }
    *out_controller = NULL;
    cc_runtime_controller_t *controller = calloc(1, sizeof(*controller));
    if (!controller) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate runtime controller");
    controller->builder = builder;
    controller->session_generation = 1;
    controller->drop_late_events = config ? config->drop_late_events != 0 : 1;
    controller->fresh_session_on_start = config &&
        config->size >= offsetof(cc_runtime_controller_config_t, fresh_session_on_start) +
            sizeof(config->fresh_session_on_start) && config->fresh_session_on_start;
    controller->lifecycle = CC_RUNTIME_CONTROLLER_STARTING;

    cc_result_t rc = cc_mutex_create(&controller->mutex);
    if (rc.code != CC_OK) goto fail;
    rc = cc_cond_create(&controller->state_changed);
    if (rc.code != CC_OK) goto fail;
    rc = replace_owned_string(&controller->session_base,
        config ? config->session_base : "default");
    if (rc.code == CC_OK) {
        rc = replace_owned_string(&controller->workspace_dir,
            config ? config->workspace_dir : NULL);
    }
    if (rc.code != CC_OK) goto fail;
    if (controller->fresh_session_on_start) {
        rc = generate_fresh_session_id(controller->session_base,
            controller->session_id, sizeof(controller->session_id));
        if (rc.code != CC_OK) goto fail;
        controller->session_needs_create = 1;
    } else {
        update_session_id_locked(controller);
    }

    cc_runtime_generation_t *generation = NULL;
    rc = cc_runtime_builder_acquire_generation(builder, &generation);
    if (rc.code == CC_OK) {
        controller->active_runtime_generation = cc_runtime_generation_id(generation);
        if (!controller->session_needs_create) {
            rc = create_session_on_generation(controller, generation, controller->session_id);
        }
        cc_runtime_generation_release(generation);
    }
    if (rc.code != CC_OK) goto fail;
    controller->lifecycle = CC_RUNTIME_CONTROLLER_RUNNING;
    *out_controller = controller;
    return cc_result_ok();

fail:
    controller->lifecycle = CC_RUNTIME_CONTROLLER_FAILED;
    if (controller->state_changed) cc_cond_destroy(controller->state_changed);
    if (controller->mutex) cc_mutex_destroy(controller->mutex);
    free(controller->session_base);
    free(controller->workspace_dir);
    free(controller);
    return rc;
}

/**
 * @brief 取消检查通过后，在锁内决定是否转发本次运行的流事件。
 * @param[in] ctx 同步运行上下文，可为 NULL，controller 与令牌在运行结束前有效。
 * @return 允许转发返回非零，缺少上下文、已取消，或启用过滤且运行或代际不匹配时返回零。
 */
static int should_forward_chunk(controller_stream_ctx_t *ctx)
{
    if (!ctx || !ctx->controller || cc_cancel_token_is_cancelled(ctx->cancel_token)) return 0;
    int forward = 1;
    cc_mutex_lock(ctx->controller->mutex);
    if (ctx->controller->drop_late_events &&
        (ctx->controller->active_run_id != ctx->run_id ||
         ctx->controller->active_runtime_generation != ctx->runtime_generation)) {
        forward = 0;
    }
    cc_mutex_unlock(ctx->controller->mutex);
    return forward;
}

/**
 * @brief 为 runtime 片段附加固定会话和运行身份，再同步通知应用回调。
 * @param[in] chunk runtime 借出的片段，须非空，借用期覆盖本次回调。
 * @param[in,out] user_data 非空 controller_stream_ctx_t 借用上下文，持有应用回调和取消令牌。
 * @return 参数缺失返回 CC_ERR_INVALID_ARGUMENT，入口发现已取消返回 CC_ERR_CANCELLED，过滤丢弃或完成通知返回 CC_OK。
 * 应用回调在控制器锁外运行，事件及其借用字段仅在回调内有效。
 */
static cc_result_t controller_stream_cb(const cc_stream_chunk_t *chunk, void *user_data)
{
    controller_stream_ctx_t *ctx = (controller_stream_ctx_t *)user_data;
    if (!chunk || !ctx || !ctx->on_stream) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid controller stream callback");
    }
    if (cc_cancel_token_is_cancelled(ctx->cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "Runtime controller stream cancelled");
    }
    if (!should_forward_chunk(ctx)) return cc_result_ok();
    cc_runtime_controller_stream_event_t event = {0};
    event.size = sizeof(event);
    event.chunk = chunk;
    event.session_id = ctx->session_id;
    event.run_id = ctx->run_id;
    event.runtime_generation = ctx->runtime_generation;
    ctx->on_stream(&event, ctx->user_data);
    return cc_result_ok();
}

/**
 * @brief 接纳单个运行，登记取消源后在锁外取得代际并同步执行对应 runtime 路径。
 *
 * user_input、resume_run_id、manual_call 须恰有一项非空。登记 active_run_id 覆盖可能阻塞的
 * 代际取得、会话创建和所有回调，结束时清理活动状态并唤醒关闭等待者，再释放本次资源。
 * @param[in,out] controller 非空控制器，锁内检查生命周期、忙状态和预期会话。
 * @param[in] user_input 新文本输入，调用期间借用，非文本动作时为 NULL。
 * @param[in] resume_run_id 待恢复检查点 ID，须非空字符串，其他动作时为 NULL。
 * @param[in] start_run_id 新检查点 ID，可为 NULL，非空时须配合文本输入和 expected_session_id。
 * @param[in] expected_session_id 可选预期会话 ID，锁内核对当前选择，调用期间借用。
 * @param[in] manual_call 手动调用及字段的借用，其他动作时为 NULL。
 * @param[in] trusted_tool 可选手动工具实现借用，传给 runtime 核对并执行。
 * @param[in] on_stream 可选应用流回调，在锁外同步调用。
 * @param[in,out] user_data 应用回调的借用上下文，可为 NULL。
 * @param[out] out_response 文本或恢复动作的必填独立输出，基础参数通过后置 NULL，拥有字符串归调用者 free。
 * @param[out] out_tool_result 手动动作的必填独立输出，基础参数通过后清零，拥有字段归调用者清理。
 * @return 停止接纳返回状态错误，繁忙返回 CC_ERR_QUEUE_FULL，其余返回准备或 runtime 执行结果。
 * runtime 错误时也可能已有输出、历史或账本进展，后续取消保持已经确认的成功结果。
 */
static cc_result_t controller_execute(
    cc_runtime_controller_t *controller,
    const char *user_input,
    const char *resume_run_id,
    const char *start_run_id,
    const char *expected_session_id,
    const cc_tool_call_t *manual_call,
    const cc_tool_t *trusted_tool,
    cc_runtime_controller_stream_fn on_stream,
    void *user_data,
    char **out_response,
    cc_tool_result_t *out_tool_result)
{
    int actions=(user_input!=NULL)+(resume_run_id!=NULL)+(manual_call!=NULL);
    if (!controller || actions!=1 || (manual_call ? !out_tool_result : !out_response) ||
        (resume_run_id && !resume_run_id[0]) ||
        (start_run_id && (!user_input || !start_run_id[0] || !expected_session_id || !expected_session_id[0]))) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid runtime controller submit request");
    }
    if (out_response) *out_response = NULL;
    if (out_tool_result) memset(out_tool_result,0,sizeof(*out_tool_result));
    /* 代际取得可能阻塞，先登记已接纳工作，使 shutdown 能覆盖所有锁外依赖访问。 */
    cc_mutex_lock(controller->mutex);
    if (controller->lifecycle != CC_RUNTIME_CONTROLLER_RUNNING) {
        cc_mutex_unlock(controller->mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Runtime controller is not accepting runs");
    }
    if (controller->active_run_id != 0 || controller->session_change_in_progress) {
        cc_mutex_unlock(controller->mutex);
        return cc_result_error(CC_ERR_QUEUE_FULL, "Runtime controller already has an active run");
    }
    if (expected_session_id && strcmp(controller->session_id,expected_session_id)!=0) {
        cc_mutex_unlock(controller->mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Selected session changed before run submission");
    }
    cc_cancel_source_t *cancel_source = NULL;
    cc_result_t rc = cc_cancel_source_create(&cancel_source);
    if (rc.code != CC_OK) {
        cc_mutex_unlock(controller->mutex);
        return rc;
    }

    char session_id[sizeof(controller->session_id)];
    int create_session = 0;
    cc_runtime_controller_run_id_t run_id = 0;
    rc = cc_id_generate_u64(&run_id);
    if (rc.code != CC_OK) {
        cc_mutex_unlock(controller->mutex);
        cc_cancel_source_destroy(cancel_source);
        return rc;
    }
    cc_result_free(&rc);
    snprintf(session_id, sizeof(session_id), "%s", controller->session_id);
    create_session = controller->session_needs_create;
    controller->active_run_id = run_id;
    controller->active_cancel = cancel_source;
    cc_mutex_unlock(controller->mutex);

    cc_runtime_generation_t *generation = NULL;
    rc = cc_runtime_builder_acquire_generation(controller->builder, &generation);
    unsigned long runtime_generation = generation ? cc_runtime_generation_id(generation) : 0;
    if (rc.code == CC_OK) {
        cc_mutex_lock(controller->mutex);
        controller->active_runtime_generation = runtime_generation;
        cc_mutex_unlock(controller->mutex);
    }
    if (rc.code == CC_OK && create_session && resume_run_id) {
        rc = cc_result_error(CC_ERR_INVALID_STATE, "Select the historical session before resuming its run");
    } else if (rc.code == CC_OK && create_session) {
        rc = create_session_on_generation(controller, generation, session_id);
        if (rc.code == CC_OK) {
            cc_mutex_lock(controller->mutex);
            controller->session_needs_create = 0;
            cc_mutex_unlock(controller->mutex);
        }
    }
    cc_agent_runtime_t *runtime = generation ? cc_runtime_generation_runtime(generation) : NULL;
    cc_cancel_token_t *cancel_token = cc_cancel_source_token(cancel_source);
    if (rc.code == CC_OK && runtime && resume_run_id) {
        cc_run_checkpoint_t checkpoint = {0};
        rc = cc_agent_runtime_load_interrupted_run(runtime, resume_run_id, &checkpoint);
        if (rc.code == CC_OK && strcmp(checkpoint.session_id, session_id) != 0)
            rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Interrupted run belongs to another session");
        cc_run_checkpoint_cleanup(&checkpoint);
    }
    if (rc.code == CC_OK && runtime && manual_call) {
        cc_agent_runtime_run_options_t options={0};
        options.size=sizeof(options); options.cancel_token=cancel_token;
        rc=cc_agent_runtime_execute_manual_tool_with_options(runtime,session_id,manual_call,
                                                             trusted_tool,&options,out_tool_result);
    } else if (rc.code == CC_OK && runtime && (resume_run_id || start_run_id || cc_agent_runtime_supports_stream(runtime))) {
        controller_stream_ctx_t stream_ctx = {0};
        stream_ctx.controller = controller;
        stream_ctx.on_stream = on_stream;
        stream_ctx.user_data = user_data;
        stream_ctx.run_id = run_id;
        stream_ctx.runtime_generation = runtime_generation;
        stream_ctx.cancel_token = cancel_token;
        snprintf(stream_ctx.session_id, sizeof(stream_ctx.session_id), "%s", session_id);
        cc_agent_runtime_stream_options_t options = {0};
        options.size = sizeof(options);
        options.cancel_token = cancel_token;
        options.on_chunk = on_stream ? controller_stream_cb : NULL;
        options.user_data = &stream_ctx;
        if (resume_run_id) rc=cc_agent_runtime_resume_run(runtime,resume_run_id,&options,out_response);
        else if (start_run_id) rc=cc_agent_runtime_handle_message_with_run_id(runtime,session_id,user_input,
                                                                                start_run_id,&options,out_response);
        else rc=cc_agent_runtime_handle_message_stream_cb(runtime,session_id,user_input,&options,out_response);
    } else if (rc.code == CC_OK && runtime) {
        cc_agent_runtime_run_options_t options = {0};
        options.size = sizeof(options);
        options.cancel_token = cancel_token;
        rc = cc_agent_runtime_handle_message_with_options(
            runtime, session_id, user_input, &options, out_response);
    } else if (rc.code == CC_OK) {
        rc = cc_result_error(CC_ERR_INVALID_STATE, "Runtime generation has no runtime");
    }
    cc_mutex_lock(controller->mutex);
    if (controller->active_run_id == run_id) {
        controller->active_run_id = 0;
        controller->active_cancel = NULL;
    }
    cc_cond_broadcast(controller->state_changed);
    cc_mutex_unlock(controller->mutex);
    cc_cancel_source_destroy(cancel_source);
    if (generation) cc_runtime_generation_release(generation);

    /* 执行期间由 runtime 核对取消，已经确认成功的答复或工具结果保持原返回值。 */
    return rc;
}

/**
 * @brief 将普通文本提交交给统一运行接纳与执行路径。
 * @param[in,out] controller 非空控制器，须处于运行中且空闲。
 * @param[in] user_input 非空输入指针，调用期间借用，内容预算由 runtime 检查。
 * @param[in] on_stream 可选同步流事件回调。
 * @param[in,out] user_data 借用回调上下文，可为 NULL。
 * @param[out] out_response 必填独立输出，基础参数通过后置 NULL，非空结果或错误诊断归调用者 free。
 * @return 原样返回统一执行结果，错误时也须核对返回字符串及持久化进展。
 */
cc_result_t cc_runtime_controller_submit_text(
    cc_runtime_controller_t *controller, const char *user_input,
    cc_runtime_controller_stream_fn on_stream, void *user_data, char **out_response)
{
    return controller_execute(controller, user_input, NULL, NULL, NULL, NULL, NULL, on_stream, user_data, out_response, NULL);
}

/**
 * @brief 核对显式会话和检查点 ID 后提交新的文本运行。
 * @param[in,out] controller 非空、空闲且运行中的控制器。
 * @param[in] expected_session_id 非空预备会话 ID，接纳时与当前选择比较，调用期间借用。
 * @param[in] run_id 非空检查点 ID，调用期间借用，格式及已有状态由 runtime 核对。
 * @param[in] user_input 非空文本输入指针，调用期间借用。
 * @param[in] on_stream 可选同步流事件回调。
 * @param[in,out] user_data 借用回调上下文，可为 NULL。
 * @param[out] out_response 必填独立输出，ID 指针缺失且输出非空时先置 NULL，其他初始化沿用统一执行路径。
 * 非空返回字符串即使出错也归调用者 free。
 * @return 返回参数、会话选择或运行结果，失败可能已有检查点和历史写入。
 */
cc_result_t cc_runtime_controller_submit_text_with_run_id(
    cc_runtime_controller_t *controller, const char *expected_session_id,
    const char *run_id, const char *user_input, cc_runtime_controller_stream_fn on_stream,
    void *user_data, char **out_response)
{
    if (!run_id || !expected_session_id) {
        if (out_response) *out_response=NULL;
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid checkpointed submission");
    }
    return controller_execute(controller,user_input,NULL,run_id,expected_session_id,NULL,NULL,
                              on_stream,user_data,out_response,NULL);
}

/**
 * @brief 将指定检查点恢复交给统一接纳路径，并核对其属于当前会话。
 * @param[in,out] controller 非空、空闲且运行中的对象，当前选择须已建立会话。
 * @param[in] run_id 非空检查点运行 ID，调用期间借用。
 * @param[in] on_stream 可选同步流事件回调。
 * @param[in,out] user_data 借用回调上下文，可为 NULL。
 * @param[out] out_response 必填独立输出，基础参数通过后置 NULL，非空结果或诊断归调用者 free。
 * @return 返回接纳、检查点读取或恢复结果，错误时仍可能有持久化进展。
 */
cc_result_t cc_runtime_controller_resume_run(
    cc_runtime_controller_t *controller, const char *run_id,
    cc_runtime_controller_stream_fn on_stream, void *user_data, char **out_response)
{
    return controller_execute(controller, NULL, run_id, NULL, NULL, NULL, NULL, on_stream, user_data, out_response, NULL);
}

/**
 * @brief 经统一接纳路径执行当前会话的手动工具运行。
 * @param[in,out] controller 非空、空闲且运行中的对象，负责持有代际及取消源。
 * @param[in] call 借用工具调用，名称、JSON 参数及唯一 ID 由 runtime 核对。
 * @param[in] trusted_tool 可选借用实现，非空时 runtime 要求操作账本，NULL 时查注册表。
 * @param[out] out_result 必填独立输出，基础参数通过后清零，返回拥有字段由调用者 cc_tool_result_cleanup。
 * @return 返回接纳或工具路径结果，CC_OK 时须继续读取 out_result->ok，错误时也须清理输出并核对副作用。
 */
cc_result_t cc_runtime_controller_execute_manual_tool(cc_runtime_controller_t *controller,
    const cc_tool_call_t *call, const cc_tool_t *trusted_tool, cc_tool_result_t *out_result)
{ return controller_execute(controller,NULL,NULL,NULL,NULL,call,trusted_tool,NULL,NULL,NULL,out_result); }

/**
 * @brief 在互斥量保护下向匹配的活动取消源发信号。
 * @param[in,out] controller 非空控制器，取消源与活动 ID 的核对和使用均在锁内完成。
 * @param[in] run_id 非零控制器数值运行 ID。
 * @return 匹配并发出信号返回 CC_OK，活动已结束或 ID 不匹配返回 CC_ERR_NOT_FOUND，参数无效返回参数错误。
 */
cc_result_t cc_runtime_controller_cancel(
    cc_runtime_controller_t *controller,
    cc_runtime_controller_run_id_t run_id)
{
    if (!controller || run_id == 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid runtime cancel request");
    }
    cc_mutex_lock(controller->mutex);
    if (controller->active_run_id != run_id || !controller->active_cancel) {
        cc_mutex_unlock(controller->mutex);
        return cc_result_error(CC_ERR_NOT_FOUND, "Runtime run is not active");
    }
    cc_cancel_source_cancel(controller->active_cancel);
    cc_mutex_unlock(controller->mutex);
    return cc_result_ok();
}

/**
 * @brief 进入排空状态，取消已接纳运行并按预算等待运行或会话变更结束。
 *
 * 条件等待释放互斥量，每片最多 250 毫秒，重新核对活动状态与单调时钟预算。
 * @pre owner 已关闭外层入口并保持对象存活，等待发生在应用流回调之外。
 * @param[in,out] controller 非空对象，成功置 STOPPED，超时保留 DRAINING 供再次调用。
 * @param[in] timeout_ms 非负等待预算，单位毫秒，零时只核对当前是否已排空。
 * @return STOPPED 或排空成功返回 CC_OK，仍有已接纳工作返回 CC_ERR_TIMEOUT，参数无效返回参数错误。
 * 历史读取和其他 API 的外层调用寿命由 owner 协调，此函数保留对象资源。
 */
cc_result_t cc_runtime_controller_shutdown(cc_runtime_controller_t *controller, int timeout_ms)
{
    if (!controller || timeout_ms < 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid runtime shutdown request");
    }
    cc_mutex_lock(controller->mutex);
    if (controller->lifecycle == CC_RUNTIME_CONTROLLER_STOPPED) {
        cc_mutex_unlock(controller->mutex);
        return cc_result_ok();
    }
    controller->lifecycle = CC_RUNTIME_CONTROLLER_DRAINING;
    if (controller->active_cancel) cc_cancel_source_cancel(controller->active_cancel);
    uint64_t wait_started_ms = cc_platform_monotonic_ms();
    while (controller->active_run_id != 0 || controller->session_change_in_progress) {
        uint64_t elapsed_ms = cc_platform_monotonic_ms() - wait_started_ms;
        if (elapsed_ms >= (uint64_t)timeout_ms) break;
        uint64_t remaining_ms = (uint64_t)timeout_ms - elapsed_ms;
        int slice = remaining_ms > 250U ? 250 : (int)remaining_ms;
        (void)cc_cond_timedwait(controller->state_changed, controller->mutex, slice);
    }
    if (controller->active_run_id != 0 || controller->session_change_in_progress) {
        cc_mutex_unlock(controller->mutex);
        return cc_result_error(CC_ERR_TIMEOUT, "Runtime controller shutdown timed out");
    }
    controller->lifecycle = CC_RUNTIME_CONTROLLER_STOPPED;
    cc_cond_broadcast(controller->state_changed);
    cc_mutex_unlock(controller->mutex);
    return cc_result_ok();
}

/**
 * @brief shutdown 成功后释放控制器拥有的配置与同步资源。
 * @pre owner 已关闭外层入口、排空其他 API 及内部缓冲区借用，builder 仍存活。
 * 已接纳运行与会话变更由 shutdown 等待，销毁成功后外层不得再次使用句柄。
 * @param[in,out] controller 待销毁对象，可为 NULL。
 * @param[in] timeout_ms shutdown 的非负等待预算，单位毫秒。
 * @return NULL 或清理成功返回 CC_OK，shutdown 失败原样返回并保留对象以便重试。
 */
cc_result_t cc_runtime_controller_destroy(cc_runtime_controller_t *controller, int timeout_ms)
{
    if (!controller) return cc_result_ok();
    cc_result_t rc = cc_runtime_controller_shutdown(controller, timeout_ms);
    if (rc.code != CC_OK) return rc;
    cc_cancel_source_destroy(controller->active_cancel);
    cc_cond_destroy(controller->state_changed);
    cc_mutex_destroy(controller->mutex);
    free(controller->session_base);
    free(controller->workspace_dir);
    free(controller);
    return cc_result_ok();
}

/**
 * @brief 在锁内生成新选择并标记会话待创建，普通模式使用递增版本，UUID 模式生成新 ID。
 * @param[in,out] controller 非空且空闲的运行中对象，成功时更新 ID、版本和待创建标志。
 * @return 空对象返回 CC_ERR_INVALID_ARGUMENT，繁忙或关闭返回 CC_ERR_INVALID_STATE，ID 错误向上传递。
 * 成功返回 CC_OK 并仅更新 RAM 中的会话状态，失败保留原选择。
 */
cc_result_t cc_runtime_controller_start_new_session(cc_runtime_controller_t *controller)
{
    if (!controller) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null runtime controller");
    cc_mutex_lock(controller->mutex);
    if (controller->lifecycle != CC_RUNTIME_CONTROLLER_RUNNING) {
        cc_mutex_unlock(controller->mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Runtime controller is not running");
    }
    if (controller->active_run_id != 0 || controller->session_change_in_progress) {
        cc_mutex_unlock(controller->mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Cannot clear an active session");
    }
    char new_id[sizeof(controller->session_id)];
    if (controller->fresh_session_on_start) {
        cc_result_t rc = generate_fresh_session_id(controller->session_base, new_id, sizeof(new_id));
        if (rc.code != CC_OK) {
            cc_mutex_unlock(controller->mutex);
            return rc;
        }
        cc_result_free(&rc);
    }
    controller->session_generation++;
    controller->session_needs_create = 1;
    if (controller->fresh_session_on_start) {
        snprintf(controller->session_id, sizeof(controller->session_id), "%s", new_id);
    } else {
        update_session_id_locked(controller);
    }
    cc_mutex_unlock(controller->mutex);
    return cc_result_ok();
}

/**
 * @brief 通过兼容入口执行新会话切换。
 * @param[in,out] controller 待切换对象，接纳条件由 start_new_session 核对。
 * @return 原样返回 start_new_session 的结果，页面状态由应用层维护。
 */
cc_result_t cc_runtime_controller_clear_session(cc_runtime_controller_t *controller)
{
    return cc_runtime_controller_start_new_session(controller);
}

/**
 * @brief 预期会话仍匹配时，在代际中创建当前尚未持久化的会话。
 *
 * 锁内登记 session_change_in_progress，锁外取得代际并访问存储，最后清标志并唤醒 shutdown。
 * @param[in,out] controller 非空、空闲且运行中的对象，成功创建后清除 session_needs_create。
 * @param[in] expected_session_id 非空预期会话 ID，调用期间借用，锁内比较。
 * @return 已创建时直接返回 CC_OK，其余返回接纳、代际或底层创建结果。
 * 创建失败保留待创建标志，磁盘实际状态由存储结果确定。
 */
cc_result_t cc_runtime_controller_ensure_session(cc_runtime_controller_t *controller,
                                                  const char *expected_session_id)
{
    if (!controller || !expected_session_id || !expected_session_id[0])
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid session persistence request");
    char selected[sizeof(controller->session_id)];
    cc_mutex_lock(controller->mutex);
    if (controller->lifecycle!=CC_RUNTIME_CONTROLLER_RUNNING || controller->active_run_id ||
        controller->session_change_in_progress || strcmp(controller->session_id,expected_session_id)!=0) {
        cc_mutex_unlock(controller->mutex);
        return cc_result_error(CC_ERR_INVALID_STATE,"Selected session is active or changed");
    }
    if (!controller->session_needs_create) { cc_mutex_unlock(controller->mutex); return cc_result_ok(); }
    snprintf(selected,sizeof(selected),"%s",controller->session_id);
    controller->session_change_in_progress=1;
    cc_mutex_unlock(controller->mutex);

    cc_runtime_generation_t *generation=NULL;
    cc_result_t rc=cc_runtime_builder_acquire_generation(controller->builder,&generation);
    if (rc.code==CC_OK) rc=create_session_on_generation(controller,generation,selected);
    if (generation) cc_runtime_generation_release(generation);
    cc_mutex_lock(controller->mutex);
    if (rc.code==CC_OK) controller->session_needs_create=0;
    controller->session_change_in_progress=0;
    cc_cond_broadcast(controller->state_changed);
    cc_mutex_unlock(controller->mutex);
    return rc;
}

/**
 * @brief 持有一次 builder 代际引用，通过该代际的 store 列出会话。
 * @param[in] controller 非空借用对象，owner 须协调此读取与销毁。
 * @param[out] out_sessions 必填独立输出，基础参数通过后置 NULL，后端返回数组由调用者 cc_session_free_array。
 * @param[out] out_count 必填独立输出，基础参数通过后置零，后端填写数量。
 * @return 返回代际或后端结果，缺少列表入口返回 CC_ERR_UNSUPPORTED，错误时也清理后端实际交出的数组。
 */
cc_result_t cc_runtime_controller_list_sessions(
    cc_runtime_controller_t *controller, cc_session_t **out_sessions, size_t *out_count)
{
    if (!controller || !out_sessions || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid session list request");
    }
    *out_sessions = NULL;
    *out_count = 0;
    cc_runtime_generation_t *generation = NULL;
    cc_result_t rc = cc_runtime_builder_acquire_generation(controller->builder, &generation);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    cc_agent_runtime_t *runtime = cc_runtime_generation_runtime(generation);
    cc_session_store_t *store = runtime ? cc_agent_runtime_session_store(runtime) : NULL;
    if (!store || !store->vtable || !store->vtable->list_sessions) {
        rc = cc_result_error(CC_ERR_UNSUPPORTED, "Session store cannot list conversations");
    } else {
        rc = store->vtable->list_sessions(store->self, out_sessions, out_count);
    }
    cc_runtime_generation_release(generation);
    return rc;
}

/**
 * @brief 持有一次 builder 代际引用，通过该代际的 store 加载消息。
 * @param[in] controller 非空借用对象，owner 须协调此读取与销毁。
 * @param[in] session_id 非空目标会话 ID，调用期间借用。
 * @param[in] limit 原样传给后端的最近消息数量限制，负数表示全部。
 * @param[out] out_messages 必填独立输出，基础参数通过后置 NULL，数组由调用者逐项 cc_message_cleanup 后 free。
 * @param[out] out_count 必填独立输出，基础参数通过后置零，后端填写数量。
 * @return 返回代际或后端结果，缺少消息入口返回 CC_ERR_UNSUPPORTED，错误时也清理后端实际交出的数组。
 */
cc_result_t cc_runtime_controller_load_messages(
    cc_runtime_controller_t *controller, const char *session_id, int limit,
    cc_message_t **out_messages, size_t *out_count)
{
    if (!controller || !session_id || !session_id[0] || !out_messages || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid session history request");
    }
    *out_messages = NULL;
    *out_count = 0;
    cc_runtime_generation_t *generation = NULL;
    cc_result_t rc = cc_runtime_builder_acquire_generation(controller->builder, &generation);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    cc_agent_runtime_t *runtime = cc_runtime_generation_runtime(generation);
    cc_session_store_t *store = runtime ? cc_agent_runtime_session_store(runtime) : NULL;
    if (!store || !store->vtable || !store->vtable->load_messages) {
        rc = cc_result_error(CC_ERR_UNSUPPORTED, "Session store cannot load history");
    } else {
        rc = store->vtable->load_messages(store->self, session_id, limit, out_messages, out_count);
    }
    cc_runtime_generation_release(generation);
    return rc;
}

/**
 * @brief 先登记会话变更，再在锁外读取列表确认目标，仍在运行时提交新选择。
 * @param[in,out] controller 非空、空闲且运行中的对象，结束时清变更标志并唤醒 shutdown。
 * @param[in] session_id 非空、最多 95 字节的借用 ID，入口先复制以稳定跨锁使用。
 * @return 目标不存在返回 CC_ERR_NOT_FOUND，查询失败保留原选择，查询后已关闭返回 CC_ERR_CANCELLED。
 * 成功更新 ID、递增会话版本，并标记该历史会话已创建。
 */
cc_result_t cc_runtime_controller_resume_session(
    cc_runtime_controller_t *controller, const char *session_id)
{
    if (!controller || !session_id || !session_id[0] ||
        strlen(session_id) >= sizeof(controller->session_id)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid session to resume");
    }
    char requested_id[sizeof(controller->session_id)];
    snprintf(requested_id, sizeof(requested_id), "%s", session_id);
    cc_mutex_lock(controller->mutex);
    if (controller->lifecycle != CC_RUNTIME_CONTROLLER_RUNNING ||
        controller->active_run_id != 0 || controller->session_change_in_progress) {
        cc_mutex_unlock(controller->mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Session selection requires an idle controller");
    }
    controller->session_change_in_progress = 1;
    cc_mutex_unlock(controller->mutex);

    cc_session_t *sessions = NULL;
    size_t count = 0;
    cc_result_t rc = cc_runtime_controller_list_sessions(controller, &sessions, &count);
    int found = 0;
    if (rc.code == CC_OK) {
        for (size_t i = 0; i < count; i++) {
            if (sessions[i].id && strcmp(sessions[i].id, requested_id) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            cc_result_free(&rc);
            rc = cc_result_error(CC_ERR_NOT_FOUND, "Session was not found");
        }
    }
    cc_session_free_array(sessions, count);

    cc_mutex_lock(controller->mutex);
    if (rc.code == CC_OK && controller->lifecycle == CC_RUNTIME_CONTROLLER_RUNNING) {
        snprintf(controller->session_id, sizeof(controller->session_id), "%s", requested_id);
        controller->session_generation++;
        controller->session_needs_create = 0;
    } else if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = cc_result_error(CC_ERR_CANCELLED, "Controller stopped during session selection");
    }
    controller->session_change_in_progress = 0;
    cc_cond_broadcast(controller->state_changed);
    cc_mutex_unlock(controller->mutex);
    return rc;
}

/**
 * @brief 锁内比较配置身份，先准备新副本和 ID，再提交配置与会话状态。
 * @param[in,out] controller 非空、空闲且运行中的对象。
 * @param[in] config 非空借用配置，前缀或工作区指针为 NULL 时沿用原值，非空值复制。
 * drop_late_events 总是更新，UUID 会话模式保持创建时选择。
 * @return 成功返回 CC_OK，身份变化时递增版本并标记新会话待创建。
 * 参数、状态、复制或 UUID 错误保持原配置，返回对应错误。
 */
cc_result_t cc_runtime_controller_apply_config(
    cc_runtime_controller_t *controller,
    const cc_runtime_controller_config_t *config)
{
    if (!controller || !config) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid runtime controller config request");
    }
    cc_mutex_lock(controller->mutex);
    if (controller->lifecycle != CC_RUNTIME_CONTROLLER_RUNNING || controller->active_run_id != 0 ||
        controller->session_change_in_progress) {
        cc_mutex_unlock(controller->mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Controller config requires an idle running controller");
    }
    const char *base = config->session_base ? config->session_base : controller->session_base;
    const char *workspace = config->workspace_dir ? config->workspace_dir : controller->workspace_dir;
    int identity_changed = strcmp(base, controller->session_base) != 0 ||
        strcmp(workspace, controller->workspace_dir) != 0;
    if (!identity_changed) {
        controller->drop_late_events = config->drop_late_events != 0;
        cc_mutex_unlock(controller->mutex);
        return cc_result_ok();
    }
    char *new_base = cc_copy_string(base);
    char *new_workspace = cc_copy_string(workspace);
    if (!new_base || !new_workspace) {
        free(new_base);
        free(new_workspace);
        cc_mutex_unlock(controller->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to prepare runtime controller config");
    }
    char new_id[sizeof(controller->session_id)];
    if (controller->fresh_session_on_start) {
        cc_result_t rc = generate_fresh_session_id(new_base, new_id, sizeof(new_id));
        if (rc.code != CC_OK) {
            free(new_base);
            free(new_workspace);
            cc_mutex_unlock(controller->mutex);
            return rc;
        }
        cc_result_free(&rc);
    }
    free(controller->session_base);
    free(controller->workspace_dir);
    controller->session_base = new_base;
    controller->workspace_dir = new_workspace;
    controller->drop_late_events = config->drop_late_events != 0;
    controller->session_generation++;
    controller->session_needs_create = 1;
    if (controller->fresh_session_on_start) {
        snprintf(controller->session_id, sizeof(controller->session_id), "%s", new_id);
    } else {
        update_session_id_locked(controller);
    }
    cc_mutex_unlock(controller->mutex);
    return cc_result_ok();
}

/**
 * @brief 清零输出后在锁内复制状态与会话 ID，形成独立快照。
 * @param[in] controller 借用对象，可为 NULL，非空时 owner 须保证本次读取期间存活。
 * @param[out] out_status 可选输出，非空时先清零并设置 size。
 * @post 输出非空时获得状态快照，controller 为 NULL 时仅 size 非零。
 */
void cc_runtime_controller_get_status(
    cc_runtime_controller_t *controller,
    cc_runtime_controller_status_t *out_status)
{
    if (!out_status) return;
    memset(out_status, 0, sizeof(*out_status));
    out_status->size = sizeof(*out_status);
    if (!controller) return;
    cc_mutex_lock(controller->mutex);
    out_status->lifecycle = controller->lifecycle;
    out_status->active_run_id = controller->active_run_id;
    out_status->runtime_generation = controller->active_runtime_generation;
    out_status->session_generation = controller->session_generation;
    out_status->session_is_persisted = !controller->session_needs_create;
    snprintf(out_status->session_id, sizeof(out_status->session_id), "%s", controller->session_id);
    cc_mutex_unlock(controller->mutex);
}

/**
 * @brief 直接借出内部会话 ID 缓冲区。
 * @pre 外层同步会话修改、借用读取与销毁，此入口直接读取字段。
 * @param[in] controller 借用对象，可为 NULL。
 * @return 内部借用 ID，下一次会话修改或对象销毁后失效，NULL 对象返回 NULL。
 */
const char *cc_runtime_controller_session_id(cc_runtime_controller_t *controller)
{
    return controller ? controller->session_id : NULL;
}
