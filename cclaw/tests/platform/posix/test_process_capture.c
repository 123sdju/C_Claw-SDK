



#include "cc/ports/cc_process.h"

#include <stdio.h>
#include <string.h>


/*
 * 验证 POSIX process port 在大量 stdout 输出时能完整捕获数据。
 *
 * 子进程会输出 5000 * 16 = 80000 字节，这能覆盖 pipe 非阻塞读取、
 * wait/poll 循环和结果缓冲扩容路径。嵌入式 Linux 面试中可以把这个测试
 * 讲成“避免子进程 stdout/stderr pipe 写满导致死锁”的回归用例。
 */
int main(void)
{

    /* 使用 shell 构造稳定的大输出，避免依赖额外测试程序或外部文件。 */
    char *args[] = {
        "sh",
        "-c",
        "i=0; while [ $i -lt 5000 ]; do printf 0123456789abcdef; i=$((i+1)); done",
        NULL
    };


    /*
     * 同时开启 stdout/stderr 捕获和 timeout。
     *
     * timeout 不是本测试的目标，但它能覆盖 process_run 在等待循环中持续检查
     * deadline 的路径，确保大量输出不会绕过超时控制。
     */
    cc_process_options_t options;
    memset(&options, 0, sizeof(options));
    options.command = "sh";
    options.args = args;
    options.timeout_ms = 2000;
    options.capture_stdout = 1;
    options.capture_stderr = 1;


    /*
     * cc_process_run 返回结构化 cc_result_t，进程自身的 exit_code/timed_out
     * 放在 cc_process_result_t 中；这一区分能让调用方判断“端口调用失败”
     * 和“子进程正常运行但退出码非零”。
     */
    cc_process_result_t result;
    cc_result_t rc = cc_process_run(&options, &result);
    if (rc.code != CC_OK) {
        fprintf(stderr, "cc_process_run failed: %s\n", rc.message ? rc.message : "unknown");
        cc_result_free(&rc);
        return 1;
    }


    /* 核心断言：进程成功退出、未超时，并且 stdout 字节数完全一致。 */
    size_t stdout_len = result.stdout_text ? strlen(result.stdout_text) : 0;
    int ok = result.exit_code == 0 && !result.timed_out && stdout_len == 80000;

    if (!ok) {
        fprintf(stderr, "unexpected result: exit=%d timed_out=%d stdout_len=%zu\n",
            result.exit_code, result.timed_out, stdout_len);
    }

    cc_process_result_free(&result);
    return ok ? 0 : 1;
}
