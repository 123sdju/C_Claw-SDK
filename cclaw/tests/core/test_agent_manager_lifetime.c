#include "cc/app/cc_agent_manager.h"
#include "cc/ports/cc_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "check failed at %d\n", __LINE__); return 1; } } while (0)

/**
 * @brief 创建测试拥有的内存会话后端。
 * @param[out] out_store 必填，成功写后端，最终通过其 destroy 释放。
 * @return 内存后端创建结果。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

/** @brief 测试拥有的组件和条件变量屏障，计数及屏障字段均由 mutex 保护。 */
typedef struct manager_fixture {
    cc_mutex_t mutex;
    cc_cond_t cond;
    cc_tool_registry_t *registry;
    cc_session_store_t store;
    cc_session_store_vtable_t store_vtable;
    cc_result_t (*base_clear)(void *self, const char *session_id);
    cc_agent_runtime_t *runtime;
    cc_run_queue_t *queue;
    cc_agent_manager_t *manager;
    int block_provider;
    int provider_active;
    cc_cancel_token_t *active_token;
    int provider_calls;
    int cancellation_seen;
    int allow_exit;
    int leases;
    int releases;
    int early_release;
    int check_release_active;
    int destroy_done;
    int stream_seen;
    int invalid_stream_id;
    cc_run_id_t stream_id;
    int collect_paused;
    int allow_collect_return;
    cc_run_id_t collect_run_id;
    cc_error_code_t collect_code;
    char *collect_response;
    int clear_entered;
    int allow_clear;
    int clear_failure;
    int reset_done;
    cc_error_code_t reset_code;
    int reenter_read_api;
    int read_api_failed;
    int read_api_calls;
} manager_fixture_t;

/* 测试串行创建 fixture，清理回调沿原后端 self 转发，独立保持对应 fixture。 */
static manager_fixture_t *clear_fixture;

#ifdef CC_TEST_MANAGER_QUEUE_PUBLICATION
/* 仅测试提交线程访问该指针，回调通过自己的 user_data 取得同一 fixture。 */
static manager_fixture_t *submit_barrier;
static manager_fixture_t *collect_barrier;

/**
 * @brief 声明由链接器 --wrap 转接的真实队列提交入口。
 * @param[in,out] queue 调用期间借用的队列。
 * @param[in] request 借用提交配置。
 * @param[in] task 非空任务回调。
 * @param[in,out] user_data 借用任务上下文。
 * @param[out] out_run_id 必填，成功写队列 ID。
 * @return 真实提交结果，成功后的上下文寿命由管理器保证。
 */
cc_result_t __real_cc_run_queue_submit_with_token(
    cc_run_queue_t *queue, const cc_run_queue_request_t *request,
    cc_run_queue_task_with_token_fn task, void *user_data, cc_run_id_t *out_run_id);

/**
 * @brief 在真实接纳后等待首个流事件，强制 worker 先于提交函数返回执行。
 * @param[in,out] queue 借用队列，转交真实入口。
 * @param[in] request 借用元数据，转交真实入口。
 * @param[in] task 真实 worker 回调。
 * @param[in,out] user_data 借用任务上下文。
 * @param[out] out_run_id 真实入口写入的输出槽。
 * @return 原样移交真实提交结果，启用屏障时首个事件必须在本函数返回前发生。
 */
cc_result_t __wrap_cc_run_queue_submit_with_token(
    cc_run_queue_t *queue, const cc_run_queue_request_t *request,
    cc_run_queue_task_with_token_fn task, void *user_data, cc_run_id_t *out_run_id)
{
    cc_result_t rc = __real_cc_run_queue_submit_with_token(
        queue, request, task, user_data, out_run_id);
    if (rc.code == CC_OK && submit_barrier) {
        cc_mutex_lock(submit_barrier->mutex);
        while (!submit_barrier->stream_seen) {
            cc_cond_wait(submit_barrier->cond, submit_barrier->mutex);
        }
        cc_mutex_unlock(submit_barrier->mutex);
    }
    return rc;
}

/**
 * @brief 声明链接器包装所调用的真实收集入口。
 * @param[in,out] queue 存活借用队列。
 * @param[in] run_id 本次待消费的队列 ID。
 * @return 真实拥有结果，最终由管理器调用者清理。
 */
cc_result_t __real_cc_run_queue_collect(cc_run_queue_t *queue, cc_run_id_t run_id);

/**
 * @brief 暂停已经成功消费队列结果的 collector，制造管理器摘链前的竞争窗口。
 * @param[in,out] queue 存活借用队列。
 * @param[in] run_id 待消费 ID，转交真实入口。
 * @return 原样移交真实拥有结果，测试屏障不消费诊断或响应。
 */
