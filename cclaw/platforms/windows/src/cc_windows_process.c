



#include "cc/app/cc_cancel_token.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_process.h"
#include "cc/internal/cc_alloc.h"

#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>

/* Windows 子进程输出捕获缓冲，成功后 data 转交给 cc_process_result_t。 */
typedef struct {
    char *data;
    size_t total;
    size_t cap;
} capture_buffer_t;

/* 追加 stdout/stderr 捕获数据，按 2 倍扩容并保持 NUL 结尾。 */
static int capture_append(capture_buffer_t *capture, const char *buf, size_t len)
{
    if (!capture || len == 0) return 1;
    if (capture->total + len + 1 > capture->cap) {
        size_t new_cap = capture->cap ? capture->cap : 4096;
        while (capture->total + len + 1 > new_cap) new_cap *= 2;
        char *new_data = realloc(capture->data, new_cap);
        if (!new_data) return 0;
        capture->data = new_data;
        capture->cap = new_cap;
    }
    memcpy(capture->data + capture->total, buf, len);
    capture->total += len;
    capture->data[capture->total] = '\0';
    return 1;
}

/*
 * 从 Windows pipe 中读取当前可用数据。
 *
 * 使用 PeekNamedPipe 避免阻塞；该 helper 用于轮询子进程输出。
 */
static void drain_pipe(HANDLE h, capture_buffer_t *capture)
{
    char buf[4096];
    DWORD available = 0;
    while (PeekNamedPipe(h, NULL, 0, NULL, &available, NULL) && available > 0) {
        DWORD nread = 0;
        if (ReadFile(h, buf, sizeof(buf) < available ? sizeof(buf) : available, &nread, NULL) && nread > 0) {
            if (!capture_append(capture, buf, nread)) return;
        } else {
            break;
        }
    }
}

/* 向命令行缓冲追加单个字符，带容量检查。 */
static int append_cmd_char(char *buf, size_t cap, size_t *pos, char ch)
{
    if (*pos + 1 >= cap) return 0;
    buf[(*pos)++] = ch;
    buf[*pos] = '\0';
    return 1;
}

/* 在命令行参数之间追加空格；首个参数前不加。 */
static int append_cmd_space(char *buf, size_t cap, size_t *pos)
{
    if (*pos == 0) return 1;
    return append_cmd_char(buf, cap, pos, ' ');
}

/*
 * 按 Windows CreateProcess 规则引用一个参数。
 *
 * 处理反斜杠和双引号转义，避免参数中空格或引号破坏命令行边界。
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

/*
 * 构造 Windows 环境块。
 *
 * CreateProcess 需要 `key=value\0key=value\0\0` 格式；返回块由调用方 free。
 */
static char *build_windows_env_block(char **env)
{
    if (!env) return NULL;

    size_t total = 1;
    for (char **e = env; *e; e++) total += strlen(*e) + 1;

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

/* 释放进程结果里的 stdout/stderr 文本并清零结构。 */
void cc_process_result_free(cc_process_result_t *result)
{
    if (!result) return;
    free(result->stdout_text);
    free(result->stderr_text);
    memset(result, 0, sizeof(cc_process_result_t));
}

/*
 * 执行一次性 Windows 子进程。
 *
 * 使用 CreateProcessA，按配置捕获 stdout/stderr，timeout 后 TerminateProcess。函数返回
 * OK 表示平台执行流程完成，命令退出码放在 out_result->exit_code。
 */
cc_result_t cc_process_run(
    const cc_process_options_t *options,
    cc_process_result_t *out_result
)
{
    memset(out_result, 0, sizeof(cc_process_result_t));

    HANDLE stdout_read = NULL, stdout_write = NULL;
    HANDLE stderr_read = NULL, stderr_write = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };

    if (options->capture_stdout) {
        if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0))
            return cc_result_error(CC_ERR_PLATFORM, "Failed to create stdout pipe");
        SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    }

    if (options->capture_stderr) {
        if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
            if (stdout_read) { CloseHandle(stdout_read); CloseHandle(stdout_write); }
            return cc_result_error(CC_ERR_PLATFORM, "Failed to create stderr pipe");
        }
        SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
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
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate environment block");
    }

    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, env_block,
                        options->working_dir, &si, &pi)) {
        free(env_block);
        if (stdout_read) { CloseHandle(stdout_read); CloseHandle(stdout_write); }
        if (stderr_read) { CloseHandle(stderr_read); CloseHandle(stderr_write); }
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create process");
    }
    free(env_block);

    CloseHandle(pi.hThread);
    if (stdout_write) CloseHandle(stdout_write);
    if (stderr_write) CloseHandle(stderr_write);

    capture_buffer_t stdout_capture = {0};
    capture_buffer_t stderr_capture = {0};

    DWORD elapsed = 0;
    DWORD wait_result = WAIT_TIMEOUT;
    DWORD timeout = options->timeout_ms > 0 ? (DWORD)options->timeout_ms : INFINITE;

    while (1) {
        if (options->capture_stdout) drain_pipe(stdout_read, &stdout_capture);
        if (options->capture_stderr) drain_pipe(stderr_read, &stderr_capture);

        wait_result = WaitForSingleObject(pi.hProcess, 10);
        if (wait_result == WAIT_OBJECT_0) break;
        if (wait_result == WAIT_FAILED) break;

        if (timeout != INFINITE) {
            elapsed += 10;
            if (elapsed >= timeout) break;
        }
    }

    out_result->timed_out = (timeout != INFINITE && elapsed >= timeout && wait_result != WAIT_OBJECT_0) ? 1 : 0;
    if (out_result->timed_out) {
        TerminateProcess(pi.hProcess, (UINT)-1);
        WaitForSingleObject(pi.hProcess, 5000);
    }

    if (options->capture_stdout) drain_pipe(stdout_read, &stdout_capture);
    if (options->capture_stderr) drain_pipe(stderr_read, &stderr_capture);

    DWORD exit_code = 0;
    if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
        out_result->exit_code = out_result->timed_out ? -1 : (int)exit_code;
    } else {
        out_result->exit_code = -1;
    }

    CloseHandle(pi.hProcess);
    if (stdout_read) CloseHandle(stdout_read);
    if (stderr_read) CloseHandle(stderr_read);

    out_result->stdout_text = stdout_capture.data ? stdout_capture.data : cc_copy_string("");
    out_result->stderr_text = stderr_capture.data ? stderr_capture.data : cc_copy_string("");

    return cc_result_ok();
}




