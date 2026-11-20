#include "cc/ports/cc_filesystem.h"
#include "cc/internal/cc_alloc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/** @brief 无可变配置的端口私有对象，生命周期由创建者管理。 */
typedef struct {
    int dummy; /**< 保证对象具有非零大小的占位字段。 */
} cc_posix_filesystem_t;

/**
 * @brief 从独立打开的文件末尾读取不超过容量的原始字节。
 * @param[in] self 端口对象，当前不读取，可为 NULL。
 * @param[in] path 同步借用的路径，必须非 NULL。
 * @param[out] buffer 可写 capacity 字节，容量为零时可为 NULL，失败可能已写入部分字节。
 * @param[in] capacity 缓冲容量，单位字节，不得超过 LONG_MAX。
 * @param[out] out_size 非 NULL 输出槽，入口置零，仅完整读取并成功关闭后交付字节数。
 * @return 成功返回 CC_OK，参数错误返回 INVALID_ARGUMENT，打开、定位、短读或关闭失败返回 IO。
 */
static cc_result_t posix_read_tail(void *self, const char *path, void *buffer,
                                   size_t capacity, size_t *out_size)
{
    (void)self;
    if (out_size) *out_size = 0;
    if (!path || !out_size || (!buffer && capacity) || capacity > LONG_MAX)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid tail read");
    *out_size = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return cc_result_error(CC_ERR_IO, "Cannot open file for tail read");
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return cc_result_error(CC_ERR_IO, "Cannot seek file"); }
    long end = ftell(f);
    if (end < 0) { fclose(f); return cc_result_error(CC_ERR_IO, "Cannot size file"); }
    size_t wanted = (size_t)end < capacity ? (size_t)end : capacity;
    if (fseek(f, -(long)wanted, SEEK_END) != 0) { fclose(f); return cc_result_error(CC_ERR_IO, "Cannot seek file tail"); }
    size_t got = wanted ? fread(buffer, 1, wanted, f) : 0;
    int failed = got != wanted || ferror(f);
    if (fclose(f) != 0) failed = 1;
    if (failed) return cc_result_error(CC_ERR_IO, "Cannot read file tail");
    *out_size = got;
    return cc_result_ok();
}

/**
 * @brief 按打开时取得的文件长度完整读取二进制数据。
 * @param[in] path 借用的非 NULL 路径。
 * @param[out] out_data 非 NULL 的空槽，入口置 NULL，成功分配由调用方 free，空文件也交付可释放存储。
 * @param[out] out_size 非 NULL 字节数槽，入口置零，成功后为实际文件长度。
 * @return 成功返回 CC_OK，参数、大小不可表示、分配及文件操作错误分别返回对应错误。
 * @note 文件并发增长时读取初始长度，并发缩短导致短读失败，失败回收本次缓冲。
 */
