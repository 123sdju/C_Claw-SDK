



#include "cc/ports/cc_filesystem.h"
#include "cc/internal/cc_alloc.h"

#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Windows filesystem 私有状态；当前无额外字段，保留 self 用于统一 vtable 生命周期。 */
typedef struct {
    int dummy;
} cc_windows_filesystem_t;

static cc_result_t windows_read_bytes(
    void *self, const char *path, void **out_data, size_t *out_size)
{
    (void)self;
    if (!path || !out_data || !out_size) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid binary read");
    *out_data = NULL;
    *out_size = 0;
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

/* 读取整个文本文件；out_text 成功后由调用方 free。 */
static cc_result_t windows_read_text(void *self, const char *path, char **out_text)
{
    (void)self;
    FILE *f = fopen(path, "rb");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file: %s", path);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate buffer"); }
    size_t n = fread(buf, 1, size, f);
    fclose(f);
    buf[n] = '\0';
    *out_text = buf;
    return cc_result_ok();
}

/* 覆盖写入原始字节；路径安全策略由工具层处理。 */
static cc_result_t windows_write_bytes(void *self,
                                       const char *path,
                                       const void *data,
                                       size_t size)
{
    (void)self;
    if (!path || (!data && size > 0)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid write_bytes arguments");
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

/* 覆盖写入文本文件；委托二进制接口保持一致的短写和关闭错误检查。 */
static cc_result_t windows_write_text(void *self, const char *path, const char *text)
{
    const char *safe_text = text ? text : "";
    return windows_write_bytes(self, path, safe_text, strlen(safe_text));
}

static cc_result_t windows_append_bytes(
    void *self, const char *path, const void *data, size_t size)
{
    (void)self;
    if (!path || (!data && size > 0)) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid append request");
    FILE *f = fopen(path, "ab");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file for append: %s", path);
    size_t written = size ? fwrite(data, 1, size, f) : 0;
    int failed = written != size || ferror(f);
    if (fclose(f) != 0) failed = 1;
    return failed ? cc_result_error(CC_ERR_IO, "Failed to append complete file") : cc_result_ok();
}

static cc_result_t windows_stat_path(void *self, const char *path, cc_filesystem_stat_t *out_stat)
{
    (void)self;
    if (!path || !out_stat) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid stat request");
    memset(out_stat, 0, sizeof(*out_stat));
    out_stat->size = sizeof(*out_stat);
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return cc_result_ok();
        return cc_result_errf(CC_ERR_IO, "Cannot stat path: %s", path);
    }
    out_stat->type = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ?
        CC_FS_ENTRY_DIRECTORY : CC_FS_ENTRY_FILE;
    out_stat->byte_size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    ULARGE_INTEGER ticks;
    ticks.HighPart = data.ftLastWriteTime.dwHighDateTime;
    ticks.LowPart = data.ftLastWriteTime.dwLowDateTime;
    out_stat->modified_ms = ticks.QuadPart / 10000ULL;
    return cc_result_ok();
}

static cc_result_t windows_atomic_replace(void *self, const char *temporary_path, const char *destination_path)
{
    (void)self;
    if (!temporary_path || !destination_path) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid atomic replace");
    if (!MoveFileExA(temporary_path, destination_path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return cc_result_errf(CC_ERR_IO, "Cannot atomically replace destination: %s", destination_path);
    }
    return cc_result_ok();
}

static cc_result_t windows_sync_file(void *self, const char *path)
{
    (void)self;
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return cc_result_errf(CC_ERR_IO, "Cannot open file for sync: %s", path);
    int ok = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    return ok ? cc_result_ok() : cc_result_error(CC_ERR_IO, "File sync failed");
}

static cc_result_t windows_sync_dir(void *self, const char *path)
{
    (void)self;
    (void)path;
    return cc_result_error(CC_ERR_UNSUPPORTED, "Windows directory sync is unavailable");
}

static int windows_relative_valid(const char *path)
{
    if (!path || !path[0] || path[0] == '/' || path[0] == '\\' ||
        (path[1] == ':' && ((path[0] >= 'A' && path[0] <= 'Z') ||
                           (path[0] >= 'a' && path[0] <= 'z')))) return 0;
    const char *segment = path;
    for (const char *p = path;; p++) {
        if (*p == '/' || *p == '\\' || *p == '\0') {
            size_t len = (size_t)(p - segment);
            if (len == 0 || (len == 1 && segment[0] == '.') ||
                (len == 2 && segment[0] == '.' && segment[1] == '.')) return 0;
            if (*p == '\0') break;
            segment = p + 1;
        }
    }
    return 1;
}

static int final_handle_is_within_root(HANDLE handle, const char *root)
{
    HANDLE root_handle = CreateFileA(root, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (root_handle == INVALID_HANDLE_VALUE) return 0;
    char root_path[4096];
    char target_path[4096];
    DWORD root_len = GetFinalPathNameByHandleA(root_handle, root_path, sizeof(root_path), FILE_NAME_NORMALIZED);
    DWORD target_len = GetFinalPathNameByHandleA(handle, target_path, sizeof(target_path), FILE_NAME_NORMALIZED);
    CloseHandle(root_handle);
    if (root_len == 0 || target_len == 0 || root_len >= sizeof(root_path) || target_len >= sizeof(target_path)) return 0;
    if (_strnicmp(root_path, target_path, root_len) != 0) return 0;
    return target_len == root_len || target_path[root_len] == '\\';
}

static char *windows_join_relative(const char *root, const char *relative_path)
{
    size_t root_len = strlen(root);
    size_t rel_len = strlen(relative_path);
    if (root_len > (size_t)-1 - rel_len - 2) return NULL;
    char *joined = malloc(root_len + rel_len + 2);
    if (!joined) return NULL;
    memcpy(joined, root, root_len);
    if (root_len > 0 && root[root_len - 1] != '/' && root[root_len - 1] != '\\') joined[root_len++] = '\\';
    memcpy(joined + root_len, relative_path, rel_len + 1);
    return joined;
}

static cc_result_t windows_read_relative_text(
    void *self, const char *root, const char *relative_path, char **out_text)
{
    (void)self;
    if (!root || !out_text || !windows_relative_valid(relative_path)) {
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "Unsafe workspace path");
    }
    *out_text = NULL;
    char *path = windows_join_relative(root, relative_path);
    if (!path) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate workspace path");
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE || !final_handle_is_within_root(file, root)) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        free(path);
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "Workspace path escaped root");
    }
    LARGE_INTEGER length;
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 0 || (uint64_t)length.QuadPart > SIZE_MAX - 1) {
        CloseHandle(file); free(path); return cc_result_error(CC_ERR_IO, "Cannot size workspace file");
    }
    size_t size = (size_t)length.QuadPart;
    char *text = malloc(size + 1);
    if (!text) { CloseHandle(file); free(path); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate file text"); }
    size_t offset = 0;
    while (offset < size) {
        DWORD chunk = (DWORD)((size - offset) > 0x7fffffffU ? 0x7fffffffU : size - offset);
        DWORD got = 0;
        if (!ReadFile(file, text + offset, chunk, &got, NULL) || got == 0) {
            free(text); CloseHandle(file); free(path); return cc_result_error(CC_ERR_IO, "Failed to read workspace file");
        }
        offset += got;
    }
    text[size] = '\0';
    CloseHandle(file);
    free(path);
    *out_text = text;
    return cc_result_ok();
}

