#ifndef CC_OPERATION_JOURNAL_H
#define CC_OPERATION_JOURNAL_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_tool_call.h"
#include "cc/ports/cc_filesystem.h"
#include <stddef.h>

/** 普通新操作的默认准入阈值，按待处理记录条数计。 */
#define CC_OPERATION_JOURNAL_DEFAULT_LIMIT 64U

/**
 * @brief 工具派发前的持久化操作日志，由一个目录所有者保管，可跨运行时 generation 共用。
 *
 * 实例借用 fs、复制目录路径并拥有记录及互斥锁。已打开实例的记录访问由锁保护，锁内文件系统
 * 回调禁止重入同一实例的记录读写或销毁入口。打开、销毁及目录的其他访问由外层协调。
 * 磁盘只保存操作身份和工具调用，进程内执行/人工核对标志不落盘，重新打开后全部作为 UNKNOWN。
 * UNKNOWN 及人工核对中的记录按工具名称阻断本日志的全部会话，普通新操作还受记录条数阈值限制。
 * 显式只读工具可使用只读入口越过条数阈值，已恢复和导入记录可超过阈值。
 * 当前工具执行器将未声明 READ_ONLY 的工具交给普通准入入口。工具能力声明、执行、结果持久化
 * 及日志损坏时的对话降级由上层装配处理。
 */
typedef struct cc_operation_journal cc_operation_journal_t;

/** 打开失败的证据分类，供上层决定是否进入全部工具禁用的纯对话装配。 */
typedef enum cc_operation_journal_failure {
    CC_OPERATION_JOURNAL_FAILURE_OTHER = 0, /**< 参数、能力、I/O、分配或无法证实为损坏的解析失败。 */
    CC_OPERATION_JOURNAL_FAILURE_INTEGRITY  /**< 已观察到名称、文件或记录完整性损坏。 */
} cc_operation_journal_failure_t;

typedef struct cc_pending_operation {
    char *operation_id;  /**< 拥有的 op-UUID 操作 ID，列表副本随数组释放。 */
    char *session_id;    /**< 拥有的原会话 ID。 */
    cc_tool_call_t call; /**< 拥有的调用 ID、名称及参数 JSON 文本。 */
    int in_progress;    /**< 本进程已准入且尚未结束的操作为一，为零表示需核对，重开时统一为零。 */
    int reconciling;    /**< 本进程的人工核对占用标志，为一时同名工具继续阻断，重开时归零。 */
} cc_pending_operation_t;

/**
 * @brief 从专用目录加载并校验全部 op-*.json 记录，创建操作日志。
 *
 * 匹配文件的名称、字段、长度、CRC32 或会话/调用 ID 唯一性异常使打开失败。
 * 已有记录全部保留，即使条数超过准入阈值。其他名称及 .tmp 文件跳过。
 * @param[in] fs 借用至日志销毁的文件系统及 vtable，须支持 stat、列举、文本读写、删除、文件同步和原子替换。
 * @param[in] directory 已存在的专用目录，非空路径在调用期间借用并复制，目录由外层独占保管。
 * @param[in] max_pending 普通新操作的条数准入阈值，零采用默认 64 条。
 * @param[out] out_journal 必填，先置 NULL，成功接收拥有的实例，调用者排空使用者后销毁。
 * @return 成功为 CC_OK，输出参数无效为 CC_ERR_INVALID_ARGUMENT，能力或路径不满足为 CC_ERR_UNSUPPORTED，
 * 损坏/重复记录为 CC_ERR_STORAGE，分配、文件系统和锁创建错误向上传递，失败清理全部已加载内存。
 */
cc_result_t cc_operation_journal_open(
    cc_filesystem_t *fs, const char *directory, size_t max_pending,
    cc_operation_journal_t **out_journal);
/**
 * @brief 打开日志并给出可证明的完整性损坏分类，其余行为与 open 相同。
 * @param[in] fs 借用至日志销毁的文件系统及 vtable。
 * @param[in] directory 同步借用并复制的既有专用目录路径。
 * @param[in] max_pending 普通新操作条数阈值，零使用默认 64 条。
 * @param[out] out_journal 必填，先置 NULL，成功取得拥有的日志，失败清理本次内存。
 * @param[out] out_failure 可选，先设 OTHER，仅明确损坏时设 INTEGRITY，成功时仍为 OTHER。
 * @return 沿用 open 的错误码，I/O 和无法区分的解析/分配失败保持 OTHER，原文件继续保留供修复后重新打开。
 */
cc_result_t cc_operation_journal_open_detailed(
    cc_filesystem_t *fs, const char *directory, size_t max_pending,
    cc_operation_journal_t **out_journal, cc_operation_journal_failure_t *out_failure);
/**
 * @brief 释放内存记录、锁、目录副本及日志实例。
 * @pre 日志调用者和文件系统回调已排空，借用 fs 仍存活。
 * @param[in,out] journal 待销毁实例，允许 NULL。
 * @post 实例寿命结束，持久化文件继续保留供重新打开。
 */
