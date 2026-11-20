#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_path.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include "filesystem_stubs/windows.h"
#include "filesystem_stubs/winternl.h"

/* 私有包含真实 Windows 生产实现。Linux 模型区分名称空间与已打开对象，
 * 以可控交错检查句柄相对访问，不能替代原生 NTFS、Windows ABI 或 ACP 环境验证。 */
typedef struct model_node {
    char name[64];               /**< 单组件名称，根节点另保存显示名称。 */
    struct model_node *parent;   /**< 模型中的真实父对象。 */
    struct model_node *redirect; /**< reparse 指向的对象，可为空。 */
    DWORD attributes;           /**< 文件、目录及 reparse 属性。 */
    char bytes[256];            /**< 模型文件正文。 */
    size_t size;                /**< 有效正文字节数。 */
} model_node_t;
typedef struct model_handle {
    model_node_t *node; /**< 名称重新绑定后仍固定的对象。 */
    DWORD access;      /**< 打开访问位。 */
    DWORD share;       /**< 共享模式。 */
    size_t offset;     /**< 当前文件位置。 */
    int live;          /**< 句柄是否仍由生产代码拥有。 */
} model_handle_t;

static model_node_t g_nodes[32];
static size_t g_nodes_used;
static model_handle_t g_handles[32];
static size_t g_live_handles;
static model_node_t *g_named_root;
static model_node_t *g_root;
static model_node_t *g_outside;
static model_node_t *g_safe;
static model_node_t *g_link;
static model_node_t *g_outside_file;
static DWORD g_last_error;
static int g_outside_creates;
static int g_outside_writes;
static int g_race;
static int g_race_blocked;
static int g_rebind_root;
static int g_no_native_api;
static int g_bad_native_handle;
static int g_seek_fail;
static int g_truncate_fail;
static int g_write_fail;
static int g_flush_fail;
static int g_close_fail;
static int g_stdio_seek_fail;
static int g_stdio_tell_fail;
static int g_stdio_short;
static int g_stdio_close_fail;
static int g_stdio_open;
static uint64_t g_filetime;
static const char *g_full_path;
static int g_full_path_fail;
static int g_alloc_calls;
static int g_fail_alloc;
static void *g_allocations[128];
static size_t g_live_allocations;

/**
 * @brief 新建固定地址的模型节点，测试结束由整体重置回收。
 * @param[in] parent 借用父节点，根可为 NULL。
 * @param[in] name 借用的短名称，必须容纳于节点名称字段。
 * @param[in] attributes 节点属性位。
 * @return 指向模型静态存储的新节点。
 */
static model_node_t *add_node(model_node_t *parent, const char *name, DWORD attributes)
{
    assert(g_nodes_used < sizeof(g_nodes) / sizeof(g_nodes[0]));
    model_node_t *node = &g_nodes[g_nodes_used++];
    memset(node, 0, sizeof(*node));
    assert(strlen(name) < sizeof(node->name));
    strcpy(node->name, name);
    node->parent = parent;
    node->attributes = attributes;
    return node;
}

/**
 * @brief 查询固定父节点下的名称，模拟默认大小写不敏感匹配。
 * @param[in] parent 借用父对象。
 * @param[in] name 借用单组件名称。
 * @return 已有节点或 NULL，不修改模型。
 */
static model_node_t *find_child(model_node_t *parent, const char *name)
{
    for (size_t i = 0; i < g_nodes_used; i++) {
        if (g_nodes[i].parent == parent && !strcasecmp(g_nodes[i].name, name)) return &g_nodes[i];
    }
    return NULL;
}

/**
 * @brief 判定对象是否位于外部目录，用于统计根外副作用。
 * @param[in] node 借用节点，可为 NULL。
 * @return 节点本身或祖先为外部目录时返回 1，否则返回 0。
 */
static int is_outside(model_node_t *node)
{
    for (; node; node = node->parent) if (node == g_outside) return 1;
    return 0;
}

/**
 * @brief 模拟会改变目录对象的操作受到已有句柄共享规则阻止。
 * @param[in] node 借用待修改节点。
 * @return 有句柄禁止共享写或删除时返回 1，其余返回 0。
 */
static int node_is_fixed(model_node_t *node)
{
    for (size_t i = 0; i < sizeof(g_handles) / sizeof(g_handles[0]); i++) {
        if (g_handles[i].live && g_handles[i].node == node &&
            (g_handles[i].share & (FILE_SHARE_WRITE | FILE_SHARE_DELETE)) !=
                (FILE_SHARE_WRITE | FILE_SHARE_DELETE)) return 1;
    }
    return 0;
}

/**
 * @brief 为模型对象建立独占句柄记录。
 * @param[in] node 借用的存活节点。
 * @param[in] access 记录的访问权限位。
 * @param[in] share 记录的共享位。
 * @return 新句柄，固定测试池耗尽时由断言终止。
 */
static HANDLE open_node(model_node_t *node, DWORD access, DWORD share)
{
    assert(node);
    for (size_t i = 0; i < sizeof(g_handles) / sizeof(g_handles[0]); i++) {
        if (!g_handles[i].live) {
            g_handles[i] = (model_handle_t){node, access, share, 0, 1};
            g_live_handles++;
            return &g_handles[i];
        }
    }
    assert(0);
    return INVALID_HANDLE_VALUE;
}

/**
 * @brief 解析旧 Win32 全路径，刻意支持中间 reparse，以重现先创建后拒绝的旧行为。
 * @param[in] path 借用 ANSI 测试路径。
 * @param[in] create 非零允许新建最终文件。
 * @param[in] no_follow 非零不跟随最终组件，祖先仍按 Win32 规则解析。
 * @return 节点或 NULL，新建外部节点时增加根外创建计数。
 */
