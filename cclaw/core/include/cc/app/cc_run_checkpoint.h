#ifndef CC_RUN_CHECKPOINT_H
#define CC_RUN_CHECKPOINT_H

#include "cc/core/cc_tool_call.h"
#include "cc/ports/cc_filesystem.h"

/**
 * @brief 由单一 owner 独占目录、跨运行时代际共享的检查点存储。
 *
 * 实例 mutex 串行文件事务与进程内运行声明，目录和借用 fs 的寿命由外层保证。
 * 文件系统回调期间须避免重入同一实例。销毁前关闭入口、排空调用并释放全部声明。
 */
typedef struct cc_run_checkpoint_store cc_run_checkpoint_store_t;

typedef enum cc_run_checkpoint_phase {
    CC_RUN_CHECKPOINT_MODEL = 1, /**< 等待模型，计划列表、游标和重试等待 ID 须为空。 */
    CC_RUN_CHECKPOINT_TOOLS = 2 /**< 执行已保存计划，保留下一调用游标及可选旧调用核对等待。 */
} cc_run_checkpoint_phase_t;

/**
 * @brief 持久化运行计划及本次载入来源。
 *
 * load 返回的字符串和列表均拥有，使用 cleanup 释放，save 输入在调用期间借用。
 * 调用 ID 和名称各最多 512 字节，参数 JSON 原文各最多 64 KiB，所有字节数均不计 NUL。
 */
typedef struct cc_run_checkpoint {
    char *run_id; /**< run- 加小写 UUID 外形的 40 字节 ID，载入结果拥有此副本。 */
    char *session_id; /**< 非空会话 ID，最多 512 正文字节。 */
    char *user_message_id; /**< 非空原输入消息 ID，最多 512 正文字节。 */
    char *user_input; /**< 非空原输入，最多 64 KiB 正文字节。 */
    char *reasoning_content; /**< 可选推理正文，最多 64 KiB，保存 NULL 时编码为空字符串。 */
    unsigned step; /**< 从一开始的模型步骤号。 */
    cc_run_checkpoint_phase_t phase; /**< MODEL 或 TOOLS，约束计划列表及游标组合。 */
    cc_tool_call_list_t planned_calls; /**< 最多八项的拥有调用列表，ID 唯一，参数原文须为完整 JSON 对象。 */
    size_t next_call; /**< 下一计划项零基索引，可等于列表数量表示已全部完成。 */
    int recovered_from_backup; /**< 仅本次载入状态，主文件缺失且使用备份时为真。 */
    char *retry_after_call_id; /**< 最多 512 字节的非空旧调用 ID，等待其账本清除后再分派替代调用，NULL 表示无等待。 */
    int recovered_from_temporary; /**< 仅本次载入状态，完整孤立临时记录须经用户确认后安装。 */
} cc_run_checkpoint_t;

/**
 * @brief 检查既有目录与文件系统能力，创建检查点存储实例。
 * @param[in] fs 非空文件系统借用，须存活至实例销毁，并提供原子替换、文件同步、stat 及相关读写目录入口。
 * @param[in] directory 非空既有目录，路径复制，目录在实例寿命内由外层独占。
 * @param[out] out_store 必填，先置 NULL，成功取得拥有句柄，使用 store_destroy 释放。
 * @return 成功返回 CC_OK，输出缺失返回 CC_ERR_INVALID_ARGUMENT，能力或目录参数不符返回 CC_ERR_UNSUPPORTED，
 * 目录类型不符返回 CC_ERR_STORAGE，其他 stat、分配及互斥量错误向上传递。
 * 打开只核对目录，记录读取和完整性检查在 load/list 中进行。
 */
cc_result_t cc_run_checkpoint_store_open(cc_filesystem_t *fs, const char *directory,
                                         cc_run_checkpoint_store_t **out_store);
/**
 * @brief 释放实例拥有的声明、互斥量、路径副本及实例。
 * @pre owner 已关闭入口、排空调用及文件系统回调，并释放全部活动声明，fs 仍有效。
 * @param[in,out] store 待销毁对象，可为 NULL。
 * @post 实例寿命结束，磁盘记录及外层文件系统句柄继续由各自 owner 管理。
 */
void cc_run_checkpoint_store_destroy(cc_run_checkpoint_store_t *store);
/**
 * @brief 释放拥有检查点的所有字符串和调用列表，并清零结构。
 * @param[in,out] checkpoint 拥有结果，可为 NULL，借用输入结构须由其原 owner 按实际拥有关系处理。
 * @post 非空结构的拥有字段已释放且全零，可再次接收 load 输出。
 */
void cc_run_checkpoint_cleanup(cc_run_checkpoint_t *checkpoint);
/**
 * @brief 从调用者指定的 run UUID 派生稳定原输入 ID。
 * @param[in] run_id 合法 40 字节运行 ID，调用期间借用。
 * @param[out] out_id 必填，先置 NULL，成功取得 run_id 加 -user 的拥有副本，调用者 free。
 * @return 成功返回 CC_OK，参数或 ID 外形错误返回 CC_ERR_INVALID_ARGUMENT，分配失败返回 CC_ERR_OUT_OF_MEMORY。
 */
cc_result_t cc_run_checkpoint_derive_user_message_id(const char *run_id, char **out_id);

