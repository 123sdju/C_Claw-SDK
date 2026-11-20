#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cc/app/cc_cancel_token.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_process.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* 同一端口的创建窗口串行，覆盖无原子CLOEXEC平台的pipe/fcntl间隙，运行与通信不持此锁。 */
static pthread_mutex_t process_spawn_gate = PTHREAD_MUTEX_INITIALIZER;

/** @brief 单条输出流拥有的增长缓冲，oom为本次捕获不可恢复的粘滞错误。 */
typedef struct {
    char *data; /**< 拥有的NUL结尾字节缓冲，成功转交结果。 */
    size_t total; /**< 已存正文字节数，不含NUL。 */
    size_t cap; /**< 分配容量，单位字节，包含NUL空间。 */
    int oom; /**< 分配或容量溢出失败后置一。 */
} capture_buffer_t;

/**
 * @brief 关闭单个拥有描述符并使保存槽失效。
 * @param[in,out] fd 可选描述符槽，非负值关闭后置-1。
 * @post NULL或负值跳过，close错误无返回通道，不重试可能已复用的编号。
 */
static void close_fd_if_open(int *fd)
{
    if (fd && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/** @brief 关闭本次拥有的两个管道端。 @param[in,out] pipefd 必需双槽，分别关闭并置-1。 @post 双槽均失效。 */
static void close_pipe_pair(int pipefd[2])
{
    close_fd_if_open(&pipefd[0]);
    close_fd_if_open(&pipefd[1]);
}

/**
 * @brief 创建两个高于标准流编号且 close-on-exec 的管道端。
 * @param[out] pipefd 必需双槽，进入先置 -1，失败关闭本次所有描述符。
 * @return 全部属性设置成功为零，任一系统调用失败为负一。
 */
static int create_process_pipe(int pipefd[2])
{
    pipefd[0] = pipefd[1] = -1;
#if defined(__linux__)
    if (pipe2(pipefd, O_CLOEXEC) < 0) return -1;
#else
    if (pipe(pipefd) < 0) return -1;
#endif
    for (int i = 0; i < 2; i++) {
        if (pipefd[i] < 3) {
#ifdef F_DUPFD_CLOEXEC
            int copy = fcntl(pipefd[i], F_DUPFD_CLOEXEC, 3);
#else
            int copy = fcntl(pipefd[i], F_DUPFD, 3);
#endif
            if (copy < 0) { close_pipe_pair(pipefd); return -1; }
            close(pipefd[i]);
            pipefd[i] = copy;
        }
        if (fcntl(pipefd[i], F_SETFD, FD_CLOEXEC) < 0) {
            close_pipe_pair(pipefd);
            return -1;
        }
    }
    return 0;
}

/**
 * @brief 以nanosleep等待轮询间隔，信号打断后继续剩余时间。
 * @param[in] ms 非负毫秒数，当前调用为十或一百毫秒。
 * @post 正常等待完成，其他原生错误结束等待，无状态返回。
 */
static void sleep_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000 * 1000;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/** @brief 在借用描述符上启用非阻塞读取。 @param[in] fd 存活描述符。 @return 取得和设置flags均成功为一，否则为零。 */
static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/**
 * @brief 读取单调时钟毫秒，供实际经过时间预算使用。
 * @param[out] out_ms 必需输出槽，只有成功才写入。
 * @return 时钟读取成功为一，失败为零。
 */
static int process_clock_ms(uint64_t *out_ms)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    *out_ms = (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
    return 1;
}

extern char **environ;

/**
 * @brief 释放父进程准备的独立环境数组。
 * @param[in,out] env 消费拥有且以 NULL 终止的字符串数组，NULL 安全。
 * @post 字符串和数组均释放，不修改进程环境。
 */
static void free_process_environment(char **env)
{
    if (!env) return;
    for (size_t i = 0; env[i]; i++) free(env[i]);
    free(env);
}

/**
 * @brief 在 fork 前深拷贝继承环境并合并有效覆盖项，避免子进程写输入或调用 setenv。
 * @param[in] overrides 必需 NULL 终止借用数组，KEY=VALUE 生效，空键或无等号项沿旧行为忽略。
 * @return 拥有环境数组供父进程释放，任何分配失败清理部分结果并返回 NULL。
 * @pre 调用期间进程环境与输入数组保持稳定。
 */
static char **build_process_environment(char **overrides)
{
    size_t inherited = 0, extra = 0;
    while (environ && environ[inherited]) inherited++;
    while (overrides[extra]) extra++;
    if (extra > SIZE_MAX - inherited - 1 || inherited + extra + 1 > SIZE_MAX / sizeof(char *)) return NULL;
    char **env = calloc(inherited + extra + 1, sizeof(*env));
    if (!env) return NULL;
    size_t count = 0;
    for (; count < inherited; count++) {
        env[count] = cc_copy_string(environ[count]);
        if (!env[count]) { free_process_environment(env); return NULL; }
    }
    for (size_t i = 0; i < extra; i++) {
        const char *equal = strchr(overrides[i], '=');
        if (!equal || equal == overrides[i]) continue;
        size_t key_length = (size_t)(equal - overrides[i]);
        size_t slot = 0;
        while (slot < count && (strncmp(env[slot], overrides[i], key_length) != 0 || env[slot][key_length] != '=')) slot++;
        char *copy = cc_copy_string(overrides[i]);
        if (!copy) { free_process_environment(env); return NULL; }
        free(env[slot]);
        env[slot] = copy;
        if (slot == count) count++;
    }
    return env;
}

/**
 * @brief 等待指定直接子进程，EINTR时重试。
 * @param[in] pid 必需正子进程ID，不等待其他子进程。
 * @param[out] status 可选原生wait状态槽，只有取得该子进程状态时有效。
 * @param[in] options waitpid选项，当前清理调用使用零进行阻塞回收。
 * @return 回收目标子进程为零，其余结果为负一。
 */
static int wait_for_pid(pid_t pid, int *status, int options)
{
    pid_t rc;
    do {
        rc = waitpid(pid, status, options);
    } while (rc < 0 && errno == EINTR);
    return rc == pid ? 0 : -1;
}

/**
 * @brief 扩容并追加捕获字节，维护NUL结尾和粘滞OOM状态。
 * @param[in,out] capture 可选拥有缓冲状态，NULL视为无需保存。
 * @param[in] buf len非零时借用至少len字节的输入。
 * @param[in] len 正文字节数，可含内部NUL，不从输入计算长度。
 * @return 追加或空操作为一，溢出/分配失败为零且保留旧缓冲供清理。
 */
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

/**
 * @brief 每轮最多读取十六个4096字节块，给另一输出流和预算检查执行机会。
 * @param[in] fd 非阻塞借用描述符。
 * @param[in,out] capture 拥有捕获状态，追加失败保留已分配部分。
 * @return EOF/暂时无数据为零，达到本轮块数为一，原生读取或分配失败为负一。
 */
static int drain_fd(int fd, capture_buffer_t *capture)
{
    char buf[4096];
    for (int chunks = 0; fd >= 0 && chunks < 16; chunks++) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (!capture_append(capture, buf, (size_t)n)) return -1;
            continue;
        }
        if (n == 0) return 0;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    return 1;
}

/**
 * @brief 子进程退出后继续分轮捕获余量，有限预算覆盖持续持管道的后代输出。
 * @param[in] fd 必需非阻塞读取描述符。
 * @param[in,out] capture 拥有捕获状态，追加失败保留已分配缓冲供清理。
 * @param[in] started_ms 本次单调时钟开始值。
 * @param[in] timeout_ms 正数为总毫秒预算，非正数不限时。
 * @param[in,out] timed_out 先前超时状态，达到有限总预算时置一。
 * @return 当前余量读完或预算耗尽为零，读取、时钟或分配失败为负一。
 */
static int drain_after_exit(int fd, capture_buffer_t *capture, uint64_t started_ms,
    int timeout_ms, int *timed_out)
{
    int more;
    do {
        more = drain_fd(fd, capture);
        if (more <= 0 || *timed_out) break;
        if (timeout_ms > 0) {
            uint64_t now_ms;
            if (!process_clock_ms(&now_ms)) return -1;
            if (now_ms - started_ms >= (uint64_t)timeout_ms) { *timed_out = 1; break; }
        }
    } while (more > 0);
    return more < 0 ? -1 : 0;
}

/**
 * @brief 释放结果拥有的两条输出并清零结果结构。
 * @param[in,out] result 可选拥有结果，NULL安全，不释放结构本身。
 * @post 字符串失效，非NULL结构可重复清理或再次用作空输出。
 */
void cc_process_result_free(cc_process_result_t *result)
{
    if (!result) return;
    free(result->stdout_text);
    free(result->stderr_text);
    memset(result, 0, sizeof(*result));
}

/**
 * @brief fork运行命令，分轮排空stdout/stderr并回收直接子进程。
 * @param[in] options 必需稳定借用选项，command必填，有args[0]时execvp，否则用/bin/sh -c。
 * @param[out] out_result 必需无已有资源输出，基础参数有效后清零，成功拥有所选捕获字符串。
 * @return 创建/读取/等待/时钟失败为PLATFORM，捕获或环境分配失败为OUT_OF_MEMORY，完整流程或成功终止超时进程为CC_OK。
 * @note 正超时按单调实际时间检查，终止直接PID后waitpid，超时结果exit_code=-1。exec/chdir失败由子进程退出127表达，信号退出保存负信号号。
 *       env在父进程准备继承环境的覆盖副本，输入不修改。错误清理输出，非零命令退出仍由结果字段表达。
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

    uint64_t started_ms = 0;
    if (options->timeout_ms > 0 && !process_clock_ms(&started_ms))
        return cc_result_error(CC_ERR_PLATFORM, "Failed to read process clock");
    char **child_env = options->env ? build_process_environment(options->env) : NULL;
    if (options->env && !child_env)
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy process environment");

    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    pthread_mutex_lock(&process_spawn_gate);

    if (options->capture_stdout && create_process_pipe(stdout_pipe) != 0) {
        pthread_mutex_unlock(&process_spawn_gate);
        free_process_environment(child_env);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create stdout pipe");
    }

    if (options->capture_stderr && create_process_pipe(stderr_pipe) != 0) {
        close_pipe_pair(stdout_pipe);
        pthread_mutex_unlock(&process_spawn_gate);
        free_process_environment(child_env);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create stderr pipe");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close_pipe_pair(stdout_pipe);
        close_pipe_pair(stderr_pipe);
        pthread_mutex_unlock(&process_spawn_gate);
        free_process_environment(child_env);
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

        if (child_env) environ = child_env;

        if (options->args && options->args[0]) {
            execvp(options->command, options->args);
        } else {
            execlp("/bin/sh", "sh", "-c", options->command, NULL);
        }
        _exit(127);
    }

    pthread_mutex_unlock(&process_spawn_gate);
    free_process_environment(child_env);
    int setup_ok = 1;
    if (options->capture_stdout) {
        close_fd_if_open(&stdout_pipe[1]);
        setup_ok = set_nonblocking(stdout_pipe[0]);
    }
    if (options->capture_stderr) {
        close_fd_if_open(&stderr_pipe[1]);
        setup_ok = set_nonblocking(stderr_pipe[0]) && setup_ok;
    }
    if (!setup_ok) {
        kill(pid, SIGKILL);
        wait_for_pid(pid, NULL, 0);
        close_pipe_pair(stdout_pipe);
        close_pipe_pair(stderr_pipe);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to configure process pipes");
    }

    capture_buffer_t stdout_capture = {0};
    capture_buffer_t stderr_capture = {0};
    int timed_out = 0;
    int status = 0;
    int platform_error = 0;
    int child_done = 0;

    while (!child_done) {
        if (options->capture_stdout && drain_fd(stdout_pipe[0], &stdout_capture) < 0) platform_error = 1;
        if (options->capture_stderr && drain_fd(stderr_pipe[0], &stderr_capture) < 0) platform_error = 1;
        if (platform_error) {
            kill(pid, SIGKILL);
            wait_for_pid(pid, &status, 0);
            break;
        }

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
            platform_error = 1;
            child_done = 1;
            break;
        }

        if (options->timeout_ms > 0) {
            uint64_t now_ms;
            if (!process_clock_ms(&now_ms)) platform_error = 1;
            else timed_out = now_ms - started_ms >= (uint64_t)options->timeout_ms;
            if (platform_error || timed_out) {
                kill(pid, SIGKILL);
                if (wait_for_pid(pid, &status, 0) < 0) platform_error = 1;
                out_result->exit_code = -1;
                break;
            }
        }

        sleep_ms(10);
    }

    if (options->capture_stdout) {
        if (drain_after_exit(stdout_pipe[0], &stdout_capture, started_ms, options->timeout_ms, &timed_out) < 0) platform_error = 1;
        close_fd_if_open(&stdout_pipe[0]);
        out_result->stdout_text = stdout_capture.data ? stdout_capture.data : cc_copy_string("");
        stdout_capture.data = NULL;
    }
    if (options->capture_stderr) {
        if (drain_after_exit(stderr_pipe[0], &stderr_capture, started_ms, options->timeout_ms, &timed_out) < 0) platform_error = 1;
        close_fd_if_open(&stderr_pipe[0]);
        out_result->stderr_text = stderr_capture.data ? stderr_capture.data : cc_copy_string("");
        stderr_capture.data = NULL;
    }
    out_result->timed_out = timed_out;
    if (timed_out) out_result->exit_code = -1;

    if (stdout_capture.oom || stderr_capture.oom ||
        (options->capture_stdout && !out_result->stdout_text) ||
        (options->capture_stderr && !out_result->stderr_text)) {
        free(stdout_capture.data);
        free(stderr_capture.data);
        cc_process_result_free(out_result);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to capture process output");
    }

    if (platform_error) {
        cc_process_result_free(out_result);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to wait for or capture process output");
    }

    return cc_result_ok();
}

/** @brief 拥有直接子进程与两条协议管道，stderr重定向/dev/null，FILE成功接管fd后原槽置-1。 */
struct cc_process_pipe {
    pid_t pid; /**< 尚未回收的直接子进程ID，stop后为零。 */
    int stdin_fd; /**< 尚未交给FILE的拥有写端。 */
    int stdout_fd; /**< 尚未交给FILE的拥有读端。 */
    FILE *to_child; /**< 拥有stdin写流，write不缓存正文。 */
    FILE *from_child; /**< 拥有stdout读流，实际读行使用其fileno。 */
};

/**
 * @brief 关闭对象拥有的FILE与尚未转交的原生fd。
 * @param[in,out] pipe 可选存活对象，调用方已排空读写借用。
 * @post 管道端全部失效，不停止或释放进程对象。
 */
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
}