static model_node_t *resolve_path(const char *path, int create, int no_follow)
{
    if (!strncmp(path, "\\\\?\\", 4) || !strncmp(path, "\\\\.\\", 4)) path += 4;
    model_node_t *node;
    const char *tail;
    if (!strncasecmp(path, "C:\\root", 7)) { node = g_named_root; tail = path + 7; }
    else if (!strncasecmp(path, "C:\\outside", 10)) { node = g_outside; tail = path + 10; }
    else return NULL;
    while (*tail == '\\' || *tail == '/') tail++;
    if (!*tail) return !no_follow && node->redirect ? node->redirect : node;
    char parts[256];
    assert(strlen(tail) < sizeof(parts));
    strcpy(parts, tail);
    char *save = NULL;
    char *part = strtok_r(parts, "\\/", &save);
    while (part) {
        char *next = strtok_r(NULL, "\\/", &save);
        model_node_t *child = find_child(node, part);
        if (!child && create && !next) {
            child = add_node(node, part, FILE_ATTRIBUTE_NORMAL);
            if (is_outside(child)) g_outside_creates++;
        }
        if (!child) return NULL;
        node = child->redirect && (next || !no_follow) ? child->redirect : child;
        part = next;
    }
    return node;
}

/**
 * @brief 返回模型最后一个 Win32 错误码。
 * @return 当前模型错误，不交付资源。
 */
DWORD GetLastError(void)
{
    return g_last_error;
}

/**
 * @brief 模拟 ANSI 打开，记录 root 的不跟随和共享规则，支持旧全路径创建重现。
 * @param[in] path 借用测试路径。
 * @param[in] access 期望权限。
 * @param[in] share 共享模式。
 * @param[in] security 未使用的安全属性，可为 NULL。
 * @param[in] disposition OPEN_EXISTING 或 OPEN_ALWAYS。
 * @param[in] flags 属性及打开标记。
 * @param[in] template_file 未使用的模板句柄。
 * @return 新模型句柄或 INVALID_HANDLE_VALUE。
 */
HANDLE CreateFileA(const char *path, DWORD access, DWORD share, void *security,
    DWORD disposition, DWORD flags, HANDLE template_file)
{
    (void)security;
    (void)template_file;
    if (g_race == 2 && strstr(path, "safe\\")) {
        g_safe->attributes |= FILE_ATTRIBUTE_REPARSE_POINT;
        g_safe->redirect = g_outside;
    }
    model_node_t *node = resolve_path(path, disposition == OPEN_ALWAYS,
        (flags & FILE_FLAG_OPEN_REPARSE_POINT) != 0);
    if (!node) { g_last_error = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE_VALUE; }
    HANDLE result = open_node(node, access, share);
    if (g_rebind_root && node == g_root) { g_named_root = g_outside; g_rebind_root = 0; }
    return result;
}

/**
 * @brief 结束模型句柄拥有权，关闭错误注入仍回收模型资源。
 * @param[in] handle 由模型交付的存活句柄。
 * @return 正常返回 TRUE，注入关闭错误时返回 FALSE。
 */
BOOL CloseHandle(HANDLE handle)
{
    model_handle_t *value = handle;
    assert(value && value != INVALID_HANDLE_VALUE && value->live);
    value->live = 0;
    g_live_handles--;
    if (g_close_fail) { g_close_fail = 0; return FALSE; }
    return TRUE;
}

/**
 * @brief 获取固定句柄的属性，不重新解析名称。
 * @param[in] handle 借用存活句柄。
 * @param[out] info 必需属性输出。
 * @return 模型始终返回 TRUE。
 */
BOOL GetFileInformationByHandle(HANDLE handle, BY_HANDLE_FILE_INFORMATION *info)
{
    model_handle_t *value = handle;
    assert(value->live);
    info->dwFileAttributes = value->node->attributes;
    return TRUE;
}

/**
 * @brief 以节点真实父链返回最终路径，供旧生产实现的负向对照使用。
 * @param[in] handle 借用存活句柄。
 * @param[out] buffer 输出路径缓冲。
 * @param[in] capacity 缓冲字节数。
 * @param[in] flags 名称选项，模型忽略。
 * @return 返回所需或已写长度，遵循缓冲不足返回需求的约定。
 */
DWORD GetFinalPathNameByHandleA(HANDLE handle, char *buffer, DWORD capacity, DWORD flags)
{
    (void)flags;
    model_handle_t *value = handle;
    assert(value->live);
    const char *path = is_outside(value->node) ? "C:\\outside\\file" : "C:\\root\\file";
    if (value->node == g_root) path = "C:\\root";
    if (value->node == g_outside) path = "C:\\outside";
    size_t length = strlen(path);
    if (capacity <= length) return (DWORD)length + 1;
    memcpy(buffer, path, length + 1);
    return (DWORD)length;
}

/**
 * @brief 将模型的 ASCII ACP 输入编码为 16 位单元，覆盖生产转换和容量交接。
 * @param[in] page 必须为 CP_ACP。
 * @param[in] flags 转换标记，模型忽略。
 * @param[in] source 借用 NUL 结尾 ASCII 输入。
 * @param[in] source_size 必须为 -1。
 * @param[out] destination 可选 UTF-16 目标。
 * @param[in] capacity 目标单元数，查询时为 0。
 * @return 返回包含 NUL 的需求单元数，容量不足返回 0。
 */
