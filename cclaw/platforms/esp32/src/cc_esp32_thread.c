



#include "cc/ports/cc_thread.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/** @brief 进程期完成锁，创建首个线程前通过 init_once 安装，保留至进程结束。 */
static cc_mutex_t thread_completion_mutex;

/**
 * @brief 在短临界区中安装一次普通 FreeRTOS mutex，竞争失败副本在锁外删除。
 * @param[in,out] slot 必需静态零初始化槽，安装后不得重置或销毁。
 * @return 已就绪或安装成功为 CC_OK，NULL 为 INVALID_ARGUMENT，分配错误原样返回且允许重试。
 * @note 候选创建不持 portMUX，所有槽访问通过本入口的同一静态门保护。
 */
cc_result_t cc_mutex_init_once(cc_mutex_t *slot)
{
    if (!slot) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null mutex slot");
    static portMUX_TYPE gate = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&gate);
    int ready = *slot != NULL;
    portEXIT_CRITICAL(&gate);
    if (ready) return cc_result_ok();
    cc_mutex_t candidate = NULL;
    cc_result_t rc = cc_mutex_create(&candidate);
    if (rc.code != CC_OK) return rc;
    portENTER_CRITICAL(&gate);
    if (!*slot) { *slot = candidate; candidate = NULL; }
    portEXIT_CRITICAL(&gate);
    if (candidate) cc_mutex_destroy(candidate);
    return rc;
}

/** @brief 单次条件等待拥有的节点及私有信号量，链关系由条件变量 gate 保护。 */
typedef struct cc_esp32_cond_waiter {
    SemaphoreHandle_t signal; /**< 本次等待独占的二值信号量。 */
    struct cc_esp32_cond_waiter *next; /**< gate 保护的等待链借用。 */
} cc_esp32_cond_waiter_t;

/** @brief 条件变量拥有的门锁和当前等待链，节点由各等待调用回收。 */
typedef struct {
    SemaphoreHandle_t gate; /**< 保护链及 signal/broadcast 的普通 mutex。 */
    cc_esp32_cond_waiter_t *waiters; /**< 借用的已登记等待节点。 */
} cc_esp32_cond_t;

#ifndef CC_ESP32_THREAD_STACK_BYTES
#define CC_ESP32_THREAD_STACK_BYTES 8192U
#endif

#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && configSUPPORT_STATIC_ALLOCATION
#define CC_ESP32_THREAD_STACK_IN_PSRAM 1
#else
#define CC_ESP32_THREAD_STACK_IN_PSRAM 0
#endif

/**
 * @brief 按当前 IDF 配置创建任务，允许外部栈时先尝试 PSRAM 再回退内部内存。
 * @param[in] task 必需原生任务入口，覆盖任务寿命。
 * @param[in] name 同步借用任务名，由 IDF 创建过程复制。
 * @param[in] stack_bytes 栈预算，单位为 IDF API 的字节。
 * @param[in,out] arg 借用任务上下文，覆盖入口执行期。
 * @param[in] priority FreeRTOS 优先级。
 * @param[out] out_handle 必需空资源任务句柄槽，成功由对应删除路径回收。
 * @return pdPASS 表示成功，其余为任务创建失败，调用者清理其线程包装。
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

/**
 * @brief 按任务创建方式请求删除当前任务。
 * @post 当前任务不再继续执行，栈和 TCB 由 IDF 删除机制回收，WithCaps 自删可能借助清理任务。
 * @note 本函数不访问 SDK thread 包装，用户入口与完成通知的退场屏障先行完成。
 */
static void cc_esp32_task_delete_self(void)
{
#if CC_ESP32_THREAD_STACK_IN_PSRAM
    vTaskDeleteWithCaps(NULL);
#else
    vTaskDelete(NULL);
#endif
}

