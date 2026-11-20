



#ifndef CC_FILESYSTEM_H
#define CC_FILESYSTEM_H

#include "cc/core/cc_result.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 当前结构布局版本，版本 4 包含根相对二进制写与尾部读取扩展。 */
#define CC_FILESYSTEM_VTABLE_VERSION 4U

/** 后端能力描述，调用者还须检查对应 vtable 成员及结构版本。 */
enum {
    CC_FS_CAP_BINARY = 1ULL << 0,         /**< 提供原始字节文件读写。 */
    CC_FS_CAP_APPEND = 1ULL << 1,         /**< 提供二进制追加。 */
    CC_FS_CAP_STAT = 1ULL << 2,           /**< 提供元数据查询。 */
    CC_FS_CAP_ATOMIC_REPLACE = 1ULL << 3, /**< 提供替换安装入口，原子性及恢复规则按具体后端说明。 */
    CC_FS_CAP_FILE_SYNC = 1ULL << 4,      /**< 提供文件同步请求。 */
    CC_FS_CAP_DIR_SYNC = 1ULL << 5,       /**< 提供目录同步请求。 */
    CC_FS_CAP_ROOT_SCOPED = 1ULL << 6,    /**< 提供根目录约束的相对路径入口。 */
    CC_FS_CAP_NO_SYMLINKS = 1ULL << 7,    /**< 根相对入口拒绝链接，或后端文件系统本身无此能力。 */
    CC_FS_CAP_READ_TAIL = 1ULL << 8,      /**< 提供调用方缓冲的文件尾读取。 */
};

/** 路径查询类型，具体后端对链接和特殊文件的分类可能不同。 */
typedef enum cc_filesystem_entry_type {
    CC_FS_ENTRY_MISSING = 0, /**< 路径不存在。 */
    CC_FS_ENTRY_FILE,        /**< 普通文件或后端按文件分类的对象。 */
    CC_FS_ENTRY_DIRECTORY,   /**< 目录。 */
    CC_FS_ENTRY_OTHER,       /**< 后端能够区分的链接或其他特殊对象。 */
} cc_filesystem_entry_type_t;

typedef struct cc_filesystem_stat {
    size_t size;                      /**< 当前结构大小，由后端写入。 */
    cc_filesystem_entry_type_t type;   /**< 不存在、文件、目录或其他对象。 */
    uint64_t byte_size;               /**< 后端报告的字节长度，目录及特殊对象的含义由平台决定。 */
    uint64_t modified_ms;             /**< Unix epoch 毫秒，早于 1970 或后端无法提供时为 0，超范围时饱和。 */
} cc_filesystem_stat_t;


/* filesystem vtable 前置声明。 */
typedef struct cc_filesystem_vtable cc_filesystem_vtable_t;

/* filesystem port 前置声明。 */
typedef struct cc_filesystem cc_filesystem_t;

/**
 * @brief 文件系统接口及其能力元数据，按值复制时仅复制对 self 的借用。
 * @note 创建者持有 self 的唯一销毁责任，排空所有调用及浅拷贝借用后执行 destroy。
 *       各普通操作的文件竞争及事务顺序由上层协调，根约束能力由所选后端实现。
 */
struct cc_filesystem {
    void *self;                          /**< 平台拥有的私有对象。 */
    const cc_filesystem_vtable_t *vtable; /**< 生命周期内有效的函数表借用，默认后端为静态表。 */
    size_t size;                         /**< 接口对象布局大小。 */
    unsigned version;                    /**< 后端发布的函数表布局版本。 */
    uint64_t capabilities;               /**< 后端支持的 CC_FS_CAP 位集合。 */
};


/**
 * @brief 文件系统操作表，所有路径和正文输入仅在同步调用期间借用。
 * @note 路径编码沿平台原生约定，POSIX 使用原始路径字节，Windows 当前使用 ANSI/CRT 窄字符，
 *       文本接口不作通用 UTF-8 校验或转码。ESP32 VFS 和 FreeRTOS FatFs 可另有路径映射及容量限制。
 *       返回 cc_result_t 的动态成员归调用者 cc_result_free，普通写入失败可能已改变目标，
 *       多操作持久化事务由上层结合 atomic_replace、sync_file 和 sync_dir 组织。
 */
