#include "cc/ports/cc_filesystem.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(CCLAW_FREERTOS_ENABLE_FATFS) && CCLAW_FREERTOS_ENABLE_FATFS

#include "ff.h"

/* FatFs filesystem 私有对象；当前无额外状态，保留 self 用于统一 vtable 生命周期。 */
typedef struct cc_freertos_fatfs {
    int unused;
} cc_freertos_fatfs_t;

static const char *const k_mount_prefix = "/sdcard";
static const char *const k_workspace_prefix = "/sdcard/cclaw/workspace";

/*
 * 将 SDK 路径映射到 FatFs 路径。
 *
 * SDK 使用类 POSIX 的 `/sdcard/...` 路径，FatFs 使用 `0:/...`；workspace 前缀会被映射到
 * FatFs 卷根下的相对路径，便于 MCU 配置固定工作区。
 */
static cc_result_t map_path(const char *path, char *out, size_t out_len)
{
    if (!path || !out || out_len == 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid filesystem path");
    }

    const char *rel = path;
    const char *fat_prefix = "0:/";
    size_t workspace_len = strlen(k_workspace_prefix);
    if (strncmp(path, k_workspace_prefix, workspace_len) == 0 &&
        (path[workspace_len] == '\0' || path[workspace_len] == '/')) {
        rel = path + workspace_len;
        while (*rel == '/') rel++;
        int n = *rel
            ? snprintf(out, out_len, "0:/%s", rel)
            : snprintf(out, out_len, "0:/");
        if (n < 0 || (size_t)n >= out_len) {
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Filesystem path is too long");
        }
        return cc_result_ok();
    }

    size_t prefix_len = strlen(k_mount_prefix);
    if (strncmp(path, k_mount_prefix, prefix_len) == 0) {
        rel = path + prefix_len;
    }
    while (*rel == '/') rel++;

    int n = *rel
        ? snprintf(out, out_len, "%s%s", fat_prefix, rel)
        : snprintf(out, out_len, "0:/");
    if (n < 0 || (size_t)n >= out_len) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Filesystem path is too long");
    }
    return cc_result_ok();
}

/*
 * 确保目标文件的父目录存在。
 *
 * FatFs 写文件遇到 FR_NO_PATH 时调用该 helper，逐级 f_mkdir；路径缓冲固定 256 字节，
 * 符合 FreeRTOS profile 的内存预算。
 */
static cc_result_t ensure_parent_dir(const char *fpath)
{
    char parent[256];
    size_t len = strlen(fpath);
    if (len >= sizeof(parent)) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Directory path is too long");
    strcpy(parent, fpath);

    char *slash = strrchr(parent + 3, '/');
    if (!slash) return cc_result_ok();
    *slash = '\0';

    for (char *p = parent + 3; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            FRESULT fr = f_mkdir(parent);
            if (fr != FR_OK && fr != FR_EXIST) return cc_result_error(CC_ERR_PLATFORM, "FatFs parent mkdir failed");
            *p = '/';
        }
    }
    FRESULT fr = f_mkdir(parent);
    if (fr != FR_OK && fr != FR_EXIST) return cc_result_error(CC_ERR_PLATFORM, "FatFs parent mkdir failed");
    return cc_result_ok();
}

/* 将 FatFs 错误码包装成 SDK cc_result_t，方便上层统一处理平台错误。 */
static cc_result_t fatfs_error(FRESULT res, const char *op)
{
    char msg[96];
    snprintf(msg, sizeof(msg), "%s failed: FatFs=%u", op, (unsigned)res);
    return cc_result_error(CC_ERR_PLATFORM, msg);
}

/*
 * FatFs 读取文本文件。
 *
 * 为保护 MCU RAM，单文件读取限制在 256 KiB；out_text 成功后由调用方 free。
 */