static cc_result_t read_file_bytes(const char *path, void **out_data, size_t *out_size)
{
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0;
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
    if ((uintmax_t)end > SIZE_MAX) {
        fclose(f);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "File size cannot be represented");
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

/**
 * @brief 回收目录枚举已拥有的名称和数组。
 * @param[in] items 独占的数组，可为 NULL。
 * @param[in] count 已初始化且可 free 的名称数量。
 * @post 数组及前 count 项释放，调用方保存的指针失效。
 */
static void free_items(char **items, size_t count)
{
    if (!items) return;
    for (size_t i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

/**
 * @brief 按初始文件长度读取原始内容并补末尾 NUL，不作字符编码转换。
 * @param[in] self 当前不读取的端口对象，可为 NULL。
 * @param[in] path 借用的非 NULL 路径。
 * @param[out] out_text 非 NULL 的空输出槽，入口置 NULL，成功后由调用方 free。
 * @return 成功返回 CC_OK，参数、大小溢出、分配或完整读取及关闭失败返回对应错误。
 * @note 文件内嵌 NUL 原样保留，接口没有独立正文长度输出；需要二进制长度时使用 read_bytes。
 */
static cc_result_t posix_read_text(void *self, const char *path, char **out_text)
{
    (void)self;
    if (out_text) *out_text = NULL;
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

    if ((uintmax_t)size > SIZE_MAX - 1) {
        fclose(f);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Text file size cannot be represented");
    }

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate buffer");
    }

    size_t read_size = fread(buf, 1, (size_t)size, f);
    if (read_size != (size_t)size || ferror(f)) {
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

/**
 * @brief 以二进制覆盖方式创建或截断文件并检查完整写入及关闭。
 * @param[in] self 当前不读取的端口对象，可为 NULL。
 * @param[in] path 借用的非 NULL 路径，父目录须已存在。
 * @param[in] data 借用 size 字节，size 为零时可为 NULL。
 * @param[in] size 写入字节数。
 * @return 完整写入并关闭成功返回 CC_OK，参数错误返回 INVALID_ARGUMENT，I/O 失败返回 IO。
 * @note 失败可能已截断或写入部分文件，不回滚，不隐含 fsync。
 */
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

/**
 * @brief 将文本字符串委托给二进制覆盖写接口。
 * @param[in] self 借用端口对象，当前实现忽略。
 * @param[in] path 非 NULL 的借用路径。
 * @param[in] text 借用的 NUL 结尾正文，NULL 等价于空正文。
 * @return 原样返回 write_bytes 结果，失败可留下部分文件。
 */
static cc_result_t posix_write_text(void *self, const char *path, const char *text)
{
    const char *safe_text = text ? text : "";
    return posix_write_bytes(self, path, safe_text, strlen(safe_text));
}

/**
 * @brief 通过完整二进制读取 helper 交付拥有的文件缓冲。
 * @param[in] self 当前不读取，可为 NULL。
 * @param[in] path 借用的非 NULL 路径。
 * @param[out] out_data 非 NULL 空槽，入口置 NULL，成功后调用方 free。
 * @param[out] out_size 非 NULL 槽，入口置零，成功写文件字节数。
 * @return 原样返回 read_file_bytes 结果，失败输出为空。
 */
static cc_result_t posix_read_bytes(
    void *self, const char *path, void **out_data, size_t *out_size)
{
    (void)self;
    return read_file_bytes(path, out_data, out_size);
}

/**
 * @brief 打开或创建文件并追加原始字节，检查短写和关闭。
 * @param[in] self 当前忽略的端口对象，可为 NULL。
 * @param[in] path 借用的非 NULL 路径。
 * @param[in] data 调用期借用 size 字节，零长度时可为 NULL。
 * @param[in] size 追加字节数，零长度仍打开或创建文件。
 * @return 成功返回 CC_OK，参数错误返回 INVALID_ARGUMENT，写入或关闭失败返回 IO。
 * @note 不保证多个调用的整个正文作为一条记录原子追加，失败可能已写前缀。
 */
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

/**
 * @brief 使用 lstat 查询路径自身的类型、大小和修改时间。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] path 非 NULL 的借用路径，最终符号链接归为 OTHER。
 * @param[out] out_stat 非 NULL 输出，入口清零并设置 size，ENOENT 时 type 保持 MISSING。
 * @return 查询成功或路径不存在返回 CC_OK，参数错误返回 INVALID_ARGUMENT，其他查询失败返回 IO。
 * @note 修改时间使用 Unix 毫秒，早于 epoch 为 0，超出毫秒范围为 UINT64_MAX，源精度为秒。
 */
static cc_result_t posix_stat_path(void *self, const char *path, cc_filesystem_stat_t *out_stat)
{
    (void)self;
    if (out_stat) {
        memset(out_stat, 0, sizeof(*out_stat));
        out_stat->size = sizeof(*out_stat);
    }
    if (!path || !out_stat) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid stat request");
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) return cc_result_ok();
        return cc_result_errf(CC_ERR_IO, "Cannot stat path: %s", path);
    }
    out_stat->type = S_ISREG(st.st_mode) ? CC_FS_ENTRY_FILE :
        (S_ISDIR(st.st_mode) ? CC_FS_ENTRY_DIRECTORY : CC_FS_ENTRY_OTHER);
    out_stat->byte_size = (uint64_t)st.st_size;
    if (st.st_mtime > 0) {
        uintmax_t seconds = (uintmax_t)st.st_mtime;
        out_stat->modified_ms = seconds > UINT64_MAX / 1000U
            ? UINT64_MAX : (uint64_t)seconds * 1000U;
    }
    return cc_result_ok();
}

/**
 * @brief 通过同一文件系统上的 rename 安装临时文件并替换目标。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] temporary_path 借用的非 NULL 源路径，成功后原路径被移走。
 * @param[in] destination_path 借用的非 NULL 目标路径，按原生 rename 规则替换。
 * @return 成功返回 CC_OK，参数错误返回 INVALID_ARGUMENT，跨卷或其他重命名失败返回 IO。
 * @note 原子可见性依赖文件系统，不隐含文件或父目录同步，失败由调用者依据文件状态恢复。
 */
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

/**
 * @brief 重新打开文件并执行 fsync，返回前关闭本次描述符。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] path 必需的借用文件路径。
 * @return 同步和关闭成功返回 CC_OK，空路径返回 INVALID_ARGUMENT，其余失败返回 IO。
 */
static cc_result_t posix_sync_file(void *self, const char *path)
{
    (void)self;
    if (!path) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null sync path");
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return cc_result_errf(CC_ERR_IO, "Cannot open file for sync: %s", path);
    int sync_result;
    do { sync_result = fsync(fd); } while (sync_result != 0 && errno == EINTR);
    int failed = sync_result != 0;
    if (close(fd) != 0) failed = 1;
    return failed ? cc_result_error(CC_ERR_IO, "File sync failed") : cc_result_ok();
}

/**
 * @brief 打开目录并同步其元数据，返回前关闭本次描述符。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] path 必需的借用目录路径，最终对象必须为目录。
 * @return 同步和关闭成功返回 CC_OK，空路径返回 INVALID_ARGUMENT，其他失败返回 IO。
 * @note 是否提供持久化保证还取决于文件系统对目录 fsync 的支持。
 */
static cc_result_t posix_sync_dir(void *self, const char *path)
{
    (void)self;
    if (!path) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null sync path");
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return cc_result_errf(CC_ERR_IO, "Cannot open directory for sync: %s", path);
    int sync_result;
    do { sync_result = fsync(fd); } while (sync_result != 0 && errno == EINTR);
    int failed = sync_result != 0;
    if (close(fd) != 0) failed = 1;
    return failed ? cc_result_error(CC_ERR_IO, "Directory sync failed") : cc_result_ok();
}

/**
 * @brief 校验根内操作使用的非空相对路径，拒绝绝对路径、空段、点和父目录段。
 * @param[in] path 借用的 NUL 结尾路径，NULL 返回失败。
 * @return 合法返回 1，否则返回 0，POSIX 下反斜杠按普通名称字符处理。
 */
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

/**
 * @brief 逐级借助目录描述符打开相对路径，各级最终组件均要求不跟随符号链接。
 * @param[in] root 借用根目录路径，其最终组件必须为真实目录，祖先路径仍由系统解析。
 * @param[in] relative_path 借用的非空相对路径，必须通过 relative_path_valid。
 * @param[in] final_flags 最终 openat 标记，函数另加 NOFOLLOW 和 CLOEXEC。
 * @param[in] mode 新建文件权限，受 umask 影响，仅创建时使用。
 * @return 成功交付由调用方 close 的描述符，失败返回 -1 并设置 errno，所有中间资源释放。
 * @note 根描述符取得后逐段固定目录对象，不能只用事先的字符串前缀判断代替该访问过程。
 */
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
        int open_error = errno;
        close(dirfd);
        if (next < 0) { free(copy); errno = open_error; return -1; }
        dirfd = next;
        segment = slash + 1;
    }
    int fd = openat(dirfd, segment, final_flags | O_NOFOLLOW | O_CLOEXEC, mode);
    int open_error = errno;
    close(dirfd);
    free(copy);
    if (fd < 0) errno = open_error;
    return fd;
}

