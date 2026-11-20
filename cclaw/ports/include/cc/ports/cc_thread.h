



#ifndef CC_THREAD_H
#define CC_THREAD_H

#include "cc/core/cc_result.h"
#include <stddef.h>

/**
 * @brief 在新线程或任务中执行 SDK 工作，入口可能先于 create 返回。
 * @param[in,out] arg 创建者交付的借用上下文，可为 NULL，拥有者覆盖入口执行期。
 * @return 当前四个平台均忽略该指针，返回对象需由入口自身清理或通过另有拥有权约定的上下文交接。
 */
typedef void *(*cc_thread_fn_t)(void *arg);

/** @brief create 交付的拥有线程句柄，唯一拥有者成功 join 一次，失败时保留句柄。 */
typedef void *cc_thread_t;

/** @brief 不透明 mutex，普通创建由调用者 destroy，init_once 安装的锁持续进程期。 */
typedef void *cc_mutex_t;

/** @brief 拥有的条件变量句柄，拥有者排空等待和通知后 destroy。 */
typedef void *cc_cond_t;

/**
 * @brief 创建可 join 的平台线程或 FreeRTOS 任务。
 * @param[in] fn 必需入口，覆盖执行期，可能在 create 返回前开始。
 * @param[in,out] arg 借用上下文，允许 NULL，覆盖入口返回。
 * @param[out] out_thread 必需无已有资源输出，基础参数有效后置 NULL，成功交付拥有句柄。
 * @return 成功为 CC_OK，空入口/输出为 INVALID_ARGUMENT，分配失败为 OUT_OF_MEMORY，原生创建失败为 PLATFORM，失败回收本次资源。
 * @note RTOS 首次创建还可能安装进程期完成锁，后续创建失败继续保留该锁。
 */
cc_result_t cc_thread_create(cc_thread_fn_t fn, void *arg, cc_thread_t *out_thread);

/**
 * @brief 等待用户入口及句柄最后使用结束，成功后消费拥有句柄。
 * @param[in,out] thread 尚未成功 join 的唯一拥有句柄，NULL 返回参数错误。
 * @return 成功为 CC_OK，NULL 为 INVALID_ARGUMENT，自 join 或原生等待/关闭失败为 PLATFORM，失败保留句柄可由外部拥有者重试。
 * @note 外层排除并发 join 和句柄查询。POSIX/Windows 等待原生线程退出，RTOS 通过完成通知和进程期锁保证 SDK 访问退场，kernel/Idle 的 TCB/栈回收可随后完成。
 */
cc_result_t cc_thread_join(cc_thread_t thread);

/**
 * @brief 判断当前执行者是否为指定的未结束 SDK 线程。
 * @param[in] thread 可选存活借用句柄，拥有者排除并发成功 join。
 * @return 身份匹配为非零，NULL、已结束或不匹配为零，无资源交接。
 */
int cc_thread_is_current(cc_thread_t thread);

/**
 * @brief 创建拥有的进程内互斥原语。
 * @param[out] out_mutex 必需无已有资源槽，进入后置 NULL，成功供 destroy。
 * @return 成功为 CC_OK，NULL 为 INVALID_ARGUMENT，分配失败为 OUT_OF_MEMORY，原生初始化失败为 PLATFORM。
 * @note POSIX/Windows 可递归，ESP32/通用 FreeRTOS 使用普通 mutex，共享调用只依赖一次持有并配对归还。
 */
cc_result_t cc_mutex_create(cc_mutex_t *out_mutex);
/**
 * @brief 为静态零初始化槽并发安装一次 mutex，失败允许后续重试。
 * @param[in,out] slot 必需进程期槽，所有初始化均经本入口，安装后不重置或销毁。
 * @return 已有或安装成功为 CC_OK，NULL 为 INVALID_ARGUMENT，候选创建错误原样返回。
 * @note 安装槽和 mutex 持续到进程结束，平台门锁保护初始化，后续共享数据仍需取得已安装 mutex。
 */
cc_result_t cc_mutex_init_once(cc_mutex_t *slot);

