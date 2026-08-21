



#ifndef CC_THREAD_H
#define CC_THREAD_H

#include "cc/core/cc_result.h"
#include <stddef.h>

/*
 * 平台线程入口函数。
 *
 * 返回值透传给 join 所在平台；多数 SDK worker 不使用返回值。arg 由创建线程的调用方
 * 管理，线程函数必须明确自己的生命周期约定。
 */
typedef void *(*cc_thread_fn_t)(void *arg);

/* 不透明线程句柄；POSIX/FreeRTOS/Windows 实现可映射到不同底层类型。 */
typedef void *cc_thread_t;

/* 不透明互斥锁句柄。 */
typedef void *cc_mutex_t;

/* 不透明条件变量句柄。 */
typedef void *cc_cond_t;

/* 创建线程；out_thread 成功后必须 cc_thread_join() 或由平台实现定义回收方式。 */
cc_result_t cc_thread_create(cc_thread_fn_t fn, void *arg, cc_thread_t *out_thread);

/* 等待线程结束并释放线程句柄相关资源。 */
cc_result_t cc_thread_join(cc_thread_t thread);

/* 创建互斥锁；用于 event bus、registry、queue 等共享状态保护。 */
cc_result_t cc_mutex_create(cc_mutex_t *out_mutex);

/* 销毁互斥锁；调用前必须保证没有线程持有或等待该锁。 */
void cc_mutex_destroy(cc_mutex_t mutex);

/* 加锁；当前接口不返回错误，平台实现应在失败时采取明确策略。 */
void cc_mutex_lock(cc_mutex_t mutex);

/* 解锁；必须与 lock 成对使用。 */
void cc_mutex_unlock(cc_mutex_t mutex);

/* 创建条件变量；常与 mutex 配合实现队列、flush 和取消等待。 */
cc_result_t cc_cond_create(cc_cond_t *out_cond);

/* 销毁条件变量；调用前应确保没有等待者。 */
void cc_cond_destroy(cc_cond_t cond);

/* 等待条件变量；调用时 mutex 必须已加锁，返回时仍持有 mutex。 */
void cc_cond_wait(cc_cond_t cond, cc_mutex_t mutex);

/* 带超时等待条件变量；返回值由平台实现映射，通常 0 表示被唤醒。 */
int cc_cond_timedwait(cc_cond_t cond, cc_mutex_t mutex, int timeout_ms);

/* 唤醒一个等待线程。 */
void cc_cond_signal(cc_cond_t cond);

/* 唤醒所有等待线程；用于 shutdown/flush/cancel 等广播状态变化。 */
void cc_cond_broadcast(cc_cond_t cond);

#endif
