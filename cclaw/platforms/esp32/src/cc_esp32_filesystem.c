



#include "cc/ports/cc_filesystem.h"

#ifdef ESP_PLATFORM
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cc/internal/cc_alloc.h"

/*
 * ESP32 filesystem 私有状态。
 *
 * 当前通过 ESP-IDF VFS 的 stdio/dirent 接口实现，因此不需要额外状态；保留 self 是为了
 * 满足统一 filesystem vtable。
 */
typedef struct cc_esp32_filesystem {
    int dummy;
} cc_esp32_filesystem_t;

/*
 * 判断字符是否为 ASCII 字母（A-Z 或 a-z）。
 *
 * 参数：ch - 待判断字符。
 * 返回：1 表示是字母，0 表示不是。
 */
static int esp32_ascii_letter(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

/*
 * 判断路径是否为宿主机风格路径（含盘符或反斜杠）。
 *
 * ESP32 的 VFS 不支持 Windows 风格路径，此函数用于路径校验时提前拒绝。
 * 参数：path - 文件路径。
 * 返回：1 表示是宿主机路径，0 表示不是。
 */
static int esp32_host_path(const char *path)
{
    if (!path || !path[0]) {
        return 0;
    }
    return (esp32_ascii_letter(path[0]) && path[1] == ':') || strchr(path, '\\') != NULL;
}

/*
 * 校验 ESP32 文件系统路径合法性。
 *
 * 拒绝 null、空字符串和宿主机风格路径。
 * 参数：path - 文件路径。
 * 返回：cc_result_t 校验结果。
 */
static cc_result_t esp32_validate_path(const char *path)
{
    if (!path || !path[0]) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid filesystem path");
    }
    if (esp32_host_path(path)) {
        return cc_result_errf(CC_ERR_INVALID_ARGUMENT, "Host path is not valid on ESP32: %s", path);
    }
    return cc_result_ok();
}

/*
 * 读取整个文本文件。
 *
 * 适合小配置/上下文文件；大文件读取应由上层 limits 限制，避免 MCU RAM 被一次性占满。
 */
static cc_result_t esp32_read_text(void *self, const char *path, char **out_text)
{
    (void)self;
    cc_result_t rc = esp32_validate_path(path);
    if (rc.code != CC_OK) return rc;
    FILE *f = fopen(path, "rb");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file: %s", path);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return cc_result_error(CC_ERR_IO, "Failed to determine file size");
    }
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate file buffer");
    }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';
    *out_text = buf;
    return cc_result_ok();
}

/* 覆盖写入原始字节；路径安全检查由工具层完成，平台层只负责 VFS I/O。 */
static cc_result_t esp32_write_bytes(void *self,
                                     const char *path,
                                     const void *data,
                                     size_t size)
{
    (void)self;
    cc_result_t rc = esp32_validate_path(path);
    if (rc.code != CC_OK) return rc;
    if (!data && size > 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid write_bytes data");
    }
    FILE *f = fopen(path, "wb");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file for writing: %s", path);
    size_t written = size > 0 ? fwrite(data, 1, size, f) : 0;
    int write_failed = (written != size || ferror(f));
    int close_failed = (fclose(f) != 0);
    if (write_failed) return cc_result_error(CC_ERR_IO, "Failed to write all data");
    if (close_failed) return cc_result_errf(CC_ERR_IO, "Cannot close file after writing: %s", path);
    return cc_result_ok();
}

/* 覆盖写入文本文件；委托二进制接口处理实际 VFS 写入。 */
static cc_result_t esp32_write_text(void *self, const char *path, const char *text)
{
    const char *safe_text = text ? text : "";
    return esp32_write_bytes(self, path, safe_text, strlen(safe_text));
}