/**
 * @brief 创建stdin/stdout行协议子进程，stderr丢弃到/dev/null避免未消费管道反压。
 * @param[in] command 必需同步借用可执行名，不隐式运行shell。
 * @param[in] argv 可选NULL终止借用数组，有argv[0]则原样execvp，否则仅传command作为argv[0]。
 * @param[out] out_pipe 必需空资源槽，基础参数有效后先置NULL，成功交付唯一拥有对象供destroy。
 * @return 管道/fork/非阻塞/fdopen失败为PLATFORM，包装分配失败为OUT_OF_MEMORY，成功为CC_OK。
 * @note fork后父进程创建失败会关闭本次资源、杀死并wait回收直接子进程，成功只表示spawn完成，exec失败在首次通信中体现。
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
    pthread_mutex_lock(&process_spawn_gate);

    int to_child[2] = {-1, -1};
    int from_child[2] = {-1, -1};

    if (create_process_pipe(to_child) < 0 || create_process_pipe(from_child) < 0) {
        close_pipe_pair(to_child);
        close_pipe_pair(from_child);
        pthread_mutex_unlock(&process_spawn_gate);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create pipes for pipe process");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close_pipe_pair(to_child);
        close_pipe_pair(from_child);
        pthread_mutex_unlock(&process_spawn_gate);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to fork pipe process");
    }

    if (pid == 0) {
        if (dup2(to_child[0], STDIN_FILENO) < 0) _exit(127);
        if (dup2(from_child[1], STDOUT_FILENO) < 0) _exit(127);
        close_pipe_pair(to_child);
        close_pipe_pair(from_child);
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd < 0 || dup2(null_fd, STDERR_FILENO) < 0) _exit(127);
        if (null_fd != STDERR_FILENO) close(null_fd);
        if (argv && argv[0]) {
            execvp(command, argv);
        } else {
            execlp(command, command, NULL);
        }
        _exit(127);
    }

    pthread_mutex_unlock(&process_spawn_gate);
    close_fd_if_open(&to_child[0]);
    close_fd_if_open(&from_child[1]);

    cc_process_pipe_t *p = calloc(1, sizeof(*p));
    if (!p) {
        close_pipe_pair(to_child);
        close_pipe_pair(from_child);
        kill(pid, SIGKILL);
        wait_for_pid(pid, NULL, 0);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate pipe process");
    }

    p->pid = pid;
    p->stdin_fd = to_child[1];
    p->stdout_fd = from_child[0];
    if (!set_nonblocking(p->stdout_fd)) {
        close_process_pipe_handles(p);
        kill(pid, SIGKILL);
        wait_for_pid(pid, NULL, 0);
        free(p);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to configure pipe stdout");
    }

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

/**
 * @brief 将借用正文及一个换行阻塞写入stdin，并在当前线程隔离本次SIGPIPE。
 * @param[in,out] pipe 必需存活对象，stdin仍开放，同方向写入由调用方串行。
 * @param[in] data 必需NUL结尾借用正文，函数始终另追加换行。
 * @return 完整stdio写/刷新为CC_OK，信号mask或写入失败为PLATFORM，空参数为INVALID_ARGUMENT。
 * @note 不改进程全局SIGPIPE处置，只消费本次EPIPE新产生且原先不存在的pending信号后恢复线程mask。
 *       本入口没有超时或取消参数，失败可能已写部分前缀，不回滚管道字节。
 */