/**
 * @brief 以根目录描述符约束打开普通文件，完整读取并补 NUL。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] root 借用的根目录路径，必须非 NULL。
 * @param[in] relative_path 非空借用相对路径，禁止点段、空段和逐级符号链接。
 * @param[out] out_text 必需空输出槽，入口置 NULL，成功由调用方 free。
 * @return 成功返回 CC_OK，空输出返回 INVALID_ARGUMENT，根内打开失败返回 PERMISSION_DENIED，分配、容量及 I/O 失败返回对应错误。
 * @note 按 fstat 长度读取，文件缩短造成失败，增长只读取初始长度，EINTR 原描述符重试。
 */
static cc_result_t posix_read_relative_text(
    void *self, const char *root, const char *relative_path, char **out_text)
{
    (void)self;
    if (!out_text) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null relative read output");
    *out_text = NULL;
    int fd = open_relative_no_follow(root, relative_path, O_RDONLY, 0);
    if (fd < 0) return cc_result_error(errno == ENOMEM ? CC_ERR_OUT_OF_MEMORY : CC_ERR_PERMISSION_DENIED,
        "Unsafe or inaccessible workspace path");
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close(fd);
        return cc_result_error(CC_ERR_IO, "Workspace path is not a regular file");
    }
    if ((uintmax_t)st.st_size > SIZE_MAX - 1) {
        close(fd);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Workspace file size cannot be represented");
    }
    size_t size = (size_t)st.st_size;
    char *text = malloc(size + 1);
    if (!text) { close(fd); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate file text"); }
    size_t offset = 0;
    while (offset < size) {
        size_t chunk = size - offset > SSIZE_MAX ? (size_t)SSIZE_MAX : size - offset;
        ssize_t got = read(fd, text + offset, chunk);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { free(text); close(fd); return cc_result_error(CC_ERR_IO, "Failed to read workspace file"); }
        offset += (size_t)got;
    }
    if (close(fd) != 0) {
        free(text);
        return cc_result_error(CC_ERR_IO, "Failed to close workspace file");
    }
    text[size] = '\0';
    *out_text = text;
    return cc_result_ok();
}

