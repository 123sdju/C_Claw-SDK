#include "cc/adapters/cc_managed_memory_editor.h"
#include "cc/core/cc_id.h"

#include <stdlib.h>
#include <string.h>

struct cc_managed_memory_editor {
    cc_memory_store_t *store;                /* 借用，整个实例寿命内有效。 */
    cc_memory_snapshot_store_t *snapshots;   /* 借用，仅重置提交使用。 */
    char *epoch;                            /* 本实例拥有的堆字符串。 */
};

/**
 * @brief 先分配实例及带 memory-editor- 前缀的 UUID，再保存借用依赖。
 * @param[in] store 借用至实例销毁的存储，创建时只检查非空。
 * @param[in] snapshots 借用至实例销毁的快照存储，创建时只检查非空。
 * @param[out] out_editor 必填，先置 NULL，成功后接收拥有的实例。
 * @return CC_OK 表示创建成功，空参数、分配或 UUID 生成错误向上传递。
 */
cc_result_t cc_managed_memory_editor_create(cc_memory_store_t *store,
    cc_memory_snapshot_store_t *snapshots, cc_managed_memory_editor_t **out_editor)
{
    if (!out_editor) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory editor output");
    *out_editor=NULL;
    if (!store || !snapshots) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory editor dependencies");
    cc_managed_memory_editor_t *editor=calloc(1,sizeof(*editor));
    if (!editor) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot allocate memory editor");
    cc_result_t rc=cc_id_generate_uuid_v4("memory-editor-",&editor->epoch);
    if (rc.code!=CC_OK) { free(editor); return rc; }
    editor->store=store; editor->snapshots=snapshots; *out_editor=editor;
    return cc_result_ok();
}

/**
 * @brief 释放 epoch 后释放实例。
 * @pre 编辑调用已退出，借用的 store/snapshots 仍存活。
 * @param[in,out] editor 待释放实例，允许 NULL。
 * @post 实例 epoch 与 editor 已释放，借用依赖继续由外层管理。
 */
void cc_managed_memory_editor_destroy(cc_managed_memory_editor_t *editor)
{
    if (!editor) return;
    free(editor->epoch); free(editor);
}

/**
 * @brief 借出实例 epoch 至编辑器销毁前，用于识别跨实例的陈旧编辑请求。
 * @param[in] editor 借用的实例，允许 NULL。
 * @return 本实例 epoch 的借用字符串，editor 为 NULL 时返回 NULL。
 */
const char *cc_managed_memory_editor_epoch(const cc_managed_memory_editor_t *editor)
{ return editor ? editor->epoch : NULL; }

/**
 * @brief 只核对借用 store 的对象身份，进一步可写性在修改路径检查。
 * @param[in] editor 借用的实例，允许 NULL。
 * @param[in] store 要比较的存储指针，允许 NULL。
 * @return editor 借用同一 store 对象时为一，缺失或不匹配时为零。
 */
int cc_managed_memory_editor_uses_store(const cc_managed_memory_editor_t *editor, const cc_memory_store_t *store)
{ return editor && store && editor->store==store; }

/**
 * @brief 将列表复制和版本读取委托给 JSON store，允许读取写入阻断期间的内存集合。
 * @param[in] editor 借用的编辑器。
 * @param[out] out_state 必填且无待释放内容，成功输出拥有的计划，用 cc_memory_plan_cleanup 清理。
 * @param[out] out_revision 必填，成功输出与计划同时读取的版本。
 * @return 空实例返回 CC_ERR_INVALID_ARGUMENT，其余返回 JSON store 的列表结果。
 */
cc_result_t cc_managed_memory_editor_list(cc_managed_memory_editor_t *editor,
    cc_memory_plan_t *out_state, uint64_t *out_revision)
{
    if (!editor) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory editor");
    return cc_json_memory_managed_list(editor->store,out_state,out_revision);
}

/**
 * @brief 校验实例 epoch、存储可写性及列表 revision，返回拥有的当前记录集合。
 *
 * 读取前后检查可写状态，陈旧版本或存储阻断时清理已取得的计划。
 * @param[in] editor 借用的编辑器及其存储。
 * @param[in] epoch 读取列表时所属 editor 实例的身份字符串。
 * @param[in] revision 读取列表时取得的受管理集合版本。
 * @param[out] out 调用者提供的空计划，成功取得拥有副本，调用者用 cc_memory_plan_cleanup 清理。
 * @return CC_OK 表示实例、可写性和 revision 均通过核对，冲突、存储或读取错误向上传递。
 */
