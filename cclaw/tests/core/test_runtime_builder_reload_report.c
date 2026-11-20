#include "cc/app/cc_runtime_builder.h"
#include "cc/ports/cc_thread.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "check failed at %d\n", __LINE__); return 1; } } while (0)

static int destroyed_providers;

/**
 * @brief 消费结果诊断并返回是否成功。
 * @param[in] rc 按值移交的拥有结果，调用者不再使用原消息指针。
 * @return CC_OK 为一，其他为零。
 */
static int ok(cc_result_t rc)
{
    int success = rc.code == CC_OK;
    cc_result_free(&rc);
    return success;
}

/**
 * @brief 累计无私有资源的 provider 销毁，测试屏障保证各次析构顺序执行。
 * @param[in] self 非拥有哨兵，忽略。
 * @post 销毁计数增加，无动态资源交付。
 */
static void destroy_provider(void *self)
{
    (void)self;
    destroyed_providers++;
}

static const cc_llm_provider_vtable_t provider_vtable = { NULL, NULL, destroy_provider, NULL };

/**
 * @brief 创建仅用于代际装配的 provider 哨兵。
 * @param[in] config 调用期间借用配置，忽略。
 * @param[in] http 借用共享客户端，忽略。
 * @param[out] out 必填空资源槽，成功写静态虚表及非拥有哨兵。
 * @return 始终 CC_OK，builder 最终调用 destroy_provider。
 */
static cc_result_t create_provider(const cc_config_t *config, cc_http_client_t *http, cc_llm_provider_t *out)
{
    (void)config;
    (void)http;
    out->self = (void *)1;
    out->vtable = &provider_vtable;
    return cc_result_ok();
}

/**
 * @brief 提供无资源的会话端口，测试仅重载而不执行消息。
 * @param[in] config 借用配置，忽略。
 * @param[out] out 必填空资源槽，清零。
 * @return 始终 CC_OK。
 */
static cc_result_t create_store(const cc_config_t *config, cc_session_store_t *out)
{
    (void)config;
    memset(out, 0, sizeof(*out));
    return cc_result_ok();
}

/**
 * @brief 提供无资源的策略端口。
 * @param[in] config 借用配置，忽略。
 * @param[out] out 必填空资源槽，清零。
 * @return 始终 CC_OK。
 */
static cc_result_t create_policy(const cc_config_t *config, cc_policy_engine_t *out)
{
    (void)config;
    memset(out, 0, sizeof(*out));
    return cc_result_ok();
}

#ifdef CC_TEST_BUILDER_RELOAD_REPORT
/** @brief 首次重载的提交返回屏障，字段受 mutex 保护，builder/config 覆盖 worker 整个调用。 */
typedef struct reload_fixture {
    cc_mutex_t mutex;
    cc_cond_t cond;
    cc_thread_t first_thread;
    cc_mutex_t generation_mutex;
    cc_runtime_builder_t *builder;
    const cc_config_t *config;
    cc_runtime_reload_report_t first_report;
    cc_error_code_t first_code;
    int start;
    int active;
    int unlock_count;
    int paused;
    int allow_return;
    int report_complete_at_commit;
} reload_fixture_t;

static reload_fixture_t fixture;

/**
 * @brief 声明链接包装所调用的真实 mutex 解锁入口。
 * @param[in] mutex 当前线程持有的有效互斥量。
 * @post 归还互斥量所有权，具体行为由线程端口实现。
 */
void __real_cc_mutex_unlock(cc_mutex_t mutex);

/**
 * @brief 首次重载第二次释放代际锁时暂停，允许另一重载回收刚发布的新代。
 * @param[in] mutex 调用方持有的互斥量，先转交真实解锁。
 * @post 普通解锁直接完成，目标提交解锁记录报告是否已填写并等待主线程放行。
 * @note fixture 自身解锁直接使用真实入口，避免包装重入，线程和状态均在 fixture 锁下判断。
 */
void __wrap_cc_mutex_unlock(cc_mutex_t mutex)
{
    __real_cc_mutex_unlock(mutex);
    if (!fixture.mutex || mutex == fixture.mutex) return;
    cc_mutex_lock(fixture.mutex);
    if (fixture.active && cc_thread_is_current(fixture.first_thread)) {
        if (!fixture.generation_mutex) fixture.generation_mutex = mutex;
        if (mutex == fixture.generation_mutex) {
            fixture.unlock_count++;
            if (fixture.unlock_count == 2) {
                fixture.report_complete_at_commit = fixture.first_report.old_generation == 1 &&
                    fixture.first_report.new_generation == 2 && fixture.first_report.tools_reloaded;
                fixture.paused = 1;
                cc_cond_broadcast(fixture.cond);
                while (!fixture.allow_return) cc_cond_wait(fixture.cond, fixture.mutex);
            }
        }
    }
    __real_cc_mutex_unlock(fixture.mutex);
}

