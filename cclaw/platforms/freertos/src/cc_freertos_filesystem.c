#include "cc/ports/cc_filesystem.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#if defined(CCLAW_FREERTOS_ENABLE_FATFS) && CCLAW_FREERTOS_ENABLE_FATFS

#include "ff.h"

/** @brief 无可变配置的 FatFs 端口对象，卷挂载和故障恢复归 BSP 管理。 */
typedef struct cc_freertos_fatfs {
    int unused; /**< 保证私有对象非零大小的占位字段。 */
} cc_freertos_fatfs_t;

static const char *const k_mount_prefix = "/sdcard"; /**< SDK 挂载前缀。 */
static const char *const k_workspace_prefix = "/sdcard/cclaw/workspace"; /**< 映射到卷根的兼容工作区别名。 */

/**
 * @brief 将 SDK 路径映射到卷 0，工作区别名直接映射卷根。
 * @param[in] path 借用路径，NULL 返回参数错误，空串沿既有规则映射卷根。
 * @param[out] out 可写 out_len 字节缓冲，成功得到 NUL 结尾 FatFs 路径。
 * @param[in] out_len 缓冲字节容量，必须非零。
 * @return 成功返回 CC_OK，参数或完整输出放不下返回 INVALID_ARGUMENT，失败内容不可使用。
 * @note 普通路径保留 FatFs 字符和终止语义，本适配器要求窄字符 TCHAR，卷及编码配置由 BSP 提供。
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
    if (strncmp(path, k_mount_prefix, prefix_len) == 0 &&
        (path[prefix_len] == '\0' || path[prefix_len] == '/')) {
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

/**
 * @brief 在原生 0:/ 路径上逐级创建父目录。
 * @param[in] fpath 借用 map_path 输出，须完整容纳于 256 字节。
 * @return 成功返回 CC_OK，容量错误返回 INVALID_ARGUMENT，创建或已有对象非目录返回 PLATFORM。
 * @note 失败保留此前创建的目录，路径与类型检查之间的竞争由调用者协调。
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
            if (fr == FR_EXIST) {
                FILINFO info;
                fr = f_stat(parent, &info);
                if (fr == FR_OK && !(info.fattrib & AM_DIR)) fr = FR_DENIED;
            }
            if (fr != FR_OK) return cc_result_error(CC_ERR_PLATFORM, "FatFs parent mkdir failed");
            *p = '/';
        }
    }
    FRESULT fr = f_mkdir(parent);
    if (fr == FR_EXIST) {
        FILINFO info;
        fr = f_stat(parent, &info);
        if (fr == FR_OK && !(info.fattrib & AM_DIR)) fr = FR_DENIED;
    }
    if (fr != FR_OK) return cc_result_error(CC_ERR_PLATFORM, "FatFs parent mkdir failed");
    return cc_result_ok();
}

/**
 * @brief 将 FatFs 错误码和操作名复制到拥有型 SDK 结果。
 * @param[in] res 原生错误码，NOT_ENOUGH_CORE 归为 OOM，其余归为 PLATFORM。
 * @param[in] op 非 NULL 的借用操作名，用于受限长度诊断。
 * @return 动态诊断由调用方 cc_result_free，格式化或消息分配失败仍保留错误分类。
 */
static cc_result_t fatfs_error(FRESULT res, const char *op)
{
    char msg[96];
    snprintf(msg, sizeof(msg), "%s failed: FatFs=%u", op, (unsigned)res);
    return cc_result_error(res == FR_NOT_ENOUGH_CORE ? CC_ERR_OUT_OF_MEMORY : CC_ERR_PLATFORM, msg);
}

/**
 * @brief 对已打开 FIL 执行一次关闭，成功交付原结果，关闭失败保留操作上下文并返回平台错误。
 * @param[in,out] file 借用的有效文件对象，本函数调用 f_close，成功后对象失效。
 * @param[in] result 按值交接拥有型操作结果，关闭失败时消费其动态成员。
 * @return 调用方拥有的结果，关闭失败不会被原先的成功、OOM 或读写错误掩盖。
 * @note f_close 失败时原生卷 share 记录可能保留，后端恢复归 BSP，不能假设局部 FIL 已完成退场。
 */
