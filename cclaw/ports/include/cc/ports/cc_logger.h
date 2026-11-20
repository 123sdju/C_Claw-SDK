



#ifndef CC_LOGGER_PORT_H
#define CC_LOGGER_PORT_H

#include "cc/core/cc_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 日志级别。
 *
 * 从 TRACE 到 FATAL 递增，logger 实现可以按当前 level 过滤。日志 payload 进入 event
 * bus 或外部系统前应通过 redaction，避免泄漏 token/password。
 */
typedef enum cc_log_level {
    CC_LOG_TRACE,

    CC_LOG_DEBUG,

    CC_LOG_INFO,

    CC_LOG_WARN,

    CC_LOG_ERROR,

    CC_LOG_FATAL

} cc_log_level_t;

/* 不透明 logger 句柄；内部实现负责并发保护和输出目标。 */
typedef struct cc_logger cc_logger_t;

/**
 * @brief 同步消费一条已经格式化的日志，允许不同线程并发调用。
 * @param[in,out] user_data 注册者保管的上下文借用，覆盖本次回调。
 * @param[in] level 本条日志级别。
 * @param[in] logger_name 回调期间有效的实例名称借用，异步保留需复制。
 * @param[in] message 回调期间有效的完整行借用，正文经过脱敏或静态错误降级，异步保留需复制。
 * @post 回调负责完成自身投递和并发协调，返回后上述字符串由 logger 回收。
 */
typedef void (*cc_logger_sink_fn)(
    void *user_data,
    cc_log_level_t level,
    const char *logger_name,
    const char *message);

/**
 * @brief 创建拥有独立名称及 mutex 的 logger。
 * @param[in] name 可选名称借用，当前实现复制，NULL 使用 c-claw。
 * @param[in] level 初始过滤阈值。
 * @param[out] out_logger 必填独立输出槽，先置 NULL，成功实例由调用者最终销毁。
 * @return CC_OK 或参数、对象/名称分配及锁错误，错误结果的动态成员由调用者消费。
 */
cc_result_t cc_logger_create(const char *name, cc_log_level_t level, cc_logger_t **out_logger);

/**
 * @brief 消费实例及其名称、mutex。
 * @pre owner 已关闭入口、结束在途访问和锁等待，销毁后禁止继续使用该句柄。
 * @param[in,out] logger 可选拥有实例，NULL 为空操作。
 * @post 实例失效，全局 sink 的资源仍由注册者保管。
 */
void cc_logger_destroy(cc_logger_t *logger);

/**
 * @brief 按实例阈值过滤并同步输出格式化、脱敏正文。
 * @param[in,out] logger 可选稳定借用实例，NULL 不输出。
 * @param[in] level 本条级别。
 * @param[in] fmt 可选 printf 格式串，NULL 不输出，覆盖两次读取。
 * @param[in] ... 格式参数，引用内容在格式化期间存活且稳定。
 * @post 临时资源内部清理，分配、格式化或脱敏失败使用诊断降级，sink 可被并发及重入调用。
 */
void cc_logger_log(cc_logger_t *logger, cc_log_level_t level, const char *fmt, ...);

/**
 * @brief 通过实例锁更新过滤阈值。
 * @param[in,out] logger 可选稳定借用实例，NULL 为空操作。
 * @param[in] level 新阈值，已接纳日志仍按旧检查完成。
 * @post 后续读取阈值的日志看到新值，外层继续协调销毁。
 */
void cc_logger_set_level(cc_logger_t *logger, cc_log_level_t level);

/**
 * @brief 成对替换全局同步日志 sink 和上下文。
 * @param[in] sink 可选回调，NULL 恢复 stderr。
 * @param[in,out] user_data 注册者拥有的借用上下文，覆盖旧快照可能触发的回调。
 * @post 新快照读取新注册，进程期 mutex 创建失败时保留原值，void 接口不返回错误。
 * @note 替换不等待旧回调退出，注册者在结束旧 user_data 寿命前协调所有已取快照调用。
 */
void cc_logger_set_sink(cc_logger_sink_fn sink, void *user_data);

#ifdef __cplusplus
}
#endif

#endif
