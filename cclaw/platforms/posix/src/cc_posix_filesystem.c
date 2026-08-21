#include "cc/ports/cc_filesystem.h"
#include "cc/internal/cc_alloc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * POSIX filesystem 私有状态。
 *
 * 当前实现没有额外状态，保留 self 对象是为了符合 cc_filesystem_t 的 vtable/OOP
 * 结构，后续可在这里加入根目录、权限策略或测试注入状态。
 */
typedef struct {
    int dummy;
} cc_posix_filesystem_t;

static cc_result_t read_file_bytes(const char *path, void **out_data, size_t *out_size)
{
    if (!path || !out_data || !out_size) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid read_bytes arguments");
    }
    *out_data = NULL;
    *out_size = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file: %s", path);
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return cc_result_error(CC_ERR_IO, "Cannot seek file"); }
    long end = ftell(f);
    if (end < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return cc_result_error(CC_ERR_IO, "Cannot determine file size");
    }
    size_t size = (size_t)end;
    void *data = malloc(size ? size : 1);
    if (!data) { fclose(f); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate file buffer"); }
    size_t read_size = size ? fread(data, 1, size, f) : 0;
    int failed = read_size != size || ferror(f);
    if (fclose(f) != 0) failed = 1;
    if (failed) { free(data); return cc_result_error(CC_ERR_IO, "Failed to read complete file"); }
    *out_data = data;
    *out_size = size;
    return cc_result_ok();
}

static void free_items(char **items, size_t count)
{
    if (!items) return;
    for (size_t i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

/*
 * 读取整个文本文件。
 *
 * out_text 成功后由调用方 free。函数按二进制读取并补 '\0'，适用于配置、源码等文本
 * 内容。fseek、ftell、fread 和分配失败都会关闭文件并保持 out_text 为 NULL。
 */
static cc_result_t posix_read_text(void *self, const char *path, char **out_text)
{
    (void)self;
    if (!path || !out_text) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid read_text arguments");
    }
    *out_text = NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file: %s", path);

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return cc_result_errf(CC_ERR_IO, "Cannot seek file: %s", path);
    }

    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return cc_result_errf(CC_ERR_IO, "Cannot tell file size: %s", path);
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return cc_result_errf(CC_ERR_IO, "Cannot rewind file: %s", path);
    }

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate buffer");
    }

    size_t read_size = fread(buf, 1, (size_t)size, f);
    if (read_size != (size_t)size && ferror(f)) {
        free(buf);
        fclose(f);
        return cc_result_errf(CC_ERR_IO, "Cannot read file: %s", path);
    }

    if (fclose(f) != 0) {
        free(buf);
        return cc_result_errf(CC_ERR_IO, "Cannot close file after reading: %s", path);
    }

    buf[read_size] = '\0';
    *out_text = buf;
    return cc_result_ok();
}

/* 覆盖写入原始字节；允许 data=NULL,size=0 创建空文件。 */
static cc_result_t posix_write_bytes(void *self,
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

    if (write_failed) {
        return cc_result_error(CC_ERR_IO, "Failed to write all data");
    }
    if (close_failed) {
        return cc_result_errf(CC_ERR_IO, "Cannot close file after writing: %s", path);
    }
    return cc_result_ok();
}

/*
 * 覆盖写入文本文件。
 *
 * 平台层只执行 I/O，不负责 workspace 安全检查；调用方应先通过 path policy。
 */
static cc_result_t posix_write_text(void *self, const char *path, const char *text)
{
    const char *safe_text = text ? text : "";
    return posix_write_bytes(self, path, safe_text, strlen(safe_text));
}

static cc_result_t posix_read_bytes(
    void *self, const char *path, void **out_data, size_t *out_size)
{
    (void)self;
    return read_file_bytes(path, out_data, out_size);
}

static cc_result_t posix_append_bytes(
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

static cc_result_t posix_stat_path(void *self, const char *path, cc_filesystem_stat_t *out_stat)
{
    (void)self;
    if (!path || !out_stat) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid stat request");
    memset(out_stat, 0, sizeof(*out_stat));
    out_stat->size = sizeof(*out_stat);
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) return cc_result_ok();
        return cc_result_errf(CC_ERR_IO, "Cannot stat path: %s", path);
    }
    out_stat->type = S_ISREG(st.st_mode) ? CC_FS_ENTRY_FILE :
        (S_ISDIR(st.st_mode) ? CC_FS_ENTRY_DIRECTORY : CC_FS_ENTRY_OTHER);
    out_stat->byte_size = (uint64_t)st.st_size;
    out_stat->modified_ms = (uint64_t)st.st_mtime * 1000ULL;
    return cc_result_ok();
}

static cc_result_t posix_atomic_replace(void *self, const char *temporary_path, const char *destination_path)
{
    (void)self;
    if (!temporary_path || !destination_path) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid atomic replace request");
    }
    if (rename(temporary_path, destination_path) != 0) {
        return cc_result_errf(CC_ERR_IO, "Cannot atomically replace destination: %s", destination_path);
    }
    return cc_result_ok();
}