static cc_result_t fatfs_finish_file(FIL *file, cc_result_t result)
{
    FRESULT closed = f_close(file);
    if (closed == FR_OK) return result;
    cc_result_t failure = cc_result_errf(CC_ERR_PLATFORM, "%s; f_close=%u",
        result.message ? result.message : "FatFs file operation", (unsigned)closed);
    cc_result_free(&result);
    return failure;
}

/**
 * @brief 将文件末尾的有限字节读取到调用方缓冲。
 * @param[in] self 当前不读取，可为 NULL。
 * @param[in] path 借用 SDK 路径，按 map_path 映射。
 * @param[out] buffer 可写 capacity 字节，零容量允许 NULL，失败可能已写部分数据。
 * @param[in] capacity 字节容量，必须可由 UINT 表示。
 * @param[out] out_size 必需槽，入口置零，完整读取并成功关闭才交付长度。
 * @return 成功返回 CC_OK，参数或映射错误、短读及 FatFs 操作失败返回对应错误。
 */
static cc_result_t fatfs_read_tail(void *self, const char *path, void *buffer,
                                   size_t capacity, size_t *out_size)
{
    (void)self;
    if (out_size) *out_size = 0;
    if (!out_size || (!buffer && capacity) || (size_t)(UINT)capacity != capacity)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid tail read");
    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    if (rc.code != CC_OK) return rc;
    FIL file;
    FRESULT fr = f_open(&file, fpath, FA_READ);
    if (fr != FR_OK) return fatfs_error(fr, "f_open");
    FSIZE_t length = f_size(&file);
    UINT wanted = length < (FSIZE_t)capacity ? (UINT)length : (UINT)capacity;
    fr = f_lseek(&file, length - wanted);
    UINT got = 0;
    if (fr == FR_OK && wanted) fr = f_read(&file, buffer, wanted, &got);
    cc_result_t result = fr != FR_OK ? fatfs_error(fr, "f_read tail") :
        (got != wanted ? cc_result_error(CC_ERR_PLATFORM, "Short FatFs tail read") : cc_result_ok());
    result = fatfs_finish_file(&file, result);
    if (result.code != CC_OK) return result;
    *out_size = got;
    return result;
}

/**
 * @brief 从已经映射的原生路径读取完整文本，分块读取以适配较窄的 UINT。
 * @param[in] fpath 借用的 NUL 结尾原生路径，调用方负责映射和根相对校验。
 * @param[out] out_text 可选输出槽，入口置 NULL，成功交付调用方 free 的字符串，省略时读后释放。
 * @return 文件超过 256 KiB 返回 INVALID_ARGUMENT，容量或分配失败返回对应错误，读或关闭失败不交付部分字符串。
 * @note 文本保留文件中的全部字节并追加 NUL，内嵌 NUL 的解释归调用方，读取期间外部修改文件须由 owner 协调。
 */