static cc_result_t fatfs_read(void *self, const char *path, char **out_text)
{
    (void)self;
    if (out_text) *out_text = NULL;

    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    if (rc.code != CC_OK) return rc;

    FIL file;
    FRESULT fr = f_open(&file, fpath, FA_READ);
    if (fr != FR_OK) return fatfs_error(fr, "f_open");

    FSIZE_t size = f_size(&file);
    if (size > 256U * 1024U) {
        f_close(&file);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "File is too large for STM32 text read");
    }

    char *text = malloc((size_t)size + 1U);
    if (!text) {
        f_close(&file);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate file buffer");
    }

    UINT got = 0;
    fr = f_read(&file, text, (UINT)size, &got);
    f_close(&file);
    if (fr != FR_OK) {
        free(text);
        return fatfs_error(fr, "f_read");
    }
    text[got] = '\0';
    if (out_text) *out_text = text;
    else free(text);
    return cc_result_ok();
}

/* FatFs 覆盖写入原始字节；父目录不存在时先创建再重试。 */
static cc_result_t fatfs_write_bytes(void *self,
                                     const char *path,
                                     const void *data,
                                     size_t size)
{
    (void)self;
    if (!data && size > 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid write_bytes data");
    }
    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    if (rc.code != CC_OK) return rc;

    UINT write_size = (UINT)size;
    if ((size_t)write_size != size) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "FatFs write exceeds UINT range");
    }

    FIL file;
    FRESULT fr = f_open(&file, fpath, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr == FR_NO_PATH) {
        rc = ensure_parent_dir(fpath);
        if (rc.code != CC_OK) return rc;
        fr = f_open(&file, fpath, FA_CREATE_ALWAYS | FA_WRITE);
    }
    if (fr != FR_OK) return fatfs_error(fr, "f_open");

    UINT wrote = 0;
    fr = f_write(&file, size > 0 ? data : "", write_size, &wrote);
    FRESULT close_fr = f_close(&file);
    if (fr != FR_OK) return fatfs_error(fr, "f_write");
    if (close_fr != FR_OK) return fatfs_error(close_fr, "f_close");
    if ((size_t)wrote != size) return cc_result_error(CC_ERR_PLATFORM, "Short FatFs write");
    return cc_result_ok();
}

/* FatFs 覆盖写入文本文件；委托二进制接口保持统一写入语义。 */
static cc_result_t fatfs_write(void *self, const char *path, const char *text)
{
    const char *safe_text = text ? text : "";
    return fatfs_write_bytes(self, path, safe_text, strlen(safe_text));
}

static cc_result_t fatfs_read_bytes(
    void *self, const char *path, void **out_data, size_t *out_size)
{
    (void)self;
    if (!out_data || !out_size) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid binary read");
    *out_data = NULL; *out_size = 0;
    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    if (rc.code != CC_OK) return rc;
    FIL file;
    FRESULT fr = f_open(&file, fpath, FA_READ);
    if (fr != FR_OK) return fatfs_error(fr, "f_open");
    FSIZE_t length = f_size(&file);
    if ((uint64_t)length > SIZE_MAX) { f_close(&file); return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "File is too large"); }
    size_t size = (size_t)length;
    void *data = malloc(size ? size : 1);
    if (!data) { f_close(&file); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate file data"); }
    UINT got = 0;
    fr = f_read(&file, data, (UINT)size, &got);
    FRESULT close_fr = f_close(&file);
    if (fr != FR_OK || close_fr != FR_OK || got != size) {
        free(data);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to read complete FatFs file");
    }
    *out_data = data; *out_size = size;
    return cc_result_ok();
}

static cc_result_t fatfs_append_bytes(
    void *self, const char *path, const void *data, size_t size)
{
    (void)self;
    if (!data && size > 0) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid append data");
    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    if (rc.code != CC_OK) return rc;
    FIL file;
    FRESULT fr = f_open(&file, fpath, FA_OPEN_APPEND | FA_WRITE);
    if (fr != FR_OK) return fatfs_error(fr, "f_open append");
    UINT written = 0;
    fr = f_write(&file, size ? data : "", (UINT)size, &written);
    if (fr == FR_OK) fr = f_sync(&file);
    FRESULT close_fr = f_close(&file);
    if (fr != FR_OK || close_fr != FR_OK || written != size) {
        return cc_result_error(CC_ERR_PLATFORM, "Failed to append complete FatFs file");
    }
    return cc_result_ok();
}

