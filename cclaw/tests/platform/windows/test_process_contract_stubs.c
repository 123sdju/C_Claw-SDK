#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "windows.h"
#include "io.h"
#include "cc/app/cc_cancel_token.h"

/** @brief 固定句柄记录，原生对象只允许唯一拥有者关闭一次。 */
struct model_handle {
    int kind; /**< 一为管道，二为进程，三为主线程。 */
    int closed; /**< 唯一拥有者已经关闭。 */
    int finished; /**< 模型直接进程已经结束。 */
    int waited; /**< 已取得原生完成等待结果。 */
    size_t cursor; /**< 该读端已消费的payload字节数。 */
};
/** @brief CRT接管记录，open到stream再到closed的拥有权单向推进。 */
typedef struct {
    HANDLE native; /**< CRT接管的拥有原生句柄。 */
    FILE *stream; /**< 非NULL时FILE已经接管fd。 */
    int open; /**< fd或FILE仍拥有句柄。 */
} model_fd_t;
static struct model_handle handles[64], standard_handle;
static model_fd_t descriptors[16];
static size_t handle_count, descriptor_count;
static void *allocations[64];
static const char *payload;
static ULONGLONG ticks;
static unsigned read_calls;
static int continuous, running, expect_empty_environment;
static int fail_pipe, pipe_calls, fail_protect, protect_calls, fail_create;
static int fail_open, open_calls, fail_fdopen, fdopen_calls, fail_wait;
static int fail_calloc, fail_realloc;
static char model_command[8192];
unsigned model_gate_held;
DWORD model_last_error, model_exit_code;
HANDLE model_standard = &standard_handle;

/** @brief 建立固定模型句柄。 @param[in] kind 一为管道，二为进程，三为主线程。 @return 拥有句柄，fixture检查容量。 */
static HANDLE new_handle(int kind)
{
    assert(handle_count < 64);
    HANDLE handle = &handles[handle_count++];
    handle->kind = kind;
    return handle;
}

/** @brief 创建管道并可注入第N次失败。 @param[out] read_end 成功交付读端。 @param[out] write_end 成功交付写端。 @param[in] attributes 借用属性，忽略。 @param[in] size 容量提示，忽略。 @return TRUE或定向FALSE。 */
BOOL CreatePipe(HANDLE *read_end, HANDLE *write_end, SECURITY_ATTRIBUTES *attributes, DWORD size)
{
    (void)attributes; (void)size; assert(model_gate_held == 1);
    if (++pipe_calls == fail_pipe) return FALSE;
    *read_end = new_handle(1); *write_end = new_handle(1); return TRUE;
}

/** @brief 记录继承属性保护步骤。 @param[in,out] handle 存活借用。 @param[in] mask 继承掩码。 @param[in] flags 必须清零。 @return 正常TRUE，注入FALSE。 */
BOOL SetHandleInformation(HANDLE handle, DWORD mask, DWORD flags)
{
    assert(!handle->closed && mask == HANDLE_FLAG_INHERIT && flags == 0 && model_gate_held == 1);
    return ++protect_calls == fail_protect ? FALSE : TRUE;
}

/** @brief 唯一关闭拥有句柄并检查进程已跨等待屏障。 @param[in,out] handle 消费拥有句柄。 @return TRUE。 */
BOOL CloseHandle(HANDLE handle)
{
    assert(handle != model_standard && !handle->closed);
    if (handle->kind == 2) assert(handle->finished && handle->waited);
    handle->closed = 1; return TRUE;
}

/** @brief 查询模拟输出。 @param[in] handle 存活读端。 @param[out] buffer 未使用。 @param[in] size 未使用。 @param[out] read 未使用。 @param[out] available 输出可读字节数。 @param[out] remaining 未使用。 @return 余量可读TRUE，EOF为FALSE/BROKEN_PIPE。 */
BOOL PeekNamedPipe(HANDLE handle, void *buffer, DWORD size, DWORD *read, DWORD *available, DWORD *remaining)
{
    (void)buffer; (void)size; (void)read; (void)remaining; assert(!handle->closed);
    *available = continuous ? 1U : (DWORD)(strlen(payload) - handle->cursor);
    if (*available) return TRUE;
    model_last_error = ERROR_BROKEN_PIPE; return FALSE;
}