static cc_result_t fatfs_read_mapped(const char *fpath, char **out_text)
{
    if (out_text) *out_text = NULL;
    FIL file;
    FRESULT fr = f_open(&file, fpath, FA_READ);
    if (fr != FR_OK) return fatfs_error(fr, "f_open");

    FSIZE_t length = f_size(&file);
    if (length > 256UL * 1024UL) {
        return fatfs_finish_file(&file,
            cc_result_error(CC_ERR_INVALID_ARGUMENT, "File is too large for FatFs text read"));
    }
    if ((uint64_t)length >= SIZE_MAX) {
        return fatfs_finish_file(&file,
            cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Text size exceeds addressable buffer"));
    }
    size_t size = (size_t)length;
    char *text = malloc(size + 1);
    if (!text) {
        return fatfs_finish_file(&file,
            cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate file buffer"));
    }
    size_t offset = 0;
    cc_result_t result = cc_result_ok();
    while (offset < size) {
        size_t remaining = size - offset;
        UINT wanted = remaining > (size_t)((UINT)-1) ? (UINT)-1 : (UINT)remaining;
        UINT got = 0;
        fr = f_read(&file, text + offset, wanted, &got);
        if (fr != FR_OK || got != wanted) {
            result = fr != FR_OK ? fatfs_error(fr, "f_read") :
                cc_result_error(CC_ERR_PLATFORM, "Short FatFs text read");
            break;
        }
        offset += got;
    }
    result = fatfs_finish_file(&file, result);
    if (result.code != CC_OK) {
        free(text);
        return result;
    }
    text[size] = '\0';
    if (out_text) *out_text = text;
    else free(text);
    return result;
}

/**
 * @brief 映射 SDK 路径后读取最多 256 KiB 的完整文本。
 * @param[in] self 当前不读取，可为 NULL。
 * @param[in] path 借用路径，NULL 返回参数错误。
 * @param[out] out_text 可选拥有型输出槽，入口置 NULL，成功字符串由调用方 free。
 * @return 映射、分配、完整读取和关闭结果，失败保持输出 NULL。
 */
static cc_result_t fatfs_read(void *self, const char *path, char **out_text)
{
    (void)self;
    if (out_text) *out_text = NULL;
    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    return rc.code == CC_OK ? fatfs_read_mapped(fpath, out_text) : rc;
}

/**
 * @brief 覆盖写入原生路径，缺父目录时逐级创建并重新打开。
 * @param[in] fpath 借用完整映射路径，已通过调用方的根相对检查。
 * @param[in] data 借用 size 字节，零长度允许 NULL。
 * @param[in] size 写入字节数，超过 UINT 范围在打开文件前返回 LIMIT_EXCEEDED。
 * @return 完整写入并关闭成功返回 CC_OK，参数、父目录、写入及关闭错误按实际阶段返回。
 * @note 成功打开即可截断已有文件，失败可能保留部分文件或已创建目录，不提供替换事务。
 */
static cc_result_t fatfs_write_mapped(const char *fpath, const void *data, size_t size)
{
    if (!data && size > 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid write_bytes data");
    }
    UINT write_size = (UINT)size;
    if ((size_t)write_size != size) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "FatFs write exceeds UINT range");
    }

    FIL file;
    FRESULT fr = f_open(&file, fpath, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr == FR_NO_PATH) {
        cc_result_t rc = ensure_parent_dir(fpath);
        if (rc.code != CC_OK) return rc;
        fr = f_open(&file, fpath, FA_CREATE_ALWAYS | FA_WRITE);
    }
    if (fr != FR_OK) return fatfs_error(fr, "f_open");

    UINT wrote = 0;
    fr = f_write(&file, size > 0 ? data : "", write_size, &wrote);
    cc_result_t result = fr != FR_OK ? fatfs_error(fr, "f_write") :
        ((size_t)wrote != size ? cc_result_error(CC_ERR_PLATFORM, "Short FatFs write") : cc_result_ok());
    return fatfs_finish_file(&file, result);
}

/**
 * @brief 将 SDK 路径映射一次后覆盖写入完整字节序列。
 * @param[in] self 当前不读取，可为 NULL。
 * @param[in] path 借用 SDK 路径。
 * @param[in] data 借用 size 字节，零长度允许 NULL。
 * @param[in] size 字节长度，必须可由 UINT 表示。
 * @return 返回映射或原生写入结果，成功关闭包含 FatFs 自身同步，失败可能已修改目标。
 */
static cc_result_t fatfs_write_bytes(void *self, const char *path, const void *data, size_t size)
{
    (void)self;
    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    return rc.code == CC_OK ? fatfs_write_mapped(fpath, data, size) : rc;
}

/**
 * @brief 以覆盖字节写入实现文本写入。
 * @param[in] self 透传给字节接口，当前不读取。
 * @param[in] path 借用 SDK 路径。
 * @param[in] text 借用 NUL 结尾文本，NULL 按空文本处理。
 * @return 返回映射、长度范围、打开、完整写入及关闭结果，失败可能已截断或部分写入。
 */
static cc_result_t fatfs_write(void *self, const char *path, const char *text)
{
    const char *safe_text = text ? text : "";
    return fatfs_write_bytes(self, path, safe_text, strlen(safe_text));
}

/**
 * @brief 按打开时文件长度分块读取完整二进制文件。
 * @param[in] self 当前不读取，可为 NULL。
 * @param[in] path 借用 SDK 路径。
 * @param[out] out_data 必需拥有型槽，入口置 NULL，成功由调用方 free，空文件也交付可释放指针。
 * @param[out] out_size 必需字节长度槽，入口置零，仅完整读取并成功关闭才交付。
 * @return 参数、size_t 容量、分配、短读或原生错误，失败释放私有缓冲。
 * @note 不提供跨外部修改者的一致快照，内存峰值约为整个文件大小。
 */