/** @brief join 拥有的 SDK 线程包装，完成标志和完成通知由进程期完成锁保护。 */
typedef struct cc_esp32_thread {
    cc_thread_fn_t fn; /**< 借用 SDK 入口。 */
    void *arg; /**< 原创建者保管的入口上下文。 */
    SemaphoreHandle_t done; /**< 包装拥有的完成通知。 */
    TaskHandle_t task; /**< 原生任务身份，自删后不再用来认定当前线程。 */
    int completed; /**< 用户入口已返回，访问持完成锁。 */
} cc_esp32_thread_t;

/**
 * @brief 调用 SDK 入口，在线程完成锁内通知 join，随后仅执行原生自删。
 * @param[in,out] arg 借用线程包装，用户入口和最后通知结束前由 join 保活。
 * @post 用户返回指针被忽略，completed 置一并发送 done，解完成锁后不再访问包装或 done。
 * @note join 取得同一完成锁后才能删除 done，覆盖 xSemaphoreGive 唤醒接收者后仍执行的原生尾部。
 */
static void cc_esp32_thread_entry(void *arg)
{
    cc_esp32_thread_t *thread = (cc_esp32_thread_t *)arg;
    (void)thread->fn(thread->arg);
    cc_mutex_lock(thread_completion_mutex);
    thread->completed = 1;
    xSemaphoreGive(thread->done);
    cc_mutex_unlock(thread_completion_mutex);
    cc_esp32_task_delete_self();
}

/**
 * @brief 准备完成屏障并创建 SDK 任务，栈按 CC_ESP32_THREAD_STACK_BYTES 字节配置。
 * @param[in] fn 必需借用入口，可能在创建函数返回前执行。
 * @param[in,out] arg 借用入口上下文，覆盖用户函数返回。
 * @param[out] out_thread 必需空资源槽，基础参数有效后置 NULL，成功交付唯一 join 拥有的包装。
 * @return 成功为 CC_OK，空参数为 INVALID_ARGUMENT，包装/信号量分配失败为 OUT_OF_MEMORY，任务创建失败为 PLATFORM，完成锁错误原样返回。
 * @note 失败回收本次包装和 done，已安装完成锁为进程期资源，后续创建失败也保留该锁。
 */
cc_result_t cc_thread_create(cc_thread_fn_t fn, void *arg, cc_thread_t *out_thread)
{
    if (!fn || !out_thread)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid thread arguments");
    *out_thread = NULL;
    cc_result_t rc = cc_mutex_init_once(&thread_completion_mutex);
    if (rc.code != CC_OK) return rc;

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
        &thread->task);
    if (ok != pdPASS) {
        vSemaphoreDelete(thread->done);
        free(thread);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to create FreeRTOS task");
    }

    *out_thread = thread;
    return cc_result_ok();
}

/**
 * @brief 等待用户入口与完成通知原语退出，再消费线程包装。
 * @param[in,out] handle 唯一拥有且未成功 join 的句柄，NULL 为参数错误。
 * @return 完成通知及屏障通过为 CC_OK 并释放包装，等待失败或自 join 为 PLATFORM 且保留句柄。
 * @note 外层排除并发 join/身份查询，成功保证 SDK 包装最后访问结束，原生自删及栈/TCB 回收可能随后完成。
 */
cc_result_t cc_thread_join(cc_thread_t handle)
{
    cc_esp32_thread_t *thread = (cc_esp32_thread_t *)handle;
    if (!thread) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid thread handle");
    if (cc_thread_is_current(handle))
        return cc_result_error(CC_ERR_PLATFORM, "Cannot join the current thread");
    if (xSemaphoreTake(thread->done, portMAX_DELAY) != pdTRUE)
        return cc_result_error(CC_ERR_PLATFORM, "Failed to wait for thread completion");
    cc_mutex_lock(thread_completion_mutex);
    cc_mutex_unlock(thread_completion_mutex);
    vSemaphoreDelete(thread->done);
    free(thread);
    return cc_result_ok();
}

/**
 * @brief 在完成锁内比较未完成任务的原生身份，避免自删后的 TCB 地址复用别名。
 * @param[in] handle 可选存活借用包装，拥有者排除并发成功 join。
 * @return 未完成且当前任务匹配为一，NULL、已完成或不匹配为零。
 */
