#include "cc/app/cc_runtime_builder.h"
#include "cc/app/cc_runtime_controller.h"
#include "cc/ports/cc_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 创建供本测试使用的内存会话后端。
 * @param[out] out_store 必填输出，成功取得拥有后端，使用其 destroy 释放。
 * @return 返回内存后端创建结果。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "check failed at %d\n", __LINE__); return 1; } } while (0)
/**
 * @brief 检查结果是否成功并消费其诊断资源。
 * @param[in] rc 按值移交的拥有结果，调用后原副本中的拥有指针须视为失效。
 * @return CC_OK 返回非零，其余返回零。
 */
static int ok(cc_result_t rc) { int success = rc.code == CC_OK; cc_result_free(&rc); return success; }
/**
 * @brief 结束无私有资源的测试 provider 寿命。
 * @param[in] self 工厂设置的哨兵值，忽略。
 * @post 哨兵无资源需要释放。
 */
static void provider_destroy(void *self) { (void)self; }
static const cc_llm_provider_vtable_t provider_vtable = {NULL, NULL, provider_destroy, NULL};
/**
 * @brief 填写仅用于构建代际的测试 provider，chat 入口保持为空。
 * @param[in] config 工厂配置，忽略，可为 NULL。
 * @param[in] http 借用 HTTP 客户端，忽略，可为 NULL。
 * @param[out] out 必填后端输出，接收静态虚表和哨兵 self。
 * @return 始终返回 CC_OK。
 */
static cc_result_t provider_create(const cc_config_t *config, cc_http_client_t *http, cc_llm_provider_t *out)
{
    (void)config; (void)http;
    out->self = (void *)1;
    out->vtable = &provider_vtable;
    return cc_result_ok();
}
/**
 * @brief 适配 builder 工厂签名以创建内存会话后端。
 * @param[in] config 配置，忽略，可为 NULL。
 * @param[out] out 必填拥有后端输出，由内存后端工厂填写。
 * @return 原样返回内存后端创建结果。
 */
static cc_result_t store_create(const cc_config_t *config, cc_session_store_t *out)
{
    (void)config;
    return cc_memory_session_store_create(out);
}
/**
 * @brief 提供没有策略实现的清零测试后端。
 * @param[in] config 配置，忽略，可为 NULL。
 * @param[out] out 必填输出，整项清零。
 * @return 始终返回 CC_OK。
 */
static cc_result_t policy_create(const cc_config_t *config, cc_policy_engine_t *out)
{
    (void)config;
    memset(out, 0, sizeof(*out));
    return cc_result_ok();
}

#ifdef CC_TEST_GENERATION_ADMISSION
static cc_mutex_t admission_mutex;
static cc_cond_t admission_cond;
static int admission_enabled, admission_entered, admission_release;
/**
 * @brief 链接器 --wrap 对应的真实代际取得入口。
 *
 * 第一个参数借用 builder，第二个参数输出拥有的 generation 引用，由调用者 release。
 * @return 返回实际代际取得结果。
 */
cc_result_t __real_cc_runtime_builder_acquire_generation(cc_runtime_builder_t *, cc_runtime_generation_t **);
/**
 * @brief 启用接纳屏障时等待测试线程放行，再调用真实代际取得入口。
 * @param[in] builder 借用 builder，调用期间有效。
 * @param[out] generation 必填输出，屏障结束后由真实入口填写拥有引用。
 * @return 原样返回真实代际取得结果，等待期间使用 admission_mutex 和条件变量同步。
 */