/**
 * @brief 在根约束下创建或截断文件，完整写入文本并 fsync。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] root 非 NULL 的借用根目录。
 * @param[in] relative_path 借用的合法相对路径，父目录须存在，逐级拒绝符号链接。
 * @param[in] text NUL 结尾的借用文本，NULL 按空正文处理。
 * @return 完整写入、同步及关闭成功返回 CC_OK，打开失败返回 PERMISSION_DENIED 或 OOM，写入失败返回 IO。
 * @note 失败可留下截断或部分内容，不作原子替换，EINTR 在原描述符上重试。
 */
static cc_result_t posix_write_relative_text(
    void *self, const char *root, const char *relative_path, const char *text)
{
    (void)self;
    const char *data = text ? text : "";
    size_t size = strlen(data);
    int fd = open_relative_no_follow(root, relative_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return cc_result_error(errno == ENOMEM ? CC_ERR_OUT_OF_MEMORY : CC_ERR_PERMISSION_DENIED,
        "Unsafe or inaccessible workspace path");
    size_t offset = 0;
    while (offset < size) {
        size_t chunk = size - offset > SSIZE_MAX ? (size_t)SSIZE_MAX : size - offset;
        ssize_t written = write(fd, data + offset, chunk);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { close(fd); return cc_result_error(CC_ERR_IO, "Failed to write workspace file"); }
        offset += (size_t)written;
    }
    int sync_result;
    do { sync_result = fsync(fd); } while (sync_result != 0 && errno == EINTR);
    int failed = sync_result != 0;
    if (close(fd) != 0) failed = 1;
    return failed ? cc_result_error(CC_ERR_IO, "Workspace file sync failed") : cc_result_ok();
}

/**
 * @brief 在根约束下覆盖写入二进制数据，完整写入后同步并关闭。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] root 借用的非 NULL 根目录。
 * @param[in] relative_path 借用的合法相对路径，父目录须存在，各级拒绝符号链接。
 * @param[in] data 借用 size 字节，零长度时可为 NULL。
 * @param[in] size 正文字节数，零长度仍创建或截断文件。
 * @return 成功返回 CC_OK，非零长度配 NULL 返回 INVALID_ARGUMENT，打开、分配及 I/O 失败返回对应错误。
 * @note 失败可能已修改文件，调用者若需要事务安装须另用临时文件和 atomic_replace。
 */
static cc_result_t posix_write_relative_bytes(
    void *self, const char *root, const char *relative_path, const void *data, size_t size)
{
    (void)self;
    if (size > 0 && !data) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null binary data");
    int fd = open_relative_no_follow(root, relative_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return cc_result_error(errno == ENOMEM ? CC_ERR_OUT_OF_MEMORY : CC_ERR_PERMISSION_DENIED,
                                      "Unsafe or inaccessible workspace path");
    size_t offset = 0;
    while (offset < size) {
        size_t chunk = size - offset > SSIZE_MAX ? (size_t)SSIZE_MAX : size - offset;
        ssize_t written = write(fd, (const unsigned char *)data + offset, chunk);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            close(fd);
            return cc_result_error(CC_ERR_IO, "Failed to write workspace bytes");
        }
        offset += (size_t)written;
    }
    int sync_result;
    do { sync_result = fsync(fd); } while (sync_result != 0 && errno == EINTR);
    int failed = sync_result != 0;
    if (close(fd) != 0) failed = 1;
    return failed ? cc_result_error(CC_ERR_IO, "Workspace file sync failed") : cc_result_ok();
}

/**
 * @brief 使用 access 查询路径存在性，符号链接按系统规则跟随。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] path 非 NULL 的借用路径。
 * @param[out] out_exists 必需输出槽，入口置零，access 成功写 1。
 * @return 有效参数始终返回 CC_OK，空参数返回 INVALID_ARGUMENT，访问错误按不存在表示。
 */
static cc_result_t posix_exists(void *self, const char *path, int *out_exists)
{
    (void)self;
    if (out_exists) *out_exists = 0;
    if (!path || !out_exists) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid exists arguments");
    }
    *out_exists = (access(path, F_OK) == 0) ? 1 : 0;
    return cc_result_ok();
}

