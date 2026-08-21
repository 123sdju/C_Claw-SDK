#include "cc/app/cc_cancel_token.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_process.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * 子进程输出捕获缓冲。
 *
 * stdout/stderr 通过非阻塞 pipe 分块读入这里；成功后 data 交给 out_result，由调用方
 * 使用 cc_process_result_free 释放。
 */
typedef struct {
    char *data;
    size_t total;
    size_t cap;
    int oom;
} capture_buffer_t;

static void close_fd_if_open(int *fd)
{
    if (fd && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void close_pipe_pair(int pipefd[2])
{
    close_fd_if_open(&pipefd[0]);
    close_fd_if_open(&pipefd[1]);
}

static void sleep_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000 * 1000;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* 设置非阻塞，避免读 pipe 时因为子进程尚未输出而卡住 runtime。 */
static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

static int wait_for_pid(pid_t pid, int *status, int options)
{
    pid_t rc;
    do {
        rc = waitpid(pid, status, options);
    } while (rc < 0 && errno == EINTR);
    return rc == pid ? 0 : -1;
}

static int capture_append(capture_buffer_t *capture, const char *buf, size_t len)
{
    if (!capture || len == 0) return 1;
    if (capture->oom) return 0;

    if (len > (size_t)-1 - capture->total - 1) {
        capture->oom = 1;
        return 0;
    }

    if (capture->total + len + 1 > capture->cap) {
        size_t new_cap = capture->cap ? capture->cap : 4096;
        while (capture->total + len + 1 > new_cap) {
            if (new_cap > (size_t)-1 / 2) {
                capture->oom = 1;
                return 0;
            }
            new_cap *= 2;
        }

        char *new_data = realloc(capture->data, new_cap);
        if (!new_data) {
            capture->oom = 1;
            return 0;
        }

        capture->data = new_data;
        capture->cap = new_cap;
    }

    memcpy(capture->data + capture->total, buf, len);
    capture->total += len;
    capture->data[capture->total] = '\0';
    return 1;
}

/*
 * 尽可能读完一个非阻塞 fd 中当前可用的数据。
 *
 * EAGAIN/EWOULDBLOCK 表示暂时没有更多数据，不是错误。内存分配失败会记录到 capture，
 * 外层统一返回 OOM。
 */
static void drain_fd(int fd, capture_buffer_t *capture)
{
    char buf[4096];
    while (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (!capture_append(capture, buf, (size_t)n)) return;
            continue;
        }
        if (n == 0) return;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        return;
    }
}

void cc_process_result_free(cc_process_result_t *result)
{
    if (!result) return;
    free(result->stdout_text);
    free(result->stderr_text);
    memset(result, 0, sizeof(*result));
}

/*
 * 执行一次性子进程命令。
 *
 * 平台调用成功表示 fork/等待流程完成；命令业务失败通过 out_result->exit_code 表达。
 * timeout 会先 SIGKILL 子进程，再回收 fd 和捕获缓冲。
 */
cc_result_t cc_process_run(
    const cc_process_options_t *options,
    cc_process_result_t *out_result
)
{
    if (!options || !options->command || !out_result) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid process run arguments");
    }
    memset(out_result, 0, sizeof(*out_result));

    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};

    if (options->capture_stdout && pipe(stdout_pipe) != 0) {
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create stdout pipe");
    }

    if (options->capture_stderr && pipe(stderr_pipe) != 0) {
        close_pipe_pair(stdout_pipe);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create stderr pipe");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close_pipe_pair(stdout_pipe);
        close_pipe_pair(stderr_pipe);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to fork");
    }

    if (pid == 0) {
        if (options->capture_stdout) {
            close_fd_if_open(&stdout_pipe[0]);
            if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0) _exit(127);
            close_fd_if_open(&stdout_pipe[1]);
        }
        if (options->capture_stderr) {
            close_fd_if_open(&stderr_pipe[0]);
            if (dup2(stderr_pipe[1], STDERR_FILENO) < 0) _exit(127);
            close_fd_if_open(&stderr_pipe[1]);
        }

        if (options->working_dir && chdir(options->working_dir) != 0) {
            _exit(127);
        }

        if (options->env) {
            for (char **e = options->env; *e; e++) {
                char *eq = strchr(*e, '=');
                if (eq) {
                    *eq = '\0';
                    setenv(*e, eq + 1, 1);
                    *eq = '=';
                }
            }
        }

        if (options->args && options->args[0]) {
            execvp(options->command, options->args);
        } else {
            execlp("/bin/sh", "sh", "-c", options->command, NULL);
        }
        _exit(127);
    }

    if (options->capture_stdout) {
        close_fd_if_open(&stdout_pipe[1]);
        set_nonblocking(stdout_pipe[0]);
    }
    if (options->capture_stderr) {
        close_fd_if_open(&stderr_pipe[1]);
        set_nonblocking(stderr_pipe[0]);
    }

    capture_buffer_t stdout_capture = {0};
    capture_buffer_t stderr_capture = {0};
    int timed_out = 0;
    int status = 0;
    int waited = 0;
    int child_done = 0;

    while (!child_done) {
        if (options->capture_stdout) drain_fd(stdout_pipe[0], &stdout_capture);
        if (options->capture_stderr) drain_fd(stderr_pipe[0], &stderr_capture);

        pid_t wait_rc = waitpid(pid, &status, WNOHANG);
        if (wait_rc == pid) {
            if (WIFEXITED(status)) {
                out_result->exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                out_result->exit_code = -WTERMSIG(status);
            } else {
                out_result->exit_code = -1;
            }
            child_done = 1;
            break;
        }

        if (wait_rc < 0 && errno != EINTR) {
            out_result->exit_code = -1;
            child_done = 1;
            break;
        }

        if (options->timeout_ms > 0 && waited >= options->timeout_ms) {
            timed_out = 1;
            kill(pid, SIGKILL);
            wait_for_pid(pid, &status, 0);
            out_result->exit_code = -1;
            child_done = 1;
            break;
        }

        sleep_ms(10);
        waited += 10;
    }

    out_result->timed_out = timed_out;

    if (options->capture_stdout) {
        drain_fd(stdout_pipe[0], &stdout_capture);
        close_fd_if_open(&stdout_pipe[0]);
        out_result->stdout_text = stdout_capture.data ? stdout_capture.data : cc_copy_string("");
        stdout_capture.data = NULL;
    }
    if (options->capture_stderr) {
        drain_fd(stderr_pipe[0], &stderr_capture);
        close_fd_if_open(&stderr_pipe[0]);
        out_result->stderr_text = stderr_capture.data ? stderr_capture.data : cc_copy_string("");
        stderr_capture.data = NULL;
    }

    if (stdout_capture.oom || stderr_capture.oom ||
        (options->capture_stdout && !out_result->stdout_text) ||
        (options->capture_stderr && !out_result->stderr_text)) {
        free(stdout_capture.data);
        free(stderr_capture.data);
        cc_process_result_free(out_result);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to capture process output");
    }

    return cc_result_ok();
}