/**
 * @brief 等主线程发布线程句柄后执行第一次带报告的重载。
 * @param[in,out] arg 借用 reload_fixture_t，保持到线程 join。
 * @return NULL，拥有诊断在本线程清理，主线程 join 后读取结果和报告。
 */
static void *first_reload(void *arg)
{
    reload_fixture_t *state = arg;
    cc_mutex_lock(state->mutex);
    while (!state->start) cc_cond_wait(state->cond, state->mutex);
    cc_mutex_unlock(state->mutex);
    cc_result_t rc = cc_runtime_builder_reload_with_report(state->builder, state->config, &state->first_report);
    state->first_code = rc.code;
    cc_result_free(&rc);
    return NULL;
}

/**
 * @brief 第一重载提交后暂停，第二重载完成并回收第二代，再恢复首调用检查拥有报告。
 * @param[in,out] builder 借用初始版本一的 builder，调用期间保活。
 * @param[in] config 借用重载配置，覆盖两个线程。
 * @return 两报告分别为一到二、二到三且首报告在提交解锁时已完成为零，失败一。
 */
static int test_concurrent_reports(cc_runtime_builder_t *builder, const cc_config_t *config)
{
    fixture.builder = builder;
    fixture.config = config;
    CHECK(ok(cc_thread_create(first_reload, &fixture, &fixture.first_thread)));
    cc_mutex_lock(fixture.mutex);
    fixture.active = 1;
    fixture.start = 1;
    cc_cond_broadcast(fixture.cond);
    while (!fixture.paused) cc_cond_wait(fixture.cond, fixture.mutex);
    cc_mutex_unlock(fixture.mutex);

    cc_runtime_reload_report_t second_report;
    cc_result_t rc = cc_runtime_builder_reload_with_report(builder, config, &second_report);
    int second_ok = rc.code == CC_OK && second_report.old_generation == 2 && second_report.new_generation == 3;
    cc_result_free(&rc);
    cc_mutex_lock(fixture.mutex);
    fixture.allow_return = 1;
    fixture.active = 0;
    cc_cond_broadcast(fixture.cond);
    cc_mutex_unlock(fixture.mutex);
    CHECK(ok(cc_thread_join(fixture.first_thread)));
    CHECK(second_ok && fixture.first_code == CC_OK && fixture.report_complete_at_commit);
    CHECK(fixture.first_report.old_generation == 1 && fixture.first_report.new_generation == 2);
    return 0;
}
#endif

/**
 * @brief 构建最小 builder，验证连续重载的报告寿命及早期拒绝保留输出。
 * @return 全部检查成功零，失败一，成功路径释放三代 provider 和测试同步资源。
 * @note Linux 包装控制两个重载的精确窗口，其他平台执行相同版本序列的同步报告检查。
 */
int main(void)
{
#ifdef CC_TEST_BUILDER_RELOAD_REPORT
    CHECK(ok(cc_mutex_create(&fixture.mutex)) && ok(cc_cond_create(&fixture.cond)));
#endif
    cc_config_t config = {0};
    config.provider = "fake";
    config.model = "fake";
    config.data_dir = ".";
    config.workspace_path = ".";
    const cc_llm_provider_descriptor_t providers[] = { { "fake", 1, create_provider } };
    cc_runtime_feature_set_t features = {0};
    features.llm_providers = providers;
    features.llm_provider_count = 1;
    features.create_session_store = create_store;
    features.create_policy_engine = create_policy;
    cc_runtime_builder_t *builder = NULL;
    CHECK(ok(cc_runtime_builder_create(&config, &features, &builder)));
#ifdef CC_TEST_BUILDER_RELOAD_REPORT
    CHECK(test_concurrent_reports(builder, &config) == 0);
#else
    cc_runtime_reload_report_t report;
    CHECK(ok(cc_runtime_builder_reload_with_report(builder, &config, &report)));
    CHECK(report.old_generation == 1 && report.new_generation == 2);
    CHECK(ok(cc_runtime_builder_reload_with_report(builder, &config, &report)));
    CHECK(report.old_generation == 2 && report.new_generation == 3);
#endif
    cc_runtime_reload_report_t untouched;
    memset(&untouched, 0x5a, sizeof(untouched));
    cc_runtime_reload_report_t saved = untouched;
    cc_result_t invalid = cc_runtime_builder_reload_with_report(builder, NULL, &untouched);
    CHECK(invalid.code == CC_ERR_INVALID_ARGUMENT && memcmp(&untouched, &saved, sizeof(saved)) == 0);
    cc_result_free(&invalid);
    CHECK(ok(cc_runtime_builder_destroy(builder, 1000)));
    CHECK(destroyed_providers == 3);
#ifdef CC_TEST_BUILDER_RELOAD_REPORT
    cc_cond_destroy(fixture.cond);
    cc_mutex_destroy(fixture.mutex);
    fixture.mutex = NULL;
#endif
    return 0;
}
