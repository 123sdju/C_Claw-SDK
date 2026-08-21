



#include "cc/ports/cc_thread.h"

#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>

/*
 * 创建 Windows 线程。
 *
 * cc_thread_t 直接保存 HANDLE；join 后 CloseHandle。线程函数签名通过强转适配 Windows
 * LPTHREAD_START_ROUTINE，移植时需要保证返回值约定可接受。
 */
cc_result_t cc_thread_create(cc_thread_fn_t fn, void *arg, cc_thread_t *out_thread)
{
    if (!fn || !out_thread) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid thread argument");
    }
    HANDLE h = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)fn, arg, 0, NULL);
    if (!h) {
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create thread");
    }
    *out_thread = h;
    return cc_result_ok();
}

/* 等待线程结束并关闭 HANDLE；join 只能调用一次。 */
cc_result_t cc_thread_join(cc_thread_t thread)
{
    if (!thread) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null thread");
    }
    WaitForSingleObject((HANDLE)thread, INFINITE);
    CloseHandle((HANDLE)thread);
    return cc_result_ok();
}

/*
 * 创建 Windows mutex。
 *
 * 使用 CRITICAL_SECTION 作为进程内互斥原语，轻量且适合 SDK 内部对象保护。
 */
cc_result_t cc_mutex_create(cc_mutex_t *out_mutex)
{
    if (!out_mutex) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null mutex output");
    }
    CRITICAL_SECTION *cs = malloc(sizeof(CRITICAL_SECTION));
    if (!cs) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate mutex");
    }
    InitializeCriticalSection(cs);
    *out_mutex = cs;
    return cc_result_ok();
}

/* 销毁 CRITICAL_SECTION；调用方必须保证没有线程仍持有。 */
void cc_mutex_destroy(cc_mutex_t mutex)
{
    if (!mutex) return;
    DeleteCriticalSection((CRITICAL_SECTION *)mutex);
    free(mutex);
}

/* 进入临界区。 */
void cc_mutex_lock(cc_mutex_t mutex)
{
    if (!mutex) return;
    EnterCriticalSection((CRITICAL_SECTION *)mutex);
}

/* 离开临界区。 */
void cc_mutex_unlock(cc_mutex_t mutex)
{
    if (!mutex) return;
    LeaveCriticalSection((CRITICAL_SECTION *)mutex);
}

/* 创建 Windows condition variable。 */
cc_result_t cc_cond_create(cc_cond_t *out_cond)
{
    if (!out_cond) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null condition output");
    }
    CONDITION_VARIABLE *cond = malloc(sizeof(CONDITION_VARIABLE));
    if (!cond) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate condition");
    }
    InitializeConditionVariable(cond);
    *out_cond = cond;
    return cc_result_ok();
}

/* Windows CONDITION_VARIABLE 不需要显式 destroy，只释放包装内存。 */
void cc_cond_destroy(cc_cond_t cond)
{
    free(cond);
}

/* 无限等待条件变量；调用方进入前必须持有对应 CRITICAL_SECTION。 */
void cc_cond_wait(cc_cond_t cond, cc_mutex_t mutex)
{
    if (!cond || !mutex) return;
    SleepConditionVariableCS((CONDITION_VARIABLE *)cond, (CRITICAL_SECTION *)mutex, INFINITE);
}

/* 带超时等待条件变量，返回 1 表示被唤醒，0 表示超时或失败。 */
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

/* 唤醒一个等待者。 */
void cc_cond_signal(cc_cond_t cond)
{
    if (!cond) return;
    WakeConditionVariable((CONDITION_VARIABLE *)cond);
}

/* 唤醒所有等待者。 */
void cc_cond_broadcast(cc_cond_t cond)
{
    if (!cond) return;
    WakeAllConditionVariable((CONDITION_VARIABLE *)cond);
}

#endif
