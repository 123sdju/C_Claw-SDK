



#include "cc/ports/cc_filesystem.h"
#include "cc/internal/cc_alloc.h"

#ifdef _WIN32
#include <windows.h>
#include <winternl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief 无可变配置的 Windows 端口对象，生命周期由创建者管理。 */
typedef struct {
    int dummy; /**< 保证对象非零大小的占位字段。 */
} cc_windows_filesystem_t;

/**
 * @brief 通过二进制 stdio 读取文件末尾的有限字节。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] path 非 NULL 的借用路径，沿 Windows CRT 当前窄字符路径编码解释。
 * @param[out] buffer capacity 字节的可写缓冲，容量为零时允许 NULL，失败可能已修改前缀。
 * @param[in] capacity 容量字节数，不得超过 LONG_MAX，文件长度受 CRT long 定位范围约束。
 * @param[out] out_size 必需槽，入口置零，仅成功读取及关闭后写入字节数。
 * @return 成功返回 CC_OK，参数错误返回 INVALID_ARGUMENT，其余定位、短读或关闭失败返回 IO。
 */
static cc_result_t windows_read_tail(void *self, const char *path, void *buffer,
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
 * @brief 按初始文件长度完整读取二进制内容，不添加末尾 NUL。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] path 借用的非 NULL 窄字符路径。
 * @param[out] out_data 必需空槽，入口置 NULL，成功交付调用方 free 的存储，空文件也分配。
 * @param[out] out_size 必需字节数槽，入口置零，仅完整读取并关闭后交付长度。
 * @return 成功返回 CC_OK，参数错误、大小不可表示、分配及 I/O 失败返回对应错误。
 * @note 并发增长只读取初始长度，并发缩短导致短读失败，所有部分缓冲回收。
 */
static cc_result_t windows_read_bytes(
    void *self, const char *path, void **out_data, size_t *out_size)
{
    (void)self;
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0;
    if (!path || !out_data || !out_size) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid binary read");
    *out_data = NULL;
    *out_size = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file: %s", path);
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return cc_result_error(CC_ERR_IO, "Cannot seek file"); }
    long end = ftell(f);
    if (end < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return cc_result_error(CC_ERR_IO, "Cannot size file"); }
    if ((uintmax_t)end > SIZE_MAX) {
        fclose(f);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "File size cannot be represented");
    }
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
 * @brief 完整读取初始文件内容并补 NUL，保持原字节和内嵌 NUL。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] path 必需的借用窄字符路径，不执行 UTF-8 转码。
 * @param[out] out_text 必需空槽，入口置 NULL，成功分配由调用方 free。
 * @return 成功返回 CC_OK，参数、容量、分配、定位、短读及关闭失败返回对应错误。
 * @note 文本接口不提供原始长度，二进制内容使用 read_bytes。
 */
static cc_result_t windows_read_text(void *self, const char *path, char **out_text)
{
    (void)self;
    if (out_text) *out_text = NULL;
    if (!path || !out_text) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid text read");
    FILE *f = fopen(path, "rb");
    if (!f) return cc_result_errf(CC_ERR_IO, "Cannot open file: %s", path);
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return cc_result_error(CC_ERR_IO, "Cannot seek file");
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return cc_result_error(CC_ERR_IO, "Cannot size or rewind file");
    }
    if ((uintmax_t)size > SIZE_MAX - 1) {
        fclose(f);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Text file size cannot be represented");
    }
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate buffer"); }
    size_t n = size ? fread(buf, 1, (size_t)size, f) : 0;
    int failed = n != (size_t)size || ferror(f);
    if (fclose(f) != 0) failed = 1;
    if (failed) {
        free(buf);
        return cc_result_error(CC_ERR_IO, "Failed to read complete text file");
    }
    buf[n] = '\0';
    *out_text = buf;
    return cc_result_ok();
}