cc_result_t __wrap_cc_run_queue_collect(cc_run_queue_t *queue, cc_run_id_t run_id)
{
    cc_result_t rc = __real_cc_run_queue_collect(queue, run_id);
    if (rc.code == CC_OK && collect_barrier) {
        cc_mutex_lock(collect_barrier->mutex);
        collect_barrier->collect_paused = 1;
        cc_cond_broadcast(collect_barrier->cond);
        while (!collect_barrier->allow_collect_return) {
            cc_cond_wait(collect_barrier->cond, collect_barrier->mutex);
        }
        cc_mutex_unlock(collect_barrier->mutex);
    }
    return rc;
}
#endif

/**
 * @brief 读取成功状态并释放按值移交的诊断消息。
 * @param[in] rc 消费拥有结果，调用方不再使用原副本的消息。
 * @return 成功为一，失败为零。
 */
static int ok(cc_result_t rc)
{
    int success = rc.code == CC_OK;
    cc_result_free(&rc);
    return success;
}

/**
 * @brief 记录 provider 在途状态，启用时观察取消后继续等待主线程允许退出。
 * @param[in,out] fixture 借用且存活的屏障状态。
 * @param[in] request 借用模型请求，取消 token 由 runtime 保持存活。
 * @post provider 调用数增加，返回前清除在途标记，屏障可验证管理器等待与租约寿命。
 */
static void provider_barrier(manager_fixture_t *fixture, const cc_llm_chat_request_t *request)
{
    cc_mutex_lock(fixture->mutex);
    fixture->provider_calls++;
    fixture->provider_active = 1;
    fixture->active_token = request->cancel_token;
    cc_cond_broadcast(fixture->cond);
    if (fixture->block_provider) {
        while (!cc_cancel_token_is_cancelled(request->cancel_token)) {
            (void)cc_cond_timedwait(fixture->cond, fixture->mutex, 5);
        }
        fixture->cancellation_seen = 1;
        cc_cond_broadcast(fixture->cond);
        while (!fixture->allow_exit) cc_cond_wait(fixture->cond, fixture->mutex);
    }
    fixture->provider_active = 0;
    fixture->active_token = NULL;
    cc_mutex_unlock(fixture->mutex);
}

/**
 * @brief 执行测试屏障并生成固定同步响应。
 * @param[in,out] self 借用 manager_fixture_t。
 * @param[in] request 借用模型请求。
 * @param[out] out_response runtime 提供的响应，成功填充拥有的 ok 文本。
 * @return 响应构造结果。
 */
static cc_result_t fake_chat(void *self, const cc_llm_chat_request_t *request,
                             cc_llm_response_t *out_response)
{
    provider_barrier(self, request);
    cc_llm_response_init(out_response);
    return cc_llm_response_set_text(out_response, "ok");
}

/**
 * @brief 执行测试屏障后发出文本和完成流块。
 * @param[in,out] self 借用 manager_fixture_t。
 * @param[in] request 借用模型请求。
 * @param[in] on_chunk 必填 runtime 回调，调用期间借用栈上流块。
 * @param[in,out] user_data 借用 runtime 回调上下文。
 * @return 原样移交回调结果，文本回调失败时停止发送。
 */
static cc_result_t fake_chat_stream(void *self, const cc_llm_chat_request_t *request,
                                   cc_llm_stream_callback_fn on_chunk, void *user_data)
{
    provider_barrier(self, request);
    cc_stream_chunk_t text = { .type = CC_STREAM_CHUNK_TEXT, .content = "ok", .tool_index = -1 };
    cc_stream_chunk_t done = { .type = CC_STREAM_CHUNK_FINISHED, .tool_index = -1 };
    cc_result_t rc = on_chunk(&text, user_data);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    return on_chunk(&done, user_data);
}

static const cc_llm_provider_vtable_t provider_vtable = { fake_chat, fake_chat_stream, NULL, NULL };

/**
 * @brief 为一次管理器任务取得计数租约。
 * @param[in,out] context 借用 fixture，覆盖任务执行和归还。
 * @param[out] out_runtime 必填，写 fixture 拥有的 runtime。
 * @param[out] out_lease 必填，写非空 fixture 地址作为测试租约。
 * @return 始终 CC_OK。
 */
static cc_result_t acquire_runtime(void *context, cc_agent_runtime_t **out_runtime, void **out_lease)
{
    manager_fixture_t *fixture = context;
    cc_mutex_lock(fixture->mutex);
    fixture->leases++;
    cc_mutex_unlock(fixture->mutex);
    *out_runtime = fixture->runtime;
    *out_lease = fixture;
    return cc_result_ok();
}

/**
 * @brief 归还计数租约并记录 provider 尚在执行时的错误释放。
 * @param[in,out] context 借用 fixture。
 * @param[in] lease 消费本测试的非空计数租约，须等于 context。
 * @post 引用数减一，累计归还数增加，全部计数受 mutex 保护。
 */
static void release_runtime(void *context, void *lease)
{
    manager_fixture_t *fixture = context;
    cc_mutex_lock(fixture->mutex);
    if (lease != fixture || (fixture->check_release_active && fixture->provider_active)) {
        fixture->early_release = 1;
    }
    fixture->leases--;
    fixture->releases++;
    cc_mutex_unlock(fixture->mutex);
}

