



#include "cc/ports/cc_filesystem.h"

#ifdef ESP_PLATFORM
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "cc/internal/cc_alloc.h"

#define CC_ESP32_MAX_TEXT_FILE_BYTES (1024U * 1024U)

/*
 * ESP32 filesystem 私有状态。
 *
 * 当前通过 ESP-IDF VFS 的 stdio/dirent 接口实现，因此不需要额外状态；保留 self 是为了
 * 满足统一 filesystem vtable。
 */
typedef struct cc_esp32_filesystem {
    int dummy;
} cc_esp32_filesystem_t;

/**
 * @brief 判断单字节是否为 ASCII 大小写字母。
 * @param[in] ch 按值检查的字符。
 * @return 字母返回一，其他字符返回零，无共享可变状态。
 */
static int esp32_ascii_letter(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

/**
 * @brief 识别 ESP32 VFS 入口拒绝的盘符前缀或反斜杠。
 * @param[in] path 调用期借用的 NUL 结尾路径，可为 NULL。
 * @return 命中宿主路径形式为一，其余为零，不保留输入。
 */
static int esp32_host_path(const char *path)
{
    if (!path || !path[0]) {
        return 0;
    }
    return (esp32_ascii_letter(path[0]) && path[1] == ':') || strchr(path, '\\') != NULL;
}

/**
 * @brief 检查普通 VFS 操作所需的非空路径形式。
 * @param[in] path 调用期借用的 NUL 结尾路径。
 * @return 有效为 CC_OK，空值或宿主形式为 INVALID_ARGUMENT，拥有诊断由调用者释放。
 * @note 普通入口沿 VFS 路径解析，根相对入口另检查组件约束。
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

/**
 * @brief 将文件尾部至多 capacity 字节读入调用者缓冲。
 * @param[in,out] self 借用存活后端，当前实现忽略。
 * @param[in] path 调用期借用的文件路径。
 * @param[out] buffer 借用的 capacity 字节可写区，容量为零时可为 NULL。
 * @param[in] capacity 尾部最大字节数，须能由 long 表示。
 * @param[out] out_size 必填槽，入口置零，仅完整读取并关闭成功后写实际数量。
 * @return 参数、路径或 I/O 失败为拥有错误结果，成功为 CC_OK，诊断由调用者释放。
 * @note 失败可能已修改缓冲前缀，按文件尾原始字节读取，调用者协调文件并发修改。
 */
static cc_result_t esp32_read_tail(void *self, const char *path, void *buffer,
                                   size_t capacity, size_t *out_size)
{
    (void)self;
    if (out_size) *out_size = 0;
    if (!out_size || (!buffer && capacity) || capacity > LONG_MAX)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid tail read");
    cc_result_t rc = esp32_validate_path(path);
    if (rc.code != CC_OK) return rc;
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
 * @brief 完整读取至多一 MiB 的无内嵌 NUL 文本。
 * @param[in,out] self 借用存活后端，当前实现忽略。
 * @param[in] path 调用期借用的 VFS 文件路径。
 * @param[out] out_text 必填空拥有槽，入口置 NULL，成功取得附终止 NUL 的分配并最终 free。
 * @return 成功为 CC_OK，参数、容量、分配、定位、短读或关闭失败返回拥有错误，调用者释放诊断。
 * @note 临时 FILE 和失败缓冲在返回前归还，文件变化及实例寿命由外层协调。
 */
static cc_result_t esp32_read_text(void *self, const char *path, char **out_text)
{
    (void)self;
    if (!out_text) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null text output");
    *out_text = NULL;
    cc_result_t rc = esp32_validate_path(path);
    if (rc.code != CC_OK) return rc;
    FILE *f = fopen(path, "rb");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file: %s", path);
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return cc_result_error(CC_ERR_IO, "Failed to seek file end");
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return cc_result_error(CC_ERR_IO, "Failed to determine file size");
    }
    if ((uint64_t)size > CC_ESP32_MAX_TEXT_FILE_BYTES) {
        fclose(f);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Text file exceeds 1 MiB read limit");
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return cc_result_error(CC_ERR_IO, "Failed to seek file start");
    }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate file buffer");
    }
    size_t n = fread(buf, 1, (size_t)size, f);
    int failed = n != (size_t)size || ferror(f);
    if (fclose(f) != 0) failed = 1;
    if (failed) {
        free(buf);
        return cc_result_error(CC_ERR_IO, "Failed to read complete text file");
    }
    if (memchr(buf, '\0', n) != NULL) {
        free(buf);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Text file contains an embedded NUL byte");
    }
    buf[n] = '\0';
    *out_text = buf;
    return cc_result_ok();
}

