



#include "cc/ports/cc_thread.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <stdlib.h>

#ifndef CCLAW_FREERTOS_TASK_STACK_WORDS
#define CCLAW_FREERTOS_TASK_STACK_WORDS 2048
#endif

/*
 * FreeRTOS thread 包装对象。
 *
 * FreeRTOS task 不提供 pthread 风格 join，因此用 done 信号量表示任务函数已经返回。
 * 栈大小通过 CCLAW_FREERTOS_TASK_STACK_WORDS 宏配置，方便 MCU profile 做内存预算。
 */
typedef struct cc_freertos_thread {
    cc_thread_fn_t fn;
    void *arg;
    SemaphoreHandle_t done;
} cc_freertos_thread_t;

/* FreeRTOS task 入口：调用 SDK 线程函数、通知 done、删除当前 task。 */
static void cc_freertos_thread_entry(void *arg)
{
    cc_freertos_thread_t *thread = (cc_freertos_thread_t *)arg;
    (void)thread->fn(thread->arg);
    xSemaphoreGive(thread->done);
    vTaskDelete(NULL);
}

/*
 * 创建 FreeRTOS task。
 *
 * 返回的 cc_thread_t 是包装对象指针，join 后释放；任务优先级当前为 idle+1。
 */
cc_result_t cc_thread_create(cc_thread_fn_t fn, void *arg, cc_thread_t *out_thread)
{
    if (!fn || !out_thread)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid thread arguments");

    cc_freertos_thread_t *thread = calloc(1, sizeof(cc_freertos_thread_t));
    if (!thread) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate thread");

    thread->fn = fn;
    thread->arg = arg;
    thread->done = xSemaphoreCreateBinary();
    if (!thread->done) {
        free(thread);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create thread semaphore");
    }

    BaseType_t ok = xTaskCreate(
        cc_freertos_thread_entry,
        "cclaw",
        CCLAW_FREERTOS_TASK_STACK_WORDS,
        thread,
        tskIDLE_PRIORITY + 1,
        NULL);
    if (ok != pdPASS) {
        vSemaphoreDelete(thread->done);
        free(thread);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create FreeRTOS task");
    }

    *out_thread = thread;
    return cc_result_ok();
}

/* 等待 task 完成并释放包装对象；join 只能调用一次。 */
cc_result_t cc_thread_join(cc_thread_t handle)
{
    cc_freertos_thread_t *thread = (cc_freertos_thread_t *)handle;
    if (!thread) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid thread handle");
    xSemaphoreTake(thread->done, portMAX_DELAY);
    vSemaphoreDelete(thread->done);
    free(thread);
    return cc_result_ok();
}

/* 创建 FreeRTOS mutex。 */
cc_result_t cc_mutex_create(cc_mutex_t *out_mutex)
{
    if (!out_mutex) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid mutex output");
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (!mutex) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create mutex");
    *out_mutex = mutex;
    return cc_result_ok();
}

/* 删除 FreeRTOS mutex；调用方必须保证无人持有。 */
void cc_mutex_destroy(cc_mutex_t mutex)
{
    if (mutex) vSemaphoreDelete((SemaphoreHandle_t)mutex);
}

/* 阻塞获取 mutex。 */
void cc_mutex_lock(cc_mutex_t mutex)
{
    if (mutex) xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY);
}

/* 释放 mutex。 */
void cc_mutex_unlock(cc_mutex_t mutex)
{
    if (mutex) xSemaphoreGive((SemaphoreHandle_t)mutex);
}

/*
 * 创建条件变量替代物。
 *
 * 用二值信号量模拟条件变量，适合简单等待/唤醒；不具备完整 pthread cond broadcast 语义。
 */
cc_result_t cc_cond_create(cc_cond_t *out_cond)
{
    if (!out_cond) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid condition output");
    SemaphoreHandle_t cond = xSemaphoreCreateBinary();
    if (!cond) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create condition semaphore");
    *out_cond = cond;
    return cc_result_ok();
}

/* 删除条件变量信号量。 */
void cc_cond_destroy(cc_cond_t cond)
{
    if (cond) vSemaphoreDelete((SemaphoreHandle_t)cond);
}

/* 等待条件：释放 mutex，等待信号量，再重新获取 mutex。 */
void cc_cond_wait(cc_cond_t cond, cc_mutex_t mutex)
{
    if (!cond || !mutex) return;
    xSemaphoreGive((SemaphoreHandle_t)mutex);
    xSemaphoreTake((SemaphoreHandle_t)cond, portMAX_DELAY);
    xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY);
}

/* 带超时等待条件，timeout_ms <= 0 表示永久等待。 */
int cc_cond_timedwait(cc_cond_t cond, cc_mutex_t mutex, int timeout_ms)
{
    if (!cond || !mutex) return 0;
    TickType_t ticks = timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
    xSemaphoreGive((SemaphoreHandle_t)mutex);
    BaseType_t ok = xSemaphoreTake((SemaphoreHandle_t)cond, ticks);
    xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY);
    return ok == pdTRUE ? 1 : 0;
}

/* 唤醒一个等待者。 */
void cc_cond_signal(cc_cond_t cond)
{
    if (cond) xSemaphoreGive((SemaphoreHandle_t)cond);
}

/* FreeRTOS 二值信号量无法广播，这里退化为 signal。 */
void cc_cond_broadcast(cc_cond_t cond)
{
    if (cond) xSemaphoreGive((SemaphoreHandle_t)cond);
}