int cc_thread_is_current(cc_thread_t handle)
{
    cc_esp32_thread_t *thread = (cc_esp32_thread_t *)handle;
    if (!thread) return 0;
    cc_mutex_lock(thread_completion_mutex);
    int matches = !thread->completed && thread->task == xTaskGetCurrentTaskHandle();
    cc_mutex_unlock(thread_completion_mutex);
    return matches;
}

/**
 * @brief 创建普通非递归 FreeRTOS mutex。
 * @param[out] out_mutex 必需空资源槽，进入后置 NULL，成功由调用者 destroy。
 * @return 成功为 CC_OK，NULL 为 INVALID_ARGUMENT，原生分配失败为 OUT_OF_MEMORY。
 */
cc_result_t cc_mutex_create(cc_mutex_t *out_mutex)
{
    if (!out_mutex) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid mutex output");
    *out_mutex = NULL;
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (!mutex) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create mutex");
    *out_mutex = mutex;
    return cc_result_ok();
}

/**
 * @brief 删除拥有的普通 mutex。
 * @pre 已排空持有、等待和其他借用，不用于进程期 init_once 锁。
 * @param[in,out] mutex 消费拥有句柄，NULL 跳过。
 * @post 非 NULL 原生对象及借用失效。
 */
void cc_mutex_destroy(cc_mutex_t mutex)
{
    if (mutex) vSemaphoreDelete((SemaphoreHandle_t)mutex);
}

/**
 * @brief 以 portMAX_DELAY 取得普通非递归 mutex。
 * @param[in,out] mutex 存活借用句柄，NULL 跳过，当前任务尚未持有。
 * @post 成功取得后由当前任务配对解锁，原生 Take 状态无返回通道。
 */
void cc_mutex_lock(cc_mutex_t mutex)
{
    if (mutex) xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY);
}

/**
 * @brief 归还当前任务持有的普通 mutex。
 * @param[in,out] mutex 存活借用锁，NULL 跳过。
 * @post 已调用原生 Give，状态无返回通道。
 */
void cc_mutex_unlock(cc_mutex_t mutex)
{
    if (mutex) xSemaphoreGive((SemaphoreHandle_t)mutex);
}

/**
 * @brief 创建拥有 gate 和逐等待者链的条件变量。
 * @param[out] out_cond 必需空资源槽，进入后置 NULL，成功由调用者 destroy。
 * @return 成功为 CC_OK，NULL 为 INVALID_ARGUMENT，包装或 gate 分配失败为 OUT_OF_MEMORY，失败清理本次资源。
 * @note 每次等待另分配节点和私有信号量，通知只面向当时已登记的等待者。
 */
cc_result_t cc_cond_create(cc_cond_t *out_cond)
{
    if (!out_cond) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid condition output");
    *out_cond = NULL;
    cc_esp32_cond_t *cond = calloc(1, sizeof(*cond));
    if (!cond) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate condition state");
    cond->gate = xSemaphoreCreateMutex();
    if (!cond->gate) {
        free(cond);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create condition semaphores");
    }
    *out_cond = cond;
    return cc_result_ok();
}

/**
 * @brief 删除条件变量 gate 并释放状态。
 * @pre 已排空等待、signal、broadcast 及其他借用。
 * @param[in,out] cond 消费拥有句柄，NULL 跳过。
 * @post 原生 gate 和包装失效，等待节点已由各等待调用清理。
 */
void cc_cond_destroy(cc_cond_t cond)
{
    cc_esp32_cond_t *state = (cc_esp32_cond_t *)cond;
    if (!state) return;
    vSemaphoreDelete(state->gate);
    free(state);
}

/**
 * @brief 分配单次等待节点及其二值信号量。
 * @return 成功交付拥有节点供 destroy，分配失败回收本次部分资源并返回 NULL。
 */
static cc_esp32_cond_waiter_t *cc_cond_waiter_create(void)
{
    cc_esp32_cond_waiter_t *waiter = calloc(1, sizeof(*waiter));
    if (!waiter) return NULL;
    waiter->signal = xSemaphoreCreateBinary();
    if (!waiter->signal) {
        free(waiter);
        return NULL;
    }
    return waiter;
}