cc_result_t cc_process_pipe_write(cc_process_pipe_t *pipe, const char *data)
{
    if (!pipe || !pipe->to_child || !data) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pipe write arguments");
    }

    sigset_t blocked, original, pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &blocked, &original) != 0)
        return cc_result_error(CC_ERR_PLATFORM, "Failed to block pipe write signal");
    if (sigpending(&pending) != 0) {
        pthread_sigmask(SIG_SETMASK, &original, NULL);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to inspect pipe write signal");
    }
    int had_pending = sigismember(&pending, SIGPIPE);
    int written = fprintf(pipe->to_child, "%s\n", data);
    int write_error = written < 0 ? errno : 0;
    int flushed = fflush(pipe->to_child);
    int flush_error = flushed != 0 ? errno : 0;
    int failed = written < 0 || flushed != 0 || ferror(pipe->to_child);
    if (!had_pending && (write_error == EPIPE || flush_error == EPIPE) &&
        sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE)) {
        int received;
        (void)sigwait(&blocked, &received);
    }
    int restored = pthread_sigmask(SIG_SETMASK, &original, NULL) == 0;
    if (failed || !restored) {
        clearerr(pipe->to_child);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to write to pipe process");
    }

    return cc_result_ok();
}

/** @brief 取得stdout原生fd借用。 @param[in] pipe 可选存活对象。 @return 未转交fd或FILE的fileno，已关闭/NULL为-1。 */
static int pipe_stdout_fd(cc_process_pipe_t *pipe)
{
    if (!pipe) return -1;
    if (pipe->stdout_fd >= 0) return pipe->stdout_fd;
    return pipe->from_child ? fileno(pipe->from_child) : -1;
}

