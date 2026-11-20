#include "cc/app/cc_cancel_token.h"
#include "cc/ports/cc_process.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(CC_TEST_PROCESS_FAILURES)
static int fail_capture_allocation;
static int fail_nonblocking;
static int interrupt_wait;
/** @brief 真实realloc链接入口。 @param[in,out] old 旧拥有块或NULL。 @param[in] size 新字节数。 @return 新拥有块或NULL。 */
void *__real_realloc(void *old, size_t size);
/** @brief 真实fcntl链接入口。 @param[in] fd 描述符。 @param[in] command 操作码。 @param[in] ... 对应原生参数。 @return 原生结果。 */
int __real_fcntl(int fd, int command, ...);
/** @brief 真实waitpid链接入口。 @param[in] pid 子进程选择。 @param[out] status 可选状态槽。 @param[in] options 等待选项。 @return 原生PID或错误。 */
pid_t __real_waitpid(pid_t pid, int *status, int options);

/**
 * @brief 注入下一次捕获扩容失败，其余走真实libc。
 * @param[in,out] old 旧拥有缓冲，失败时仍由生产代码拥有。
 * @param[in] size 请求字节数。
 * @return 定向失败为NULL，其他为真实realloc结果。
 */
void *__wrap_realloc(void *old, size_t size)
{
    if (fail_capture_allocation) { fail_capture_allocation = 0; return NULL; }
    return __real_realloc(old, size);
}

/**
 * @brief 仅向生产端口下一次F_SETFL注入失败。
 * @param[in] fd 借用描述符。
 * @param[in] command 本测试涉及的GETFD、GETFL、SETFL、SETFD或DUPFD操作。
 * @param[in] ... 这些调用均提供一个int参数，原样转交。
 * @return 注入失败为-1/EIO，其余为真实fcntl结果。
 */
int __wrap_fcntl(int fd, int command, ...)
{
    va_list args; va_start(args, command);
    int value = va_arg(args, int); va_end(args);
    if (command == F_SETFL && fail_nonblocking) {
        fail_nonblocking = 0; errno = EIO; return -1;
    }
    return __real_fcntl(fd, command, value);
}

/**
 * @brief 在stop首次非阻塞wait时注入EINTR。
 * @param[in] pid 必需直接子进程ID。
 * @param[out] status 可选原生状态输出。
 * @param[in] options 等待选项。
 * @return 注入为-1/EINTR，其余为真实waitpid结果。
 */
pid_t __wrap_waitpid(pid_t pid, int *status, int options)
{
    if (interrupt_wait && options == WNOHANG) {
        interrupt_wait = 0; errno = EINTR; return -1;
    }
    return __real_waitpid(pid, status, options);
}
#endif

/** @brief 取得测试单调时间，失败终止测试。 @return 单调时钟毫秒值。 */
static uint64_t now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) abort();
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/**
 * @brief 消费拥有结果并检查错误码。
 * @param[in] result 按值移交的拥有结果，原副本不再清理。
 * @param[in] expected 预期状态码。
 * @return 匹配为一，其他为零，诊断始终释放。
 */
static int result_is(cc_result_t result, cc_error_code_t expected)
{
    int ok = result.code == expected;
    if (!ok) fprintf(stderr, "process result=%d expected=%d\n", result.code, expected);
    cc_result_free(&result);
    return ok;
}

/**
 * @brief 让测试子进程完整写出片段，处理EINTR和短写。
 * @param[in] fd 借用输出描述符。
 * @param[in] data 借用输入字节。
 * @param[in] length 要写出的字节数。
 * @return 写完为一，原生失败为零。
 */