struct cc_filesystem_vtable {

    size_t size;           /**< 本函数表布局字节数。 */
    unsigned version;      /**< CC_FILESYSTEM_VTABLE_VERSION 对应布局版本。 */
    uint64_t capabilities; /**< 支持的操作能力，未支持的扩展可能为空或返回 UNSUPPORTED。 */


    /**
     * @brief 读取文件内容并提供末尾 NUL。
     * @param[in] self 存活后端对象的借用，具体实现可忽略。
     * @param[in] path 非 NULL 的借用路径。
     * @param[out] out_text 必需的空输出槽，成功获得调用方 free 的缓冲。
     * @return 成功返回 CC_OK，参数、路径、分配、容量或 I/O 错误返回对应结果。
     * @note POSIX/Windows 保留内嵌 NUL，ESP32 文本读取会拒绝内嵌 NUL，文件大小上限由后端决定。
     */
    cc_result_t (*read_text)(
        void *self,
        const char *path,
        char **out_text
    );



    /**
     * @brief 创建或覆盖文件为 NUL 结尾正文。
     * @param[in] self 存活后端对象的借用。
     * @param[in] path 非 NULL 的借用路径，父目录行为由后端决定。
     * @param[in] text 调用期借用正文，默认后端将 NULL 作为空文本。
     * @return 成功返回 CC_OK，分配、路径或 I/O 失败返回错误，可能已经截断或写入部分正文。
     * @note POSIX/Windows 不创建父目录，FatFs 在缺父目录时可尝试创建，不隐含原子替换。
     */
    cc_result_t (*write_text)(
        void *self,
        const char *path,
        const char *text
    );



    /**
     * @brief 查询后端是否能确认路径存在。
     * @param[in] self 存活后端借用。
     * @param[in] path 必需的借用路径。
     * @param[out] out_exists 必需输出槽，成功写 0 或 1。
     * @return 成功返回 CC_OK，参数或后端错误返回对应结果，部分平台将访问失败归为不存在。
     */
    cc_result_t (*exists)(
        void *self,
        const char *path,
        int *out_exists
    );



    /**
     * @brief 列举目录中的名称，顺序由后端遍历决定。
     * @param[in] self 存活后端借用。
     * @param[in] path 必需的借用目录路径。
     * @param[out] out_items 必需空槽，成功取得名称数组及逐项字符串的拥有权，均由调用方 free。
     * @param[out] out_count 必需计数槽，成功写名称数量，空目录允许数组为 NULL 或可释放空分配。
     * @return 成功返回 CC_OK，参数、分配、容量或枚举错误返回对应结果。
     * @note 名称不含完整父路径，结果受目录并发变化影响，POSIX/Windows 失败回收部分列表。
     */
    cc_result_t (*list_dir)(
        void *self,
        const char *path,
        char ***out_items,
        size_t *out_count
    );



    /**
     * @brief 创建目录，递归行为和路径映射由后端决定。
     * @param[in] self 存活后端借用。
     * @param[in] path 必需的借用目录路径。
     * @return 成功返回 CC_OK，路径、分配或创建失败返回错误，失败可能已创建部分父目录。
     */
    cc_result_t (*make_dir)(
        void *self,
        const char *path
    );



    /**
     * @brief 删除选定路径，目标类型支持由后端决定。
     * @param[in] self 存活后端借用。
     * @param[in] path 必需的借用路径，调用方负责目标选择。
     * @return 删除成功返回 CC_OK，路径不存在、类型或 I/O 失败返回错误。
     * @note POSIX remove 支持空目录，Windows 当前只调用 DeleteFileA，不执行递归删除。
     */
    cc_result_t (*remove)(
        void *self,
        const char *path
    );



