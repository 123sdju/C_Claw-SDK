#include "cc/app/cc_memory_plan.h"
#include "cc/internal/cc_alloc.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 释放一条记录拥有的五个字符串并清零整个记录。
 * @param[in,out] record 拥有字符串的记录或零记录，允许 NULL。
 * @post 非空记录恢复为全零，其字符串指针全部失效。
 */
static void record_cleanup(cc_memory_plan_record_t *record)
{
    if (!record) return;
    free(record->id); free(record->text); free(record->category);
    free(record->evidence); free(record->source_session);
    memset(record, 0, sizeof(*record));
}

/**
 * @brief 按 count 清理有效记录，再释放数组并清零计划与计数。
 * @param[in,out] plan 拥有记录数组的计划或零计划，允许 NULL，调用者独占清理期间的使用权。
 * @post 非空计划恢复为全零，可再次清理或用于构建输出。
 */
void cc_memory_plan_cleanup(cc_memory_plan_t *plan)
{
    if (!plan) return;
    for (size_t i = 0; i < plan->count; i++) record_cleanup(&plan->records[i]);
    free(plan->records);
    memset(plan, 0, sizeof(*plan));
}

/**
 * @brief 检查记录尺寸及 ID，完整模式再检查正文、类别、重要度和时间。
 * @param[in] record 同步借用的记录，允许 NULL。
 * @param[in] id_only 非零只检查 size 和非空 ID，供 DELETE 使用。
 * @return 通过相应检查为一，否则为零，证据与来源会话允许 NULL。
 */
static int valid_record(const cc_memory_plan_record_t *record, int id_only)
{
    if (!record || record->size < sizeof(*record) || !record->id || !record->id[0]) return 0;
    if (id_only) return 1;
    return record->text && record->text[0] && record->category && record->category[0] &&
        record->importance <= 100U && record->updated_at >= 0;
}

/**
 * @brief 深拷贝必需及可选字符串，保留重要度和更新时间。
 * @pre source 是已校验的完整记录，source 与 out 独立。
 * @param[in] source 同步借用的源记录及其字符串。
 * @param[out] out 必填且无待释放内容，先清零，成功取得拥有副本。
 * @return 成功为 CC_OK，任一分配失败时清理本次副本并返回 CC_ERR_OUT_OF_MEMORY。
 */
static cc_result_t record_copy(const cc_memory_plan_record_t *source, cc_memory_plan_record_t *out)
{
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    out->id = cc_copy_string(source->id);
    out->text = cc_copy_string(source->text);
    out->category = cc_copy_string(source->category);
    out->evidence = source->evidence ? cc_copy_string(source->evidence) : NULL;
    out->source_session = source->source_session ? cc_copy_string(source->source_session) : NULL;
    out->importance = source->importance;
    out->updated_at = source->updated_at;
    if (!out->id || !out->text || !out->category ||
        (source->evidence && !out->evidence) || (source->source_session && !out->source_session)) {
        record_cleanup(out);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy memory plan record");
    }
    return cc_result_ok();
}

/**
 * @brief 线性查找与 ID 字节相同的首条记录。
 * @param[in] records 借用的记录数组，前 count 条须有有效 ID，count 为零时允许 NULL。
 * @param[in] count 本次搜索的条数。
 * @param[in] id 同步借用的非空指针字符串。
 * @return 找到时返回零基索引，未找到返回 count。
 */
static size_t find_id(const cc_memory_plan_record_t *records, size_t count, const char *id)
{
    for (size_t i = 0; i < count; i++) if (strcmp(records[i].id, id) == 0) return i;
    return count;
}

/**
 * @brief 按可选字符串的存在性和字节内容判断相同。
 * @param[in] a 同步借用的字符串，允许 NULL。
 * @param[in] b 同步借用的字符串，允许 NULL。
 * @return 同为 NULL 或均存在且内容相同时为一，其余为零，NULL 与空字符串有别。
 */
static int same_string(const char *a, const char *b)
{
    return (!a && !b) || (a && b && strcmp(a, b) == 0);
}

/**
 * @brief 比较五个字符串及重要度、更新时间，供最终原记录差异计数使用。
 * @param[in] a 必填的借用记录。
 * @param[in] b 必填的借用记录。
 * @return 所有业务字段相同为一，否则为零，结构 size 不参与比较。
 */
static int same_record(const cc_memory_plan_record_t *a, const cc_memory_plan_record_t *b)
{
    return same_string(a->id, b->id) && same_string(a->text, b->text) &&
        same_string(a->category, b->category) && same_string(a->evidence, b->evidence) &&
        same_string(a->source_session, b->source_session) && a->importance == b->importance &&
        a->updated_at == b->updated_at;
}

/**
 * @brief 释放目标记录并前移尾部记录，保持剩余顺序和数组拥有关系。
 * @pre 调用者独占 plan，且 index 小于 plan->count。
 * @param[in,out] plan 本次构建拥有的计划，数组分配容量保持原值。
 * @param[in] index 待删除项的零基索引。
 * @post count 减一，原末尾槽清零，差异及淘汰计数由调用方更新。
 */
static void erase_record(cc_memory_plan_t *plan, size_t index)
{
    record_cleanup(&plan->records[index]);
    if (index + 1U < plan->count)
        memmove(&plan->records[index], &plan->records[index + 1U],
                (plan->count - index - 1U) * sizeof(*plan->records));
    plan->count--;
    memset(&plan->records[plan->count], 0, sizeof(*plan->records));
}