static cc_result_t fatfs_stat_path(void *self, const char *path, cc_filesystem_stat_t *out_stat)
{
    (void)self;
    if (!out_stat) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null stat output");
    memset(out_stat, 0, sizeof(*out_stat)); out_stat->size = sizeof(*out_stat);
    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    if (rc.code != CC_OK) return rc;
    FILINFO info;
    FRESULT fr = f_stat(fpath, &info);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) return cc_result_ok();
    if (fr != FR_OK) return fatfs_error(fr, "f_stat");
    out_stat->type = (info.fattrib & AM_DIR) ? CC_FS_ENTRY_DIRECTORY : CC_FS_ENTRY_FILE;
    out_stat->byte_size = (uint64_t)info.fsize;
    return cc_result_ok();
}

static cc_result_t fatfs_atomic_replace(void *self, const char *temporary_path, const char *destination_path)
{
    (void)self;
    char source[256], destination[256];
    cc_result_t rc = map_path(temporary_path, source, sizeof(source));
    if (rc.code != CC_OK) return rc;
    rc = map_path(destination_path, destination, sizeof(destination));
    if (rc.code != CC_OK) return rc;
    (void)f_unlink(destination);
    FRESULT fr = f_rename(source, destination);
    return fr == FR_OK ? cc_result_ok() : fatfs_error(fr, "f_rename");
}

static cc_result_t fatfs_sync_file(void *self, const char *path)
{
    (void)self;
    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    if (rc.code != CC_OK) return rc;
    FIL file;
    FRESULT fr = f_open(&file, fpath, FA_READ);
    if (fr != FR_OK) return fatfs_error(fr, "f_open sync");
    fr = f_sync(&file);
    f_close(&file);
    return fr == FR_OK ? cc_result_ok() : fatfs_error(fr, "f_sync");
}

static cc_result_t fatfs_sync_dir(void *self, const char *path)
{
    (void)self; (void)path;
    return cc_result_error(CC_ERR_UNSUPPORTED, "FatFs directory sync is unavailable");
}

static int fatfs_relative_valid(const char *path)
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

static cc_result_t fatfs_relative_path(
    const char *root, const char *relative_path, char *out, size_t out_size)
{
    if (!root || !fatfs_relative_valid(relative_path)) {
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "Unsafe workspace path");
    }
    int n = snprintf(out, out_size, "%s%s%s", root,
        root[strlen(root) - 1] == '/' ? "" : "/", relative_path);
    return n < 0 || (size_t)n >= out_size ?
        cc_result_error(CC_ERR_INVALID_ARGUMENT, "Workspace path is too long") : cc_result_ok();
}

static cc_result_t fatfs_read_relative_text(
    void *self, const char *root, const char *relative_path, char **out_text)
{
    char path[256];
    cc_result_t rc = fatfs_relative_path(root, relative_path, path, sizeof(path));
    return rc.code == CC_OK ? fatfs_read(self, path, out_text) : rc;
}

static cc_result_t fatfs_write_relative_text(
    void *self, const char *root, const char *relative_path, const char *text)
{
    char path[256];
    cc_result_t rc = fatfs_relative_path(root, relative_path, path, sizeof(path));
    return rc.code == CC_OK ? fatfs_write(self, path, text) : rc;
}

static cc_result_t fatfs_write_relative_bytes(
    void *self, const char *root, const char *relative_path, const void *data, size_t size)
{
    char path[256];
    cc_result_t rc = fatfs_relative_path(root, relative_path, path, sizeof(path));
    return rc.code == CC_OK ? fatfs_write_bytes(self, path, data, size) : rc;
}

/* FatFs 路径存在性检查；FR_NO_FILE/FR_NO_PATH 被视为 exists=0 且不是错误。 */
static cc_result_t fatfs_exists(void *self, const char *path, int *out_exists)
{
    (void)self;
    if (out_exists) *out_exists = 0;

    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    if (rc.code != CC_OK) return rc;

    FILINFO info;
    FRESULT fr = f_stat(fpath, &info);
    if (fr == FR_OK) {
        if (out_exists) *out_exists = 1;
        return cc_result_ok();
    }
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) return cc_result_ok();
    return fatfs_error(fr, "f_stat");
}

