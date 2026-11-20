



#ifndef CC_PROCESS_H
#define CC_PROCESS_H

#include "cc/core/cc_result.h"

/* cancel token 前置声明，用于可取消的管道读取。 */
typedef struct cc_cancel_token cc_cancel_token_t;

/** @brief 一次同步执行借用的选项，全部数组和字符串覆盖调用期并保持稳定。 */
typedef struct cc_process_options {
    char *command; /**< 必需NUL结尾命令，无args[0]时POSIX用/bin/sh -c，Windows用cmd.exe /c。 */
    char **args; /**< 可选NULL终止argv。POSIX原样传execvp，Windows引用command及args[1..]。 */
    char *working_dir; /**< 可选工作目录，NULL沿当前目录，Windows使用ANSI代码页。 */
    char **env; /**< 可选NULL终止环境数组，不修改输入。POSIX合并继承环境，Windows作为显式ANSI环境块，NULL均继承。 */
    int timeout_ms; /**< 正值为实际经过时间的毫秒预算，非正数不限时，原生终止和回收可能额外等待。 */
    int capture_stdout; /**< 非零捕获stdout，零沿父进程标准输出。 */
    int capture_stderr; /**< 非零独立捕获stderr，零沿父进程标准错误。 */
} cc_process_options_t;

/** @brief 成功执行流程交付的拥有输出，命令非零退出和超时由字段表达。 */
typedef struct cc_process_result {
    int exit_code; /**< 原生命令退出码，POSIX信号退出取负信号号，超时为-1。 */
    char *stdout_text; /**< 拥有NUL结尾输出，空捕获为拥有空串，POSIX未选捕获为NULL，Windows仍提供空串。 */
    char *stderr_text; /**< 拥有NUL结尾错误输出，空值规则同stdout_text，无独立二进制长度字段。 */
    int timed_out; /**< 非零表示有限预算耗尽并执行直接进程终止/回收路径，已捕获前缀可用。 */
} cc_process_result_t;

/**
 * @brief 同步运行命令，轮询排空所选输出并回收直接子进程。
 * @param[in] options 必需稳定借用选项，command必填，POSIX准备环境时进程环境也保持稳定。
 * @param[out] out_result 必需无已有资源槽，基础参数有效后清零，成功交付拥有字符串供result_free。
 * @return 平台流程或成功完成超时终止为CC_OK，参数/Windows命令行过长为INVALID_ARGUMENT，分配失败为OUT_OF_MEMORY，原生创建/读取/等待失败为PLATFORM。
 * @note 基础参数错误保留原输出槽，其余错误清理输出。命令业务失败通过exit_code表达，POSIX exec/chdir失败退出127。
 *       超时针对直接进程并等待回收，未建立或终止独立进程组，后代输出可能影响剩余捕获。Windows路径、命令和环境使用ANSI接口。
 */
cc_result_t cc_process_run(
    const cc_process_options_t *options,
    cc_process_result_t *out_result
);

/**
 * @brief 释放结果拥有的两条输出并清零结构。
 * @param[in,out] result 可选拥有结果，NULL安全，不释放结构本身。
 * @post 字符串借用失效，非NULL结构可重复清理。
 */
void cc_process_result_free(cc_process_result_t *result);




/** @brief 调用方唯一拥有的长连接协议子进程，内部没有对象级读写或生命周期锁。 */
typedef struct cc_process_pipe cc_process_pipe_t;

/**
 * @brief 创建带stdin/stdout管道的可执行程序，不隐式启动shell。
 * @param[in] command 必需调用期间借用可执行名，Windows采用ANSI代码页。
 * @param[in] argv 可选NULL终止借用数组，缺少argv[0]时只传command，Windows有argv[0]时仍以command构建首项。
 * @param[out] out_pipe 必需空资源槽，基础参数有效后先置NULL，成功由调用者destroy。
 * @return 成功为CC_OK，参数或Windows命令行超长为INVALID_ARGUMENT，包装分配失败为OUT_OF_MEMORY，原生创建/流转换失败为PLATFORM。
 * @note 创建半失败终止并等待已创建的直接进程。POSIX stderr丢弃到/dev/null，Windows沿既有行为合并stderr到stdout。
 *       POSIX成功表示fork完成，exec失败在后续EOF或写入错误中体现。
 */
