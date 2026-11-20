



#include "cc/ports/cc_logger.h"
#include "cc/ports/cc_thread.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_redaction.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

/*
 * logger 拥有独立名称及实例锁，锁只保护名称快照和过滤级别。
 * 格式化在实例锁外执行，stderr 由进程期 sink 锁串行，用户 sink 的并发由其自身协调。
 */
struct cc_logger {
    char *name;
    cc_log_level_t level;
    cc_mutex_t mutex;
};

static cc_mutex_t g_sink_mutex;
static cc_logger_sink_fn g_sink;
static void *g_sink_user_data;

/**
 * @brief 通过线程端口一次性安装进程期 sink 锁，失败允许后续重试。
 * @return 进程期锁借用，创建失败为 NULL，拥有结果的消息在本函数消费。
 * @note 该锁保护 sink/user_data 成对快照及 stderr 输出，当前无进程期销毁入口。
 */
static cc_mutex_t ensure_sink_mutex(void)
{
    cc_result_t rc = cc_mutex_init_once(&g_sink_mutex);
    int ready = rc.code == CC_OK;
    cc_result_free(&rc);
    return ready ? g_sink_mutex : NULL;
}

/**
 * @brief 依序创建拥有名称、过滤阈值和实例 mutex 的 logger。
 * @param[in] name 可选名称借用，复制失败报 OOM，NULL 使用 c-claw。
 * @param[in] level 初始过滤阈值，按枚举数值比较。
 * @param[out] out_logger 必填独立槽，先置 NULL，成功由调用者拥有并最终 destroy。
 * @return CC_OK 或参数、名称/对象分配、mutex 创建错误，失败清理本次部分资源。
 */
cc_result_t cc_logger_create(const char *name, cc_log_level_t level, cc_logger_t **out_logger)
{
    if (!out_logger) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null logger output");
    *out_logger = NULL;
    cc_logger_t *logger = calloc(1, sizeof(cc_logger_t));
    if (!logger) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create logger");

    logger->name = name ? cc_copy_string(name) : cc_copy_string("c-claw");
    if (!logger->name) {
        free(logger);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy logger name");
    }
    logger->level = level;
    cc_result_t rc = cc_mutex_create(&logger->mutex);
    if (rc.code != CC_OK) {
        free(logger->name);
        free(logger);
        return rc;
    }
    cc_result_free(&rc);
    *out_logger = logger;
    return cc_result_ok();
}


/**
 * @brief 消费实例名称、mutex 和 logger 存储。
 * @pre owner 已关闭新入口并结束该实例的在途访问及锁等待，成功后禁止继续调用旧句柄。
 * @param[in,out] logger 可选拥有实例，NULL 为空操作。
 * @post 实例及名称借用失效，全局 sink 注册与其 user_data 仍由全局拥有者保管。
 */
void cc_logger_destroy(cc_logger_t *logger)
{
    if (!logger) return;
    cc_mutex_lock(logger->mutex);
    free(logger->name);
    cc_mutex_unlock(logger->mutex);
    cc_mutex_destroy(logger->mutex);
    free(logger);
}


/**
 * @brief 将日志级别转换为固定展示字符串。
 * @param[in] level 输入枚举值。
 * @return 进程期只读字符串，未知值返回 UNKNOWN，调用者禁止释放。
 */
static const char *level_string(cc_log_level_t level)
{
    switch (level) {
    case CC_LOG_TRACE: return "TRACE";
    case CC_LOG_DEBUG: return "DEBUG";
    case CC_LOG_INFO: return "INFO";
    case CC_LOG_WARN: return "WARN";
    case CC_LOG_ERROR: return "ERROR";
    case CC_LOG_FATAL: return "FATAL";
    default: return "UNKNOWN";
    }
}


/**
 * @brief 取得实例快照后格式化及脱敏正文，交给全局 sink 或 stderr。
 * @param[in,out] logger 可选借用实例，NULL 为空操作，owner 保证入口和快照期间存活。
 * @param[in] level 本条日志级别，小于实例阈值时直接返回。
 * @param[in] fmt 可选 printf 格式串，NULL 不输出，正文借用覆盖两次格式化。
 * @param[in] ... 符合格式类型的参数，引用内容在两次读取期间保持稳定。
 * @post 临时名称、格式化正文、脱敏副本和完整行均在返回前释放，日志可能因 OOM 降级为静态诊断。
 * @note 实例锁只保护阈值与名称复制，sink 在全部锁外同步调用，允许并发及重入。
 * sink/user_data 成对读取后仍借用旧注册，外层保证其寿命覆盖已取得快照的调用。
 */