cc_result_t __wrap_cc_runtime_builder_acquire_generation(cc_runtime_builder_t *builder,
                                                        cc_runtime_generation_t **generation)
{
    if (admission_mutex) {
        cc_mutex_lock(admission_mutex);
        if (admission_enabled) {
            admission_entered = 1;
            cc_cond_broadcast(admission_cond);
            while (!admission_release) cc_cond_wait(admission_cond, admission_mutex);
        }
        cc_mutex_unlock(admission_mutex);
    }
    return __real_cc_runtime_builder_acquire_generation(builder, generation);
}
/**
 * @brief 在线程中提交输入，供销毁路径验证代际取得前已登记运行。
 * @param[in,out] arg 借用控制器，主线程保持其存活至线程结束。
 * @return 始终返回 NULL，提交结果和可能的答复在本线程清理。
 */
static void *admission_submit(void *arg)
{
    char *answer = NULL;
    cc_result_t rc = cc_runtime_controller_submit_text(arg, "input", NULL, NULL, &answer);
    cc_result_free(&rc);
    free(answer);
    return NULL;
}
/**
 * @brief 阻塞代际取得，验证首次销毁超时后释放屏障并重试销毁成功。
 * @param[in] config 借用配置，调用期间有效。
 * @param[in] features 按值传入的工厂集合。
 * @return 断言全部通过返回零，失败返回一。成功路径 join 工作线程后释放测试同步资源。
 */
static int test_generation_admission(const cc_config_t *config, cc_runtime_feature_set_t features)
{
    cc_runtime_builder_t *builder = NULL;
    cc_runtime_controller_t *controller = NULL;
    cc_runtime_controller_config_t io = {0};
    io.size = sizeof(io); io.session_base = "admission"; io.fresh_session_on_start = 1;
    CHECK(ok(cc_runtime_builder_create(config, &features, &builder)) &&
          ok(cc_runtime_controller_create(builder, &io, &controller)));
    CHECK(ok(cc_mutex_create(&admission_mutex)) && ok(cc_cond_create(&admission_cond)));
    admission_enabled = 1;
    cc_thread_t worker;
    CHECK(ok(cc_thread_create(admission_submit, controller, &worker)));
    cc_mutex_lock(admission_mutex);
    while (!admission_entered) cc_cond_wait(admission_cond, admission_mutex);
    cc_mutex_unlock(admission_mutex);
    cc_result_t rc = cc_runtime_controller_destroy(controller, 1);
    CHECK(rc.code == CC_ERR_TIMEOUT);
    cc_result_free(&rc);
    cc_mutex_lock(admission_mutex);
    admission_release = 1;
    cc_cond_broadcast(admission_cond);
    cc_mutex_unlock(admission_mutex);
    CHECK(ok(cc_thread_join(worker)));
    CHECK(ok(cc_runtime_controller_destroy(controller, 1000)) && ok(cc_runtime_builder_destroy(builder, 1000)));
    cc_cond_destroy(admission_cond);
    cc_mutex_destroy(admission_mutex);
    admission_mutex = NULL;
    return 0;
}
#endif
/**
 * @brief 注入记忆后端存储错误，供诊断保留与降级构建断言使用。
 * @param[in] config 配置，忽略，可为 NULL。
 * @param[out] out 工厂输出，本测试分支保持原值。
 * @return 始终返回 CC_ERR_STORAGE，诊断由返回结果拥有。
 */
static cc_result_t memory_create_failure(const cc_config_t *config, cc_memory_store_t *out)
{
    (void)config; (void)out;
    return cc_result_error(CC_ERR_STORAGE, "memory data damaged");
}

typedef struct blocking_store {
    cc_session_store_t base;
    cc_mutex_t mutex;
    cc_cond_t cond;
    int block, started, release;
} blocking_store_t;
static blocking_store_t blocking_store;
/**
 * @brief 在测试屏障处暂停会话创建，获准后调用基础内存后端。
 * @param[in,out] self 非空 blocking_store_t 借用对象，屏障状态在 mutex 下访问。
 * @param[in] id 借用会话 ID，透传基础后端。
 * @param[in] workspace 借用工作区，透传基础后端。
 * @return 原样返回基础后端创建结果，存储调用发生在屏障锁外。
 */
