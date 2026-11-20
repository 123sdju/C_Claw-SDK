



#include "cc/app/cc_cancel_token.h"
#include "cc/ports/cc_process.h"
#include "cc/internal/cc_alloc.h"

#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <stdint.h>

/* 串行化本端口创建可继承端到关闭子端的窗口，避免并行SDK子进程继承彼此的临时管道。 */
static SRWLOCK process_spawn_gate = SRWLOCK_INIT;

/** @brief 拥有一条Windows输出流的增长缓冲，oom在捕获失败后保持到统一清理。 */
typedef struct {
    char *data; /**< 拥有的NUL结尾输出，成功移交结果。 */
    size_t total; /**< 已捕获正文的字节数。 */
    size_t cap; /**< 包含末尾NUL的分配容量。 */
    int oom; /**< 溢出或分配失败后置一。 */
} capture_buffer_t;

/**
 * @brief 按需倍增容量并追加原始输出字节，维持NUL结尾。
 * @param[in,out] capture 可选拥有状态，NULL或零长度按无需追加处理。
 * @param[in] buf len非零时借用至少len字节输入。
 * @param[in] len 要追加的正文长度，单位字节。
 * @return 成功为一，溢出或分配失败为零并置粘滞oom，旧缓冲保留供清理。
 */
static int capture_append(capture_buffer_t *capture, const char *buf, size_t len)
{
    if (!capture || len == 0) return 1;
    if (capture->oom) return 0;
    if (len > SIZE_MAX - capture->total - 1) { capture->oom = 1; return 0; }
    if (capture->total + len + 1 > capture->cap) {
        size_t new_cap = capture->cap ? capture->cap : 4096;
        while (capture->total + len + 1 > new_cap) {
            if (new_cap > SIZE_MAX / 2) { capture->oom = 1; return 0; }
            new_cap *= 2;
        }
        char *new_data = realloc(capture->data, new_cap);
        if (!new_data) { capture->oom = 1; return 0; }
        capture->data = new_data;
        capture->cap = new_cap;
    }
    memcpy(capture->data + capture->total, buf, len);
    capture->total += len;
    capture->data[capture->total] = '\0';
    return 1;
}

/**
 * @brief Peek后读取最多十六块数据，为另一流和预算检查保留轮询机会。
 * @param[in] h 存活借用的读取HANDLE。
 * @param[in,out] capture 拥有捕获状态，失败仍由调用方清理。
 * @return 暂无数据/管道关闭为零，达到本轮块数为一，原生错误或分配失败为负一。
 */
static int drain_pipe(HANDLE h, capture_buffer_t *capture)
{
    char buf[4096];
    DWORD available = 0;
    for (int chunks = 0; chunks < 16; chunks++) {
        if (!PeekNamedPipe(h, NULL, 0, NULL, &available, NULL))
            return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
        if (available == 0) return 0;
        DWORD nread = 0;
        if (ReadFile(h, buf, sizeof(buf) < available ? sizeof(buf) : available, &nread, NULL) && nread > 0) {
            if (!capture_append(capture, buf, nread)) return -1;
        } else {
            return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
        }
    }
    return 1;
}

/**
 * @brief 终止直接拥有的进程并等待原生完成屏障。
 * @param[in] process 借用有效进程HANDLE，调用期间保持拥有权。
 * @return 等待完成为零，原生等待失败为负一，进程已经退出也可成功。
 */
static int terminate_and_wait(HANDLE process)
{
    (void)TerminateProcess(process, (UINT)-1);
    return WaitForSingleObject(process, INFINITE) == WAIT_OBJECT_0 ? 0 : -1;
}

/**
 * @brief 子进程结束后分轮排空余量，同时保留有限总预算。
 * @param[in] pipe 借用有效读取HANDLE。
 * @param[in,out] capture 拥有捕获状态，失败保留缓冲供统一清理。
 * @param[in] started 开始时GetTickCount64毫秒值。
 * @param[in] timeout_ms 正数为总预算，非正数不限时。
 * @param[in,out] timed_out 已有超时状态，达到预算时置一。
 * @return 余量完成或超时为零，读取或分配失败为负一。
 */