/**
 * @brief 释放普通创建的拥有 mutex。
 * @pre 已关闭新使用入口并排空持有者、等待者及句柄借用，init_once 锁沿进程期保留。
 * @param[in,out] mutex 消费拥有句柄，NULL 跳过。
 * @post 非 NULL 对象及借用失效，原生 destroy 状态无返回通道。
 */
void cc_mutex_destroy(cc_mutex_t mutex);

/**
 * @brief 阻塞取得 mutex。
 * @param[in,out] mutex 存活借用句柄，NULL 跳过，普通 RTOS mutex 当前任务尚未持有。
 * @post 正常成功后当前线程拥有一次持有，需配对 unlock，原生失败无返回通道。
 */
void cc_mutex_lock(cc_mutex_t mutex);

/**
 * @brief 归还当前执行者的一次 mutex 持有。
 * @param[in,out] mutex 当前执行者持有的存活锁，NULL 跳过。
 * @post 正常解锁后该次持有结束，原生状态无返回通道。
 */
void cc_mutex_unlock(cc_mutex_t mutex);

/**
 * @brief 创建拥有的条件变量或逐等待者通知包装。
 * @param[out] out_cond 必需无已有资源槽，进入后置 NULL，成功供 destroy。
 * @return 成功为 CC_OK，NULL 为 INVALID_ARGUMENT，分配失败为 OUT_OF_MEMORY，原生初始化失败为 PLATFORM。
 */
cc_result_t cc_cond_create(cc_cond_t *out_cond);

/**
 * @brief 释放条件变量及其平台资源。
 * @pre 已排空等待、signal、broadcast 及其他句柄借用。
 * @param[in,out] cond 消费拥有句柄，NULL 跳过。
 * @post 非 NULL 条件变量及借用失效。
 */
void cc_cond_destroy(cc_cond_t cond);

/**
 * @brief 登记等待并释放谓词锁，通知后重新取得锁。
 * @pre 当前执行者恰好持有 mutex 一次，同时等待者使用相同谓词锁，返回后循环检查谓词。
 * @param[in,out] cond 存活借用条件变量，NULL 跳过。
 * @param[in,out] mutex 当前执行者持有的存活锁，NULL 跳过。
 * @post 正常等待返回时仍持锁，允许伪唤醒。RTOS 节点 OOM 时以短延迟释放/重取锁后返回，原生错误无返回通道。
 */
void cc_cond_wait(cc_cond_t cond, cc_mutex_t mutex);

/**
 * @brief 在本次预算内等待通知并重新取得谓词锁。
 * @pre 当前执行者恰好持有 mutex 一次，返回后在锁内重查谓词。
 * @param[in,out] cond 存活借用条件变量，NULL 返回零。
 * @param[in,out] mutex 当前执行者持有的存活锁，NULL 返回零。
 * @param[in] timeout_ms 正数为毫秒预算，非正数表示无限等待。
 * @return 原生通知等待成功为一，超时、分配或原生错误为零，返回值不替代谓词检查。
 * @note 重新持锁的时间额外取决于持有者。POSIX 尽量采用 MONOTONIC，否则 REALTIME 受墙钟影响；RTOS 正预算至少一 tick，超长有限预算受 tick 表示范围限制且避免无限哨兵。
 */
int cc_cond_timedwait(cc_cond_t cond, cc_mutex_t mutex, int timeout_ms);

/**
 * @brief 通知一个当时已登记等待者。
 * @param[in,out] cond 存活借用条件变量，NULL 跳过。
 * @post 无等待者时不保存通知，等待者重新取得谓词锁后检查状态。
 */
void cc_cond_signal(cc_cond_t cond);

/**
 * @brief 通知当时全部已登记等待者。
 * @param[in,out] cond 存活借用条件变量，NULL 跳过。
 * @post 每个已登记者可重新竞争谓词锁，后续新等待者不消费旧广播，无等待顺序保证。
 */
void cc_cond_broadcast(cc_cond_t cond);

#endif