/** @brief 读取模拟余量，连续模式用有限断言防旧预算循环挂住。 @param[in,out] handle 借用读端。 @param[out] buffer 输出字节。 @param[in] size 容量。 @param[out] read 实际数量。 @param[in] overlapped 未使用。 @return TRUE。 */
BOOL ReadFile(HANDLE handle, void *buffer, DWORD size, DWORD *read, void *overlapped)
{
    (void)overlapped; assert(!handle->closed && ++read_calls < 1000);
    size_t count = continuous ? 1 : strlen(payload) - handle->cursor;
    if (count > size) count = size;
    memcpy(buffer, continuous ? "x" : payload + handle->cursor, count);
    if (!continuous) handle->cursor += count;
    *read = (DWORD)count; return TRUE;
}

/** @brief 模拟等待并记录完成屏障。 @param[in,out] handle 借用进程。 @param[in] timeout 毫秒或INFINITE。 @return 完成、超时或注入失败。 */
DWORD WaitForSingleObject(HANDLE handle, DWORD timeout)
{
    assert(!handle->closed && handle->kind == 2);
    if (fail_wait) { fail_wait = 0; return WAIT_FAILED; }
    if (handle->finished) { handle->waited = 1; return WAIT_OBJECT_0; }
    assert(timeout != INFINITE); ticks += timeout; return WAIT_TIMEOUT;
}

/** @brief 让模型直接进程完成。 @param[in,out] handle 借用进程。 @param[in] code 退出码，忽略。 @return TRUE，仍需后续等待。 */
BOOL TerminateProcess(HANDLE handle, UINT code)
{ (void)code; assert(!handle->closed); handle->finished = 1; return TRUE; }

/**
 * @brief 检查真实创建参数并交付模型进程/线程拥有句柄。
 * @param[in] application 可选应用名，忽略。
 * @param[in,out] command 借用NUL结尾命令行，保持原值。
 * @param[in] process_attributes 未使用。
 * @param[in] thread_attributes 未使用。
 * @param[in] inherit 必须为TRUE。
 * @param[in] flags 创建标志，忽略。
 * @param[in] environment 空环境场景借用双NUL块。
 * @param[in] directory 可选工作目录，忽略。
 * @param[in] startup 借用标准句柄，协议模式核对stderr合并。
 * @param[out] process 成功交付两个拥有句柄。
 * @return TRUE或注入失败FALSE。
 */
BOOL CreateProcessA(const char *application, char *command, void *process_attributes,
    void *thread_attributes, BOOL inherit, DWORD flags, void *environment,
    const char *directory, STARTUPINFOA *startup, PROCESS_INFORMATION *process)
{
    (void)application; (void)process_attributes; (void)thread_attributes; (void)flags; (void)directory;
    assert(model_gate_held == 1 && inherit && command[0]);
    assert(strlen(command) < sizeof(model_command)); strcpy(model_command, command);
    if (expect_empty_environment) assert(environment && ((char *)environment)[0] == 0 && ((char *)environment)[1] == 0);
    if (startup->hStdInput != model_standard) assert(startup->hStdOutput == startup->hStdError);
    if (fail_create) return FALSE;
    process->hProcess = new_handle(2); process->hThread = new_handle(3);
    process->hProcess->finished = !running; return TRUE;
}

/** @brief 推进确定性时钟。 @return 当前毫秒数。 */
ULONGLONG GetTickCount64(void) { return ++ticks; }
/** @brief 模拟休眠经过时间。 @param[in] milliseconds 毫秒数。 @post 模型时间增加。 */
void Sleep(DWORD milliseconds) { ticks += milliseconds; }

