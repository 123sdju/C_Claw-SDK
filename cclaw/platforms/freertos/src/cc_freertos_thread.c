



#include "cc/ports/cc_thread.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <stdlib.h>
#include <stdint.h>

/** @brief 进程期完成 mutex，首个任务发布前安装，供 done 原语退场与身份查询使用。 */
static cc_mutex_t thread_completion_mutex;

/**
 * @brief 在 FreeRTOS 临界区内安装一次普通 mutex，竞争副本在临界区外删除。
 * @param[in,out] slot 必需静态零初始化槽，所有安装和读取均通过本入口。
 * @return 已有或安装成功为 CC_OK，NULL 为 INVALID_ARGUMENT，创建失败原样返回且可重试。
 * @note 安装槽及其 mutex 保持到进程结束，不与重置或销毁重叠。
 */
cc_result_t cc_mutex_init_once(cc_mutex_t *slot)
{
    if (!slot) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null mutex slot");
    taskENTER_CRITICAL();
    int ready = *slot != NULL;
    taskEXIT_CRITICAL();
    if (ready) return cc_result_ok();
    cc_mutex_t candidate = NULL;
    cc_result_t rc = cc_mutex_create(&candidate);
    if (rc.code != CC_OK) return rc;
    taskENTER_CRITICAL();
    if (!*slot) { *slot = candidate; candidate = NULL; }
    taskEXIT_CRITICAL();
    if (candidate) cc_mutex_destroy(candidate);
    return rc;
}

#ifndef CCLAW_FREERTOS_TASK_STACK_WORDS
#define CCLAW_FREERTOS_TASK_STACK_WORDS 2048
#endif

/** @brief join 拥有的线程包装，完成标志由进程期完成 mutex 保护。 */
typedef struct cc_freertos_thread {
    cc_thread_fn_t fn; /**< 借用用户入口。 */
    void *arg; /**< 原创建方保管的借用上下文。 */
    SemaphoreHandle_t done; /**< 包装拥有的二值完成通知。 */
    TaskHandle_t task; /**< 未完成时可比较的原生任务身份。 */
    int completed; /**< 用户入口已返回，访问持完成锁。 */
} cc_freertos_thread_t;

/**
 * @brief 调用用户入口，在完成锁内发送 done，最后仅执行原生自删。
 * @param[in,out] arg 借用线程包装，join 通过 done 和完成 mutex 保活至最后访问。
 * @post 用户返回指针被忽略，completed 置一，Give 返回并解锁后不再访问包装或 done。
 * @note 同锁屏障覆盖 Give 解除接收者后尚未返回的原生代码，TCB/栈回收仍由 kernel/Idle 完成。
 */
static void cc_freertos_thread_entry(void *arg)
{
    cc_freertos_thread_t *thread = (cc_freertos_thread_t *)arg;
    (void)thread->fn(thread->arg);
    cc_mutex_lock(thread_completion_mutex);
    thread->completed = 1;
    xSemaphoreGive(thread->done);
    cc_mutex_unlock(thread_completion_mutex);
    vTaskDelete(NULL);
}

/**
 * @brief 准备完成屏障后创建优先级 idle+1 的 SDK 任务。
 * @param[in] fn 必需借用用户入口，可能在创建函数返回前执行。
 * @param[in,out] arg 借用入口上下文，覆盖用户函数执行期。
 * @param[out] out_thread 必需空资源槽，基础参数合法后置 NULL，成功由唯一拥有者 join。
 * @return 成功为 CC_OK，空参数为 INVALID_ARGUMENT，包装或信号量失败为 OUT_OF_MEMORY，任务创建失败为 PLATFORM，完成锁错误原样返回。
 * @note 栈为 CCLAW_FREERTOS_TASK_STACK_WORDS 个 StackType_t 单元，失败清理本次包装/done，已安装完成锁持续进程期。
 */