int MultiByteToWideChar(unsigned page, DWORD flags, const char *source, int source_size,
    WCHAR *destination, int capacity)
{
    (void)flags;
    assert(page == CP_ACP && source_size == -1);
    int length = (int)strlen(source) + 1;
    if (!destination) return length;
    if (capacity < length) return 0;
    for (int i = 0; i < length; i++) destination[i] = (unsigned char)source[i];
    return length;
}

/**
 * @brief 模拟用户态 NtCreateFile，真正根据父句柄而非全路径寻找目标。
 * @param[out] out_handle 成功交付模型句柄。
 * @param[in] access 访问位。
 * @param[in] attributes 借用的单组件对象属性。
 * @param[out] status 交付原生状态。
 * @param[in] allocation 未使用的预分配参数。
 * @param[in] file_attributes 新文件属性。
 * @param[in] share 共享模式。
 * @param[in] disposition 打开或创建方式。
 * @param[in] options 对象类型、同步和 reparse 打开方式。
 * @param[in] ea 未使用扩展属性。
 * @param[in] ea_size 未使用扩展属性长度。
 * @return 成功返回 0，reparse 或不存在返回负 NTSTATUS。
 */
static NTSTATUS NTAPI model_nt_create_file(PHANDLE out_handle, ACCESS_MASK access,
    POBJECT_ATTRIBUTES attributes, PIO_STATUS_BLOCK status, PLARGE_INTEGER allocation,
    ULONG file_attributes, ULONG share, ULONG disposition, ULONG options, PVOID ea, ULONG ea_size)
{
    (void)allocation;
    (void)ea;
    (void)ea_size;
    model_handle_t *parent = attributes->RootDirectory;
    assert(parent && parent->live && (parent->node->attributes & FILE_ATTRIBUTE_DIRECTORY));
    assert((parent->share & (FILE_SHARE_WRITE | FILE_SHARE_DELETE)) == 0);
    assert(attributes->Attributes & OBJ_DONT_REPARSE);
    assert(options & FILE_OPEN_REPARSE_POINT);
    assert(attributes->ObjectName->Length % sizeof(WCHAR) == 0);
    size_t length = attributes->ObjectName->Length / sizeof(WCHAR);
    char name[64];
    assert(length && length < sizeof(name));
    for (size_t i = 0; i < length; i++) {
        WCHAR ch = attributes->ObjectName->Buffer[i];
        assert(ch < 128 && ch != '/' && ch != '\\');
        name[i] = (char)ch;
    }
    name[length] = '\0';
    if (g_race == 1 && !strcmp(name, "safe")) {
        g_safe->attributes |= FILE_ATTRIBUTE_REPARSE_POINT;
        g_safe->redirect = g_outside;
        g_race = 0;
    }
    if (g_race == 2 && parent->node == g_safe) {
        assert(node_is_fixed(g_root));
        if (node_is_fixed(g_safe)) g_race_blocked++;
        else { g_safe->attributes |= FILE_ATTRIBUTE_REPARSE_POINT; g_safe->redirect = g_outside; }
        g_race = 0;
    }
    model_node_t *node = find_child(parent->node, name);
    if (node && (node->attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return (NTSTATUS)0xC000050BL;
    if (!node && disposition == FILE_OPEN_IF) {
        assert(!(options & FILE_DIRECTORY_FILE));
        node = add_node(parent->node, name, file_attributes);
        if (is_outside(node)) g_outside_creates++;
    }
    if (!node) return (NTSTATUS)0xC0000034L;
    assert(!(share & (FILE_SHARE_WRITE | FILE_SHARE_DELETE)));
    *out_handle = g_bad_native_handle ? INVALID_HANDLE_VALUE : open_node(node, access, share);
    status->Status = 0;
    return 0;
}

/**
 * @brief 返回进程期 ntdll 模块的模型借用。
 * @param[in] name 借用宿主宽字符模块常量，模型不解释其 ABI。
 * @return 进程期占位模块句柄。
 */
HMODULE GetModuleHandleW(const wchar_t *name)
{
    assert(name);
    return (HMODULE)(uintptr_t)1;
}

/**
 * @brief 返回模型原生入口，或注入入口不可用。
 * @param[in] module 借用模块句柄。
 * @param[in] name 必须为 NtCreateFile。
 * @return 通用函数地址，注入不可用时返回 NULL。
 */
FARPROC GetProcAddress(HMODULE module, const char *name)
{
    assert(module && !strcmp(name, "NtCreateFile"));
    return g_no_native_api ? NULL : (FARPROC)model_nt_create_file;
}

/**
 * @brief 查询模型文件长度。
 * @param[in] handle 借用存活句柄。
 * @param[out] size 必需长度槽。
 * @return 模型返回 TRUE。
 */
BOOL GetFileSizeEx(HANDLE handle, LARGE_INTEGER *size)
{
    model_handle_t *value = handle;
    assert(value->live);
    size->QuadPart = (int64_t)value->node->size;
    return TRUE;
}

/**
 * @brief 从固定对象读取二进制数据。
 * @param[in,out] handle 借用存活文件句柄，更新位置。
 * @param[out] data 可写 capacity 字节缓冲。
 * @param[in] capacity 读取容量字节数。
 * @param[out] got 实际读取长度。
 * @param[in] overlap 未使用的异步参数。
 * @return 模型返回 TRUE，EOF 通过 got 为零表示。
 */
BOOL ReadFile(HANDLE handle, void *data, DWORD capacity, DWORD *got, void *overlap)
{
    (void)overlap;
    model_handle_t *value = handle;
    assert(value->live);
    size_t available = value->node->size - value->offset;
    *got = (DWORD)(available < capacity ? available : capacity);
    memcpy(data, value->node->bytes + value->offset, *got);
    value->offset += *got;
    return TRUE;
}

/**
 * @brief 向固定对象写入数据，统计任何根外修改。
 * @param[in,out] handle 借用存活文件句柄。
 * @param[in] data 借用 size 字节。
 * @param[in] size 写入字节数，模型正文容量须充足。
 * @param[out] written 实际写入字节数，注入失败时置零。
 * @param[in] overlap 未使用异步参数。
 * @return 注入时 FALSE，其余 TRUE。
 */
BOOL WriteFile(HANDLE handle, const void *data, DWORD size, DWORD *written, void *overlap)
{
    (void)overlap;
    model_handle_t *value = handle;
    assert(value->live && (value->access & GENERIC_WRITE));
    *written = 0;
    if (g_write_fail) { g_write_fail = 0; return FALSE; }
    assert(value->offset + size <= sizeof(value->node->bytes));
    memcpy(value->node->bytes + value->offset, data, size);
    value->offset += size;
    if (value->offset > value->node->size) value->node->size = value->offset;
    if (is_outside(value->node)) g_outside_writes++;
    *written = size;
    return TRUE;
}

/**
 * @brief 定位模型文件，或注入定位错误。
 * @param[in,out] handle 借用存活句柄。
 * @param[in] offset 目标字节偏移，模型要求为零。
 * @param[in,out] high 未使用的高位参数。
 * @param[in] origin 必须为 FILE_BEGIN。
 * @return 成功返回 0，注入失败返回 UINT32_MAX。
 */
DWORD SetFilePointer(HANDLE handle, LONG offset, LONG *high, DWORD origin)
{
    (void)high;
    model_handle_t *value = handle;
    assert(value->live && offset == 0 && origin == FILE_BEGIN);
    if (g_seek_fail) { g_seek_fail = 0; return UINT32_MAX; }
    value->offset = 0;
    return 0;
}

/**
 * @brief 截断固定对象，并记录根外副作用。
 * @param[in,out] handle 借用存活写句柄。
 * @return 注入失败时 FALSE，其余截断到当前位置并返回 TRUE。
 */
BOOL SetEndOfFile(HANDLE handle)
{
    model_handle_t *value = handle;
    assert(value->live && (value->access & GENERIC_WRITE));
    if (g_truncate_fail) { g_truncate_fail = 0; return FALSE; }
    value->node->size = value->offset;
    if (is_outside(value->node)) g_outside_writes++;
    return TRUE;
}

/**
 * @brief 核验刷新句柄具有写权限，并注入刷新失败。
 * @param[in] handle 借用存活写句柄。
 * @return 注入失败时 FALSE，其余 TRUE。
 */
BOOL FlushFileBuffers(HANDLE handle)
{
    model_handle_t *value = handle;
    assert(value->live && (value->access & GENERIC_WRITE));
    if (g_flush_fail) { g_flush_fail = 0; return FALSE; }
    return TRUE;
}

/**
 * @brief 查询模型路径属性。
 * @param[in] path 借用 ANSI 路径。
 * @return 节点属性，缺失返回 INVALID_FILE_ATTRIBUTES。
 */
DWORD GetFileAttributesA(const char *path)
{
    if (!strcmp(path, "C:") || !strcmp(path, "C:\\")) return FILE_ATTRIBUTE_DIRECTORY;
    model_node_t *node = resolve_path(path, 0, 0);
    return node ? node->attributes : INVALID_FILE_ATTRIBUTES;
}

/**
 * @brief 输出模型属性、长度和原始 FILETIME。
 * @param[in] path 借用路径。
 * @param[in] level 模型忽略的信息级别。
 * @param[out] data 必需属性槽。
 * @return 存在返回 TRUE，缺失设置 FILE_NOT_FOUND 并返回 FALSE。
 */
BOOL GetFileAttributesExA(const char *path, int level, WIN32_FILE_ATTRIBUTE_DATA *data)
{
    (void)level;
    model_node_t *node = resolve_path(path, 0, 0);
    if (!node) { g_last_error = ERROR_FILE_NOT_FOUND; return FALSE; }
    memset(data, 0, sizeof(*data));
    data->dwFileAttributes = node->attributes;
    data->nFileSizeLow = (DWORD)node->size;
    data->ftLastWriteTime.dwHighDateTime = (DWORD)(g_filetime >> 32);
    data->ftLastWriteTime.dwLowDateTime = (DWORD)g_filetime;
    return TRUE;
}

/**
 * @brief 模拟递归创建过程中的已存在类型分支。
 * @param[in] path 借用目录路径。
 * @param[in] security 未使用，可为 NULL。
 * @return 已存在返回 FALSE 和 ALREADY_EXISTS，其余返回 FALSE 和 PATH_NOT_FOUND，模型只覆盖已有类型分支。
 */
BOOL CreateDirectoryA(const char *path, void *security)
{
    (void)security;
    if (!strcmp(path, "C:") || !strcmp(path, "C:\\")) {
        g_last_error = ERROR_ALREADY_EXISTS;
        return FALSE;
    }
    model_node_t *node = resolve_path(path, 0, 0);
    if (node) { g_last_error = ERROR_ALREADY_EXISTS; return FALSE; }
    g_last_error = ERROR_PATH_NOT_FOUND;
    return FALSE;
}

/**
 * @brief 为未使用的普通删除调用提供确定失败结果。
 * @param[in] path 借用路径，模型忽略。
 * @return 返回 FALSE，根相对 I/O 回归不调用本入口。
 */
BOOL DeleteFileA(const char *path)
{
    (void)path;
    return FALSE;
}

/**
 * @brief 为未使用的替换入口提供确定失败结果。
 * @param[in] source 借用源路径。
 * @param[in] destination 借用目标路径。
 * @param[in] flags 替换标记，模型忽略。
 * @return 返回 FALSE，本模型不证明 Win32 替换行为。
 */
BOOL MoveFileExA(const char *source, const char *destination, DWORD flags)
{
    (void)source; (void)destination; (void)flags;
    return FALSE;
}

/**
 * @brief 为未使用的目录枚举入口提供失败结果。
 * @param[in] path 借用搜索路径。
 * @param[out] data 本模型不写入。
 * @return 返回 INVALID_HANDLE_VALUE，本模型不覆盖原生目录枚举。
 */
HANDLE FindFirstFileA(const char *path, WIN32_FIND_DATAA *data)
{
    (void)path; (void)data;
    g_last_error = ERROR_PATH_NOT_FOUND;
    return INVALID_HANDLE_VALUE;
}

/**
 * @brief 为未使用的后续枚举入口提供结束结果。
 * @param[in] handle 借用枚举句柄，模型忽略。
 * @param[out] data 本模型不写入。
 * @return 返回 FALSE 并设置 NO_MORE_FILES。
 */
BOOL FindNextFileA(HANDLE handle, WIN32_FIND_DATAA *data)
{
    (void)handle; (void)data;
    g_last_error = ERROR_NO_MORE_FILES;
    return FALSE;
}

/**
 * @brief 完成未启用的模型枚举清理。
 * @param[in] handle 模型忽略的枚举句柄。
 * @return 返回 TRUE，无动态资源交付。
 */
BOOL FindClose(HANDLE handle)
{
    (void)handle;
    return TRUE;
}

/**
 * @brief 模拟 ANSI 全路径 API 的需求长度与原生失败回退。
 * @param[in] path 借用输入，默认直接作为结果，测试可提供更长结果。
 * @param[in] capacity 目标字节容量。
 * @param[out] buffer 可写 capacity 字节，容量不足时刻意不给 NUL。
 * @param[out] file_part 未使用，可为 NULL。
 * @return 成功返回不含 NUL 的长度，不足返回包含 NUL 的需求，注入原生失败返回 0。
 */
DWORD GetFullPathNameA(const char *path, DWORD capacity, char *buffer, char **file_part)
{
    (void)file_part;
    if (g_full_path_fail) return 0;
    const char *full = g_full_path ? g_full_path : path;
    size_t length = strlen(full);
    if (length >= capacity) { memset(buffer, '!', capacity); return (DWORD)length + 1; }
    memcpy(buffer, full, length + 1);
    return (DWORD)length;
}

/**
 * @brief 为 ASCII 大小写测试提供窄字符前缀比较。
 * @param[in] left 借用左字符串。
 * @param[in] right 借用右字符串。
 * @param[in] size 最大比较字节数。
 * @return 转发 POSIX strncasecmp 结果，不证明 Windows 非 ASCII 代码页排序。
 */
int _strnicmp(const char *left, const char *right, size_t size)
{
    return strncasecmp(left, right, size);
}

/**
 * @brief 登记生产堆分配以检查失败清理。
 * @param[in] ptr 可为 NULL 的新分配。
 * @return 原样返回，非空指针登记到测试表。
 */
static void *remember_allocation(void *ptr)
{
    if (ptr) {
        assert(g_live_allocations < sizeof(g_allocations) / sizeof(g_allocations[0]));
        g_allocations[g_live_allocations++] = ptr;
    }
    return ptr;
}

/**
 * @brief 对生产 malloc 的第 N 次调用注入失败。
 * @param[in] size 分配字节数。
 * @return 指定失败点为 NULL，其余返回登记的真实分配。
 */
static void *model_malloc(size_t size)
{
    if (++g_alloc_calls == g_fail_alloc) return NULL;
    return remember_allocation(malloc(size));
}

/**
 * @brief 对生产 calloc 的第 N 次调用注入失败。
 * @param[in] count 元素数。
 * @param[in] size 每元素字节数。
 * @return 失败点为 NULL，其余返回登记的零初始化存储。
 */
static void *model_calloc(size_t count, size_t size)
{
    if (++g_alloc_calls == g_fail_alloc) return NULL;
    return remember_allocation(calloc(count, size));
}

/**
 * @brief 模拟生产扩容并在成功搬迁后更新分配表。
 * @param[in,out] ptr 原拥有分配，可为 NULL。
 * @param[in] size 新容量字节数。
 * @return 新指针或 NULL，失败保留原分配。
 */
static void *model_realloc(void *ptr, size_t size)
{
    if (++g_alloc_calls == g_fail_alloc) return NULL;
    size_t index = 0;
    while (index < g_live_allocations && g_allocations[index] != ptr) index++;
    void *next = realloc(ptr, size);
    if (!next) return NULL;
    if (index < g_live_allocations) g_allocations[index] = next;
    else remember_allocation(next);
    return next;
}

/**
 * @brief 归还生产堆存储并撤销登记。
 * @param[in] ptr 独占指针，可为 NULL。
 * @post 资源释放，模型不再持有该地址。
 */
static void model_free(void *ptr)
{
    for (size_t i = 0; i < g_live_allocations; i++) {
        if (g_allocations[i] == ptr) { g_allocations[i] = g_allocations[--g_live_allocations]; break; }
    }
    free(ptr);
}

/**
 * @brief 用真实匿名临时文件提供普通文本读取夹具。
 * @param[in] path 借用路径，模型不解释。
 * @param[in] mode 借用打开方式，模型不解释。
 * @return 含三个原始字节的文件，由生产 fclose 结束拥有权。
 */
static FILE *model_fopen(const char *path, const char *mode)
{
    (void)path; (void)mode;
    FILE *file = tmpfile();
    assert(file);
    const char data[] = {'a', '\0', 'b'};
    assert(fwrite(data, 1, sizeof(data), file) == sizeof(data));
    rewind(file);
    g_stdio_open++;
    return file;
}

/**
 * @brief 对普通读取定位注入一次失败。
 * @param[in,out] file 借用真实夹具文件。
 * @param[in] offset 偏移字节数。
 * @param[in] origin 定位基准。
 * @return 注入时 -1，其余真实 fseek 结果。
 */
static int model_fseek(FILE *file, long offset, int origin)
{
    if (g_stdio_seek_fail) { g_stdio_seek_fail = 0; return -1; }
    return fseek(file, offset, origin);
}

/**
 * @brief 注入负文件长度。
 * @param[in,out] file 借用夹具文件。
 * @return 注入时 -1，其余真实 ftell 结果。
 */
static long model_ftell(FILE *file)
{
    if (g_stdio_tell_fail) { g_stdio_tell_fail = 0; return -1; }
    return ftell(file);
}

/**
 * @brief 注入无 ferror 的短读。
 * @param[out] data 可写的读取缓冲。
 * @param[in] size 元素字节数。
 * @param[in] count 元素数量。
 * @param[in,out] file 借用夹具文件。
 * @return 实际读取数量，注入时少请求一个元素。
 */
static size_t model_fread(void *data, size_t size, size_t count, FILE *file)
{
    if (g_stdio_short && count) { g_stdio_short = 0; count--; }
    return fread(data, size, count, file);
}

/**
 * @brief 真实关闭夹具后注入错误，核对生产关闭失败分支。
 * @param[in] file 由模型交付的独占文件。
 * @return 注入时 EOF，其余真实 fclose 结果。
 */
static int model_fclose(FILE *file)
{
    int result = fclose(file);
    g_stdio_open--;
    if (g_stdio_close_fail) { g_stdio_close_fail = 0; return EOF; }
    return result;
}

/**
 * @brief 为更名后的路径父目录函数提供生产前置声明。
 * @param[in] path 借用路径。
 * @return 由下方生产定义交付拥有字符串或 NULL。
 */
char *model_path_dirname(const char *path);

#ifndef _WIN32
#define _WIN32 1
#endif
#define malloc model_malloc
#define calloc model_calloc
#define realloc model_realloc
#define free model_free
#define fopen model_fopen
#define fseek model_fseek
#define ftell model_ftell
#define fread model_fread
#define fclose model_fclose
#define cc_filesystem_get_posix model_filesystem_create
#define cc_filesystem_get_default model_filesystem_default
#define cc_path_join model_path_join
#define cc_path_canonical model_path_canonical
#define cc_path_is_within model_path_is_within
#define cc_path_dirname model_path_dirname
#define cc_path_exists model_path_exists
#ifdef CC_TEST_WINDOWS_FS_SOURCE
#include CC_TEST_WINDOWS_FS_SOURCE
#else
#include "../../platforms/windows/src/cc_windows_filesystem.c"
#endif
#include "../../platforms/windows/src/cc_windows_path.c"
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef fopen
#undef fseek
#undef ftell
#undef fread
#undef fclose
#undef cc_filesystem_get_posix
#undef cc_filesystem_get_default
#undef cc_path_join
#undef cc_path_canonical
#undef cc_path_is_within
#undef cc_path_dirname
#undef cc_path_exists

/**
 * @brief 重置节点及故障项，要求上一场景已排空全部 I/O 资源。
 * @post 根、外部目录及链接重新建立，客户端堆对象继续保留。
 */
static void reset_model(void)
{
    assert(g_live_handles == 0 && g_stdio_open == 0);
    memset(g_nodes, 0, sizeof(g_nodes));
    memset(g_handles, 0, sizeof(g_handles));
    g_nodes_used = 0;
    g_root = add_node(NULL, "root", FILE_ATTRIBUTE_DIRECTORY);
    g_outside = add_node(NULL, "outside", FILE_ATTRIBUTE_DIRECTORY);
    g_named_root = g_root;
    g_safe = add_node(g_root, "safe", FILE_ATTRIBUTE_DIRECTORY);
    g_link = add_node(g_root, "link", FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT);
    g_link->redirect = g_outside;
    model_node_t *data = add_node(g_root, "data", FILE_ATTRIBUTE_NORMAL);
    memcpy(data->bytes, "old", 3); data->size = 3;
    g_outside_file = add_node(g_outside, "sentinel", FILE_ATTRIBUTE_NORMAL);
    memcpy(g_outside_file->bytes, "untouched", 9); g_outside_file->size = 9;
    g_outside_creates = g_outside_writes = g_race = g_race_blocked = g_rebind_root = 0;
    g_no_native_api = g_bad_native_handle = 0;
    g_seek_fail = g_truncate_fail = g_write_fail = g_flush_fail = g_close_fail = 0;
    g_stdio_seek_fail = g_stdio_tell_fail = g_stdio_short = g_stdio_close_fail = 0;
    g_filetime = UINT64_C(116444736000000000);
    g_full_path = NULL; g_full_path_fail = 0;
    g_alloc_calls = g_fail_alloc = 0;
}

/**
 * @brief 断言返回类别并释放按值交接的结果。
 * @param[in] result 交接唯一动态成员拥有权的结果。
 * @param[in] expected 预期错误码。
 * @post 动态成员已经归还。
 */
static void expect_result(cc_result_t result, cc_error_code_t expected)
{
    assert(result.code == expected);
    cc_result_free(&result);
}

/**
 * @brief 断言根外对象及全部 I/O 资源均保持预期状态。
 * @post 所有本次句柄已归还，外部哨兵未改变。
 */
static void assert_no_outside_effect(void)
{
    assert(g_outside_creates == 0 && g_outside_writes == 0 && g_live_handles == 0);
    assert(g_outside_file->size == 9 && !memcmp(g_outside_file->bytes, "untouched", 9));
}

/**
 * @brief 验证中间与最终 reparse、根 reparse 及目录替换交错都无法产生根外副作用。
 * @param[in,out] fs 借用存活客户端。
 * @post 每个场景的句柄释放，外部哨兵及目录均未修改。
 */
static void test_relative_identity(cc_filesystem_t *fs)
{
    reset_model();
    expect_result(fs->vtable->write_relative_text(fs->self, "C:\\root", "link/created", "bad"), CC_ERR_PERMISSION_DENIED);
    assert_no_outside_effect();
    reset_model();
    model_node_t *data = find_child(g_root, "data");
    data->attributes |= FILE_ATTRIBUTE_REPARSE_POINT;
    data->redirect = g_outside_file;
    expect_result(fs->vtable->write_relative_bytes(fs->self, "C:\\root", "data", "bad", 3), CC_ERR_PERMISSION_DENIED);
    char *text = (char *)(uintptr_t)1;
    expect_result(fs->vtable->read_relative_text(fs->self, "C:\\root", "data", &text), CC_ERR_PERMISSION_DENIED);
    assert(!text);
    assert_no_outside_effect();
    reset_model();
    g_root->attributes |= FILE_ATTRIBUTE_REPARSE_POINT;
    g_root->redirect = g_outside;
    expect_result(fs->vtable->write_relative_text(fs->self, "C:\\root", "created", "bad"), CC_ERR_PERMISSION_DENIED);
    assert_no_outside_effect();
    reset_model();
    g_race = 1;
    expect_result(fs->vtable->write_relative_text(fs->self, "C:\\root", "safe/created", "bad"), CC_ERR_PERMISSION_DENIED);
    assert_no_outside_effect();
    reset_model();
    g_race = 2;
    expect_result(fs->vtable->write_relative_text(fs->self, "C:\\root", "safe/created", "good"), CC_OK);
    assert(g_race_blocked == 1 && find_child(g_safe, "created"));
    assert_no_outside_effect();
    reset_model();
    g_rebind_root = 1;
    expect_result(fs->vtable->write_relative_text(fs->self, "C:\\root", "created", "good"), CC_OK);
    assert(find_child(g_root, "created") && !find_child(g_outside, "created"));
    assert_no_outside_effect();
}

/**
 * @brief 验证普通组件归一化、读写二进制、既有目标截断及完整失败清理。
 * @param[in,out] fs 借用存活客户端。
 * @post 所有返回缓冲归还，堆分配恢复到客户端基线。
 */
static void test_relative_data_and_failures(cc_filesystem_t *fs)
{
    size_t baseline = g_live_allocations;
    reset_model();
    expect_result(fs->vtable->write_relative_text(fs->self, "C:\\root", "foo.", "one"), CC_OK);
    expect_result(fs->vtable->write_relative_text(fs->self, "C:\\root", "foo ", "two"), CC_OK);
    model_node_t *foo = find_child(g_root, "foo");
    assert(foo && foo->size == 3 && !memcmp(foo->bytes, "two", 3));
    expect_result(fs->vtable->write_relative_text(fs->self, "C:\\root", ".. /created", "bad"), CC_ERR_PERMISSION_DENIED);
    expect_result(fs->vtable->write_relative_text(fs->self, "C:\\root", " . /created", "bad"), CC_ERR_PERMISSION_DENIED);
    expect_result(fs->vtable->write_relative_text(fs->self, "C:\\root", "NUL.txt", "bad"), CC_ERR_PERMISSION_DENIED);
    expect_result(fs->vtable->write_relative_text(fs->self, "\\\\?\\C:\\root", "literal.", "one"), CC_OK);
    assert(find_child(g_root, "literal.") && !find_child(g_root, "literal"));
    const char bytes[] = {'a', '\0', 'b'};
    expect_result(fs->vtable->write_relative_bytes(fs->self, "C:\\root", "data", bytes, sizeof(bytes)), CC_OK);
    char *text = NULL;
    expect_result(fs->vtable->read_relative_text(fs->self, "C:\\root", "data", &text), CC_OK);
    assert(!memcmp(text, bytes, sizeof(bytes)) && text[sizeof(bytes)] == '\0');
    model_free(text);
    assert_no_outside_effect();
    reset_model();
    expect_result(fs->vtable->write_relative_text(fs->self, "C:\\root", "safe/created", "ok"), CC_OK);
    int allocations = g_alloc_calls;
    assert(g_live_allocations == baseline);
    for (int index = 1; index <= allocations; index++) {
        reset_model();
        g_fail_alloc = index;
        expect_result(fs->vtable->write_relative_text(fs->self, "C:\\root", "safe/created", "ok"), CC_ERR_OUT_OF_MEMORY);
        assert_no_outside_effect();
        assert(g_live_allocations == baseline);
    }
    int *failures[] = {&g_seek_fail, &g_truncate_fail, &g_write_fail, &g_flush_fail, &g_close_fail};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        reset_model();
        *failures[i] = 1;
        expect_result(fs->vtable->write_relative_text(fs->self, "C:\\root", "data", "ok"), CC_ERR_IO);
        assert_no_outside_effect();
        assert(g_live_allocations == baseline);
    }
    reset_model(); g_no_native_api = 1;
    expect_result(fs->vtable->write_relative_text(fs->self, "C:\\root", "data", "ok"), CC_ERR_PLATFORM);
    assert_no_outside_effect();
    reset_model(); g_bad_native_handle = 1;
    expect_result(fs->vtable->write_relative_text(fs->self, "C:\\root", "data", "ok"), CC_ERR_IO);
    assert_no_outside_effect();
    assert(g_live_allocations == baseline);
    reset_model(); g_fail_alloc = 3;
    text = (char *)(uintptr_t)1;
    expect_result(fs->vtable->read_relative_text(fs->self, "C:\\root", "data", &text), CC_ERR_OUT_OF_MEMORY);
    assert(!text && g_live_allocations == baseline);
    assert_no_outside_effect();
}

/**
 * @brief 验证负长度、无 ferror 短读、关闭及 OOM 失败都不交付半文本。
 * @param[in,out] fs 借用客户端。
 * @post 真实匿名文件和文本存储全部释放。
 */
static void test_stdio_contract(cc_filesystem_t *fs)
{
    int *failures[] = {&g_stdio_seek_fail, &g_stdio_tell_fail, &g_stdio_short, &g_stdio_close_fail};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        reset_model();
        *failures[i] = 1;
        char *text = (char *)(uintptr_t)1;
        expect_result(fs->vtable->read_text(fs->self, "C:\\root\\data", &text), CC_ERR_IO);
        assert(!text && g_stdio_open == 0);
    }
    reset_model(); g_fail_alloc = 1;
    char *text = NULL;
    expect_result(fs->vtable->read_text(fs->self, "C:\\root\\data", &text), CC_ERR_OUT_OF_MEMORY);
    assert(!text && g_stdio_open == 0);
    reset_model();
    expect_result(fs->vtable->read_text(fs->self, "C:\\root\\data", &text), CC_OK);
    assert(text[0] == 'a' && text[1] == '\0' && text[2] == 'b' && text[3] == '\0');
    model_free(text);
}