static cc_result_t blocking_create(void *self,const char *id,const char *workspace)
{
    blocking_store_t *store=self;
    cc_mutex_lock(store->mutex);
    if (store->block) {
        store->started=1; cc_cond_broadcast(store->cond);
        while (!store->release) cc_cond_wait(store->cond,store->mutex);
    }
    cc_mutex_unlock(store->mutex);
    return store->base.vtable->create_session(store->base.self,id,workspace);
}
/**
 * @brief 将记录追加直接转发给基础后端。
 * @param[in,out] self 非空屏障后端，借出其中的基础后端。
 * @param[in] records 借用记录数组，调用期间有效。
 * @param[in] count 记录数，透传。
 * @return 原样返回基础追加结果。
 */
static cc_result_t blocking_append(void *self,const cc_session_record_t *records,size_t count)
{ blocking_store_t *s=self; return s->base.vtable->append_records(s->base.self,records,count); }
/**
 * @brief 将消息读取直接转发给基础后端。
 * @param[in] self 非空屏障后端，调用期间借用。
 * @param[in] id 借用会话 ID。
 * @param[in] limit 消息数量限制，原样透传，负数表示全部。
 * @param[out] messages 基础后端填写的拥有消息数组，调用者逐项 cleanup 后 free。
 * @param[out] count 基础后端填写的消息数量。
 * @return 原样返回基础读取结果。
 */
static cc_result_t blocking_load(void *self,const char *id,int limit,cc_message_t **messages,size_t *count)
{ blocking_store_t *s=self; return s->base.vtable->load_messages(s->base.self,id,limit,messages,count); }
/**
 * @brief 将会话列表读取直接转发给基础后端。
 * @param[in] self 非空屏障后端，调用期间借用。
 * @param[out] sessions 基础后端填写的拥有数组，调用者使用 cc_session_free_array。
 * @param[out] count 基础后端填写的会话数量。
 * @return 原样返回基础列表读取结果。
 */
static cc_result_t blocking_list(void *self,cc_session_t **sessions,size_t *count)
{ blocking_store_t *s=self; return s->base.vtable->list_sessions(s->base.self,sessions,count); }
/**
 * @brief 将会话清理请求直接转发给基础后端。
 * @param[in,out] self 非空屏障后端，调用期间借用。
 * @param[in] id 借用目标会话 ID。
 * @return 原样返回基础清理结果。
 */
static cc_result_t blocking_clear(void *self,const char *id)
{ blocking_store_t *s=self; return s->base.vtable->clear_session(s->base.self,id); }
/**
 * @brief 销毁屏障后端拥有的基础存储并清零基础句柄。
 * @param[in,out] self 非空屏障后端，调用前测试已排空工作线程。
 * @post 基础后端已销毁，mutex 和 cond 继续由测试函数持有并释放。
 */
static void blocking_destroy(void *self)
{ blocking_store_t *s=self; s->base.vtable->destroy(s->base.self); memset(&s->base,0,sizeof(s->base)); }
static const cc_session_store_vtable_t blocking_vtable={blocking_create,blocking_append,blocking_load,
                                                       blocking_list,blocking_clear,blocking_destroy};
/**
 * @brief 创建全局测试包装器拥有的内存后端，并输出屏障虚表。
 * @param[in] config 配置，忽略，可为 NULL。
 * @param[out] out 必填输出，基础创建成功后填写全局包装器借用与静态虚表，失败保持原值。
 * @return 原样返回基础存储创建结果。
 */
static cc_result_t blocking_store_create(const cc_config_t *config,cc_session_store_t *out)
{
    (void)config;
    cc_result_t rc=cc_memory_session_store_create(&blocking_store.base);
    if (rc.code==CC_OK) { out->self=&blocking_store; out->vtable=&blocking_vtable; }
    return rc;
}
typedef struct ensure_thread_context {
    cc_runtime_controller_t *controller;
    char session_id[96];
    cc_result_t result;
} ensure_thread_context_t;
/**
 * @brief 在线程中执行目标会话持久化，并将拥有结果交回测试主线程。
 * @param[in,out] arg 非空 ensure_thread_context_t 借用对象，controller 和 session_id 保持有效至 join。
 * @return 始终返回 NULL，ctx->result 由主线程在 join 后检查和释放。
 */