/**
 * @brief 通过普通 VFS 路径创建或截断并完整写入字节。
 * @param[in,out] self 借用存活后端，当前实现忽略。
 * @param[in] path 调用期借用的文件路径。
 * @param[in] data 调用期借用的 size 字节，零长度允许 NULL。
 * @param[in] size 写入字节数，普通二进制入口沿调用方预算。
 * @return 写入与关闭均成功为 CC_OK，参数、路径或 I/O 失败返回拥有错误结果。
 * @note 失败可能已截断或写入部分正文，临时 FILE 总会关闭，调用者释放诊断并协调文件竞争。
 */
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

/**
 * @brief 按 C 字符串长度委托普通二进制覆盖写入。
 * @param[in,out] self 调用期间借用的存活后端。
 * @param[in] path 调用期借用的文件路径。
 * @param[in] text 借用的 NUL 结尾正文，NULL 按空文本处理。
 * @return 直接转交 write_bytes 的拥有结果，调用者释放诊断。
 * @note 本入口按普通路径写入，根相对文本入口另实施一 MiB 上限与临时安装流程。
 */
static cc_result_t esp32_write_text(void *self, const char *path, const char *text)
{
    const char *safe_text = text ? text : "";
    return esp32_write_bytes(self, path, safe_text, strlen(safe_text));
}

/**
 * @brief 完整读取文件字节并交付独占缓冲。
 * @param[in,out] self 借用存活后端，当前实现忽略。
 * @param[in] path 调用期借用的 VFS 路径。
 * @param[out] out_data 必填空拥有槽，有效参数组合后置 NULL，成功缓冲由调用者 free。
 * @param[out] out_size 必填数量槽，有效参数组合后置零，成功写实际字节数。
 * @return 成功为 CC_OK，参数、分配、定位、短读或关闭失败返回拥有错误结果。
 * @note 空文件仍交付可释放分配，缓冲无额外终止符，大小预算和文件并发由上层协调。
 */
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

/**
 * @brief 以追加模式创建或打开文件并写入全部借用字节。
 * @param[in,out] self 借用存活后端，当前实现忽略。
 * @param[in] path 调用期借用的文件路径。
 * @param[in] data 借用的 size 字节，零长度可为 NULL。
 * @param[in] size 请求追加字节数。
 * @return 写入与关闭均成功为 CC_OK，参数、路径或 I/O 失败返回拥有错误结果。
 * @note 失败可能已追加前缀，整条记录的并发顺序由拥有者协调，诊断由调用者释放。
 */
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

/**
 * @brief 读取路径类型、大小及 Unix 毫秒修改时间。
 * @param[in,out] self 借用文件端口状态，当前实现忽略。
 * @param[in] path 调用期间借用的非 NULL、NUL 结尾路径。
 * @param[out] out_stat 必填独占输出，先清零并填写结构大小，不存在路径保持空类型。
 * @return 成功或 ENOENT 为 CC_OK，空参数为 INVALID_ARGUMENT，其他 stat 失败为 IO，拥有诊断由调用者释放。
 * @note 修改时间按底层秒精度换算，早于 Unix 起点为零，超出毫秒范围时饱和。
 * 路径及外部文件修改的同步由使用方协调，文件大小沿底层 stat 元数据。
 */
