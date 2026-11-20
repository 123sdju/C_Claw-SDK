#include "cc/adapters/cc_managed_memory_worker.h"
#include "cc/core/cc_id.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)
/**
 * @brief 判断测试操作是否成功并消费结果拥有的错误正文。
 * @param[in] rc 按值传入的结果，错误正文释放责任在本函数完成。
 * @return CC_OK 时为一，其他状态打印诊断后返回零。
 */
static int ok(cc_result_t rc) { int good=rc.code==CC_OK; if (!good) fprintf(stderr,"%s\n",rc.message ? rc.message : "error"); cc_result_free(&rc); return good; }

typedef struct fault_fs {
    cc_filesystem_t *base;
    const char *root;
    int fail_active_write;
    int fail_active_sync;
    int fail_active_replace_after_install;
    int hide_active_reads;
} fault_fs_t;

/**
 * @brief 读取夹具文件，故障开启时拒绝 memory.json 主文件回读。
 * @param[in] s 借用的 fault_fs_t 夹具，底层 filesystem 保持有效。
 * @param[in] p 借用路径，临时文件和历史版本按底层接口读取。
 * @param[out] v 底层成功返回的拥有文本，注入失败时置为 NULL。
 * @return 注入时为 CC_ERR_IO，其余返回底层读取结果。
 */
static cc_result_t rd(void *s, const char *p, char **v)
{
    fault_fs_t *f = s;
    const char *name = strrchr(p, '/');
    if (f->hide_active_reads && strcmp(name ? name + 1 : p, "memory.json") == 0) {
        *v = NULL;
        return cc_result_error(CC_ERR_IO, "injected unavailable primary readback");
    }
    return f->base->vtable->read_text(f->base->self, p, v);
}
/**
 * @brief 在主记忆临时文件写入前模拟磁盘已满。
 * @param[in] s 借用的文件系统故障夹具。
 * @param[in] p 写入路径，包含 memory.json.tmp 时受 fail_active_write 控制。
 * @param[in] v 要写入的借用文本。
 * @return 故障命中时返回 CC_ERR_IO，其余返回底层写入结果。
 */
static cc_result_t wr(void *s,const char *p,const char *v) { fault_fs_t *f=s; if (f->fail_active_write && strstr(p,"memory.json.tmp")) return cc_result_error(CC_ERR_IO,"injected full disk"); return f->base->vtable->write_text(f->base->self,p,v); }
/**
 * @brief 把目录枚举委托给夹具底层文件系统。
 * @param[in] s 借用的故障夹具。
 * @param[in] p 要枚举的借用目录路径。
 * @param[out] v 成功取得的拥有名称数组，按 filesystem 接口清理。
 * @param[out] n 成功取得的名称数量。
 * @return 底层 list_dir 的结果。
 */
static cc_result_t ls(void *s,const char *p,char ***v,size_t *n) { fault_fs_t *f=s; return f->base->vtable->list_dir(f->base->self,p,v,n); }
/**
 * @brief 把文件状态查询委托给底层文件系统。
 * @param[in] s 借用的故障夹具。
 * @param[in] p 待查询路径。
 * @param[in,out] v 调用方准备的状态结构，底层填写类型和长度等字段。
 * @return 底层 stat 的结果。
 */
static cc_result_t st(void *s,const char *p,cc_filesystem_stat_t *v) { fault_fs_t *f=s; return f->base->vtable->stat(f->base->self,p,v); }
/**
 * @brief 委托底层删除测试文件或目录。
 * @param[in] s 借用的故障夹具。
 * @param[in] p 要删除的路径。
 * @return 底层 remove 的结果。
 */
static cc_result_t rm(void *s,const char *p) { fault_fs_t *f=s; return f->base->vtable->remove(f->base->self,p); }
/**
 * @brief 先实际替换主文件，再模拟安装返回值和后续回读同时丢失。
 * @param[in,out] s 故障夹具，命中后消费一次性替换标志并开启主文件读故障。
 * @param[in] a 已写好的临时文件路径。
 * @param[in] b 目标路径，仅 memory.json 安装受本项故障控制。
 * @return 底层失败原样返回，安装后故障返回 CC_ERR_IO，其他情况返回成功。
 */