static void *ensure_thread(void *arg)
{
    ensure_thread_context_t *ctx=arg;
    ctx->result=cc_runtime_controller_ensure_session(ctx->controller,ctx->session_id);
    return NULL;
}

/**
 * @brief 验证长前缀在创建、新会话切换与配置变化时仍保留完整版本后缀。
 * @param[in] config 借用运行时配置，调用期间有效。
 * @param[in] features 测试工厂集合，按值传入。
 * @return 全部断言通过返回零，失败返回非零。
 */
static int test_long_session_base(const cc_config_t *config, cc_runtime_feature_set_t features)
{
    char base[160];
    memset(base, 'x', sizeof(base) - 1U);
    base[sizeof(base) - 1U] = '\0';
    cc_runtime_controller_config_t io = {0};
    io.size = sizeof(io);
    io.session_base = base;
    io.workspace_dir = ".";
    cc_runtime_builder_t *builder = NULL;
    cc_runtime_controller_t *controller = NULL;
    CHECK(ok(cc_runtime_builder_create(config, &features, &builder)));
    CHECK(ok(cc_runtime_controller_create(builder, &io, &controller)));
    cc_runtime_controller_status_t before, after;
    cc_runtime_controller_get_status(controller, &before);
    CHECK(before.session_generation == 1);
    CHECK(before.session_id[sizeof(before.session_id) - 1U] == '\0');
    CHECK(strlen(before.session_id) == sizeof(before.session_id) - 1U);
    CHECK(strcmp(before.session_id + strlen(before.session_id) - 2U, "-1") == 0);

    for (unsigned long generation = 2; generation <= 12; generation++) {
        CHECK(ok(cc_runtime_controller_start_new_session(controller)));
        cc_runtime_controller_get_status(controller, &after);
        char suffix[32];
        snprintf(suffix, sizeof(suffix), "-%lu", generation);
        CHECK(after.session_generation == generation);
        CHECK(after.session_id[sizeof(after.session_id) - 1U] == '\0');
        CHECK(strlen(after.session_id) == sizeof(after.session_id) - 1U);
        CHECK(strcmp(after.session_id + strlen(after.session_id) - strlen(suffix), suffix) == 0);
        CHECK(strcmp(before.session_id, after.session_id) != 0);
        before = after;
    }

    io.workspace_dir = "alternate-a";
    CHECK(ok(cc_runtime_controller_apply_config(controller, &io)));
    cc_runtime_controller_get_status(controller, &after);
    CHECK(after.session_generation == 13);
    CHECK(after.session_id[sizeof(after.session_id) - 1U] == '\0');
    CHECK(strlen(after.session_id) == sizeof(after.session_id) - 1U);
    CHECK(strcmp(after.session_id + strlen(after.session_id) - 3U, "-13") == 0);
    CHECK(strcmp(before.session_id, after.session_id) != 0);
    before = after;
    io.workspace_dir = "alternate-b";
    CHECK(ok(cc_runtime_controller_apply_config(controller, &io)));
    cc_runtime_controller_get_status(controller, &after);
    CHECK(after.session_generation == 14);
    CHECK(after.session_id[sizeof(after.session_id) - 1U] == '\0');
    CHECK(strlen(after.session_id) == sizeof(after.session_id) - 1U);
    CHECK(strcmp(after.session_id + strlen(after.session_id) - 3U, "-14") == 0);
    CHECK(strcmp(before.session_id, after.session_id) != 0);

    /* 三字节前缀在剩余 92 字节处退回到完整字符边界。 */
    char utf8_base[121];
    for (size_t i = 0; i < 40; i++) {
        memcpy(utf8_base + i * 3U, "\xE4\xB8\xAD", 3U);
    }
    utf8_base[sizeof(utf8_base) - 1U] = '\0';
    io.session_base = utf8_base;
    CHECK(ok(cc_runtime_controller_apply_config(controller, &io)));
    cc_runtime_controller_get_status(controller, &after);
    CHECK(after.session_generation == 15);
    CHECK(strlen(after.session_id) == 93U && after.session_id[93] == '\0');
    CHECK(strncmp(after.session_id, utf8_base, 90U) == 0);
    CHECK(strcmp(after.session_id + 90U, "-15") == 0);
    CHECK(ok(cc_runtime_controller_destroy(controller, 1000)));
    CHECK(ok(cc_runtime_builder_destroy(builder, 1000)));
    return 0;
}