static cc_result_t windows_write_relative_text(
    void *self, const char *root, const char *relative_path, const char *text)
{
    (void)self;
    if (!root || !windows_relative_valid(relative_path)) {
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "Unsafe workspace path");
    }
    char *path = windows_join_relative(root, relative_path);
    if (!path) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate workspace path");
    HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE || !final_handle_is_within_root(file, root)) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        free(path);
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "Workspace path escaped root");
    }
    SetFilePointer(file, 0, NULL, FILE_BEGIN);
    if (!SetEndOfFile(file)) { CloseHandle(file); free(path); return cc_result_error(CC_ERR_IO, "Failed to truncate workspace file"); }
    const char *data = text ? text : "";
    size_t size = strlen(data);
    size_t offset = 0;
    while (offset < size) {
        DWORD chunk = (DWORD)((size - offset) > 0x7fffffffU ? 0x7fffffffU : size - offset);
        DWORD written = 0;
        if (!WriteFile(file, data + offset, chunk, &written, NULL) || written == 0) {
            CloseHandle(file); free(path); return cc_result_error(CC_ERR_IO, "Failed to write workspace file");
        }
        offset += written;
    }
    int ok = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    free(path);
    return ok ? cc_result_ok() : cc_result_error(CC_ERR_IO, "Workspace file sync failed");
}

static cc_result_t windows_write_relative_bytes(
    void *self, const char *root, const char *relative_path, const void *data, size_t size)
{
    (void)self;
    if ((size > 0 && !data) || !root || !windows_relative_valid(relative_path)) {
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "Unsafe workspace path or binary data");
    }
    char *path = windows_join_relative(root, relative_path);
    if (!path) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate workspace path");
    HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE || !final_handle_is_within_root(file, root)) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        free(path);
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "Workspace path escaped root");
    }
    SetFilePointer(file, 0, NULL, FILE_BEGIN);
    if (!SetEndOfFile(file)) {
        CloseHandle(file); free(path);
        return cc_result_error(CC_ERR_IO, "Failed to truncate workspace file");
    }
    size_t offset = 0;
    while (offset < size) {
        DWORD chunk = (DWORD)((size - offset) > 0x7fffffffU ? 0x7fffffffU : size - offset);
        DWORD written = 0;
        if (!WriteFile(file, (const unsigned char *)data + offset, chunk, &written, NULL) ||
            written == 0) {
            CloseHandle(file); free(path);
            return cc_result_error(CC_ERR_IO, "Failed to write workspace bytes");
        }
        offset += written;
    }
    int ok = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    free(path);
    return ok ? cc_result_ok() : cc_result_error(CC_ERR_IO, "Workspace file sync failed");
}

