#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "FreeRTOS.h"
#include "cc/core/cc_result.h"

/** @brief pthread 模型保存信号量的许可与原语在途状态。 */
struct stub_semaphore {
    pthread_mutex_t gate;
    pthread_cond_t changed;
    unsigned count;
    unsigned giving;
    int mutex;
    int deleted;
};

/** @brief 固定任务记录模拟原生身份，实际 pthread 由测试最后 join。 */
struct stub_task {
    pthread_t native;
    TaskFunction_t entry;
    void *arg;
};

pthread_mutex_t stub_critical_mutex = PTHREAD_MUTEX_INITIALIZER;
unsigned stub_tick_rate = 100;
static pthread_mutex_t model_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t model_changed = PTHREAD_COND_INITIALIZER;
static struct stub_task tasks[16];
static size_t task_count;
static _Thread_local TaskHandle_t current_task;
static _Thread_local int joining;
static SemaphoreHandle_t captured_done;
static SemaphoreHandle_t paused_signal;
static int capture_done;
static int give_paused;
static int release_give;
static int join_blocked;
static int join_returned;
static int early_delete;
static int fail_take;
static int fail_binary;
static int fail_mutex;
static int fail_task;
static int fail_allocation;
static int skip_binary_wait;
static TickType_t observed_ticks;
static TickType_t observed_delay;
static unsigned binary_waiters;

/**
 * @brief 为模型有限等待建立主机实时时钟截止点。
 * @param[in] ticks 有限 tick 预算，按当前 stub_tick_rate 换算。
 * @return CLOCK_REALTIME 绝对截止值，模型要求时钟读取成功。
 */
static struct timespec deadline_for(TickType_t ticks)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    uint64_t nanos = (uint64_t)ticks * 1000000000ULL / stub_tick_rate;
    deadline.tv_sec += (time_t)(nanos / 1000000000ULL);
    deadline.tv_nsec += (long)(nanos % 1000000000ULL);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

/**
 * @brief 在模型控制锁内等待确定交错标志，十秒未到达即终止测试。
 * @pre 调用方已持 model_gate。
 * @param[in] flag 由 model_gate 保护的借用标志。
 * @post 标志非零，调用方仍持 model_gate。
 */
static void wait_flag(const int *flag)
{
    struct timespec deadline = deadline_for(stub_tick_rate * 10U);
    while (!*flag)
        assert(pthread_cond_timedwait(&model_changed, &model_gate, &deadline) == 0);
}

/**
 * @brief 创建独立模型信号量，故障开关仅针对下一个二值通知。
 * @param[in] mutex 非零创建初始可取得的普通锁，零创建空通知。
 * @return 成功交付拥有对象，注入失败为 NULL。
 */
SemaphoreHandle_t stub_semaphore_create(int mutex)
{
    if (mutex && fail_mutex) { fail_mutex = 0; return NULL; }
    if (!mutex && fail_binary) { fail_binary = 0; return NULL; }
    SemaphoreHandle_t semaphore = calloc(1, sizeof(*semaphore));
    assert(semaphore);
    assert(pthread_mutex_init(&semaphore->gate, NULL) == 0);
    assert(pthread_cond_init(&semaphore->changed, NULL) == 0);
    semaphore->mutex = mutex;
    semaphore->count = mutex ? 1U : 0U;
    if (!mutex && capture_done) {
        captured_done = semaphore;
        capture_done = 0;
    }
    return semaphore;
}

/**
 * @brief 在 pthread 模型取得许可，记录 join 屏障与实际有限 tick 参数。
 * @param[in,out] semaphore 存活模型对象，所有者保证调用期间保活。
 * @param[in] ticks portMAX_DELAY 为无限，其余为有限预算。
 * @return 获得许可为 pdTRUE，超时或定向故障为 pdFALSE。
 */
