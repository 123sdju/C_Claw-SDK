#include "cc/ports/cc_path.h"
#include "cc/internal/cc_alloc.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int is_absolute_path(const char *path)
{
    return path && path[0] == '/';
}

/*
 * 对不存在的路径做词法归一化。
 *
 * realpath 要求目标存在；写文件工具需要在目标文件尚不存在时检查 parent/workspace 边界。
 * 这里把相对路径转成基于 cwd 的绝对路径，再手工处理 "." 和 ".."，但不解析符号链接。
 */
static char *normalize_absolute_path(const char *path)
{
    if (!path || !path[0]) return NULL;

    char input[PATH_MAX];
    if (is_absolute_path(path)) {
        if (snprintf(input, sizeof(input), "%s", path) >= (int)sizeof(input)) {
            return NULL;
        }
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return NULL;
        if (snprintf(input, sizeof(input), "%s/%s", cwd, path) >= (int)sizeof(input)) {
            return NULL;
        }
    }

    char work[PATH_MAX];
    if (snprintf(work, sizeof(work), "%s", input) >= (int)sizeof(work)) {
        return NULL;
    }

    char *parts[PATH_MAX / 2];
    size_t count = 0;
    char *save = NULL;
    char *token = strtok_r(work, "/", &save);
    while (token) {
        if (strcmp(token, ".") == 0 || token[0] == '\0') {
            /* 跳过当前目录和空段。 */
        } else if (strcmp(token, "..") == 0) {
            if (count > 0) count--;
        } else {
            if (count >= sizeof(parts) / sizeof(parts[0])) return NULL;
            parts[count++] = token;
        }
        token = strtok_r(NULL, "/", &save);
    }

    char out[PATH_MAX];
    size_t pos = 0;
    out[pos++] = '/';
    for (size_t i = 0; i < count; i++) {
        size_t len = strlen(parts[i]);
        if (pos + len + (i > 0 ? 1 : 0) + 1 > sizeof(out)) return NULL;
        if (i > 0) out[pos++] = '/';
        memcpy(out + pos, parts[i], len);
        pos += len;
    }
    out[pos] = '\0';

    return cc_copy_string(out);
}

/*
 * 拼接 base 和 child。
 *
 * 函数只做平台分隔符拼接，不做 canonical 或安全检查；安全边界由 cc_path_is_within 负责。
 */
char *cc_path_join(const char *base, const char *child)
{
    if (!base || !base[0]) return child ? cc_copy_string(child) : NULL;
    if (!child || !child[0]) return cc_copy_string(base);
    if (is_absolute_path(child)) return cc_copy_string(child);

    size_t base_len = strlen(base);
    size_t child_len = strlen(child);
    int has_slash = (base_len > 0 && base[base_len - 1] == '/');

    if (base_len > (size_t)-1 - child_len - (has_slash ? 1 : 2)) {
        return NULL;
    }

    size_t len = base_len + child_len + (has_slash ? 0 : 1) + 1;
    char *result = malloc(len);
    if (!result) return NULL;

    memcpy(result, base, base_len);
    size_t pos = base_len;
    if (!has_slash) result[pos++] = '/';
    memcpy(result + pos, child, child_len + 1);
    return result;
}

/*
 * 获取 canonical path。
 *
 * 目标存在时使用 realpath 解析符号链接；目标不存在时退回词法归一化，保证写入前也能做
 * workspace prefix 检查。
 */
char *cc_path_canonical(const char *path)
{
    if (!path || !path[0]) return NULL;

    char resolved[PATH_MAX];
    if (realpath(path, resolved)) {
        return cc_copy_string(resolved);
    }

    char *parent = cc_path_dirname(path);
    if (parent) {
        char parent_resolved[PATH_MAX];
        if (realpath(parent, parent_resolved)) {
            const char *name = strrchr(path, '/');
            name = name ? name + 1 : path;
            char *joined = cc_path_join(parent_resolved, name);
            free(parent);
            if (!joined) return NULL;
            char *normalized = normalize_absolute_path(joined);
            free(joined);
            return normalized;
        }
        free(parent);
    }

    return normalize_absolute_path(path);
}

/*
 * 判断 path 是否位于 base_dir 内。
 *
 * 两边先 canonical，再做 prefix + 路径分隔符检查，避免 "/tmp/ws2" 被误判为 "/tmp/ws"
 * 内部路径。base_dir 为 "/" 时，任意绝对 canonical 路径都在其内。
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

    int result = 0;
    if (strcmp(canon_base, "/") == 0) {
        result = is_absolute_path(canon_path);
    } else {
        size_t base_len = strlen(canon_base);
        result = (strncmp(canon_path, canon_base, base_len) == 0 &&
                  (canon_path[base_len] == '/' || canon_path[base_len] == '\0'));
    }

    free(canon_base);
    free(canon_path);
    return result;
}

/*
 * 返回路径的父目录。
 *
 * 根目录返回 "/"；没有斜杠的相对文件返回 "."；空路径返回 NULL。
 */
char *cc_path_dirname(const char *path)
{
    if (!path || !path[0]) return NULL;

    char *copy = cc_copy_string(path);
    if (!copy) return NULL;

    size_t len = strlen(copy);
    while (len > 1 && copy[len - 1] == '/') {
        copy[--len] = '\0';
    }

    char *last_slash = strrchr(copy, '/');
    if (last_slash) {
        if (last_slash == copy) {
            free(copy);
            return cc_copy_string("/");
        }
        *last_slash = '\0';
        return copy;
    }

    free(copy);
    return cc_copy_string(".");
}

/* 判断路径是否存在；POSIX 实现使用 access(F_OK)。 */
int cc_path_exists(const char *path)
{
    if (!path || !path[0]) return 0;
    return access(path, F_OK) == 0;
}