void cc_operation_journal_destroy(cc_operation_journal_t *journal);
/**
 * @brief 在工具派发前持久化意图并登记本进程正在执行的操作。
 *
 * 同一会话/调用 ID 已存在、任一会话有同名 UNKNOWN、或总条数达到阈值时拒绝新操作。
 * UUID 最多尝试八次以避开当前内存操作 ID，call.id 保留原值。
 * @param[in,out] journal 借用的日志，持锁覆盖检查、写入、同步、校验和登记。
 * @param[in] session_id 非空会话 ID，同步借用并复制。
 * @param[in] call 借用的调用，id/name 须非空，arguments_json 须为非空指针，可为空字符串，内容复制保存。
 * @param[out] out_operation_id 必填，存在时先置 NULL，成功输出拥有的操作 ID，用 free 释放。
 * @return 成功为 CC_OK，参数无效为 CC_ERR_INVALID_ARGUMENT，身份/同名阻断为 CC_ERR_INVALID_STATE，
 * 字段或条数超限为 CC_ERR_LIMIT_EXCEEDED，UUID、分配、文件系统错误向上传递。
 * @note 替换已尝试后的错误会保留内存 UNKNOWN 记录并保持输出 NULL，调用者须核对现场后恢复。
 */
cc_result_t cc_operation_journal_begin(
    cc_operation_journal_t *journal, const char *session_id,
    const cc_tool_call_t *call, char **out_operation_id);

/**
 * @brief 为可信的显式只读工具登记操作，允许越过普通条数准入阈值。
 * @pre 调用者已核实工具只读能力，工具执行器仅在注册能力含 READ_ONLY 时选择此入口。
 * @param[in,out] journal 借用的日志，同名 UNKNOWN 和相同会话/调用 ID 仍受检查。
 * @param[in] session_id 非空会话 ID，同步借用并复制。
 * @param[in] call 借用的调用，id/name 非空，arguments_json 指针有效，内容复制保存。
 * @param[out] out_operation_id 必填，存在时先置 NULL，成功取得拥有的 ID，用 free 释放。
 * @return 沿用 begin 的校验和持久化结果，省略条数阈值检查，失败后的 UNKNOWN 仍需核对。
 */
cc_result_t cc_operation_journal_begin_readonly(
    cc_operation_journal_t *journal, const char *session_id,
    const cc_tool_call_t *call, char **out_operation_id);

/**
 * @brief 将中断调用导入为 UNKNOWN，供上层恢复历史后人工核对。
 *
 * 导入越过同名 UNKNOWN 和条数阈值。重复的会话/调用 ID 须名称和参数逐字相同，且磁盘记录
 * 完整匹配并完成文件及支持的目录同步后才成功，重复成功保留现有进程标志。
 * @param[in,out] journal 借用的日志及文件系统，目录由外层独占。
 * @param[in] session_id 非空会话 ID，同步借用并复制。
 * @param[in] call 借用的调用，id/name 非空，arguments_json 指针有效，内容复制保存。
 * @param[out] out_operation_id 必填，存在时先置 NULL，成功取得新建或已核验记录的 ID 副本，用 free 释放。
 * @return 成功为 CC_OK，身份冲突为 CC_ERR_INVALID_STATE，磁盘记录缺失/损坏、分配和同步等错误向上传递，
 * 不确定安装仍保留 UNKNOWN，输出为空。缺失或损坏时须恢复目录现场并重新打开后核对。
 */
cc_result_t cc_operation_journal_import_unknown(
    cc_operation_journal_t *journal, const char *session_id,
    const cc_tool_call_t *call, char **out_operation_id);

/**
 * @brief 将记录的 in_progress 清零，使其进入待人工核对状态。
 * @param[in,out] journal 借用的日志，锁内只更新内存标志，保留 reconciling 值。
 * @param[in] operation_id 同步借用的操作 ID，须非 NULL。
 * @return 成功为 CC_OK，参数无效为 CC_ERR_INVALID_ARGUMENT，ID 缺失为 CC_ERR_NOT_FOUND。
 */
cc_result_t cc_operation_journal_mark_unknown(
    cc_operation_journal_t *journal, const char *operation_id);

/**
 * @brief 独占申领一个 UNKNOWN 的人工核对过程，工具名称继续阻断。
 * @param[in,out] journal 借用的日志，锁内检查并置 reconciling 为一，申领仅本进程有效。
 * @param[in] operation_id 同步借用的操作 ID，须非 NULL。
 * @return 成功为 CC_OK，参数无效为 CC_ERR_INVALID_ARGUMENT，缺失为 CC_ERR_NOT_FOUND，正在执行或已申领为 CC_ERR_INVALID_STATE。
 */
cc_result_t cc_operation_journal_claim_reconciliation(
    cc_operation_journal_t *journal, const char *operation_id);
