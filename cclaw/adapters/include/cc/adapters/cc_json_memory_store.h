#ifndef CC_JSON_MEMORY_STORE_H
#define CC_JSON_MEMORY_STORE_H

#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_memory_store.h"
#include "cc/app/cc_memory_plan.h"
#include "cc/app/cc_memory_snapshot.h"

/**
 * @brief 使用默认文件系统打开整文件 JSON 记忆存储。
 *
 * 主文件缺失且无 .tmp/.bak 时创建空的内存状态，首次写入才生成文件。存在合法主文件和旁路文件时
 * 可读取主文件，但写入被阻断。主文件损坏、不可读或仅剩旁路文件时创建失败，已有文件留待恢复。
 * @param[out] out_store 无已有资源的输出句柄，out_store 和 file_path 非 NULL 后先清零，成功后调用 cc_memory_store_destroy 释放。
 * @param[in] file_path 主文件路径，调用期间借用并复制，按原字符串保存。
 * @return 成功为 CC_OK，NULL 参数为 CC_ERR_INVALID_ARGUMENT，能力不足为 CC_ERR_UNSUPPORTED，
 * 分配失败为 CC_ERR_OUT_OF_MEMORY，文件超限为 CC_ERR_LIMIT_EXCEEDED，文件结构或恢复状态异常为 CC_ERR_STORAGE。
 * 默认文件系统、锁及 JSON 解析错误原样返回。
 * @note 普通操作由存储锁保护，销毁和原位恢复需调用者排空使用者。加载、查询和写入会分配整文件或记录副本，
 * CC_JSON_MEMORY_MAX_FILE_BYTES 约束文件字节数，实际峰值内存还包含旧数组、事务副本、JSON 树与序列化及回读缓冲。
 * 普通 set 要求非空 key，空 key 返回 CC_ERR_INVALID_ARGUMENT，校验发生在加锁及修改状态之前。
 * 加载时拒绝会解码为 NUL 的转义，返回 CC_ERR_STORAGE 并保留原文件及旁路证据，避免字段被 C 字符串截断。
 */
cc_result_t cc_memory_store_create_json_file(cc_memory_store_t *out_store, const char *file_path);

/**
 * @brief 使用调用者提供的文件系统打开 JSON 记忆存储。
 * @param[out] out_store 无已有资源的输出句柄，必需参数通过检查后清零，成功后由调用者销毁。
 * @param[in] file_path 调用期间借用并复制的主文件路径。
 * @param[in] filesystem 必需的文件系统描述，结构复制，self/vtable 继续借用且须覆盖存储生命周期。
 * 至少提供版本 3 的 stat、文本读写、原子替换与文件同步，声明目录同步能力时还须提供 sync_dir。
 * @return NULL filesystem 返回 CC_ERR_INVALID_ARGUMENT，其他结果与默认文件系统创建入口一致。
 * @note 主文件与旁路文件的加载规则、锁和内存开销见 cc_memory_store_create_json_file。
 */
cc_result_t cc_memory_store_create_json_file_with_filesystem(
    cc_memory_store_t *out_store, const char *file_path, const cc_filesystem_t *filesystem);

/**
 * @brief 在同一次存储锁下读取受管理记忆及进程内修订号。
 * @param[in] store 本适配器创建的有效存储，写入被阻断时仍可读取已有内存状态。
 * @param[out] out_state 无已有资源的输出结构，参数合法后清零，成功取得拥有副本，使用 cc_memory_plan_cleanup 释放。
 * @param[out] out_revision 参数合法后先置零，仅成功时填写当前修订号，供 managed_apply 乐观并发核对。
 * @return 成功为 CC_OK，参数或适配器不符为 CC_ERR_INVALID_ARGUMENT，元数据缺失对应项为 CC_ERR_STORAGE，
 * 分配失败为 CC_ERR_OUT_OF_MEMORY，方案校验错误原样返回。
 */
cc_result_t cc_json_memory_managed_list(
    cc_memory_store_t *store, cc_memory_plan_t *out_state, uint64_t *out_revision);
/**
 * @brief 查询内存中的写入阻断标志。
 * @param[in] store 本适配器创建的有效存储。
 * @param[out] out_ready 必需输出，参数合法后在锁内填写，未阻断为一，需恢复为零。
 * @return 成功查询为 CC_OK，参数或适配器不符为 CC_ERR_INVALID_ARGUMENT，此时输出保持原值。
 * @note 本次查询读取缓存标志，磁盘外部变更和新出现的 .tmp/.bak 由加载或写入路径检查。
 * 可读性与写入许可分别判断，阻断后已有内存记录仍可查询。
 */
cc_result_t cc_json_memory_write_ready(cc_memory_store_t *store, int *out_ready);
/**
 * @brief 核对适配器类型及创建时保存的路径字符串。
 * @param[in] store 待核对存储，可为 NULL。
 * @param[in] path 借用的路径字符串，可为 NULL，直接按字节比较。
 * @return 类型、句柄和路径均匹配为一，其他情况为零。路径别名和大小写差异依字符串比较结果处理。
 */
