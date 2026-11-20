#include "cc/ports/cc_platform.h"

#include "FreeRTOS.h"
#include "task.h"

/**
 * @brief 提供通用 FreeRTOS 平台初始化入口，内核和 BSP 由应用建立。
 * @post 无分配或共享状态变更，允许重复调用。
 */
void cc_platform_init(void)
{
}

/**
 * @brief 原子取得 tick 和回绕计数，换算为本次内核启动以来的毫秒数。
 * @pre FreeRTOS 内核已初始化，从任务上下文调用。
 * @return 毫秒时间，按整数毫秒截断，超出可表示毫秒时饱和为 UINT64_MAX。
 * @note 使用 vTaskSetTimeOutState 的临界区快照，允许任务并发读取；重启后重新计数。
 * tick 与回绕合计取 64 位范围，内核的回绕计数字段本身也有有限位宽。
 */
uint64_t cc_platform_monotonic_ms(void)
{
    TimeOut_t snapshot;
    vTaskSetTimeOutState(&snapshot);
    const uint64_t ticks_per_overflow = (uint64_t)portMAX_DELAY + 1U;
    uint64_t ticks = (uint64_t)snapshot.xTimeOnEntering;
    if (ticks_per_overflow != 0) {
        ticks += (uint64_t)(UBaseType_t)snapshot.xOverflowCount * ticks_per_overflow;
    }
    const uint64_t rate = (uint64_t)configTICK_RATE_HZ;
    uint64_t seconds = ticks / rate;
    uint64_t fractional_ms = (ticks % rate) * 1000U / rate;
    if (seconds > UINT64_MAX / 1000U) return UINT64_MAX;
    uint64_t milliseconds = seconds * 1000U;
    return fractional_ms > UINT64_MAX - milliseconds
        ? UINT64_MAX : milliseconds + fractional_ms;
}

#if defined(__GNUC__)
__attribute__((weak))
#endif
/**
 * @brief 为 BSP 保留硬件随机源的默认弱实现，当前未提供随机数据。
 * @param[out] buffer 借用输出槽，默认实现不访问。
 * @param[in] size 请求字节数，默认实现不执行填充。
 * @return 默认始终返回负一，BSP 可提供同名强符号实现硬件随机填充。
 * @note 无共享状态，GCC 构建使用弱符号，替代实现负责其缓冲与并发契约。
 */
int cc_freertos_hardware_random_bytes(void *buffer, size_t size)
{
    (void)buffer;
    (void)size;
    return -1;
}

/**
 * @brief 校验随机输出缓冲后同步转发 BSP 硬件随机源。
 * @param[out] buffer 调用期间借用的可写缓冲，size 为零时允许 NULL。
 * @param[in] size 待填充的字节数，调用者保证容量充足。
 * @return 非零长度配 NULL 返回负一，其余转发 BSP 结果，默认实现返回负一。
 * @note 不保留缓冲地址，并发与熵源条件由 BSP 替代实现负责。
 */
int cc_platform_random_bytes(void *buffer, size_t size)
{
    if (!buffer && size > 0) return -1;
    return cc_freertos_hardware_random_bytes(buffer, size);
}
