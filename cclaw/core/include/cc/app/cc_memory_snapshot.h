#ifndef CC_MEMORY_SNAPSHOT_H
#define CC_MEMORY_SNAPSHOT_H

#include "cc/ports/cc_filesystem.h"

/**
 * @brief 绑定一个记忆主文件的快照存储。
 *
 * 实例借用 fs，复制路径并拥有内部互斥锁。快照创建、确认、列举、读取和删除在实例锁内执行
 * 文件操作。主文件的写入与其他实例的访问由外层协调，创建/确认至对应记忆提交期间须持续
 * 持有该主文件的写入所有权。销毁前先排空所有调用。
 * 快照经 cc_json_parse_strict 完成尾部消费校验并要求顶层为数组，再检查文件长度及 ID 中的 CRC32。
 * 解析沿用 cJSON 的接纳规则，记录业务格式由记忆存储校验。
 */
typedef struct cc_memory_snapshot_store cc_memory_snapshot_store_t;
typedef enum cc_memory_snapshot_reason {
    CC_MEMORY_SNAPSHOT_REWRITE = 1, /**< 大规模自动重写前的版本，ID 使用 rewrite- 前缀。 */
    CC_MEMORY_SNAPSHOT_RESET = 2   /**< 用户重置前的版本，ID 使用 reset- 前缀。 */
} cc_memory_snapshot_reason_t;
typedef struct cc_memory_snapshot_info {
    char *id;                           /**< 列表拥有的快照 ID 字符串。 */
    cc_memory_snapshot_reason_t reason;  /**< 从 ID 前缀解析出的创建原因。 */
    uint64_t modified_ms;                /**< 文件系统返回的修改时间，单位毫秒。 */
} cc_memory_snapshot_info_t;

/**
 * @brief 校验文件系统能力和既有目录，创建绑定主文件路径的快照存储。
 * @param[in] fs 借用至存储销毁的文件系统及其 vtable，须支持 stat、目录列举、读写、删除、文件同步及原子替换。
 * @param[in] directory 已存在目录的非空路径，同步借用并复制。
 * @param[in] active_path 主文件的非空路径，同步借用并复制，打开时仅检查路径参数。
 * @param[out] out_store 必填，先置 NULL，成功取得由调用者销毁的存储。
 * @return 成功为 CC_OK，输出为空为 CC_ERR_INVALID_ARGUMENT，能力/路径参数不满足为 CC_ERR_UNSUPPORTED，
 * 目录类型错误为 CC_ERR_STORAGE，分配失败为 CC_ERR_OUT_OF_MEMORY，文件系统和锁创建错误原样返回。
 */
cc_result_t cc_memory_snapshot_store_open(cc_filesystem_t *fs, const char *directory,
                                          const char *active_path,
                                          cc_memory_snapshot_store_t **out_store);
/**
 * @brief 释放快照存储的锁、路径副本及实例。
 * @pre 所有使用者已退出，借用的 fs 仍存活。
 * @param[in,out] store 待销毁实例，允许 NULL。
 * @post 非空实例寿命结束，历史文件继续保存在目录中。
 */
void cc_memory_snapshot_store_destroy(cc_memory_snapshot_store_t *store);
/**
 * @brief 按路径字符串字节比较绑定的主文件身份。
 * @param[in] store 借用的存储，允许 NULL。
 * @param[in] active_path 同步借用的待比较路径，允许 NULL。
 * @return 两个参数有效且与绑定路径逐字相同时为一，否则为零。
 */
int cc_memory_snapshot_source_matches(cc_memory_snapshot_store_t *store, const char *active_path);

/**
 * @brief 读取并解析当前数组主文件，经复制、同步及回读校验后安装为新快照。
 *
 * ID 由原因前缀、UUID 和内容 CRC32 组成。写入先使用同 ID 的 .tmp，再原子替换为 .json。
 * 替换前失败仅尝试清理本次写入的临时文件。替换已经尝试时保留现场并输出 ID，
 * 即使返回错误，调用者仍须保管 ID，使用 confirm 核对后决定是否继续对应记忆提交。
 * @param[in,out] store 借用的快照存储及文件系统。
 * @param[in] reason 创建原因，须为 REWRITE 或 RESET。
 * @param[out] out_id 必填，初始参数有效后置 NULL，成功或已尝试替换时输出堆字符串，调用者用 free 释放。
 * @return 成功为 CC_OK，参数错误为 CC_ERR_INVALID_ARGUMENT，主文件缺失为 CC_ERR_NOT_FOUND，
 * 文件、校验、路径冲突、分配、UUID 生成或同步错误向上传递，错误时可能保留文件及输出 ID。
 */