static int drain_after_exit(HANDLE pipe, capture_buffer_t *capture, ULONGLONG started,
    int timeout_ms, int *timed_out)
{
    int more;
    do {
        more = drain_pipe(pipe, capture);
        if (more <= 0 || *timed_out) break;
        if (timeout_ms > 0 && GetTickCount64() - started >= (ULONGLONG)timeout_ms) {
            *timed_out = 1;
            break;
        }
    } while (more > 0);
    return more < 0 ? -1 : 0;
}

/**
 * @brief 向可写命令行缓冲追加一个字符并保持NUL。
 * @param[in,out] buf 必需可写缓冲，当前内容已NUL结尾。
 * @param[in] cap 缓冲总字节容量。
 * @param[in,out] pos 当前长度，须小于cap，成功递增。
 * @param[in] ch 追加字节。
 * @return 容量足够为一，不足为零并保留原内容。
 */
static int append_cmd_char(char *buf, size_t cap, size_t *pos, char ch)
{
    if (*pos + 1 >= cap) return 0;
    buf[(*pos)++] = ch;
    buf[*pos] = '\0';
    return 1;
}

/**
 * @brief 在非首个命令行参数前追加分隔空格。
 * @param[in,out] buf 必需可写NUL结尾缓冲。
 * @param[in] cap 缓冲字节容量。
 * @param[in,out] pos 当前长度，成功追加时递增。
 * @return 首参数无需分隔或追加成功为一，容量不足为零。
 */
static int append_cmd_space(char *buf, size_t cap, size_t *pos)
{
    if (*pos == 0) return 1;
    return append_cmd_char(buf, cap, pos, ' ');
}

/**
 * @brief 按常用Windows CRT argv规则引用参数，双写引号前和末尾反斜杠。
 * @param[in,out] buf 必需可写NUL结尾命令行缓冲。
 * @param[in] cap 总字节容量，当前调用使用8192字节缓冲。
 * @param[in,out] pos 当前已用长度，失败也可能保留已追加前缀。
 * @param[in] arg 借用参数，NULL按空串引用。
 * @return 完整参数写入为一，容量不足为零，由上层放弃整条命令行。
 * @note CreateProcessA直接接收该命令行，具体子程序的参数解析由其自身实现。
 */
static int append_windows_quoted_arg(char *buf, size_t cap, size_t *pos, const char *arg)
{
    if (!arg) arg = "";
    if (!append_cmd_space(buf, cap, pos)) return 0;
    if (!append_cmd_char(buf, cap, pos, '"')) return 0;

    size_t backslashes = 0;
    for (const char *p = arg; *p; p++) {
        if (*p == '\\') {
            backslashes++;
            if (backslashes >= cap) return 0;
            continue;
        }
        if (*p == '"') {
            for (size_t i = 0; i < backslashes * 2 + 1; i++) {
                if (!append_cmd_char(buf, cap, pos, '\\')) return 0;
            }
            if (!append_cmd_char(buf, cap, pos, '"')) return 0;
            backslashes = 0;
            continue;
        }
        for (size_t i = 0; i < backslashes; i++) {
            if (!append_cmd_char(buf, cap, pos, '\\')) return 0;
        }
        backslashes = 0;
        if (!append_cmd_char(buf, cap, pos, *p)) return 0;
    }

    for (size_t i = 0; i < backslashes * 2; i++) {
        if (!append_cmd_char(buf, cap, pos, '\\')) return 0;
    }
    return append_cmd_char(buf, cap, pos, '"');
}

/**
 * @brief 复制调用方环境数组为ANSI双NUL终止的Windows环境块。
 * @param[in] env 可选NULL终止借用数组，NULL请求继承环境，非NULL空数组产生空环境块。
 * @return 拥有字节块供free，NULL输入或溢出/分配失败返回NULL，上层用原输入区分。
 * @note 环境项内容沿调用方原值，不执行POSIX式合并、排序或Unicode转码。
 */