/**
 * @brief 二进制覆盖写入文件，检查完整写入及关闭。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] path 非 NULL 的借用窄字符路径，父目录须存在。
 * @param[in] data 借用 size 字节，零长度时可为 NULL。
 * @param[in] size 写入字节数，零长度仍创建或截断文件。
 * @return 成功返回 CC_OK，参数错误返回 INVALID_ARGUMENT，写入或关闭失败返回 IO。
 * @note 失败可已截断或写入部分内容，不隐含 FlushFileBuffers 或原子替换。
 */
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

/**
 * @brief 将 NUL 结尾文本委托给二进制覆盖写入口。
 * @param[in] self 借用对象，当前实现忽略。
 * @param[in] path 必需的借用窄字符路径。
 * @param[in] text 调用期间借用正文，NULL 按空文本处理。
 * @return 原样返回 write_bytes 的拥有型结果，失败可留下部分文件。
 */
static cc_result_t windows_write_text(void *self, const char *path, const char *text)
{
    const char *safe_text = text ? text : "";
    return windows_write_bytes(self, path, safe_text, strlen(safe_text));
}

/**
 * @brief 以二进制追加模式打开或创建文件并写入全部数据。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] path 借用的非 NULL 窄字符路径。
 * @param[in] data 借用 size 字节，零长度时可为 NULL。
 * @param[in] size 追加字节数，零长度仍打开或创建文件。
 * @return 成功返回 CC_OK，参数错误返回 INVALID_ARGUMENT，短写或关闭失败返回 IO。
 * @note 并发整条记录的串行顺序由上层协调，失败可能已经追加前缀。
 */
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

/**
 * @brief 使用 Win32 属性查询类型、64 位长度及 Unix 毫秒修改时间。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] path 非 NULL 的借用 ANSI 路径。
 * @param[out] out_stat 必需槽，入口清零并设置 size，不存在时 type 保持 MISSING。
 * @return 成功或文件及父路径不存在返回 CC_OK，参数错误返回 INVALID_ARGUMENT，其他错误返回 IO。
 * @note 目录按 DIRECTORY 分类，其他可查询项按 FILE 分类，FILETIME 早于 1970 时记 0。
 */
static cc_result_t windows_stat_path(void *self, const char *path, cc_filesystem_stat_t *out_stat)
{
    (void)self;
    if (out_stat) {
        memset(out_stat, 0, sizeof(*out_stat));
        out_stat->size = sizeof(*out_stat);
    }
    if (!path || !out_stat) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid stat request");
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
    const uint64_t unix_epoch_ticks = UINT64_C(116444736000000000);
    if (ticks.QuadPart > unix_epoch_ticks) {
        out_stat->modified_ms = (ticks.QuadPart - unix_epoch_ticks) / 10000U;
    }
    return cc_result_ok();
}

/**
 * @brief 通过 MoveFileExA 替换安装临时文件并请求写穿。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] temporary_path 必需的借用 ANSI 源路径，成功后源路径被移走。
 * @param[in] destination_path 必需的借用 ANSI 目标路径。
 * @return 成功返回 CC_OK，参数错误返回 INVALID_ARGUMENT，替换失败返回 IO。
 * @note 不启用跨卷复制回退，原子可见性及持久化还取决于原生文件系统，失败由调用者恢复。
 */
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

/**
 * @brief 用写权限重新打开文件并调用 FlushFileBuffers，随后关闭本次句柄。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] path 必需的借用 ANSI 文件路径。
 * @return 刷新及关闭成功返回 CC_OK，空路径返回 INVALID_ARGUMENT，打开或刷新失败返回 IO。
 */
static cc_result_t windows_sync_file(void *self, const char *path)
{
    (void)self;
    if (!path) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null sync path");
    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return cc_result_errf(CC_ERR_IO, "Cannot open file for sync: %s", path);
    int ok = FlushFileBuffers(file) != 0;
    if (!CloseHandle(file)) ok = 0;
    return ok ? cc_result_ok() : cc_result_error(CC_ERR_IO, "File sync failed");
}