    /**
     * @brief 释放平台私有对象。
     * @param[in] self 唯一拥有的后端对象，默认后端允许 NULL。
     * @pre 所有端口调用和浅拷贝借用已经退出。
     * @post self 失效，外层接口容器和其清零责任继续由调用方承担。
     */
    void (*destroy)(void *self);



    /**
     * @brief 创建或截断文件并写入原始二进制正文。
     * @param[in] self 存活后端借用。
     * @param[in] path 必需的借用目标路径。
     * @param[in] data 借用 size 字节，size 为零时允许 NULL。
     * @param[in] size 写入字节数，零长度仍可创建或截断文件。
     * @return 成功返回 CC_OK，失败返回错误且可能留下部分内容，不隐含原子安装。
     */
    cc_result_t (*write_bytes)(
        void *self,
        const char *path,
        const void *data,
        size_t size
    );

    /**
     * @brief 读取整个文件的原始字节并交付独占缓冲。
     * @param[in] self 存活后端借用。
     * @param[in] path 必需的借用文件路径。
     * @param[out] out_data 必需空槽，成功取得调用方 free 的缓冲，空文件分配形式由后端决定。
     * @param[out] out_size 必需长度槽，成功写有效字节数，缓冲不保证另有 NUL。
     * @return 成功返回 CC_OK，参数、容量、分配或读取失败返回错误。
     */
    cc_result_t (*read_bytes)(
        void *self,
        const char *path,
        void **out_data,
        size_t *out_size
    );

    /**
     * @brief 向文件末尾追加二进制正文。
     * @param[in] self 存活后端借用。
     * @param[in] path 必需的借用文件路径，是否自动创建由后端决定。
     * @param[in] data 借用 size 字节，零长度时允许 NULL。
     * @param[in] size 待追加字节数。
     * @return 成功返回 CC_OK，失败返回错误，可能已经追加前缀。
     * @note 整条记录的并发原子性由调用者或后端更高层协议保证。
     */
    cc_result_t (*append_bytes)(
        void *self,
        const char *path,
        const void *data,
        size_t size
    );

    /**
     * @brief 查询路径元数据，不存在可作为成功的 MISSING 类型交付。
     * @param[in] self 存活后端借用。
     * @param[in] path 必需的借用路径。
     * @param[out] out_stat 必需槽，成功填 size、类型、长度及 Unix 毫秒修改时间。
     * @return 查询成功或缺失返回 CC_OK，其他参数或后端错误返回错误结果。
     * @note POSIX lstat 将最终链接归为 OTHER，其他后端分类由其原生属性或文件系统能力决定。
     */
    cc_result_t (*stat)(
        void *self,
        const char *path,
        cc_filesystem_stat_t *out_stat
    );

    /**
     * @brief 将临时文件安装到目标路径，后端须发布 ATOMIC_REPLACE 能力方可依赖。
     * @param[in] self 存活后端借用。
     * @param[in] temporary_path 必需的借用源路径，成功后源路径消失。
     * @param[in] destination_path 必需的借用目标路径，按原生替换规则安装。
     * @return 成功返回 CC_OK，不支持返回 UNSUPPORTED，其他失败返回错误并由调用者恢复文件状态。
     * @note 同卷、持久化和目录同步条件由后端及文件系统决定。ESP32 FAT 后端可能采用
     *       目标到备份、临时到目标的两步安装，使用方协调候选并在重启后恢复。
     */
    cc_result_t (*atomic_replace)(
        void *self,
        const char *temporary_path,
        const char *destination_path
    );

    /**
     * @brief 请求将文件内容提交到后端存储。
     * @param[in] self 存活后端借用。
     * @param[in] path 必需的借用文件路径。
     * @return 成功返回 CC_OK，不支持返回 UNSUPPORTED，打开或同步失败返回错误。
     * @note 是否提供物理持久化保证还受文件系统、驱动和设备影响。
     */
    cc_result_t (*sync_file)(void *self, const char *path);
    /**
     * @brief 请求同步目录元数据。
     * @param[in] self 存活后端借用。
     * @param[in] path 借用目录路径，支持此能力的后端要求有效。
     * @return 成功返回 CC_OK，Windows、默认 ESP32/FatFs 当前返回 UNSUPPORTED，其他失败返回错误。
     */
    cc_result_t (*sync_dir)(void *self, const char *path);