/**
 * @brief 以poll分片等待stdout一行，持续有数据时也检查实际时间与取消。
 * @param[in,out] pipe 必需存活对象，同方向读取串行，stop/destroy前排空本次借用。
 * @param[in] timeout_ms 正毫秒预算，非正数沿三十秒默认。
 * @param[in] cancel_token 可选存活借用token，NULL不取消。
 * @param[out] out_line 必需空资源槽，基础参数有效后置NULL，成功交付free可释放的拥有行。
 * @return 去换行/末尾CR后成功为CC_OK，空EOF/原生错误为PLATFORM，超时为TIMEOUT，取消为CANCELLED，分配失败为OUT_OF_MEMORY。
 * @note EOF前已有正文则交付末行，错误释放已收前缀但不回滚已消费字节；单次poll最多五十毫秒，取消先于本轮超时检查。
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

    uint64_t started_ms;
    if (!process_clock_ms(&started_ms)) {
        free(line);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to read pipe clock");
    }
    while (1) {
        if (cc_cancel_token_is_cancelled(cancel_token)) {
            free(line);
            return cc_result_error(CC_ERR_CANCELLED, "Pipe process read cancelled");
        }

        uint64_t now_ms;
        if (!process_clock_ms(&now_ms)) {
            free(line);
            return cc_result_error(CC_ERR_PLATFORM, "Failed to read pipe clock");
        }
        uint64_t elapsed_ms = now_ms - started_ms;
        int wait_ms = elapsed_ms >= (uint64_t)timeout_ms ? 0 : timeout_ms - (int)elapsed_ms;
        if (wait_ms > 50) wait_ms = 50;
        if (wait_ms <= 0) {
            free(line);
            return cc_result_error(CC_ERR_TIMEOUT, "Timed out waiting for pipe process line");
        }

        struct pollfd input = {.fd = fd, .events = POLLIN};
        int ready = poll(&input, 1, wait_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            free(line);
            return cc_result_error(CC_ERR_PLATFORM, "Pipe process stdout poll failed");
        }
        if (ready == 0) continue;

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

/**
 * @brief 不带取消token的有限预算读行适配。
 * @param[in,out] pipe 存活借用对象，读取规则同可取消入口。
 * @param[in] timeout_ms 毫秒预算，非正数沿三十秒默认。
 * @param[out] out_line 成功交付拥有字符串，输出与错误规则同可取消入口。
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
 * @brief 以三十秒默认预算读取一行。
 * @param[in,out] pipe 存活借用对象。
 * @param[out] out_line 成功交付free可释放的拥有行，失败规则同有限入口。
 * @return 下游原样结果，不接管pipe。
 */