/**
 * @brief 表示此端口未提供目录同步操作。
 * @param[in] self 当前不读取，可为 NULL。
 * @param[in] path 当前不读取，可为 NULL。
 * @return 始终返回 CC_ERR_UNSUPPORTED，对应能力位不发布 DIR_SYNC。
 */
static cc_result_t windows_sync_dir(void *self, const char *path)
{
    (void)self;
    (void)path;
    return cc_result_error(CC_ERR_UNSUPPORTED, "Windows directory sync is unavailable");
}

/**
 * @brief 校验根内操作的非空相对路径，兼容两种分隔符。
 * @param[in] path 借用的 NUL 结尾路径，可为 NULL。
 * @return 合法返回 1，绝对路径、盘符起始、空段和点段返回 0。
 * @note 本函数执行词法检查，不解析 junction 或其他 reparse point。
 */
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

/**
 * @brief 动态取得的原生相对句柄创建函数，调用约定与 Windows SDK NtCreateFile 一致。
 * @param[out] file_handle 成功取得调用方 CloseHandle 的句柄。
 * @param[in] desired_access 访问权限位。
 * @param[in] attributes 借用的对象属性，RootDirectory 固定父目录，ObjectName 为单个组件。
 * @param[out] io_status 原生 I/O 状态输出。
 * @param[in] allocation_size 可选预分配大小，本实现传 NULL。
 * @param[in] file_attributes 文件属性位。
 * @param[in] share_access 共享访问位，本实现不共享写和删除。
 * @param[in] disposition 打开或创建规则。
 * @param[in] options 同步、对象类型及不跟随 reparse 的选项。
 * @param[in] ea_buffer 可选扩展属性，本实现传 NULL。
 * @param[in] ea_length 扩展属性字节数，本实现传 0。
 * @return 非负 NTSTATUS 表示成功，负值表示失败。
 */
typedef NTSTATUS (NTAPI *cc_nt_create_file_fn)(PHANDLE file_handle, ACCESS_MASK desired_access,
    POBJECT_ATTRIBUTES attributes, PIO_STATUS_BLOCK io_status, PLARGE_INTEGER allocation_size,
    ULONG file_attributes, ULONG share_access, ULONG disposition, ULONG options,
    PVOID ea_buffer, ULONG ea_length);

/** @brief 一次根相对操作独占的整条目录及目标句柄链。 */
typedef struct windows_handle_chain {
    HANDLE *handles; /**< 独占数组，根、目录和最终文件按打开顺序保存。 */
    size_t count;    /**< 已拥有的有效句柄数。 */
} windows_handle_chain_t;

/**
 * @brief 逆序关闭整条链并回收数组。
 * @param[in,out] chain 已初始化的独占链，清理后归零。
 * @return 全部 CloseHandle 成功返回 1，任一次失败返回 0，仍尝试清理剩余资源。
 */
static int windows_close_chain(windows_handle_chain_t *chain)
{
    int ok = 1;
    while (chain->count) {
        if (!CloseHandle(chain->handles[--chain->count])) ok = 0;
    }
    free(chain->handles);
    chain->handles = NULL;
    return ok;
}

/**
 * @brief 识别 Win32 DOS 设备别名，避免原生组件打开将其新建为普通文件。
 * @param[in] name 借用 UTF-16 组件。
 * @param[in] length 组件单元数，允许包含扩展名或流名。
 * @return 属于保留设备别名返回 1，其余返回 0。
 * @note 原有 CreateFileA 设备句柄无法通过根内最终路径核验，这里保留其拒绝结果。
 */