/**
 * @brief 先按最坏条数分配数组，再复制原集合、顺序变更、容量淘汰并计算最终差异。
 *
 * 原记录与变更输入同步借用。每次复制独立拥有字符串，UPDATE 先完整复制再释放旧项，
 * 所有阶段失败统一清理输出。容量淘汰的原/新分类按 ID 是否属于原集合决定。
 * 末次比较使同一原 ID 的多次变更至多计一次，变化后恢复原值的记录计为未变化。
 * @param[in] existing 原集合，数量为零时允许 NULL，记录及字符串在调用期间保持稳定。
 * @param[in] existing_count 原记录条数。
 * @param[in] changes 有序变更，数量为零时允许 NULL，DELETE 仅需有效 size 和 ID。
 * @param[in] change_count 变更条数。
 * @param[in] capacity 最终条数上限，须大于零。
 * @param[in] select_eviction 超限时同步调用的可选策略，每次选择一项，须遵守外层持锁约束。
 * @param[in] user_data 原样借给淘汰策略的上下文，允许 NULL。
 * @param[out] out_plan 必填且无待释放内容，初始参数合法后清零，成功拥有数组及字符串。
 * @return 成功为 CC_OK，非法参数/记录/ID/索引为 CC_ERR_INVALID_ARGUMENT，目标缺失为 CC_ERR_NOT_FOUND，
 * 规模溢出或超限缺策略为 CC_ERR_LIMIT_EXCEEDED，分配失败为 CC_ERR_OUT_OF_MEMORY，策略错误原样返回。
 */
cc_result_t cc_memory_plan_build(
    const cc_memory_plan_record_t *existing, size_t existing_count,
    const cc_memory_plan_change_t *changes, size_t change_count,
    size_t capacity, cc_memory_eviction_select_fn select_eviction, void *user_data,
    cc_memory_plan_t *out_plan)
{
    if (!out_plan || (!existing && existing_count) || (!changes && change_count) || !capacity)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory plan request");
    memset(out_plan, 0, sizeof(*out_plan));
    if (existing_count > SIZE_MAX - change_count ||
        existing_count + change_count > SIZE_MAX / sizeof(*out_plan->records))
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory plan is too large");
    size_t maximum = existing_count + change_count;
    if (maximum) {
        out_plan->records = calloc(maximum, sizeof(*out_plan->records));
        if (!out_plan->records) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate memory plan");
    }
    cc_result_t rc = cc_result_ok();
    for (size_t i = 0; rc.code == CC_OK && i < existing_count; i++) {
        if (!valid_record(&existing[i], 0) || find_id(existing, i, existing[i].id) != i) {
            rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Existing memory record is invalid or duplicated");
            break;
        }
        rc = record_copy(&existing[i], &out_plan->records[out_plan->count]);
        if (rc.code == CC_OK) out_plan->count++;
    }
    for (size_t i = 0; rc.code == CC_OK && i < change_count; i++) {
        const cc_memory_plan_change_t *change = &changes[i];
        if ((change->kind != CC_MEMORY_CHANGE_ADD && change->kind != CC_MEMORY_CHANGE_UPDATE &&
             change->kind != CC_MEMORY_CHANGE_DELETE) ||
            !valid_record(&change->record, change->kind == CC_MEMORY_CHANGE_DELETE)) {
            rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Memory change is invalid");
            break;
        }
        size_t index = find_id(out_plan->records, out_plan->count, change->record.id);
        if (change->kind == CC_MEMORY_CHANGE_ADD) {
            if (index != out_plan->count) {
                rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "New memory id already exists");
                break;
            }
            int duplicate = 0;
            for (size_t j = 0; j < out_plan->count; j++) {
                if (strcmp(out_plan->records[j].text, change->record.text) == 0 &&
                    strcmp(out_plan->records[j].category, change->record.category) == 0) {
                    duplicate = 1;
                    break;
                }
            }
            if (!duplicate) {
                rc = record_copy(&change->record, &out_plan->records[out_plan->count]);
                if (rc.code == CC_OK) out_plan->count++;
            }
        } else if (index == out_plan->count) {
            rc = cc_result_error(CC_ERR_NOT_FOUND, "Memory to change was not found");
        } else if (change->kind == CC_MEMORY_CHANGE_DELETE) {
            erase_record(out_plan, index);
        } else {
            cc_memory_plan_record_t replacement = {0};
            rc = record_copy(&change->record, &replacement);
            if (rc.code == CC_OK) {
                record_cleanup(&out_plan->records[index]);
                out_plan->records[index] = replacement;
            }
        }
    }
    while (rc.code == CC_OK && out_plan->count > capacity) {
        if (!select_eviction) {
            rc = cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory capacity requires an eviction policy");
            break;
        }
        size_t index = SIZE_MAX;
        rc = select_eviction(out_plan->records, out_plan->count, user_data, &index);
        if (rc.code != CC_OK) break;
        if (index >= out_plan->count) {
            rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Memory eviction selection is invalid");
            break;
        }
        if (find_id(existing, existing_count, out_plan->records[index].id) < existing_count)
            out_plan->evicted_existing++;
        else out_plan->evicted_new++;
        erase_record(out_plan, index);
    }
    if (rc.code == CC_OK) {
        for (size_t i = 0; i < existing_count; i++) {
            size_t index = find_id(out_plan->records, out_plan->count, existing[i].id);
            if (index == out_plan->count || !same_record(&existing[i], &out_plan->records[index]))
                out_plan->changed_existing++;
            if (index == out_plan->count ||
                !same_string(existing[i].text, out_plan->records[index].text) ||
                !same_string(existing[i].category, out_plan->records[index].category))
                out_plan->rewritten_existing++;
        }
        for (size_t i = 0; i < out_plan->count; i++)
            if (find_id(existing, existing_count, out_plan->records[i].id) == existing_count) out_plan->added++;
    } else {
        cc_memory_plan_cleanup(out_plan);
    }
    return rc;
}