/**
 * @brief 在条件变量 gate 内登记尚未释放谓词锁的等待节点。
 * @param[in,out] state 必需存活条件变量。
 * @param[in,out] waiter 必需未登记节点，链关系交 state 借用，节点仍由等待调用拥有。
 * @post 节点位于等待链头，后续释放谓词锁前不会丢失已送达通知。
 */
static void cc_cond_waiter_register(cc_esp32_cond_t *state,
                                   cc_esp32_cond_waiter_t *waiter)
{
    xSemaphoreTake(state->gate, portMAX_DELAY);
    waiter->next = state->waiters;
    state->waiters = waiter;
    xSemaphoreGive(state->gate);
}

/**
 * @brief 从已持 gate 的等待链移除指定节点。
 * @pre 当前任务持 state->gate。
 * @param[in,out] state 必需存活条件变量。
 * @param[in,out] waiter 本次等待拥有节点，可能已被 signal/broadcast 摘链。
 * @post 命中时摘链，节点 next 清零，节点所有权不变。
 */
static void cc_cond_waiter_remove(cc_esp32_cond_t *state,
                                 cc_esp32_cond_waiter_t *waiter)
{
    cc_esp32_cond_waiter_t **link = &state->waiters;
    while (*link && *link != waiter) link = &(*link)->next;
    if (*link == waiter) *link = waiter->next;
    waiter->next = NULL;
}

/**
 * @brief 删除已摘链节点及其私有信号量。
 * @pre 已跨过 gate 屏障，通知者不会再使用节点或其信号量。
 * @param[in,out] waiter 消费拥有节点，NULL 跳过。
 * @post 节点和信号量失效。
 */
static void cc_cond_waiter_destroy(cc_esp32_cond_waiter_t *waiter)
{
    if (!waiter) return;
    vSemaphoreDelete(waiter->signal);
    free(waiter);
}

/**
 * @brief 先登记私有节点，再释放谓词 mutex 等待并重新取得。
 * @pre 当前任务恰好持有 mutex 一次，调用者返回后在锁内重查谓词。
 * @param[in,out] cond 存活借用条件变量，NULL 跳过。
 * @param[in,out] mutex 当前任务持有的普通锁，NULL 跳过。
 * @post 正常返回后重新持锁，节点和信号量已清理，通知原语的最后使用由 gate 屏障覆盖。
 * @note 节点 OOM 时释放谓词锁并延迟约五十毫秒后重新取得，作为允许的伪唤醒。
 */
void cc_cond_wait(cc_cond_t cond, cc_mutex_t mutex)
{
    if (!cond || !mutex) return;
    cc_esp32_cond_t *state = (cc_esp32_cond_t *)cond;
    cc_esp32_cond_waiter_t *waiter = cc_cond_waiter_create();
    if (!waiter) {
        xSemaphoreGive((SemaphoreHandle_t)mutex);
        vTaskDelay(pdMS_TO_TICKS(50U) ? pdMS_TO_TICKS(50U) : 1U);
        xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY);
        return;
    }
    cc_cond_waiter_register(state, waiter);
    xSemaphoreGive((SemaphoreHandle_t)mutex);
    xSemaphoreTake(waiter->signal, portMAX_DELAY);
    xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY);
    xSemaphoreTake(state->gate, portMAX_DELAY);
    cc_cond_waiter_remove(state, waiter);
    xSemaphoreGive(state->gate);
    cc_cond_waiter_destroy(waiter);
}

/**
 * @brief 将毫秒预算向上取整为 tick，有限等待避开无限哨兵。
 * @param[in] timeout_ms 正数毫秒预算，非正数表示无限等待。
 * @return 非正数返回 portMAX_DELAY，正数至少一 tick，过长有限预算截到 portMAX_DELAY-1。
 */