static int windows_reserved_component(const WCHAR *name, size_t length)
{
    size_t stem_length = 0;
    while (stem_length < length && name[stem_length] != '.' && name[stem_length] != ':') stem_length++;
    if (stem_length > 7) return 0;
    char stem[8] = {0};
    for (size_t i = 0; i < stem_length; i++) {
        WCHAR ch = name[i];
        if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
        stem[i] = ch <= 0x7f ? (char)ch : 0;
    }
    if ((stem_length == 3 && (!memcmp(stem, "CON", 3) || !memcmp(stem, "PRN", 3) ||
         !memcmp(stem, "AUX", 3) || !memcmp(stem, "NUL", 3))) ||
        (stem_length == 6 && !memcmp(stem, "CONIN$", 6)) ||
        (stem_length == 7 && !memcmp(stem, "CONOUT$", 7))) return 1;
    return stem_length == 4 && (!memcmp(stem, "COM", 3) || !memcmp(stem, "LPT", 3)) &&
        ((name[3] >= '1' && name[3] <= '9') || name[3] == 0x00b9 || name[3] == 0x00b2 || name[3] == 0x00b3);
}

/**
 * @brief 通过固定父目录打开单个组件，禁止 reparse 并核验最终对象类型。
 * @param[in] create_file 借用的 ntdll 函数入口。
 * @param[in] parent 借用且在操作期间保持打开的父目录句柄。
 * @param[in] name 借用的 UTF-16 单个组件，不含路径分隔符。
 * @param[in] length 组件 UTF-16 单元数，须能装入 UNICODE_STRING 的字节长度。
 * @param[in] directory 非零要求已存在目录，零要求普通非目录文件。
 * @param[in] writing 非零允许创建最终文件，目录组件始终只打开。
 * @param[out] out_handle 必需空槽，入口置 INVALID_HANDLE_VALUE，成功后调用方拥有句柄。
 * @return 成功返回 CC_OK，容量、原生资源或路径约束失败返回对应错误，失败关闭本次句柄。
 */
static cc_result_t windows_open_component(cc_nt_create_file_fn create_file, HANDLE parent,
    WCHAR *name, size_t length, int directory, int writing, HANDLE *out_handle)
{
    *out_handle = INVALID_HANDLE_VALUE;
    if (!length || length > USHRT_MAX / sizeof(WCHAR)) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Workspace component is too long");
    }
    UNICODE_STRING component;
    component.Buffer = name;
    component.Length = (USHORT)(length * sizeof(WCHAR));
    component.MaximumLength = component.Length;
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &component,
        OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE, parent, NULL);
    IO_STATUS_BLOCK io_status;
    HANDLE file = INVALID_HANDLE_VALUE;
    ACCESS_MASK access = directory ? FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE
        : GENERIC_READ | SYNCHRONIZE | (writing ? GENERIC_WRITE : 0);
    ULONG options = FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT |
        (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE);
    NTSTATUS status = create_file(&file, access, &attributes, &io_status, NULL,
        FILE_ATTRIBUTE_NORMAL, writing && !directory ? 0 : FILE_SHARE_READ,
        writing && !directory ? FILE_OPEN_IF : FILE_OPEN, options, NULL, 0);
    if (status < 0) {
        if (file != INVALID_HANDLE_VALUE && file != NULL) CloseHandle(file);
        cc_error_code_t code = status == (NTSTATUS)0xC0000017L || status == (NTSTATUS)0xC000009AL
            ? CC_ERR_OUT_OF_MEMORY : CC_ERR_PERMISSION_DENIED;
        return cc_result_error(code, "Cannot open workspace component");
    }
    if (file == INVALID_HANDLE_VALUE || file == NULL) {
        return cc_result_error(CC_ERR_IO, "Relative open returned no file handle");
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        (!!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != !!directory)) {
        CloseHandle(file);
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "Invalid workspace component");
    }
    *out_handle = file;
    return cc_result_ok();
}

/**
 * @brief 固定根和逐级目录，在整条链无 reparse 的前提下打开或创建最终文件。
 * @param[in] root 调用期借用的 ANSI 根路径，其最终组件必须是非 reparse 目录。
 * @param[in] relative_path 借用相对路径，按 ACP 转 UTF-16，普通 Win32 根逐组件折叠尾点空格后校验，显式设备命名空间根保留字面组件。
 * @param[in] writing 非零允许在固定的最终父目录内创建文件，零仅打开已存在文件。
 * @param[out] out_chain 必需空链，入口归零，成功拥有根、全部父目录及最后文件句柄。
 * @return 成功返回 CC_OK，参数路径、转换、容量、分配或原生操作失败返回对应错误，失败回收整链。
 * @note 各目录禁止共享写和删除且存活到实际 I/O 结束，后续访问使用父句柄，不重新解析根字符串。
 */
