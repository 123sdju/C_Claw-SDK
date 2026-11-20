



#include "cc/ports/cc_thread.h"

#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>

/**
 * @brief 在进程期 SRW 门锁内安装一次 CRITICAL_SECTION 包装。
 * @param[in,out] slot 必需静态零初始化槽，安装后保持至进程退出。
 * @return 已有或创建成功为 CC_OK，NULL 为 INVALID_ARGUMENT，创建错误原样返回。
 * @note 所有初始化通过本入口，调用者不并发重置或销毁已安装锁，失败允许重试。
 */
cc_result_t cc_mutex_init_once(cc_mutex_t *slot)
{
    if (!slot) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null mutex slot");
    static SRWLOCK gate = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&gate);
    cc_result_t rc = *slot ? cc_result_ok() : cc_mutex_create(slot);
    ReleaseSRWLockExclusive(&gate);
    return rc;
}

/** @brief 创建线程前拥有的入口副本，由新线程取走并释放。 */
typedef struct cc_windows_thread_start {
    cc_thread_fn_t fn; /**< 覆盖线程执行期的借用入口。 */
    void *arg; /**< 原调用方保管的借用上下文。 */
} cc_windows_thread_start_t;

/**
 * @brief 按 WINAPI 调用约定进入 SDK 回调，避免函数指针强转。
 * @param[in,out] arg 消费拥有的启动包装，先复制字段并释放，再调用 SDK 入口。
 * @return 固定零，SDK 回调返回指针由本端口忽略。
 */
static DWORD WINAPI cc_windows_thread_entry(LPVOID arg)
{
    cc_windows_thread_start_t *start = arg;
    cc_thread_fn_t fn = start->fn;
    void *user_arg = start->arg;
    free(start);
    (void)fn(user_arg);
    return 0;
}

/**
 * @brief 创建 Windows 线程和独立启动包装，成功交付拥有 HANDLE。
 * @param[in] fn 必需 SDK 入口，可能在创建函数返回前执行，返回指针不交付 join。
 * @param[in,out] arg 借用线程上下文，调用者保持到入口结束。
 * @param[out] out_thread 必需空资源槽，基础参数合法后置 NULL，成功由唯一拥有者 join。
 * @return 成功为 CC_OK，空参数为 INVALID_ARGUMENT，包装分配失败为 OUT_OF_MEMORY，CreateThread 失败为 PLATFORM。
 * @note 创建失败清理启动包装，成功后包装由新线程释放，HANDLE 由 join 消费。
 */
cc_result_t cc_thread_create(cc_thread_fn_t fn, void *arg, cc_thread_t *out_thread)
{
    if (!fn || !out_thread) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid thread argument");
    }
    *out_thread = NULL;
    cc_windows_thread_start_t *start = malloc(sizeof(*start));
    if (!start) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate thread start");
    start->fn = fn;
    start->arg = arg;
    HANDLE h = CreateThread(NULL, 0, cc_windows_thread_entry, start, 0, NULL);
    if (!h) {
        free(start);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create thread");
    }
    *out_thread = h;
    return cc_result_ok();
}

/**
 * @brief 无限等待 Windows 线程退出，确认结束后消费线程 HANDLE。
 * @param[in,out] thread create 交付且尚未成功 join 的拥有句柄，NULL 返回参数错误。
 * @return 等待为 WAIT_OBJECT_0 且关闭成功时返回 CC_OK；等待或关闭失败返回 CC_ERR_PLATFORM，保留句柄供拥有者重试。
 * @note 外层串行化同一线程的 join，并在目标线程之外调用。
 */
cc_result_t cc_thread_join(cc_thread_t thread)
{
    if (!thread) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null thread");
    }
    if (cc_thread_is_current(thread)) {
        return cc_result_error(CC_ERR_PLATFORM, "Cannot join the current thread");
    }
    DWORD wait_result = WaitForSingleObject((HANDLE)thread, INFINITE);
    if (wait_result != WAIT_OBJECT_0) {
        return cc_result_error(CC_ERR_PLATFORM, "Failed to wait for thread completion");
    }
    if (!CloseHandle((HANDLE)thread)) {
        return cc_result_error(CC_ERR_PLATFORM, "Failed to close thread handle");
    }
    return cc_result_ok();
}

/**
 * @brief 比较当前线程 ID 与存活 HANDLE 所指线程。
 * @param[in] thread 可选借用且尚未关闭的 HANDLE，拥有者排除并发成功 join。
 * @return 尚未结束且当前线程 ID 匹配为一，NULL、已结束、原生查询失败或不同线程为零。
 */
int cc_thread_is_current(cc_thread_t thread)
{
    return thread && GetThreadId((HANDLE)thread) == GetCurrentThreadId() &&
        WaitForSingleObject((HANDLE)thread, 0) == WAIT_TIMEOUT;
}

/**
 * @brief 创建可递归的 Windows CRITICAL_SECTION 包装。
 * @param[out] out_mutex 必需空资源槽，进入后置 NULL，成功由调用者 destroy。
 * @return 成功为 CC_OK，NULL 为 INVALID_ARGUMENT，分配失败为 OUT_OF_MEMORY，原生初始化失败为 PLATFORM。
 */
