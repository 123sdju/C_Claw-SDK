#ifndef CC_TEST_FILESYSTEM_WINTERNL_H
#define CC_TEST_FILESYSTEM_WINTERNL_H

#include "windows.h"
/* 仅模拟 NtCreateFile 所需的计数字符串与对象属性，实际声明以 Windows SDK 为准。 */
#define NTAPI
typedef int32_t NTSTATUS;
typedef struct UNICODE_STRING {
    USHORT Length;        /**< 有效 UTF-16 字节数。 */
    USHORT MaximumLength; /**< 缓冲容量字节数。 */
    WCHAR *Buffer;        /**< 调用期借用的 UTF-16 缓冲。 */
} UNICODE_STRING, *PUNICODE_STRING;
typedef struct OBJECT_ATTRIBUTES {
    ULONG Length;                  /**< 结构字节数。 */
    HANDLE RootDirectory;          /**< 借用的固定父目录句柄。 */
    PUNICODE_STRING ObjectName;     /**< 借用的单组件名称。 */
    ULONG Attributes;              /**< 名称查找及 reparse 控制位。 */
    PVOID SecurityDescriptor;      /**< 未使用的安全描述符。 */
    PVOID SecurityQualityOfService; /**< 未使用的服务质量信息。 */
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;
typedef struct IO_STATUS_BLOCK {
    NTSTATUS Status;   /**< 原生状态码。 */
    size_t Information; /**< 创建或打开结果信息。 */
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;
#define InitializeObjectAttributes(value, name, flags, parent, security) do { \
    (value)->Length = sizeof(OBJECT_ATTRIBUTES); \
    (value)->RootDirectory = (parent); \
    (value)->ObjectName = (name); \
    (value)->Attributes = (flags); \
    (value)->SecurityDescriptor = (security); \
    (value)->SecurityQualityOfService = NULL; \
} while (0)
#define OBJ_CASE_INSENSITIVE 0x40U
#define OBJ_DONT_REPARSE 0x1000U
#define FILE_OPEN 1U
#define FILE_OPEN_IF 3U
#define FILE_DIRECTORY_FILE 0x1U
#define FILE_SYNCHRONOUS_IO_NONALERT 0x20U
#define FILE_NON_DIRECTORY_FILE 0x40U
#define FILE_OPEN_REPARSE_POINT 0x200000U

#endif