/*
 * FatFs 列举目录。
 *
 * 返回数组由调用方释放；按需逐项 realloc，适合小目录，超大目录应在产品层限制。
 */
static cc_result_t fatfs_list(void *self, const char *path, char ***out_items, size_t *out_count)
{
    (void)self;
    if (out_items) *out_items = NULL;
    if (out_count) *out_count = 0;

    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    if (rc.code != CC_OK) return rc;

    DIR dir;
    FRESULT fr = f_opendir(&dir, fpath);
    if (fr != FR_OK) return fatfs_error(fr, "f_opendir");

    char **items = NULL;
    size_t count = 0;
    for (;;) {
        FILINFO info;
        fr = f_readdir(&dir, &info);
        if (fr != FR_OK || info.fname[0] == '\0') break;
        char **next = realloc(items, sizeof(char *) * (count + 1));
        if (!next) {
            fr = FR_NOT_ENOUGH_CORE;
            break;
        }
        items = next;
        items[count] = cc_copy_string(info.fname);
        if (!items[count]) {
            fr = FR_NOT_ENOUGH_CORE;
            break;
        }
        count++;
    }
    f_closedir(&dir);

    if (fr != FR_OK) {
        for (size_t i = 0; i < count; i++) free(items[i]);
        free(items);
        return fatfs_error(fr, "f_readdir");
    }

    if (out_items) *out_items = items;
    else {
        for (size_t i = 0; i < count; i++) free(items[i]);
        free(items);
    }
    if (out_count) *out_count = count;
    return cc_result_ok();
}

/*
 * FatFs 递归创建目录。
 *
 * 逐级 f_mkdir，FR_EXIST 视为成功；用于初始化 workspace/data 目录。
 */
static cc_result_t fatfs_make_dir(void *self, const char *path)
{
    (void)self;
    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    if (rc.code != CC_OK) return rc;

    char partial[256];
    size_t len = strlen(fpath);
    if (len >= sizeof(partial)) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Directory path is too long");
    strcpy(partial, fpath);

    for (char *p = partial + 3; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            FRESULT fr = f_mkdir(partial);
            if (fr != FR_OK && fr != FR_EXIST) return fatfs_error(fr, "f_mkdir");
            *p = '/';
        }
    }
    FRESULT fr = f_mkdir(partial);
    if (fr != FR_OK && fr != FR_EXIST) return fatfs_error(fr, "f_mkdir");
    return cc_result_ok();
}

/* FatFs 删除文件或空目录；安全审批和 workspace 检查由上层工具/policy 完成。 */
static cc_result_t fatfs_remove(void *self, const char *path)
{
    (void)self;
    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    if (rc.code != CC_OK) return rc;
    FRESULT fr = f_unlink(fpath);
    if (fr != FR_OK) return fatfs_error(fr, "f_unlink");
    return cc_result_ok();
}

/* 销毁 FatFs filesystem 私有对象。 */
static void fatfs_destroy(void *self)
{
    free(self);
}

/* FreeRTOS FatFs vtable。 */
static cc_filesystem_vtable_t freertos_fs_vtable = {
    .size = sizeof(cc_filesystem_vtable_t),
    .version = CC_FILESYSTEM_VTABLE_VERSION,
    .capabilities = CC_FS_CAP_BINARY | CC_FS_CAP_APPEND | CC_FS_CAP_STAT |
        CC_FS_CAP_FILE_SYNC | CC_FS_CAP_ROOT_SCOPED |
        CC_FS_CAP_NO_SYMLINKS,
    .read_text = fatfs_read,
    .write_text = fatfs_write,
    .write_bytes = fatfs_write_bytes,
    .exists = fatfs_exists,
    .list_dir = fatfs_list,
    .make_dir = fatfs_make_dir,
    .remove = fatfs_remove,
    .destroy = fatfs_destroy,
    .read_bytes = fatfs_read_bytes,
    .append_bytes = fatfs_append_bytes,
    .stat = fatfs_stat_path,
    .atomic_replace = fatfs_atomic_replace,
    .sync_file = fatfs_sync_file,
    .sync_dir = fatfs_sync_dir,
    .read_relative_text = fatfs_read_relative_text,
    .write_relative_text = fatfs_write_relative_text,
    .write_relative_bytes = fatfs_write_relative_bytes,
};