static cc_result_t windows_open_relative(const char *root, const char *relative_path,
    int writing, windows_handle_chain_t *out_chain)
{
    memset(out_chain, 0, sizeof(*out_chain));
    if (!root || !windows_relative_valid(relative_path)) {
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "Unsafe workspace path");
    }
    int normalize_dos_names = strncmp(root, "\\\\?\\", 4) != 0 && strncmp(root, "\\\\.\\", 4) != 0;
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    cc_nt_create_file_fn create_file = module
        ? (cc_nt_create_file_fn)GetProcAddress(module, "NtCreateFile") : NULL;
    if (!create_file) return cc_result_error(CC_ERR_PLATFORM, "Relative file API unavailable");
    int wide_count = MultiByteToWideChar(CP_ACP, 0, relative_path, -1, NULL, 0);
    if (wide_count <= 0) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Cannot convert workspace path");
    if ((size_t)wide_count > SIZE_MAX / sizeof(WCHAR)) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Workspace path is too long");
    }
    WCHAR *wide = malloc((size_t)wide_count * sizeof(*wide));
    if (!wide) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate workspace path");
    if (MultiByteToWideChar(CP_ACP, 0, relative_path, -1, wide, wide_count) != wide_count) {
        free(wide);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Cannot convert workspace path");
    }
    size_t capacity = 2;
    for (WCHAR *p = wide; *p; p++) if (*p == '/' || *p == '\\') capacity++;
    if (capacity > SIZE_MAX / sizeof(HANDLE)) {
        free(wide);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Too many workspace components");
    }
    out_chain->handles = malloc(capacity * sizeof(*out_chain->handles));
    if (!out_chain->handles) {
        free(wide);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate workspace handles");
    }
    HANDLE root_handle = CreateFileA(root, FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE,
        FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    cc_result_t rc = cc_result_ok();
    if (root_handle == INVALID_HANDLE_VALUE) {
        rc = cc_result_error(CC_ERR_PERMISSION_DENIED, "Cannot open workspace root");
        goto fail;
    }
    out_chain->handles[out_chain->count++] = root_handle;
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(root_handle, &info) ||
        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        rc = cc_result_error(CC_ERR_PERMISSION_DENIED, "Invalid workspace root");
        goto fail;
    }
    WCHAR *part = wide;
    for (;;) {
        WCHAR *end = part;
        while (*end && *end != '/' && *end != '\\') end++;
        int last = *end == 0;
        size_t length = (size_t)(end - part);
        if (normalize_dos_names) {
            while (length && (part[length - 1] == '.' || part[length - 1] == ' ')) length--;
        }
        if (!length || (length == 1 && part[0] == '.') ||
            (length == 2 && part[0] == '.' && part[1] == '.') ||
            (normalize_dos_names && windows_reserved_component(part, length))) {
            rc = cc_result_error(CC_ERR_PERMISSION_DENIED, "Invalid normalized workspace component");
            goto fail;
        }
        part[length] = 0;
        HANDLE child;
        rc = windows_open_component(create_file, out_chain->handles[out_chain->count - 1],
            part, length, !last, writing && last, &child);
        if (rc.code != CC_OK) goto fail;
        cc_result_free(&rc);
        out_chain->handles[out_chain->count++] = child;
        if (last) break;
        part = end + 1;
    }
    free(wide);
    return cc_result_ok();
fail:
    free(wide);
    windows_close_chain(out_chain);
    return rc;
}