/** @brief 接管原生句柄到CRT记录。 @param[in] handle 拥有句柄值。 @param[in] flags 读取或写入模式。 @return 模型fd或-1，失败不接管。 */
int _open_osfhandle(intptr_t handle, int flags)
{
    assert(flags == _O_WRONLY || flags == _O_RDONLY);
    if (++open_calls == fail_open) return -1;
    assert(descriptor_count < 16);
    size_t index = descriptor_count++;
    descriptors[index].native = (HANDLE)handle; descriptors[index].open = 1;
    return (int)index + 100;
}

/** @brief 将CRT记录转给拥有FILE。 @param[in] fd 模型拥有fd。 @param[in] mode 借用访问串。 @return FILE或注入NULL，失败保留fd。 */
FILE *_fdopen(int fd, const char *mode)
{
    (void)mode; model_fd_t *record = &descriptors[fd - 100]; assert(record->open);
    if (++fdopen_calls == fail_fdopen) return NULL;
    record->stream = tmpfile(); assert(record->stream); return record->stream;
}

/** @brief 消费未转给FILE的CRT记录。 @param[in] fd 模型拥有fd。 @return 零，句柄唯一关闭。 */
int _close(int fd)
{
    model_fd_t *record = &descriptors[fd - 100]; assert(record->open && !record->stream);
    record->open = 0; CloseHandle(record->native); return 0;
}

/** @brief 关闭已接管CRT/原生句柄的FILE。 @param[in,out] stream 消费拥有FILE。 @return 真实fclose结果。 */
static int source_fclose(FILE *stream)
{
    for (size_t i = 0; i < descriptor_count; i++) {
        if (descriptors[i].stream == stream) {
            assert(descriptors[i].open); descriptors[i].open = 0;
            descriptors[i].stream = NULL; CloseHandle(descriptors[i].native);
            return fclose(stream);
        }
    }
    abort();
}

/** @brief 登记私有生产分配。 @param[in] allocation 新拥有块或NULL。 @return 原指针，容量耗尽触发断言。 */
static void *track(void *allocation)
{
    if (!allocation) return NULL;
    for (size_t i = 0; i < 64; i++) {
        if (!allocations[i]) {
            allocations[i] = allocation;
            return allocation;
        }
    }
    abort();
}
/** @brief 记录私有malloc。 @param[in] bytes 字节数。 @return 拥有块或NULL。 */
static void *source_malloc(size_t bytes) { return track(malloc(bytes)); }
/** @brief 记录或拒绝私有calloc。 @param[in] count 元素数。 @param[in] bytes 单元字节数。 @return 拥有块或NULL。 */
static void *source_calloc(size_t count, size_t bytes)
{
    if (fail_calloc) { fail_calloc = 0; return NULL; }
    return track(calloc(count, bytes));
}
/** @brief 记录或拒绝捕获扩容。 @param[in,out] old 原拥有块，失败保留。 @param[in] bytes 新容量。 @return 新拥有块或NULL。 */
static void *source_realloc(void *old, size_t bytes)
{
    if (fail_realloc) { fail_realloc = 0; return NULL; }
    size_t slot = 0; while (slot < 64 && allocations[slot] != old) slot++;
    int had_old = old != NULL;
    void *next = realloc(old, bytes);
    if (!next) return NULL;
    if (had_old && slot < 64) { allocations[slot] = next; return next; }
    return track(next);
}
/** @brief 消费生产拥有块，外部cc_copy_string的空串也允许释放。 @param[in,out] allocation 拥有对象或NULL。 @post 登记清除且对象释放。 */
static void source_free(void *allocation)
{
    for (size_t i = 0; i < 64; i++) {
        if (allocations[i] == allocation) {
            allocations[i] = NULL;
            break;
        }
    }
    free(allocation);
}