/**
 * @brief 阻塞空会话创建，验证并行切换、重复创建、提交及关闭的接纳规则。
 * @param[in] config 借用配置，调用期间有效。
 * @param[in] features 按值传入的工厂集合，本函数仅替换本地会话后端工厂。
 * @return 全部断言通过返回零，失败返回一，成功路径 join 后验证持久化状态并释放所有测试资源。
 */
static int test_ensure_session_concurrency(const cc_config_t *config,cc_runtime_feature_set_t features)
{
    memset(&blocking_store,0,sizeof(blocking_store));
    CHECK(ok(cc_mutex_create(&blocking_store.mutex)) && ok(cc_cond_create(&blocking_store.cond)));
    features.create_session_store=blocking_store_create;
    cc_runtime_builder_t *builder=NULL; cc_runtime_controller_t *controller=NULL;
    cc_runtime_controller_config_t io={0}; io.size=sizeof(io); io.session_base="concurrent"; io.fresh_session_on_start=1;
    CHECK(ok(cc_runtime_builder_create(config,&features,&builder)) &&
          ok(cc_runtime_controller_create(builder,&io,&controller)));
    cc_runtime_controller_status_t status={0}; cc_runtime_controller_get_status(controller,&status);
    CHECK(!status.session_is_persisted);
    ensure_thread_context_t ctx={0}; ctx.controller=controller;
    snprintf(ctx.session_id,sizeof(ctx.session_id),"%s",status.session_id);
    cc_mutex_lock(blocking_store.mutex); blocking_store.block=1; cc_mutex_unlock(blocking_store.mutex);
    cc_thread_t thread=NULL; CHECK(ok(cc_thread_create(ensure_thread,&ctx,&thread)));
    cc_mutex_lock(blocking_store.mutex);
    while (!blocking_store.started)
        if (cc_cond_timedwait(blocking_store.cond,blocking_store.mutex,5000)!=0) break;
    int started=blocking_store.started; cc_mutex_unlock(blocking_store.mutex); CHECK(started);
    cc_result_t rc=cc_runtime_controller_start_new_session(controller);
    CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    rc=cc_runtime_controller_ensure_session(controller,ctx.session_id);
    CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    char *answer=NULL; rc=cc_runtime_controller_submit_text(controller,"parallel input",NULL,NULL,&answer);
    CHECK(rc.code==CC_ERR_QUEUE_FULL && !answer); cc_result_free(&rc);
    rc=cc_runtime_controller_shutdown(controller,1);
    CHECK(rc.code==CC_ERR_TIMEOUT); cc_result_free(&rc);
    cc_mutex_lock(blocking_store.mutex); blocking_store.release=1;
    cc_cond_broadcast(blocking_store.cond); cc_mutex_unlock(blocking_store.mutex);
    CHECK(ok(cc_thread_join(thread)) && ok(ctx.result));
    cc_runtime_controller_get_status(controller,&status);
    CHECK(status.session_is_persisted && strcmp(status.session_id,ctx.session_id)==0);
    CHECK(ok(cc_runtime_controller_destroy(controller,1000)) && ok(cc_runtime_builder_destroy(builder,1000)));
    cc_cond_destroy(blocking_store.cond); cc_mutex_destroy(blocking_store.mutex); return 0;
}