/**
 * @brief 清除人工核对申领标志，待处理记录继续保留。
 * @param[in,out] journal 借用的日志，锁内置 reconciling 为零，可重复释放已存在记录。
 * @param[in] operation_id 同步借用的操作 ID，须非 NULL，调用者负责申领使用顺序。
 * @return 成功为 CC_OK，参数无效为 CC_ERR_INVALID_ARGUMENT，ID 缺失为 CC_ERR_NOT_FOUND。
 */
cc_result_t cc_operation_journal_release_reconciliation(
    cc_operation_journal_t *journal, const char *operation_id);

/**
 * @brief 删除操作记录并同步目录，成功后释放内存项。
 * @pre 结果已持久化到其他存储，或用户已完成结果核对，执行及申领顺序由调用者保证。
 * @param[in,out] journal 借用的日志及文件系统，持锁覆盖删除和同步。
 * @param[in] operation_id 同步借用的操作 ID，须非 NULL。
 * @return 成功为 CC_OK，参数无效为 CC_ERR_INVALID_ARGUMENT，内存 ID 缺失为 CC_ERR_NOT_FOUND，
 * 分配、删除或同步错误向上传递。磁盘已缺失时允许重新同步后完成，删除/同步失败保留内存项并清零 in_progress，路径分配失败保持原状态。
 */
cc_result_t cc_operation_journal_finish(
    cc_operation_journal_t *journal, const char *operation_id);

/**
 * @brief 跨会话检查同名工具是否存在 UNKNOWN 或人工核对中的记录。
 * @param[in] journal 借用的日志，锁内查询。
 * @param[in] tool_name 同步借用的工具名称，须非 NULL，按字节匹配。
 * @param[out] out_blocked 必填，参数有效后置零，找到 in_progress 为零的同名项时置一。
 * @return 参数有效返回 CC_OK，否则返回 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t cc_operation_journal_is_blocked(
    cc_operation_journal_t *journal, const char *tool_name, int *out_blocked);
/**
 * @brief 查找一个会话中首个匹配的 UNKNOWN，人工核对中的记录也参与匹配。
 * @param[in] journal 借用的日志，锁内查找和复制。
 * @param[in] session_id 同步借用的会话 ID，须非 NULL。
 * @param[in] tool_call_id 可选调用 ID，NULL 匹配该会话任意 UNKNOWN，返回首项的顺序由内存数组决定。
 * @param[out] out_call 可选且无待释放内容，参数有效后清零，成功副本用 cc_tool_call_cleanup 清理。
 * @param[out] out_operation_id 可选输出，参数有效后置 NULL，成功副本用 free 释放。
 * @param[out] out_found 必填，参数有效后置零，完成所请求的副本后置一。
 * @return 找到或未找到均为 CC_OK，参数无效为 CC_ERR_INVALID_ARGUMENT，复制错误向上传递并清理部分输出。
 */
cc_result_t cc_operation_journal_find_unknown(
    cc_operation_journal_t *journal, const char *session_id, const char *tool_call_id,
    cc_tool_call_t *out_call, char **out_operation_id, int *out_found);
/**
 * @brief 检查任意进程状态的会话/调用身份，并核对名称及参数完全一致。
 * @param[in] journal 借用的日志，锁内查询。
 * @param[in] session_id 同步借用的会话 ID，须非 NULL。
 * @param[in] call 借用的调用，id/name/arguments_json 指针均有效。
 * @param[out] out_found 必填，参数有效后置零，身份及内容完全匹配时置一。
 * @return 匹配或不存在为 CC_OK，同一会话/调用 ID 对应不同名称或参数为 CC_ERR_STORAGE，参数无效为 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t cc_operation_journal_contains(
    cc_operation_journal_t *journal, const char *session_id,
    const cc_tool_call_t *call, int *out_found);
/**
 * @brief 在同一次加锁期间深拷贝全部记录及其进程标志。
 * @param[in] journal 借用的日志，当前内存顺序允许因 finish 移尾而改变。
 * @param[out] out_items 必填且无待释放内容，参数有效后置 NULL，成功取得拥有数组，用 cc_pending_operation_array_free 清理。
 * @param[out] out_count 必填，参数有效后置零，成功输出条数，空日志返回 NULL 和零。
 * @return 成功为 CC_OK，参数无效为 CC_ERR_INVALID_ARGUMENT，分配或复制错误向上传递，失败清理部分列表。
 */
cc_result_t cc_operation_journal_list(
    cc_operation_journal_t *journal, cc_pending_operation_t **out_items,
    size_t *out_count);
/**
 * @brief 释放操作数组中每条记录的拥有字段及数组。
 * @param[in,out] items 拥有的数组，允许 NULL，非空时前 count 条须为有效或零初始化记录。
 * @param[in] count 需清理的记录条数，须在数组范围内。
 * @post 非空数组及其全部拥有字段已释放，调用者持有的指针失效。
 */
void cc_pending_operation_array_free(cc_pending_operation_t *items, size_t count);

#endif