static char *build_windows_env_block(char **env)
{
    if (!env) return NULL;

    size_t total = 2;
    for (char **e = env; *e; e++) {
        size_t length = strlen(*e);
        if (length >= SIZE_MAX - total) return NULL;
        total += length + 1;
    }

    char *block = calloc(total, 1);
    if (!block) return NULL;

    char *p = block;
    for (char **e = env; *e; e++) {
        size_t len = strlen(*e);
        memcpy(p, *e, len);
        p += len + 1;
    }
    *p = '\0';
    return block;
}

/**
 * @brief 释放结果拥有的stdout/stderr并清零结构。
 * @param[in,out] result 可选拥有结果，NULL安全，不释放结构本身。
 * @post 非NULL结果可重复清理，旧字符串借用失效。
 */
void cc_process_result_free(cc_process_result_t *result)
{
    if (!result) return;
    free(result->stdout_text);
    free(result->stderr_text);
    memset(result, 0, sizeof(cc_process_result_t));
}

/**
 * @brief 通过CreateProcessA运行命令，分轮捕获输出并等待进程结束。
 * @param[in] options 必需稳定借用选项，command必填，有args[0]时引用command与args[1..]，否则cmd.exe /c。
 * @param[out] out_result 必需无已有资源槽，基础参数有效后清零，成功拥有两条输出字符串。
 * @return 空参数/命令行超长为INVALID_ARGUMENT，分配失败为OUT_OF_MEMORY，原生创建/捕获/等待失败为PLATFORM，正常退出或超时终止完成为CC_OK。
 * @note 正预算按GetTickCount64实际时间检查，超时终止并等待直接进程，结果timed_out=1、exit_code=-1。
 *       command/working_dir/env使用Windows ANSI代码页，创建返回前释放环境块，进程业务退出码由结果表达，错误清理全部输出。
 */
