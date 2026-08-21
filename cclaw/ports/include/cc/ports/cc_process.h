



#ifndef CC_PROCESS_H
#define CC_PROCESS_H

#include "cc/core/cc_result.h"

/* cancel token 前置声明，用于可取消的管道读取。 */
typedef struct cc_cancel_token cc_cancel_token_t;

/*
 * 一次进程执行选项。
 *
 * command/args/working_dir/env 都由调用方借用；实现不能保存指针。shell 类能力默认
 * 属于高风险操作，业务层应通过 policy/approval 控制是否允许执行。
 */
typedef struct cc_process_options {

    char *command;

    char **args;

    char *working_dir;

    char **env;

    int timeout_ms;

    int capture_stdout;

    int capture_stderr;
} cc_process_options_t;

/*
 * 进程执行结果。
 *
 * stdout_text/stderr_text 由 result 拥有，调用 cc_process_result_free() 释放。timed_out
 * 用于区分进程非零退出和被 SDK timeout 终止。
 */
typedef struct cc_process_result {

    int exit_code;

    char *stdout_text;

    char *stderr_text;

    int timed_out;
} cc_process_result_t;

/* 运行一次进程并等待结束；out_result 成功或部分失败后由调用方 free。 */
cc_result_t cc_process_run(
    const cc_process_options_t *options,
    cc_process_result_t *out_result
);

/* 释放进程结果中的 stdout/stderr 字符串；不释放 result 指针。 */
void cc_process_result_free(cc_process_result_t *result);




/* 长连接子进程管道句柄，用于 MCP/plugin 等请求响应式协议。 */
typedef struct cc_process_pipe cc_process_pipe_t;

/* 启动带 stdin/stdout 管道的子进程；成功后调用方用 destroy 释放。 */
cc_result_t cc_process_pipe_spawn(
    const char *command,
    char *const argv[],
    cc_process_pipe_t **out_pipe
);

/* 向子进程 stdin 写入数据；data 通常需要包含协议换行符。 */
cc_result_t cc_process_pipe_write(
    cc_process_pipe_t *pipe,
    const char *data
);

/* 从子进程 stdout 读取一行；out_line 由调用方 free()。 */
cc_result_t cc_process_pipe_read_line(
    cc_process_pipe_t *pipe,
    char **out_line
);

/* 带超时读取一行；超时应返回 CC_ERR_TIMEOUT。 */
cc_result_t cc_process_pipe_read_line_timeout(
    cc_process_pipe_t *pipe,
    int timeout_ms,
    char **out_line
);

/* 带超时和取消 token 读取一行；取消应返回 CC_ERR_CANCELLED。 */
cc_result_t cc_process_pipe_read_line_timeout_cancel(
    cc_process_pipe_t *pipe,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    char **out_line
);

/* 请求停止子进程；可用于取消或 runtime shutdown。 */
void cc_process_pipe_stop(cc_process_pipe_t *pipe);

/* 销毁管道句柄并回收子进程资源；允许 NULL。 */
void cc_process_pipe_destroy(cc_process_pipe_t *pipe);

#endif
