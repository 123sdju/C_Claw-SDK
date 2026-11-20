#ifndef CC_TEST_WINDOWS_H
#define CC_TEST_WINDOWS_H

#include <stddef.h>
#include <stdint.h>

/* Linux 模型保持 Windows API 的参数形状，调用约定 ABI 仍需原生 Windows 验证。 */
#define WINAPI
typedef uint32_t DWORD;
typedef int BOOL;
typedef void *LPVOID;
/**
 * @brief 模拟 Windows 原生线程入口类型。
 * @param[in,out] arg 创建方交付的上下文，具体所有权由真实平台 trampoline 处理。
 * @return 原生线程退出码，本端口固定为零。
 */
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID arg);
typedef struct stub_handle *HANDLE;
typedef struct { int held; } CRITICAL_SECTION;
typedef struct { int initialized; } CONDITION_VARIABLE;
typedef struct { int held; } SRWLOCK;
#define SRWLOCK_INIT {0}
#define INFINITE UINT32_MAX
#define WAIT_OBJECT_0 0U
#define WAIT_TIMEOUT 258U
#define WAIT_FAILED UINT32_MAX

/**
 * @brief 保存真实平台传入的线程入口，模型在等待或显式调度时执行。
 * @param[in] attributes 真实实现传 NULL，模型忽略。
 * @param[in] stack 真实实现传零，模型忽略。
 * @param[in] entry 借用 WINAPI 入口，保留到模拟执行。
 * @param[in,out] arg 借用入口上下文，所有权由真实平台入口处理。
 * @param[in] flags 真实实现传零，模型忽略。
 * @param[out] id 可选输出原生身份，真实实现传 NULL。
 * @return 成功为模型拥有句柄，注入失败为 NULL。
 */
HANDLE CreateThread(void *attributes, size_t stack, LPTHREAD_START_ROUTINE entry,
    LPVOID arg, DWORD flags, DWORD *id);
/**
 * @brief 查询完成或运行保存入口，支持定向等待失败。
 * @param[in,out] handle 存活模型句柄。
 * @param[in] timeout 零仅查询，INFINITE 模拟等待完成。
 * @return 完成为 WAIT_OBJECT_0，未完成零查询为 WAIT_TIMEOUT，注入失败为 WAIT_FAILED。
 */
DWORD WaitForSingleObject(HANDLE handle, DWORD timeout);
/** @brief 关闭完成句柄，可注入失败。 @param[in,out] handle 存活模型句柄。 @return 成功为一，失败为零并保留句柄。 */
BOOL CloseHandle(HANDLE handle);
/** @brief 查询句柄身份。 @param[in] handle 存活借用模型句柄。 @return 保存的非零线程 ID。 */
DWORD GetThreadId(HANDLE handle);
/** @brief 查询模型当前线程身份。 @return 当前非零线程 ID。 */
DWORD GetCurrentThreadId(void);
/**
 * @brief 初始化模型临界区，可注入原生失败。
 * @param[out] section 成功置零持有次数。
 * @param[in] spin 真实实现传零，模型忽略。
 * @param[in] flags 真实实现传零，模型忽略。
 * @return 成功为一，注入失败为零。
 */
BOOL InitializeCriticalSectionEx(CRITICAL_SECTION *section, DWORD spin, DWORD flags);
/**
 * @brief 模拟条件等待释放并重新取得临界区。
 * @param[in,out] condition 已初始化模型对象。
 * @param[in,out] section 恰好持有一次的临界区，返回后仍持有。
 * @param[in] timeout 记录毫秒预算或 INFINITE。
 * @return 固定零，供超时返回值与重新持锁检查。
 */
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *condition, CRITICAL_SECTION *section,
    DWORD timeout);

/* 其余原语仅用于串行模型计数，不模拟 Windows 调度或内核锁。 */
#define AcquireSRWLockExclusive(lock) ((void)((lock)->held++))
#define ReleaseSRWLockExclusive(lock) ((void)((lock)->held--))
#define DeleteCriticalSection(section) ((void)(section))
#define EnterCriticalSection(section) ((void)((section)->held++))
#define LeaveCriticalSection(section) ((void)((section)->held--))
#define InitializeConditionVariable(condition) ((void)((condition)->initialized = 1))
#define WakeConditionVariable(condition) ((void)(condition))
#define WakeAllConditionVariable(condition) ((void)(condition))

#endif