cc_result_t cc_process_run(
    const cc_process_options_t *options,
    cc_process_result_t *out_result
)
{
    if (!options || !options->command || !out_result)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid process run arguments");
    memset(out_result, 0, sizeof(cc_process_result_t));
    AcquireSRWLockExclusive(&process_spawn_gate);

    HANDLE stdout_read = NULL, stdout_write = NULL;
    HANDLE stderr_read = NULL, stderr_write = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };

    if (options->capture_stdout) {
        if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
            ReleaseSRWLockExclusive(&process_spawn_gate);
            return cc_result_error(CC_ERR_PLATFORM, "Failed to create stdout pipe");
        }
        if (!SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0)) {
            CloseHandle(stdout_read); CloseHandle(stdout_write);
            ReleaseSRWLockExclusive(&process_spawn_gate);
            return cc_result_error(CC_ERR_PLATFORM, "Failed to protect stdout handle");
        }
    }

    if (options->capture_stderr) {
        if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
            if (stdout_read) { CloseHandle(stdout_read); CloseHandle(stdout_write); }
            ReleaseSRWLockExclusive(&process_spawn_gate);
            return cc_result_error(CC_ERR_PLATFORM, "Failed to create stderr pipe");
        }
        if (!SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0)) {
            if (stdout_read) { CloseHandle(stdout_read); CloseHandle(stdout_write); }
            CloseHandle(stderr_read); CloseHandle(stderr_write);
            ReleaseSRWLockExclusive(&process_spawn_gate);
            return cc_result_error(CC_ERR_PLATFORM, "Failed to protect stderr handle");
        }
    }

    char cmd_line[8192] = {0};
    size_t cmd_pos = 0;
    int cmd_ok = 1;
    if (options->args && options->args[0]) {
        cmd_ok = append_windows_quoted_arg(cmd_line, sizeof(cmd_line), &cmd_pos, options->command);
        for (int i = 1; cmd_ok && options->args[i]; i++) {
            cmd_ok = append_windows_quoted_arg(cmd_line, sizeof(cmd_line), &cmd_pos, options->args[i]);
        }
    } else {
        cmd_ok = append_windows_quoted_arg(cmd_line, sizeof(cmd_line), &cmd_pos, "cmd.exe") &&
                 append_windows_quoted_arg(cmd_line, sizeof(cmd_line), &cmd_pos, "/c") &&
                 append_windows_quoted_arg(cmd_line, sizeof(cmd_line), &cmd_pos, options->command);
    }
    if (!cmd_ok) {
        if (stdout_read) { CloseHandle(stdout_read); CloseHandle(stdout_write); }
        if (stderr_read) { CloseHandle(stderr_read); CloseHandle(stderr_write); }
        ReleaseSRWLockExclusive(&process_spawn_gate);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Command line too long");
    }

    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = options->capture_stdout ? stdout_write : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = options->capture_stderr ? stderr_write : GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi = {0};
    char *env_block = build_windows_env_block(options->env);
    if (options->env && !env_block) {
        if (stdout_read) { CloseHandle(stdout_read); CloseHandle(stdout_write); }
        if (stderr_read) { CloseHandle(stderr_read); CloseHandle(stderr_write); }
        ReleaseSRWLockExclusive(&process_spawn_gate);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate environment block");
    }

    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, env_block,
                        options->working_dir, &si, &pi)) {
        free(env_block);
        if (stdout_read) { CloseHandle(stdout_read); CloseHandle(stdout_write); }
        if (stderr_read) { CloseHandle(stderr_read); CloseHandle(stderr_write); }
        ReleaseSRWLockExclusive(&process_spawn_gate);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create process");
    }
    free(env_block);

    CloseHandle(pi.hThread);
    if (stdout_write) CloseHandle(stdout_write);
    if (stderr_write) CloseHandle(stderr_write);
    ReleaseSRWLockExclusive(&process_spawn_gate);

    capture_buffer_t stdout_capture = {0};
    capture_buffer_t stderr_capture = {0};

    ULONGLONG started = GetTickCount64();
    int platform_error = 0;
    DWORD wait_result = WAIT_TIMEOUT;
    DWORD timeout = options->timeout_ms > 0 ? (DWORD)options->timeout_ms : INFINITE;

    while (1) {
        if (options->capture_stdout && drain_pipe(stdout_read, &stdout_capture) < 0) platform_error = 1;
        if (options->capture_stderr && drain_pipe(stderr_read, &stderr_capture) < 0) platform_error = 1;
        if (platform_error) { terminate_and_wait(pi.hProcess); break; }

        wait_result = WaitForSingleObject(pi.hProcess, 10);
        if (wait_result == WAIT_OBJECT_0) break;
        if (wait_result == WAIT_FAILED) { platform_error = 1; terminate_and_wait(pi.hProcess); break; }

        if (timeout != INFINITE) {
            if (GetTickCount64() - started >= timeout) { out_result->timed_out = 1; break; }
        }
    }

    if (out_result->timed_out) {
        if (terminate_and_wait(pi.hProcess) < 0) platform_error = 1;
    }

    if (options->capture_stdout && drain_after_exit(stdout_read, &stdout_capture, started, options->timeout_ms, &out_result->timed_out) < 0) platform_error = 1;
    if (options->capture_stderr && drain_after_exit(stderr_read, &stderr_capture, started, options->timeout_ms, &out_result->timed_out) < 0) platform_error = 1;

    DWORD exit_code = 0;
    if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
        out_result->exit_code = out_result->timed_out ? -1 : (int)exit_code;
    } else {
        out_result->exit_code = -1;
        platform_error = 1;
    }

    CloseHandle(pi.hProcess);
    if (stdout_read) CloseHandle(stdout_read);
    if (stderr_read) CloseHandle(stderr_read);

    out_result->stdout_text = stdout_capture.data ? stdout_capture.data : cc_copy_string("");
    out_result->stderr_text = stderr_capture.data ? stderr_capture.data : cc_copy_string("");

    if (stdout_capture.oom || stderr_capture.oom || !out_result->stdout_text || !out_result->stderr_text) {
        cc_process_result_free(out_result);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to capture process output");
    }
    if (platform_error) {
        cc_process_result_free(out_result);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to wait for or capture process output");
    }

    return cc_result_ok();
}




