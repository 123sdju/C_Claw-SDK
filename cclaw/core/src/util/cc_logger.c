



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
 * logger 内部状态。
 *
 * name 由 logger 拥有，level 控制过滤阈值，mutex 保证多线程日志不会交错输出。
 */
struct cc_logger {
    char *name;
    cc_log_level_t level;
    cc_mutex_t mutex;
};

static cc_mutex_t g_sink_mutex;
static cc_logger_sink_fn g_sink;
static void *g_sink_user_data;

/*
 * 懒初始化 sink 互斥锁（线程安全）。
 *
 * 在首次调用时创建 g_sink_mutex，后续调用直接返回。
 * 用于保护 g_sink_fn 和 g_sink_user_data 的并发访问。
 */
static void ensure_sink_mutex(void)
{
    if (!g_sink_mutex) {
        cc_mutex_create(&g_sink_mutex);
    }
}

/*
 * 创建 logger。
 *
 * name 为空时使用默认 "c-claw"；mutex 创建失败会释放已分配资源。当前 logger 输出到
 * stderr，后续可通过 port 扩展到 syslog、RTOS console 或 ring buffer。
 */
cc_result_t cc_logger_create(const char *name, cc_log_level_t level, cc_logger_t **out_logger)
{
    if (!out_logger) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null logger output");
    *out_logger = NULL;
    cc_logger_t *logger = calloc(1, sizeof(cc_logger_t));
    if (!logger) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create logger");

    logger->name = name ? cc_copy_string(name) : cc_copy_string("c-claw");
    logger->level = level;
    cc_result_t rc = cc_mutex_create(&logger->mutex);
    if (rc.code != CC_OK) {
        free(logger->name);
        free(logger);
        return rc;
    }
    *out_logger = logger;
    return cc_result_ok();
}


/*
 * 销毁 logger。
 *
 * 先在锁内释放 name，随后销毁 mutex。调用方必须保证没有其他线程还在 cc_logger_log()
 * 中使用该 logger。
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


/* 日志级别到固定字符串的映射，用于 stderr 前缀。 */
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


/*
 * 输出一条日志。
 *
 * 函数在 mutex 内完成过滤、格式化、脱敏和写 stderr，保证单条日志原子输出。格式化后
 * 先经过 cc_redact_secrets()，这是防止 api_key/token/password 泄漏的最后一道防线。
 */
void cc_logger_log(cc_logger_t *logger, cc_log_level_t level, const char *fmt, ...)
{
    if (!logger) return;

    cc_mutex_lock(logger->mutex);

    if (level < logger->level) {
        cc_mutex_unlock(logger->mutex);
        return;
    }

    char *logger_name = logger->name ? cc_copy_string(logger->name) : cc_copy_string("c-claw");
    cc_mutex_unlock(logger->mutex);

    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);



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
            vsnprintf(formatted, (size_t)needed + 1, fmt, args);
            char *redacted = cc_redact_secrets(formatted);
            const char *message = redacted ? redacted : formatted;
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
                    snprintf(line,
                             (size_t)line_needed + 1,
                             "[%s] [%s] [%s] %s",
                             time_buf,
                             level_string(level),
                             logger_name ? logger_name : "c-claw",
                             message);
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
    ensure_sink_mutex();
    if (g_sink_mutex) {
        cc_mutex_lock(g_sink_mutex);
        sink = g_sink;
        sink_user_data = g_sink_user_data;
        cc_mutex_unlock(g_sink_mutex);
    }

    if (sink && line) {
        sink(sink_user_data, level, logger_name ? logger_name : "c-claw", line);
    } else {
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
    }
    free(line);
    free(logger_name);
}


/* 动态调整日志过滤级别；加锁保证其他线程看到一致的 level。 */
void cc_logger_set_level(cc_logger_t *logger, cc_log_level_t level)
{
    if (!logger) return;
    cc_mutex_lock(logger->mutex);
    logger->level = level;
    cc_mutex_unlock(logger->mutex);
}

/*
 * 设置全局日志输出回调函数。
 *
 * 参数:
 *   sink      - 日志输出回调，接收格式化后的日志行
 *   user_data - 透传给回调的用户数据
 *
 * 线程安全：通过 g_sink_mutex 保护。设置后所有 cc_log 调用都将
 * 通过此回调输出。设为 NULL 可禁用日志输出。
 */
void cc_logger_set_sink(cc_logger_sink_fn sink, void *user_data)
{
    ensure_sink_mutex();
    if (!g_sink_mutex) {
        return;
    }

    cc_mutex_lock(g_sink_mutex);
    g_sink = sink;
    g_sink_user_data = user_data;
    cc_mutex_unlock(g_sink_mutex);
}
