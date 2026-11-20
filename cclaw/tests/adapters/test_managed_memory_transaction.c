#include "cc/adapters/cc_json_memory_store.h"
#include "cc/adapters/cc_managed_memory_editor.h"
#include "cc/core/cc_id.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief 检查单次断言，失败报告行号并结束本测试进程。
 * @param[in] x 只求值一次的条件，失败跳过余下场景，由进程退出结束 fixture 寿命。
 */
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)

/** @brief 检查并消费拥有结果，失败输出诊断。
 * @param[in] rc 按值移入的结果，消息在本层释放。
 * @return 成功为一，失败为零，无动态结果留给调用者。
 */
static int ok(cc_result_t rc)
{
    int good = rc.code == CC_OK;
    if (!good) fprintf(stderr, "%s\n", rc.message ? rc.message : "error");
    cc_result_free(&rc);
    return good;
}
typedef struct fault_fs { cc_filesystem_t *base; int fail_active_write; } fault_fs_t;

/** @brief 同步转发整文件读取。
 * @param[in] s 存活 fault_fs_t 借用，base 覆盖整个测试。
 * @param[in] p 同步路径借用。
 * @param[out] v 必填新文本拥有槽，成功由上层按文件端口契约释放。
 * @return 原端口拥有结果，诊断交接给调用者，无额外缓存。
 */
static cc_result_t rd(void *s, const char *p, char **v)
{
    fault_fs_t *f = s;
    return f->base->vtable->read_text(f->base->self, p, v);
}

/** @brief 在 active 临时文件写入点注入磁盘满错误，其余原样转发。
 * @param[in] s 存活故障状态借用，开关只由本测试主线程修改。
 * @param[in] p 同步路径借用，匹配 memory.json.tmp 时可拒绝。
 * @param[in] v 同步 NUL 正文借用，底层调用返回后归还借用。
 * @return 注入 IO 或真实写入结果，诊断由上层消费，输入始终归上层拥有。
 */
static cc_result_t wr(void *s, const char *p, const char *v)
{
    fault_fs_t *f = s;
    if (f->fail_active_write && strstr(p, "memory.json.tmp"))
        return cc_result_error(CC_ERR_IO, "injected full disk");
    return f->base->vtable->write_text(f->base->self, p, v);
}

/** @brief 同步转发目录列举。
 * @param[in] s 存活故障状态借用。
 * @param[in] p 同步目录路径借用。
 * @param[out] v 必填独占字符串数组输出，由上层逐项及容器释放。
 * @param[out] n 必填元素数输出。
 * @return 原端口拥有结果，无额外持有和并发修改。
 */
static cc_result_t ls(void *s, const char *p, char ***v, size_t *n)
{
    fault_fs_t *f = s;
    return f->base->vtable->list_dir(f->base->self, p, v, n);
}

/** @brief 同步转发文件属性读取。
 * @param[in] s 存活故障状态借用。
 * @param[in] p 同步路径借用。
 * @param[out] v 必填按值属性槽，由原端口填写。
 * @return 原端口拥有结果，属性槽不包含本函数拥有资源。
 */
static cc_result_t st(void *s, const char *p, cc_filesystem_stat_t *v)
{
    fault_fs_t *f = s;
    return f->base->vtable->stat(f->base->self, p, v);
}

/** @brief 同步转发路径删除。
 * @param[in] s 存活故障状态借用。
 * @param[in] p 同步 NUL 路径借用，内容归调用者。
 * @return 原端口拥有结果，文件系统改变由原端口执行。
 */
static cc_result_t rm(void *s, const char *p)
{
    fault_fs_t *f = s;
    return f->base->vtable->remove(f->base->self, p);
}

/** @brief 同步转发原子替换，保持源和目标参数顺序。
 * @param[in] s 存活故障状态借用。
 * @param[in] a 同步源路径借用。
 * @param[in] b 同步目标路径借用。
 * @return 原端口拥有结果，持久提交状态按底层契约解释。
 */
static cc_result_t replace(void *s, const char *a, const char *b)
{
    fault_fs_t *f = s;
    return f->base->vtable->atomic_replace(f->base->self, a, b);
}