static cc_result_t esp32_stat_path(void *self, const char *path, cc_filesystem_stat_t *out_stat)
{
    (void)self;
    if (!out_stat) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null stat output");
    memset(out_stat, 0, sizeof(*out_stat));
    out_stat->size = sizeof(*out_stat);
    cc_result_t rc = esp32_validate_path(path);
    if (rc.code != CC_OK) return rc;
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT) return cc_result_ok();
        return cc_result_errf(CC_ERR_IO, "Cannot stat path: %s", path);
    }
    out_stat->type = S_ISDIR(st.st_mode) ? CC_FS_ENTRY_DIRECTORY : CC_FS_ENTRY_FILE;
    out_stat->byte_size = (uint64_t)st.st_size;
    if (st.st_mtime > 0) {
        uint64_t seconds = (uint64_t)st.st_mtime;
        out_stat->modified_ms = seconds > UINT64_MAX / 1000U
            ? UINT64_MAX : seconds * 1000U;
    }
    return cc_result_ok();
}

/**
 * @brief 安装临时文件，原生覆盖不可用时采用目标与备份的可恢复重命名流程。
 * @param[in,out] self 借用存活后端，当前实现忽略。
 * @param[in] temporary_path 调用期借用的源路径，须与目标及其 .bak 文件独立。
 * @param[in] destination_path 调用期借用的目标路径，.bak 后缀由本流程保管。
 * @return 安装完成为 CC_OK，参数、容量、分配或 I/O 失败为拥有错误结果，调用者释放诊断。
 * @note 拥有者串行协调目标及备用路径。失败可能已恢复或移动文件，核对所有候选后继续。
 * FAT 的两步安装需要上层恢复协议，成功后的旧备份清理失败会保留备份并记录日志。
 */
static cc_result_t esp32_atomic_replace(void *self, const char *temporary_path, const char *destination_path)
{
    (void)self;
    cc_result_t rc = esp32_validate_path(temporary_path);
    if (rc.code != CC_OK) return rc;
    rc = esp32_validate_path(destination_path);
    if (rc.code != CC_OK) return rc;
    size_t length = strlen(destination_path);
    if (length > SIZE_MAX - 5) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Path too long");
    char *backup = malloc(length + 5);
    if (!backup) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate backup path");
    memcpy(backup, destination_path, length);
    memcpy(backup + length, ".bak", 5);
    if (strcmp(temporary_path, backup) == 0) {
        free(backup);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Temporary path conflicts with replacement backup");
    }

    struct stat destination_stat;
    bool destination_exists = stat(destination_path, &destination_stat) == 0;
    if (!destination_exists && errno != ENOENT) {
        int saved = errno;
        free(backup);
        return cc_result_errf(CC_ERR_IO, "Destination cannot be inspected errno=%d", saved);
    }

    struct stat backup_stat;
    bool backup_exists = stat(backup, &backup_stat) == 0;
    if (!backup_exists && errno != ENOENT) {
        int saved = errno;
        free(backup);
        return cc_result_errf(CC_ERR_IO, "Backup cannot be inspected errno=%d", saved);
    }

    /* 恢复孤立备份后才启动本次安装。两者均存在时沿本流程将目标视为已安装版本，
     * 备份视为此前安装成功但清理失败留下的候选，上层须先完成候选完整性核对。 */
    if (backup_exists) {
        if (!destination_exists) {
            if (rename(backup, destination_path) != 0) {
                int saved = errno;
                free(backup);
                return cc_result_errf(CC_ERR_IO, "Backup restore failed errno=%d", saved);
            }
            destination_exists = true;
        } else if (remove(backup) != 0) {
            int saved = errno;
            free(backup);
            return cc_result_errf(CC_ERR_IO, "Stale backup cleanup failed errno=%d", saved);
        }
    }

    if (rename(temporary_path, destination_path) == 0) {
        free(backup);
        return cc_result_ok();
    }
    int rename_error = errno;
    /* FAT VFS 拒绝覆盖已存在目标时，先保留旧目标，再安装源文件。
     * 拥有者协调每个路径的写入顺序，并在重启后执行候选恢复。 */
    if (rename_error != EEXIST && rename_error != ENOTEMPTY) {
        free(backup);
        return cc_result_errf(CC_ERR_IO, "Replace rename failed errno=%d", rename_error);
    }
    if (rename(destination_path, backup) != 0) {
        int saved = errno;
        free(backup);
        return cc_result_errf(CC_ERR_IO, "Backup rename failed errno=%d", saved);
    }
    if (rename(temporary_path, destination_path) != 0) {
        int saved = errno;
        int restored = rename(backup, destination_path);
        int restore_error = restored == 0 ? 0 : errno;
        free(backup);
        return cc_result_errf(CC_ERR_IO, "Install rename failed errno=%d restore_errno=%d; recovery files retained",
                             saved, restore_error);
    }
    /* 安装后清理失败时保留备份，仍返回已完成安装的状态。 */
    if (remove(backup) != 0) {
        int saved = errno;
        ESP_LOGW("cc_filesystem", "Destination installed; backup retained (cleanup errno=%d)", saved);
        free(backup);
        return cc_result_ok();
    }
    free(backup);
    return cc_result_ok();
}