cc_result_t cc_process_pipe_spawn(
    const char *command,
    char *const argv[],
    cc_process_pipe_t **out_pipe
);

/**
 * @brief 向子进程stdin阻塞写入正文，另追加一个换行并刷新。
 * @param[in,out] pipe 必需存活且写端开放的借用对象，同方向写入串行。
 * @param[in] data 必需NUL结尾借用正文，返回后不保留，已有换行也会再次追加。
 * @return 完整stdio写/刷新为CC_OK，空参数为INVALID_ARGUMENT，信号或写入失败为PLATFORM。
 * @note 没有写入超时或取消参数，调用者安排子进程消费stdin及所需的并发读输出，失败可能已写出前缀。
 *       Windows文本流按CRT规则处理换行，POSIX只临时屏蔽当前线程本次SIGPIPE，不改变全进程处置。
 */
cc_result_t cc_process_pipe_write(
    cc_process_pipe_t *pipe,
    const char *data
);

/**
 * @brief 以三十秒默认预算从stdout读取一行。
 * @param[in,out] pipe 必需存活对象，同方向读取串行。
 * @param[out] out_line 必需空资源槽，成功交付free可释放的拥有行。
 * @return 与有限预算入口相同的成功和错误分类。
 */
cc_result_t cc_process_pipe_read_line(
    cc_process_pipe_t *pipe,
    char **out_line
);

/**
 * @brief 在实际经过时间预算内读取一行，不使用取消token。
 * @param[in,out] pipe 必需存活对象，同方向读取串行。
 * @param[in] timeout_ms 正毫秒预算，非正数沿三十秒默认。
 * @param[out] out_line 必需空资源槽，基础参数有效后先置NULL，成功交付拥有行。
 * @return 完整行或EOF前末行为CC_OK，超时为TIMEOUT，分配失败为OUT_OF_MEMORY，空EOF/原生错误为PLATFORM，空参数为INVALID_ARGUMENT。
 * @note 去换行和末尾CR，失败释放已收前缀但不回滚已消费字节，未停止子进程。
 */
cc_result_t cc_process_pipe_read_line_timeout(
    cc_process_pipe_t *pipe,
    int timeout_ms,
    char **out_line
);

/**
 * @brief 在实际预算内读取一行，并在轮询中检查借用取消token。
 * @param[in,out] pipe 必需存活对象，同方向读取串行。
 * @param[in] timeout_ms 正毫秒预算，非正数沿三十秒默认。
 * @param[in] cancel_token 可选存活借用token，NULL不取消。
 * @param[out] out_line 必需空资源槽，基础参数有效后置NULL，成功交付free可释放行，失败保持NULL。
 * @return 取消为CANCELLED，其余分类同有限预算入口，取消与超时都不自动停止进程。
 * @note POSIX单次poll最多五十毫秒，Windows无数据时约十毫秒休眠，实际响应还取决于调度和分配。
 */
cc_result_t cc_process_pipe_read_line_timeout_cancel(
    cc_process_pipe_t *pipe,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    char **out_line
);

/**
 * @brief 停止并等待直接子进程，关闭协议管道，保留包装以供最终destroy。
 * @param[in,out] pipe 可选拥有对象，NULL安全，可重复stop。
 * @pre 已排空同对象读写和其他生命周期调用，POSIX调用方不另行wait回收该PID。
 * @post 正常原生路径中直接进程已结束且资源槽清空，void接口无原生错误返回通道。
 * @note POSIX先TERM约一百毫秒再KILL，Windows直接TerminateProcess，不保证后代进程结束。
 */
void cc_process_pipe_stop(cc_process_pipe_t *pipe);

/**
 * @brief 执行stop并释放唯一拥有的包装。
 * @param[in,out] pipe 消费拥有对象，NULL安全，所有读写和生命周期借用已排空。
 * @post 非NULL包装及其借用失效，不得重复使用旧指针。
 */
void cc_process_pipe_destroy(cc_process_pipe_t *pipe);

#endif