static cc_result_t fatfs_read_bytes(
    void *self, const char *path, void **out_data, size_t *out_size)
{
    (void)self;
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0;
    if (!out_data || !out_size) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid binary read");
    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    if (rc.code != CC_OK) return rc;
    FIL file;
    FRESULT fr = f_open(&file, fpath, FA_READ);
    if (fr != FR_OK) return fatfs_error(fr, "f_open");
    FSIZE_t length = f_size(&file);
    if ((uint64_t)length > SIZE_MAX) {
        return fatfs_finish_file(&file, cc_result_error(CC_ERR_LIMIT_EXCEEDED, "File is too large"));
    }
    size_t size = (size_t)length;
    void *data = malloc(size ? size : 1);
    if (!data) {
        return fatfs_finish_file(&file, cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate file data"));
    }
    cc_result_t result = cc_result_ok();
    size_t offset = 0;
    while (offset < size) {
        size_t remaining = size - offset;
        UINT wanted = remaining > (size_t)((UINT)-1) ? (UINT)-1 : (UINT)remaining;
        UINT got = 0;
        fr = f_read(&file, (unsigned char *)data + offset, wanted, &got);
        if (fr != FR_OK || got != wanted) {
            result = fr != FR_OK ? fatfs_error(fr, "f_read") :
                cc_result_error(CC_ERR_PLATFORM, "Short FatFs binary read");
            break;
        }
        offset += got;
    }
    result = fatfs_finish_file(&file, result);
    if (result.code != CC_OK) {
        free(data);
        return result;
    }
    *out_data = data; *out_size = size;
    return result;
}

/**
 * @brief 向文件末尾追加一段可由 UINT 表示的字节，再同步并关闭。
 * @param[in] self 当前不读取，可为 NULL。
 * @param[in] path 借用 SDK 路径，父目录须存在。
 * @param[in] data 借用 size 字节，零长度允许 NULL。
 * @param[in] size 字节数，超过 UINT 在打开目标前返回 LIMIT_EXCEEDED。
 * @return 完整写入、同步和关闭成功返回 CC_OK，失败可能保留部分追加内容。
 */
static cc_result_t fatfs_append_bytes(
    void *self, const char *path, const void *data, size_t size)
{
    (void)self;
    if (!data && size > 0) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid append data");
    if ((size_t)(UINT)size != size) return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "FatFs append exceeds UINT range");
    char fpath[256];
    cc_result_t rc = map_path(path, fpath, sizeof(fpath));
    if (rc.code != CC_OK) return rc;
    FIL file;
    FRESULT fr = f_open(&file, fpath, FA_OPEN_APPEND | FA_WRITE);
    if (fr != FR_OK) return fatfs_error(fr, "f_open append");
    UINT written = 0;
    fr = f_write(&file, size ? data : "", (UINT)size, &written);
    if (fr == FR_OK) fr = f_sync(&file);
    cc_result_t result = fr != FR_OK ? fatfs_error(fr, "f_write/f_sync append") :
        (written != size ? cc_result_error(CC_ERR_PLATFORM, "Short FatFs append") : cc_result_ok());
    return fatfs_finish_file(&file, result);
}

/**
 * @brief 查询路径类型与字节大小，缺失路径作为正常结果。
 * @param[in] self 当前不读取，可为 NULL。
 * @param[in] path 借用 SDK 路径。
 * @param[out] out_stat 必需槽，入口清零并设置 size，成功填类型和大小。
 * @return 缺失或查询成功返回 CC_OK，其余返回参数、映射或原生错误。
 * @note modified_ms 保持零，FatFs 本地日期时间未由本端口转换为 Unix 毫秒。
 */
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

/**
 * @brief 明确拒绝本端口无法保证的原子替换。
 * @param[in] self 不读取，可为 NULL。
 * @param[in] temporary_path 不读取，未转移源文件所有权。
 * @param[in] destination_path 不读取，目标保持原状。
 * @return 始终返回 UNSUPPORTED，不执行删除、重命名或路径解析。
 */