/** @brief 同步转发文件持久化屏障。
 * @param[in] s 存活故障状态借用。
 * @param[in] p 同步文件路径借用。
 * @return 原端口拥有结果，原生文件句柄由底层管理。
 */
static cc_result_t sync_file(void *s, const char *p)
{
    fault_fs_t *f = s;
    return f->base->vtable->sync_file(f->base->self, p);
}

/** @brief 同步转发目录持久化屏障。
 * @param[in] s 存活故障状态借用。
 * @param[in] p 同步目录路径借用。
 * @return 原端口拥有结果，底层分配和句柄仍由底层消费。
 */
static cc_result_t sync_dir(void *s, const char *p)
{
    fault_fs_t *f = s;
    return f->base->vtable->sync_dir(f->base->self, p);
}

/** @brief 构造供 plan_build 同步深拷贝的借用记录。
 * @param[in] id 稳定 ID 借用，保持至 plan_build 返回。
 * @param[in] text 稳定正文借用，保持至 plan_build 返回。
 * @param[in] importance 重要性值，本 fixture 使用合法百分制值。
 * @param[in] updated 时间戳数值。
 * @return 按值记录，字符串均借用本次输入或字面量，调用者只清理生成后的拥有 plan。
 */
static cc_memory_plan_record_t record(char *id, char *text, unsigned importance, int64_t updated)
{
    cc_memory_plan_record_t r = {0};
    r.size = sizeof(r);
    r.id = id;
    r.text = text;
    r.category = "fact";
    r.evidence = "user evidence";
    r.source_session = "s1";
    r.importance = importance;
    r.updated_at = updated;
    return r;
}

/** @brief 核对 revision/epoch、失败后快照复用、重开恢复和编辑器重置事务。
 * @return 全部通过为零，任意 CHECK 失败报告行号并以一结束测试。
 * @note 单线程保管 fs/fault/vtable 到 store、snapshot 和 editor 全部销毁，成功路径清理文件及所有副本。
 * 借用 epoch 在编辑器销毁前使用，跨重建验证用独立复制的 old_epoch。
 */
