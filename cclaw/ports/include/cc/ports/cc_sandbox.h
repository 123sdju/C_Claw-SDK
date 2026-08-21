



#ifndef CC_SANDBOX_H
#define CC_SANDBOX_H

#include "cc/core/cc_result.h"

/*
 * sandbox 中要执行的命令。
 *
 * command/working_dir/env 由调用方借用；具体实现必须结合 policy/approval 控制高风险
 * 行为。timeout_ms 是执行预算，防止进程长期占用嵌入式设备资源。
 */
typedef struct cc_sandbox_command {
    char *command;

    char *working_dir;

    char **env;

    int timeout_ms;

} cc_sandbox_command_t;

/*
 * sandbox 执行结果。
 *
 * stdout_text/stderr_text 由 result 拥有，通过 cc_sandbox_result_free() 释放。timed_out
 * 区分命令自身失败和沙箱超时终止。
 */
typedef struct cc_sandbox_result {
    int exit_code;

    char *stdout_text;

    char *stderr_text;

    int timed_out;

} cc_sandbox_result_t;


/* sandbox vtable 前置声明。 */
typedef struct cc_sandbox_vtable cc_sandbox_vtable_t;

/* sandbox 接口对象前置声明。 */
typedef struct cc_sandbox cc_sandbox_t;

/*
 * sandbox 接口对象。
 *
 * self 指向具体平台 sandbox 实现，vtable 提供 run/destroy。核心 SDK 不内置业务命令
 * 网关，只提供受控执行的抽象边界。
 */
struct cc_sandbox {
    void *self;

    const cc_sandbox_vtable_t *vtable;
};


/* sandbox vtable。 */
struct cc_sandbox_vtable {


    /* 执行命令并填充结果；实现应处理 timeout、输出限制和工作目录边界。 */
    cc_result_t (*run)(
        void *self,
        const cc_sandbox_command_t *command,
        cc_sandbox_result_t *out_result
    );



    /* 销毁 sandbox self。 */
    void (*destroy)(void *self);
};

/* 释放 sandbox result 的 stdout/stderr 字符串并清零。 */
void cc_sandbox_result_free(cc_sandbox_result_t *result);

#endif