static cc_result_t current_state(cc_managed_memory_editor_t *editor, const char *epoch,
                                 uint64_t revision, cc_memory_plan_t *out)
{
    if (!editor || !epoch || strcmp(editor->epoch,epoch)!=0)
        return cc_result_error(CC_ERR_INVALID_STATE,"Memory editor changed; refresh the list");
    int ready=0;
    cc_result_t rc=cc_json_memory_write_ready(editor->store,&ready);
    if (rc.code!=CC_OK) return rc;
    if (!ready) return cc_result_error(CC_ERR_STORAGE,"Memory storage needs recovery before further writes");
    uint64_t actual=0;
    rc=cc_json_memory_managed_list(editor->store,out,&actual);
    if (rc.code==CC_OK && actual!=revision) {
        cc_memory_plan_cleanup(out);
        return cc_result_error(CC_ERR_INVALID_STATE,"Memory changed; refresh the list");
    }
    if (rc.code==CC_OK) {
        rc=cc_json_memory_write_ready(editor->store,&ready);
        if (rc.code!=CC_OK || !ready) {
            cc_memory_plan_cleanup(out);
            if (rc.code==CC_OK) return cc_result_error(CC_ERR_STORAGE,"Memory storage needs recovery before further writes");
        }
    }
    return rc;
}

/**
 * @brief 以 epoch/revision 定位现有受管理记录并构造单项更新/删除方案。
 *
 * 正文或类别变化时清除原模型证据，普通修改使用无快照提交。
 * 提交时 JSON store 再核对 revision，临时计划全部在本函数清理。
 * 无变更时跳过提交。out_installed 为零的写入错误仍可能已经安装，须结合存储写入阻断
 * 恢复并核对内容。实例同步由调用者协调，存储分别锁定读取和提交，提交再次验证版本。
 * @param[in,out] editor 借用的编辑器及其存储。
 * @param[in] epoch 读取列表时所属 editor 实例的身份字符串。
 * @param[in] revision 读取列表时取得的受管理集合版本。
 * @param[in] id 目标记忆的稳定 ID，调用期间借用。
 * @param[in] text 更新时借用的非空正文，删除时忽略。
 * @param[in] category 更新时借用的非空类别，删除时忽略。
 * @param[in] importance 更新时采用的 0 到 100 重要度，删除时忽略。
 * @param[in] updated_at 更新时采用的非负时间戳，删除时忽略。
 * @param[in] deleting 非零构造删除变更，零构造更新变更。
 * @param[out] out_installed 必填，先置零，提交观察到安装时置一。
 * @return CC_OK 表示变更已提交或内容相同，目标缺失返回 CC_ERR_NOT_FOUND，参数、版本、规划和存储错误向上传递。
 */
static cc_result_t apply_change(cc_managed_memory_editor_t *editor, const char *epoch,
                                uint64_t revision, const char *id, const char *text,
                                const char *category, unsigned importance, int64_t updated_at,
                                int deleting, int *out_installed)
{
    if (!out_installed) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory update output");
    *out_installed=0;
    if (!id || !id[0] || (!deleting && (!text || !text[0] || !category || !category[0] ||
                                        importance>100U || updated_at<0)))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory update");
    cc_memory_plan_t current={0}, plan={0};
    cc_result_t rc=current_state(editor,epoch,revision,&current);
    size_t index=0;
    if (rc.code==CC_OK) {
        while (index<current.count && strcmp(current.records[index].id,id)!=0) index++;
        if (index==current.count) rc=cc_result_error(CC_ERR_NOT_FOUND,"Memory to change was not found");
    }
    if (rc.code==CC_OK) {
        cc_memory_plan_change_t change={0};
        change.kind=deleting ? CC_MEMORY_CHANGE_DELETE : CC_MEMORY_CHANGE_UPDATE;
        change.record=current.records[index];
        if (!deleting) {
            if (strcmp(change.record.text,text)!=0 || strcmp(change.record.category,category)!=0) {
                change.record.evidence=NULL; change.record.source_session=NULL;
            }
            change.record.text=(char *)text; change.record.category=(char *)category;
            change.record.importance=importance; change.record.updated_at=updated_at;
        }
        rc=cc_memory_plan_build(current.records,current.count,&change,1,
                                current.count ? current.count : 1U,NULL,NULL,&plan);
    }
    if (rc.code==CC_OK && plan.changed_existing) {
        char *snapshot_id=NULL;
        rc=cc_json_memory_managed_apply(editor->store,revision,&plan,NULL,0,NULL,&snapshot_id,out_installed);
        free(snapshot_id);
    }
    cc_memory_plan_cleanup(&current); cc_memory_plan_cleanup(&plan); return rc;
}