/**
 * @brief 逐级固定无 reparse 的目录和最终文件，按 64 位长度读取并补 NUL。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] root 必需的借用 ANSI 根路径。
 * @param[in] relative_path 借用合法相对路径，两种分隔符均可，尾点空格按 Win32 组件规则折叠。
 * @param[out] out_text 必需空槽，入口置 NULL，成功后调用方 free。
 * @return 成功返回 CC_OK，非法或越界路径返回 PERMISSION_DENIED，容量、分配及读取失败返回对应错误。
 * @note 内嵌 NUL 保留，整链句柄在读取和关闭期间保活，不重新解析根字符串。
 */
static cc_result_t windows_read_relative_text(
    void *self, const char *root, const char *relative_path, char **out_text)
{
    (void)self;
    if (out_text) *out_text = NULL;
    if (!out_text) {
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "Unsafe workspace path");
    }
    windows_handle_chain_t chain;
    cc_result_t rc = windows_open_relative(root, relative_path, 0, &chain);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    HANDLE file = chain.handles[chain.count - 1];
    LARGE_INTEGER length;
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 0 || (uint64_t)length.QuadPart > SIZE_MAX - 1) {
        windows_close_chain(&chain);
        return cc_result_error(CC_ERR_IO, "Cannot size workspace file");
    }
    size_t size = (size_t)length.QuadPart;
    char *text = malloc(size + 1);
    if (!text) {
        windows_close_chain(&chain);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate file text");
    }
    size_t offset = 0;
    while (offset < size) {
        DWORD chunk = (DWORD)((size - offset) > 0x7fffffffU ? 0x7fffffffU : size - offset);
        DWORD got = 0;
        if (!ReadFile(file, text + offset, chunk, &got, NULL) || got == 0) {
            free(text);
            windows_close_chain(&chain);
            return cc_result_error(CC_ERR_IO, "Failed to read workspace file");
        }
        offset += got;
    }
    text[size] = '\0';
    if (!windows_close_chain(&chain)) {
        free(text);
        return cc_result_error(CC_ERR_IO, "Failed to close workspace file");
    }
    *out_text = text;
    return cc_result_ok();
}

/**
 * @brief 固定无 reparse 的完整目录链后，创建或截断最终文件并完整写入二进制内容。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] root 非 NULL 的借用 ANSI 根路径。
 * @param[in] relative_path 借用的合法相对路径，父目录须存在。
 * @param[in] data 调用期间借用 size 字节，零长度时可为 NULL。
 * @param[in] size 正文字节数，零长度仍创建或截断文件。
 * @return 成功返回 CC_OK，无效路径或数据返回 PERMISSION_DENIED，分配及 I/O 失败返回对应错误。
 * @note 所有祖先组件先完成验证，最终创建相对固定父句柄进行，失败仅可能留下根内部分结果。
 */
static cc_result_t windows_write_relative_bytes(
    void *self, const char *root, const char *relative_path, const void *data, size_t size)
{
    (void)self;
    if (size > 0 && !data) {
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "Null workspace binary data");
    }
    windows_handle_chain_t chain;
    cc_result_t rc = windows_open_relative(root, relative_path, 1, &chain);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    HANDLE file = chain.handles[chain.count - 1];
    if (SetFilePointer(file, 0, NULL, FILE_BEGIN) != 0) {
        windows_close_chain(&chain);
        return cc_result_error(CC_ERR_IO, "Failed to rewind workspace file");
    }
    if (!SetEndOfFile(file)) {
        windows_close_chain(&chain);
        return cc_result_error(CC_ERR_IO, "Failed to truncate workspace file");
    }
    size_t offset = 0;
    while (offset < size) {
        DWORD chunk = (DWORD)((size - offset) > 0x7fffffffU ? 0x7fffffffU : size - offset);
        DWORD written = 0;
        if (!WriteFile(file, (const unsigned char *)data + offset, chunk, &written, NULL) || written == 0) {
            windows_close_chain(&chain);
            return cc_result_error(CC_ERR_IO, "Failed to write workspace file");
        }
        offset += written;
    }
    int ok = FlushFileBuffers(file) != 0;
    if (!windows_close_chain(&chain)) ok = 0;
    return ok ? cc_result_ok() : cc_result_error(CC_ERR_IO, "Workspace file sync failed");
}