/**
 * @brief 暂停会话清理，供主线程核对重置 gate 在存储调用期间仍有效。
 * @param[in,out] self 基础内存后端的借用 self，透传原 clear_session。
 * @param[in] session_id 借用目标会话 ID。
 * @return 故障开关开启时返回 STORAGE，否则返回原后端清理结果。
 * @note 回调由 reset 同步调用，fixture 与原虚表保持到该调用返回。
 */
static cc_result_t tracked_clear(void *self, const char *session_id)
{
    manager_fixture_t *fixture = clear_fixture;
    cc_mutex_lock(fixture->mutex);
    fixture->clear_entered = 1;
    cc_cond_broadcast(fixture->cond);
    while (!fixture->allow_clear) cc_cond_wait(fixture->cond, fixture->mutex);
    int fail = fixture->clear_failure;
    cc_mutex_unlock(fixture->mutex);
    if (fail) return cc_result_error(CC_ERR_STORAGE, "Injected clear failure");
    return fixture->base_clear(self, session_id);
}

/**
 * @brief 创建共享 runtime、外部借用队列和租约路由，用于单个测试场景。
 * @param[out] fixture 必填零初始化输出，成功拥有组件和同步资源。
 * @return 成功零，断言失败一，测试进程在失败后结束。
 */
static int create_fixture(manager_fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    CHECK(ok(cc_mutex_create(&fixture->mutex)) && ok(cc_cond_create(&fixture->cond)));
    CHECK(ok(cc_tool_registry_create(&fixture->registry)));
    cc_tool_registry_freeze(fixture->registry);
    CHECK(ok(cc_memory_session_store_create(&fixture->store)));
    fixture->base_clear = fixture->store.vtable->clear_session;
    fixture->store_vtable = *fixture->store.vtable;
    fixture->store_vtable.clear_session = tracked_clear;
    fixture->store.vtable = &fixture->store_vtable;
    clear_fixture = fixture;
    cc_agent_runtime_deps_t deps = {0};
    deps.llm.self = fixture;
    deps.llm.vtable = &provider_vtable;
    deps.tool_registry = fixture->registry;
    deps.store = fixture->store;
    cc_agent_runtime_options_t options = {0};
    options.config.max_steps = 1;
    options.config.workspace_dir = ".";
    CHECK(ok(cc_agent_runtime_create(&deps, &options, &fixture->runtime)));
    CHECK(ok(cc_run_queue_create(NULL, &fixture->queue)));
    cc_agent_manager_options_t manager_options = {0};
    manager_options.queue = fixture->queue;
    CHECK(ok(cc_agent_manager_create(&manager_options, &fixture->manager)));
    CHECK(ok(cc_agent_manager_add_agent_lease(
        fixture->manager, "default", fixture, acquire_runtime, release_runtime)));
    return 0;
}

/**
 * @brief 在管理器已无并发 API 时按依赖顺序释放测试拥有组件。
 * @param[in,out] fixture 消费组件，manager 可已由专用销毁线程置 NULL。
 * @post 队列在管理器销毁后释放，runtime、存储、registry 和屏障随后释放。
 */
static void destroy_fixture(manager_fixture_t *fixture)
{
    cc_agent_manager_destroy(fixture->manager);
    cc_run_queue_destroy(fixture->queue);
    cc_agent_runtime_destroy(fixture->runtime);
    fixture->store.vtable->destroy(fixture->store.self);
    cc_tool_registry_destroy(fixture->registry);
    cc_cond_destroy(fixture->cond);
    cc_mutex_destroy(fixture->mutex);
}

/**
 * @brief 记录回调携带的运行 ID，并释放提交返回屏障。
 * @param[in] event 管理器借用的流事件，本函数只复制 ID。
 * @param[in,out] user_data 借用 fixture，覆盖同步流式调用。
 * @post 首个事件 ID 被保存，任何零 ID 或后续不一致都会设置错误标志。
 */
static void on_stream(const cc_runtime_controller_stream_event_t *event, void *user_data)
{
    manager_fixture_t *fixture = user_data;
    if (fixture->reenter_read_api) {
        char **ids = NULL;
        size_t count = 0;
        cc_result_t rc = cc_agent_manager_list_agents(fixture->manager, &ids, &count);
        if (rc.code != CC_OK || count == 0) fixture->read_api_failed = 1;
        fixture->read_api_calls++;
        cc_result_free(&rc);
        for (size_t i = 0; i < count; i++) free(ids[i]);
        free(ids);
    }
    cc_mutex_lock(fixture->mutex);
    if (!event->run_id || (fixture->stream_seen && fixture->stream_id != event->run_id)) {
        fixture->invalid_stream_id = 1;
    }
    fixture->stream_id = event->run_id;
    fixture->stream_seen = 1;
    cc_cond_broadcast(fixture->cond);
    cc_mutex_unlock(fixture->mutex);
}

/**
 * @brief 验证空响应输出在接纳前拒绝，以及首个流事件在 submit 返回前已有有效 ID。
 * @return 全部断言通过零，失败一，成功路径收集响应并释放组件。
 */