static cc_result_t fatfs_atomic_replace(void *self, const char *temporary_path, const char *destination_path)
{
    (void)self; (void)temporary_path; (void)destination_path;
    return cc_result_error(CC_ERR_UNSUPPORTED, "FatFs atomic replace is unavailable");
}

/**
 * @brief 重新打开路径并执行 FatFs 的同步与关闭流程。
 * @param[in] self 当前不读取，可为 NULL。
 * @param[in] path 借用已存在文件的 SDK 路径。
 * @return 打开、f_sync 或 f_close 的结果，关闭失败传播为平台错误。
 * @note 新 FIL 通常无 FA_MODIFIED，f_sync 可为空操作，不能替代其他仍打开 FIL 的同步，也不提供额外介质屏障保证。
 */
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
    return fatfs_finish_file(&file, fr == FR_OK ? cc_result_ok() : fatfs_error(fr, "f_sync"));
}

/**
 * @brief 拒绝 FatFs 未提供的目录同步入口。
 * @param[in] self 不读取，可为 NULL。
 * @param[in] path 不读取，不执行目录操作。
 * @return 始终返回 UNSUPPORTED。
 */
static cc_result_t fatfs_sync_dir(void *self, const char *path)
{
    (void)self; (void)path;
    return cc_result_error(CC_ERR_UNSUPPORTED, "FatFs directory sync is unavailable");
}

/**
 * @brief 按 FatFs 文件名配置拒绝根相对操作中会提前终止或退回父目录的输入。
 * @param[in] path 借用相对路径，NULL、空串、绝对路径和反斜杠均无效。
 * @return 所有组件可按给定顺序解析返回 1，否则返回 0。
 * @note LFN 模式保留普通空格与原生尾点空白别名，SFN 模式的空格会终止名称，因而拒绝含空格输入。
 */
static int fatfs_relative_valid(const char *path)
{
    if (!path || !path[0] || path[0] == '/' || strchr(path, '\\')) return 0;
    const char *segment = path;
    for (const char *p = path;; p++) {
        if (*p && (unsigned char)*p < 0x20) return 0;
#if (defined(FF_USE_LFN) && FF_USE_LFN == 0) || (!defined(FF_USE_LFN) && defined(_USE_LFN) && _USE_LFN == 0)
        if (*p == ' ') return 0;
#endif
        if (*p == '/' || *p == '\0') {
            size_t len = (size_t)(p - segment);
            if (len == 0 || (len == 1 && segment[0] == '.') ||
                (len == 2 && segment[0] == '.' && segment[1] == '.')) return 0;
            size_t meaningful = len;
            while (meaningful && (segment[meaningful - 1] == '.' || segment[meaningful - 1] == ' ')) meaningful--;
            if (!meaningful) return 0;
            if (*p == '\0') break;
            segment = p + 1;
        }
    }
    return 1;
}

/**
 * @brief 先确定根的原生位置，再附加已校验相对路径，避免完整字符串触发工作区别名重映射。
 * @param[in] root 借用非空 SDK 根路径，控制字节及 SFN 空格终止形式无效。
 * @param[in] relative_path 借用非空相对路径，须通过组件检查。
 * @param[out] out 可写 out_size 字节，成功得到原生路径，失败内容不可使用。
 * @param[in] out_size 输出容量，内部调用传 256 字节。
 * @return 非法根或相对路径返回 PERMISSION_DENIED，参数、映射或容量错误按实际原因返回。
 * @note 根目录及卷在调用期间由 owner 保持稳定，本端口不持有目录句柄和独立命名空间锁。
 */
static cc_result_t fatfs_relative_path(
    const char *root, const char *relative_path, char *out, size_t out_size)
{
    if (!root || !root[0] || !fatfs_relative_valid(relative_path)) {
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "Unsafe workspace path");
    }
    for (const char *p = root; *p; p++) {
        if ((unsigned char)*p < 0x20) return cc_result_error(CC_ERR_PERMISSION_DENIED, "Unsafe workspace root");
#if (defined(FF_USE_LFN) && FF_USE_LFN == 0) || (!defined(FF_USE_LFN) && defined(_USE_LFN) && _USE_LFN == 0)
        if (*p == ' ') return cc_result_error(CC_ERR_PERMISSION_DENIED, "Unsafe SFN workspace root");
#endif
    }
    if (!out || !out_size) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid workspace output");
    char native_root[256];
    cc_result_t rc = map_path(root, native_root, sizeof(native_root));
    if (rc.code != CC_OK) return rc;
    size_t root_len = strlen(native_root);
    int n = snprintf(out, out_size, "%s%s%s", native_root,
        native_root[root_len - 1] == '/' ? "" : "/", relative_path);
    return n < 0 || (size_t)n >= out_size ?
        cc_result_error(CC_ERR_INVALID_ARGUMENT, "Workspace path is too long") : cc_result_ok();
}

