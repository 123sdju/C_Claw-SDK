#include "cc/ports/cc_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * 验证 workspace 路径边界检查。
 *
 * 覆盖 `../` 穿越、workspace prefix 绕过和 symlink 指向 workspace 外的 parent 归一化，
 * 这些都是文件写入工具必须防住的场景。
 */
int main(void)
{
    char base[256];
    snprintf(base, sizeof(base), "/tmp/cclaw_path_security_%ld", (long)getpid());
    char ws[320];
    char outside[320];
    snprintf(ws, sizeof(ws), "%s/ws", base);
    snprintf(outside, sizeof(outside), "%s/outside", base);
    mkdir(base, 0700);
    mkdir(ws, 0700);
    mkdir(outside, 0700);

    int failed = 0;
    char *traversal = cc_path_join(ws, "../outside/new.txt");
    if (cc_path_is_within(ws, traversal)) failed = 1;
    free(traversal);

    char *prefix = cc_path_join(ws, "../ws2/file.txt");
    if (cc_path_is_within(ws, prefix)) failed = 1;
    free(prefix);

    char link_path[360];
    snprintf(link_path, sizeof(link_path), "%s/link", ws);
    symlink(outside, link_path);
    char *via_link = cc_path_join(link_path, "new.txt");
    char *parent = cc_path_dirname(via_link);
    if (cc_path_is_within(ws, parent)) failed = 1;
    free(parent);
    free(via_link);

    unlink(link_path);
    rmdir(outside);
    rmdir(ws);
    rmdir(base);
    return failed ? 1 : 0;
}