static cc_result_t esp32_read_bytes(
    void *self, const char *path, void **out_data, size_t *out_size)
{
    (void)self;
    if (!out_data || !out_size) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid binary read");
    *out_data = NULL;
    *out_size = 0;
    cc_result_t rc = esp32_validate_path(path);
    if (rc.code != CC_OK) return rc;
    FILE *f = fopen(path, "rb");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file: %s", path);
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return cc_result_error(CC_ERR_IO, "Cannot seek file"); }
    long end = ftell(f);
    if (end < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return cc_result_error(CC_ERR_IO, "Cannot size file"); }
    size_t size = (size_t)end;
    void *data = malloc(size ? size : 1);
    if (!data) { fclose(f); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate file buffer"); }
    size_t got = size ? fread(data, 1, size, f) : 0;
    int failed = got != size || ferror(f);
    if (fclose(f) != 0) failed = 1;
    if (failed) { free(data); return cc_result_error(CC_ERR_IO, "Failed to read complete file"); }
    *out_data = data;
    *out_size = size;
    return cc_result_ok();
}

static cc_result_t esp32_append_bytes(
    void *self, const char *path, const void *data, size_t size)
{
    (void)self;
    cc_result_t rc = esp32_validate_path(path);
    if (rc.code != CC_OK) return rc;
    if (!data && size > 0) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid append data");
    FILE *f = fopen(path, "ab");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file for append: %s", path);
    size_t written = size ? fwrite(data, 1, size, f) : 0;
    int failed = written != size || ferror(f);
    if (fclose(f) != 0) failed = 1;
    return failed ? cc_result_error(CC_ERR_IO, "Failed to append complete file") : cc_result_ok();
}

static cc_result_t esp32_stat_path(void *self, const char *path, cc_filesystem_stat_t *out_stat)
{
    (void)self;
    if (!out_stat) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null stat output");
    memset(out_stat, 0, sizeof(*out_stat));
    out_stat->size = sizeof(*out_stat);
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT) return cc_result_ok();
        return cc_result_errf(CC_ERR_IO, "Cannot stat path: %s", path);
    }
    out_stat->type = S_ISDIR(st.st_mode) ? CC_FS_ENTRY_DIRECTORY : CC_FS_ENTRY_FILE;
    out_stat->byte_size = (uint64_t)st.st_size;
    out_stat->modified_ms = (uint64_t)st.st_mtime * 1000ULL;
    return cc_result_ok();
}

static cc_result_t esp32_atomic_replace(void *self, const char *temporary_path, const char *destination_path)
{
    (void)self;
    if (rename(temporary_path, destination_path) != 0) {
        return cc_result_errf(CC_ERR_IO, "Cannot atomically replace destination: %s", destination_path);
    }
    return cc_result_ok();
}

static cc_result_t esp32_sync_file(void *self, const char *path)
{
    (void)self;
    FILE *f = fopen(path, "rb");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file for sync: %s", path);
    int failed = fsync(fileno(f)) != 0;
    fclose(f);
    return failed ? cc_result_error(CC_ERR_IO, "File sync failed") : cc_result_ok();
}

static cc_result_t esp32_sync_dir(void *self, const char *path)
{
    (void)self; (void)path;
    return cc_result_error(CC_ERR_UNSUPPORTED, "Directory sync is unavailable on this ESP VFS");
}

static int esp32_relative_valid(const char *path)
{
    if (!path || !path[0] || path[0] == '/' || strchr(path, '\\')) return 0;
    const char *segment = path;
    for (const char *p = path;; p++) {
        if (*p == '/' || *p == '\0') {
            size_t len = (size_t)(p - segment);
            if (len == 0 || (len == 1 && segment[0] == '.') ||
                (len == 2 && segment[0] == '.' && segment[1] == '.')) return 0;
            if (*p == '\0') break;
            segment = p + 1;
        }
    }
    return 1;
}

static char *esp32_join_relative(const char *root, const char *relative_path)
{
    if (!root || !esp32_relative_valid(relative_path)) return NULL;
    size_t a = strlen(root), b = strlen(relative_path);
    if (a > (size_t)-1 - b - 2) return NULL;
    char *path = malloc(a + b + 2);
    if (!path) return NULL;
    memcpy(path, root, a);
    if (a > 0 && root[a - 1] != '/') path[a++] = '/';
    memcpy(path + a, relative_path, b + 1);
    return path;
}

