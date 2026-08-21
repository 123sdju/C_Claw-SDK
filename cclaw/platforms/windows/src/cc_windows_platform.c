#include "cc/ports/cc_platform.h"

#include <windows.h>
#include <bcrypt.h>

/*
 * 初始化 Windows 平台运行环境。
 *
 * 当前实现为空，无需 WSAStartup 等额外初始化。
 */
void cc_platform_init(void)
{
}

/*
 * 获取 Windows 平台单调递增的毫秒级时间戳。
 *
 * 通过 GetTickCount64() 实现，返回值不受系统时间调整影响。
 * 返回：单调递增的毫秒级时间戳。
 */
uint64_t cc_platform_monotonic_ms(void)
{
    return (uint64_t)GetTickCount64();
}

int cc_platform_random_bytes(void *buffer, size_t size)
{
    if ((!buffer && size > 0) || size > (size_t)ULONG_MAX) return -1;
    NTSTATUS status = BCryptGenRandom(
        NULL, (PUCHAR)buffer, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status >= 0 ? 0 : -1;
}
