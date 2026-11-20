



#include "cc/ports/cc_path.h"
#include "cc/internal/cc_alloc.h"

#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/**
 * @brief 按已有 Windows 规则拼接路径，中间按需加入反斜杠。
 * @param[in] base 借用基路径，NULL 时复制 child。
 * @param[in] child 借用子路径，NULL 时复制 base，绝对或带盘符文本仍参与拼接。
 * @return 返回调用方 free 的字符串，双 NULL、长度溢出或分配失败返回 NULL。
 * @note 空字符串沿原规则参与拼接，本函数不解析文件系统或判断根内归属。
 */
char *cc_path_join(const char *base, const char *child)
{
    if (!base) return child ? cc_copy_string(child) : NULL;
    if (!child) return cc_copy_string(base);

    size_t base_len = strlen(base);
    size_t child_len = strlen(child);
    if (base_len > SIZE_MAX - child_len || base_len + child_len == SIZE_MAX) return NULL;
    int has_slash = (base_len > 0 && (base[base_len - 1] == '/' || base[base_len - 1] == '\\'));

    size_t padding = has_slash ? 1 : 2;
    if (base_len + child_len > SIZE_MAX - padding) return NULL;
    size_t len = base_len + child_len + padding;
    char *result = malloc(len);
    if (!result) return NULL;

    strcpy(result, base);
    if (!has_slash) strcat(result, "\\");
    strcat(result, child);

    return result;
}

/**
 * @brief 使用 ANSI GetFullPathName 词法绝对化路径，并按需扩容输出。
 * @param[in] path 借用路径，NULL 返回 NULL，相对路径依赖进程当前目录。
 * @return 返回调用方 free 的字符串，原生转换失败按既有行为复制原输入，分配或容量失败返回 NULL。
 * @note 不解析 junction 或符号链接，多个转换调用期间的 cwd 稳定性由调用者协调。
 */
char *cc_path_canonical(const char *path)
{
    if (!path) return NULL;

    char resolved[MAX_PATH];
    DWORD needed = GetFullPathNameA(path, MAX_PATH, resolved, NULL);
    if (needed == 0) return cc_copy_string(path);
    if (needed < MAX_PATH) {
        return cc_copy_string(resolved);
    }
    char *buffer = NULL;
    for (;;) {
        if ((uint64_t)needed > SIZE_MAX) { free(buffer); return NULL; }
        char *next = realloc(buffer, (size_t)needed);
        if (!next) { free(buffer); return NULL; }
        buffer = next;
        DWORD capacity = needed;
        needed = GetFullPathNameA(path, capacity, buffer, NULL);
        if (needed == 0) { free(buffer); return cc_copy_string(path); }
        if (needed < capacity) return buffer;
    }
}

/**
 * @brief 对两边作词法规范化后，以大小写不敏感的完整组件前缀判断归属。
 * @param[in] base_dir 借用根字符串，NULL 时返回 0。
 * @param[in] path 借用目标字符串，NULL 时返回 0。
 * @return 同根或根下返回 1，其余及分配失败返回 0，末尾分隔符已构成边界。
 * @note 原生转换失败可回退原输入，不能单靠此字符串判断固定 reparse 或目录并发变更后的实际对象。
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
    size_t path_len = strlen(canon_path);
    int result = (base_len > 0 && path_len >= base_len &&
                  _strnicmp(canon_path, canon_base, base_len) == 0 &&
                  (canon_base[base_len - 1] == '\\' || canon_base[base_len - 1] == '/' ||
                   canon_path[base_len] == '\\' || canon_path[base_len] == '/' || canon_path[base_len] == '\0'));

    free(canon_base);
    free(canon_path);
    return result;
}

/**
 * @brief 词法取得父目录，兼容混合分隔符并保留盘根。
 * @param[in] path 借用路径，NULL 返回 NULL，空字符串按无分隔符处理。
 * @return 返回调用方 free 的字符串，无分隔符返回点，分配失败返回 NULL。
 * @note 先去掉非盘根的尾分隔符，再选择位置最靠后的分隔符，不查询目录存在性。
 */
char *cc_path_dirname(const char *path)
{
    if (!path) return NULL;

    char *copy = cc_copy_string(path);
    if (!copy) return NULL;

    size_t len = strlen(copy);
    while (len > 1 && (copy[len - 1] == '\\' || copy[len - 1] == '/') &&
           !(len == 3 && copy[1] == ':')) copy[--len] = '\0';
    char *last_sep = strrchr(copy, '\\');
    char *forward_sep = strrchr(copy, '/');
    if (!last_sep || (forward_sep && forward_sep > last_sep)) last_sep = forward_sep;

    if (last_sep) {
        if (last_sep == copy) {
            free(copy);
            return cc_copy_string("\\");
        }
        if (last_sep == copy + 2 && copy[1] == ':') {
            copy[3] = '\0';
            return copy;
        }
        *last_sep = '\0';
        return copy;
    }

    free(copy);
    return cc_copy_string(".");
}

/**
 * @brief 用 ANSI GetFileAttributes 查询路径是否能确认存在。
 * @param[in] path 借用路径，NULL 时返回 0。
 * @return 属性查询成功返回 1，不存在、权限或其他查询错误返回 0。
 */
int cc_path_exists(const char *path)
{
    if (!path) return 0;
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
}

#endif