cc_result_t cc_mutex_create(cc_mutex_t *out_mutex)
{
    if (!out_mutex) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null mutex output");
    }
    *out_mutex = NULL;
    CRITICAL_SECTION *cs = malloc(sizeof(CRITICAL_SECTION));
    if (!cs) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate mutex");
    }
    if (!InitializeCriticalSectionEx(cs, 0, 0)) {
        free(cs);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to initialize mutex");
    }
    *out_mutex = cs;
    return cc_result_ok();
}

/**
 * @brief 删除并释放拥有的 CRITICAL_SECTION。
 * @pre 已排空持有者、等待者及其他句柄借用。
 * @param[in,out] mutex 消费拥有锁，NULL 跳过。
 * @post 非 NULL 锁和借用失效。
 */
void cc_mutex_destroy(cc_mutex_t mutex)
{
    if (!mutex) return;
    DeleteCriticalSection((CRITICAL_SECTION *)mutex);
    free(mutex);
}

/**
 * @brief 阻塞取得一次递归临界区持有。
 * @param[in,out] mutex 存活借用锁，NULL 跳过。
 * @post 正常返回后当前线程增加一次持有，须对应解锁。
 */
void cc_mutex_lock(cc_mutex_t mutex)
{
    if (!mutex) return;
    EnterCriticalSection((CRITICAL_SECTION *)mutex);
}

/**
 * @brief 归还当前线程的一次临界区持有。
 * @param[in,out] mutex 当前线程持有的借用锁，NULL 跳过。
 * @post 持有深度减一，归零后其他线程可取得。
 */
void cc_mutex_unlock(cc_mutex_t mutex)
{
    if (!mutex) return;
    LeaveCriticalSection((CRITICAL_SECTION *)mutex);
}

/**
 * @brief 分配并初始化 Windows 条件变量包装。
 * @param[out] out_cond 必需空资源槽，进入后置 NULL，成功交付拥有对象。
 * @return 成功为 CC_OK，NULL 为 INVALID_ARGUMENT，分配失败为 OUT_OF_MEMORY。
 */
cc_result_t cc_cond_create(cc_cond_t *out_cond)
{
    if (!out_cond) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null condition output");
    }
    *out_cond = NULL;
    CONDITION_VARIABLE *cond = malloc(sizeof(CONDITION_VARIABLE));
    if (!cond) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate condition");
    }
    InitializeConditionVariable(cond);
    *out_cond = cond;
    return cc_result_ok();
}

/**
 * @brief 释放无需原生析构的 CONDITION_VARIABLE 包装。
 * @pre 已排空等待者、通知者和其他句柄借用。
 * @param[in,out] cond 消费拥有对象，NULL 安全。
 * @post 非 NULL 包装和借用失效。
 */
void cc_cond_destroy(cc_cond_t cond)
{
    free(cond);
}

/**
 * @brief 原子释放一次持有的临界区并无限等待，返回后重新持有。
 * @pre 当前线程恰好持有 mutex 一次，同时等待者使用同一谓词锁。
 * @param[in,out] cond 存活借用条件变量，NULL 跳过。
 * @param[in,out] mutex 当前线程持有的存活锁，NULL 跳过。
 * @post 正常返回后重新持锁，可能伪唤醒，调用者循环检查谓词，原生错误无返回通道。
 */
void cc_cond_wait(cc_cond_t cond, cc_mutex_t mutex)
{
    if (!cond || !mutex) return;
    SleepConditionVariableCS((CONDITION_VARIABLE *)cond, (CRITICAL_SECTION *)mutex, INFINITE);
}

/**
 * @brief 释放临界区并在毫秒预算内等待，再重新持锁。
 * @pre 当前线程恰好持有 mutex 一次，调用者在返回后重查谓词。
 * @param[in,out] cond 存活借用条件变量，NULL 返回零。
 * @param[in,out] mutex 当前线程持有的存活锁，NULL 返回零。
 * @param[in] timeout_ms 正数为毫秒预算，非正数为无限等待。
 * @return 原生等待成功为一，超时或失败为零，重新取得 mutex 可能额外等待。
 */
int cc_cond_timedwait(cc_cond_t cond, cc_mutex_t mutex, int timeout_ms)
{
    if (!cond || !mutex) return 0;
    DWORD timeout = timeout_ms > 0 ? (DWORD)timeout_ms : INFINITE;
    BOOL ok = SleepConditionVariableCS(
        (CONDITION_VARIABLE *)cond,
        (CRITICAL_SECTION *)mutex,
        timeout);
    return ok ? 1 : 0;
}

/**
 * @brief 请求唤醒一个已登记等待者。
 * @param[in,out] cond 存活借用条件变量，NULL 跳过。
 * @post 已调用原生唤醒，通知不积存，等待者仍需争用谓词锁。
 */
void cc_cond_signal(cc_cond_t cond)
{
    if (!cond) return;
    WakeConditionVariable((CONDITION_VARIABLE *)cond);
}

/**
 * @brief 请求唤醒当时全部等待者。
 * @param[in,out] cond 存活借用条件变量，NULL 跳过。
 * @post 已调用原生广播，等待者返回后重新核对共享谓词。
 */
void cc_cond_broadcast(cc_cond_t cond)
{
    if (!cond) return;
    WakeAllConditionVariable((CONDITION_VARIABLE *)cond);
}

#endif
