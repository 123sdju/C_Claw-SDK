



#include "cc/ports/cc_path.h"

#ifdef ESP_PLATFORM
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "cc/internal/cc_alloc.h"

#ifndef PATH_MAX
#define PATH_MAX 256
#endif

/**
 * @brief 以斜杠为根词法绝对化路径，折叠点段和多余分隔符。
 * @param[in] path 借用 NUL 结尾路径，NULL 返回 NULL，空串归一为斜杠根。
 * @return 返回调用方 free 的独立字符串，输入或输出容量超限、组件表满或分配失败返回 NULL。
 * @note 相对路径直接加根斜杠，不读取 cwd、目标存在性或链接，固定 PATH_MAX 缓冲始终完整检查。
 */
static char *normalize_absolute_path(const char *path)
{
    char input[PATH_MAX];
    if (!path) return NULL;
    size_t path_size = strlen(path);
    if (sizeof(input) < 2 || path_size >= sizeof(input)) return NULL;
    if (path[0] == '/') {
        memcpy(input, path, path_size + 1);
    } else {
        if (path_size > sizeof(input) - 2) return NULL;
        input[0] = '/';
        memcpy(input + 1, path, path_size + 1);
    }

    char *parts[PATH_MAX / 2];
    size_t count = 0;
    char work[PATH_MAX];
    memcpy(work, input, strlen(input) + 1);
    char *save = NULL;
    char *token = strtok_r(work, "/", &save);
    while (token) {
        if (strcmp(token, ".") == 0 || token[0] == '\0') {
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
        size_t available = sizeof(out) - pos - 1;
        size_t separator_size = i > 0 ? 1 : 0;
        if (separator_size > available || len > available - separator_size) return NULL;
        if (i > 0) out[pos++] = '/';
        memcpy(out + pos, parts[i], len);
        pos += len;
    }
    out[pos] = '\0';
    return cc_copy_string(out);
}

/**
 * @brief 按已有 ESP32 规则复制参数或在两串之间补斜杠。
 * @param[in] base 借用基路径，NULL 时复制 child。
 * @param[in] child 借用子路径，NULL 时复制 base，非 NULL 空串或绝对串沿原规则参与拼接。
 * @return 返回调用方 free 的字符串，双 NULL、长度溢出或分配失败返回 NULL。
 * @note 仅进行字符串拼接，文件系统根约束由实际 I/O 入口承担。
 */
char *cc_path_join(const char *base, const char *child)
{
    if (!base) return child ? cc_copy_string(child) : NULL;
    if (!child) return cc_copy_string(base);
    size_t base_len = strlen(base);
    size_t child_len = strlen(child);
    if (base_len > SIZE_MAX - child_len || base_len + child_len == SIZE_MAX) return NULL;
    int slash = base_len > 0 && base[base_len - 1] == '/';
    size_t padding = slash ? 1 : 2;
    if (base_len + child_len > SIZE_MAX - padding) return NULL;
    size_t len = base_len + child_len + padding;
    char *out = (char *)malloc(len);
    if (!out) return NULL;
    strcpy(out, base);
    if (!slash) strcat(out, "/");
    strcat(out, child);
    return out;
}

/**
 * @brief 返回 ESP32 词法绝对化结果，沿用空串为根的规则。
 * @param[in] path 调用期借用路径，NULL 返回 NULL。
 * @return 成功交付调用方 free 的路径，容量或分配失败返回 NULL。
 * @note 不调用 realpath，不能据此判断链接、挂载或并发变化后的实际对象。
 */
char *cc_path_canonical(const char *path)
{
    if (!path) return NULL;
    return normalize_absolute_path(path);
}

/**
 * @brief 两边词法绝对化后判断同根或完整组件前缀。
 * @param[in] base_dir 借用根字符串，NULL 返回 0。
 * @param[in] path 借用目标字符串，NULL 返回 0。
 * @return 规范化后同根或根内返回 1，超限、分配失败或不匹配返回 0。
 * @note 斜杠根包含所有词法绝对路径，本函数不固定文件系统对象。
 */
int cc_path_is_within(const char *base_dir, const char *path)
{
    if (!base_dir || !path) return 0;
    char *base = cc_path_canonical(base_dir);
    char *target = cc_path_canonical(path);
    if (!base || !target) {
        free(base);
        free(target);
        return 0;
    }
    size_t n = strlen(base);
    int ok = strncmp(target, base, n) == 0 &&
        (base[n - 1] == '/' || target[n] == '/' || target[n] == '\0');
    free(base);
    free(target);
    return ok;
}

/**
 * @brief 词法截取最后一个斜杠之前的父目录。
 * @param[in] path 借用路径，NULL 返回 NULL，保留末尾斜杠和空输入的既有截取规则。
 * @return 返回调用方 free 的父目录，首斜杠为末分隔符时返回根，无分隔符返回点，分配失败返回 NULL。
 */
char *cc_path_dirname(const char *path)
{
    if (!path) return NULL;
    char *copy = cc_copy_string(path);
    if (!copy) return NULL;
    char *slash = strrchr(copy, '/');
    if (!slash) {
        free(copy);
        return cc_copy_string(".");
    }
    if (slash == copy) {
        free(copy);
        return cc_copy_string("/");
    }
    *slash = '\0';
    return copy;
}

/**
 * @brief 通过 VFS access 查询是否能确认路径存在。
 * @param[in] path 借用路径，NULL 返回 0。
 * @return access 成功返回 1，其余返回 0，不提供详细错误。
 */
int cc_path_exists(const char *path)
{
    return path && access(path, F_OK) == 0;
}
#else
#error "cc_esp32_path.c must be built under ESP-IDF"
#endif