static int write_all(int fd, const char *data, size_t length)
{
    while (length) {
        ssize_t written = write(fd, data, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return 0;
        data += written; length -= (size_t)written;
    }
    return 1;
}

/**
 * @brief 当前可执行文件的子进程夹具，生成双流、连续流、协议或环境输出。
 * @param[in] mode 必需借用模式名。
 * @return 子进程业务退出码，continuous写到读端关闭或进程被终止为止。
 */
static int child_mode(const char *mode)
{
    char block[4096]; memset(block, 'x', sizeof(block));
    if (strcmp(mode, "--both") == 0) {
        for (int i = 0; i < 64; i++) {
            if (!write_all(STDOUT_FILENO, block, sizeof(block)) ||
                !write_all(STDERR_FILENO, block, sizeof(block))) return 2;
        }
        return 0;
    }
    if (strcmp(mode, "--continuous") == 0) {
        while (write_all(STDOUT_FILENO, block, sizeof(block))) {}
        return 0;
    }
    if (strcmp(mode, "--stderr-echo") == 0) {
        for (int i = 0; i < 64; i++)
            if (!write_all(STDERR_FILENO, block, sizeof(block))) return 2;
        if (!write_all(STDOUT_FILENO, "ready\n", 6)) return 2;
        char line[64];
        if (!fgets(line, sizeof(line), stdin)) return 3;
        return write_all(STDOUT_FILENO, line, strlen(line)) ? 0 : 2;
    }
    if (strcmp(mode, "--partial") == 0)
        return write_all(STDOUT_FILENO, "tail\r", 5) ? 0 : 2;
    if (strcmp(mode, "--environment") == 0) {
        const char *value = getenv("CC_PROCESS_LITERAL");
        return value && getenv("PATH") && write_all(STDOUT_FILENO, value, strlen(value)) ? 0 : 3;
    }
    if (strcmp(mode, "--expect-uninherited") == 0) {
        const char *entry = getenv("CC_PROCESS_PIPE_IDS");
        if (!entry) return 3;
        while (*entry) {
            char *next;
            long fd = strtol(entry, &next, 10);
            if (*next != ':') return 3;
            unsigned long long inode = strtoull(next + 1, &next, 10);
            struct stat status;
            if (fstat((int)fd, &status) == 0 && S_ISFIFO(status.st_mode) &&
                (unsigned long long)status.st_ino == inode) return 3;
            if (*next == ',') next++;
            entry = next;
        }
        return 0;
    }
    if (strcmp(mode, "--idle") == 0) { sleep(3); return 0; }
    return 7;
}

/**
 * @brief 同步运行本可执行文件的子模式并捕获双流。
 * @param[in] executable 借用当前测试程序路径。
 * @param[in] mode 借用子模式。
 * @param[in] timeout 正毫秒预算或非正不限时。
 * @param[out] output 必需空资源槽，遵循真实process_run输出规则。
 * @return 真实端口拥有结果，交调用方清理。
 */
static cc_result_t run_mode(const char *executable, const char *mode, int timeout,
    cc_process_result_t *output)
{
    char *args[] = {(char *)executable, (char *)mode, NULL};
    cc_process_options_t options = {0};
    options.command = (char *)executable; options.args = args;
    options.timeout_ms = timeout; options.capture_stdout = options.capture_stderr = 1;
    return cc_process_run(&options, output);
}

/**
 * @brief 检查双流大输出、业务退出码及只读字面量环境覆盖。
 * @param[in] executable 借用当前测试程序路径。
 * @return 全部满足为一，失败为零，捕获结果均释放。
 */
static int test_run_capture(const char *executable)
{
    cc_process_result_t output = {0};
    output.exit_code = 91;
    int ok = result_is(cc_process_run(NULL, &output), CC_ERR_INVALID_ARGUMENT) && output.exit_code == 91;
    cc_process_options_t invalid = {0};
    ok = result_is(cc_process_run(&invalid, &output), CC_ERR_INVALID_ARGUMENT) && output.exit_code == 91 && ok;
    ok = result_is(run_mode(executable, "--both", 3000, &output), CC_OK) && ok;
    ok = ok && output.exit_code == 0 && !output.timed_out &&
        output.stdout_text && strlen(output.stdout_text) == 262144 &&
        output.stderr_text && strlen(output.stderr_text) == 262144;
    cc_process_result_free(&output);
    ok = result_is(run_mode(executable, "--seven", 3000, &output), CC_OK) && ok;
    ok = ok && output.exit_code == 7 && !output.timed_out;
    cc_process_result_free(&output);
    char *args[] = {(char *)executable, "--environment", NULL};
    char *env[] = {"CC_PROCESS_LITERAL=first", "CC_PROCESS_LITERAL=literal-value", "ignored", NULL};
    cc_process_options_t options = {0};
    options.command = (char *)executable; options.args = args; options.env = env;
    options.capture_stdout = 1; options.timeout_ms = 3000;
    ok = result_is(cc_process_run(&options, &output), CC_OK) && ok;
    ok = ok && output.exit_code == 0 && output.stdout_text &&
        strcmp(output.stdout_text, "literal-value") == 0 && !output.stderr_text;
    cc_process_result_free(&output);
    return ok;
}

/**
 * @brief 检查stderr不反压协议、EOF末行以及stop后的资源状态。
 * @param[in] executable 借用当前测试程序路径。
 * @return 全部满足为一，失败为零，正常收尾释放行和进程对象。
 */
static int test_pipe_protocol(const char *executable)
{
    cc_process_pipe_t *pipe = NULL; char *line = NULL;
    char *args[] = {(char *)executable, "--stderr-echo", NULL};
    if (!result_is(cc_process_pipe_spawn(executable, args, &pipe), CC_OK)) return 0;
    int ok = result_is(cc_process_pipe_read_line_timeout(pipe, 1500, &line), CC_OK);
    ok = ok && line && strcmp(line, "ready") == 0; free(line); line = NULL;
    if (ok) ok = result_is(cc_process_pipe_write(pipe, "echo"), CC_OK);
    if (ok) ok = result_is(cc_process_pipe_read_line(pipe, &line), CC_OK);
    ok = ok && line && strcmp(line, "echo") == 0; free(line);
    cc_process_pipe_destroy(pipe); pipe = NULL; line = NULL;
    args[1] = "--partial";
    if (!result_is(cc_process_pipe_spawn(executable, args, &pipe), CC_OK)) return 0;
    int partial = result_is(cc_process_pipe_read_line(pipe, &line), CC_OK);
    ok = ok && partial && line && strcmp(line, "tail") == 0; free(line);
    line = (char *)&line;
    ok = result_is(cc_process_pipe_read_line(pipe, &line), CC_ERR_PLATFORM) && !line && ok;
    cc_process_pipe_stop(pipe); cc_process_pipe_stop(pipe);
    ok = result_is(cc_process_pipe_write(pipe, "closed"), CC_ERR_INVALID_ARGUMENT) && ok;
    line = (char *)&line;
    ok = result_is(cc_process_pipe_read_line(pipe, &line), CC_ERR_INVALID_ARGUMENT) && !line && ok;
    cc_process_pipe_destroy(pipe);
    return ok;
}

/**
 * @brief 检查持续输出仍推进真实预算，取消结束读行后对象仍由调用方清理。
 * @param[in] executable 借用当前测试程序路径。
 * @return 超时、取消与回收满足为一，失败为零。
 */
static int test_budgets_and_cancel(const char *executable)
{
    cc_process_result_t output = {0}; uint64_t started = now_ms();
    int ok = result_is(run_mode(executable, "--continuous", 50, &output), CC_OK);
    ok = ok && output.timed_out && output.exit_code == -1 && now_ms() - started < 1500;
    cc_process_result_free(&output);
    char *args[] = {(char *)executable, "--continuous", NULL};
    cc_process_pipe_t *pipe = NULL; char *line = (char *)&line;
    if (!result_is(cc_process_pipe_spawn(executable, args, &pipe), CC_OK)) return 0;
    started = now_ms();
    ok = result_is(cc_process_pipe_read_line_timeout(pipe, 50, &line), CC_ERR_TIMEOUT) &&
        !line && now_ms() - started < 1500 && ok;
    cc_cancel_source_t *source = NULL;
    if (!result_is(cc_cancel_source_create(&source), CC_OK)) { cc_process_pipe_destroy(pipe); return 0; }
    cc_cancel_source_cancel(source);
    ok = result_is(cc_process_pipe_read_line_timeout_cancel(pipe, 1000,
        cc_cancel_source_token(source), &line), CC_ERR_CANCELLED) && !line && ok;
    cc_cancel_source_destroy(source); cc_process_pipe_destroy(pipe);
    return ok;
}

static volatile sig_atomic_t sigpipe_calls;
/** @brief 记录意外SIGPIPE。 @param[in] signal_number 信号编号，忽略。 @post 计数递增，不分配或执行stdio。 */
static void on_sigpipe(int signal_number) { (void)signal_number; sigpipe_calls++; }

/**
 * @brief 检查spawn保留进程SIGPIPE处置，写关闭stdin只返回错误。
 * @param[in] executable 借用当前测试程序路径。
 * @return 策略、投递次数和错误码均满足为一，所有路径恢复原信号处置。
 */
static int test_sigpipe_scope(const char *executable)
{
    struct sigaction previous, action = {0}, current;
    action.sa_handler = on_sigpipe; sigemptyset(&action.sa_mask);
    if (sigaction(SIGPIPE, &action, &previous) != 0) return 0;
    cc_process_pipe_t *pipe = NULL; char *line = NULL;
    char *args[] = {(char *)executable, "--partial", NULL};
    int ok = result_is(cc_process_pipe_spawn(executable, args, &pipe), CC_OK);
    if (ok) {
        ok = result_is(cc_process_pipe_read_line(pipe, &line), CC_OK); free(line); line = NULL;
        ok = result_is(cc_process_pipe_read_line(pipe, &line), CC_ERR_PLATFORM) && ok;
        ok = result_is(cc_process_pipe_write(pipe, "closed"), CC_ERR_PLATFORM) && ok;
        ok = sigaction(SIGPIPE, NULL, &current) == 0 && current.sa_handler == on_sigpipe && !sigpipe_calls && ok;
        sigset_t blocked, original, pending; sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
        if (pthread_sigmask(SIG_BLOCK, &blocked, &original) == 0) {
            int raised = raise(SIGPIPE) == 0;
            ok = result_is(cc_process_pipe_write(pipe, "closed"), CC_ERR_PLATFORM) && ok;
            int remained = sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE);
            ok = raised && remained && ok;
            if (remained) { int received; (void)sigwait(&blocked, &received); }
            ok = pthread_sigmask(SIG_SETMASK, &original, NULL) == 0 && ok;
        } else ok = 0;
    }
    cc_process_pipe_destroy(pipe);
    if (sigaction(SIGPIPE, &previous, NULL) != 0) ok = 0;
    return ok;
}

