



#include "cc/ports/cc_path.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef CC_FREERTOS_PATH_MAX
#define CC_FREERTOS_PATH_MAX 256
#endif

/* FreeRTOS 裁剪环境可能没有 strdup，因此提供本文件私有复制函数。 */
static char *freertos_copy_string(const char *text)
{
    if (!text) return NULL;
    size_t len = strlen(text) + 1;
    char *copy = malloc(len);
    if (copy) memcpy(copy, text, len);
    return copy;
}

/* 拼接 base/child，返回字符串由调用方释放。 */
char *cc_path_join(const char *base, const char *child)
{
    if (!base) return freertos_copy_string(child);
    if (!child) return freertos_copy_string(base);
    size_t base_len = strlen(base);
    int slash = base_len > 0 && base[base_len - 1] == '/';
    size_t len = base_len + strlen(child) + (slash ? 0U : 1U) + 1U;
    char *out = malloc(len);
    if (!out) return NULL;
    strcpy(out, base);
    if (!slash) strcat(out, "/");
    strcat(out, child);
    return out;
}

/*
 * FreeRTOS 词法 canonical。
 *
 * 没有依赖 realpath/getcwd，只处理路径中的 `.` 和 `..`；适合无 POSIX 文件系统的 MCU
 * profile，但不能解析符号链接。
 */
char *cc_path_canonical(const char *path)
{
    if (!path) return NULL;
    char work[CC_FREERTOS_PATH_MAX];
    snprintf(work, sizeof(work), "%s", path);
    char *parts[CC_FREERTOS_PATH_MAX / 2];
    size_t count = 0;
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

    char out[CC_FREERTOS_PATH_MAX];
    size_t pos = 0;
    if (path[0] == '/') out[pos++] = '/';
    for (size_t i = 0; i < count; i++) {
        size_t len = strlen(parts[i]);
        if (pos + len + 2 >= sizeof(out)) return NULL;
        if (i > 0) out[pos++] = '/';
        memcpy(out + pos, parts[i], len);
        pos += len;
    }
    if (pos == 0) out[pos++] = '.';
    out[pos] = '\0';
    return freertos_copy_string(out);
}

/*
 * 判断 path 是否位于 base_dir 下。
 *
 * 该实现是轻量 prefix 检查，假设调用方已经对路径做 canonical；在真实文件系统上要注意
 * 符号链接和挂载点边界。
 */
int cc_path_is_within(const char *base_dir, const char *path)
{
    if (!base_dir || !path) return 0;
    size_t n = strlen(base_dir);
    return strncmp(path, base_dir, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

/* 返回父目录字符串；调用方负责 free。 */
char *cc_path_dirname(const char *path)
{
    if (!path) return NULL;
    char *copy = freertos_copy_string(path);
    if (!copy) return NULL;
    char *slash = strrchr(copy, '/');
    if (!slash) {
        free(copy);
        return freertos_copy_string(".");
    }
    if (slash == copy) {
        free(copy);
        return freertos_copy_string("/");
    }
    *slash = '\0';
    return copy;
}

/*
 * FreeRTOS 默认无统一文件存在 API。
 *
 * 当前返回 0，具体产品可在 filesystem adapter 中接入 FATFS/LittleFS 后重写该能力。
 */
int cc_path_exists(const char *path)
{
    (void)path;
    return 0;
}
