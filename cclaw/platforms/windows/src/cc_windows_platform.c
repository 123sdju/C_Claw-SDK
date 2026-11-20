#include "cc/ports/cc_platform.h"

#include <windows.h>
#include <bcrypt.h>

/**
 * @brief 完成默认 Windows 平台初始化入口，当前实现为空操作。
 * @post 无拥有资源产生，可重复调用；网络和应用资源由各自工厂及拥有者初始化。
 */
void cc_platform_init(void)
{
}

/**
 * @brief 读取 GetTickCount64 的系统运行毫秒计数。
 * @return 原生计数值，供同一运行时钟域内的间隔和截止时间计算使用。
 * @note 无共享可变状态，可并发读取，墙上时钟的校时独立于该计数。
 */
uint64_t cc_platform_monotonic_ms(void)
{
    return (uint64_t)GetTickCount64();
}

/**
 * @brief 使用 Windows 系统首选随机源同步填充缓冲区。
 * @param[out] buffer 借用的至少 size 字节可写区，size 为零时可为 NULL。
 * @param[in] size 请求字节数，必须能由原生 ULONG 表示。
 * @return 原生调用成功返回零，参数、长度或 BCrypt 调用失败返回负一。
 * @note 输入在调用期间保持有效，失败时调用者应放弃缓冲区内容。
 */
int cc_platform_random_bytes(void *buffer, size_t size)
{
    if ((!buffer && size > 0) || size > (size_t)ULONG_MAX) return -1;
    NTSTATUS status = BCryptGenRandom(
        NULL, (PUCHAR)buffer, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status >= 0 ? 0 : -1;
}