/**
 * @brief 在隔离子进程关闭0/1/2后核对两类管道的标准流重号处理。
 * @param[in] executable 借用当前测试程序路径。
 * @return 捕获与协议均正确为一，失败为零，主测试回收隔离子进程。
 */
static int test_closed_stdio(const char *executable)
{
    pid_t child = fork();
    if (child < 0) return 0;
    if (child == 0) {
        close(0); close(1); close(2);
        cc_process_result_t output = {0};
        int ok = result_is(run_mode(executable, "--both", 3000, &output), CC_OK);
        ok = ok && output.exit_code == 0 && output.stdout_text && strlen(output.stdout_text) == 262144 &&
            output.stderr_text && strlen(output.stderr_text) == 262144;
        cc_process_result_free(&output);
        char *args[] = {(char *)executable, "--partial", NULL};
        cc_process_pipe_t *pipe = NULL; char *line = NULL;
        if (!result_is(cc_process_pipe_spawn(executable, args, &pipe), CC_OK)) _exit(1);
        ok = result_is(cc_process_pipe_read_line(pipe, &line), CC_OK) && ok;
        ok = ok && line && strcmp(line, "tail") == 0;
        free(line); cc_process_pipe_destroy(pipe); _exit(ok ? 0 : 1);
    }
    int status; pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/**
 * @brief 通过仍存活管道的inode核对另一子进程exec后不继承协议端。
 * @param[in] executable 借用当前测试程序路径。
 * @return 新建两端均未继承且运行成功为一，失败为零，最终销毁原管道。
 * @note 比较inode允许ASan启动后复用同一描述符编号，已有父进程描述符不纳入本次断言。
 */
static int test_pipe_cloexec(const char *executable)
{
    unsigned char existed[256] = {0};
    for (int fd = 3; fd < 256; fd++) existed[fd] = fcntl(fd, F_GETFD, 0) >= 0;
    char *args[] = {(char *)executable, "--idle", NULL};
    cc_process_pipe_t *pipe = NULL;
    if (!result_is(cc_process_pipe_spawn(executable, args, &pipe), CC_OK)) return 0;
    char environment[1024] = "CC_PROCESS_PIPE_IDS=";
    size_t used = strlen(environment); int found = 0;
    for (int fd = 3; fd < 256; fd++) {
        struct stat status;
        if (existed[fd] || fstat(fd, &status) != 0 || !S_ISFIFO(status.st_mode)) continue;
        int added = snprintf(environment + used, sizeof(environment) - used,
            "%d:%llu,", fd, (unsigned long long)status.st_ino);
        if (added < 0 || (size_t)added >= sizeof(environment) - used) {
            cc_process_pipe_destroy(pipe); return 0;
        }
        used += (size_t)added; found++;
    }
    args[1] = "--expect-uninherited";
    char *env[] = {environment, NULL};
    cc_process_options_t options = {0};
    options.command = (char *)executable; options.args = args; options.env = env;
    options.capture_stdout = options.capture_stderr = 1; options.timeout_ms = 3000;
    cc_process_result_t output = {0};
    int ok = result_is(cc_process_run(&options, &output), CC_OK) && found == 2 && output.exit_code == 0;
    cc_process_result_free(&output); cc_process_pipe_destroy(pipe);
    return ok;
}

#if defined(CC_TEST_PROCESS_FAILURES)
/**
 * @brief 核对捕获OOM、非阻塞设置失败和stop首次wait打断后的清理。
 * @param[in] executable 借用当前测试程序路径。
 * @return 错误、空输出、故障消费和及时回收均满足为一，失败为零。
 */
static int test_failures(const char *executable)
{
    cc_process_result_t output = {0};
    fail_capture_allocation = 1;
    int ok = result_is(run_mode(executable, "--both", 1000, &output), CC_ERR_OUT_OF_MEMORY);
    ok = ok && !fail_capture_allocation && !output.stdout_text && !output.stderr_text;
    fail_nonblocking = 1;
    ok = result_is(run_mode(executable, "--idle", 1000, &output), CC_ERR_PLATFORM) &&
        !fail_nonblocking && !output.stdout_text && !output.stderr_text && ok;
    char *args[] = {(char *)executable, "--idle", NULL};
    cc_process_pipe_t *pipe = (cc_process_pipe_t *)&pipe;
    fail_nonblocking = 1;
    ok = result_is(cc_process_pipe_spawn(executable, args, &pipe), CC_ERR_PLATFORM) &&
        !pipe && !fail_nonblocking && ok;
    if (!result_is(cc_process_pipe_spawn(executable, args, &pipe), CC_OK)) return 0;
    interrupt_wait = 1; uint64_t started = now_ms();
    cc_process_pipe_destroy(pipe);
    return !interrupt_wait && now_ms() - started < 1500 && ok;
}
#endif

/**
 * @brief 运行真实POSIX进程端口回归，子模式以当前可执行文件提供夹具。
 * @param[in] argc 参数数量，有模式参数时进入子进程分支。
 * @param[in] argv 进程期借用参数数组，argv[0]作为后续子进程路径。
 * @return 父模式全部通过为零，失败为一，子模式返回对应业务退出码。
 * @note Linux可另启三项链接包装，测试未覆盖真实Windows内核行为。
 */
int main(int argc, char **argv)
{
    if (argc > 1) return child_mode(argv[1]);
    alarm(20);
    const char *executable = argv[0];
    int ok = test_run_capture(executable) && test_pipe_protocol(executable) &&
        test_budgets_and_cancel(executable) && test_sigpipe_scope(executable) &&
        test_closed_stdio(executable) && test_pipe_cloexec(executable);
#if defined(CC_TEST_PROCESS_FAILURES)
    if (ok) ok = test_failures(executable);
#endif
    int status; errno = 0;
    ok = waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD && ok;
    alarm(0);
    return ok ? 0 : 1;
}