static TickType_t cc_cond_timeout_ticks(int timeout_ms)
{
    if (timeout_ms <= 0) return portMAX_DELAY;
    uint64_t ticks = ((uint64_t)(unsigned)timeout_ms * (uint64_t)configTICK_RATE_HZ + 999U) / 1000U;
    if (ticks == 0) ticks = 1;
    if (ticks >= (uint64_t)portMAX_DELAY) ticks = (uint64_t)portMAX_DELAY - 1U;
    return (TickType_t)ticks;
}

/**
 * @brief 按 tick 预算等待本次私有通知，返回前重新持有谓词锁并清理节点。
 * @pre 当前任务恰好持有 mutex 一次，所有等待者按谓词循环检查。
 * @param[in,out] cond 存活借用条件变量，NULL 返回零。
 * @param[in,out] mutex 当前任务持有的普通锁，NULL 返回零。
 * @param[in] timeout_ms 正数为毫秒预算并向上取整到 tick，非正数沿 portMAX_DELAY。
 * @return 原生 Take 成功为一，超时、分配失败或 Take 失败为零，重新取得 mutex 可能额外等待。
 * @note 节点 OOM 以同预算延迟后返回零，无限预算 OOM 使用约五十毫秒延迟；超长有限值受 tick 表示范围限制。
 */
int cc_cond_timedwait(cc_cond_t cond, cc_mutex_t mutex, int timeout_ms)
{
    if (!cond || !mutex) return 0;
    cc_esp32_cond_t *state = (cc_esp32_cond_t *)cond;
    TickType_t ticks = cc_cond_timeout_ticks(timeout_ms);
    cc_esp32_cond_waiter_t *waiter = cc_cond_waiter_create();
    if (!waiter) {
        xSemaphoreGive((SemaphoreHandle_t)mutex);
        TickType_t fallback = ticks == portMAX_DELAY ? pdMS_TO_TICKS(50U) : ticks;
        if (fallback == 0) fallback = 1;
        vTaskDelay(fallback);
        xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY);
        return 0;
    }
    cc_cond_waiter_register(state, waiter);
    xSemaphoreGive((SemaphoreHandle_t)mutex);
    BaseType_t ok = xSemaphoreTake(waiter->signal, ticks);
    xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY);
    xSemaphoreTake(state->gate, portMAX_DELAY);
    cc_cond_waiter_remove(state, waiter);
    xSemaphoreGive(state->gate);
    cc_cond_waiter_destroy(waiter);
    return ok == pdTRUE ? 1 : 0;
}

/**
 * @brief 在 gate 内摘下一个已登记节点并通知其私有信号量。
 * @param[in,out] cond 存活借用条件变量，NULL 跳过。
 * @post 至多一个当时等待者得到通知，无等待者时不积存许可，选取顺序为当前链头。
 */
void cc_cond_signal(cc_cond_t cond)
{
    cc_esp32_cond_t *state = (cc_esp32_cond_t *)cond;
    if (!state) return;
    xSemaphoreTake(state->gate, portMAX_DELAY);
    cc_esp32_cond_waiter_t *waiter = state->waiters;
    if (waiter) {
        state->waiters = waiter->next;
        waiter->next = NULL;
        xSemaphoreGive(waiter->signal);
    }
    xSemaphoreGive(state->gate);
}

/**
 * @brief 在 gate 内摘下当时全部节点并逐一通知。
 * @param[in,out] cond 存活借用条件变量，NULL 跳过。
 * @post 当时已登记者各获一份通知，随后新等待者不消费旧广播，各调用重新争用谓词锁。
 */
void cc_cond_broadcast(cc_cond_t cond)
{
    cc_esp32_cond_t *state = (cc_esp32_cond_t *)cond;
    if (!state) return;
    xSemaphoreTake(state->gate, portMAX_DELAY);
    cc_esp32_cond_waiter_t *waiter = state->waiters;
    state->waiters = NULL;
    while (waiter) {
        cc_esp32_cond_waiter_t *next = waiter->next;
        waiter->next = NULL;
        xSemaphoreGive(waiter->signal);
        waiter = next;
    }
    xSemaphoreGive(state->gate);
}
#else
#error "cc_esp32_thread.c must be built under ESP-IDF"
#endif