static cc_result_t esp32_read_relative_text(
    void *self, const char *root, const char *relative_path, char **out_text)
{
    char *path = esp32_join_relative(root, relative_path);
    if (!path) return cc_result_error(CC_ERR_PERMISSION_DENIED, "Unsafe workspace path");
    cc_result_t rc = esp32_read_text(self, path, out_text);
    free(path);
    return rc;
}

static cc_result_t esp32_write_relative_text(
    void *self, const char *root, const char *relative_path, const char *text)
{
    char *path = esp32_join_relative(root, relative_path);
    if (!path) return cc_result_error(CC_ERR_PERMISSION_DENIED, "Unsafe workspace path");
    cc_result_t rc = esp32_write_text(self, path, text);
    free(path);
    return rc;
}

static cc_result_t esp32_write_relative_bytes(
    void *self, const char *root, const char *relative_path, const void *data, size_t size)
{
    char *path = esp32_join_relative(root, relative_path);
    if (!path) return cc_result_error(CC_ERR_PERMISSION_DENIED, "Unsafe workspace path");
    cc_result_t rc = esp32_write_bytes(self, path, data, size);
    free(path);
    return rc;
}

/* 判断路径是否存在，底层使用 ESP-IDF VFS access。 */
static cc_result_t esp32_exists(void *self, const char *path, int *out_exists)
{
    (void)self;
    cc_result_t rc = esp32_validate_path(path);
    if (rc.code != CC_OK) return rc;
    *out_exists = (path && access(path, F_OK) == 0) ? 1 : 0;
    return cc_result_ok();
}

/*
 * 列举目录项。
 *
 * 返回数组由调用方逐项 free 后释放数组；容量从 8 开始，控制 MCU 上的初始内存占用。
 */
static cc_result_t esp32_list_dir(void *self, const char *path, char ***out_items, size_t *out_count)
{
    (void)self;
    cc_result_t rc = esp32_validate_path(path);
    if (rc.code != CC_OK) return rc;
    DIR *dir = opendir(path);
    if (!dir) return cc_result_errf(CC_ERR_IO, "Cannot open directory: %s", path);

    size_t cap = 8;
    size_t count = 0;
    char **items = (char **)calloc(cap, sizeof(char *));
    if (!items) {
        closedir(dir);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate directory list");
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (count >= cap) {
            cap *= 2;
            char **next = (char **)realloc(items, cap * sizeof(char *));
            if (!next) {
                for (size_t i = 0; i < count; i++) free(items[i]);
                free(items);
                closedir(dir);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow directory list");
            }
            items = next;
        }
        items[count] = cc_copy_string(entry->d_name);
        if (!items[count]) {
            for (size_t i = 0; i < count; i++) free(items[i]);
            free(items);
            closedir(dir);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy directory entry");
        }
        count++;
    }
    closedir(dir);
    *out_items = items;
    *out_count = count;
    return cc_result_ok();
}

/*
 * 递归创建目录。
 *
 * ESP32 文件系统可能是 SPIFFS/FATFS/VFS，逐级 mkdir 并把 EEXIST 视为成功。
 */
static int esp32_is_dir(const char *path)
{
    struct stat st;
    if (!path) {
        return 0;
    }
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return 1;
    }
    DIR *dir = opendir(path);
    if (dir) {
        closedir(dir);
        return 1;
    }
    return 0;
}

/*
 * 创建单级目录，兼容 SPIFFS/FATFS/VFS 多种文件系统。
 *
 * 已存在的目录视为成功，mkdir 返回 EEXIST 时通过 stat/opendir 双重确认。
 * 参数：path - 目录路径。
 * 返回：cc_result_t 结果。
 */
static cc_result_t esp32_make_one_dir(const char *path)
{
    if (esp32_is_dir(path)) {
        return cc_result_ok();
    }
    if (mkdir(path, 0755) == 0) {
        return cc_result_ok();
    }
    int mkdir_errno = errno;
    if ((mkdir_errno == EEXIST && esp32_is_dir(path)) || esp32_is_dir(path)) {
        return cc_result_ok();
    }
    return cc_result_errf(CC_ERR_IO, "Cannot create directory: %s", path);
}