/**
 * @brief 验证同步访问权限、FILETIME epoch、类型冲突与路径长缓冲和盘根结果。
 * @param[in,out] fs 借用客户端。
 * @post 所有路径和结果资源归还，模型注入回到缺省。
 */
static void test_metadata_and_paths(cc_filesystem_t *fs)
{
    reset_model();
    expect_result(fs->vtable->sync_file(fs->self, "C:\\root\\data"), CC_OK);
    expect_result(fs->vtable->make_dir(fs->self, "C:\\root\\data"), CC_ERR_IO);
    cc_filesystem_stat_t st;
    g_filetime = 0;
    expect_result(fs->vtable->stat(fs->self, "C:\\root\\data", &st), CC_OK);
    assert(st.modified_ms == 0);
    g_filetime = UINT64_C(116444736000000000) + UINT64_C(420070000);
    expect_result(fs->vtable->stat(fs->self, "C:\\root\\data", &st), CC_OK);
    assert(st.modified_ms == 42007);
    g_filetime = UINT64_MAX;
    expect_result(fs->vtable->stat(fs->self, "C:\\root\\data", &st), CC_OK);
    assert(st.modified_ms == (UINT64_MAX - UINT64_C(116444736000000000)) / 10000U);
    char long_path[2048];
    memset(long_path, 'x', sizeof(long_path) - 1);
    memcpy(long_path, "C:\\", 3);
    long_path[sizeof(long_path) - 1] = '\0';
    g_full_path = long_path;
    char *path = model_path_canonical("relative");
    assert(path && !strcmp(path, long_path));
    model_free(path);
    g_full_path = NULL;
    assert(model_path_is_within("C:\\", "C:\\folder\\file"));
    assert(!model_path_is_within("C:\\root", "C:\\root2\\file"));
    path = model_path_dirname("C:\\one/two\\file");
    assert(path && !strcmp(path, "C:\\one/two"));
    model_free(path);
    path = model_path_dirname("C:\\file");
    assert(path && !strcmp(path, "C:\\"));
    model_free(path);
    assert_no_outside_effect();
}

/**
 * @brief 运行 Windows 文件系统与路径的真实生产包含模型。
 * @return 全部断言成立返回 0，失败由 assert 终止。
 * @note API 替身覆盖本机控制流和句柄身份，不证明 Windows 调度、NTFS 分享规则、完整代码页或 ABI。
 */
int main(void)
{
    reset_model();
    cc_filesystem_t fs = {0};
    expect_result(model_filesystem_create(&fs), CC_OK);
    test_relative_identity(&fs);
    test_relative_data_and_failures(&fs);
    test_stdio_contract(&fs);
    test_metadata_and_paths(&fs);
    assert(g_live_allocations == 1 && g_live_handles == 0 && g_stdio_open == 0);
    fs.vtable->destroy(fs.self);
    assert(g_live_allocations == 0);
    return 0;
}