/**
 * @brief 验证配置身份、延迟会话、历史恢复、长前缀及已接纳工作的关闭行为。
 * @return 全部测试通过返回零，任一断言失败返回一。
 */
int main(void)
{
    cc_config_t config = {0};
    config.provider = "fake"; config.model = "fake";
    config.data_dir = "."; config.workspace_path = ".";
    const cc_llm_provider_descriptor_t providers[] = {{"fake", 1, provider_create}};
    cc_runtime_feature_set_t features = {0};
    features.llm_providers = providers; features.llm_provider_count = 1;
    features.create_session_store = store_create; features.create_policy_engine = policy_create;
    features.create_memory_store = memory_create_failure;
    cc_runtime_builder_t *builder = NULL;
    CHECK(ok(cc_runtime_builder_create(&config, &features, &builder)));
    const cc_runtime_diagnostics_t *diagnostics = cc_runtime_builder_diagnostics(builder);
    CHECK(diagnostics && diagnostics->count == 1 &&
          strcmp(diagnostics->items[0].kind, "memory") == 0 &&
          strstr(diagnostics->items[0].message, "damaged"));
    cc_runtime_controller_config_t io = {0};
    io.size = sizeof(io); io.session_base = "review"; io.drop_late_events = 1;
    cc_runtime_controller_t *controller = NULL;
    CHECK(ok(cc_runtime_controller_create(builder, &io, &controller)));
    cc_runtime_controller_status_t before, after;
    cc_runtime_controller_get_status(controller, &before);
    CHECK(strcmp(before.session_id, "review-1") == 0);
    for (int i = 0; i < 3; ++i) CHECK(ok(cc_runtime_controller_apply_config(controller, &io)));
    io.session_base = NULL; io.drop_late_events = 0;
    CHECK(ok(cc_runtime_controller_apply_config(controller, &io)));
    cc_runtime_controller_get_status(controller, &after);
    CHECK(before.session_generation == after.session_generation);
    CHECK(strcmp(before.session_id, after.session_id) == 0);
    CHECK(ok(cc_runtime_controller_clear_session(controller)));
    cc_runtime_controller_get_status(controller, &before);
    CHECK(before.session_generation == after.session_generation + 1);
    CHECK(strcmp(before.session_id, "review-2") == 0);
    CHECK(ok(cc_runtime_controller_apply_config(controller, &io)));
    cc_runtime_controller_get_status(controller, &after);
    CHECK(strcmp(before.session_id, after.session_id) == 0);
    io.session_base = "changed";
    CHECK(ok(cc_runtime_controller_apply_config(controller, &io)));
    cc_runtime_controller_get_status(controller, &after);
    CHECK(after.session_generation == before.session_generation + 1);
    CHECK(strcmp(after.session_id, "changed-3") == 0);
    CHECK(ok(cc_runtime_controller_destroy(controller, 1000)));
    CHECK(ok(cc_runtime_builder_destroy(builder, 1000)));

    /* 新会话在实际输入前保持未持久化，恢复历史时列表保持稳定，重新创建控制器使用新 ID。 */
    builder = NULL;
    CHECK(ok(cc_runtime_builder_create(&config, &features, &builder)));
    memset(&io, 0, sizeof(io));
    io.size = sizeof(io); io.session_base = "device"; io.fresh_session_on_start = 1;
    controller = NULL;
    CHECK(ok(cc_runtime_controller_create(builder, &io, &controller)));
    cc_runtime_controller_get_status(controller, &before);
    CHECK(!before.session_is_persisted);
    cc_session_t *sessions = NULL;
    size_t session_count = 0;
    CHECK(ok(cc_runtime_controller_list_sessions(controller, &sessions, &session_count)));
    CHECK(session_count == 0);
    cc_session_free_array(sessions, session_count);

    cc_result_t missing = cc_runtime_controller_resume_session(controller, "missing");
    CHECK(missing.code == CC_ERR_NOT_FOUND);
    cc_result_free(&missing);
    cc_runtime_generation_t *generation = NULL;
    CHECK(ok(cc_runtime_builder_acquire_generation(builder, &generation)));
    cc_session_store_t *saved = cc_agent_runtime_session_store(cc_runtime_generation_runtime(generation));
    CHECK(saved && saved->vtable);
    CHECK(ok(saved->vtable->create_session(saved->self, "old-session", ".")));
    cc_message_t *message = NULL;
    CHECK(ok(cc_message_create_text("old-message", "old-session", CC_ROLE_USER,
                                    "previous input", NULL, &message)));
    cc_session_record_t record = { .type = CC_SESSION_RECORD_MESSAGE,
        .session_id = "old-session", .data.message = message };
    CHECK(ok(saved->vtable->append_records(saved->self, &record, 1)));
    cc_message_destroy(message);
    cc_runtime_generation_release(generation);

    CHECK(ok(cc_runtime_controller_resume_session(controller, "old-session")));
    cc_runtime_controller_get_status(controller, &after);
    CHECK(after.session_is_persisted && strcmp(after.session_id, "old-session") == 0);
    sessions = NULL; session_count = 0;
    CHECK(ok(cc_runtime_controller_list_sessions(controller, &sessions, &session_count)));
    CHECK(session_count == 1 && strcmp(sessions[0].id, "old-session") == 0);
    cc_session_free_array(sessions, session_count);
    cc_message_t *messages = NULL;
    size_t message_count = 0;
    CHECK(ok(cc_runtime_controller_load_messages(controller, "old-session", 10,
                                                 &messages, &message_count)));
    CHECK(message_count == 1 && strcmp(messages[0].id, "old-message") == 0);
    cc_message_cleanup(&messages[0]);
    free(messages);

    CHECK(ok(cc_runtime_controller_start_new_session(controller)));
    cc_runtime_controller_get_status(controller, &after);
    CHECK(!after.session_is_persisted && strcmp(after.session_id, before.session_id) != 0);
    char last_new_id[sizeof(after.session_id)];
    snprintf(last_new_id, sizeof(last_new_id), "%s", after.session_id);
    CHECK(ok(cc_runtime_controller_destroy(controller, 1000)));
    CHECK(ok(cc_runtime_controller_create(builder, &io, &controller)));
    cc_runtime_controller_get_status(controller, &after);
    CHECK(!after.session_is_persisted && strcmp(after.session_id, last_new_id) != 0);
    sessions = NULL; session_count = 0;
    CHECK(ok(cc_runtime_controller_list_sessions(controller, &sessions, &session_count)));
    CHECK(session_count == 1);
    cc_session_free_array(sessions, session_count);
    cc_result_t wrong_session=cc_runtime_controller_ensure_session(controller,"different-session");
    CHECK(wrong_session.code==CC_ERR_INVALID_STATE); cc_result_free(&wrong_session);
    cc_runtime_controller_get_status(controller,&after); CHECK(!after.session_is_persisted);
    CHECK(ok(cc_runtime_controller_ensure_session(controller,after.session_id)));
    cc_runtime_controller_get_status(controller,&after); CHECK(after.session_is_persisted);
    CHECK(ok(cc_runtime_controller_ensure_session(controller,after.session_id)));
    CHECK(ok(cc_runtime_controller_destroy(controller, 1000)));
    CHECK(ok(cc_runtime_builder_destroy(builder, 1000)));
    CHECK(test_long_session_base(&config, features) == 0);
    CHECK(test_ensure_session_concurrency(&config,features)==0);
#ifdef CC_TEST_GENERATION_ADMISSION
    CHECK(test_generation_admission(&config,features)==0);
#endif
    return 0;
}
