#include "cc/ports/cc_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * @brief 在真实独占临时目录验证穿越、相似前缀和符号链接父路径的工作区边界。
 * @return 全部路径拒绝且夹具清理成功为零，准备、分配、边界或清理失败为一。
 * @note 目录与链接创建必须成功才进入断言，拥有路径逐项 free，串行清理链接后删除目录。
 */
int main(void)
{
    char base[256];
    snprintf(base, sizeof(base), "/tmp/cclaw_path_security_%ld", (long)getpid());
    char ws[320];
    char outside[320];
    snprintf(ws, sizeof(ws), "%s/ws", base);
    snprintf(outside, sizeof(outside), "%s/outside", base);
    if (mkdir(base, 0700) != 0) return 1;
    if (mkdir(ws, 0700) != 0) { rmdir(base); return 1; }
    if (mkdir(outside, 0700) != 0) { rmdir(ws); rmdir(base); return 1; }

    int failed = 0;
    char *traversal = cc_path_join(ws, "../outside/new.txt");
    if (!traversal || cc_path_is_within(ws, traversal)) failed = 1;
    free(traversal);

    char *prefix = cc_path_join(ws, "../ws2/file.txt");
    if (!prefix || cc_path_is_within(ws, prefix)) failed = 1;
    free(prefix);

    char link_path[360];
    snprintf(link_path, sizeof(link_path), "%s/link", ws);
    int linked = symlink(outside, link_path) == 0;
    if (!linked) failed = 1;
    char *via_link = cc_path_join(link_path, "new.txt");
    char *parent = via_link ? cc_path_dirname(via_link) : NULL;
    if (!via_link || !parent || cc_path_is_within(ws, parent)) failed = 1;
    free(parent);
    free(via_link);

    if (linked && unlink(link_path) != 0) failed = 1;
    if (rmdir(outside) != 0) failed = 1;
    if (rmdir(ws) != 0) failed = 1;
    if (rmdir(base) != 0) failed = 1;
    return failed ? 1 : 0;
}
