



#include "cc/ports/cc_path.h"
#include "cc/internal/cc_alloc.h"

#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* 拼接 Windows 路径，自动使用反斜杠；返回字符串由调用方释放。 */
char *cc_path_join(const char *base, const char *child)
{
    if (!base) return child ? cc_copy_string(child) : NULL;
    if (!child) return cc_copy_string(base);

    size_t base_len = strlen(base);
    int has_slash = (base_len > 0 && (base[base_len - 1] == '/' || base[base_len - 1] == '\\'));

    size_t len = base_len + strlen(child) + (has_slash ? 0 : 1) + 1;
    char *result = malloc(len);
    if (!result) return NULL;

    strcpy(result, base);
    if (!has_slash) strcat(result, "\\");
    strcat(result, child);

    return result;
}

/*
 * Windows canonical path。
 *
 * 使用 GetFullPathNameA 做绝对化和 `.`/`..` 归一；失败时退回原始字符串复制。
 */
char *cc_path_canonical(const char *path)
{
    if (!path) return NULL;

    char resolved[MAX_PATH];
    if (GetFullPathNameA(path, MAX_PATH, resolved, NULL)) {
        return cc_copy_string(resolved);
    }
    return cc_copy_string(path);
}

/*
 * 判断 path 是否在 base_dir 内。
 *
 * Windows 文件系统大小写通常不敏感，因此使用 _strnicmp；同时检查后续字符必须是路径
 * 分隔符或字符串结束，避免 prefix 绕过。
 */
int cc_path_is_within(const char *base_dir, const char *path)
{
    if (!base_dir || !path) return 0;

    char *canon_base = cc_path_canonical(base_dir);
    char *canon_path = cc_path_canonical(path);

    if (!canon_base || !canon_path) {
        free(canon_base);
        free(canon_path);
        return 0;
    }

    size_t base_len = strlen(canon_base);
    int result = (_strnicmp(canon_path, canon_base, base_len) == 0 &&
                  (canon_path[base_len] == '\\' || canon_path[base_len] == '/' || canon_path[base_len] == '\0'));

    free(canon_base);
    free(canon_path);
    return result;
}

/* 返回 Windows 路径父目录，兼容 '/' 和 '\\' 两种分隔符。 */
char *cc_path_dirname(const char *path)
{
    if (!path) return NULL;

    char *copy = cc_copy_string(path);
    if (!copy) return NULL;

    char *last_sep = strrchr(copy, '\\');
    if (!last_sep) last_sep = strrchr(copy, '/');

    if (last_sep) {
        if (last_sep == copy) {
            free(copy);
            return cc_copy_string("\\");
        }
        *last_sep = '\0';
        return copy;
    }

    free(copy);
    return cc_copy_string(".");
}

/* 使用 GetFileAttributesA 判断路径是否存在。 */
int cc_path_exists(const char *path)
{
    if (!path) return 0;
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
}

#endif
