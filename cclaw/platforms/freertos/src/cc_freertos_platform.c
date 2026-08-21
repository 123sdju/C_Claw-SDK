#include "cc/ports/cc_platform.h"

#include "FreeRTOS.h"
#include "task.h"

/*
 * 初始化 FreeRTOS 平台运行环境。
 *
 * 当前实现为空，内核由 FreeRTOS 调度器自行初始化。
 */
void cc_platform_init(void)
{
}

/*
 * 获取 FreeRTOS 平台单调递增的毫秒级时间戳。
 *
 * 通过 xTaskGetTickCount() * portTICK_PERIOD_MS 实现，自调度器启动开始计数。
 * 返回：单调递增的毫秒级时间戳。
 */
uint64_t cc_platform_monotonic_ms(void)
{
    return (uint64_t)xTaskGetTickCount() * (uint64_t)portTICK_PERIOD_MS;
}

/*
 * 板级 BSP 可提供同名强符号，把 TRNG/安全元件能力注入通用 FreeRTOS profile。
 * 默认实现 fail-closed，绝不使用 tick、rand() 或设备计数器伪装 CSPRNG。
 */
#if defined(__GNUC__)
__attribute__((weak))
#endif
int cc_freertos_hardware_random_bytes(void *buffer, size_t size)
{
    (void)buffer;
    (void)size;
    return -1;
}

int cc_platform_random_bytes(void *buffer, size_t size)
{
    if (!buffer && size > 0) return -1;
    return cc_freertos_hardware_random_bytes(buffer, size);
}