/*
 * 长生命周期管道进程状态。
 *
 * stdin/stdout 使用 FILE* 便于按行写读；stderr 当前只保留 fd 供后续扩展。
 */
struct cc_process_pipe {
    pid_t pid;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
    FILE *to_child;
    FILE *from_child;
};

static void close_process_pipe_handles(cc_process_pipe_t *pipe)
{
    if (!pipe) return;
    if (pipe->to_child) {
        fclose(pipe->to_child);
        pipe->to_child = NULL;
        pipe->stdin_fd = -1;
    } else {
        close_fd_if_open(&pipe->stdin_fd);
    }
    if (pipe->from_child) {
        fclose(pipe->from_child);
        pipe->from_child = NULL;
        pipe->stdout_fd = -1;
    } else {
        close_fd_if_open(&pipe->stdout_fd);
    }
    close_fd_if_open(&pipe->stderr_fd);
}

/*
 * 启动可交互的管道进程。
 *
 * 父进程保留子进程 stdin/stdout/stderr 的管道端；任一 fdopen 失败都会关闭所有 fd、
 * 杀死并回收子进程，避免遗留孤儿进程。
 */
cc_result_t cc_process_pipe_spawn(
    const char *command,
    char *const argv[],
    cc_process_pipe_t **out_pipe
)
{
    if (!command || !out_pipe) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pipe spawn arguments");
    }
    *out_pipe = NULL;

    signal(SIGPIPE, SIG_IGN);

    int to_child[2] = {-1, -1};
    int from_child[2] = {-1, -1};
    int err_child[2] = {-1, -1};

    if (pipe(to_child) < 0 || pipe(from_child) < 0 || pipe(err_child) < 0) {
        close_pipe_pair(to_child);
        close_pipe_pair(from_child);
        close_pipe_pair(err_child);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create pipes for pipe process");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close_pipe_pair(to_child);
        close_pipe_pair(from_child);
        close_pipe_pair(err_child);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to fork pipe process");
    }

    if (pid == 0) {
        if (dup2(to_child[0], STDIN_FILENO) < 0) _exit(127);
        if (dup2(from_child[1], STDOUT_FILENO) < 0) _exit(127);
        if (dup2(err_child[1], STDERR_FILENO) < 0) _exit(127);
        close_pipe_pair(to_child);
        close_pipe_pair(from_child);
        close_pipe_pair(err_child);
        if (argv && argv[0]) {
            execvp(command, argv);
        } else {
            execlp(command, command, NULL);
        }
        _exit(127);
    }

    close_fd_if_open(&to_child[0]);
    close_fd_if_open(&from_child[1]);
    close_fd_if_open(&err_child[1]);

    cc_process_pipe_t *p = calloc(1, sizeof(*p));
    if (!p) {
        close_pipe_pair(to_child);
        close_pipe_pair(from_child);
        close_pipe_pair(err_child);
        kill(pid, SIGKILL);
        wait_for_pid(pid, NULL, 0);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate pipe process");
    }

    p->pid = pid;
    p->stdin_fd = to_child[1];
    p->stdout_fd = from_child[0];
    p->stderr_fd = err_child[0];
    set_nonblocking(p->stdout_fd);

    p->to_child = fdopen(p->stdin_fd, "w");
    if (p->to_child) p->stdin_fd = -1;
    p->from_child = fdopen(p->stdout_fd, "r");
    if (p->from_child) p->stdout_fd = -1;

    if (!p->to_child || !p->from_child) {
        close_process_pipe_handles(p);
        kill(pid, SIGKILL);
        wait_for_pid(pid, NULL, 0);
        free(p);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to fdopen pipe process pipes");
    }

    setvbuf(p->to_child, NULL, _IONBF, 0);

    *out_pipe = p;
    return cc_result_ok();
}