/**
 * @brief 枚举目录下除点和父目录项外的名称，不排序或构造完整路径。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] path 借用的非 NULL 目录路径。
 * @param[out] out_items 必需空槽，入口置 NULL，成功数组及每个名称均由调用方 free。
 * @param[out] out_count 必需计数槽，入口置零，成功时为名称数。
 * @return 成功返回 CC_OK，参数错误、容量溢出、OOM、枚举或关闭失败返回对应错误。
 * @note 失败回收全部部分列表，目录并发变化时结果是遍历期间的观察集合。
 */
static cc_result_t posix_list_dir(void *self, const char *path, char ***out_items, size_t *out_count)
{
    (void)self;
    if (out_items) *out_items = NULL;
    if (out_count) *out_count = 0;
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
    int read_error = 0;
    for (;;) {
        errno = 0;
        entry = readdir(d);
        if (!entry) { read_error = errno; break; }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (count >= cap) {
            if (cap > SIZE_MAX / 2 / sizeof(*items)) {
                closedir(d);
                free_items(items, count);
                return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Too many directory entries");
            }
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

    if (read_error || closedir(d) != 0) {
        if (read_error) (void)closedir(d);
        free_items(items, count);
        return cc_result_errf(CC_ERR_IO, "Cannot read or close directory: %s", path);
    }

    *out_items = items;
    *out_count = count;
    return cc_result_ok();
}

/**
 * @brief 按斜杠逐级创建目录，已存在对象必须实际为目录。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] path 借用的非空路径，新目录权限为 0755 并受 umask 影响。
 * @return 成功返回 CC_OK，空路径返回 INVALID_ARGUMENT，分配失败返回 OOM，创建或类型冲突返回 IO。
 * @note 失败保留此前已创建的目录，已存在符号链接按 stat 跟随后的目录类型判断。
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
        struct stat st;
        if (tmp[0] && mkdir(tmp, 0755) != 0 &&
            (errno != EEXIST || stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode))) {
            cc_result_t rc = cc_result_errf(CC_ERR_IO, "Cannot create directory: %s", tmp);
            free(tmp);
            return rc;
        }
        *p = '/';
    }

    struct stat st;
    if (mkdir(tmp, 0755) != 0 &&
        (errno != EEXIST || stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode))) {
        cc_result_t rc = cc_result_errf(CC_ERR_IO, "Cannot create directory: %s", path);
        free(tmp);
        return rc;
    }

    free(tmp);
    return cc_result_ok();
}

/**
 * @brief 按原生 remove 规则删除文件、符号链接或空目录。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] path 借用的非空路径，调用方负责选择允许删除的目标。
 * @return 成功返回 CC_OK，空路径返回 INVALID_ARGUMENT，删除失败返回 IO。
 */
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

/**
 * @brief 释放创建函数分配的私有对象。
 * @param[in] self 独占对象，可为 NULL。
 * @pre 拥有者已排空全部端口调用，所有浅拷贝停止借用。
 * @post self 失效，外层 cc_filesystem_t 槽由调用方清空。
 */
static void posix_destroy(void *self)
{
    free(self);
}

static cc_filesystem_vtable_t posix_vtable = {
    .size = sizeof(cc_filesystem_vtable_t),
    .version = CC_FILESYSTEM_VTABLE_VERSION,
    .capabilities = CC_FS_CAP_BINARY | CC_FS_CAP_APPEND | CC_FS_CAP_STAT |
        CC_FS_CAP_ATOMIC_REPLACE | CC_FS_CAP_FILE_SYNC | CC_FS_CAP_DIR_SYNC |
        CC_FS_CAP_ROOT_SCOPED | CC_FS_CAP_NO_SYMLINKS | CC_FS_CAP_READ_TAIL,
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
    .read_tail = posix_read_tail,
};

/**
 * @brief 创建 POSIX 文件系统端口并发布完整 v4 能力表。
 * @param[out] out_fs 必需的空接口槽，入口清零，成功后拥有 self，vtable 为进程期静态借用。
 * @return 成功返回 CC_OK，空输出返回 INVALID_ARGUMENT，分配失败返回 OOM 并保持空输出。
 * @note 对象无可变配置，各调用拥有独立 I/O 资源，同一文件的竞争与事务顺序由调用者协调。
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

/**
 * @brief 为 POSIX profile 创建默认文件系统端口。
 * @param[out] out_fs 必需空槽，成功拥有 self，失败清零。
 * @return 原样返回 cc_filesystem_get_posix 的拥有型结果。
 */
cc_result_t cc_filesystem_get_default(cc_filesystem_t *out_fs)
{
    return cc_filesystem_get_posix(out_fs);
}
