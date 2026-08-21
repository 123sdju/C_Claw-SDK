#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 读取源码文件并确认业务路径没有退回旧事件名或底层 event bus 直发。 */
static int file_contains(const char *path, const char *needle)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 1;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len < 0) {
        fclose(fp);
        return 1;
    }
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(fp);
        return 1;
    }
    size_t n = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[n] = '\0';
    int found = strstr(buf, needle) != NULL;
    free(buf);
    return found;
}

/*
 * 静态回归测试：禁止 runtime/tool executor 重新引入旧事件名或直接发布 event bus。
 *
 * 这不是通用源码扫描器，只检查 observability 迁移中最关键的业务路径文件。
 */
int main(void)
{
    const char *files[] = {
        "cclaw/core/src/app/cc_agent_runtime.c",
        "cclaw/core/src/app/cc_tool_executor.c"
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