/*
 * 向管道进程写入一行。
 *
 * data 后自动追加换行并刷新，匹配 JSONL/RPC line protocol。
 */
cc_result_t cc_process_pipe_write(cc_process_pipe_t *pipe, const char *data)
{
    if (!pipe || !pipe->to_child || !data) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pipe write arguments");
    }

    if (fprintf(pipe->to_child, "%s\n", data) < 0 || fflush(pipe->to_child) != 0 ||
        ferror(pipe->to_child)) {
        clearerr(pipe->to_child);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to write to pipe process");
    }

    return cc_result_ok();
}

static int pipe_stdout_fd(cc_process_pipe_t *pipe)
{
    if (!pipe) return -1;
    if (pipe->stdout_fd >= 0) return pipe->stdout_fd;
    return pipe->from_child ? fileno(pipe->from_child) : -1;
}

/*
 * 带 timeout 和取消 token 读取一行 stdout。
 *
 * select 每 50ms 最多等待一次，以便及时检查 cancel_token。EOF 且已有部分数据时返回
 * 该行；完全 EOF 返回平台错误。
 */
cc_result_t cc_process_pipe_read_line_timeout_cancel(
    cc_process_pipe_t *pipe,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    char **out_line
)
{
    if (!pipe || !out_line) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pipe read arguments");
    }
    *out_line = NULL;

    int fd = pipe_stdout_fd(pipe);
    if (fd < 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Pipe stdout is closed");
    }
    if (timeout_ms <= 0) timeout_ms = 30000;

    size_t cap = 256;
    size_t len = 0;
    char *line = malloc(cap);
    if (!line) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate pipe line");

    int waited_ms = 0;
    while (1) {
        if (cc_cancel_token_is_cancelled(cancel_token)) {
            free(line);
            return cc_result_error(CC_ERR_CANCELLED, "Pipe process read cancelled");
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);

        int wait_ms = timeout_ms - waited_ms;
        if (wait_ms > 50) wait_ms = 50;
        if (wait_ms <= 0) {
            free(line);
            return cc_result_error(CC_ERR_TIMEOUT, "Timed out waiting for pipe process line");
        }

        struct timeval tv;
        tv.tv_sec = wait_ms / 1000;
        tv.tv_usec = (wait_ms % 1000) * 1000;

        int ready = select(fd + 1, &readfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            free(line);
            return cc_result_error(CC_ERR_PLATFORM, "Pipe process stdout select failed");
        }
        if (ready == 0) {
            waited_ms += wait_ms;
            continue;
        }

        char ch;
        ssize_t nread = read(fd, &ch, 1);
        if (nread < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            free(line);
            return cc_result_error(CC_ERR_PLATFORM, "Pipe process stdout read failed");
        }
        if (nread == 0) {
            if (len == 0) {
                free(line);
                return cc_result_error(CC_ERR_PLATFORM, "Pipe process closed stdout");
            }
            break;
        }
        if (ch == '\n') break;

        if (len + 1 >= cap) {
            if (cap > (size_t)-1 / 2) {
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

    if (len > 0 && line[len - 1] == '\r') {
        len--;
    }
    line[len] = '\0';
    *out_line = line;
    return cc_result_ok();
}

cc_result_t cc_process_pipe_read_line_timeout(
    cc_process_pipe_t *pipe,
    int timeout_ms,
    char **out_line
)
{
    return cc_process_pipe_read_line_timeout_cancel(pipe, timeout_ms, NULL, out_line);
}

cc_result_t cc_process_pipe_read_line(cc_process_pipe_t *pipe, char **out_line)
{
    return cc_process_pipe_read_line_timeout(pipe, 30000, out_line);
}

/*
 * 停止管道进程并关闭管道端。
 *
 * 先给 SIGTERM 一点退出机会，再用 SIGKILL 兜底；随后关闭 FILE 指针和 fd。函数可重复调用。
 */
void cc_process_pipe_stop(cc_process_pipe_t *pipe)
{
    if (!pipe) return;

    if (pipe->pid > 0) {
        int status = 0;
        if (waitpid(pipe->pid, &status, WNOHANG) == 0) {
            kill(pipe->pid, SIGTERM);
            sleep_ms(100);
            if (waitpid(pipe->pid, &status, WNOHANG) == 0) {
                kill(pipe->pid, SIGKILL);
            }
        }
        wait_for_pid(pipe->pid, &status, 0);
        pipe->pid = 0;
    }

    close_process_pipe_handles(pipe);
}

void cc_process_pipe_destroy(cc_process_pipe_t *pipe)
{
    if (!pipe) return;
    cc_process_pipe_stop(pipe);
    free(pipe);
}