static int test_outputs_and_publication(void)
{
    manager_fixture_t fixture;
    CHECK(create_fixture(&fixture) == 0);
    CHECK(ok(cc_agent_runtime_create_session(fixture.runtime, "session", ".")));
    cc_result_t rc = cc_agent_manager_handle_message(fixture.manager, NULL, "session", "input", NULL);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT);
    cc_result_free(&rc);
    cc_run_queue_status_t status;
    cc_run_queue_get_status(fixture.queue, &status);
    CHECK(status.submitted == 0 && fixture.provider_calls == 0 && fixture.leases == 0);
    char *response = NULL;
#ifdef CC_TEST_MANAGER_QUEUE_PUBLICATION
    submit_barrier = &fixture;
#endif
    rc = cc_agent_manager_handle_message_stream_with_options(
        fixture.manager, NULL, "session", "input", NULL, on_stream, &fixture, &response);
#ifdef CC_TEST_MANAGER_QUEUE_PUBLICATION
    submit_barrier = NULL;
#endif
    CHECK(ok(rc) && response && strcmp(response, "ok") == 0);
    CHECK(fixture.stream_seen && fixture.stream_id && !fixture.invalid_stream_id);
    CHECK(fixture.releases == 1 && fixture.leases == 0 && !fixture.early_release);
    free(response);
    destroy_fixture(&fixture);
    return 0;
}

/**
 * @brief 在独立线程销毁管理器，返回后发布完成标记。
 * @param[in,out] arg 主线程借用的 fixture，覆盖本线程及 provider 回调。
 * @return NULL，主线程须 join 后释放 fixture 同步资源。
 */
static void *destroy_manager(void *arg)
{
    manager_fixture_t *fixture = arg;
    cc_agent_manager_destroy(fixture->manager);
    cc_mutex_lock(fixture->mutex);
    fixture->manager = NULL;
    fixture->destroy_done = 1;
    cc_cond_broadcast(fixture->cond);
    cc_mutex_unlock(fixture->mutex);
    return NULL;
}

/**
 * @brief 验证借用队列时销毁会等待在途任务，保持租约且保留队列可用状态。
 * @return 全部断言通过零，失败一，成功路径 join 后释放外部队列及 runtime。
 */
static int test_borrowed_queue_destroy(void)
{
    manager_fixture_t fixture;
    CHECK(create_fixture(&fixture) == 0);
    CHECK(ok(cc_agent_runtime_create_session(fixture.runtime, "blocked", ".")));
    fixture.block_provider = 1;
    fixture.check_release_active = 1;
    cc_run_id_t run_id = 0;
    CHECK(ok(cc_agent_manager_submit(fixture.manager, NULL, "blocked", "input", &run_id)));
    cc_mutex_lock(fixture.mutex);
    while (!fixture.provider_active) cc_cond_wait(fixture.cond, fixture.mutex);
    cc_mutex_unlock(fixture.mutex);
    cc_thread_t destroy_thread;
    CHECK(ok(cc_thread_create(destroy_manager, &fixture, &destroy_thread)));
    cc_mutex_lock(fixture.mutex);
    while (!fixture.cancellation_seen && !fixture.destroy_done) {
        cc_cond_wait(fixture.cond, fixture.mutex);
    }
    CHECK(!fixture.destroy_done && fixture.leases == 1 && !fixture.early_release);
    fixture.allow_exit = 1;
    cc_cond_broadcast(fixture.cond);
    cc_mutex_unlock(fixture.mutex);
    CHECK(ok(cc_thread_join(destroy_thread)));
    CHECK(fixture.destroy_done && fixture.leases == 0 && fixture.releases == 1 && !fixture.early_release);
    cc_run_queue_status_t status;
    cc_run_queue_get_status(fixture.queue, &status);
    CHECK(!status.shutting_down && status.total_tasks == 0);
    destroy_fixture(&fixture);
    return 0;
}

/**
 * @brief 在独立线程收集一个任务，并保存结果供主线程 join 后断言。
 * @param[in,out] arg 借用 fixture，包含管理器和已提交 ID。
 * @return NULL，拥有响应保存到 fixture，诊断消息在本线程释放。
 */
static void *collect_message(void *arg)
{
    manager_fixture_t *fixture = arg;
    cc_result_t rc = cc_agent_manager_collect(
        fixture->manager, fixture->collect_run_id, &fixture->collect_response);
    fixture->collect_code = rc.code;
    cc_result_free(&rc);
    return NULL;
}

/**
 * @brief 验证并发收集不能由队列失败方先释放成功方响应。
 * @return 全部断言通过零，失败一，成功响应在 join 后释放。
 * @note Linux 包装暂停队列成功方制造确定窗口，其他平台验证重复消费的顺序语义。
 */
