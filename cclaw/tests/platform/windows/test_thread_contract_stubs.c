#include <assert.h>
#include <stddef.h>
#include <stdlib.h>

#include "windows.h"
#include "cc/core/cc_result.h"

/** @brief 固定句柄记录保留线程入口和完成/关闭状态。 */
struct stub_handle {
    LPTHREAD_START_ROUTINE entry;
    LPVOID arg;
    DWORD id;
    int finished;
    int closed;
};
static struct stub_handle handles[8];
static size_t handle_count;
static DWORD current_id = 100;
static int fail_create;
static int fail_wait;
static int fail_close;
static int fail_initialize;
static int fail_allocation;
static unsigned live_allocations;
static unsigned close_count;
static DWORD observed_timeout;

/**
 * @brief 运行真实平台传给 CreateThread 的 trampoline 并记录原生完成。
 * @param[in,out] handle 尚未完成的借用模型句柄。
 * @post 入口执行一次且返回零，当前身份恢复，句柄进入完成状态。
 */
static void run_handle(HANDLE handle)
{
    DWORD previous = current_id;
    current_id = handle->id;
    assert(handle->entry(handle->arg) == 0);
    current_id = previous;
    handle->finished = 1;
}

/**
 * @brief 保存入口并建立固定模型句柄，不自动调度。
 * @param[in] attributes 未使用，允许 NULL。
 * @param[in] stack 未使用的栈请求。
 * @param[in] entry 借用线程入口。
 * @param[in,out] arg 借用入口参数，启动包装由真实入口消费。
 * @param[in] flags 未使用的创建标志。
 * @param[out] id 可选线程 ID 输出。
 * @return 拥有模型句柄或注入失败时 NULL。
 */
HANDLE CreateThread(void *attributes, size_t stack, LPTHREAD_START_ROUTINE entry,
    LPVOID arg, DWORD flags, DWORD *id)
{
    (void)attributes; (void)stack; (void)flags;
    if (fail_create) { fail_create = 0; return NULL; }
    assert(handle_count < sizeof(handles) / sizeof(handles[0]));
    HANDLE handle = &handles[handle_count++];
    handle->entry = entry;
    handle->arg = arg;
    handle->id = (DWORD)handle_count;
    if (id) *id = handle->id;
    return handle;
}

/**
 * @brief 查询模型完成状态，或在无限等待时同步执行入口。
 * @param[in,out] handle 未关闭的存活句柄。
 * @param[in] timeout 零为状态查询，INFINITE 为完成等待。
 * @return 完成、未完成和定向失败分别对应 Windows 等待常量。
 */
DWORD WaitForSingleObject(HANDLE handle, DWORD timeout)
{
    assert(!handle->closed);
    if (timeout == 0) return handle->finished ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    assert(timeout == INFINITE);
    if (fail_wait) { fail_wait = 0; return WAIT_FAILED; }
    if (!handle->finished) run_handle(handle);
    return WAIT_OBJECT_0;
}

/**
 * @brief 标记模型句柄关闭，失败时保留原状态。
 * @param[in,out] handle 已完成且尚未关闭的借用句柄。
 * @return 成功为一，定向失败为零。
 */
BOOL CloseHandle(HANDLE handle)
{
    assert(handle->finished && !handle->closed);
    if (fail_close) { fail_close = 0; return 0; }
    handle->closed = 1;
    close_count++;
    return 1;
}

/** @brief 读取模型句柄身份。 @param[in] handle 存活借用句柄。 @return 保存的线程 ID。 */
DWORD GetThreadId(HANDLE handle) { return handle->id; }
/** @brief 读取当前模拟线程身份。 @return 当前线程 ID。 */
DWORD GetCurrentThreadId(void) { return current_id; }

/**
 * @brief 模拟原生临界区初始化失败和成功输出。
 * @param[out] section 成功时持有次数清零。
 * @param[in] spin 未使用的自旋配置。
 * @param[in] flags 未使用的初始化标志。
 * @return 成功为一，注入失败为零。
 */
BOOL InitializeCriticalSectionEx(CRITICAL_SECTION *section, DWORD spin, DWORD flags)
{
    (void)spin; (void)flags;
    if (fail_initialize) { fail_initialize = 0; return 0; }
    section->held = 0;
    return 1;
}

/**
 * @brief 记录等待预算，并检查调用方以一次临界区持有进入。
 * @param[in,out] condition 已初始化的借用条件对象。
 * @param[in,out] section 已持有一次的借用临界区，模型保持该持有。
 * @param[in] timeout 请求的毫秒数或 INFINITE。
 * @return 零，模拟超时并保留重新持锁的结果。
 */
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *condition, CRITICAL_SECTION *section,
    DWORD timeout)
{
    assert(condition->initialized && section->held == 1);
    observed_timeout = timeout;
    return 0;
}

/**
 * @brief 对私有生产源码分配注入失败并统计启动包装资源。
 * @param[in] bytes 请求字节数。
 * @return 拥有堆对象或 NULL，成功分配增加 live_allocations。
 */