/**
 * @brief 以更新模式调用 apply_change，正文/类别变化时清除原证据和来源会话。
 * @param[in,out] editor 借用的编辑器及其存储。
 * @param[in] epoch 读取列表时所属 editor 实例的身份字符串。
 * @param[in] revision 读取列表时取得的受管理集合版本。
 * @param[in] id 目标记忆的稳定 ID，调用期间借用。
 * @param[in] text 同步借用的非空新正文。
 * @param[in] category 更新后的记忆类别，调用期间借用。
 * @param[in] importance 更新后的重要度，范围为 0 到 100。
 * @param[in] updated_at 新的非负更新时间，提交时还校验存储支持的范围。
 * @param[out] out_installed 必填，先置零，观察到安装时置一，零值的写入错误须恢复核对。
 * @return 返回 apply_change 的校验、规划和提交结果，相同内容成功且安装标志为零。
 */
cc_result_t cc_managed_memory_editor_update(cc_managed_memory_editor_t *editor,
    const char *epoch, uint64_t revision, const char *id, const char *text,
    const char *category, unsigned importance, int64_t updated_at, int *out_installed)
{ return apply_change(editor,epoch,revision,id,text,category,importance,updated_at,0,out_installed); }

/**
 * @brief 以删除模式调用 apply_change，更新字段均传入空值。
 * @param[in,out] editor 借用的编辑器及其存储。
 * @param[in] epoch 读取列表时所属 editor 实例的身份字符串。
 * @param[in] revision 读取列表时取得的受管理集合版本。
 * @param[in] id 目标记忆的稳定 ID，调用期间借用。
 * @param[out] out_installed 必填，先置零，观察到安装时置一，零值的写入错误须恢复核对。
 * @return 返回 apply_change 的校验、查找、规划和提交结果。
 */
cc_result_t cc_managed_memory_editor_delete(cc_managed_memory_editor_t *editor,
    const char *epoch, uint64_t revision, const char *id, int *out_installed)
{ return apply_change(editor,epoch,revision,id,NULL,NULL,0,0,1,out_installed); }

/**
 * @brief 验证实例、可写性及版本，集合非空时将 RESET 快照和空计划交给存储提交。
 *
 * 输出快照 ID 由调用者 free，失败也可能有已创建 ID 供后续核对复用。
 * 空集合跳过快照与提交，待处理队列与重置操作的执行顺序由调用者协调。
 * @param[in,out] editor 借用的编辑器及其存储。
 * @param[in] epoch 读取列表时所属 editor 实例的身份字符串。
 * @param[in] revision 读取列表时取得的受管理集合版本。
 * @param[in] prior_snapshot_id 可选的既有重置快照 ID，供同版本重试确认。
 * @param[out] out_snapshot_id 必填，两个输出均有效时先置 NULL，失败也可能返回拥有的 ID，调用者 free。
 * @param[out] out_installed 必填，两个输出均有效时先置零，观察到安装时置一，零值的写入错误须恢复核对。
 * @return 空集合成功，其余返回状态校验或存储提交结果，错误时保留已输出快照的重试关联。
 */
cc_result_t cc_managed_memory_editor_reset(cc_managed_memory_editor_t *editor,
    const char *epoch, uint64_t revision, const char *prior_snapshot_id,
    char **out_snapshot_id, int *out_installed)
{
    if (!out_snapshot_id || !out_installed)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory reset output");
    *out_snapshot_id=NULL; *out_installed=0;
    cc_memory_plan_t current={0}, empty={0};
    cc_result_t rc=current_state(editor,epoch,revision,&current);
    if (rc.code==CC_OK && current.count)
        rc=cc_json_memory_managed_apply(editor->store,revision,&empty,editor->snapshots,
                                        CC_MEMORY_SNAPSHOT_RESET,prior_snapshot_id,
                                        out_snapshot_id,out_installed);
    cc_memory_plan_cleanup(&current); return rc;
}