static int test_collect_claim(void)
{
    manager_fixture_t fixture;
    CHECK(create_fixture(&fixture) == 0);
    CHECK(ok(cc_agent_runtime_create_session(fixture.runtime, "collect", ".")));
    CHECK(ok(cc_agent_manager_submit(
        fixture.manager, NULL, "collect", "input", &fixture.collect_run_id)));
#ifdef CC_TEST_MANAGER_QUEUE_PUBLICATION
    collect_barrier = &fixture;
#endif
    cc_thread_t collector;
    CHECK(ok(cc_thread_create(collect_message, &fixture, &collector)));
#ifdef CC_TEST_MANAGER_QUEUE_PUBLICATION
    cc_mutex_lock(fixture.mutex);
    while (!fixture.collect_paused) cc_cond_wait(fixture.cond, fixture.mutex);
    cc_mutex_unlock(fixture.mutex);
    char *second_response = (char *)1;
    cc_result_t second = cc_agent_manager_collect(
        fixture.manager, fixture.collect_run_id, &second_response);
    int rejected = second.code == CC_ERR_INVALID_STATE && second_response == NULL;
    cc_result_free(&second);
    cc_mutex_lock(fixture.mutex);
    fixture.allow_collect_return = 1;
    cc_cond_broadcast(fixture.cond);
    cc_mutex_unlock(fixture.mutex);
#endif
    CHECK(ok(cc_thread_join(collector)));
#ifdef CC_TEST_MANAGER_QUEUE_PUBLICATION
    collect_barrier = NULL;
    CHECK(rejected);
#else
    char *second_response = (char *)1;
    cc_result_t second = cc_agent_manager_collect(
        fixture.manager, fixture.collect_run_id, &second_response);
    CHECK(second.code == CC_ERR_NOT_FOUND && second_response == NULL);
    cc_result_free(&second);
#endif
    CHECK(fixture.collect_code == CC_OK && fixture.collect_response &&
          strcmp(fixture.collect_response, "ok") == 0);
    free(fixture.collect_response);
    destroy_fixture(&fixture);
    return 0;
}

/**
 * @brief 在独立线程重置 reset 会话，保存错误码并发布完成状态。
 * @param[in,out] arg 主线程保持存活的 fixture，provider 和 clear 屏障由主线程释放。
 * @return NULL，拥有诊断消息在本线程清理，主线程 join 后读取错误码。
 */
static void *reset_session(void *arg)
{
    manager_fixture_t *fixture = arg;
    cc_result_t rc = cc_agent_manager_reset_session(fixture->manager, NULL, "reset");
    cc_mutex_lock(fixture->mutex);
    fixture->reset_code = rc.code;
    fixture->reset_done = 1;
    cc_cond_broadcast(fixture->cond);
    cc_mutex_unlock(fixture->mutex);
    cc_result_free(&rc);
    return NULL;
}

/**
 * @brief 收集指定任务并核对成功文本或取消后的空响应。
 * @param[in,out] fixture 借用存活管理器及依赖。
 * @param[in] run_id 已由该管理器接纳的任务 ID。
 * @param[in] expected 预期 CC_OK 或 CC_ERR_CANCELLED。
 * @return 断言通过零，失败一，响应及结果消息均在本函数清理。
 */
static int expect_collected(manager_fixture_t *fixture, cc_run_id_t run_id, cc_error_code_t expected)
{
    char *response = NULL;
    cc_result_t rc = cc_agent_manager_collect(fixture->manager, run_id, &response);
    int matched = rc.code == expected;
    if (expected == CC_OK) matched = matched && response && strcmp(response, "ok") == 0;
    else matched = matched && response == NULL;
    cc_result_free(&rc);
    free(response);
    CHECK(matched);
    return 0;
}

/**
 * @brief 验证取消等待期间和存储清理期间的同会话 gate，以及旧结果保留和失败后解除。
 * @return 全部断言通过零，失败一，成功路径 join 重置线程并清理全部结果与依赖。
 * @note 其他会话保持可接纳，流回调重入只读 list_agents 不受全局重置停顿影响。
 */