int cc_json_memory_store_matches_path(cc_memory_store_t *store, const char *path);
/**
 * @brief 在锁内启用普通检索只返回受管理记忆的过滤规则。
 * @param[in,out] store 本适配器创建的有效存储，修改该实例的内存配置。
 * @return 成功为 CC_OK，参数或适配器不符为 CC_ERR_INVALID_ARGUMENT。
 * @note search/list 和普通 query 应用此过滤。显式 category=context_summary 的结构化 query 可查询派生摘要，
 * get 仍按精确 key 读取任意记录。启用过滤只更新内存标志，磁盘上的其他记录继续保留。
 */
cc_result_t cc_json_memory_restrict_recall_to_managed(cc_memory_store_t *store);
/**
 * @brief 配置派生 context_summary 缓存上限，并按新限制裁剪已有派生摘要。
 * @pre 调用者已排空运行中的生成及直接存储使用者，配置期间保持该屏障。
 * @param[in,out] store 本适配器创建的有效存储，配置在锁内更新，裁剪失败时仍保留新配置。
 * @param[in] per_session 每个会话最多保留的派生摘要条数，必须大于零，NULL 会话单独成组。
 * @param[in] max_entries 全部会话合计的派生摘要条数上限，必须大于零。
 * @param[in] max_value_bytes 所有保留派生摘要正文的 UTF-8 字节总上限，必须大于零，不含 NUL。
 * @return 成功配置及裁剪为 CC_OK，非法参数为 CC_ERR_INVALID_ARGUMENT，分配失败为 CC_ERR_OUT_OF_MEMORY，
 * 容量或修订号溢出为 CC_ERR_LIMIT_EXCEEDED，写入阻断为 CC_ERR_STORAGE，持久化错误原样返回。
 * @note 由数组尾部向前保留摘要，更新的摘要移到末尾。裁剪只涉及未受管理的 context_summary。
 * 需要裁剪时执行一次整文件替换，已安装但后续同步失败时返回错误，内存保留已安装结果并阻断后续写入。
 */
cc_result_t cc_json_memory_set_summary_cache_limits(
    cc_memory_store_t *store, size_t per_session, size_t max_entries, size_t max_value_bytes);
/**
 * @brief 重新加载被写入阻断的存储，在同一外部句柄中安装新实例。
 * @pre 调用者已排空所有读写者，包括 worker、运行中的生成及直接借用者，直至本次调用返回。
 * @param[in,out] store 本适配器创建的有效句柄，成功替换 self 并释放旧实例，失败保留旧实例与阻断状态。
 * @return 成功为 CC_OK，参数或适配器不符为 CC_ERR_INVALID_ARGUMENT，当前无需恢复为 CC_ERR_INVALID_STATE，
 * 修订号溢出为 CC_ERR_LIMIT_EXCEEDED，主文件缺失或仍有 .tmp/.bak 为 CC_ERR_STORAGE，加载及资源错误原样返回。
 * @note 成功保留过滤和摘要限制配置，修订号在旧值上加一。对象内部锁只读取恢复前提，排空并发访问依赖外层屏障。
 */
cc_result_t cc_json_memory_recover_in_place(cc_memory_store_t *store);

/**
 * @brief 校验方案并以整文件事务替换全部受管理记忆，保留其他记录和派生摘要。
 * @param[in,out] store 本适配器创建的有效存储，锁内核对修订号、快照和替换结果。
 * @param[in] expected_revision managed_list 返回的进程内修订号，须与当前值一致。
 * @param[in] plan 借用的完整目标集合，函数验证并复制，ID 须与未受管理记录互不冲突，分类须为长期记忆分类。
 * @param[in,out] snapshots reason 非零时必需，借用且须绑定相同路径，reason 为零时须为 NULL。
 * @param[in] reason 零表示跳过快照，或 CC_MEMORY_SNAPSHOT_REWRITE、CC_MEMORY_SNAPSHOT_RESET。
 * @param[in] prior_snapshot_id 可选借用的已有快照 ID，仅提供 snapshots 时可用，前缀须匹配 reason，使用前重新确认。
 * @param[out] out_snapshot_id 必需的无已有资源输出槽，参数合法后置 NULL。创建或复用快照时取得拥有 ID，后续失败也可能保留，调用者 free。
 * @param[out] out_installed 必需输出，参数合法后置零。确认目标文件已可见时为一，即使后续同步或清理失败。
 * @return 完整成功为 CC_OK，非法参数、目标或快照不符为 CC_ERR_INVALID_ARGUMENT，修订号冲突为 CC_ERR_INVALID_STATE，
 * 容量或修订号溢出为 CC_ERR_LIMIT_EXCEEDED，分配失败为 CC_ERR_OUT_OF_MEMORY，写入阻断为 CC_ERR_STORAGE，快照与文件系统错误原样返回。
 * @note 已安装时内存采用新集合且修订号加一。安装前失败恢复旧集合，安装结果未知时保留旧集合并阻断后续写入，需恢复后核对。
 * 初始空存储且主文件缺失时无需创建快照，out_snapshot_id 可保持 NULL。
 */
cc_result_t cc_json_memory_managed_apply(
    cc_memory_store_t *store, uint64_t expected_revision, const cc_memory_plan_t *plan,
    cc_memory_snapshot_store_t *snapshots, cc_memory_snapshot_reason_t reason,
    const char *prior_snapshot_id, char **out_snapshot_id, int *out_installed);

#endif
