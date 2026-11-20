#ifndef CC_MEMORY_PLAN_H
#define CC_MEMORY_PLAN_H

#include "cc/core/cc_result.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @brief 长期记忆及其证据来源。
 *
 * build 的输入记录及字符串在同步调用期间借用，输出计划深拷贝并拥有各字符串。
 */
typedef struct cc_memory_plan_record {
    size_t size;          /**< 结构字节数，输入须至少为 sizeof(cc_memory_plan_record_t)。 */
    char *id;             /**< 非空稳定 ID，按字节比较。 */
    char *text;           /**< 非空正文，DELETE 时忽略。 */
    char *category;       /**< 非空类别，DELETE 时忽略。 */
    char *evidence;       /**< 可选证据字符串，允许 NULL。 */
    char *source_session; /**< 可选来源会话，允许 NULL。 */
    unsigned importance;  /**< 重要度，完整记录范围为 0 到 100。 */
    int64_t updated_at;   /**< 非负更新时间，沿用调用者的时间戳单位，存储层可另行限制范围。 */
} cc_memory_plan_record_t;

typedef enum cc_memory_change_kind {
    CC_MEMORY_CHANGE_ADD = 1,    /**< 新增 ID，正文和类别均相同的候选会跳过。 */
    CC_MEMORY_CHANGE_UPDATE = 2, /**< 按 ID 用完整记录替换当前候选。 */
    CC_MEMORY_CHANGE_DELETE = 3  /**< 按 ID 删除当前候选。 */
} cc_memory_change_kind_t;

typedef struct cc_memory_plan_change {
    cc_memory_change_kind_t kind;   /**< 按数组顺序执行的变更种类。 */
    cc_memory_plan_record_t record; /**< DELETE 只校验 size 和非空 id，其他种类校验完整记录。 */
} cc_memory_plan_change_t;

/**
 * @brief 从已应用全部变更的当前候选集合中选择一条淘汰记录。
 * @param[in] records 仅回调期间借用的完整候选数组，包含仍保留的新记录，回调保持字段及字符串内容不变。
 * @param[in] count 当前候选条数，大于容量时才调用，因而大于零。
 * @param[in] user_data build 调用者提供的借用上下文。
 * @param[out] out_index 必填，成功须输出小于 count 的零基索引。
 * @return CC_OK 表示已选择，其他错误由 build 原样返回并清理整个输出计划。
 * @note build 自身同步调用回调，worker 等外层可持处理锁，回调须遵守调用方的重入约束。
 */
typedef cc_result_t (*cc_memory_eviction_select_fn)(
    const cc_memory_plan_record_t *records, size_t count, void *user_data, size_t *out_index);

typedef struct cc_memory_plan {
    cc_memory_plan_record_t *records; /**< 拥有的最终记录数组及字符串，用 cc_memory_plan_cleanup 释放。 */
    size_t count;                    /**< 最终记录条数，成功时不超过 capacity。 */
    size_t changed_existing;         /**< 与最终集合相比已移除或任意业务字段变化的原记录数，每个原 ID 至多计一次，size 不参与比较。 */
    size_t added;                    /**< 最终集合中 ID 未出现在原集合的记录数。 */
    size_t evicted_existing;         /**< 容量淘汰阶段删除的、ID 属于原集合的候选数。 */
    size_t evicted_new;              /**< 容量淘汰阶段删除的、ID 未属于原集合的候选数。 */
    size_t rewritten_existing;       /**< 最终已移除或正文/类别改变的原记录数，元数据单独变化不计入。 */
} cc_memory_plan_t;

/**
 * @brief 深拷贝原集合，顺序应用变更，再按容量淘汰并计算最终差异。
 *
 * 原集合须完整且 ID 唯一。ADD 的 ID 与当前候选冲突时报错，ID 合法但正文和类别均与
 * 当前某条候选相同则跳过新增。UPDATE 完整替换目标，证据来源等字段由调用者决定。
 * 变更按顺序作用于当前候选，因此后续变更可引用此前新增的 ID。差异计数以最终状态为准。
 * 计划构建只处理内存，输入在调用期间保持稳定，输出拥有独立副本。
 * @param[in] existing 原集合，existing_count 为零时允许 NULL，同步借用。
 * @param[in] existing_count 原记录条数。
 * @param[in] changes 有序变更数组，change_count 为零时允许 NULL，同步借用。
 * @param[in] change_count 变更条数。
 * @param[in] capacity 最终条数上限，须大于零。
 * @param[in] select_eviction 可选淘汰回调，只有变更后超出容量时才必需。
 * @param[in] user_data 同步借用并原样传给淘汰回调的上下文，允许 NULL。
 * @param[out] out_plan 必填且无待释放内容，初始参数校验通过后清零，成功输出须用 cc_memory_plan_cleanup 清理。
 * @return 成功为 CC_OK，参数、记录、重复 ID 或淘汰索引非法为 CC_ERR_INVALID_ARGUMENT，
 * UPDATE/DELETE 目标缺失为 CC_ERR_NOT_FOUND，规模溢出或缺少必需淘汰策略为 CC_ERR_LIMIT_EXCEEDED，
 * 分配失败为 CC_ERR_OUT_OF_MEMORY，回调错误原样返回，构建中失败清空输出。
 */
cc_result_t cc_memory_plan_build(
    const cc_memory_plan_record_t *existing, size_t existing_count,
    const cc_memory_plan_change_t *changes, size_t change_count,
    size_t capacity, cc_memory_eviction_select_fn select_eviction, void *user_data,
    cc_memory_plan_t *out_plan);
/**
 * @brief 释放计划的全部记录、字符串及数组并清零结构。
 * @param[in,out] plan 拥有有效记录数组的计划或零初始化计划，允许 NULL。
 * @post 非空计划恢复为全零，可重复清理。
 */
void cc_memory_plan_cleanup(cc_memory_plan_t *plan);

#endif
