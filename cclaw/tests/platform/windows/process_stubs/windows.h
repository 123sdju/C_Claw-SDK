#pragma once
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef uint32_t DWORD;
typedef uint64_t ULONGLONG;
typedef unsigned UINT;
typedef int BOOL;
typedef struct model_handle *HANDLE;
typedef int SRWLOCK;
typedef struct { DWORD length; void *security; BOOL inherit; } SECURITY_ATTRIBUTES;
typedef struct { DWORD size, dwFlags; HANDLE hStdInput, hStdOutput, hStdError; } STARTUPINFOA;
typedef struct { HANDLE hProcess, hThread; } PROCESS_INFORMATION;
#define SRWLOCK_INIT 0
#define TRUE 1
#define FALSE 0
#define INFINITE UINT32_MAX
#define WAIT_OBJECT_0 0U
#define WAIT_TIMEOUT 258U
#define WAIT_FAILED UINT32_MAX
#define ERROR_BROKEN_PIPE 109U
#define HANDLE_FLAG_INHERIT 1U
#define CREATE_NO_WINDOW 1U
#define STARTF_USESTDHANDLES 1U
#define STD_INPUT_HANDLE 0U
#define STD_OUTPUT_HANDLE 1U
#define STD_ERROR_HANDLE 2U
extern unsigned model_gate_held;
extern DWORD model_last_error;
extern DWORD model_exit_code;
extern HANDLE model_standard;
/* 串行模型核对创建门配对，真实SRW调度由原生Windows验证。 */
#define AcquireSRWLockExclusive(lock) ((void)(lock), assert(model_gate_held++ == 0))
#define ReleaseSRWLockExclusive(lock) ((void)(lock), assert(model_gate_held-- == 1))
#define GetLastError() model_last_error
#define GetStdHandle(kind) ((void)(kind), model_standard)
#define GetExitCodeProcess(handle, output) ((void)(handle), *(output) = model_exit_code, TRUE)

/** @brief 创建模型管道对。 @param[out] read_end 拥有读端。 @param[out] write_end 拥有写端。 @param[in] attributes 借用属性。 @param[in] size 原生容量提示。 @return 成功为TRUE，故障为FALSE。 */
BOOL CreatePipe(HANDLE *read_end, HANDLE *write_end, SECURITY_ATTRIBUTES *attributes, DWORD size);
/** @brief 模拟设置父端继承标志。 @param[in,out] handle 借用句柄。 @param[in] mask 掩码。 @param[in] flags 新标志。 @return 成功为TRUE，故障为FALSE。 */
BOOL SetHandleInformation(HANDLE handle, DWORD mask, DWORD flags);
/** @brief 消费模型拥有句柄。 @param[in,out] handle 尚未关闭的拥有句柄。 @return TRUE，重复关闭触发断言。 */
BOOL CloseHandle(HANDLE handle);
/** @brief 查询模型管道可读数。 @param[in] handle 借用读端。 @param[out] buffer 未使用。 @param[in] size 未使用容量。 @param[out] read 未使用。 @param[out] available 必需可读字节槽。 @param[out] remaining 未使用。 @return 有效查询为TRUE，EOF/故障为FALSE。 */
BOOL PeekNamedPipe(HANDLE handle, void *buffer, DWORD size, DWORD *read, DWORD *available, DWORD *remaining);
/** @brief 读取模型管道字节。 @param[in,out] handle 借用读端，推进游标。 @param[out] buffer 可写字节。 @param[in] size 最大字节数。 @param[out] read 实际字节数。 @param[in] overlapped 未使用。 @return 成功TRUE，EOF为FALSE。 */
BOOL ReadFile(HANDLE handle, void *buffer, DWORD size, DWORD *read, void *overlapped);
/** @brief 模拟进程等待。 @param[in,out] handle 借用进程，完成时记录屏障。 @param[in] timeout 毫秒或INFINITE。 @return Windows等待常量。 */
DWORD WaitForSingleObject(HANDLE handle, DWORD timeout);
/** @brief 模拟进程终止请求。 @param[in,out] handle 借用进程。 @param[in] code 退出码，模型忽略。 @return TRUE，完成状态置一。 */
BOOL TerminateProcess(HANDLE handle, UINT code);
/**
 * @brief 保存真实生产源码构造的进程参数并交付模型句柄。
 * @param[in] application 可选应用名，模型忽略。
 * @param[in,out] command 可写借用命令行，模型只检查NUL结尾。
 * @param[in] process_attributes 未使用属性。
 * @param[in] thread_attributes 未使用属性。
 * @param[in] inherit 继承标志，模型要求为TRUE。
 * @param[in] flags 创建标志，模型忽略。
 * @param[in] environment 可选ANSI环境，空数组场景检查双NUL。
 * @param[in] directory 可选目录，模型忽略。
 * @param[in] startup 借用标准句柄描述。
 * @param[out] process 成功交付两个拥有句柄。
 * @return 成功TRUE，注入失败FALSE。
 */
BOOL CreateProcessA(const char *application, char *command, void *process_attributes,
    void *thread_attributes, BOOL inherit, DWORD flags, void *environment,
    const char *directory, STARTUPINFOA *startup, PROCESS_INFORMATION *process);
/** @brief 推进模型单调时钟。 @return 毫秒值，用于持续有数据时的预算测试。 */
ULONGLONG GetTickCount64(void);
/** @brief 推进模型无数据休眠时间。 @param[in] milliseconds 请求毫秒数。 @post 模型时钟增加。 */
void Sleep(DWORD milliseconds);