/**
 * @brief 将文本转为字节借用后复用固定目录链的二进制写入口。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] root 借用的非 NULL ANSI 根路径。
 * @param[in] relative_path 借用的合法相对路径，父目录须存在。
 * @param[in] text 借用的 NUL 结尾正文，NULL 表示空文本。
 * @return 原样交付根相对二进制写结果，失败可能留下根内新建文件或部分内容。
 */
static cc_result_t windows_write_relative_text(
    void *self, const char *root, const char *relative_path, const char *text)
{
    const char *data = text ? text : "";
    return windows_write_relative_bytes(self, root, relative_path, data, strlen(data));
}

/**
 * @brief 使用 ANSI 属性查询路径存在性，原生查询错误按不存在表示。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] path 必需的借用路径。
 * @param[out] out_exists 必需槽，入口置零，查询成功写 1。
 * @return 有效参数返回 CC_OK，空参数返回 INVALID_ARGUMENT。
 */
static cc_result_t windows_exists(void *self, const char *path, int *out_exists)
{
    (void)self;
    if (out_exists) *out_exists = 0;
    if (!path || !out_exists) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid exists request");
    DWORD attr = GetFileAttributesA(path);
    *out_exists = (attr != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
    return cc_result_ok();
}

/**
 * @brief 使用 ANSI Find API 枚举目录名称，忽略点和父目录项，不作排序。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] path 非 NULL 的借用目录路径，追加通配符后须容纳于 MAX_PATH。
 * @param[out] out_items 必需空槽，入口置 NULL，成功数组及每个名称均由调用方 free。
 * @param[out] out_count 必需槽，入口置零，成功交付名称数。
 * @return 成功返回 CC_OK，参数、容量、分配及原生枚举或关闭失败返回对应错误。
 * @note 并发目录变化时只交付枚举期间观察到的集合，失败回收整个部分列表。
 */
static cc_result_t windows_list_dir(void *self, const char *path, char ***out_items, size_t *out_count)
{
    (void)self;
    if (out_items) *out_items = NULL;
    if (out_count) *out_count = 0;
    if (!path || !out_items || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid directory list request");
    *out_items = NULL;
    *out_count = 0;
    char search_path[MAX_PATH];
    int written = snprintf(search_path, sizeof(search_path), "%s\\*", path);
    if (written < 0 || (size_t)written >= sizeof(search_path))
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Directory path is too long");

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
            if (cap > SIZE_MAX / 2 / sizeof(*items)) {
                for (size_t i = 0; i < count; i++) free(items[i]);
                free(items);
                FindClose(hFind);
                return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Too many directory entries");
            }
            size_t next_cap = cap * 2;
            char **next = realloc(items, next_cap * sizeof(char *));
            if (!next) {
                for (size_t i = 0; i < count; i++) free(items[i]);
                free(items);
                FindClose(hFind);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow items");
            }
            cap = next_cap;
            items = next;
        }
        items[count] = cc_copy_string(fd.cFileName);
        if (!items[count]) {
            for (size_t i = 0; i < count; i++) free(items[i]);
            free(items);
            FindClose(hFind);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy directory entry");
        }
        count++;
    } while (FindNextFileA(hFind, &fd));

    DWORD read_error = GetLastError();
    BOOL closed = FindClose(hFind);
    if (read_error != ERROR_NO_MORE_FILES || !closed) {
        for (size_t i = 0; i < count; i++) free(items[i]);
        free(items);
        return cc_result_errf(CC_ERR_IO, "Cannot read or close directory: %s", path);
    }
    *out_items = items;
    *out_count = count;
    return cc_result_ok();
}

