#include "cc/ports/cc_platform.h"

#include "esp_random.h"
#include "esp_timer.h"

/**
 * @brief 提供 ESP32 平台初始化入口，运行环境由 ESP-IDF 启动流程建立。
 * @post 无分配、共享状态变更或失败结果，允许重复调用。
 */
void cc_platform_init(void)
{
}

/**
 * @brief 将 ESP-IDF 的 64 位启动计时转换为单调毫秒时间。
 * @return 本次启动以来的毫秒数，深度睡眠重启后重新计数，精度为一毫秒。
 * @note 可并发读取，避免 FreeRTOS 32 位 tick 回绕使截止时间和连接冷却倒退。
 */
uint64_t cc_platform_monotonic_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000U;
}

/**
 * @brief 使用 ESP-IDF 随机源填充调用者缓冲区。
 * @param[out] buffer 调用期间借用的可写区域，size 为零时允许 NULL。
 * @param[in] size 要填充的字节数，调用者保证缓冲容量充足。
 * @return 填充完成为零，非零长度配 NULL 时为负一。
 * @note 不保留缓冲区，可并发调用；随机源的熵条件沿用 ESP-IDF 当前外设状态。
 */
int cc_platform_random_bytes(void *buffer, size_t size)
{
    if (!buffer && size > 0) return -1;
    if (size > 0) esp_fill_random(buffer, size);
    return 0;
}