#else

/* 未启用 FatFs 时，读取明确返回平台不支持，避免静默返回空数据。 */
static cc_result_t unsupported_read(void *self, const char *path, char **out_text)
{
    (void)self;
    (void)path;
    if (out_text) *out_text = NULL;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

/* 未挂载文件系统时，写入返回平台错误。 */
static cc_result_t unsupported_write(void *self, const char *path, const char *text)
{
    (void)self;
    (void)path;
    (void)text;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

/* 未挂载文件系统时，二进制写入同样返回平台错误。 */
static cc_result_t unsupported_write_bytes(void *self,
                                           const char *path,
                                           const void *data,
                                           size_t size)
{
    (void)self;
    (void)path;
    (void)data;
    (void)size;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

/* unsupported profile 下 exists 返回 0，允许上层把能力视为不可用而不是崩溃。 */
static cc_result_t unsupported_exists(void *self, const char *path, int *out_exists)
{
    (void)self;
    (void)path;
    if (out_exists) *out_exists = 0;
    return cc_result_ok();
}

/* 未挂载文件系统时，列目录返回平台错误并清空输出。 */
static cc_result_t unsupported_list(void *self, const char *path, char ***out_items, size_t *out_count)
{
    (void)self;
    (void)path;
    if (out_items) *out_items = NULL;
    if (out_count) *out_count = 0;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

/* 未挂载文件系统时，创建目录返回平台错误。 */
static cc_result_t unsupported_make_dir(void *self, const char *path)
{
    (void)self;
    (void)path;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

/* 未挂载文件系统时，删除返回平台错误。 */
static cc_result_t unsupported_remove(void *self, const char *path)
{
    (void)self;
    (void)path;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

/* 销毁 unsupported filesystem 占位对象。 */
static void unsupported_destroy(void *self)
{
    free(self);
}

/* FreeRTOS unsupported filesystem vtable，保持端口存在但能力显式失败。 */
static cc_filesystem_vtable_t freertos_fs_vtable = {
    .size = sizeof(cc_filesystem_vtable_t),
    .version = CC_FILESYSTEM_VTABLE_VERSION,
    .capabilities = 0,
    .read_text = unsupported_read,
    .write_text = unsupported_write,
    .write_bytes = unsupported_write_bytes,
    .exists = unsupported_exists,
    .list_dir = unsupported_list,
    .make_dir = unsupported_make_dir,
    .remove = unsupported_remove,
    .destroy = unsupported_destroy,
};

#endif

/*
 * 创建 FreeRTOS 默认 filesystem。
 *
 * 编译启用 FatFs 时返回 FatFs adapter，否则返回 unsupported adapter。这样 core 可以链接
 * 同一 filesystem port，而能力差异由运行时错误体现。
 */
cc_result_t cc_filesystem_get_default(cc_filesystem_t *out_fs)
{
    if (!out_fs) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid filesystem output");
#if defined(CCLAW_FREERTOS_ENABLE_FATFS) && CCLAW_FREERTOS_ENABLE_FATFS
    cc_freertos_fatfs_t *self = calloc(1, sizeof(*self));
#else
    int *self = calloc(1, sizeof(int));
#endif
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate filesystem");
    out_fs->self = self;
    out_fs->vtable = &freertos_fs_vtable;
    out_fs->size = sizeof(*out_fs);
    out_fs->version = freertos_fs_vtable.version;
    out_fs->capabilities = freertos_fs_vtable.capabilities;
    return cc_result_ok();
}

/* 兼容 POSIX 命名入口，FreeRTOS 下返回默认 filesystem。 */
cc_result_t cc_filesystem_get_posix(cc_filesystem_t *out_fs)
{
    return cc_filesystem_get_default(out_fs);
}