BaseType_t stub_semaphore_take(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    pthread_mutex_lock(&model_gate);
    if (joining && semaphore->mutex && give_paused && !release_give) {
        join_blocked = 1;
        pthread_cond_broadcast(&model_changed);
    }
    if (!semaphore->mutex) {
        observed_ticks = ticks;
        if (semaphore == captured_done && fail_take) {
            fail_take = 0;
            pthread_mutex_unlock(&model_gate);
            return pdFALSE;
        }
        if (skip_binary_wait) {
            pthread_mutex_unlock(&model_gate);
            return pdFALSE;
        }
        binary_waiters++;
        pthread_cond_broadcast(&model_changed);
    }
    pthread_mutex_unlock(&model_gate);

    pthread_mutex_lock(&semaphore->gate);
    int rc = 0;
    struct timespec deadline = {0};
    if (ticks != portMAX_DELAY) deadline = deadline_for(ticks);
    while (!semaphore->count && rc == 0) {
        rc = ticks == portMAX_DELAY
            ? pthread_cond_wait(&semaphore->changed, &semaphore->gate)
            : pthread_cond_timedwait(&semaphore->changed, &semaphore->gate, &deadline);
    }
    int acquired = semaphore->count != 0;
    if (acquired) semaphore->count = 0;
    pthread_mutex_unlock(&semaphore->gate);
    if (!semaphore->mutex) {
        pthread_mutex_lock(&model_gate);
        binary_waiters--;
        pthread_mutex_unlock(&model_gate);
    }
    return acquired ? pdTRUE : pdFALSE;
}

/**
 * @brief 回收已经没有在途原语的模型对象。
 * @param[in,out] semaphore 消费拥有对象，内部锁和条件对象先销毁。
 * @post 所有模型资源释放，调用方不得继续访问对象。
 */
static void reclaim_semaphore(SemaphoreHandle_t semaphore)
{
    assert(pthread_cond_destroy(&semaphore->changed) == 0);
    assert(pthread_mutex_destroy(&semaphore->gate) == 0);
    free(semaphore);
}

/**
 * @brief 先放行接收者，再在可控尾部继续持有模型对象。
 * @param[in,out] semaphore 借用存活对象，模拟 FreeRTOS Give 的唤醒后原语尾部。
 * @return pdTRUE，提前删除会记录错误并延迟回收以供断言。
 */
BaseType_t stub_semaphore_give(SemaphoreHandle_t semaphore)
{
    pthread_mutex_lock(&semaphore->gate);
    semaphore->giving++;
    semaphore->count = 1;
    pthread_cond_broadcast(&semaphore->changed);
    pthread_mutex_unlock(&semaphore->gate);
    pthread_mutex_lock(&model_gate);
    if (semaphore == paused_signal) {
        give_paused = 1;
        pthread_cond_broadcast(&model_changed);
        wait_flag(&release_give);
    }
    pthread_mutex_unlock(&model_gate);
    pthread_mutex_lock(&semaphore->gate);
    semaphore->giving--;
    int reclaim = semaphore->deleted;
    pthread_mutex_unlock(&semaphore->gate);
    if (reclaim) reclaim_semaphore(semaphore);
    return pdTRUE;
}

/**
 * @brief 删除模型对象并检测在 Give 返回前删除的交错。
 * @param[in,out] semaphore 消费拥有对象，NULL 跳过。
 * @post 正常路径立即回收，在途 Give 路径标记违规并由该 Give 最后回收。
 */
void stub_semaphore_delete(SemaphoreHandle_t semaphore)
{
    if (!semaphore) return;
    pthread_mutex_lock(&semaphore->gate);
    int busy = semaphore->giving != 0;
    semaphore->deleted = busy;
    pthread_mutex_unlock(&semaphore->gate);
    if (busy) {
        pthread_mutex_lock(&model_gate);
        early_delete = 1;
        pthread_cond_broadcast(&model_changed);
        pthread_mutex_unlock(&model_gate);
    } else {
        reclaim_semaphore(semaphore);
    }
}

/**
 * @brief 为真实平台任务入口安装模型当前身份。
 * @param[in,out] arg 借用固定 stub_task 记录，覆盖 pthread 执行期。
 * @return 原生自删应通过 pthread_exit 结束，意外返回也交付 NULL。
 */
static void *task_trampoline(void *arg)
{
    current_task = arg;
    current_task->entry(current_task->arg);
    return NULL;
}