/*
 * 递归创建多级目录。
 *
 * 按路径中的 '/' 逐级调用 esp32_make_one_dir，容忍 EEXIST。
 * 参数：self - filesystem 私有对象；path - 目录路径。
 * 返回：cc_result_t 结果。
 */
static cc_result_t esp32_make_dir(void *self, const char *path)
{
    (void)self;
    cc_result_t valid = esp32_validate_path(path);
    if (valid.code != CC_OK) return valid;

    char *tmp = cc_copy_string(path);
    if (!tmp) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate path");
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (tmp[0]) {
            cc_result_t rc = esp32_make_one_dir(tmp);
            if (rc.code != CC_OK) {
                free(tmp);
                return rc;
            }
        }
        *p = '/';
    }
    cc_result_t rc = esp32_make_one_dir(tmp);
    if (rc.code != CC_OK) {
        free(tmp);
        return rc;
    }
    free(tmp);
    return cc_result_ok();
}

/* 删除文件或空目录；高风险删除策略不在平台层处理。 */
static cc_result_t esp32_remove(void *self, const char *path)
{
    (void)self;
    cc_result_t rc = esp32_validate_path(path);
    if (rc.code != CC_OK) return rc;
    if (remove(path) != 0)
        return cc_result_errf(CC_ERR_IO, "Cannot remove: %s", path);
    return cc_result_ok();
}

/* 销毁 ESP32 filesystem 私有对象。 */
static void esp32_destroy(void *self)
{
    free(self);
}

/* ESP32 filesystem vtable。 */
static cc_filesystem_vtable_t esp32_vtable = {
    .size = sizeof(cc_filesystem_vtable_t),
    .version = CC_FILESYSTEM_VTABLE_VERSION,
    .capabilities = CC_FS_CAP_BINARY | CC_FS_CAP_APPEND | CC_FS_CAP_STAT |
        CC_FS_CAP_ATOMIC_REPLACE | CC_FS_CAP_FILE_SYNC | CC_FS_CAP_ROOT_SCOPED |
        CC_FS_CAP_NO_SYMLINKS,
    .read_text = esp32_read_text,
    .write_text = esp32_write_text,
    .write_bytes = esp32_write_bytes,
    .exists = esp32_exists,
    .list_dir = esp32_list_dir,
    .make_dir = esp32_make_dir,
    .remove = esp32_remove,
    .destroy = esp32_destroy,
    .read_bytes = esp32_read_bytes,
    .append_bytes = esp32_append_bytes,
    .stat = esp32_stat_path,
    .atomic_replace = esp32_atomic_replace,
    .sync_file = esp32_sync_file,
    .sync_dir = esp32_sync_dir,
    .read_relative_text = esp32_read_relative_text,
    .write_relative_text = esp32_write_relative_text,
    .write_relative_bytes = esp32_write_relative_bytes,
};

/*
 * 创建 ESP32 默认 filesystem 端口。
 *
 * 成功后 out_fs 持有 self/vtable；VFS 挂载应由应用或平台初始化代码提前完成。
 */
cc_result_t cc_filesystem_get_default(cc_filesystem_t *out_fs)
{
    if (!out_fs) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null filesystem output");
    }
    memset(out_fs, 0, sizeof(*out_fs));
    cc_esp32_filesystem_t *self = (cc_esp32_filesystem_t *)calloc(1, sizeof(cc_esp32_filesystem_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create ESP32 filesystem");
    out_fs->self = self;
    out_fs->vtable = &esp32_vtable;
    out_fs->size = sizeof(*out_fs);
    out_fs->version = esp32_vtable.version;
    out_fs->capabilities = esp32_vtable.capabilities;
    return cc_result_ok();
}

/* 兼容 POSIX 命名入口，在 ESP32 profile 中返回同一个默认 filesystem。 */
cc_result_t cc_filesystem_get_posix(cc_filesystem_t *out_fs)
{
    return cc_filesystem_get_default(out_fs);
}
#else
#error "cc_esp32_filesystem.c must be built under ESP-IDF"
#endif
