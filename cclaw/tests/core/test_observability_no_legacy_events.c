#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/**
 * @brief 完整读取源码并检查禁用文本，任何读取失败都使静态回归失败。
 * @param[in] path 当前工作目录可解析的非 NULL 文件路径借用。
 * @param[in] needle 非 NULL 禁用子串借用。
 * @return 命中或打开/定位/大小/分配/读取/关闭失败为一，完整未命中为零。
 * @note 临时正文由本函数释放，FILE 在返回前消费，输入文件由 CTest 独占稳定读取。
 */
static int file_contains(const char *path, const char *needle)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 1;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return 1; }
    long len = ftell(fp);
    if (len < 0 || (uintmax_t)len >= (uintmax_t)SIZE_MAX || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 1;
    }
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(fp);
        return 1;
    }
    size_t n = fread(buf, 1, (size_t)len, fp);
    int read_failed = n != (size_t)len || ferror(fp);
    int close_failed = fclose(fp) != 0;
    buf[n] = '\0';
    int found = read_failed || close_failed || strstr(buf, needle) != NULL;
    free(buf);
    return found;
}

/**
 * @brief 对 runtime 和 executor 两个生产文件逐项检查禁用事件名及直接发布入口。
 * @return 全部可完整读取且不含七类禁用文本为零，其他为一，无保留资源。
 * @note CTest 从 SDK 根目录运行，本测试按文本匹配，业务运行事件另由动态测试覆盖。
 */
int main(void)
{
    const char *files[] = {
        "core/src/app/cc_agent_runtime.c",
        "core/src/app/cc_tool_executor.c"
    };
    const char *forbidden[] = {
        "cc_event_bus_publish(",
        "CC_EVENT_STREAM",
        "agent.finished",
        "llm.request.started",
        "llm.response.received",
        "tool.call.started",
        "tool.call.finished"
    };

    int failed = 0;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        for (size_t j = 0; j < sizeof(forbidden) / sizeof(forbidden[0]); j++) {
            if (file_contains(files[i], forbidden[j])) failed = 1;
        }
    }
    return failed ? 1 : 0;
}
