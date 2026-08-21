



#include "cc/ports/cc_path.h"

#ifdef ESP_PLATFORM
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "cc/internal/cc_alloc.h"

#ifndef PATH_MAX
#define PATH_MAX 256
#endif

/*
 * ESP32 上对路径做词法绝对化。
 *
 * SPIFFS/FATFS 场景下目标文件可能不存在，ESP-IDF 也没有 POSIX realpath；
 * 这里只做词法归一化，用 `.`/`..` 折叠来支持 workspace 边界检查。
 */
static char *normalize_absolute_path(const char *path)
{
    char input[PATH_MAX];
    if (!path) return NULL;
    if (path[0] == '/') {
        snprintf(input, sizeof(input), "%s", path);
    } else {
        if (strlen(path) + 2 > sizeof(input)) return NULL;
        input[0] = '/';
        memcpy(input + 1, path, strlen(path) + 1);
    }

    char *parts[PATH_MAX / 2];
    size_t count = 0;
    char work[PATH_MAX];
    snprintf(work, sizeof(work), "%s", input);
    char *save = NULL;
    char *token = strtok_r(work, "/", &save);
    while (token) {
        if (strcmp(token, ".") == 0 || token[0] == '\0') {
        } else if (strcmp(token, "..") == 0) {
            if (count > 0) count--;
        } else if (count < sizeof(parts) / sizeof(parts[0])) {
            parts[count++] = token;
        }
        token = strtok_r(NULL, "/", &save);
    }

    char out[PATH_MAX];
    size_t pos = 0;
    out[pos++] = '/';
    for (size_t i = 0; i < count; i++) {
        size_t len = strlen(parts[i]);
        if (pos + len + 2 >= sizeof(out)) return NULL;
        if (i > 0) out[pos++] = '/';
        memcpy(out + pos, parts[i], len);
        pos += len;
    }
    out[pos] = '\0';
    return cc_copy_string(out);
}

/* 拼接 base/child，返回字符串由调用方释放；不在这里做安全检查。 */
char *cc_path_join(const char *base, const char *child)
{
    if (!base) return child ? cc_copy_string(child) : NULL;
    if (!child) return cc_copy_string(base);
    size_t base_len = strlen(base);
    int slash = base_len > 0 && base[base_len - 1] == '/';
    size_t len = base_len + strlen(child) + (slash ? 0 : 1) + 1;
    char *out = (char *)malloc(len);
    if (!out) return NULL;
    strcpy(out, base);
    if (!slash) strcat(out, "/");
    strcat(out, child);
    return out;
}

/*
 * canonical path。
 *
 * ESP32 使用词法归一化，保证写入前的 parent/workspace 检查仍然可用。
 */
char *cc_path_canonical(const char *path)
{
    if (!path) return NULL;
    return normalize_absolute_path(path);
}

/* 判断目标路径是否位于 base_dir 内，使用 canonical 后的 prefix + 分隔符检查。 */
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
    int ok = strncmp(target, base, n) == 0 && (target[n] == '/' || target[n] == '\0');
    free(base);
    free(target);
    return ok;
}

/* 返回父目录字符串；调用方负责 free。 */
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

/* ESP32 文件存在性检查，底层依赖 VFS access。 */
int cc_path_exists(const char *path)
{
    return path && access(path, F_OK) == 0;
}
#else
#error "cc_esp32_path.c must be built under ESP-IDF"
#endif