/**
 * @brief 建立模拟任务，任务身份在 pthread 发布前写入输出。
 * @param[in] entry 借用真实平台入口。
 * @param[in] name 借用名称，模型忽略。
 * @param[in] stack 平台栈数值，模型不改变主机栈。
 * @param[in,out] arg 借用平台包装上下文。
 * @param[in] priority 平台优先级，模型忽略。
 * @param[out] out_task 成功写静态任务记录借用。
 * @return pthread 创建成功为 pdPASS，注入失败为 pdFAIL。
 */
BaseType_t stub_task_create(TaskFunction_t entry, const char *name, uint32_t stack,
    void *arg, UBaseType_t priority, TaskHandle_t *out_task)
{
    (void)name; (void)stack; (void)priority;
    if (fail_task) { fail_task = 0; return pdFAIL; }
    assert(task_count < sizeof(tasks) / sizeof(tasks[0]));
    TaskHandle_t task = &tasks[task_count++];
    task->entry = entry;
    task->arg = arg;
    *out_task = task;
    return pthread_create(&task->native, NULL, task_trampoline, task) == 0 ? pdPASS : pdFAIL;
}

/**
 * @brief 模拟当前任务自删，保留固定身份记录供测试收尾。
 * @param[in] task 必须为 NULL，外部任务删除不在本模型范围。
 * @post 当前 pthread 退出，测试随后 pthread_join 释放主机线程资源。
 */
void stub_task_delete(TaskHandle_t task)
{
    assert(task == NULL);
    pthread_exit(NULL);
}

/** @brief 取得可由测试模拟地址复用的当前任务身份。 @return TLS 借用身份，普通主线程为 NULL。 */
TaskHandle_t stub_task_current(void) { return current_task; }

/**
 * @brief 记录平台 OOM 回退延迟，避免让故障测试实际休眠。
 * @param[in] ticks 平台请求的 tick 数。
 * @post observed_delay 更新，模型不改变任务调度。
 */
void stub_task_delay(TickType_t ticks) { observed_delay = ticks; }

/**
 * @brief 仅向私有包含的生产源码注入一次 calloc 失败。
 * @param[in] count 元素数量。
 * @param[in] bytes 每元素字节数。
 * @return 故障开关消费后返回 NULL，否则返回真实 calloc 的拥有对象。
 */
static void *source_calloc(size_t count, size_t bytes)
{
    if (fail_allocation) { fail_allocation = 0; return NULL; }
    return calloc(count, bytes);
}

/* 私有重命名包含真实实现，测试只替换原生原语与本翻译单元分配入口。 */
#define cc_thread_create review_thread_create
#define cc_thread_join review_thread_join
#define cc_thread_is_current review_thread_is_current
#define cc_mutex_create review_mutex_create
#define cc_mutex_init_once review_mutex_init_once
#define cc_mutex_destroy review_mutex_destroy
#define cc_mutex_lock review_mutex_lock
#define cc_mutex_unlock review_mutex_unlock
#define cc_cond_create review_cond_create
#define cc_cond_destroy review_cond_destroy
#define cc_cond_wait review_cond_wait
#define cc_cond_timedwait review_cond_timedwait
#define cc_cond_signal review_cond_signal
#define cc_cond_broadcast review_cond_broadcast
#define calloc source_calloc
#if defined(CC_TEST_ESP32_THREAD) && CC_TEST_ESP32_THREAD
#ifndef ESP_PLATFORM
#define ESP_PLATFORM 1
#endif
#include "../../../platforms/esp32/src/cc_esp32_thread.c"
#else
#include "../../../platforms/freertos/src/cc_freertos_thread.c"
#endif
#undef calloc

/**
 * @brief 消费拥有结果并断言预期状态。
 * @param[in] result 按值移交的拥有结果，调用后原副本不再释放。
 * @param[in] code 预期错误码。
 * @post 消息释放，错误码不匹配时终止测试。
 */
static void expect_result(cc_result_t result, cc_error_code_t code)
{
    int actual = result.code;
    cc_result_free(&result);
    assert(actual == code);
}

static int allow_user_return;
/**
 * @brief 等待测试放行后返回，让完成 Give 尾部交错可重现。
 * @param[in] arg 未使用，允许 NULL。
 * @return NULL，借用控制状态由测试保管。
 */
static void *controlled_worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&model_gate);
    wait_flag(&allow_user_return);
    pthread_mutex_unlock(&model_gate);
    return NULL;
}

