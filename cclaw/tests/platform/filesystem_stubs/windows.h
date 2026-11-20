#ifndef CC_TEST_FILESYSTEM_WINDOWS_H
#define CC_TEST_FILESYSTEM_WINDOWS_H

#include <stdint.h>
#include <stddef.h>

/* Linux 模型只提供生产文件使用的 Win32 布局片段和常量，不能作为原生 ABI 头使用。 */
typedef void *HANDLE;
typedef HANDLE *PHANDLE;
typedef void *HMODULE;
typedef void *PVOID;
typedef uint32_t DWORD;
typedef uint32_t ULONG;
typedef uint32_t ACCESS_MASK;
typedef uint16_t USHORT;
typedef uint16_t WCHAR;
typedef int BOOL;
typedef int32_t LONG;
/**
 * @brief 提供 GetProcAddress 返回地址的通用函数指针载体。
 * @post 使用者转换到生产定义中的完整 NTAPI 签名后调用，模型不通过此占位签名执行函数。
 */
typedef void (*FARPROC)(void);

typedef union LARGE_INTEGER {
    int64_t QuadPart; /**< 有符号 64 位长度。 */
} LARGE_INTEGER, *PLARGE_INTEGER;
typedef union ULARGE_INTEGER {
    struct { DWORD LowPart; DWORD HighPart; }; /**< Linux 小端模型的低、高位部分。 */
    uint64_t QuadPart; /**< 无符号 64 位 FILETIME 值。 */
} ULARGE_INTEGER;
typedef struct FILETIME {
    DWORD dwLowDateTime;  /**< 低 32 位。 */
    DWORD dwHighDateTime; /**< 高 32 位。 */
} FILETIME;
typedef struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes; /**< 模型对象属性。 */
    FILETIME ftLastWriteTime; /**< 模型文件修改时间。 */
    DWORD nFileSizeHigh; /**< 文件长度高 32 位。 */
    DWORD nFileSizeLow;  /**< 文件长度低 32 位。 */
} WIN32_FILE_ATTRIBUTE_DATA;
typedef struct BY_HANDLE_FILE_INFORMATION {
    DWORD dwFileAttributes; /**< 本模型只使用属性，完整原生结构由 Windows SDK 定义。 */
} BY_HANDLE_FILE_INFORMATION;
typedef struct WIN32_FIND_DATAA {
    char cFileName[260]; /**< 本模型只使用枚举名称。 */
} WIN32_FIND_DATAA;

#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES UINT32_MAX
#define MAX_PATH 260
#define GENERIC_READ UINT32_C(0x80000000)
#define GENERIC_WRITE UINT32_C(0x40000000)
#define SYNCHRONIZE UINT32_C(0x00100000)
#define FILE_READ_ATTRIBUTES UINT32_C(0x00000080)
#define FILE_TRAVERSE UINT32_C(0x00000020)
#define FILE_SHARE_READ 1U
#define FILE_SHARE_WRITE 2U
#define FILE_SHARE_DELETE 4U
#define OPEN_EXISTING 3U
#define OPEN_ALWAYS 4U
#define FILE_ATTRIBUTE_DIRECTORY 0x10U
#define FILE_ATTRIBUTE_NORMAL 0x80U
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400U
#define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000U
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000U
#define FILE_BEGIN 0U
#define FILE_NAME_NORMALIZED 0U
#define MOVEFILE_REPLACE_EXISTING 1U
#define MOVEFILE_WRITE_THROUGH 8U
#define GetFileExInfoStandard 0
#define ERROR_FILE_NOT_FOUND 2U
#define ERROR_PATH_NOT_FOUND 3U
#define ERROR_ACCESS_DENIED 5U
#define ERROR_ALREADY_EXISTS 183U
#define ERROR_NO_MORE_FILES 18U
#define CP_ACP 0U

#endif