    /**
     * @brief 按 ROOT_SCOPED 后端的根约束读取文本。
     * @param[in] self 存活后端借用。
     * @param[in] root 必需的借用根目录路径。
     * @param[in] relative_path 必需非空相对路径，组件、分隔符及链接规则由后端决定。
     * @param[out] out_text 必需空槽，成功取得调用方 free 的 NUL 结尾缓冲。
     * @return 成功返回 CC_OK，路径约束、容量、分配或 I/O 失败返回对应错误。
     * @note POSIX 逐级目录描述符固定对象，Windows 保持整条目录句柄链并拒绝 reparse；
     *       ESP32/FatFs 结合其不提供符号链接的后端假设检查并映射路径。
     */
    cc_result_t (*read_relative_text)(
        void *self,
        const char *root,
        const char *relative_path,
        char **out_text
    );

    /**
     * @brief 按后端根约束创建或覆盖文本文件。
     * @param[in] self 存活后端借用。
     * @param[in] root 必需的借用根目录路径。
     * @param[in] relative_path 必需非空借用相对路径。
     * @param[in] text 借用 NUL 结尾正文，默认后端允许 NULL 表示空正文。
     * @return 成功返回 CC_OK，路径约束、分配或 I/O 失败返回错误，可能已修改根内文件。
     * @note 不隐含临时文件事务，父目录创建与同步行为由具体后端决定。
     */
    cc_result_t (*write_relative_text)(
        void *self,
        const char *root,
        const char *relative_path,
        const char *text
    );

    /**
     * @brief 按后端根约束覆盖写入二进制内容。
     * @param[in] self 存活后端借用。
     * @param[in] root 必需的借用根目录路径。
     * @param[in] relative_path 必需非空借用相对路径。
     * @param[in] data 借用 size 字节，零长度时可为 NULL。
     * @param[in] size 待写字节数，零长度可创建或截断文件。
     * @return 成功返回 CC_OK，路径约束、分配或 I/O 失败返回错误，根内目标可能已有部分结果。
     */
    cc_result_t (*write_relative_bytes)(
        void *self,
        const char *root,
        const char *relative_path,
        const void *data,
        size_t size
    );

    /**
     * @brief 从文件末尾读取不超过 capacity 字节，不补 NUL。
     * @param[in] self 存活后端借用。
     * @param[in] path 必需的借用文件路径。
     * @param[out] buffer 容量至少为 capacity 的可写缓冲，零容量允许 NULL。
     * @param[in] capacity 字节容量，平台定位或原生计数类型可进一步限制。
     * @param[out] out_size 必需槽，基本参数有效后置零，成功才写入有效字节数。
     * @return 成功返回 CC_OK，参数、定位、短读或关闭失败返回错误，失败可已经修改 buffer 前缀。
     */
    cc_result_t (*read_tail)(
        void *self,
        const char *path,
        void *buffer,
        size_t capacity,
        size_t *out_size
    );
};

/**
 * @brief 创建当前 profile 默认文件系统对象。
 * @param[out] out_fs 必需空接口槽，成功拥有 self，默认后端失败保持空输出。
 * @return 成功返回 CC_OK，参数或分配失败返回错误；未启用 FatFs 时仍可得到零能力的占位对象。
 */
cc_result_t cc_filesystem_get_default(cc_filesystem_t *out_fs);

/**
 * @brief 创建兼容命名的当前平台文件系统对象。
 * @param[out] out_fs 必需空槽，成功拥有 self 并借用静态 vtable。
 * @return 原样返回平台创建结果，POSIX 为本机实现，Windows/ESP32/FreeRTOS 为各自默认实现。
 */
cc_result_t cc_filesystem_get_posix(cc_filesystem_t *out_fs);

#ifdef __cplusplus
}
#endif

#endif