/**
 * @brief 执行 SDK join，并标记调用是否在 Give 尾部期间提前返回。
 * @param[in] arg 借用 SDK 线程句柄，成功 join 消费该句柄。
 * @return NULL，拥有结果在本线程释放。
 */
static void *join_worker(void *arg)
{
    joining = 1;
    expect_result(review_thread_join(arg), CC_OK);
    pthread_mutex_lock(&model_gate);
    join_returned = 1;
    pthread_cond_broadcast(&model_changed);
    pthread_mutex_unlock(&model_gate);
    return NULL;
}

/**
 * @brief 证明 done 已可取得时 join 仍等待 Give 最后使用退出。
 * @post 用户函数、SDK join 与模拟任务 pthread 全部结束，通知未提前销毁。
 */
static void test_completion_tail_barrier(void)
{
    cc_thread_t thread = NULL;
    capture_done = 1;
    expect_result(review_thread_create(controlled_worker, NULL, &thread), CC_OK);
    TaskHandle_t task = &tasks[task_count - 1];
    pthread_mutex_lock(&model_gate);
    paused_signal = captured_done;
    allow_user_return = 1;
    pthread_cond_broadcast(&model_changed);
    wait_flag(&give_paused);
    pthread_mutex_unlock(&model_gate);
    pthread_t collector;
    assert(pthread_create(&collector, NULL, join_worker, thread) == 0);
    pthread_mutex_lock(&model_gate);
    wait_flag(&join_blocked);
    assert(!early_delete && !join_returned);
    release_give = 1;
    pthread_cond_broadcast(&model_changed);
    pthread_mutex_unlock(&model_gate);
    assert(pthread_join(collector, NULL) == 0);
    assert(pthread_join(task->native, NULL) == 0);
    assert(!early_delete && join_returned);
    paused_signal = NULL;
}

/** @brief 提供立即完成的用户入口。 @param[in] arg 未使用，允许 NULL。 @return NULL。 */
static void *empty_worker(void *arg) { (void)arg; return NULL; }

/**
 * @brief 核对完成身份不会被复用任务地址误认，失败 Take 保留句柄可重试。
 * @post 成功重试消费 SDK 句柄，模拟任务 pthread 已收尾。
 */
static void test_completed_identity_and_retry(void)
{
    cc_thread_t thread = NULL;
    capture_done = 1;
    expect_result(review_thread_create(empty_worker, NULL, &thread), CC_OK);
    TaskHandle_t task = &tasks[task_count - 1];
    assert(pthread_join(task->native, NULL) == 0);
    current_task = task;
    assert(!review_thread_is_current(thread));
    current_task = NULL;
    fail_take = 1;
    expect_result(review_thread_join(thread), CC_ERR_PLATFORM);
    expect_result(review_thread_join(thread), CC_OK);
}

/** @brief 广播场景由主线程拥有，等待结果每个线程写自己的槽。 */
typedef struct wait_case {
    cc_mutex_t mutex;
    cc_cond_t condition;
    int result;
} wait_case_t;

/**
 * @brief 在真实平台条件变量实现中登记并有限等待。
 * @param[in,out] arg 借用独立 wait_case_t，同步对象覆盖 pthread 执行期。
 * @return NULL，结果写入本线程独占槽，返回前释放谓词锁。
 */
static void *condition_worker(void *arg)
{
    wait_case_t *test = arg;
    review_mutex_lock(test->mutex);
    test->result = review_cond_timedwait(test->condition, test->mutex, 1000);
    review_mutex_unlock(test->mutex);
    return NULL;
}

/**
 * @brief 检查广播逐个放行当前等待者，空队列通知不遗留许可。
 * @post 四个 pthread 均收尾，共享条件变量和谓词 mutex 销毁。
 */