/**
 * @brief 打开现有文件并请求 VFS 执行同步。
 * @param[in,out] self 借用存活后端，当前实现忽略。
 * @param[in] path 调用期借用的文件路径。
 * @return 打开、同步和关闭均成功为 CC_OK，其他情况返回拥有错误结果。
 * @note 本入口只能同步当前后端可见状态，其他未提交 FILE 缓冲由其写入者先刷新。
 * 物理持久化由 VFS、文件系统与设备实现，诊断由调用者释放。
 */
static cc_result_t esp32_sync_file(void *self, const char *path)
{
    (void)self;
    cc_result_t rc = esp32_validate_path(path);
    if (rc.code != CC_OK) return rc;
    FILE *f = fopen(path, "rb");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file for sync errno=%d", errno);
    int failed = fsync(fileno(f)) != 0;
    int saved = failed ? errno : 0;
    if (fclose(f) != 0 && !failed) { failed = 1; saved = errno; }
    return failed ? cc_result_errf(CC_ERR_IO, "File sync/close failed errno=%d", saved) : cc_result_ok();
}

/**
 * @brief 返回当前 ESP VFS 端口的目录同步能力限制。
 * @param[in,out] self 忽略的借用后端。
 * @param[in] path 忽略的借用路径，可为 NULL。
 * @return 始终返回 UNSUPPORTED，拥有诊断由调用者释放。
 */
static cc_result_t esp32_sync_dir(void *self, const char *path)
{
    (void)self; (void)path;
    return cc_result_error(CC_ERR_UNSUPPORTED, "Directory sync is unavailable on this ESP VFS");
}

/**
 * @brief 校验根相对路径的组件与终止规则。
 * @param[in] path 调用期借用的 NUL 结尾相对路径。
 * @return 非空相对路径且组件合法为一，绝对、反斜杠、空/点组件或控制终止符为零。
 * 配置采用 FatFs 短文件名时同时拒绝会终止名称的空格。
 * @note 当前端口按无符号链接的板级文件系统使用，调用方负责实际挂载与根路径寿命。
 */