static cc_result_t replace(void *s, const char *a, const char *b)
{
    fault_fs_t *f = s;
    cc_result_t rc = f->base->vtable->atomic_replace(f->base->self, a, b);
    const char *name = strrchr(b, '/');
    if (rc.code == CC_OK && f->fail_active_replace_after_install &&
        strcmp(name ? name + 1 : b, "memory.json") == 0) {
        cc_result_free(&rc);
        f->fail_active_replace_after_install = 0;
        f->hide_active_reads = 1;
        return cc_result_error(CC_ERR_IO, "injected lost install result");
    }
    return rc;
}
/**
 * @brief 委托底层同步测试文件。
 * @param[in] s 借用的故障夹具。
 * @param[in] p 待同步文件路径。
 * @return 底层 sync_file 的结果。
 */
static cc_result_t sync_file(void *s,const char *p) { fault_fs_t *f=s; return f->base->vtable->sync_file(f->base->self,p); }
/**
 * @brief 在活动记忆所属目录同步时模拟安装后的持久化失败。
 * @param[in] s 借用的故障夹具，读取 fail_active_sync 与根路径。
 * @param[in] p 待同步目录路径。
 * @return 故障命中时返回 CC_ERR_IO，其余返回底层 sync_dir 的结果。
 */
static cc_result_t sync_dir(void *s,const char *p) { fault_fs_t *f=s; if (f->fail_active_sync && strcmp(p,f->root)==0) return cc_result_error(CC_ERR_IO,"injected sync failure"); return f->base->vtable->sync_dir(f->base->self,p); }

typedef struct fake_llm { const char *response; int calls; char session_id[64]; } fake_llm_t;
/**
 * @brief 返回预设提取响应并记录调用次数、session 和固定用量。
 * @param[in,out] self 借用的 fake_llm_t，递增 calls 并保存请求会话。
 * @param[in] request worker 发出的同步请求，session_id 可为空。
 * @param[in,out] out 调用者初始化的响应，填入用量并复制预设文本，按响应契约清理。
 * @return cc_llm_response_set_text 的复制结果。
 */
static cc_result_t fake_chat(void *self,const cc_llm_chat_request_t *request,cc_llm_response_t *out)
{ fake_llm_t *fake=self; fake->calls++; snprintf(fake->session_id,sizeof(fake->session_id),"%s",request->session_id ? request->session_id : ""); out->usage.valid=1; out->usage.prompt_tokens=23; return cc_llm_response_set_text(out,fake->response); }
static const cc_llm_provider_vtable_t provider_vt={fake_chat,NULL,NULL,NULL};
/**
 * @brief 为本夹具选择最后一条候选记忆供淘汰。
 * @param[in] records 借用的候选数组，本策略只使用数量。
 * @param[in] count 候选条数，调用路径保证大于零。
 * @param[in] user 策略上下文，本夹具忽略。
 * @param[out] index 输出 count-1 的零基索引。
 * @return 始终返回 CC_OK。
 */
static cc_result_t choose_eviction(const cc_memory_plan_record_t *records,size_t count,void *user,size_t *index)
{ (void)records; (void)user; *index=count-1U; return cc_result_ok(); }
/**
 * @brief 根据标志模拟规划资源失败，解除故障后淘汰第一条候选。
 * @param[in] records 候选数组，本策略忽略内容。
 * @param[in] count 候选数量，规划器保证存在候选。
 * @param[in] user 借用的整数故障标志，非零时注入失败。
 * @param[out] index 成功时输出零，失败时保持调用前状态。
 * @return 故障开启为 CC_ERR_OUT_OF_MEMORY，解除后为 CC_OK。
 */
