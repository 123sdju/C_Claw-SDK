#ifndef CC_TEST_FREERTOS_H
#define CC_TEST_FREERTOS_H

#include <pthread.h>
#include <stdint.h>

typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
/**
 * @brief 模型执行的原生任务入口，真实平台包装负责最后自删。
 * @param[in,out] arg 创建任务时借用的上下文，覆盖入口执行期。
 * @post 入口完成或调用模型自删结束当前 pthread。
 */
typedef void (*TaskFunction_t)(void *arg);
typedef struct stub_semaphore *SemaphoreHandle_t;
typedef struct stub_task *TaskHandle_t;
typedef pthread_mutex_t portMUX_TYPE;

extern pthread_mutex_t stub_critical_mutex;
extern unsigned stub_tick_rate;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY UINT32_MAX
#define tskIDLE_PRIORITY 0U
#define configTICK_RATE_HZ stub_tick_rate
#define configSUPPORT_STATIC_ALLOCATION 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(((TickType_t)(ms) * (TickType_t)configTICK_RATE_HZ) / 1000U))
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(gate) pthread_mutex_lock(gate)
#define portEXIT_CRITICAL(gate) pthread_mutex_unlock(gate)
#define taskENTER_CRITICAL() pthread_mutex_lock(&stub_critical_mutex)
#define taskEXIT_CRITICAL() pthread_mutex_unlock(&stub_critical_mutex)

/**
 * @brief 创建 pthread 模型信号量，供真实 RTOS 平台源码调用。
 * @param[in] mutex 非零创建可取得一次的普通 mutex，零创建空二值通知。
 * @return 拥有模型对象，故障或分配失败为 NULL，由 stub_semaphore_delete 回收。
 */
SemaphoreHandle_t stub_semaphore_create(int mutex);
/**
 * @brief 在模型中取得 mutex 或二值通知，支持确定交错和故障开关。
 * @param[in,out] semaphore 存活借用模型对象。
 * @param[in] ticks tick 预算，portMAX_DELAY 为无限等待。
 * @return 取得为 pdTRUE，超时或注入失败为 pdFALSE。
 */
BaseType_t stub_semaphore_take(SemaphoreHandle_t semaphore, TickType_t ticks);
/**
 * @brief 发布模型通知，并允许测试在原语尾部暂停。
 * @param[in,out] semaphore 存活借用模型对象。
 * @return 通知完成为 pdTRUE，模型记录原语仍在途时的提前销毁。
 */
BaseType_t stub_semaphore_give(SemaphoreHandle_t semaphore);
/**
 * @brief 删除拥有模型对象，检查是否仍有 Give 尾部使用。
 * @param[in,out] semaphore 消费拥有对象，NULL 跳过。
 * @post 无在途 Give 时销毁，违规交错记录错误并延迟到尾部退出后回收。
 */
void stub_semaphore_delete(SemaphoreHandle_t semaphore);
/**
 * @brief 通过 pthread 执行传入的真实 RTOS 任务入口。
 * @param[in] entry 借用的任务入口。
 * @param[in] name 借用名称，模型忽略。
 * @param[in] stack 配置栈数值，模型记录，实际 pthread 采用主机默认栈。
 * @param[in,out] arg 借用任务上下文。
 * @param[in] priority 原生优先级配置，模型忽略。
 * @param[out] out_task 成功写静态任务记录借用。
 * @return 创建成功为 pdPASS，故障或 pthread 失败为 pdFAIL。
 */
BaseType_t stub_task_create(TaskFunction_t entry, const char *name, uint32_t stack,
    void *arg, UBaseType_t priority, TaskHandle_t *out_task);
/**
 * @brief 结束当前模型任务的 pthread，模拟 kernel 自删。
 * @param[in] task 真实源码传 NULL，模型要求当前任务自删。
 * @post 当前 pthread 退出，模型任务记录仍由测试保管至 pthread_join。
 */
void stub_task_delete(TaskHandle_t task);
/**
 * @brief 取得当前模型任务身份。
 * @return TLS 身份指针，测试可模拟已回收任务地址复用，普通主线程为 NULL。
 */
TaskHandle_t stub_task_current(void);
/**
 * @brief 记录 OOM 回退延迟，模型不实际阻塞。
 * @param[in] ticks 延迟 tick 数。
 * @post 最后延迟数值更新，供重取谓词锁测试核对。
 */
void stub_task_delay(TickType_t ticks);

#define xSemaphoreCreateMutex() stub_semaphore_create(1)
#define xSemaphoreCreateBinary() stub_semaphore_create(0)
#define xSemaphoreTake stub_semaphore_take
#define xSemaphoreGive stub_semaphore_give
#define vSemaphoreDelete stub_semaphore_delete
#define xTaskCreate stub_task_create
#define vTaskDelete stub_task_delete
#define xTaskGetCurrentTaskHandle stub_task_current
#define vTaskDelay stub_task_delay

#endif