/**
 * @brief 在映射后的根下读取完整文本，避免重复路径映射改变目标。
 * @param[in] self 当前不读取，可为 NULL。
 * @param[in] root 借用 SDK 根目录，卷和目录生命周期由调用方维持。
 * @param[in] relative_path 借用受限相对路径。
 * @param[out] out_text 可选拥有型槽，入口置 NULL，成功字符串由调用方 free。
 * @return 根相对校验、容量、完整文本读取和关闭结果，失败不交付部分文本。
 */
static cc_result_t fatfs_read_relative_text(
    void *self, const char *root, const char *relative_path, char **out_text)
{
    (void)self;
    if (out_text) *out_text = NULL;
    char path[256];
    cc_result_t rc = fatfs_relative_path(root, relative_path, path, sizeof(path));
    return rc.code == CC_OK ? fatfs_read_mapped(path, out_text) : rc;
}

/**
 * @brief 在映射后的根下覆盖写入文本，缺父目录时逐级创建。
 * @param[in] self 当前不读取，可为 NULL。
 * @param[in] root 借用 SDK 根目录。
 * @param[in] relative_path 借用受限相对路径。
 * @param[in] text 借用 NUL 结尾文本，NULL 按空串处理。
 * @return 校验、映射和原生写入结果，失败可能已截断目标或创建部分目录。
 */
static cc_result_t fatfs_write_relative_text(
    void *self, const char *root, const char *relative_path, const char *text)
{
    (void)self;
    char path[256];
    cc_result_t rc = fatfs_relative_path(root, relative_path, path, sizeof(path));
    const char *safe_text = text ? text : "";
    return rc.code == CC_OK ? fatfs_write_mapped(path, safe_text, strlen(safe_text)) : rc;
}

/**
 * @brief 在映射后的根下覆盖写入字节，目标与根在同一原生路径解释下。
 * @param[in] self 当前不读取，可为 NULL。
 * @param[in] root 借用 SDK 根目录。
 * @param[in] relative_path 借用受限相对路径。
 * @param[in] data 借用 size 字节，零长度允许 NULL。
 * @param[in] size 字节长度，须可由 UINT 表示。
 * @return 校验和完整写入关闭结果，失败可能保留部分文件，未承诺原子替换。
 */
static cc_result_t fatfs_write_relative_bytes(
    void *self, const char *root, const char *relative_path, const void *data, size_t size)
{
    (void)self;
    char path[256];
    cc_result_t rc = fatfs_relative_path(root, relative_path, path, sizeof(path));
    return rc.code == CC_OK ? fatfs_write_mapped(path, data, size) : rc;
}

/**
 * @brief 查询 SDK 路径是否存在，缺失路径作为正常结果。
 * @param[in] self 当前不读取，可为 NULL。
 * @param[in] path 借用 SDK 路径。
 * @param[out] out_exists 可选槽，入口置零，查询到对象后置 1。
 * @return 缺失或查询成功返回 CC_OK，其余返回映射或 FatFs 错误。
 */
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