/* 私有重命名包含真实端口，原生API、CRT交接与本TU分配为模型边界。 */
#define cc_process_run review_process_run
#define cc_process_result_free review_process_result_free
#define cc_process_pipe_spawn review_process_pipe_spawn
#define cc_process_pipe_write review_process_pipe_write
#define cc_process_pipe_read_line review_process_pipe_read_line
#define cc_process_pipe_read_line_timeout review_process_pipe_read_line_timeout
#define cc_process_pipe_read_line_timeout_cancel review_process_pipe_read_line_timeout_cancel
#define cc_process_pipe_stop review_process_pipe_stop
#define cc_process_pipe_destroy review_process_pipe_destroy
#define malloc source_malloc
#define calloc source_calloc
#define realloc source_realloc
#define free source_free
#define fclose source_fclose
#ifndef _WIN32
#define _WIN32 1
#endif
#include "../../../platforms/windows/src/cc_windows_process.c"
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef fclose

/** @brief 消费结果并断言分类。 @param[in] result 按值移交拥有结果。 @param[in] code 预期状态。 @post 诊断释放，错误分类不符即终止。 */
static void expect_result(cc_result_t result, cc_error_code_t code)
{ int actual = result.code; cc_result_free(&result); assert(actual == code); }

/** @brief 在所有前一场景资源清理后重置模型。 @post 计数与故障清零，默认有限payload和已结束进程。 */
static void reset_model(void)
{
    assert(!model_gate_held);
    for (size_t i = 0; i < handle_count; i++) assert(handles[i].closed);
    for (size_t i = 0; i < descriptor_count; i++) assert(!descriptors[i].open);
    for (size_t i = 0; i < 64; i++) assert(!allocations[i]);
    memset(handles, 0, sizeof(handles)); memset(descriptors, 0, sizeof(descriptors));
    handle_count = descriptor_count = 0; payload = "tail\r"; ticks = 0; read_calls = 0;
    continuous = running = expect_empty_environment = 0;
    fail_pipe = pipe_calls = fail_protect = protect_calls = fail_create = 0;
    fail_open = open_calls = fail_fdopen = fdopen_calls = fail_wait = 0;
    fail_calloc = fail_realloc = 0; model_last_error = 0; model_exit_code = 7;
    model_command[0] = '\0';
}

/** @brief 从公共入口构造一次模型运行。 @param[in] timeout 毫秒预算。 @param[out] output 空结果槽。 @return 真实端口拥有结果。 */
static cc_result_t run_model(int timeout, cc_process_result_t *output)
{
    cc_process_options_t options = {0}; options.command = "program";
    char *args[] = {"program", "argument with spaces", NULL}; options.args = args;
    options.capture_stdout = options.capture_stderr = 1; options.timeout_ms = timeout;
    return review_process_run(&options, output);
}

/** @brief 验证捕获、OOM、实际预算、等待失败及空环境双NUL。 @post 各场景拥有结果清理，最后原生句柄均关闭。 */
static void test_run_contract(void)
{
    cc_process_result_t output = {0}; reset_model();
    expect_result(review_process_run(NULL, &output), CC_ERR_INVALID_ARGUMENT);
    cc_process_options_t invalid = {0};
    expect_result(review_process_run(&invalid, &output), CC_ERR_INVALID_ARGUMENT);
    invalid.command = "program";
    expect_result(review_process_run(&invalid, NULL), CC_ERR_INVALID_ARGUMENT);
    expect_result(run_model(100, &output), CC_OK);
    assert(output.exit_code == 7 && !output.timed_out && strcmp(output.stdout_text, "tail\r") == 0);
    assert(strcmp(model_command, "\"program\" \"argument with spaces\"") == 0);
    review_process_result_free(&output); reset_model(); fail_realloc = 1; running = 1;
    expect_result(run_model(100, &output), CC_ERR_OUT_OF_MEMORY);
    assert(!output.stdout_text && !output.stderr_text && !fail_realloc);
    reset_model(); continuous = running = 1;
    expect_result(run_model(5, &output), CC_OK); assert(output.timed_out && output.exit_code == -1);
    review_process_result_free(&output); reset_model(); fail_wait = 1; running = 1;
    expect_result(run_model(100, &output), CC_ERR_PLATFORM); reset_model();
    char *env[] = {NULL}; cc_process_options_t options = {0};
    options.command = "program"; options.env = env; expect_empty_environment = 1;
    expect_result(review_process_run(&options, &output), CC_OK);
    review_process_result_free(&output); reset_model();
    char oversized[9000]; memset(oversized, 'x', sizeof(oversized) - 1); oversized[sizeof(oversized) - 1] = '\0';
    options.command = oversized; options.env = NULL;
    expect_result(review_process_run(&options, &output), CC_ERR_INVALID_ARGUMENT); reset_model();
}