static void *source_malloc(size_t bytes)
{
    if (fail_allocation) { fail_allocation = 0; return NULL; }
    void *allocation = malloc(bytes);
    if (allocation) live_allocations++;
    return allocation;
}

/**
 * @brief 消费私有生产源码拥有的堆资源并核对计数。
 * @param[in,out] allocation source_malloc 交付的拥有对象，NULL 安全。
 * @post 非 NULL 对象释放，live_allocations 减一。
 */
static void source_free(void *allocation)
{
    if (allocation) { assert(live_allocations); live_allocations--; }
    free(allocation);
}

/* 私有包含真实 Windows 源码，仅以模型 API 替换系统边界。 */
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
#define malloc source_malloc
#define free source_free
#ifndef _WIN32
#define _WIN32 1
#endif
#include "../../../platforms/windows/src/cc_windows_thread.c"
#undef malloc
#undef free

/**
 * @brief 消费拥有结果并核对预期状态。
 * @param[in] result 按值移交的拥有结果，原副本不得再清理。
 * @param[in] code 预期状态码。
 * @post 诊断资源释放，状态不匹配时终止测试。
 */
static void expect_result(cc_result_t result, cc_error_code_t code)
{
    int actual = result.code;
    cc_result_free(&result);
    assert(actual == code);
}

/** @brief 用户回调检查 trampoline 先释放启动包装。 @param[in,out] arg 借用调用计数。 @return 测试固定非 NULL 指针，端口应忽略。 */
static void *user_worker(void *arg)
{
    assert(live_allocations == 0);
    (*(int *)arg)++;
    return arg;
}

/**
 * @brief 检查等待失败与关闭失败均保留 HANDLE，启动包装只消费一次。
 * @post 成功重试关闭句柄，全部本次堆资源释放。
 */
static void test_join_failures_and_trampoline(void)
{
    cc_thread_t thread = NULL;
    int calls = 0;
    expect_result(review_thread_create(user_worker, &calls, &thread), CC_OK);
    HANDLE handle = thread;
    assert(live_allocations == 1);
    fail_wait = 1;
    expect_result(review_thread_join(thread), CC_ERR_PLATFORM);
    assert(!handle->closed && !calls && live_allocations == 1);
    fail_close = 1;
    expect_result(review_thread_join(thread), CC_ERR_PLATFORM);
    assert(!handle->closed && calls == 1 && live_allocations == 0);
    current_id = handle->id;
    assert(!review_thread_is_current(thread));
    current_id = 100;
    expect_result(review_thread_join(thread), CC_OK);
    assert(handle->closed && calls == 1 && close_count == 1);
}

/**
 * @brief 检查自 join 拒绝时不运行或关闭自身模型句柄。
 * @post 恢复外部身份后成功完成该线程，诊断和启动包装均释放。
 */
static void test_self_join(void)
{
    cc_thread_t thread = NULL;
    int calls = 0;
    expect_result(review_thread_create(user_worker, &calls, &thread), CC_OK);
    HANDLE handle = thread;
    current_id = handle->id;
    assert(review_thread_is_current(thread));
    expect_result(review_thread_join(thread), CC_ERR_PLATFORM);
    assert(!handle->closed && !calls);
    current_id = 100;
    expect_result(review_thread_join(thread), CC_OK);
    assert(calls == 1);
}

/**
 * @brief 检查分配和原生创建失败清理，验证条件等待毫秒与无限映射。
 * @post 成功创建的同步资源全部释放，故障后输出为空且分配数为零。
 */
static void test_creation_and_condition(void)
{
    cc_thread_t thread = (cc_thread_t)&thread;
    fail_allocation = 1;
    expect_result(review_thread_create(user_worker, NULL, &thread), CC_ERR_OUT_OF_MEMORY);
    assert(!thread && !live_allocations);
    fail_create = 1;
    expect_result(review_thread_create(user_worker, NULL, &thread), CC_ERR_PLATFORM);
    assert(!thread && !live_allocations);
    cc_mutex_t mutex = (cc_mutex_t)&mutex;
    fail_initialize = 1;
    expect_result(review_mutex_create(&mutex), CC_ERR_PLATFORM);
    assert(!mutex && !live_allocations);
    expect_result(review_mutex_create(&mutex), CC_OK);
    cc_cond_t condition = NULL;
    expect_result(review_cond_create(&condition), CC_OK);
    review_mutex_lock(mutex);
    assert(!review_cond_timedwait(condition, mutex, 123));
    assert(observed_timeout == 123);
    assert(!review_cond_timedwait(condition, mutex, 0));
    assert(observed_timeout == INFINITE);
    review_mutex_unlock(mutex);
    review_cond_destroy(condition);
    review_mutex_destroy(mutex);
    assert(!live_allocations);
}

/**
 * @brief 运行真实 Windows 线程包装的串行原生 API 模型回归。
 * @return 全部断言通过为零，失败终止进程。
 * @note 本模型验证入口转交与失败所有权，实际 WINAPI ABI、Windows 调度和内核等待需原生平台验证。
 */
int main(void)
{
    test_join_failures_and_trampoline();
    test_self_join();
    test_creation_and_condition();
    return 0;
}