/**
 * @brief 按 FatFs 枚举顺序复制当前目录的文件名，逐项扩展拥有型数组。
 * @param[in] self 当前不读取，可为 NULL。
 * @param[in] path 借用 SDK 目录路径。
 * @param[out] out_items 可选槽，入口置 NULL，成功后调用方逐项 free 再 free 数组，省略时内部释放。
 * @param[out] out_count 可选数量槽，入口置零，完整枚举并关闭才交付。
 * @return 分配失败返回 OOM，数组容量溢出返回 LIMIT_EXCEEDED，枚举或关闭失败清理全部部分结果。
 * @note 未提供跨其他目录修改者的快照，f_closedir 失败后的卷状态恢复由 BSP 负责。
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
    cc_result_t result = cc_result_ok();
    for (;;) {
        FILINFO info;
        fr = f_readdir(&dir, &info);
        if (fr != FR_OK || info.fname[0] == '\0') break;
        if (count >= SIZE_MAX / sizeof(*items)) {
            result = cc_result_error(CC_ERR_LIMIT_EXCEEDED, "FatFs directory array is too large");
            break;
        }
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
    if (fr != FR_OK) result = fatfs_error(fr, "f_readdir");
    FRESULT close_fr = f_closedir(&dir);
    if (close_fr != FR_OK) {
        cc_result_free(&result);
        result = fatfs_error(close_fr, "f_closedir");
    }
    if (result.code != CC_OK) {
        for (size_t i = 0; i < count; i++) free(items[i]);
        free(items);
        return result;
    }

    if (out_items) *out_items = items;
    else {
        for (size_t i = 0; i < count; i++) free(items[i]);
        free(items);
    }
    if (out_count) *out_count = count;
    return result;
}

/**
 * @brief 逐级创建 SDK 目录，已有对象必须经 f_stat 确认为目录。
 * @param[in] self 当前不读取，可为 NULL。
 * @param[in] path 借用 SDK 路径，映射后须完整放入 256 字节。
 * @return 各级创建或目录确认成功返回 CC_OK，失败保留此前已创建的目录并返回对应错误。
 * @note 无端口级目录树锁，检查期间的外部命名空间变更由 owner 协调。
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
            if (fr == FR_EXIST) {
                FILINFO info;
                fr = f_stat(partial, &info);
                if (fr == FR_OK && !(info.fattrib & AM_DIR)) fr = FR_DENIED;
            }
            if (fr != FR_OK) return fatfs_error(fr, "f_mkdir");
            *p = '/';
        }
    }
    FRESULT fr = f_mkdir(partial);
    if (fr == FR_EXIST) {
        FILINFO info;
        fr = f_stat(partial, &info);
        if (fr == FR_OK && !(info.fattrib & AM_DIR)) fr = FR_DENIED;
    }
    if (fr != FR_OK) return fatfs_error(fr, "f_mkdir");
    return cc_result_ok();
}

/**
 * @brief 按普通 SDK 路径映射规则删除文件或空目录。
 * @param[in] self 当前不读取，可为 NULL。
 * @param[in] path 借用待删除路径，本入口不执行根相对校验。
 * @return 删除成功返回 CC_OK，缺失、非空目录及其他原生错误原样分类到 SDK 结果。
 */
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

/**
 * @brief 释放本端口占位对象，卷挂载与卸载仍由 BSP 管理。
 * @param[in] self 交接 factory 创建的对象，NULL 安全。
 * @post 私有对象被释放，owner 须已停止全部端口调用，函数不卸载卷。
 */
static void fatfs_destroy(void *self)
{
    free(self);
}

/* FatFs 方法表，原子替换和目录同步未发布能力，卷锁及驱动同步由 BSP 配置。 */
static cc_filesystem_vtable_t freertos_fs_vtable = {
    .size = sizeof(cc_filesystem_vtable_t),
    .version = CC_FILESYSTEM_VTABLE_VERSION,
    .capabilities = CC_FS_CAP_BINARY | CC_FS_CAP_APPEND | CC_FS_CAP_STAT |
        CC_FS_CAP_FILE_SYNC | CC_FS_CAP_ROOT_SCOPED |
        CC_FS_CAP_NO_SYMLINKS | CC_FS_CAP_READ_TAIL,
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
    .read_tail = fatfs_read_tail,
};

#else

/**
 * @brief 未编入 FatFs 时清空文本输出并报告平台不可用。
 * @param[in] self 不读取，可为 NULL。
 * @param[in] path 不读取，可为 NULL。
 * @param[out] out_text 可选槽，提供时置 NULL，不分配内存。
 * @return 始终返回 PLATFORM。
 */
