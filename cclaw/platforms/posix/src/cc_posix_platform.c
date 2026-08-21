#include "cc/ports/cc_platform.h"

#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/*
 * 初始化 POSIX 平台运行环境。
 *
 * 当前实现为空，无需额外初始化。
 */
void cc_platform_init(void)
{
}

/*
 * 获取 POSIX 平台单调递增的毫秒级时间戳。
 *
 * 通过 clock_gettime(CLOCK_MONOTONIC) 实现，不受系统时间跳变影响。
 * 返回：单调递增的毫秒级时间戳，clock_gettime 失败时返回 0。
 */
uint64_t cc_platform_monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

int cc_platform_random_bytes(void *buffer, size_t size)
{
    if (!buffer && size > 0) return -1;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < size) {
        ssize_t n = read(fd, (unsigned char *)buffer + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return -1; }
        done += (size_t)n;
    }
    close(fd);
    return 0;
}
