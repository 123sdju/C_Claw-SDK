



#include "cc/ports/cc_thread.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdint.h>
#include <stdlib.h>

#ifndef CC_ESP32_THREAD_STACK_BYTES
#define CC_ESP32_THREAD_STACK_BYTES 8192U
#endif

#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && configSUPPORT_STATIC_ALLOCATION
#define CC_ESP32_THREAD_STACK_IN_PSRAM 1
#else
#define CC_ESP32_THREAD_STACK_IN_PSRAM 0
#endif

/*
 * 创建 FreeRTOS 任务，根据 PSRAM 配置选择栈分配策略。
 *
 * 优先使用 MALLOC_CAP_SPIRAM 分配（节省内部 RAM），回退到内部 RAM。
 * 参数：task - 任务函数；name - 任务名称；stack_bytes - 栈字节数；arg - 任务参数；priority - FreeRTOS 优先级；out_handle - 输出任务句柄。
 * 返回：pdPASS 表示成功。
 */
static BaseType_t cc_esp32_task_create(TaskFunction_t task,
                                       const char *name,
                                       uint32_t stack_bytes,
                                       void *arg,
                                       UBaseType_t priority,
                                       TaskHandle_t *out_handle)
{
#if CC_ESP32_THREAD_STACK_IN_PSRAM
    BaseType_t ok = xTaskCreateWithCaps(task,
                                        name,
                                        stack_bytes,
                                        arg,
                                        priority,
                                        out_handle,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok == pdPASS) return ok;

    return xTaskCreateWithCaps(task,
                               name,
                               stack_bytes,
                               arg,
                               priority,
                               out_handle,
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return xTaskCreate(task, name, stack_bytes, arg, priority, out_handle);
#endif
}

/*
 * 删除当前 FreeRTOS 任务并释放栈内存。
 *
 * 根据 PSRAM 配置选择 vTaskDeleteWithCaps 或 vTaskDelete，确保 DIERS 栈内存正确回收。
 */
static void cc_esp32_task_delete_self(void)
{
#if CC_ESP32_THREAD_STACK_IN_PSRAM
    vTaskDeleteWithCaps(NULL);
#else
    vTaskDelete(NULL);
#endif
}

/*
 * ESP32 thread 包装对象。
 *
 * FreeRTOS task 本身不能像 pthread 那样直接 join，因此用二值信号量 done 表示任务函数
 * 已结束。join 等待 done 后释放包装对象。
 */
typedef struct cc_esp32_thread {
    cc_thread_fn_t fn;
    void *arg;
    SemaphoreHandle_t done;
} cc_esp32_thread_t;

/*
 * FreeRTOS task 入口。
 *
 * 调用 SDK 线程函数后释放 done 信号量通知 join，再删除当前 task。arg 指向堆上的
 * cc_esp32_thread_t。
 */
static void cc_esp32_thread_entry(void *arg)
{
    cc_esp32_thread_t *thread = (cc_esp32_thread_t *)arg;
    thread->fn(thread->arg);
    xSemaphoreGive(thread->done);
    cc_esp32_task_delete_self();
}

/*
 * 创建 ESP32 FreeRTOS task。
 *
 * cc_thread_t 返回包装对象指针；栈大小当前固定 8192，嵌入式产品可按实际 profile 调整。
 */
cc_result_t cc_thread_create(cc_thread_fn_t fn, void *arg, cc_thread_t *out_thread)
{
    if (!fn || !out_thread)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid thread arguments");

    cc_esp32_thread_t *thread = (cc_esp32_thread_t *)calloc(1, sizeof(cc_esp32_thread_t));
    if (!thread) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate thread");

    thread->fn = fn;
    thread->arg = arg;
    thread->done = xSemaphoreCreateBinary();
    if (!thread->done) {
        free(thread);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create thread semaphore");
    }

    BaseType_t ok = cc_esp32_task_create(
        cc_esp32_thread_entry,
        "cclaw_thread",
        CC_ESP32_THREAD_STACK_BYTES,
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

/*
 * 等待 ESP32 task 完成。
 *
 * join 通过 done 信号量实现，只能调用一次；成功后删除信号量并释放包装对象。
 */
cc_result_t cc_thread_join(cc_thread_t handle)
{
    cc_esp32_thread_t *thread = (cc_esp32_thread_t *)handle;
    if (!thread) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid thread handle");
    xSemaphoreTake(thread->done, portMAX_DELAY);
    vSemaphoreDelete(thread->done);
    free(thread);
    return cc_result_ok();
}

/* 创建 FreeRTOS mutex；返回的不透明句柄由调用方 destroy。 */
cc_result_t cc_mutex_create(cc_mutex_t *out_mutex)
{
    if (!out_mutex) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid mutex output");
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (!mutex) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create mutex");
    *out_mutex = mutex;
    return cc_result_ok();
}

/* 删除 FreeRTOS mutex；调用方必须保证没有任务仍在等待或持有。 */
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
 * FreeRTOS 没有 pthread cond，这里用二值信号量近似；它不具备真正 broadcast 语义，适合
 * 简单等待/唤醒场景。
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

/*
 * 条件等待。
 *
 * 进入等待前释放 mutex，被唤醒后重新获取 mutex，模拟 pthread_cond_wait 的基本语义。
 */
void cc_cond_wait(cc_cond_t cond, cc_mutex_t mutex)
{
    if (!cond || !mutex) return;
    xSemaphoreGive((SemaphoreHandle_t)mutex);
    xSemaphoreTake((SemaphoreHandle_t)cond, portMAX_DELAY);
    xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY);
}

/* 带超时条件等待；timeout_ms <= 0 表示永久等待。 */
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

/* FreeRTOS 二值信号量无法真正广播，这里退化为一次 give。 */
void cc_cond_broadcast(cc_cond_t cond)
{
    if (cond) xSemaphoreGive((SemaphoreHandle_t)cond);
}
#else
#error "cc_esp32_thread.c must be built under ESP-IDF"
#endif