cc_result_t cc_memory_snapshot_create(cc_memory_snapshot_store_t *store,
                                       cc_memory_snapshot_reason_t reason, char **out_id);
/**
 * @brief 校验快照内容及 CRC32，确认与当前主文件逐字相同后同步目录。
 * @param[in,out] store 借用的快照存储，内部加锁并可能同步目录。
 * @param[in] id 同步借用的合法快照 ID，须含原因前缀、小写 UUID 和八位小写 CRC32。
 * @return 相同且同步成功为 CC_OK，非法 ID 为 CC_ERR_INVALID_ARGUMENT，文件缺失为 CC_ERR_NOT_FOUND，
 * 主文件内容已变化为 CC_ERR_INVALID_STATE，分配、读取、格式、校验或同步错误向上传递。
 */
cc_result_t cc_memory_snapshot_confirm(cc_memory_snapshot_store_t *store, const char *id);
/**
 * @brief 校验目录中的快照文件并按修改时间降序、ID 字节序升序返回列表。
 *
 * 扫描匹配原因前缀和 .json 后缀的文件，任一候选名称或内容损坏使整次列举失败。
 * @param[in] store 借用的快照存储，内部加锁遍历文件。
 * @param[out] out_items 必填且无待释放内容，参数有效后置 NULL，成功取得拥有的数组及 ID，用 cc_memory_snapshot_info_free_array 清理。
 * @param[out] out_count 必填，参数有效后置零，成功返回有效条数，零条时数组仍可能非空。
 * @return 成功为 CC_OK，参数错误为 CC_ERR_INVALID_ARGUMENT，目录读取、分配、候选文件读取/校验错误向上传递，失败清理全部部分结果。
 */
cc_result_t cc_memory_snapshot_list(cc_memory_snapshot_store_t *store,
                                     cc_memory_snapshot_info_t **out_items, size_t *out_count);
/**
 * @brief 读取指定快照并校验 JSON 数组及 ID 中的 CRC32。
 * @param[in] store 借用的快照存储，内部加锁读取。
 * @param[in] id 同步借用的合法快照 ID。
 * @param[out] out_json 必填，参数有效后置 NULL，成功取得 NUL 结尾的原文件内容，调用者用 free 释放。
 * @return 成功为 CC_OK，参数错误为 CC_ERR_INVALID_ARGUMENT，缺失为 CC_ERR_NOT_FOUND，分配、读取及校验错误向上传递，失败输出为空。
 */
cc_result_t cc_memory_snapshot_read(cc_memory_snapshot_store_t *store, const char *id, char **out_json);
/**
 * @brief 删除 ID 对应的 .json 文件并按文件系统能力同步目录。
 * @param[in,out] store 借用的快照存储及文件系统。
 * @param[in] id 同步借用的合法快照 ID，删除前仅检查目标为普通文件。
 * @return 成功为 CC_OK，非法参数为 CC_ERR_INVALID_ARGUMENT，缺失为 CC_ERR_NOT_FOUND，类型错误为 CC_ERR_STORAGE，
 * 分配、文件系统或同步错误向上传递，删除后同步失败时文件可能已删除。
 */
cc_result_t cc_memory_snapshot_delete(cc_memory_snapshot_store_t *store, const char *id);
/**
 * @brief 释放列表中前 count 个 ID 及列表数组。
 * @param[in,out] items 拥有的快照列表，count 为零时允许 NULL。
 * @param[in] count 有效且拥有 ID 的元素数，须与数组范围一致。
 * @post ID 和数组均已释放，调用者持有的指针失效。
 */
void cc_memory_snapshot_info_free_array(cc_memory_snapshot_info_t *items, size_t count);

#endif