/** @brief 拥有直接进程、主线程及协议管道，成功FILE接管后原生读取HANDLE仅为借用。 */
struct cc_process_pipe {
    HANDLE hProcess; /**< stop拥有并关闭的进程句柄。 */
    HANDLE hThread; /**< stop拥有并关闭的主线程句柄。 */
    HANDLE hStdinWrite; /**< 尚未转给CRT的拥有写端，转交后为NULL。 */
    HANDLE hStdoutRead; /**< FILE存在时为借用，否则为尚未转交的拥有读端。 */
    FILE *to_child; /**< 接管CRT fd与stdin原生句柄的拥有流。 */
    FILE *from_child; /**< 接管stdout原生句柄的拥有流，实际读取由ReadFile完成。 */
};

/**
 * @brief 创建Windows行协议子进程，stdout和stderr沿既有行为合并到同一读取管道。
 * @param[in] command 必需同步借用ANSI可执行名，不隐式运行shell。
 * @param[in] argv 可选NULL终止借用数组，有argv[0]时追加argv[1..]，命令行首项始终引用command。
 * @param[out] out_pipe 必需空资源槽，基础参数有效后置NULL，成功交付唯一拥有对象供destroy。
 * @return 参数/命令行错误为INVALID_ARGUMENT，包装分配失败为OUT_OF_MEMORY，原生管道/继承属性/创建/CRT转换失败为PLATFORM。
 * @note 原生HANDLE依次转给CRT fd和FILE，任一步失败只由当前拥有者释放，并终止/等待已创建进程。
 */
cc_result_t cc_process_pipe_spawn(
    const char *command,
    char *const argv[],
    cc_process_pipe_t **out_pipe
)
{
    if (!command || !out_pipe)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pipe spawn arguments");
    *out_pipe = NULL;
    AcquireSRWLockExclusive(&process_spawn_gate);
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };

    HANDLE hChildStdinRead = NULL, hChildStdinWrite = NULL;
    HANDLE hChildStdoutRead = NULL, hChildStdoutWrite = NULL;

    if (!CreatePipe(&hChildStdinRead, &hChildStdinWrite, &sa, 0)) {
        ReleaseSRWLockExclusive(&process_spawn_gate);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create stdin pipe");
    }
    if (!CreatePipe(&hChildStdoutRead, &hChildStdoutWrite, &sa, 0)) {
        CloseHandle(hChildStdinRead); CloseHandle(hChildStdinWrite);
        ReleaseSRWLockExclusive(&process_spawn_gate);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create stdout pipe");
    }

    if (!SetHandleInformation(hChildStdinWrite, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(hChildStdoutRead, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(hChildStdinRead); CloseHandle(hChildStdinWrite);
        CloseHandle(hChildStdoutRead); CloseHandle(hChildStdoutWrite);
        ReleaseSRWLockExclusive(&process_spawn_gate);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to protect pipe handles");
    }

    char cmd_line[8192] = {0};
    size_t pos = 0;
    int cmd_ok = append_windows_quoted_arg(cmd_line, sizeof(cmd_line), &pos, command);
    if (argv && argv[0]) {
        for (int i = 1; cmd_ok && argv[i]; i++) {
            cmd_ok = append_windows_quoted_arg(cmd_line, sizeof(cmd_line), &pos, argv[i]);
        }
    }
    if (!cmd_ok) {
        CloseHandle(hChildStdinRead); CloseHandle(hChildStdinWrite);
        CloseHandle(hChildStdoutRead); CloseHandle(hChildStdoutWrite);
        ReleaseSRWLockExclusive(&process_spawn_gate);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Command line too long");
    }

    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hChildStdinRead;
    si.hStdOutput = hChildStdoutWrite;
    si.hStdError = hChildStdoutWrite;

    PROCESS_INFORMATION pi = {0};

    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hChildStdinRead); CloseHandle(hChildStdinWrite);
        CloseHandle(hChildStdoutRead); CloseHandle(hChildStdoutWrite);
        ReleaseSRWLockExclusive(&process_spawn_gate);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create pipe process");
    }

    CloseHandle(hChildStdinRead);
    CloseHandle(hChildStdoutWrite);
    ReleaseSRWLockExclusive(&process_spawn_gate);

    cc_process_pipe_t *p = calloc(1, sizeof(cc_process_pipe_t));
    if (!p) {
        CloseHandle(hChildStdinWrite);
        CloseHandle(hChildStdoutRead);
        terminate_and_wait(pi.hProcess);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate pipe process");
    }

    p->hProcess = pi.hProcess;
    p->hThread = pi.hThread;
    p->hStdinWrite = hChildStdinWrite;
    p->hStdoutRead = hChildStdoutRead;

    int fd_write = _open_osfhandle((intptr_t)hChildStdinWrite, _O_WRONLY);
    if (fd_write >= 0) {
        p->hStdinWrite = NULL;
        p->to_child = _fdopen(fd_write, "w");
        if (!p->to_child) _close(fd_write);
    }
    int fd_read = _open_osfhandle((intptr_t)hChildStdoutRead, _O_RDONLY);
    if (fd_read >= 0) {
        p->hStdoutRead = NULL;
        p->from_child = _fdopen(fd_read, "r");
        if (!p->from_child) _close(fd_read);
        else p->hStdoutRead = hChildStdoutRead;
    }

    if (!p->to_child || !p->from_child) {
        cc_process_pipe_destroy(p);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to fdopen pipe process handles");
    }

    setvbuf(p->to_child, NULL, _IONBF, 0);

    *out_pipe = p;
    return cc_result_ok();
}