/*
 * Windows 管道进程对象。
 *
 * 保存进程/线程 HANDLE、stdin/stdout pipe HANDLE，以及用 C stdio 包装后的读写流。
 */
struct cc_process_pipe {
    HANDLE hProcess;
    HANDLE hThread;
    HANDLE hStdinWrite;
    HANDLE hStdoutRead;
    FILE *to_child;
    FILE *from_child;
};

/*
 * 启动长期管道进程。
 *
 * stdin/stdout 通过可继承 pipe 连接到子进程；父进程侧转换成 FILE*，用于 JSONL/RPC 等
 * 行协议通信。
 */
cc_result_t cc_process_pipe_spawn(
    const char *command,
    char *const argv[],
    cc_process_pipe_t **out_pipe
)
{
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };

    HANDLE hChildStdinRead = NULL, hChildStdinWrite = NULL;
    HANDLE hChildStdoutRead = NULL, hChildStdoutWrite = NULL;

    if (!CreatePipe(&hChildStdinRead, &hChildStdinWrite, &sa, 0))
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create stdin pipe");
    if (!CreatePipe(&hChildStdoutRead, &hChildStdoutWrite, &sa, 0)) {
        CloseHandle(hChildStdinRead); CloseHandle(hChildStdinWrite);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create stdout pipe");
    }

    SetHandleInformation(hChildStdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hChildStdoutRead, HANDLE_FLAG_INHERIT, 0);

    char cmd_line[8192] = {0};
    size_t pos = 0;
    int cmd_ok = append_windows_quoted_arg(cmd_line, sizeof(cmd_line), &pos, command);
    if (argv) {
        for (int i = 1; cmd_ok && argv[i]; i++) {
            cmd_ok = append_windows_quoted_arg(cmd_line, sizeof(cmd_line), &pos, argv[i]);
        }
    }
    if (!cmd_ok) {
        CloseHandle(hChildStdinRead); CloseHandle(hChildStdinWrite);
        CloseHandle(hChildStdoutRead); CloseHandle(hChildStdoutWrite);
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
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create pipe process");
    }

    CloseHandle(hChildStdinRead);
    CloseHandle(hChildStdoutWrite);

    cc_process_pipe_t *p = calloc(1, sizeof(cc_process_pipe_t));
    if (!p) {
        CloseHandle(hChildStdinWrite);
        CloseHandle(hChildStdoutRead);
        TerminateProcess(pi.hProcess, (UINT)-1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate pipe process");
    }

    p->hProcess = pi.hProcess;
    p->hThread = pi.hThread;
    p->hStdinWrite = hChildStdinWrite;
    p->hStdoutRead = hChildStdoutRead;

    int fd_write = _open_osfhandle((intptr_t)hChildStdinWrite, 0);
    int fd_read = _open_osfhandle((intptr_t)hChildStdoutRead, 0);

    p->to_child = (fd_write >= 0) ? _fdopen(fd_write, "w") : NULL;
    p->from_child = (fd_read >= 0) ? _fdopen(fd_read, "r") : NULL;

    if (!p->to_child || !p->from_child) {
        if (p->to_child) fclose(p->to_child);
        if (p->from_child) fclose(p->from_child);
        TerminateProcess(pi.hProcess, (UINT)-1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        free(p);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to fdopen pipe process handles");
    }

    setvbuf(p->to_child, NULL, _IONBF, 0);

    *out_pipe = p;
    return cc_result_ok();
}

/* 向管道进程写入一行并刷新；data 后自动追加换行。 */
cc_result_t cc_process_pipe_write(cc_process_pipe_t *pipe, const char *data)
{
    if (!pipe || !pipe->to_child || !data)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pipe write arguments");

    fprintf(pipe->to_child, "%s\n", data);
    fflush(pipe->to_child);

    if (ferror(pipe->to_child)) {
        clearerr(pipe->to_child);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to write to pipe process");
    }

    return cc_result_ok();
}

/*
 * 带 timeout 和 cancel token 读取一行。
 *
 * 通过 PeekNamedPipe 轮询 stdout，每 10ms 检查进程状态和取消状态；返回的 out_line 由
 * 调用方 free。
 */
cc_result_t cc_process_pipe_read_line_timeout_cancel(
    cc_process_pipe_t *pipe,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    char **out_line
)
{
    if (!pipe || !pipe->hStdoutRead || !out_line)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pipe read arguments");
    if (timeout_ms <= 0) timeout_ms = 30000;

    size_t cap = 256;
    size_t len = 0;
    char *line = malloc(cap);
    if (!line) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate pipe line");

    DWORD elapsed = 0;
    while (1) {
        if (cc_cancel_token_is_cancelled(cancel_token)) {
            free(line);
            return cc_result_error(CC_ERR_CANCELLED, "Pipe process read cancelled");
        }

        DWORD available = 0;
        if (!PeekNamedPipe(pipe->hStdoutRead, NULL, 0, NULL, &available, NULL)) {
            free(line);
            return cc_result_error(CC_ERR_PLATFORM, "Pipe process stdout peek failed");
        }

        if (available == 0) {
            DWORD child_status = WaitForSingleObject(pipe->hProcess, 0);
            if (child_status == WAIT_OBJECT_0 && len == 0) {
                free(line);
                return cc_result_error(CC_ERR_PLATFORM, "Pipe process closed stdout");
            }
            if (elapsed >= (DWORD)timeout_ms) {
                free(line);
                return cc_result_error(CC_ERR_TIMEOUT, "Timed out waiting for pipe process line");
            }
            Sleep(10);
            elapsed += 10;
            continue;
        }

        char ch;
        DWORD nread = 0;
        if (!ReadFile(pipe->hStdoutRead, &ch, 1, &nread, NULL) || nread == 0) {
            free(line);
            return cc_result_error(CC_ERR_PLATFORM, "Pipe process stdout read failed");
        }
        if (ch == '\n') break;
        if (len + 1 >= cap) {
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

/* 不带 cancel token 的 timeout 读行 wrapper。 */
cc_result_t cc_process_pipe_read_line_timeout(
    cc_process_pipe_t *pipe,
    int timeout_ms,
    char **out_line
)
{
    return cc_process_pipe_read_line_timeout_cancel(pipe, timeout_ms, NULL, out_line);
}

/* 默认 30 秒 timeout 的读行 wrapper。 */
cc_result_t cc_process_pipe_read_line(cc_process_pipe_t *pipe, char **out_line)
{
    return cc_process_pipe_read_line_timeout(pipe, 30000, out_line);
}

/*
 * 停止管道进程并关闭资源。
 *
 * Windows 版使用 TerminateProcess 兜底，然后关闭 process/thread HANDLE 和 FILE*。
 */
void cc_process_pipe_stop(cc_process_pipe_t *pipe)
{
    if (!pipe) return;

    if (pipe->hProcess) {
        TerminateProcess(pipe->hProcess, (UINT)-1);
        WaitForSingleObject(pipe->hProcess, 5000);
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
    }
    if (pipe->from_child) {
        fclose(pipe->from_child);
        pipe->from_child = NULL;
    }
}

/* 停止并释放管道进程对象。 */
void cc_process_pipe_destroy(cc_process_pipe_t *pipe)
{
    if (!pipe) return;
    cc_process_pipe_stop(pipe);
    free(pipe);
}

#endif