int main(void)
{
    cc_filesystem_t fs={0}; CHECK(ok(cc_filesystem_get_default(&fs)));
    char *root=NULL; CHECK(ok(cc_id_generate_uuid_v4("managed-memory-",&root)));
    char history[160], active[160]; snprintf(history,sizeof(history),"%s/history",root); snprintf(active,sizeof(active),"%s/memory.json",root);
    CHECK(ok(fs.vtable->make_dir(fs.self,history)));
    fault_fs_t fault={&fs,0}; cc_filesystem_vtable_t vt={0}; vt.size=sizeof(vt);
    vt.read_text=rd; vt.write_text=wr; vt.list_dir=ls; vt.stat=st; vt.remove=rm;
    vt.atomic_replace=replace; vt.sync_file=sync_file; vt.sync_dir=sync_dir;
    cc_filesystem_t wrapped=fs; wrapped.self=&fault; wrapped.vtable=&vt;
    cc_memory_store_t store={0}; CHECK(ok(cc_memory_store_create_json_file_with_filesystem(&store,active,&wrapped)));
    CHECK(ok(cc_memory_store_set(&store,"summary","short term","context_summary","s1")));
    cc_memory_snapshot_store_t *snapshots=NULL;
    CHECK(ok(cc_memory_snapshot_store_open(&wrapped,history,active,&snapshots)));
    cc_memory_plan_t state={0}, plan={0}; uint64_t revision=0;
    CHECK(ok(cc_json_memory_managed_list(&store,&state,&revision)) && state.count==0); cc_memory_plan_cleanup(&state);
    cc_memory_plan_change_t add={0}; add.kind=CC_MEMORY_CHANGE_ADD; add.record=record("m1","first",70,100);
    CHECK(ok(cc_memory_plan_build(NULL,0,&add,1,8,NULL,NULL,&plan)));
    char *snapshot_id=NULL; int installed=0;
    CHECK(ok(cc_json_memory_managed_apply(&store,revision,&plan,NULL,0,NULL,&snapshot_id,&installed)) && installed && !snapshot_id);
    cc_memory_plan_cleanup(&plan);
    cc_memory_entry_t entry={0}; CHECK(ok(cc_memory_store_get(&store,"m1",&entry)) && strcmp(entry.value,"first")==0); cc_memory_entry_free(&entry);
    cc_result_t rc=cc_memory_store_set(&store,"m1","bypass",NULL,NULL); CHECK(rc.code==CC_ERR_PERMISSION_DENIED); cc_result_free(&rc);
    CHECK(ok(cc_json_memory_managed_list(&store,&state,&revision)) && state.count==1 && state.records[0].importance==70);
    cc_memory_plan_change_t update={0}; update.kind=CC_MEMORY_CHANGE_UPDATE; update.record=record("m1","updated",90,200);
    CHECK(ok(cc_memory_plan_build(state.records,state.count,&update,1,8,NULL,NULL,&plan)));
    cc_memory_plan_cleanup(&state);
    /* 另一次写入先改变 revision，使旧计划在创建快照和提交前被拒绝。 */
    CHECK(ok(cc_memory_store_set(&store,"other","kept","legacy","s1")));
    rc=cc_json_memory_managed_apply(&store,revision,&plan,snapshots,CC_MEMORY_SNAPSHOT_REWRITE,NULL,&snapshot_id,&installed);
    CHECK(rc.code==CC_ERR_INVALID_STATE && !snapshot_id && !installed); cc_result_free(&rc); cc_memory_plan_cleanup(&plan);
    CHECK(ok(cc_json_memory_managed_list(&store,&state,&revision)) && state.count==1);
    CHECK(ok(cc_memory_plan_build(state.records,state.count,&update,1,8,NULL,NULL,&plan))); cc_memory_plan_cleanup(&state);
    fault.fail_active_write=1;
    rc=cc_json_memory_managed_apply(&store,revision,&plan,snapshots,CC_MEMORY_SNAPSHOT_REWRITE,NULL,&snapshot_id,&installed);
    CHECK(rc.code==CC_ERR_IO && snapshot_id && !installed); cc_result_free(&rc);
    CHECK(ok(cc_memory_store_get(&store,"m1",&entry)) && strcmp(entry.value,"first")==0); cc_memory_entry_free(&entry);
    fault.fail_active_write=0; char *reused=NULL;
    CHECK(ok(cc_json_memory_managed_apply(&store,revision,&plan,snapshots,CC_MEMORY_SNAPSHOT_REWRITE,snapshot_id,&reused,&installed)) && installed);
    CHECK(reused && strcmp(reused,snapshot_id)==0); free(reused); free(snapshot_id); cc_memory_plan_cleanup(&plan);
    cc_memory_snapshot_info_t *versions=NULL; size_t count=0;
    CHECK(ok(cc_memory_snapshot_list(snapshots,&versions,&count)) && count==1);
    char *version_id=malloc(strlen(versions[0].id)+1U); CHECK(version_id); strcpy(version_id,versions[0].id);
    char *old=NULL; CHECK(ok(cc_memory_snapshot_read(snapshots,version_id,&old)) &&
        strstr(old,"\"value\":\"first\"") && !strstr(old,"\"value\":\"updated\"")); free(old);
    cc_memory_snapshot_info_free_array(versions,count);
    cc_memory_store_destroy(&store);
    CHECK(ok(cc_memory_store_create_json_file_with_filesystem(&store,active,&wrapped)));
    CHECK(ok(cc_json_memory_managed_list(&store,&state,&revision)) && state.count==1 &&
          strcmp(state.records[0].text,"updated")==0 && state.records[0].importance==90 &&
          strcmp(state.records[0].evidence,"user evidence")==0);
    CHECK(ok(cc_memory_store_get(&store,"summary",&entry)) && strcmp(entry.value,"short term")==0); cc_memory_entry_free(&entry);
    add.record=record("m2","remove me",30,201);
    CHECK(ok(cc_memory_plan_build(state.records,state.count,&add,1,8,NULL,NULL,&plan)));
    cc_memory_plan_cleanup(&state);
    CHECK(ok(cc_json_memory_managed_apply(&store,revision,&plan,NULL,0,NULL,&snapshot_id,&installed)) && installed);
    cc_memory_plan_cleanup(&plan);
    cc_managed_memory_editor_t *editor=NULL;
    CHECK(ok(cc_managed_memory_editor_create(&store,snapshots,&editor)));
    const char *epoch=cc_managed_memory_editor_epoch(editor); CHECK(epoch);
    char *old_epoch=malloc(strlen(epoch)+1U); CHECK(old_epoch); strcpy(old_epoch,epoch);
    CHECK(ok(cc_managed_memory_editor_list(editor,&state,&revision)) && state.count==2);
    cc_memory_plan_cleanup(&state);
    rc=cc_managed_memory_editor_update(editor,"stale-epoch",revision,"m1","user-corrected","fact",80,250,&installed);
    CHECK(rc.code==CC_ERR_INVALID_STATE && !installed); cc_result_free(&rc);
    CHECK(ok(cc_managed_memory_editor_update(editor,epoch,revision,"m1","user-corrected","fact",80,250,&installed)) && installed);
    rc=cc_managed_memory_editor_delete(editor,epoch,revision,"m2",&installed);
    CHECK(rc.code==CC_ERR_INVALID_STATE && !installed); cc_result_free(&rc);
    CHECK(ok(cc_managed_memory_editor_list(editor,&state,&revision)) && state.count==2);
    size_t m1=strcmp(state.records[0].id,"m1")==0 ? 0U : 1U;
    CHECK(strcmp(state.records[m1].text,"user-corrected")==0 && !state.records[m1].evidence && !state.records[m1].source_session);
    cc_memory_plan_cleanup(&state);
    CHECK(ok(cc_managed_memory_editor_delete(editor,epoch,revision,"m2",&installed)) && installed);
    CHECK(ok(cc_memory_snapshot_list(snapshots,&versions,&count)) && count==1);
    cc_memory_snapshot_info_free_array(versions,count);
    cc_managed_memory_editor_destroy(editor); editor=NULL;
    CHECK(ok(cc_managed_memory_editor_create(&store,snapshots,&editor)));
    epoch=cc_managed_memory_editor_epoch(editor); CHECK(strcmp(old_epoch,epoch)!=0);
    CHECK(ok(cc_managed_memory_editor_list(editor,&state,&revision)) && state.count==1); cc_memory_plan_cleanup(&state);
    snapshot_id=NULL; rc=cc_managed_memory_editor_reset(editor,old_epoch,revision,NULL,&snapshot_id,&installed);
    CHECK(rc.code==CC_ERR_INVALID_STATE && !snapshot_id && !installed); cc_result_free(&rc); free(old_epoch);
    fault.fail_active_write=1;
    rc=cc_managed_memory_editor_reset(editor,epoch,revision,NULL,&snapshot_id,&installed);
    CHECK(rc.code==CC_ERR_IO && snapshot_id && !installed); cc_result_free(&rc);
    fault.fail_active_write=0; reused=NULL;
    CHECK(ok(cc_managed_memory_editor_reset(editor,epoch,revision,snapshot_id,&reused,&installed)) && installed && reused &&
          strcmp(reused,snapshot_id)==0);
    free(reused);
    CHECK(ok(cc_memory_snapshot_read(snapshots,snapshot_id,&old)) && strstr(old,"user-corrected")); free(old);
    cc_managed_memory_editor_destroy(editor);
    CHECK(ok(cc_json_memory_managed_list(&store,&state,&revision)) && state.count==0); cc_memory_plan_cleanup(&state);
    CHECK(ok(cc_memory_store_get(&store,"other",&entry)) && strcmp(entry.value,"kept")==0); cc_memory_entry_free(&entry);
    CHECK(ok(cc_memory_snapshot_delete(snapshots,version_id)) && ok(cc_memory_snapshot_delete(snapshots,snapshot_id)));
    free(version_id); free(snapshot_id);
    cc_memory_store_destroy(&store); cc_memory_snapshot_store_destroy(snapshots);
    CHECK(ok(fs.vtable->remove(fs.self,active)) && ok(fs.vtable->remove(fs.self,history)) && ok(fs.vtable->remove(fs.self,root)));
    fs.vtable->destroy(fs.self); free(root); return 0;
}
