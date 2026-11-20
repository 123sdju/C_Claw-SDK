#include "cc/ports/cc_platform.h"

#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/**
 * @brief 完成默认 POSIX 平台初始化入口，当前实现为空操作。
 * @post 无拥有资源产生，可重复调用；应用与 BSP 的资源初始化由各自拥有者执行。
 */
void cc_platform_init(void)
{
}

/**
 * @brief 将 CLOCK_MONOTONIC 时钟转换为当前运行期间的毫秒计数。
 * @return 成功返回毫秒值，原生读取失败返回零；计数域由当前系统时钟确定。
 * @note 无共享可变状态，可并发读取；该值供同一时钟域的间隔与截止时间计算使用。
 */
uint64_t cc_platform_monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

/**
 * @brief 从当前系统的 /dev/urandom 同步填满调用者缓冲区。
 * @param[out] buffer 借用的至少 size 字节可写区，size 为零时可为 NULL。
 * @param[in] size 请求字节数。
 * @return 完整填充返回零，参数、打开或读取失败返回负一，失败可能已写入部分字节。
 * @note 每次独立打开并关闭描述符，短读与 EINTR 会继续读取；零长度仍检查设备能否打开。
 */
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