static int test_reset_gate(void)
{
    manager_fixture_t fixture;
    CHECK(create_fixture(&fixture) == 0);
    CHECK(ok(cc_agent_runtime_create_session(fixture.runtime, "reset", ".")));
    CHECK(ok(cc_agent_runtime_create_session(fixture.runtime, "other", ".")));
    CHECK(ok(cc_agent_manager_add_agent(fixture.manager, "alias", fixture.runtime)));
    cc_agent_manager_submit_options_t options = { CC_RUN_QUEUE_ACTION_FOLLOWUP, CC_RUN_QUEUE_LANE_MAIN };
    cc_run_id_t completed = 0;
    CHECK(ok(cc_agent_manager_submit_with_options(
        fixture.manager, NULL, "reset", "complete", &options, &completed)));
    CHECK(ok(cc_run_queue_wait(fixture.queue, completed)));

    fixture.block_provider = 1;
    cc_run_id_t running = 0;
    cc_run_id_t queued = 0;
    CHECK(ok(cc_agent_manager_submit_with_options(
        fixture.manager, "alias", "reset", "running", &options, &running)));
    cc_mutex_lock(fixture.mutex);
    while (!fixture.provider_active) cc_cond_wait(fixture.cond, fixture.mutex);
    cc_mutex_unlock(fixture.mutex);
    CHECK(ok(cc_agent_manager_submit_with_options(
        fixture.manager, "alias", "reset", "queued", &options, &queued)));
    cc_thread_t reset_thread;
    CHECK(ok(cc_thread_create(reset_session, &fixture, &reset_thread)));
    cc_mutex_lock(fixture.mutex);
    while (!fixture.cancellation_seen) cc_cond_wait(fixture.cond, fixture.mutex);
    CHECK(!fixture.clear_entered && !fixture.reset_done);
    cc_mutex_unlock(fixture.mutex);

    cc_run_id_t rejected_id = 137;
    cc_result_t rejected = cc_agent_manager_submit(
        fixture.manager, NULL, "reset", "during wait", &rejected_id);
    CHECK(rejected.code == CC_ERR_INVALID_STATE && rejected_id == 137);
    cc_result_free(&rejected);
    rejected = cc_agent_manager_reset_session(fixture.manager, NULL, "reset");
    CHECK(rejected.code == CC_ERR_INVALID_STATE);
    cc_result_free(&rejected);
    cc_run_id_t other = 0;
    CHECK(ok(cc_agent_manager_submit(fixture.manager, NULL, "other", "unrelated", &other)));

    cc_mutex_lock(fixture.mutex);
    fixture.block_provider = 0;
    fixture.allow_exit = 1;
    cc_cond_broadcast(fixture.cond);
    while (!fixture.clear_entered) cc_cond_wait(fixture.cond, fixture.mutex);
    CHECK(!fixture.reset_done);
    cc_mutex_unlock(fixture.mutex);
    rejected = cc_agent_manager_submit(
        fixture.manager, NULL, "reset", "during clear", &rejected_id);
    CHECK(rejected.code == CC_ERR_INVALID_STATE && rejected_id == 137);
    cc_result_free(&rejected);
    CHECK(ok(cc_run_queue_wait(fixture.queue, other)));
    cc_mutex_lock(fixture.mutex);
    fixture.allow_clear = 1;
    cc_cond_broadcast(fixture.cond);
    cc_mutex_unlock(fixture.mutex);
    CHECK(ok(cc_thread_join(reset_thread)) && fixture.reset_code == CC_OK);

    CHECK(expect_collected(&fixture, completed, CC_OK) == 0);
    CHECK(expect_collected(&fixture, running, CC_ERR_CANCELLED) == 0);
    CHECK(expect_collected(&fixture, queued, CC_ERR_CANCELLED) == 0);
    CHECK(expect_collected(&fixture, other, CC_OK) == 0);
    cc_message_t *messages = NULL;
    size_t count = 0;
    CHECK(ok(fixture.store.vtable->load_messages(fixture.store.self, "reset", -1, &messages, &count)));
    CHECK(count == 0);
    free(messages);

    fixture.reenter_read_api = 1;
    char *response = NULL;
    CHECK(ok(cc_agent_manager_handle_message_stream_with_options(
        fixture.manager, NULL, "reset", "after reset", NULL, on_stream, &fixture, &response)));
    CHECK(response && strcmp(response, "ok") == 0 &&
          fixture.read_api_calls > 0 && !fixture.read_api_failed);
    free(response);
    fixture.clear_failure = 1;
    cc_result_t failure = cc_agent_manager_reset_session(fixture.manager, NULL, "reset");
    CHECK(failure.code == CC_ERR_STORAGE);
    cc_result_free(&failure);
    cc_run_id_t after_failure = 0;
    CHECK(ok(cc_agent_manager_submit(fixture.manager, NULL, "reset", "after failure", &after_failure)));
    CHECK(expect_collected(&fixture, after_failure, CC_OK) == 0);
    CHECK(fixture.leases == 0 && !fixture.early_release);
    destroy_fixture(&fixture);
    return 0;
}

/**
 * @brief 验证两个路由借用同一 runtime 时，别名不能越过目标底层会话的重置 gate。
 * @return gate 拒绝别名提交且保留 ID 输出时返回零，允许别名写入时返回一。
 * @note 清理回调阻塞期间尝试别名提交，旧实现会完成该消息并在随后 clear 时删除其历史。
 */