static cc_result_t unsupported_read(void *self, const char *path, char **out_text)
{
    (void)self;
    (void)path;
    if (out_text) *out_text = NULL;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

/**
 * @brief 未编入 FatFs 时拒绝文本写入。
 * @param[in] self 不读取，可为 NULL。
 * @param[in] path 不读取，可为 NULL。
 * @param[in] text 不读取，可为 NULL。
 * @return 始终返回 PLATFORM，无文件副作用。
 */
static cc_result_t unsupported_write(void *self, const char *path, const char *text)
{
    (void)self;
    (void)path;
    (void)text;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

/**
 * @brief 未编入 FatFs 时拒绝字节写入。
 * @param[in] self 不读取，可为 NULL。
 * @param[in] path 不读取，可为 NULL。
 * @param[in] data 不读取，不接管任何缓冲。
 * @param[in] size 不读取，单位为字节。
 * @return 始终返回 PLATFORM，无文件副作用。
 */
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

/**
 * @brief 未编入 FatFs 时沿既有缺失语义报告路径不存在。
 * @param[in] self 不读取，可为 NULL。
 * @param[in] path 不读取，可为 NULL。
 * @param[out] out_exists 可选槽，提供时置零。
 * @return 始终返回 CC_OK，能力位仍为零。
 */
static cc_result_t unsupported_exists(void *self, const char *path, int *out_exists)
{
    (void)self;
    (void)path;
    if (out_exists) *out_exists = 0;
    return cc_result_ok();
}

/**
 * @brief 未编入 FatFs 时清空目录输出并报告平台不可用。
 * @param[in] self 不读取，可为 NULL。
 * @param[in] path 不读取，可为 NULL。
 * @param[out] out_items 可选槽，提供时置 NULL。
 * @param[out] out_count 可选槽，提供时置零。
 * @return 始终返回 PLATFORM，不分配结果。
 */
static cc_result_t unsupported_list(void *self, const char *path, char ***out_items, size_t *out_count)
{
    (void)self;
    (void)path;
    if (out_items) *out_items = NULL;
    if (out_count) *out_count = 0;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

/**
 * @brief 未编入 FatFs 时拒绝创建目录。
 * @param[in] self 不读取，可为 NULL。
 * @param[in] path 不读取，可为 NULL。
 * @return 始终返回 PLATFORM，无目录副作用。
 */
static cc_result_t unsupported_make_dir(void *self, const char *path)
{
    (void)self;
    (void)path;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

/**
 * @brief 未编入 FatFs 时拒绝删除。
 * @param[in] self 不读取，可为 NULL。
 * @param[in] path 不读取，可为 NULL。
 * @return 始终返回 PLATFORM，无文件副作用。
 */
static cc_result_t unsupported_remove(void *self, const char *path)
{
    (void)self;
    (void)path;
    return cc_result_error(CC_ERR_PLATFORM, "FreeRTOS filesystem is not mounted");
}

/**
 * @brief 释放未编入 FatFs 分支的占位对象。
 * @param[in] self 交接 factory 分配的对象，NULL 安全。
 * @post 占位对象被释放，owner 须已结束所有调用。
 */
static void unsupported_destroy(void *self)
{
    free(self);
}

/* 未编入 FatFs 的方法表，扩展方法留空，基础 exists 保留缺失成功语义。 */
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

/**
 * @brief 创建当前编译配置的 FreeRTOS 文件系统端口。
 * @param[out] out_fs 必需槽，入口清零，成功交付拥有 self 的端口，失败保持空对象。
 * @return 参数错误返回 INVALID_ARGUMENT，分配失败返回 OOM，成功返回 CC_OK。
 * @note 启用 FatFs 时卷挂载、窄字符编码、原生锁和故障恢复由 BSP 负责，未启用时仍创建零能力占位端口。
 * @note owner 负责在 destroy 前结束全部并发调用，本对象不维护活动调用计数。
 */
cc_result_t cc_filesystem_get_default(cc_filesystem_t *out_fs)
{
    if (!out_fs) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid filesystem output");
    memset(out_fs, 0, sizeof(*out_fs));
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

/**
 * @brief 沿兼容命名入口创建当前 FreeRTOS 默认文件系统。
 * @param[out] out_fs 必需槽，入口清零，成功对象由调用方 destroy，失败保持空。
 * @return 与 cc_filesystem_get_default 相同的参数、分配或成功结果。
 */
cc_result_t cc_filesystem_get_posix(cc_filesystem_t *out_fs)
{
    return cc_filesystem_get_default(out_fs);
}