/**
 * @brief 阻塞写正文并额外追加换行，立即刷新stdio流。
 * @param[in,out] pipe 必需存活且stdin开放的借用对象，同方向写入串行。
 * @param[in] data 必需NUL结尾借用正文，调用后不保留。
 * @return 空参数为INVALID_ARGUMENT，stdio失败为PLATFORM，其余为CC_OK。
 * @note CRT文本模式按Windows规则处理换行，没有超时/取消参数，失败可能已经送出前缀。
 */
cc_result_t cc_process_pipe_write(cc_process_pipe_t *pipe, const char *data)
{
    if (!pipe || !pipe->to_child || !data)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pipe write arguments");

    int written = fprintf(pipe->to_child, "%s\n", data);
    int flushed = fflush(pipe->to_child);

    if (written < 0 || flushed != 0 || ferror(pipe->to_child)) {
        clearerr(pipe->to_child);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to write to pipe process");
    }

    return cc_result_ok();
}

/**
 * @brief 用Peek/ReadFile读取一行，在有数据和无数据路径都检查实际预算与取消。
 * @param[in,out] pipe 必需存活且stdout开放的借用对象，同方向读取串行，stop前排空借用。
 * @param[in] timeout_ms 正毫秒预算，非正数沿三十秒默认。
 * @param[in] cancel_token 可选存活借用token，NULL表示不取消。
 * @param[out] out_line 必需空资源槽，基础参数有效后置NULL，成功交付free可释放的拥有行。
 * @return 成功为CC_OK，超时为TIMEOUT，取消为CANCELLED，分配失败为OUT_OF_MEMORY，空EOF或原生错误为PLATFORM。
 * @note 去换行和末尾CR，EOF前有前缀时交付末行，错误释放前缀且不回滚已消费字节，无数据轮询间隔约十毫秒。
 */
