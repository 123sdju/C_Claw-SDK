#include "cc/ports/cc_path.h"
#include "cc/internal/cc_alloc.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

/**
 * @brief 识别以斜杠开始的 POSIX 绝对路径。
 * @param[in] path 借用路径，可为 NULL。
 * @return 首字节为斜杠返回 1，其余返回 0。
 */
static int is_absolute_path(const char *path)
{
    return path && path[0] == '/';
}

/**
 * @brief 基于当前 cwd 生成绝对路径并词法折叠点段及多余斜杠。
 * @param[in] path 非空借用路径，相对形式需要 getcwd 成功。
 * @return 返回调用方 free 的绝对路径，空输入、PATH_MAX 或段数超限、cwd 或分配失败返回 NULL。
 * @note 不查询组件是否存在，不解析符号链接，调用者协调进程 cwd 的并发修改。
 */
static char *normalize_absolute_path(const char *path)
{
    if (!path || !path[0]) return NULL;

    char input[PATH_MAX];
    int written;
    if (is_absolute_path(path)) {
        written = snprintf(input, sizeof(input), "%s", path);
        if (written < 0 || (size_t)written >= sizeof(input)) {
            return NULL;
        }
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return NULL;
        written = snprintf(input, sizeof(input), "%s/%s", cwd, path);
        if (written < 0 || (size_t)written >= sizeof(input)) {
            return NULL;
        }
    }

    char work[PATH_MAX];
    written = snprintf(work, sizeof(work), "%s", input);
    if (written < 0 || (size_t)written >= sizeof(work)) {
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

/**
 * @brief 拼接非空相对 child，绝对 child 直接覆盖 base。
 * @param[in] base 借用基路径，NULL 或空时复制 child。
 * @param[in] child 借用子路径，NULL 或空时复制非空 base，两者均无内容时按现有复制规则返回。
 * @return 成功返回调用方 free 的字符串，双 NULL、长度溢出或分配失败返回 NULL。
 * @note 只进行分隔符拼接，不固定目标文件系统对象，不能代替根约束 I/O。
 */
char *cc_path_join(const char *base, const char *child)
{
    if (!base || !base[0]) return child ? cc_copy_string(child) : NULL;
    if (!child || !child[0]) return cc_copy_string(base);
    if (is_absolute_path(child)) return cc_copy_string(child);

    size_t base_len = strlen(base);
    size_t child_len = strlen(child);
    if (base_len > SIZE_MAX - child_len || base_len + child_len == SIZE_MAX) return NULL;
    int has_slash = (base_len > 0 && base[base_len - 1] == '/');

    size_t padding = has_slash ? 1 : 2;
    if (base_len + child_len > SIZE_MAX - padding) {
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

/**
 * @brief 优先解析目标或直接父目录的真实路径，失败后提供词法绝对化结果。
 * @param[in] path 非空借用路径，可为相对路径，由调用者协调 cwd 变更。
 * @return 返回调用方 free 的规范化字符串，空输入、容量限制或分配失败返回 NULL。
 * @note 目标及直接父目录均无法 realpath 时采用词法回退，不能保证解析更远祖先的符号链接。
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

/**
 * @brief 分别规范化根与目标，按完整组件前缀判断同根或位于根下。
 * @param[in] base_dir 借用根路径，NULL 或无法规范化时返回 0。
 * @param[in] path 借用目标路径，NULL 或无法规范化时返回 0。
 * @return 同根或根内返回 1，其余返回 0，根为斜杠时接受所有规范化绝对路径。
 * @note 两次解析之间不持有文件系统对象，链接或目录并发变化以及 canonical 词法回退仍需根约束 I/O 处理。
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

/**
 * @brief 词法取得父目录，先移除非根路径末尾的斜杠。
 * @param[in] path 借用的非空路径。
 * @return 返回调用方 free 的父目录，根返回斜杠，无斜杠返回点，空输入或分配失败返回 NULL。
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

/**
 * @brief 使用 access 查询路径是否可确认存在。
 * @param[in] path 借用路径，NULL 或空返回 0。
 * @return access 成功返回 1，其他情况返回 0，包括不存在和访问失败。
 */
int cc_path_exists(const char *path)
{
    if (!path || !path[0]) return 0;
    return access(path, F_OK) == 0;
}