static int test_reset_alias_gate(void)
{
    manager_fixture_t fixture;
    CHECK(create_fixture(&fixture) == 0);
    CHECK(ok(cc_agent_runtime_create_session(fixture.runtime, "reset", ".")));
    CHECK(ok(cc_agent_manager_add_agent(fixture.manager, "alias", fixture.runtime)));
    cc_thread_t reset_thread;
    CHECK(ok(cc_thread_create(reset_session, &fixture, &reset_thread)));
    cc_mutex_lock(fixture.mutex);
    while (!fixture.clear_entered) cc_cond_wait(fixture.cond, fixture.mutex);
    cc_mutex_unlock(fixture.mutex);

    cc_run_id_t alias_id = 137;
    cc_result_t rc = cc_agent_manager_submit(fixture.manager, "alias", "reset", "alias input", &alias_id);
    int rejected = rc.code == CC_ERR_INVALID_STATE && alias_id == 137;
    int admitted = rc.code == CC_OK;
    cc_result_free(&rc);
    size_t records_before_clear = 0;
    if (admitted) {
        CHECK(expect_collected(&fixture, alias_id, CC_OK) == 0);
        cc_message_t *messages = NULL;
        CHECK(ok(fixture.store.vtable->load_messages(
            fixture.store.self, "reset", -1, &messages, &records_before_clear)));
        for (size_t i = 0; i < records_before_clear; i++) cc_message_cleanup(&messages[i]);
        free(messages);
    }
    cc_mutex_lock(fixture.mutex);
    fixture.allow_clear = 1;
    cc_cond_broadcast(fixture.cond);
    cc_mutex_unlock(fixture.mutex);
    CHECK(ok(cc_thread_join(reset_thread)) && fixture.reset_code == CC_OK);
    destroy_fixture(&fixture);
    if (!rejected) {
        fprintf(stderr, "alias admitted=%d, history records before clear=%zu\n", admitted, records_before_clear);
    }
    CHECK(rejected);
    return 0;
}

/**
 * @brief 验证不同 runtime 共享端口时被同一 gate 拒绝，独立端口的同名会话仍能完成。
 * @return 全部断言通过零，失败一，额外 runtime 与存储按借用关系先后释放。
 * @note 已完成任务的原 runtime 在 reset 前销毁，回归同时覆盖 pending 只用已保存身份的路径。
 */
static int test_reset_shared_store_gate(void)
{
    manager_fixture_t fixture;
    CHECK(create_fixture(&fixture) == 0);
    CHECK(ok(cc_agent_runtime_create_session(fixture.runtime, "reset", ".")));
    cc_session_store_t independent_store = {0};
    CHECK(ok(cc_memory_session_store_create(&independent_store)));
    cc_agent_runtime_deps_t deps = {0};
    deps.llm.self = &fixture;
    deps.llm.vtable = &provider_vtable;
    deps.tool_registry = fixture.registry;
    deps.store = fixture.store;
    cc_agent_runtime_options_t options = {0};
    options.config.max_steps = 1;
    options.config.workspace_dir = ".";
    cc_agent_runtime_t *shared_runtime = NULL;
    cc_agent_runtime_t *retired_runtime = NULL;
    cc_agent_runtime_t *independent_runtime = NULL;
    CHECK(ok(cc_agent_runtime_create(&deps, &options, &shared_runtime)));
    CHECK(ok(cc_agent_runtime_create(&deps, &options, &retired_runtime)));
    deps.store = independent_store;
    CHECK(ok(cc_agent_runtime_create(&deps, &options, &independent_runtime)));
    CHECK(ok(cc_agent_runtime_create_session(independent_runtime, "reset", ".")));
    CHECK(ok(cc_agent_manager_add_agent(fixture.manager, "shared", shared_runtime)));
    CHECK(ok(cc_agent_manager_add_agent(fixture.manager, "retired", retired_runtime)));
    CHECK(ok(cc_agent_manager_add_agent(fixture.manager, "independent", independent_runtime)));

    cc_run_id_t retired_id = 0;
    CHECK(ok(cc_agent_manager_submit(fixture.manager, "retired", "reset", "saved result", &retired_id)));
    CHECK(ok(cc_run_queue_wait(fixture.queue, retired_id)));
    CHECK(ok(cc_agent_manager_add_agent(fixture.manager, "retired", fixture.runtime)));
    cc_agent_runtime_destroy(retired_runtime);
    retired_runtime = NULL;

    cc_thread_t reset_thread;
    CHECK(ok(cc_thread_create(reset_session, &fixture, &reset_thread)));
    cc_mutex_lock(fixture.mutex);
    while (!fixture.clear_entered) cc_cond_wait(fixture.cond, fixture.mutex);
    cc_mutex_unlock(fixture.mutex);
    cc_run_id_t shared_id = 137;
    cc_result_t rc = cc_agent_manager_submit(
        fixture.manager, "shared", "reset", "shared input", &shared_id);
    int rejected = rc.code == CC_ERR_INVALID_STATE && shared_id == 137;
    int shared_admitted = rc.code == CC_OK;
    cc_result_free(&rc);
    if (shared_admitted) CHECK(expect_collected(&fixture, shared_id, CC_OK) == 0);

    cc_run_id_t independent_id = 0;
    rc = cc_agent_manager_submit(
        fixture.manager, "independent", "reset", "independent input", &independent_id);
    int independent_admitted = rc.code == CC_OK;
    cc_result_free(&rc);
    if (independent_admitted) CHECK(expect_collected(&fixture, independent_id, CC_OK) == 0);
    cc_mutex_lock(fixture.mutex);
    fixture.allow_clear = 1;
    cc_cond_broadcast(fixture.cond);
    cc_mutex_unlock(fixture.mutex);
    CHECK(ok(cc_thread_join(reset_thread)) && fixture.reset_code == CC_OK);
    CHECK(expect_collected(&fixture, retired_id, CC_OK) == 0);
    cc_message_t *messages = NULL;
    size_t count = 0;
    CHECK(ok(independent_store.vtable->load_messages(
        independent_store.self, "reset", -1, &messages, &count)));
    for (size_t i = 0; i < count; i++) cc_message_cleanup(&messages[i]);
    free(messages);

    cc_agent_manager_destroy(fixture.manager);
    fixture.manager = NULL;
    cc_agent_runtime_destroy(shared_runtime);
    cc_agent_runtime_destroy(independent_runtime);
    independent_store.vtable->destroy(independent_store.self);
    destroy_fixture(&fixture);
    CHECK(rejected && independent_admitted && count == 2);
    return 0;
}