/**
 * @brief 按两种分隔符逐级创建目录，已存在对象须实际具有目录属性。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] path 借用非空 ANSI 路径。
 * @return 成功返回 CC_OK，空路径返回 INVALID_ARGUMENT，复制失败返回 OOM，创建或类型冲突返回 IO。
 * @note 失败保留此前已创建目录，绝对盘路径和 UNC 前缀仍按当前逐段创建规则处理。
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
            DWORD attributes = err == ERROR_ALREADY_EXISTS ? GetFileAttributesA(tmp) : INVALID_FILE_ATTRIBUTES;
            if (err != ERROR_ALREADY_EXISTS || attributes == INVALID_FILE_ATTRIBUTES ||
                !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                cc_result_t rc = cc_result_errf(CC_ERR_IO, "Cannot create directory: %s", tmp);
                free(tmp);
                return rc;
            }
        }
        *p = saved;
    }
    if (!CreateDirectoryA(tmp, NULL)) {
        DWORD err = GetLastError();
        DWORD attributes = err == ERROR_ALREADY_EXISTS ? GetFileAttributesA(tmp) : INVALID_FILE_ATTRIBUTES;
        if (err != ERROR_ALREADY_EXISTS || attributes == INVALID_FILE_ATTRIBUTES ||
            !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            cc_result_t rc = cc_result_errf(CC_ERR_IO, "Cannot create directory: %s", path);
            free(tmp);
            return rc;
        }
    }
    free(tmp);
    return cc_result_ok();
}

/**
 * @brief 通过 DeleteFileA 删除普通文件路径，原生目录删除语义不在本入口内。
 * @param[in] self 当前忽略，可为 NULL。
 * @param[in] path 必需的借用 ANSI 路径。
 * @return 成功返回 CC_OK，空路径返回 INVALID_ARGUMENT，原生删除失败返回 IO。
 */
static cc_result_t windows_remove(void *self, const char *path)
{
    (void)self;
    if (!path) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null remove path");
    if (!DeleteFileA(path))
        return cc_result_errf(CC_ERR_IO, "Cannot remove: %s", path);
    return cc_result_ok();
}

/**
 * @brief 释放创建函数拥有的私有端口对象。
 * @param[in] self 独占的对象，可为 NULL。
 * @pre 拥有者排空所有调用并结束所有浅拷贝借用。
 * @post self 失效，外层接口槽由调用方清空。
 */
static void windows_destroy(void *self)
{
    free(self);
}

/** Windows 文件系统静态函数表，根相对入口提供逐组件无 reparse 访问。 */
static cc_filesystem_vtable_t windows_vtable = {
    .size = sizeof(cc_filesystem_vtable_t),
    .version = CC_FILESYSTEM_VTABLE_VERSION,
    .capabilities = CC_FS_CAP_BINARY | CC_FS_CAP_APPEND | CC_FS_CAP_STAT |
        CC_FS_CAP_ATOMIC_REPLACE | CC_FS_CAP_FILE_SYNC | CC_FS_CAP_ROOT_SCOPED | CC_FS_CAP_NO_SYMLINKS |
        CC_FS_CAP_READ_TAIL,
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
    .read_tail = windows_read_tail,
};

/**
 * @brief 通过兼容命名入口创建 Windows 文件系统对象。
 * @param[out] out_fs 必需空槽，入口清零，成功拥有 self，静态 vtable 借用至进程退出。
 * @return 成功返回 CC_OK，空输出返回 INVALID_ARGUMENT，分配失败返回 OOM 且输出保持空。
 * @note 无可变配置，各调用独占其 I/O 资源，同一目标上的事务和并发写由上层协调。
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

/**
 * @brief 创建 Windows profile 默认文件系统对象。
 * @param[out] out_fs 必需空接口槽，成功拥有 self，失败清零。
 * @return 原样返回兼容创建入口的拥有型结果。
 */
cc_result_t cc_filesystem_get_default(cc_filesystem_t *out_fs)
{
    return cc_filesystem_get_posix(out_fs);
}

#endif