/** @brief 验证创建半失败的HANDLE→fd→FILE唯一交接和创建门退出。 @post 每种故障后资源登记均为空。 */
static void test_spawn_failures(void)
{
    reset_model(); cc_process_pipe_t *invalid = (cc_process_pipe_t *)&invalid;
    expect_result(review_process_pipe_spawn(NULL, NULL, &invalid), CC_ERR_INVALID_ARGUMENT);
    assert(invalid == (cc_process_pipe_t *)&invalid);
    expect_result(review_process_pipe_spawn("program", NULL, NULL), CC_ERR_INVALID_ARGUMENT);
    for (int stage = 0; stage < 9; stage++) {
        reset_model(); cc_process_pipe_t *pipe = (cc_process_pipe_t *)&pipe;
        if (stage < 2) fail_pipe = stage + 1;
        else if (stage < 4) fail_protect = stage - 1;
        else if (stage == 4) fail_create = 1;
        else if (stage < 7) fail_open = stage - 4;
        else fail_fdopen = stage - 6;
        expect_result(review_process_pipe_spawn("program", NULL, &pipe), CC_ERR_PLATFORM); assert(!pipe);
    }
    reset_model(); fail_calloc = 1; cc_process_pipe_t *pipe = NULL;
    expect_result(review_process_pipe_spawn("program", NULL, &pipe), CC_ERR_OUT_OF_MEMORY); assert(!pipe);
    reset_model();
}

/** @brief 验证EOF末行、取消、持续输出预算以及stop后的raw句柄失效。 @post 行、token和进程对象全部清理。 */
static void test_pipe_read_contract(void)
{
    reset_model(); cc_process_pipe_t *pipe = NULL; char *line = NULL;
    expect_result(review_process_pipe_spawn("program", NULL, &pipe), CC_OK);
    expect_result(review_process_pipe_read_line(pipe, &line), CC_OK); assert(strcmp(line, "tail") == 0);
    source_free(line); line = (char *)&line;
    expect_result(review_process_pipe_read_line(pipe, &line), CC_ERR_PLATFORM); assert(!line);
    review_process_pipe_stop(pipe); review_process_pipe_stop(pipe);
    line = (char *)&line;
    expect_result(review_process_pipe_read_line(pipe, &line), CC_ERR_INVALID_ARGUMENT); assert(!line);
    review_process_pipe_destroy(pipe); reset_model(); continuous = running = 1;
    expect_result(review_process_pipe_spawn("program", NULL, &pipe), CC_OK);
    expect_result(review_process_pipe_read_line_timeout(pipe, 3, &line), CC_ERR_TIMEOUT); assert(!line);
    cc_cancel_source_t *source = NULL; expect_result(cc_cancel_source_create(&source), CC_OK);
    cc_cancel_source_cancel(source);
    expect_result(review_process_pipe_read_line_timeout_cancel(pipe, 100,
        cc_cancel_source_token(source), &line), CC_ERR_CANCELLED); assert(!line);
    cc_cancel_source_destroy(source); review_process_pipe_destroy(pipe); reset_model();
}

/** @brief 运行真实Windows进程源码的API/CRT模型回归。 @return 所有断言满足为零，失败终止。 @note 原生ANSI/命令解释器与Windows内核调度仍需真实平台验证。 */
int main(void)
{
    test_run_contract(); test_spawn_failures(); test_pipe_read_contract(); return 0;
}