/**
 * @brief 在进程内为运行和会话登记互斥声明，供多个 runtime generation 共享准入。
 * @param[in,out] store 非空存储，锁内检查及插入声明。
 * @param[in] run_id 合法运行 ID，复制进声明，可对应尚未创建的磁盘检查点。
 * @param[in] session_id 非空会话 ID，最多 512 字节，复制进声明。
 * @return 成功返回 CC_OK，参数错误返回 CC_ERR_INVALID_ARGUMENT，任一 ID 或会话已有声明返回 CC_ERR_INVALID_STATE，分配失败返回内存错误。
 * 声明只在 RAM 中存在，调用者在运行停止后 release，并协调 save/remove 的运行归属。
 */
cc_result_t cc_run_checkpoint_claim(cc_run_checkpoint_store_t *store,
                                     const char *run_id, const char *session_id);
/**
 * @brief 移除并释放指定运行的进程内声明。
 * @param[in,out] store 非空声明所属存储。
 * @param[in] run_id 合法借用运行 ID，调用者保证对应运行已停止访问其受保护状态。
 * @return 成功返回 CC_OK，声明缺失返回 CC_ERR_NOT_FOUND，参数非法返回 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t cc_run_checkpoint_release(cc_run_checkpoint_store_t *store, const char *run_id);

/**
 * @brief 同步保存完整计划到临时文件，核对后替换正式检查点并同步目录。
 * @pre 外层通过声明或独占初始化协调目标运行，输入在编码及写入期间保持稳定。
 * @param[in,out] store 非空存储，同一实例文件事务由 mutex 串行。
 * @param[in] checkpoint 借用输入，满足阶段、游标、字段字节预算及调用 ID 唯一性，所有权保持在调用者。
 * @return 成功返回 CC_OK，字段错误返回 CC_ERR_INVALID_ARGUMENT，分配、512 KiB 编码上限或文件系统错误向上传递。
 * 保存错误后调用者停止推进运行，正式文件可能是旧版或已安装新版，失败临时清理也可能失败。
 */
cc_result_t cc_run_checkpoint_save(cc_run_checkpoint_store_t *store,
                                    const cc_run_checkpoint_t *checkpoint);
/**
 * @brief 优先读取正式记录，仅缺失时依次尝试备份与孤立临时记录。
 * @param[in] store 非空存储，读取和解码在实例 mutex 下完成。
 * @param[in] run_id 合法借用运行 ID，载入内容须与文件 ID 一致。
 * @param[out] out_checkpoint 必填独立空输出，参数有效后清零，成功取得拥有字段，使用 cleanup 释放。
 * @return 成功返回 CC_OK，全部候选缺失返回 CC_ERR_NOT_FOUND，损坏或解析失败返回 CC_ERR_STORAGE，复制分配及文件系统错误向上传递。
 * 当前解析分配失败与语法失败仍折叠为存储错误，此码本身不能证明明确完整性损坏。
 * 当前候选存在但损坏或读取失败时立即返回，来源标志仅在成功输出中记录。
 * 字段按区分大小写的首个匹配项读取，额外字段和重复键沿用 cJSON 查找规则。
 */
cc_result_t cc_run_checkpoint_load(cc_run_checkpoint_store_t *store, const char *run_id,
                                    cc_run_checkpoint_t *out_checkpoint);
/**
 * @brief 用户确认后安装完整孤立临时记录，保留原计划内容。
 * @pre 用户已核对临时输入，外层保持原目录和文件系统有效。
 * @param[in,out] store 非空存储，锁内读取、核对活动声明并执行安装。
 * @param[in] run_id 合法借用运行 ID。
 * @return 成功返回 CC_OK，存在正式或备份来源、或相同运行/会话正在活动时返回 CC_ERR_INVALID_STATE，其他返回读取或安装错误。
 * 替换或目录同步失败时正式文件可能已经安装，调用者重新读取实际来源后核对。
 */
cc_result_t cc_run_checkpoint_confirm_temporary(cc_run_checkpoint_store_t *store,
                                                const char *run_id);
/**
 * @brief 枚举三种后缀的检查点，按运行 ID 去重、完整载入核对后排序输出。
 * @param[in] store 非空存储，目录枚举和记录检查持有实例 mutex。
 * @param[out] out_run_ids 必填独立输出，参数有效后置 NULL，成功拥有每条 ID 及数组，逐项 free 后 free 数组。
 * @param[out] out_count 必填输出，参数有效后置零，成功填写数量，零条时数组也可能已分配。
 * @return 成功返回 CC_OK，任一候选文件名或内容错误返回 CC_ERR_STORAGE，其他目录、读取或分配错误向上传递。
 * 失败清理本次列表并保持空输出，列表只说明检查点存在，任务是否已有最终答案由上层核对。
 */
cc_result_t cc_run_checkpoint_list(cc_run_checkpoint_store_t *store,
                                   char ***out_run_ids, size_t *out_count);
/**
 * @brief 按临时、备份、正式记录的顺序删除指定运行的持久检查点。
 * @pre 最终答复已可靠提交或用户已决定放弃，调用者持有相应声明或独占目标运行。
 * @param[in,out] store 非空存储，删除与目录同步持有实例 mutex。
 * @param[in] run_id 合法借用运行 ID。
 * @return 缺失文件按成功处理，全部删除及同步成功返回 CC_OK，其他参数、路径分配或文件系统错误向上传递。
 * 失败可能已删除部分文件，进程内声明继续由调用者单独 release。
 */
cc_result_t cc_run_checkpoint_remove(cc_run_checkpoint_store_t *store, const char *run_id);

#endif