cc_result_t cc_process_pipe_read_line(cc_process_pipe_t *pipe, char **out_line)
{
    return cc_process_pipe_read_line_timeout(pipe, 30000, out_line);
}

/**
 * @brief 停止直接子进程并关闭管道，先TERM约一百毫秒再KILL兜底。
 * @param[in,out] pipe 可选存活拥有对象，NULL安全，对象本身继续由调用者保管。
 * @pre 已排空同对象的读写与其他stop借用，调用方没有其他代码抢先wait回收本PID。
 * @post 正常原生路径完成wait回收并清空pid/管道端，可重复stop，void接口无原生错误返回通道。
 */
void cc_process_pipe_stop(cc_process_pipe_t *pipe)
{
    if (!pipe) return;

    if (pipe->pid > 0) {
        int status = 0;
        pid_t state;
        do { state = waitpid(pipe->pid, &status, WNOHANG); } while (state < 0 && errno == EINTR);
        if (state == 0) {
            kill(pipe->pid, SIGTERM);
            sleep_ms(100);
            do { state = waitpid(pipe->pid, &status, WNOHANG); } while (state < 0 && errno == EINTR);
            if (state == 0) {
                kill(pipe->pid, SIGKILL);
                wait_for_pid(pipe->pid, &status, 0);
            }
        }
        pipe->pid = 0;
    }

    close_process_pipe_handles(pipe);
}

/**
 * @brief 停止并释放唯一拥有的管道对象。
 * @param[in,out] pipe 消费拥有对象，NULL安全，所有借用已排空。
 * @post 进程与管道按stop路径清理，包装失效，调用方不得再次使用旧指针。
 */
void cc_process_pipe_destroy(cc_process_pipe_t *pipe)
{
    if (!pipe) return;
    cc_process_pipe_stop(pipe);
    free(pipe);
}