static int esp32_relative_valid(const char *path)
{
    if (!path || !path[0] || path[0] == '/' || strchr(path, '\\')) return 0;
    const char *segment = path;
    for (const char *p = path;; p++) {
        if (*p && (unsigned char)*p < 0x20U) return 0;
#if defined(CONFIG_FATFS_LFN_NONE) && CONFIG_FATFS_LFN_NONE
        if (*p == ' ') return 0;
#endif
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
 * @brief 检查根与相对路径并创建完整 VFS 路径副本。
 * @param[in] root 调用期借用的非空根路径。
 * @param[in] relative_path 调用期借用的合法相对路径。
 * @param[out] out_path 必填空拥有槽，成功取得独占路径并最终 free。
 * @return 成功为 CC_OK，根约束、容量或分配失败返回可定位拥有结果。
 * @note 组件约束完成后才分配，外层协调根目录与挂载的并发变化。
 */
static cc_result_t esp32_join_relative(const char *root, const char *relative_path, char **out_path)
{
    if (!out_path) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null path output");
    *out_path = NULL;
    if (!root || !root[0] || !esp32_relative_valid(relative_path))
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "Unsafe workspace path");
    cc_result_t rc = esp32_validate_path(root);
    if (rc.code != CC_OK) return rc;
    for (const unsigned char *p = (const unsigned char *)root; *p; p++) {
        if (*p < 0x20U)
            return cc_result_error(CC_ERR_PERMISSION_DENIED, "Workspace root contains a path terminator");
#if defined(CONFIG_FATFS_LFN_NONE) && CONFIG_FATFS_LFN_NONE
        if (*p == ' ')
            return cc_result_error(CC_ERR_PERMISSION_DENIED, "Workspace root contains an SFN terminator");
#endif
    }
    size_t a = strlen(root), b = strlen(relative_path);
    if (b > SIZE_MAX - 2U || a > SIZE_MAX - b - 2U)
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Workspace path is too long");
    char *path = malloc(a + b + 2);
    if (!path) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate workspace path");
    memcpy(path, root, a);
    if (a > 0 && root[a - 1] != '/') path[a++] = '/';
    memcpy(path + a, relative_path, b + 1);
    *out_path = path;
    return cc_result_ok();
}

/**
 * @brief 在根约束路径上完整读取文本。
 * @param[in,out] self 借用存活后端。
 * @param[in] root 调用期借用的非空根路径。
 * @param[in] relative_path 调用期借用的相对路径。
 * @param[out] out_text 必填空拥有槽，入口清空，成功字符串由调用者 free。
 * @return 路径构造或文本读取的拥有结果，调用者释放诊断。
 */
static cc_result_t esp32_read_relative_text(
    void *self, const char *root, const char *relative_path, char **out_text)
{
    if (!out_text) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null text output");
    *out_text = NULL;
    char *path = NULL;
    cc_result_t rc = esp32_join_relative(root, relative_path, &path);
    if (rc.code != CC_OK) return rc;
    rc = esp32_read_text(self, path, out_text);
    free(path);
    return rc;
}

/**
 * @brief 将至多一 MiB 文本写入独占临时文件，刷新后通过恢复安装流程提交。
 * @param[in,out] self 借用存活后端。
 * @param[in] root 调用期借用的非空根路径。
 * @param[in] relative_path 调用期借用的相对目标路径。
 * @param[in] text 借用的 NUL 结尾正文，NULL 按空文本处理。
 * @return 完整安装为 CC_OK，路径、容量、分配或 I/O 失败返回拥有结果。
 * @note mkstemp 独占创建本次临时文件，旧 .tmp 及其他候选继续保留。提交失败可能已移动目标，
 * 拥有者串行协调目标与 .bak 文件，并结合上层恢复协议核对最终状态，诊断由调用者释放。
 */
static cc_result_t esp32_write_relative_text(
    void *self, const char *root, const char *relative_path, const char *text)
{
    char *path = NULL;
    cc_result_t rc = esp32_join_relative(root, relative_path, &path);
    if (rc.code != CC_OK) return rc;
    const char *safe_text = text ? text : "";
    size_t text_len = strlen(safe_text);
    if (text_len > CC_ESP32_MAX_TEXT_FILE_BYTES) {
        free(path);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Text file exceeds 1 MiB write limit");
    }
    size_t path_len = strlen(path);
    if (path_len > SIZE_MAX - 12U) {
        free(path);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Text file path is too long");
    }
    char *temporary = malloc(path_len + 12U);
    if (!temporary) {
        free(path);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate temporary path");
    }
    memcpy(temporary, path, path_len);
    memcpy(temporary + path_len, ".tmp.XXXXXX", 12U);
    int descriptor = mkstemp(temporary);
    if (descriptor < 0) {
        free(temporary);
        free(path);
        return cc_result_error(CC_ERR_IO, "Cannot create exclusive temporary file");
    }
    FILE *file = fdopen(descriptor, "wb");
    if (!file) {
        close(descriptor);
        remove(temporary);
        free(temporary);
        free(path);
        return cc_result_error(CC_ERR_IO, "Cannot open temporary file stream");
    }
    size_t written = text_len ? fwrite(safe_text, 1, text_len, file) : 0;
    int failed = written != text_len || ferror(file);
    if (!failed && fflush(file) != 0) failed = 1;
    if (!failed && fsync(fileno(file)) != 0) failed = 1;
    if (fclose(file) != 0) failed = 1;
    rc = failed ? cc_result_error(CC_ERR_IO, "Cannot write and sync complete temporary file")
                : esp32_atomic_replace(self, temporary, path);
    if (rc.code != CC_OK) {
        (void)remove(temporary);
    }
    free(temporary);
    free(path);
    return rc;
}

/**
 * @brief 在根约束路径上创建或覆盖原始字节。
 * @param[in,out] self 借用存活后端。
 * @param[in] root 调用期借用的非空根路径。
 * @param[in] relative_path 调用期借用的相对目标路径。
 * @param[in] data 调用期借用的 size 字节，零长度允许 NULL。
 * @param[in] size 写入字节数。
 * @return 路径构造或普通字节写入的拥有结果，调用者释放诊断。
 * @note 失败可能已截断或写入目标前缀，拥有者协调并发写与挂载寿命。
 */
static cc_result_t esp32_write_relative_bytes(
    void *self, const char *root, const char *relative_path, const void *data, size_t size)
{
    char *path = NULL;
    cc_result_t rc = esp32_join_relative(root, relative_path, &path);
    if (rc.code != CC_OK) return rc;
    rc = esp32_write_bytes(self, path, data, size);
    free(path);
    return rc;
}

/**
 * @brief 使用 VFS access 查询能否确认路径存在。
 * @param[in,out] self 借用存活后端，当前实现忽略。
 * @param[in] path 调用期借用的路径。
 * @param[out] out_exists 必填槽，入口清零，access 成功时写一。
 * @return 参数或路径错误返回拥有结果，其余为 CC_OK，诊断由调用者释放。
 * @note access 失败沿既有约定归为未确认存在，文件并发与权限变化由使用方解释。
 */
static cc_result_t esp32_exists(void *self, const char *path, int *out_exists)
{
    (void)self;
    if (!out_exists) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null existence output");
    *out_exists = 0;
    cc_result_t rc = esp32_validate_path(path);
    if (rc.code != CC_OK) return rc;
    *out_exists = (path && access(path, F_OK) == 0) ? 1 : 0;
    return cc_result_ok();
}

/**
 * @brief 完整列举目录中的非点名称并交付独占数组。
 * @param[in,out] self 借用存活后端，当前实现忽略。
 * @param[in] path 调用期借用的目录路径。
 * @param[out] out_items 必填空拥有槽，入口置 NULL，成功由调用者逐项及数组 free。
 * @param[out] out_count 必填数量槽，入口置零，成功写名称数量。
 * @return 成功为 CC_OK，参数、容量、分配、遍历或关闭失败返回拥有错误结果。
 * @note 返回前执行目录关闭并释放失败部分数组，原生关闭报错后的卷状态由 BSP 核对。
 */
static cc_result_t esp32_list_dir(void *self, const char *path, char ***out_items, size_t *out_count)
{
    (void)self;
    if (out_items) *out_items = NULL;
    if (out_count) *out_count = 0;
    if (!out_items || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid directory outputs");
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
    int read_error = 0;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) { read_error = errno; break; }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (count >= cap) {
            if (cap > SIZE_MAX / sizeof(*items) / 2U) {
                for (size_t i = 0; i < count; i++) free(items[i]);
                free(items);
                closedir(dir);
                return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Directory list is too large");
            }
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
    int close_error = closedir(dir);
    if (read_error || close_error != 0) {
        for (size_t i = 0; i < count; i++) free(items[i]);
        free(items);
        return cc_result_errf(CC_ERR_IO, "Cannot read or close directory: %s", path);
    }
    *out_items = items;
    *out_count = count;
    return cc_result_ok();
}

/**
 * @brief 通过 stat 或目录打开确认路径当前可作为目录。
 * @param[in] path 调用期借用的路径，可为 NULL。
 * @return stat 识别为目录或 opendir 成功返回一，其余为零。
 * @note opendir 成功后关闭临时句柄，兼容部分 VFS 对 stat 目录类型支持有限的情况。
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

/**
 * @brief 创建单级目录或确认该路径已经是目录。
 * @param[in] path 调用期借用的有效路径，参数形式由外层检查。
 * @return 创建或目录确认成功为 CC_OK，其余为 IO，拥有诊断由调用者释放。
 * @note mkdir 失败后重新查询目录，沿 VFS 能力兼容已有目录，普通同名文件保持错误。
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
    if (esp32_is_dir(path)) {
        return cc_result_ok();
    }
    return cc_result_errf(CC_ERR_IO, "Cannot create directory: %s errno=%d", path, mkdir_errno);
}

/**
 * @brief 按分隔符逐级创建或确认目录。
 * @param[in,out] self 借用存活后端，当前实现忽略。
 * @param[in] path 调用期借用的非空 VFS 路径。
 * @return 全部目录有效为 CC_OK，路径、分配或 I/O 失败返回拥有错误结果。
 * @note 失败可能已创建前缀目录，临时路径副本总会归还，业务顺序由外层协调。
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

/**
 * @brief 通过普通 VFS 路径删除文件或空目录。
 * @param[in,out] self 借用存活后端，当前实现忽略。
 * @param[in] path 调用期借用的路径。
 * @return 成功为 CC_OK，路径或 remove 失败返回拥有错误结果，调用者释放诊断。
 * @note 目标选择和文件并发由上层协调，缺失对象沿 VFS 返回 IO。
 */
static cc_result_t esp32_remove(void *self, const char *path)
{
    (void)self;
    cc_result_t rc = esp32_validate_path(path);
    if (rc.code != CC_OK) return rc;
    if (remove(path) != 0)
        return cc_result_errf(CC_ERR_IO, "Cannot remove: %s", path);
    return cc_result_ok();
}

/**
 * @brief 消费工厂创建的文件端口状态。
 * @param[in,out] self 独占的堆状态，可为 NULL。
 * @pre 拥有者已经关闭调用入口并排空全部文件操作及描述符借用。
 * @post 状态分配已释放，挂载与底层驱动继续由 BSP 拥有。
 */
static void esp32_destroy(void *self)
{
    free(self);
}

/* ESP32 文件端口方法表。 */
static cc_filesystem_vtable_t esp32_vtable = {
    .size = sizeof(cc_filesystem_vtable_t),
    .version = CC_FILESYSTEM_VTABLE_VERSION,
    .capabilities = CC_FS_CAP_BINARY | CC_FS_CAP_APPEND | CC_FS_CAP_STAT |
        CC_FS_CAP_ATOMIC_REPLACE | CC_FS_CAP_FILE_SYNC | CC_FS_CAP_ROOT_SCOPED |
        CC_FS_CAP_NO_SYMLINKS | CC_FS_CAP_READ_TAIL,
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
    .read_tail = esp32_read_tail,
};

/**
 * @brief 创建依赖已挂载 VFS 的 ESP32 文件端口。
 * @param[out] out_fs 必填空描述符，成功交付独占 self 与静态 vtable，失败保持清零。
 * @return 成功为 CC_OK，空输出为 INVALID_ARGUMENT，状态分配失败为 OUT_OF_MEMORY。
 * @note 拥有结果诊断由调用者释放，工厂只分配端口状态，BSP/应用负责挂载、介质寿命及故障恢复。
 * 使用方协调同一文件及其恢复候选的并发访问，最后通过 vtable destroy 消费状态。
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

/**
 * @brief 通过通用命名别名创建当前 ESP32 默认文件端口。
 * @param[out] out_fs 必填空描述符，成功取得 ESP32 状态及方法表。
 * @return 直接转交默认工厂的拥有结果，调用者释放诊断并最终销毁成功状态。
 * @note 名称沿公共工厂接口保留，实际调用能力由 ESP32 VFS 后端提供。
 */
cc_result_t cc_filesystem_get_posix(cc_filesystem_t *out_fs)
{
    return cc_filesystem_get_default(out_fs);
}
#else
#error "cc_esp32_filesystem.c must be built under ESP-IDF"
#endif