cc_result_t cc_process_pipe_read_line_timeout_cancel(
    cc_process_pipe_t *pipe,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    char **out_line
)
{
    if (!pipe || !out_line)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pipe read arguments");
    *out_line = NULL;
    if (!pipe->hStdoutRead)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Pipe stdout is closed");
    if (timeout_ms <= 0) timeout_ms = 30000;

    size_t cap = 256;
    size_t len = 0;
    char *line = malloc(cap);
    if (!line) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate pipe line");

    ULONGLONG started = GetTickCount64();
    while (1) {
        if (cc_cancel_token_is_cancelled(cancel_token)) {
            free(line);
            return cc_result_error(CC_ERR_CANCELLED, "Pipe process read cancelled");
        }
        if (GetTickCount64() - started >= (ULONGLONG)timeout_ms) {
            free(line);
            return cc_result_error(CC_ERR_TIMEOUT, "Timed out waiting for pipe process line");
        }

        DWORD available = 0;
        if (!PeekNamedPipe(pipe->hStdoutRead, NULL, 0, NULL, &available, NULL)) {
            if (GetLastError() == ERROR_BROKEN_PIPE && len > 0) break;
            free(line);
            return cc_result_error(CC_ERR_PLATFORM, "Pipe process stdout peek failed");
        }

        if (available == 0) {
            DWORD child_status = WaitForSingleObject(pipe->hProcess, 0);
            if (child_status == WAIT_OBJECT_0 && len > 0) break;
            if (child_status == WAIT_FAILED || child_status == WAIT_OBJECT_0) {
                free(line);
                return cc_result_error(CC_ERR_PLATFORM, "Pipe process closed stdout");
            }
            Sleep(10);
            continue;
        }

        char ch;
        DWORD nread = 0;
        BOOL read_ok = ReadFile(pipe->hStdoutRead, &ch, 1, &nread, NULL);
        if (!read_ok || nread == 0) {
            if (len > 0 && (read_ok || GetLastError() == ERROR_BROKEN_PIPE)) break;
            free(line);
            return cc_result_error(CC_ERR_PLATFORM, "Pipe process stdout read failed");
        }
        if (ch == '\n') break;
        if (len + 1 >= cap) {
            if (cap > SIZE_MAX / 2) {
                free(line);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Pipe line is too large");
            }
            cap *= 2;
            char *next = realloc(line, cap);
            if (!next) {
                free(line);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow pipe line");
            }
            line = next;
        }
        line[len++] = ch;
    }

    if (len > 0 && line[len - 1] == '\r') len--;
    line[len] = '\0';
    *out_line = line;
    return cc_result_ok();
}

/**
 * @brief 不带取消token的预算读行适配。
 * @param[in,out] pipe 存活借用协议对象。
 * @param[in] timeout_ms 毫秒预算，非正数沿三十秒默认。
 * @param[out] out_line 成功交付拥有行，失败清空规则同可取消入口。
 * @return 下游原样结果。
 */
cc_result_t cc_process_pipe_read_line_timeout(
    cc_process_pipe_t *pipe,
    int timeout_ms,
    char **out_line
)
{
    return cc_process_pipe_read_line_timeout_cancel(pipe, timeout_ms, NULL, out_line);
}

/**
 * @brief 使用三十秒默认预算读行。
 * @param[in,out] pipe 存活借用协议对象。
 * @param[out] out_line 成功交付free可释放的拥有行。
 * @return 下游原样结果。
 */
cc_result_t cc_process_pipe_read_line(cc_process_pipe_t *pipe, char **out_line)
{
    return cc_process_pipe_read_line_timeout(pipe, 30000, out_line);
}

/**
 * @brief 终止并等待直接进程，释放进程/线程与FILE或尚未转交的原生管道端。
 * @param[in,out] pipe 可选存活拥有对象，NULL安全，包装本身继续保留。
 * @pre 调用方已排空读写和其他stop借用。
 * @post 正常原生路径完成后句柄和流槽均清空，可重复stop，void接口未提供原生失败返回通道。
 */
void cc_process_pipe_stop(cc_process_pipe_t *pipe)
{
    if (!pipe) return;

    if (pipe->hProcess) {
        terminate_and_wait(pipe->hProcess);
        CloseHandle(pipe->hProcess);
        pipe->hProcess = NULL;
    }

    if (pipe->hThread) {
        CloseHandle(pipe->hThread);
        pipe->hThread = NULL;
    }

    if (pipe->to_child) {
        fclose(pipe->to_child);
        pipe->to_child = NULL;
    } else if (pipe->hStdinWrite) {
        CloseHandle(pipe->hStdinWrite);
    }
    pipe->hStdinWrite = NULL;
    if (pipe->from_child) {
        fclose(pipe->from_child);
        pipe->from_child = NULL;
    } else if (pipe->hStdoutRead) {
        CloseHandle(pipe->hStdoutRead);
    }
    pipe->hStdoutRead = NULL;
}

/**
 * @brief 停止并释放唯一拥有的协议进程对象。
 * @param[in,out] pipe 消费拥有对象，NULL安全，所有借用已排空。
 * @post 包装失效，内部资源按stop路径清理。
 */
void cc_process_pipe_destroy(cc_process_pipe_t *pipe)
{
    if (!pipe) return;
    cc_process_pipe_stop(pipe);
    free(pipe);
}

#endif
