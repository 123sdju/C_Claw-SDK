



#include "cc/ports/cc_path.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#ifndef CC_FREERTOS_PATH_MAX
#define CC_FREERTOS_PATH_MAX 256
#endif

/**
 * @brief 为裁剪平台复制 NUL 结尾字符串。
 * @param[in] text 调用期借用正文，NULL 返回 NULL。
 * @return 返回调用方 free 的独立副本，长度加 NUL 溢出或分配失败返回 NULL。
 */
static char *freertos_copy_string(const char *text)
{
    if (!text) return NULL;
    size_t length = strlen(text);
    if (length == SIZE_MAX) return NULL;
    size_t len = length + 1;
    char *copy = malloc(len);
    if (copy) memcpy(copy, text, len);
    return copy;
}

/**
 * @brief 沿已有规则复制参数或按需补斜杠拼接。
 * @param[in] base 借用基路径，NULL 时复制 child。
 * @param[in] child 借用子路径，NULL 时复制 base，非 NULL 空串和绝对串仍参与拼接。
 * @return 返回调用方 free 的路径，双 NULL、容量溢出或分配失败返回 NULL。
 */
char *cc_path_join(const char *base, const char *child)
{
    if (!base) return freertos_copy_string(child);
    if (!child) return freertos_copy_string(base);
    size_t base_len = strlen(base);
    size_t child_len = strlen(child);
    if (base_len > SIZE_MAX - child_len || base_len + child_len == SIZE_MAX) return NULL;
    int slash = base_len > 0 && base[base_len - 1] == '/';
    size_t padding = slash ? 1 : 2;
    if (base_len + child_len > SIZE_MAX - padding) return NULL;
    size_t len = base_len + child_len + padding;
    char *out = malloc(len);
    if (!out) return NULL;
    strcpy(out, base);
    if (!slash) strcat(out, "/");
    strcat(out, child);
    return out;
}

/**
 * @brief 在固定容量内折叠点段及多余斜杠，保留输入相对或绝对形式。
 * @param[in] path 借用路径，NULL 返回 NULL，空串及空相对结果返回点。
 * @return 返回调用方 free 的路径，输入、组件表或输出超限及分配失败返回 NULL。
 * @note 不读取 cwd 或文件系统，前导父段沿原规则在栈为空时丢弃，不能代替实际根相对访问。
 */
char *cc_path_canonical(const char *path)
{
    if (!path) return NULL;
    char work[CC_FREERTOS_PATH_MAX];
    size_t path_size = strlen(path);
    if (sizeof(work) < 2 || path_size >= sizeof(work)) return NULL;
    memcpy(work, path, path_size + 1);
    char *parts[CC_FREERTOS_PATH_MAX / 2];
    size_t count = 0;
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

    char out[CC_FREERTOS_PATH_MAX];
    size_t pos = 0;
    if (path[0] == '/') out[pos++] = '/';
    for (size_t i = 0; i < count; i++) {
        size_t len = strlen(parts[i]);
        size_t available = sizeof(out) - pos - 1;
        size_t separator_size = i > 0 ? 1 : 0;
        if (separator_size > available || len > available - separator_size) return NULL;
        if (i > 0) out[pos++] = '/';
        memcpy(out + pos, parts[i], len);
        pos += len;
    }
    if (pos == 0) {
        if (sizeof(out) < 2) return NULL;
        out[pos++] = '.';
    }
    out[pos] = '\0';
    return freertos_copy_string(out);
}

/**
 * @brief 直接按完整组件前缀比较已规范化的输入字符串。
 * @param[in] base_dir 借用根字符串，NULL 返回 0。
 * @param[in] path 借用目标字符串，NULL 返回 0。
 * @return 同串或根下返回 1，其余返回 0，基路径末斜杠可直接构成组件边界。
 * @pre 调用者已按需要规范化路径，本函数不作分配、链接解析或实际对象固定。
 */
int cc_path_is_within(const char *base_dir, const char *path)
{
    if (!base_dir || !path) return 0;
    size_t n = strlen(base_dir);
    return strncmp(path, base_dir, n) == 0 &&
        ((n > 0 && base_dir[n - 1] == '/') || path[n] == '/' || path[n] == '\0');
}

/**
 * @brief 按最后一个斜杠截取父目录，保留既有空串和尾斜杠规则。
 * @param[in] path 借用路径，NULL 返回 NULL。
 * @return 返回调用方 free 的路径，首斜杠为末分隔符返回根，无分隔符返回点，分配失败返回 NULL。
 */
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

/**
 * @brief 提供未接入通用存在查询时的缺省结果。
 * @param[in] path 当前不读取，可为 NULL。
 * @return 始终返回 0，具体文件存在性由挂载后的 filesystem 端口查询。
 */
int cc_path_exists(const char *path)
{
    (void)path;
    return 0;
}