void cc_logger_log(cc_logger_t *logger, cc_log_level_t level, const char *fmt, ...)
{
    if (!logger || !fmt) return;

    cc_mutex_lock(logger->mutex);

    if (level < logger->level) {
        cc_mutex_unlock(logger->mutex);
        return;
    }

    char *logger_name = logger->name ? cc_copy_string(logger->name) : cc_copy_string("c-claw");
    cc_mutex_unlock(logger->mutex);

    time_t now = time(NULL);
    struct tm tm_info;
    char time_buf[32] = "unknown-time";
#ifdef _WIN32
    int time_ready = localtime_s(&tm_info, &now) == 0;
#else
    int time_ready = localtime_r(&now, &tm_info) != NULL;
#endif
    if (time_ready && strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info) == 0)
        memcpy(time_buf, "unknown-time", sizeof("unknown-time"));



    va_list args;
    va_start(args, fmt);
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);
    char *line = NULL;
    if (needed >= 0) {
        char *formatted = malloc((size_t)needed + 1);
        if (formatted) {
            int written = vsnprintf(formatted, (size_t)needed + 1, fmt, args);
            char *redacted = NULL;
            if (written >= 0 && written <= needed) redacted = cc_redact_secrets(formatted);
            const char *message = redacted ? redacted : "[REDACTION_FAILED]";
            if (written < 0 || written > needed) message = "[FORMAT_FAILED]";
            int line_needed = snprintf(NULL,
                                       0,
                                       "[%s] [%s] [%s] %s",
                                       time_buf,
                                       level_string(level),
                                       logger_name ? logger_name : "c-claw",
                                       message);
            if (line_needed >= 0) {
                line = malloc((size_t)line_needed + 1);
                if (line) {
                    int line_written = snprintf(line,
                                                (size_t)line_needed + 1,
                                                "[%s] [%s] [%s] %s",
                                                time_buf,
                                                level_string(level),
                                                logger_name ? logger_name : "c-claw",
                                                message);
                    if (line_written < 0 || line_written > line_needed) {
                        free(line);
                        line = NULL;
                    }
                }
            }
            free(redacted);
            free(formatted);
        } else {
            line = cc_copy_string("[logger] format allocation failed");
        }
    } else {
        line = cc_copy_string("[logger] format error");
    }
    va_end(args);



    cc_logger_sink_fn sink = NULL;
    void *sink_user_data = NULL;
    cc_mutex_t sink_mutex = ensure_sink_mutex();
    if (sink_mutex) {
        cc_mutex_lock(sink_mutex);
        sink = g_sink;
        sink_user_data = g_sink_user_data;
        cc_mutex_unlock(sink_mutex);
    }

    if (sink && line) {
        sink(sink_user_data, level, logger_name ? logger_name : "c-claw", line);
    } else {
        if (sink_mutex) cc_mutex_lock(sink_mutex);
        if (line) {
            fputs(line, stderr);
        } else {
            fprintf(stderr,
                    "[%s] [%s] [%s] <log OOM>",
                    time_buf,
                    level_string(level),
                    logger_name ? logger_name : "c-claw");
        }
        fprintf(stderr, "\n");
        fflush(stderr);
        if (sink_mutex) cc_mutex_unlock(sink_mutex);
    }
    free(line);
    free(logger_name);
}


/**
 * @brief 在实例 mutex 内替换日志过滤阈值。
 * @param[in,out] logger 可选稳定借用实例，NULL 为空操作。
 * @param[in] level 新阈值，已通过旧阈值检查的日志继续完成本次输出。
 * @post 后续取得实例锁的日志读取新值，未提供回溯取消或销毁屏障。
 */
void cc_logger_set_level(cc_logger_t *logger, cc_log_level_t level)
{
    if (!logger) return;
    cc_mutex_lock(logger->mutex);
    logger->level = level;
    cc_mutex_unlock(logger->mutex);
}

/**
 * @brief 在进程期 mutex 内成对替换全局 sink 及用户上下文。
 * @param[in] sink 可选同步回调，NULL 恢复 stderr 默认输出。
 * @param[in,out] user_data 回调上下文借用，原拥有者保证其覆盖已取快照的全部调用。
 * @post 成功取得 sink 锁后新调用读取新注册，锁创建失败保留原注册。
 * @note void 接口不报告锁创建失败，也不等待旧回调退出，替换及最终清理由全局 owner 协调。
 */
void cc_logger_set_sink(cc_logger_sink_fn sink, void *user_data)
{
    cc_mutex_t sink_mutex = ensure_sink_mutex();
    if (!sink_mutex) {
        return;
    }

    cc_mutex_lock(sink_mutex);
    g_sink = sink;
    g_sink_user_data = user_data;
    cc_mutex_unlock(sink_mutex);
}