static void test_broadcast_and_no_stored_signal(void)
{
    cc_mutex_t mutex = NULL;
    cc_cond_t condition = NULL;
    expect_result(review_mutex_create(&mutex), CC_OK);
    expect_result(review_cond_create(&condition), CC_OK);
    pthread_t workers[4];
    wait_case_t cases[4];
    for (size_t i = 0; i < 4; i++) {
        cases[i] = (wait_case_t){mutex, condition, 0};
        assert(pthread_create(&workers[i], NULL, condition_worker, &cases[i]) == 0);
    }
    pthread_mutex_lock(&model_gate);
    struct timespec deadline = deadline_for(stub_tick_rate * 10U);
    while (binary_waiters < 4)
        assert(pthread_cond_timedwait(&model_changed, &model_gate, &deadline) == 0);
    pthread_mutex_unlock(&model_gate);
    review_cond_broadcast(condition);
    for (size_t i = 0; i < 4; i++) {
        assert(pthread_join(workers[i], NULL) == 0);
        assert(cases[i].result == 1);
    }
    review_cond_signal(condition);
    review_mutex_lock(mutex);
    assert(review_cond_timedwait(condition, mutex, 10) == 0);
    review_mutex_unlock(mutex);
    review_cond_destroy(condition);
    review_mutex_destroy(mutex);
}

/**
 * @brief 从公共等待入口核对宽 tick 换算、有限哨兵与节点 OOM 后重新持锁。
 * @post 故障开关和 tick 率恢复，同步对象销毁。
 */
static void test_tick_budgets_and_wait_oom(void)
{
    cc_mutex_t mutex = NULL;
    cc_cond_t condition = NULL;
    expect_result(review_mutex_create(&mutex), CC_OK);
    expect_result(review_cond_create(&condition), CC_OK);
    review_mutex_lock(mutex);
    skip_binary_wait = 1;
    stub_tick_rate = 100;
    assert(!review_cond_timedwait(condition, mutex, 1));
    assert(observed_ticks == 1);
    assert(!review_cond_timedwait(condition, mutex, 0));
    assert(observed_ticks == portMAX_DELAY);
    stub_tick_rate = 1000;
    assert(!review_cond_timedwait(condition, mutex, INT_MAX));
    assert(observed_ticks == (TickType_t)INT_MAX);
    stub_tick_rate = 10000;
    assert(!review_cond_timedwait(condition, mutex, INT_MAX));
    assert(observed_ticks == portMAX_DELAY - 1U);
    stub_tick_rate = 100;
    skip_binary_wait = 0;
    fail_allocation = 1;
    observed_delay = 0;
    assert(!review_cond_timedwait(condition, mutex, 1));
    assert(observed_delay == 1);
    fail_binary = 1;
    assert(!review_cond_timedwait(condition, mutex, 1));
    SemaphoreHandle_t native = mutex;
    pthread_mutex_lock(&native->gate);
    assert(native->count == 0);
    pthread_mutex_unlock(&native->gate);
    review_mutex_unlock(mutex);
    review_cond_destroy(condition);
    review_mutex_destroy(mutex);
}

/**
 * @brief 检查线程部分创建失败清理与有效输出槽先清空。
 * @post 所有故障开关消费，失败均未发布任务或拥有句柄。
 */
static void test_creation_failures(void)
{
    cc_thread_t thread = (cc_thread_t)&thread;
    fail_allocation = 1;
    expect_result(review_thread_create(empty_worker, NULL, &thread), CC_ERR_OUT_OF_MEMORY);
    assert(thread == NULL);
    fail_binary = 1;
    expect_result(review_thread_create(empty_worker, NULL, &thread), CC_ERR_OUT_OF_MEMORY);
    assert(thread == NULL);
    fail_task = 1;
    expect_result(review_thread_create(empty_worker, NULL, &thread), CC_ERR_PLATFORM);
    assert(thread == NULL);
}

/**
 * @brief 在两个独立目标中运行同一真实 RTOS 源码契约模型。
 * @return 全部断言通过为零，失败通过 assert 终止进程。
 * @note pthread 仅模拟原生调度和资源，不证明板上 kernel、Idle 回收或实际栈峰值。
 */
int main(void)
{
    cc_thread_t thread = (cc_thread_t)&thread;
    fail_mutex = 1;
    expect_result(review_thread_create(empty_worker, NULL, &thread), CC_ERR_OUT_OF_MEMORY);
    assert(thread == NULL);
    test_completion_tail_barrier();
    test_completed_identity_and_retry();
    test_broadcast_and_no_stored_signal();
    test_tick_budgets_and_wait_oom();
    test_creation_failures();
    return 0;
}