cc_result_t cc_thread_create(cc_thread_fn_t fn, void *arg, cc_thread_t *out_thread)
{
    if (!fn || !out_thread)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid thread arguments");
    *out_thread = NULL;
    cc_result_t rc = cc_mutex_init_once(&thread_completion_mutex);
    if (rc.code != CC_OK) return rc;

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
 * @brief 等待用户入口和 done 原语退出，随后消费包装与通知信号量。
 * @param[in,out] handle 唯一拥有且未成功 join 的句柄，NULL 为参数错误。
 * @return 成功为 CC_OK，原生 Take 失败或自 join 为 PLATFORM 并保留句柄供外部重试。
 * @note 拥有者串行化 join 并排空身份查询，成功不承诺 Idle 已回收原生 TCB/栈。
 */
cc_result_t cc_thread_join(cc_thread_t handle)
{
    cc_freertos_thread_t *thread = (cc_freertos_thread_t *)handle;
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
 * @brief 在完成锁内比较未完成任务身份。
 * @param[in] handle 存活借用包装，NULL 返回零，拥有者排除并发成功 join。
 * @return 未完成且当前任务匹配为一，已完成或不匹配为零，避免自删后 TCB 地址复用误认。
 */
int cc_thread_is_current(cc_thread_t handle)
{
    cc_freertos_thread_t *thread = (cc_freertos_thread_t *)handle;
    if (!thread) return 0;
    cc_mutex_lock(thread_completion_mutex);
    int matches = !thread->completed && thread->task == xTaskGetCurrentTaskHandle();
    cc_mutex_unlock(thread_completion_mutex);
    return matches;
}

/**
 * @brief 创建普通非递归 mutex。
 * @param[out] out_mutex 必需空资源槽，进入后置 NULL，成功供调用者 destroy。
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
 * @pre 已排空持有、等待与其他借用，不删除进程期 init_once 锁。
 * @param[in,out] mutex 消费拥有句柄，NULL 跳过。
 * @post 非 NULL 原生对象和借用失效。
 */
void cc_mutex_destroy(cc_mutex_t mutex)
{
    if (mutex) vSemaphoreDelete((SemaphoreHandle_t)mutex);
}

/**
 * @brief 以 portMAX_DELAY 取得普通 mutex。
 * @param[in,out] mutex 存活借用锁，NULL 跳过，当前任务尚未持有。
 * @post 成功取得后当前任务负责对应解锁，Take 失败无返回通道。
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

/** @brief 单次等待拥有的节点与私有信号量，链关系由条件变量 gate 保护。 */
typedef struct cc_freertos_cond_waiter {
    SemaphoreHandle_t signal; /**< 本次等待拥有的二值信号量。 */
    struct cc_freertos_cond_waiter *next; /**< gate 内维护的借用链。 */
} cc_freertos_cond_waiter_t;

/** @brief 条件变量拥有的 gate 及当前已登记等待链。 */
typedef struct cc_freertos_cond {
    SemaphoreHandle_t gate; /**< 保护登记、摘链和通知原语的 mutex。 */
    cc_freertos_cond_waiter_t *waiters; /**< 节点由各等待调用拥有。 */
} cc_freertos_cond_t;

/**
 * @brief 创建逐等待者通知的条件变量包装。
 * @param[out] out_cond 必需空资源槽，进入后置 NULL，成功由调用者 destroy。
 * @return 成功为 CC_OK，NULL 为 INVALID_ARGUMENT，包装或 gate 分配失败为 OUT_OF_MEMORY，失败清理本次资源。
 */
cc_result_t cc_cond_create(cc_cond_t *out_cond)
{
    if (!out_cond) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid condition output");
    *out_cond = NULL;
    cc_freertos_cond_t *cond = calloc(1, sizeof(*cond));
    if (!cond) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate condition state");
    cond->gate = xSemaphoreCreateMutex();
    if (!cond->gate) {
        free(cond);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create condition gate");
    }
    *out_cond = cond;
    return cc_result_ok();
}

/**
 * @brief 删除条件变量 gate 和包装。
 * @pre 已排空等待、通知、广播和其他句柄借用。
 * @param[in,out] cond 消费拥有句柄，NULL 跳过。
 * @post gate 与包装失效，等待节点已由各调用清理。
 */
void cc_cond_destroy(cc_cond_t cond)
{
    cc_freertos_cond_t *state = cond;
    if (!state) return;
    vSemaphoreDelete(state->gate);
    free(state);
}

/**
 * @brief 分配本次等待独占的节点及二值信号量。
 * @return 成功交付拥有节点，分配失败清理部分结果并返回 NULL。
 */
static cc_freertos_cond_waiter_t *cc_cond_waiter_create(void)
{
    cc_freertos_cond_waiter_t *waiter = calloc(1, sizeof(*waiter));
    if (!waiter) return NULL;
    waiter->signal = xSemaphoreCreateBinary();
    if (!waiter->signal) {
        free(waiter);
        return NULL;
    }
    return waiter;
}

/**
 * @brief 在 gate 内登记尚未释放谓词锁的等待者。
 * @param[in,out] state 必需存活条件变量。
 * @param[in,out] waiter 必需未登记节点，节点继续由等待调用拥有。
 * @post 节点成为当前链头，后续 signal/broadcast 只能通知已登记节点。
 */
static void cc_cond_waiter_register(cc_freertos_cond_t *state,
                                   cc_freertos_cond_waiter_t *waiter)
{
    xSemaphoreTake(state->gate, portMAX_DELAY);
    waiter->next = state->waiters;
    state->waiters = waiter;
    xSemaphoreGive(state->gate);
}

/**
 * @brief 从当前持 gate 的链中移除本次等待节点。
 * @pre 调用者已持 state->gate。
 * @param[in,out] state 必需存活条件变量。
 * @param[in,out] waiter 本次拥有节点，可已被通知者摘链。
 * @post 命中时删除链关系，next 清零，节点仍由等待调用清理。
 */
static void cc_cond_waiter_remove(cc_freertos_cond_t *state,
                                 cc_freertos_cond_waiter_t *waiter)
{
    cc_freertos_cond_waiter_t **link = &state->waiters;
    while (*link && *link != waiter) link = &(*link)->next;
    if (*link == waiter) *link = waiter->next;
    waiter->next = NULL;
}

/**
 * @brief 删除已摘链且通知原语已退场的等待节点。
 * @pre 调用者已跨过 gate 屏障，通知者不再访问本节点。
 * @param[in,out] waiter 消费拥有节点，NULL 跳过。
 * @post 私有信号量和节点失效。
 */
static void cc_cond_waiter_destroy(cc_freertos_cond_waiter_t *waiter)
{
    if (!waiter) return;
    vSemaphoreDelete(waiter->signal);
    free(waiter);
}

/**
 * @brief 登记私有等待者后释放谓词锁等待，再重新持锁并清理。
 * @pre 当前任务恰好持有 mutex 一次，调用方在锁内循环检查谓词。
 * @param[in,out] cond 存活借用条件变量，NULL 跳过。
 * @param[in,out] mutex 当前任务持有的普通锁，NULL 跳过。
 * @post 正常返回后重新持锁，私有通知及节点已释放。
 * @note 节点 OOM 时释放锁并延迟约五十毫秒后重新取得，按允许的伪唤醒处理。
 */
void cc_cond_wait(cc_cond_t cond, cc_mutex_t mutex)
{
    if (!cond || !mutex) return;
    cc_freertos_cond_t *state = cond;
    cc_freertos_cond_waiter_t *waiter = cc_cond_waiter_create();
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
 * @brief 将毫秒预算用宽整数向上换算到 tick，避免有限值映射无限哨兵。
 * @param[in] timeout_ms 正数为毫秒预算，非正数请求无限等待。
 * @return 非正数为 portMAX_DELAY，正数至少一 tick，超长有限值截到 portMAX_DELAY-1。
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
 * @brief 按本次私有通知和 tick 预算等待，返回前重新持有谓词锁。
 * @pre 当前任务恰好持有 mutex 一次，调用者在返回后重新检查谓词。
 * @param[in,out] cond 存活借用条件变量，NULL 返回零。
 * @param[in,out] mutex 当前任务持有的普通锁，NULL 返回零。
 * @param[in] timeout_ms 正数为毫秒预算，非正数沿 portMAX_DELAY，正数向上取整并限制在有限 tick 范围。
 * @return Take 成功为一，超时、分配或原生等待失败为零，重新持锁可能额外等待。
 * @note 节点 OOM 按预算延迟后返回零，无限预算 OOM 使用约五十毫秒延迟。
 */
int cc_cond_timedwait(cc_cond_t cond, cc_mutex_t mutex, int timeout_ms)
{
    if (!cond || !mutex) return 0;
    cc_freertos_cond_t *state = cond;
    TickType_t ticks = cc_cond_timeout_ticks(timeout_ms);
    cc_freertos_cond_waiter_t *waiter = cc_cond_waiter_create();
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
 * @brief 在 gate 内摘下并通知一个已登记等待者。
 * @param[in,out] cond 存活借用条件变量，NULL 跳过。
 * @post 至多一个当前链头获得通知，无等待者时不保存许可。
 */
void cc_cond_signal(cc_cond_t cond)
{
    cc_freertos_cond_t *state = cond;
    if (!state) return;
    xSemaphoreTake(state->gate, portMAX_DELAY);
    cc_freertos_cond_waiter_t *waiter = state->waiters;
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
 * @post 所有已登记者各获得一份通知，新等待者不会消费旧广播，返回后各自重查谓词。
 */
void cc_cond_broadcast(cc_cond_t cond)
{
    cc_freertos_cond_t *state = cond;
    if (!state) return;
    xSemaphoreTake(state->gate, portMAX_DELAY);
    cc_freertos_cond_waiter_t *waiter = state->waiters;
    state->waiters = NULL;
    while (waiter) {
        cc_freertos_cond_waiter_t *next = waiter->next;
        waiter->next = NULL;
        xSemaphoreGive(waiter->signal);
        waiter = next;
    }
    xSemaphoreGive(state->gate);
}