static cc_result_t posix_sync_file(void *self, const char *path)
{
    (void)self;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return cc_result_errf(CC_ERR_IO, "Cannot open file for sync: %s", path);
    int failed = fsync(fd) != 0;
    close(fd);
    return failed ? cc_result_error(CC_ERR_IO, "File sync failed") : cc_result_ok();
}

static cc_result_t posix_sync_dir(void *self, const char *path)
{
    (void)self;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return cc_result_errf(CC_ERR_IO, "Cannot open directory for sync: %s", path);
    int failed = fsync(fd) != 0;
    close(fd);
    return failed ? cc_result_error(CC_ERR_IO, "Directory sync failed") : cc_result_ok();
}

static int relative_path_valid(const char *path)
{
    if (!path || !path[0] || path[0] == '/') return 0;
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

static int open_relative_no_follow(
    const char *root,
    const char *relative_path,
    int final_flags,
    mode_t mode)
{
    if (!root || !relative_path_valid(relative_path)) { errno = EINVAL; return -1; }
    int dirfd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0) return -1;
    char *copy = cc_copy_string(relative_path);
    if (!copy) { close(dirfd); errno = ENOMEM; return -1; }
    char *segment = copy;
    for (;;) {
        char *slash = strchr(segment, '/');
        if (!slash) break;
        *slash = '\0';
        int next = openat(dirfd, segment, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(dirfd);
        if (next < 0) { free(copy); return -1; }
        dirfd = next;
        segment = slash + 1;
    }
    int fd = openat(dirfd, segment, final_flags | O_NOFOLLOW | O_CLOEXEC, mode);
    close(dirfd);
    free(copy);
    return fd;
}

static cc_result_t posix_read_relative_text(
    void *self, const char *root, const char *relative_path, char **out_text)
{
    (void)self;
    if (!out_text) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null relative read output");
    *out_text = NULL;
    int fd = open_relative_no_follow(root, relative_path, O_RDONLY, 0);
    if (fd < 0) return cc_result_error(CC_ERR_PERMISSION_DENIED, "Unsafe or inaccessible workspace path");
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close(fd);
        return cc_result_error(CC_ERR_IO, "Workspace path is not a regular file");
    }
    size_t size = (size_t)st.st_size;
    char *text = malloc(size + 1);
    if (!text) { close(fd); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate file text"); }
    size_t offset = 0;
    while (offset < size) {
        ssize_t got = read(fd, text + offset, size - offset);
        if (got <= 0) { free(text); close(fd); return cc_result_error(CC_ERR_IO, "Failed to read workspace file"); }
        offset += (size_t)got;
    }
    close(fd);
    text[size] = '\0';
    *out_text = text;
    return cc_result_ok();
}

static cc_result_t posix_write_relative_text(
    void *self, const char *root, const char *relative_path, const char *text)
{
    (void)self;
    const char *data = text ? text : "";
    size_t size = strlen(data);
    int fd = open_relative_no_follow(root, relative_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return cc_result_error(CC_ERR_PERMISSION_DENIED, "Unsafe or inaccessible workspace path");
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = write(fd, data + offset, size - offset);
        if (written <= 0) { close(fd); return cc_result_error(CC_ERR_IO, "Failed to write workspace file"); }
        offset += (size_t)written;
    }
    int failed = fsync(fd) != 0;
    close(fd);
    return failed ? cc_result_error(CC_ERR_IO, "Workspace file sync failed") : cc_result_ok();
}

static cc_result_t posix_write_relative_bytes(
    void *self, const char *root, const char *relative_path, const void *data, size_t size)
{
    (void)self;
    if (size > 0 && !data) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null binary data");
    int fd = open_relative_no_follow(root, relative_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return cc_result_error(CC_ERR_PERMISSION_DENIED,
                                      "Unsafe or inaccessible workspace path");
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = write(fd, (const unsigned char *)data + offset, size - offset);
        if (written <= 0) {
            close(fd);
            return cc_result_error(CC_ERR_IO, "Failed to write workspace bytes");
        }
        offset += (size_t)written;
    }
    int failed = fsync(fd) != 0;
    close(fd);
    return failed ? cc_result_error(CC_ERR_IO, "Workspace file sync failed") : cc_result_ok();
}

/* 查询路径是否存在；无访问权限时按不存在处理，保持接口只返回 0/1。 */
static cc_result_t posix_exists(void *self, const char *path, int *out_exists)
{
    (void)self;
    if (!path || !out_exists) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid exists arguments");
    }
    *out_exists = (access(path, F_OK) == 0) ? 1 : 0;
    return cc_result_ok();
}

/*
 * 列举目录项名称。
 *
 * 返回的字符串数组由调用方逐项 free 后再 free 数组。任意单项复制或扩容失败都会释放已
 * 收集的条目，避免把半初始化列表交给上层。
 */
