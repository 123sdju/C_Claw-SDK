#ifndef CC_TEST_FATFS_FF_H
#define CC_TEST_FATFS_FF_H

#include <stddef.h>
#include <stdint.h>

/* 测试使用窄字符和 16 位 UINT，暴露生产适配器的分块及长度转换边界。 */
typedef uint16_t UINT;
typedef uint64_t FSIZE_t;
typedef char TCHAR;
typedef enum {
    FR_OK, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE, FR_NO_PATH,
    FR_INVALID_NAME, FR_DENIED, FR_EXIST, FR_INVALID_OBJECT, FR_WRITE_PROTECTED,
    FR_INVALID_DRIVE, FR_NOT_ENABLED, FR_NO_FILESYSTEM, FR_MKFS_ABORTED,
    FR_TIMEOUT, FR_LOCKED, FR_NOT_ENOUGH_CORE, FR_TOO_MANY_OPEN_FILES,
    FR_INVALID_PARAMETER
} FRESULT;

typedef struct {
    FSIZE_t length; /**< 打开时的文件长度。 */
    FSIZE_t position; /**< 当前字节偏移。 */
    int open; /**< 原生对象是否仍打开。 */
    int modified; /**< 是否存在需要同步的修改。 */
} FIL;
typedef struct {
    size_t index; /**< 下一个枚举项序号。 */
    int open; /**< 目录对象是否仍打开。 */
} DIR;
typedef struct {
    unsigned char fattrib; /**< AM_DIR 或普通文件属性。 */
    FSIZE_t fsize; /**< 文件字节数。 */
    char fname[64]; /**< 枚举名称及 NUL。 */
} FILINFO;

#define FA_READ 1
#define FA_WRITE 2
#define FA_CREATE_ALWAYS 8
#define FA_OPEN_APPEND 48
#define AM_DIR 16
#define f_size(file) ((file)->length)

/** @brief 打开模型文件。@param[out] file 输出对象。@param[in] path 借用原生路径。@param[in] mode 打开标志。@return FatFs 结果。 */
FRESULT f_open(FIL *file, const TCHAR *path, unsigned char mode);
/** @brief 读取模型字节。@param[in,out] file 打开对象。@param[out] data 可写缓冲。@param[in] size 字节数。@param[out] got 实际字节数。@return FatFs 结果。 */
FRESULT f_read(FIL *file, void *data, UINT size, UINT *got);
/** @brief 写入模型字节。@param[in,out] file 打开对象。@param[in] data 借用字节。@param[in] size 字节数。@param[out] wrote 实际字节数。@return FatFs 结果。 */
FRESULT f_write(FIL *file, const void *data, UINT size, UINT *wrote);
/** @brief 调整偏移。@param[in,out] file 打开对象。@param[in] offset 字节偏移。@return FatFs 结果。 */
FRESULT f_lseek(FIL *file, FSIZE_t offset);
/** @brief 关闭模型文件。@param[in,out] file 打开对象，失败保留打开状态。@return FatFs 结果。 */
FRESULT f_close(FIL *file);
/** @brief 同步模型脏状态。@param[in,out] file 打开对象。@return FatFs 结果。 */
FRESULT f_sync(FIL *file);
/** @brief 查询模型类型。@param[in] path 借用路径。@param[out] info 元数据。@return FatFs 结果。 */
FRESULT f_stat(const TCHAR *path, FILINFO *info);
/** @brief 创建模型目录。@param[in] path 借用路径。@return FatFs 结果。 */
FRESULT f_mkdir(const TCHAR *path);
/** @brief 删除模型路径。@param[in] path 借用路径。@return FatFs 结果。 */
FRESULT f_unlink(const TCHAR *path);
/** @brief 打开模型目录。@param[out] dir 目录对象。@param[in] path 借用路径。@return FatFs 结果。 */
FRESULT f_opendir(DIR *dir, const TCHAR *path);
/** @brief 枚举下一项。@param[in,out] dir 目录对象。@param[out] info 名称及属性。@return FatFs 结果。 */
FRESULT f_readdir(DIR *dir, FILINFO *info);
/** @brief 关闭模型目录。@param[in,out] dir 目录对象。@return FatFs 结果。 */
FRESULT f_closedir(DIR *dir);

#endif
