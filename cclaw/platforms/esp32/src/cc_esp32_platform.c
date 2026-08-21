#include "cc/ports/cc_platform.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"

/*
 * 初始化 ESP32 平台运行环境。
 *
 * 当前实现为空，FreeRTOS 及外设由 ESP-IDF 启动流程自行初始化。
 */
void cc_platform_init(void)
{
}

/*
 * 获取 ESP32 平台单调递增的毫秒级时间戳。
 *
 * 通过 xTaskGetTickCount() * portTICK_PERIOD_MS 实现，自 FreeRTOS 调度器启动开始计数。
 * 返回：单调递增的毫秒级时间戳。
 */
uint64_t cc_platform_monotonic_ms(void)
{
    return (uint64_t)xTaskGetTickCount() * (uint64_t)portTICK_PERIOD_MS;
}

int cc_platform_random_bytes(void *buffer, size_t size)
{
    if (!buffer && size > 0) return -1;
    esp_fill_random(buffer, size);
    return 0;
}