static cc_result_t posix_list_dir(void *self, const char *path, char ***out_items, size_t *out_count)
{
    (void)self;
    if (!path || !out_items || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid list_dir arguments");
    }
    *out_items = NULL;
    *out_count = 0;

    DIR *d = opendir(path);
    if (!d) return cc_result_errf(CC_ERR_IO, "Cannot open directory: %s", path);

    size_t count = 0;
    size_t cap = 16;
    char **items = calloc(cap, sizeof(*items));
    if (!items) {
        closedir(d);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate items");
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (count >= cap) {
            size_t next_cap = cap * 2;
            char **next = realloc(items, next_cap * sizeof(*items));
            if (!next) {
                closedir(d);
                free_items(items, count);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow items");
            }
            items = next;
            memset(items + cap, 0, (next_cap - cap) * sizeof(*items));
            cap = next_cap;
        }

        items[count] = cc_copy_string(entry->d_name);
        if (!items[count]) {
            closedir(d);
            free_items(items, count);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy directory item");
        }
        count++;
    }

    if (closedir(d) != 0) {
        free_items(items, count);
        return cc_result_errf(CC_ERR_IO, "Cannot close directory: %s", path);
    }

    *out_items = items;
    *out_count = count;
    return cc_result_ok();
}

/*
 * 递归创建目录。
 *
 * 逐级 mkdir，EEXIST 视为成功；如果路径段已存在但不是目录，后续 mkdir 或访问会返回
 * 平台错误。
 */
static cc_result_t posix_make_dir(void *self, const char *path)
{
    (void)self;
    if (!path || !path[0]) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid directory path");
    }

    char *tmp = cc_copy_string(path);
    if (!tmp) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate path");

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (tmp[0] && mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            cc_result_t rc = cc_result_errf(CC_ERR_IO, "Cannot create directory: %s", tmp);
            free(tmp);
            return rc;
        }
        *p = '/';
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        cc_result_t rc = cc_result_errf(CC_ERR_IO, "Cannot create directory: %s", path);
        free(tmp);
        return rc;
    }

    free(tmp);
    return cc_result_ok();
}

/* 删除文件或空目录；危险路径和审批由工具层或 policy 层控制。 */
static cc_result_t posix_remove(void *self, const char *path)
{
    (void)self;
    if (!path || !path[0]) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid remove path");
    }
    if (remove(path) != 0) {
        return cc_result_errf(CC_ERR_IO, "Cannot remove: %s", path);
    }
    return cc_result_ok();
}

/* 销毁 POSIX filesystem 私有对象。 */
static void posix_destroy(void *self)
{
    free(self);
}

static cc_filesystem_vtable_t posix_vtable = {
    .size = sizeof(cc_filesystem_vtable_t),
    .version = CC_FILESYSTEM_VTABLE_VERSION,
    .capabilities = CC_FS_CAP_BINARY | CC_FS_CAP_APPEND | CC_FS_CAP_STAT |
        CC_FS_CAP_ATOMIC_REPLACE | CC_FS_CAP_FILE_SYNC | CC_FS_CAP_DIR_SYNC |
        CC_FS_CAP_ROOT_SCOPED | CC_FS_CAP_NO_SYMLINKS,
    .read_text = posix_read_text,
    .write_text = posix_write_text,
    .write_bytes = posix_write_bytes,
    .exists = posix_exists,
    .list_dir = posix_list_dir,
    .make_dir = posix_make_dir,
    .remove = posix_remove,
    .destroy = posix_destroy,
    .read_bytes = posix_read_bytes,
    .append_bytes = posix_append_bytes,
    .stat = posix_stat_path,
    .atomic_replace = posix_atomic_replace,
    .sync_file = posix_sync_file,
    .sync_dir = posix_sync_dir,
    .read_relative_text = posix_read_relative_text,
    .write_relative_text = posix_write_relative_text,
    .write_relative_bytes = posix_write_relative_bytes,
};

/*
 * 创建 POSIX filesystem 端口。
 *
 * 成功后 out_fs 获得 self/vtable，调用方通过 vtable->destroy 释放。
 */
cc_result_t cc_filesystem_get_posix(cc_filesystem_t *out_fs)
{
    if (!out_fs) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null filesystem output");
    }
    memset(out_fs, 0, sizeof(*out_fs));

    cc_posix_filesystem_t *self = calloc(1, sizeof(*self));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create posix filesystem");

    out_fs->self = self;
    out_fs->vtable = &posix_vtable;
    out_fs->size = sizeof(*out_fs);
    out_fs->version = posix_vtable.version;
    out_fs->capabilities = posix_vtable.capabilities;
    return cc_result_ok();
}

/* POSIX profile 的默认 filesystem 就是 POSIX filesystem。 */
cc_result_t cc_filesystem_get_default(cc_filesystem_t *out_fs)
{
    return cc_filesystem_get_posix(out_fs);
}