/**
 * @brief 核对组合键转义、原始百分号隔离，以及 NULL、空串和普通格式兼容。
 * @return 全部字节比较通过零，失败一，成功路径逐次释放拥有键。
 */
static int test_session_key_encoding(void)
{
    const struct {
        const char *agent;
        const char *session;
        const char *expected;
    } cases[] = {
        { "a", "b:c", "a:b:c" },
        { "a:b", "c", "a%3Ab:c" },
        { "a%3Ab", "c", "a%253Ab:c" },
        { ":", "", "%3A:" },
        { "%3A", "", "%253A:" },
        { "agent", "session", "agent:session" },
        { NULL, NULL, "default:" },
        { "", "", ":" },
        { "用户:端%", "会话:一", "用户%3A端%25:会话:一" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *key = NULL;
        CHECK(ok(cc_agent_manager_make_session_key(cases[i].agent, cases[i].session, &key)));
        CHECK(key && strcmp(key, cases[i].expected) == 0);
        free(key);
    }
    cc_result_t rc = cc_agent_manager_make_session_key("a", "b", NULL);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT);
    cc_result_free(&rc);
    return 0;
}

/**
 * @brief 验证曾有组合键碰撞的两个会话在 STEER 和 interrupt 下保持取消隔离。
 * @return 全部断言通过零，失败一，成功路径显式结束在途任务并收集两份取消结果。
 * @note 第一 provider 保持在途，其有效 token 可直接核对第二会话操作没有取消第一任务。
 */
static int test_session_key_cancel_isolation(void)
{
    manager_fixture_t fixture;
    CHECK(create_fixture(&fixture) == 0);
    CHECK(ok(cc_agent_manager_add_agent(fixture.manager, "a", fixture.runtime)));
    CHECK(ok(cc_agent_manager_add_agent(fixture.manager, "a:b", fixture.runtime)));
    CHECK(ok(cc_agent_runtime_create_session(fixture.runtime, "b:c", ".")));
    CHECK(ok(cc_agent_runtime_create_session(fixture.runtime, "c", ".")));
    fixture.block_provider = 1;
    cc_run_id_t first = 0;
    cc_run_id_t second = 0;
    CHECK(ok(cc_agent_manager_submit(fixture.manager, "a", "b:c", "first", &first)));
    cc_mutex_lock(fixture.mutex);
    while (!fixture.provider_active) cc_cond_wait(fixture.cond, fixture.mutex);
    cc_mutex_unlock(fixture.mutex);
    CHECK(ok(cc_agent_manager_submit(fixture.manager, "a:b", "c", "second", &second)));
    CHECK(ok(cc_agent_manager_interrupt(fixture.manager, "a:b", "c")));
    cc_mutex_lock(fixture.mutex);
    int first_cancelled = cc_cancel_token_is_cancelled(fixture.active_token);
    cc_mutex_unlock(fixture.mutex);
    CHECK(ok(cc_agent_manager_interrupt(fixture.manager, "a", "b:c")));
    cc_mutex_lock(fixture.mutex);
    while (!fixture.cancellation_seen) cc_cond_wait(fixture.cond, fixture.mutex);
    fixture.allow_exit = 1;
    cc_cond_broadcast(fixture.cond);
    cc_mutex_unlock(fixture.mutex);
    CHECK(expect_collected(&fixture, first, CC_ERR_CANCELLED) == 0);
    CHECK(expect_collected(&fixture, second, CC_ERR_CANCELLED) == 0);
    destroy_fixture(&fixture);
    CHECK(!first_cancelled);
    return 0;
}

/**
 * @brief 运行管理器输出、发布、收集竞争、销毁寿命和会话重置 gate 回归。
 * @return 全部成功零，任一场景失败一，测试仅使用内存后端及可控 provider。
 */
int main(void)
{
    return test_outputs_and_publication() || test_borrowed_queue_destroy() ||
        test_collect_claim() || test_reset_gate() || test_reset_alias_gate() ||
        test_reset_shared_store_gate() || test_session_key_encoding() || test_session_key_cancel_isolation();
}