static cc_result_t fail_then_evict(const cc_memory_plan_record_t *records,size_t count,void *user,size_t *index)
{ (void)records; (void)count; if (*(int *)user) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"injected planning failure"); *index=0; return cc_result_ok(); }
/**
 * @brief 在方案改变既有记忆时要求快照，便于测试重试关联。
 * @param[in] plan 借用的方案，通过 changed_existing 判断是否快照。
 * @param[in] original 原记忆条数，本夹具忽略。
 * @param[in] user 策略上下文，本夹具忽略。
 * @return changed_existing 非零时为 REWRITE，其余为零。
 * @note 这是测试策略，设备使用同时达到五条与 30% 的产品规则。
 */
static cc_memory_snapshot_reason_t snapshot_rewrites(const cc_memory_plan_t *plan,size_t original,void *user)
{ (void)original; (void)user; return plan->changed_existing ? CC_MEMORY_SNAPSHOT_REWRITE : 0; }
/**
 * @brief 用整数标志模拟出队前挂载核对的拒绝结果。
 * @param[in] user 借用的整数拒绝标志。
 * @return 标志非零时返回 CC_ERR_STORAGE，其余返回 CC_OK。
 */
static cc_result_t guard_ack(void *user)
{ return *(int *)user ? cc_result_error(CC_ERR_STORAGE,"injected mount change") : cc_result_ok(); }

/**
 * @brief 为测试创建或更新 color 受管理记忆并核实已观察到安装。
 * @param[in,out] store 夹具的 JSON 存储，受管理集合为空或只含 color。
 * @param[in] text 要保存的借用正文，计划构建时复制。
 * @param[in] updated 应用提供的非负更新时间。
 * @return 提交成功且 installed 为一时返回一，其余打印错误并返回零。
 */
static int set_color(cc_memory_store_t *store,const char *text,int64_t updated)
{
    cc_memory_plan_t current={0},plan={0}; uint64_t revision=0; char *id=NULL; int installed=0;
    if (!ok(cc_json_memory_managed_list(store,&current,&revision))) return 0;
    cc_memory_plan_change_t change={0}; change.kind=current.count ? CC_MEMORY_CHANGE_UPDATE : CC_MEMORY_CHANGE_ADD;
    change.record.size=sizeof(change.record); change.record.id="color"; change.record.text=(char *)text;
    change.record.category="preference"; change.record.evidence="evidence"; change.record.source_session="initial";
    change.record.importance=70; change.record.updated_at=updated;
    int success=ok(cc_memory_plan_build(current.records,current.count,&change,1,8,NULL,NULL,&plan));
    if (success) success=ok(cc_json_memory_managed_apply(store,revision,&plan,NULL,0,NULL,&id,&installed)) && installed;
    free(id); cc_memory_plan_cleanup(&plan); cc_memory_plan_cleanup(&current); return success;
}

/**
 * @brief 读取 store 当前 RAM 中的 color 并逐字节比较正文。
 * @param[in] store 借用的 JSON 记忆存储。
 * @param[in] text 预期正文。
 * @return 读取成功且正文相同时为一，其余为零，读取副本在本函数清理。
 */
static int color_is(cc_memory_store_t *store,const char *text)
{
    cc_memory_entry_t entry={0}; int success=ok(cc_memory_store_get(store,"color",&entry));
    if (success) success=strcmp(entry.value,text)==0;
    cc_memory_entry_free(&entry); return success;
}

/**
 * @brief 从非空测试集合应用空方案，供后续场景重新建立基线。
 * @param[in,out] store 借用的 JSON 存储，调用路径保证受管理集合非空。
 * @return 提交成功且已观察到安装时为一，其余为零，本次计划和 ID 在内部清理。
 */
static int remove_all_managed(cc_memory_store_t *store)
{
    cc_memory_plan_t current={0},empty={0}; uint64_t revision=0; char *id=NULL; int installed=0;
    int success=ok(cc_json_memory_managed_list(store,&current,&revision));
    if (success) success=ok(cc_json_memory_managed_apply(store,revision,&empty,NULL,0,NULL,&id,&installed)) && installed;
    free(id); cc_memory_plan_cleanup(&current); return success;
}

