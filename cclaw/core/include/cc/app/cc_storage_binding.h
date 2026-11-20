#ifndef CC_STORAGE_BINDING_H
#define CC_STORAGE_BINDING_H

#include "cc/ports/cc_filesystem.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 显式选择身份配对的 SDK 调用者提供的持久设备端口，禁止隐式擦除或替换旧身份。
 * @note 调用者独占设备端口和身份目录并保持挂载稳定，端口借用寿命覆盖整个同步操作。
 * 当前固件默认换盘流程使用当前盘全新装配，身份 inspect/initialize 是独立可选机制。
 */
typedef struct cc_storage_identity_store {
    size_t size; /**< 初始化为 sizeof(cc_storage_identity_store_t)。 */
    void *self; /**< 同步借用的实现状态，由提供者管理寿命。 */
    /**
     * @brief 读取跨重启保留的设备身份副本。
     * @param[in] self 存活实现状态借用。
     * @param[out] out_identity 新空槽，成功返回可 free 的独立 NUL 字符串。
     * @return 缺失为 NOT_FOUND，成功或具体错误原样交付，动态诊断由绑定逻辑消费或转交。
     * @note 失败也须让输出保持 NULL 或可 free 的局部拥有缓冲，绑定逻辑会归还该输出。
     */
    cc_result_t (*read)(void *self, char **out_identity);
    /**
     * @brief 仅在设备无身份时原子安装初值。
     * @param[in,out] self 存活实现状态借用。
     * @param[in] identity 调用期借用的有效标记，成功后实现保留独立持久副本。
     * @return 成功或具体错误，失败可能已经提交，调用者必须保留错误并进入明确修复流程。
     */
    cc_result_t (*write_initial)(void *self, const char *identity);
} cc_storage_identity_store_t;

/** @brief 成功检查得到的五种身份组合，零用于失败输出。 */
typedef enum cc_storage_binding_state {
    CC_STORAGE_BINDING_UNBOUND = 1, /**< 两侧均缺失。 */
    CC_STORAGE_BINDING_MATCH, /**< 两侧有效标记字节相同。 */
    CC_STORAGE_BINDING_DEVICE_ONLY, /**< 仅设备持有标记。 */
    CC_STORAGE_BINDING_MEDIA_ONLY, /**< 仅目录内存在有效标记。 */
    CC_STORAGE_BINDING_MISMATCH /**< 两侧标记均有效但不同。 */
} cc_storage_binding_state_t;

/**
 * @brief 检查两个明确拼写的 FAT 绝对路径并拒绝相等或任向父子关系。
 * @param[in] workspace 调用期稳定路径，须非 NULL，可保留单个尾斜杠。
 * @param[in] private_directory 调用期稳定保护路径，规则同 workspace。
 * @return 合法且分离为 CC_OK，词法错误为 INVALID_ARGUMENT，重叠为 PERMISSION_DENIED。
 * @note ASCII 大小写折叠，拒绝空段、点段、尾点、短名波浪号、非 ASCII 和禁用符号。
 * 外层提供无符号链接文件系统及工具根约束，函数只做词法检查，动态诊断由调用者释放。
 */
cc_result_t cc_storage_binding_validate_fat_paths(const char *workspace, const char *private_directory);
/**
 * @brief 检查单个 FAT 绝对路径的明确拼写。
 * @param[in] path 调用期稳定路径，NULL 或词法不合法会失败。
 * @return 合法为 CC_OK，否则为 INVALID_ARGUMENT，动态诊断由调用者释放。
 * @note 最大原始长度为 255 字节，允许根路径，本入口不检查路径重叠或实际存在性。
 */
cc_result_t cc_storage_binding_validate_fat_path(const char *path);

/**
 * @brief 只读检查设备与媒体身份组合，委托详细入口。
 * @param[in] fs 存活文件系统借用，需 stat/read_text 及当前结构布局。
 * @param[in] directory 调用期稳定非空目录，实际目录缺失视为无媒体标记。
 * @param[in] device 存活设备身份端口借用。
 * @param[out] out_state 必填独立槽，先置零，成功写入组合状态。
 * @return 成功或具体依赖/存储/IO/OOM 错误，动态诊断由调用者释放。
 * @pre 外层全程独占目录与设备端口，保持挂载稳定。选择该机制的 owner 每次换挂载后重新核对。
 * @note 临时/备份存在或标记损坏返回错误。MATCH 只证明可复制标记相同，物理介质真实性需其他机制。
 */
cc_result_t cc_storage_binding_inspect(cc_filesystem_t *fs, const char *directory,
                                       const cc_storage_identity_store_t *device,
                                       cc_storage_binding_state_t *out_state);
/**
 * @brief 检查身份组合并区分设备身份读取或格式错误。
 * @param[in] fs 存活文件系统借用，依赖同简化入口。
 * @param[in] directory 稳定非空目录，可实际缺失。
 * @param[in] device 存活设备端口，read 输出由本函数归还。
 * @param[out] out_state 必填独立状态槽，先清零，成功才填写。
 * @param[out] out_device_identity_failed 可空独立标志，先清零，设备读取/格式错误时为 1。
 * @return 成功或具体错误，身份缺失为有效组合，媒体与设备错误均保持状态零。
 * @pre 外层独占目录与设备端口并保持挂载稳定，输出槽互不重叠且不覆盖输入。
 */
cc_result_t cc_storage_binding_inspect_detailed(cc_filesystem_t *fs, const char *directory,
    const cc_storage_identity_store_t *device, cc_storage_binding_state_t *out_state,
    int *out_device_identity_failed);

/**
 * @brief 在既有目录中仅为双侧缺失状态初始化身份，委托详细入口。
 * @param[in,out] fs 存活文件系统借用，需同步文件与原子替换能力。
 * @param[in] directory 已存在非空目录，外层全程独占并保持挂载稳定。
 * @param[in,out] device 存活设备端口，write_initial 仅允许原子安装初值。
 * @return 成功或具体错误，已有任意身份为 INVALID_STATE，失败现场可能含单侧或双侧标记。
 * @note 先写/同步/核对媒体再安装设备身份，replace 尝试后保留不确定现场。
 * owner 锁存初始化错误，重启或明确修复后恢复其身份相关工作，后续 MATCH 不能撤销原错误。
 */
cc_result_t cc_storage_binding_initialize(cc_filesystem_t *fs, const char *directory,
                                          const cc_storage_identity_store_t *device);
/**
 * @brief 初始化身份并单独标记设备读、写或核对失败。
 * @param[in,out] fs 存活文件系统，能力要求同简化入口。
 * @param[in] directory 已存在目录，外层全程独占且挂载不变。
 * @param[in,out] device 存活设备端口，输入身份仅在调用期间借用。
 * @param[out] out_device_identity_failed 可空独立标志，先清零，设备相关失败时为 1。
 * @return 成功或具体错误，第一错误保留，清理错误仅消费诊断。
 * @note 已检查固定临时/备份缺失，在独占前提下仅清理本次已进入写入且尚未尝试 replace 的临时文件。
 * 文件系统接口不提供 exclusive-create，外层排除其他写入者是该多步骤事务的既有依赖。
 */
cc_result_t cc_storage_binding_initialize_detailed(cc_filesystem_t *fs, const char *directory,
    const cc_storage_identity_store_t *device, int *out_device_identity_failed);

#ifdef __cplusplus
}
#endif

#endif