/* 使用 GetFileAttributesA 查询路径存在性。 */
static cc_result_t windows_exists(void *self, const char *path, int *out_exists)
{
    (void)self;
    DWORD attr = GetFileAttributesA(path);
    *out_exists = (attr != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
    return cc_result_ok();
}

/*
 * 列举 Windows 目录项。
 *
 * 通过 FindFirstFile/FindNextFile 读取名称，返回数组由调用方逐项 free 后释放。
 */
static cc_result_t windows_list_dir(void *self, const char *path, char ***out_items, size_t *out_count)
{
    (void)self;
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*", path);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search_path, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return cc_result_errf(CC_ERR_IO, "Cannot open directory: %s", path);
    }

    size_t count = 0;
    size_t cap = 16;
    char **items = malloc(cap * sizeof(char *));
    if (!items) { FindClose(hFind); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate items"); }

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (count >= cap) {
            cap *= 2;
            items = realloc(items, cap * sizeof(char *));
            if (!items) { FindClose(hFind); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow items"); }
        }
        items[count++] = cc_copy_string(fd.cFileName);
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    *out_items = items;
    *out_count = count;
    return cc_result_ok();
}

/*
 * 递归创建目录。
 *
 * 兼容 '/' 和 '\\' 分隔符，ERROR_ALREADY_EXISTS 视为成功。
 */
static cc_result_t windows_make_dir(void *self, const char *path)
{
    (void)self;
    if (!path || !path[0])
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid directory path");

    char *tmp = cc_copy_string(path);
    if (!tmp) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate path");
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        char saved = *p;
        *p = '\0';
        if (tmp[0] && !CreateDirectoryA(tmp, NULL)) {
            DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS) {
                cc_result_t rc = cc_result_errf(CC_ERR_IO, "Cannot create directory: %s", tmp);
                free(tmp);
                return rc;
            }
        }
        *p = saved;
    }
    if (!CreateDirectoryA(tmp, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            cc_result_t rc = cc_result_errf(CC_ERR_IO, "Cannot create directory: %s", path);
            free(tmp);
            return rc;
        }
    }
    free(tmp);
    return cc_result_ok();
}

/* 删除文件；删除目录需要扩展或使用 RemoveDirectoryA。 */
static cc_result_t windows_remove(void *self, const char *path)
{
    (void)self;
    if (!DeleteFileA(path))
        return cc_result_errf(CC_ERR_IO, "Cannot remove: %s", path);
    return cc_result_ok();
}

/* 销毁 Windows filesystem 私有对象。 */
static void windows_destroy(void *self)
{
    free(self);
}

/* Windows filesystem vtable。 */
static cc_filesystem_vtable_t windows_vtable = {
    .size = sizeof(cc_filesystem_vtable_t),
    .version = CC_FILESYSTEM_VTABLE_VERSION,
    .capabilities = CC_FS_CAP_BINARY | CC_FS_CAP_APPEND | CC_FS_CAP_STAT |
        CC_FS_CAP_ATOMIC_REPLACE | CC_FS_CAP_FILE_SYNC | CC_FS_CAP_ROOT_SCOPED,
    .read_text = windows_read_text,
    .write_text = windows_write_text,
    .write_bytes = windows_write_bytes,
    .exists = windows_exists,
    .list_dir = windows_list_dir,
    .make_dir = windows_make_dir,
    .remove = windows_remove,
    .destroy = windows_destroy,
    .read_bytes = windows_read_bytes,
    .append_bytes = windows_append_bytes,
    .stat = windows_stat_path,
    .atomic_replace = windows_atomic_replace,
    .sync_file = windows_sync_file,
    .sync_dir = windows_sync_dir,
    .read_relative_text = windows_read_relative_text,
    .write_relative_text = windows_write_relative_text,
    .write_relative_bytes = windows_write_relative_bytes,
};

/*
 * Windows 下兼容 cc_filesystem_get_posix 名称，返回 Windows filesystem 实现。
 *
 * 成功后 out_fs 由调用方通过 vtable destroy 释放。
 */
cc_result_t cc_filesystem_get_posix(cc_filesystem_t *out_fs)
{
    if (!out_fs) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null filesystem output");
    }
    memset(out_fs, 0, sizeof(*out_fs));
    cc_windows_filesystem_t *self = calloc(1, sizeof(cc_windows_filesystem_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create windows filesystem");
    out_fs->self = self;
    out_fs->vtable = &windows_vtable;
    out_fs->size = sizeof(*out_fs);
    out_fs->version = windows_vtable.version;
    out_fs->capabilities = windows_vtable.capabilities;
    return cc_result_ok();
}

/* 默认 filesystem 入口。 */
cc_result_t cc_filesystem_get_default(cc_filesystem_t *out_fs)
{
    return cc_filesystem_get_posix(out_fs);
}

#endif