/**
 * @brief 枚举快照数量并在本函数释放列表副本。
 * @param[in] snapshots 借用的快照存储。
 * @return 成功为快照条数，枚举失败为 (size_t)-1。
 */
static size_t snapshot_count(cc_memory_snapshot_store_t *snapshots)
{
    cc_memory_snapshot_info_t *items=NULL; size_t count=0;
    if (!ok(cc_memory_snapshot_list(snapshots,&items,&count))) return (size_t)-1;
    cc_memory_snapshot_info_free_array(items,count); return count;
}

/**
 * @brief 通过文件系统与模型夹具验证自动记忆规划、冲突、安装核对及出队。
 * @return 全部断言和清理通过为零，首个 CHECK 失败时打印位置并返回一。
 * @note 每次创建独立 UUID 目录，成功结束时删除文件和版本，失败现场保留供诊断。
 */
int main(void)
{
    cc_filesystem_t fs={0}; CHECK(ok(cc_filesystem_get_default(&fs)));
    char *root=NULL; CHECK(ok(cc_id_generate_uuid_v4("memory-worker-",&root)));
    char history[160],active[160]; snprintf(history,sizeof(history),"%s/history",root); snprintf(active,sizeof(active),"%s/memory.json",root);
    CHECK(ok(fs.vtable->make_dir(fs.self,history)));
    fault_fs_t fault={.base=&fs,.root=root}; cc_filesystem_vtable_t vt={0}; vt.size=sizeof(vt);
    vt.read_text=rd; vt.write_text=wr; vt.list_dir=ls; vt.stat=st; vt.remove=rm;
    vt.atomic_replace=replace; vt.sync_file=sync_file; vt.sync_dir=sync_dir;
    cc_filesystem_t wrapped=fs; wrapped.self=&fault; wrapped.vtable=&vt;
    cc_memory_store_t store={0}; CHECK(ok(cc_memory_store_create_json_file_with_filesystem(&store,active,&wrapped)));
    CHECK(ok(cc_memory_store_set(&store,"summary","short term","context_summary","s1")) && set_color(&store,"blue",100));
    cc_memory_snapshot_store_t *snapshots=NULL; CHECK(ok(cc_memory_snapshot_store_open(&wrapped,history,active,&snapshots)));
    cc_managed_memory_worker_options_t config={sizeof(config),8,2,4096,choose_eviction,snapshot_rewrites,NULL};
    cc_managed_memory_worker_t *worker=NULL; CHECK(ok(cc_managed_memory_worker_create(&store,snapshots,&config,&worker)));
    cc_memory_extraction_options_t options={0}; options.size=sizeof(options); options.model="fake"; options.max_output_tokens=777;
    fake_llm_t fake={0}; cc_llm_provider_t provider={&fake,&provider_vt}; cc_managed_memory_worker_result_t result={0};
    fake.response="{\"memories\":[{\"action\":\"update\",\"target_id\":\"color\",\"text\":\"green\",\"category\":\"preference\",\"importance\":80,\"evidence\":\"green\"}]}";
    CHECK(ok(cc_managed_memory_worker_enqueue(worker,"turn1","s1","I like green","ok")));
    CHECK(ok(cc_managed_memory_worker_enqueue(worker,"turn2","s2","nothing new","ok")));
    cc_result_t rc=cc_managed_memory_worker_enqueue(worker,"turn3","s1","overflow","ok"); CHECK(rc.code==CC_ERR_LIMIT_EXCEEDED); cc_result_free(&rc);
    fault.fail_active_write=1;
    rc=cc_managed_memory_worker_process_one(worker,&provider,&options,200,&result);
    CHECK(rc.code==CC_ERR_IO && fake.calls==1 && strcmp(fake.session_id,"s1")==0 && result.changed && result.model_usage.valid); cc_result_free(&rc);
    size_t pending=0,bytes=0; cc_managed_memory_worker_status(worker,&pending,&bytes);
    CHECK(pending==2 && bytes>0 && color_is(&store,"blue") && snapshot_count(snapshots)==1);
    fake.response="{\"memories\":[]}"; fault.fail_active_write=0;
    CHECK(ok(cc_managed_memory_worker_process_one(worker,&provider,&options,999,&result)));
    CHECK(fake.calls==1 && !result.model_usage.valid && color_is(&store,"green") && snapshot_count(snapshots)==1);
    CHECK(ok(cc_managed_memory_worker_process_one(worker,&provider,&options,999,&result)) && fake.calls==2 &&
          strcmp(fake.session_id,"s2")==0 && !result.changed);
    cc_managed_memory_worker_status(worker,&pending,&bytes); CHECK(pending==0 && bytes==0);

    /* 摘要变化使旧快照失效，重试复用已提取决策并建立新的改写前版本。 */
    fake.response="{\"memories\":[{\"action\":\"update\",\"target_id\":\"color\",\"text\":\"orange\",\"category\":\"preference\",\"importance\":80,\"evidence\":\"orange\"}]}";
    CHECK(ok(cc_managed_memory_worker_enqueue(worker,"turn4","s1","I like orange","ok")));
    fault.fail_active_write=1; rc=cc_managed_memory_worker_process_one(worker,&provider,&options,300,&result);
    CHECK(rc.code==CC_ERR_IO && fake.calls==3 && snapshot_count(snapshots)==2); cc_result_free(&rc);
    fault.fail_active_write=0; CHECK(ok(cc_memory_store_set(&store,"summary","new short term","context_summary","s1")));
    CHECK(ok(cc_managed_memory_worker_process_one(worker,&provider,&options,400,&result)));
    CHECK(fake.calls==3 && color_is(&store,"orange") && snapshot_count(snapshots)==3);

    /* 同一目标被用户修改后保管冲突，显式选择丢弃原待处理输入。 */
    fake.response="{\"memories\":[{\"action\":\"update\",\"target_id\":\"color\",\"text\":\"yellow\",\"category\":\"preference\",\"importance\":80,\"evidence\":\"yellow\"}]}";
    CHECK(ok(cc_managed_memory_worker_enqueue(worker,"turn5","s1","I like yellow","ok")));
    fault.fail_active_write=1; rc=cc_managed_memory_worker_process_one(worker,&provider,&options,500,&result);
    CHECK(rc.code==CC_ERR_IO && fake.calls==4); cc_result_free(&rc);
    fault.fail_active_write=0; CHECK(set_color(&store,"red",600));
    rc=cc_managed_memory_worker_process_one(worker,&provider,&options,700,&result);
    CHECK(rc.code==CC_ERR_INVALID_STATE && fake.calls==4 && color_is(&store,"red")); cc_result_free(&rc);
    cc_managed_memory_worker_status(worker,&pending,NULL); CHECK(pending==1);
    CHECK(remove_all_managed(&store));
    rc=cc_managed_memory_worker_process_one(worker,&provider,&options,700,&result);
    CHECK(rc.code==CC_ERR_INVALID_STATE && fake.calls==4); cc_result_free(&rc);
    rc=cc_managed_memory_worker_discard_head(worker,"wrong"); CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    CHECK(ok(cc_managed_memory_worker_discard_head(worker,"turn5")) && set_color(&store,"red",600));

    /* 主文件已安装但目录同步失败，恢复 store 后核对完整受管理集合再确认出队。 */
    fake.response="{\"memories\":[{\"action\":\"update\",\"target_id\":\"color\",\"text\":\"purple\",\"category\":\"preference\",\"importance\":80,\"evidence\":\"purple\"}]}";
    CHECK(ok(cc_managed_memory_worker_enqueue(worker,"turn6","s1","I like purple","ok")));
    fault.fail_active_sync=1; rc=cc_managed_memory_worker_process_one(worker,&provider,&options,800,&result);
    CHECK(rc.code==CC_ERR_IO && fake.calls==5 && color_is(&store,"purple")); cc_result_free(&rc);
    fault.fail_active_sync=0; rc=cc_managed_memory_worker_process_one(worker,&provider,&options,900,&result);
    CHECK(rc.code==CC_ERR_STORAGE && fake.calls==5); cc_result_free(&rc);
    rc=cc_managed_memory_worker_discard_head(worker,"turn6"); CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    rc=cc_managed_memory_worker_replan_head(worker,"turn6"); CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    CHECK(ok(cc_json_memory_recover_in_place(&store)));
    CHECK(ok(cc_managed_memory_worker_process_one(worker,&provider,&options,900,&result)) && fake.calls==5 && color_is(&store,"purple"));
    cc_managed_memory_worker_status(worker,&pending,NULL); CHECK(pending==0);

    /* 规划阶段的策略失败发生在写盘前，重试继续使用首次提取的决策和时间。 */
    cc_managed_memory_worker_destroy(worker); worker=NULL;
    int fail_plan=1; config.capacity=1; config.select_eviction=fail_then_evict; config.policy_user_data=&fail_plan;
    CHECK(ok(cc_managed_memory_worker_create(&store,snapshots,&config,&worker)));
    fake.response="{\"memories\":[{\"action\":\"add\",\"target_id\":\"\",\"text\":\"hardware project\",\"category\":\"project\",\"importance\":90,\"evidence\":\"hardware project\"}]}";
    CHECK(ok(cc_managed_memory_worker_enqueue(worker,"turn7","s1","hardware project","ok")));
    rc=cc_managed_memory_worker_process_one(worker,&provider,&options,1000,&result);
    CHECK(rc.code==CC_ERR_OUT_OF_MEMORY && fake.calls==6); cc_result_free(&rc);
    fail_plan=0; fake.response="{\"memories\":[]}";
    CHECK(ok(cc_managed_memory_worker_process_one(worker,&provider,&options,1100,&result)) && fake.calls==6);
    cc_memory_plan_t final={0}; uint64_t revision=0;
    CHECK(ok(cc_json_memory_managed_list(&store,&final,&revision)) && final.count==1 &&
          strcmp(final.records[0].text,"hardware project")==0 && final.records[0].updated_at==1000);
    cc_memory_plan_cleanup(&final);

    /* 写入成功后 guard 拒绝确认时保管队首，恢复后核对既有安装，
     * 模型调用次数与快照数量保持本次已完成的结果。 */
    cc_managed_memory_worker_destroy(worker); worker=NULL;
    CHECK(remove_all_managed(&store) && set_color(&store,"blue",1200));
    int reject_ack=1; config.capacity=8; config.select_eviction=choose_eviction; config.policy_user_data=NULL;
    config.before_ack=guard_ack; config.before_ack_user_data=&reject_ack;
    CHECK(ok(cc_managed_memory_worker_create(&store,snapshots,&config,&worker)));
    fake.response="{\"memories\":[{\"action\":\"update\",\"target_id\":\"color\",\"text\":\"green\",\"category\":\"preference\",\"importance\":80,\"evidence\":\"green\"}]}";
    CHECK(ok(cc_managed_memory_worker_enqueue(worker,"turn8","s1","I like green","ok")));
    size_t versions_before=snapshot_count(snapshots);
    rc=cc_managed_memory_worker_process_one(worker,&provider,&options,1300,&result);
    CHECK(rc.code==CC_ERR_STORAGE && fake.calls==7 && color_is(&store,"green") &&
          snapshot_count(snapshots)==versions_before+1U); cc_result_free(&rc);
    cc_managed_memory_worker_status(worker,&pending,NULL); CHECK(pending==1);
    reject_ack=0;
    CHECK(ok(cc_managed_memory_worker_process_one(worker,&provider,&options,1400,&result)) &&
          fake.calls==7 && snapshot_count(snapshots)==versions_before+1U);
    cc_managed_memory_worker_status(worker,&pending,NULL); CHECK(pending==0);

    /* 主文件实际安装后回读失败，installed 仍可能为零。
     * 保管方案及队首，恢复后用完整状态核对安装并确认出队。 */
    fake.response="{\"memories\":[{\"action\":\"update\",\"target_id\":\"color\",\"text\":\"cyan\",\"category\":\"preference\",\"importance\":80,\"evidence\":\"cyan\"}]}";
    CHECK(ok(cc_managed_memory_worker_enqueue(worker,"turn9","s1","I like cyan","ok")));
    versions_before=snapshot_count(snapshots);
    fault.fail_active_replace_after_install=1;
    rc=cc_managed_memory_worker_process_one(worker,&provider,&options,1500,&result);
    CHECK(rc.code==CC_ERR_IO && fake.calls==8); cc_result_free(&rc);
    int write_ready=1;
    CHECK(ok(cc_json_memory_write_ready(&store,&write_ready)) && !write_ready);
    char *installed_text=NULL;
    CHECK(ok(fs.vtable->read_text(fs.self,active,&installed_text)));
    CHECK(strstr(installed_text,"\"value\":\"cyan\"")!=NULL);
    free(installed_text);
    cc_managed_memory_worker_status(worker,&pending,&bytes); CHECK(pending==1 && bytes>0);
    size_t retained_bytes=bytes;
    rc=cc_managed_memory_worker_discard_head(worker,"turn9");
    CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    rc=cc_managed_memory_worker_replan_head(worker,"turn9");
    CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    cc_managed_memory_worker_status(worker,&pending,&bytes);
    CHECK(pending==1 && bytes==retained_bytes && fake.calls==8);

    /* 尚未尝试写入的新 worker 也遵守共享 store 的写入阻断。 */
    cc_managed_memory_worker_t *fresh_worker=NULL;
    CHECK(ok(cc_managed_memory_worker_create(&store,snapshots,&config,&fresh_worker)));
    CHECK(ok(cc_managed_memory_worker_enqueue(fresh_worker,"fresh","s1","pending","ok")));
    rc=cc_managed_memory_worker_discard_head(fresh_worker,"fresh");
    CHECK(rc.code==CC_ERR_STORAGE); cc_result_free(&rc);
    rc=cc_managed_memory_worker_replan_head(fresh_worker,"fresh");
    CHECK(rc.code==CC_ERR_STORAGE); cc_result_free(&rc);
    cc_managed_memory_worker_status(fresh_worker,&pending,NULL); CHECK(pending==1);
    cc_managed_memory_worker_destroy(fresh_worker);

    fault.hide_active_reads=0;
    CHECK(ok(cc_json_memory_recover_in_place(&store)));
    CHECK(ok(cc_managed_memory_worker_process_one(worker,&provider,&options,1600,&result)));
    CHECK(fake.calls==8 && color_is(&store,"cyan") &&
          snapshot_count(snapshots)==versions_before+1U);
    cc_managed_memory_worker_status(worker,&pending,&bytes); CHECK(pending==0 && bytes==0);

    cc_managed_memory_worker_destroy(worker); cc_memory_store_destroy(&store);
    cc_memory_snapshot_info_t *versions=NULL; size_t count=0; CHECK(ok(cc_memory_snapshot_list(snapshots,&versions,&count)));
    for (size_t i=0;i<count;i++) CHECK(ok(cc_memory_snapshot_delete(snapshots,versions[i].id)));
    cc_memory_snapshot_info_free_array(versions,count); cc_memory_snapshot_store_destroy(snapshots);
    CHECK(ok(fs.vtable->remove(fs.self,active)) && ok(fs.vtable->remove(fs.self,history)) && ok(fs.vtable->remove(fs.self,root)));
    fs.vtable->destroy(fs.self); free(root); return 0;
}
