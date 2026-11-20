#include "cc/adapters/cc_json_segmented_history.h"
#include "cc/app/cc_agent_runtime.h"
#include "cc/core/cc_id.h"
#include "cc/core/cc_observability.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief 创建本测试独占分段后端。
 * @param[in] root_path 根路径借用。
 * @param[in] segment_bytes 分段阈值。
 * @param[in] media_dir 媒体目录借用。
 * @param[in] max_base64_bytes 回填预算。
 * @param[out] out_store 新端口槽，成功最终 destroy。
 * @return 创建结果，诊断归测试。
 */
extern cc_result_t cc_json_segmented_store_create(const char *root_path, size_t segment_bytes,
    const char *media_dir, size_t max_base64_bytes, cc_session_store_t *out_store);
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "check failed at line %d\n", __LINE__); return 1; } } while (0)
/** @brief 检查成功并消费独占结果。
 * @param[in] rc 按值移入的拥有结果。
 * @return 成功一，否则报告并零，串行调用。
 */
static int ok(cc_result_t rc) { int good = rc.code == CC_OK; if (!good) fprintf(stderr, "%s\n", rc.message ? rc.message : "error"); cc_result_free(&rc); return good; }

typedef struct fault_fs { cc_filesystem_t *base; int fail_sync; int fail_remove; } fault_fs_t;
/** @brief 转发同步文本读取。
 * @param[in] s 存活故障端口上下文借用。
 * @param[in] p 路径借用。
 * @param[out] v 新槽，成功文本归调用者 free。
 * @return 原端口结果，测试串行调用。
 */
static cc_result_t f_read(void *s, const char *p, char **v) { fault_fs_t *f=s; return f->base->vtable->read_text(f->base->self,p,v); }
/** @brief 转发同步文本写入。
 * @param[in] s 存活故障上下文借用。
 * @param[in] p 路径借用。
 * @param[in] v 正文借用，仅调用期有效。
 * @return 原结果，失败可能已写入，诊断归调用者，串行调用。
 */
static cc_result_t f_write(void *s, const char *p, const char *v) { fault_fs_t *f=s; return f->base->vtable->write_text(f->base->self,p,v); }
/** @brief 转发目录枚举并交付拥有快照。
 * @param[in] s 存活故障上下文借用。
 * @param[in] p 路径借用。
 * @param[out] v 新数组槽，成功逐项及数组由调用者 free。
 * @param[out] n 条数槽，沿原端口规则。
 * @return 原结果，诊断归调用者，串行调用。
 */
static cc_result_t f_list(void *s, const char *p, char ***v, size_t *n) { fault_fs_t *f=s; return f->base->vtable->list_dir(f->base->self,p,v,n); }
/** @brief 转发元数据查询。
 * @param[in] s 存活故障上下文借用。
 * @param[in] p 路径借用。
 * @param[out] v 元数据槽，无拥有字段。
 * @return 原结果，诊断归调用者，串行调用。
 */
static cc_result_t f_stat(void *s, const char *p, cc_filesystem_stat_t *v) { fault_fs_t *f=s; return f->base->vtable->stat(f->base->self,p,v); }
/** @brief 注入移除失败或转发移除。
 * @param[in] s 存活故障上下文借用，读取 fail_remove。
 * @param[in] p 路径借用。
 * @return 开关开启时 IO，否则原结果，诊断归调用者，串行调用。
 */
static cc_result_t f_remove(void *s, const char *p) { fault_fs_t *f=s; if (f->fail_remove) return cc_result_error(CC_ERR_IO,"injected removal failure"); return f->base->vtable->remove(f->base->self,p); }
/** @brief 转发原子替换。
 * @param[in] s 存活上下文借用。
 * @param[in] a 暂存路径借用。
 * @param[in] b 目标路径借用。
 * @return 原结果，诊断归调用者，文件所有权按端口规则，串行调用。
 */
static cc_result_t f_replace(void *s, const char *a, const char *b) { fault_fs_t *f=s; return f->base->vtable->atomic_replace(f->base->self,a,b); }
/** @brief 注入检查点同步失败或转发。
 * @param[in] s 存活故障上下文借用，读取 fail_sync。
 * @param[in] p 路径借用。
 * @return 开关开启时 IO，否则原结果，诊断归调用者，串行调用。
 */
static cc_result_t f_sync(void *s, const char *p) { fault_fs_t *f=s; if (f->fail_sync) return cc_result_error(CC_ERR_IO,"injected checkpoint failure"); return f->base->vtable->sync_file(f->base->self,p); }
/** @brief 转发目录同步。
 * @param[in] s 存活上下文借用。
 * @param[in] p 路径借用。
 * @return 原结果，诊断归调用者，串行调用。
 */
static cc_result_t f_sync_dir(void *s, const char *p) { fault_fs_t *f=s; return f->base->vtable->sync_dir(f->base->self,p); }

typedef struct fake_tool { const char *name; int calls; int uncertain; int fail_after; int fail_remove; fault_fs_t *fault; } fake_tool_t;
/** @brief 借用假工具名称。
 * @param[in] p 存活假工具借用。
 * @return 覆盖工具寿命的名称借用，无分配。
 */
static const char *tool_name(void *p) { return ((fake_tool_t *)p)->name; }
/** @brief 返回固定工具描述。
 * @param[in] p 未使用借用。
 * @return 静态文本借用。
 */
static const char *tool_description(void *p) { (void)p; return "test tool"; }
/** @brief 返回固定工具 schema。
 * @param[in] p 未使用借用。
 * @return 静态 JSON 借用。
 */
static const char *tool_schema(void *p) { (void)p; return "{}"; }
/** @brief 核对操作 ID，计数执行并可在副作用后打开持久化故障。
 * @param[in,out] p 独占假工具借用，fault 覆盖调用。
 * @param[in] args 未使用参数借用。
 * @param[in] ctx 必填上下文借用，须含 op 前缀幂等键。
 * @param[out] out 必填空槽，结果正文由 runtime cleanup。
 * @return 缺幂等键为状态错误，否则成功，测试串行调用。
 */
static cc_result_t tool_call(void *p, const char *args, const cc_tool_context_t *ctx, cc_tool_result_t *out)
{
    (void)args;
    fake_tool_t *tool = p;
    if (!ctx->idempotency_key || strncmp(ctx->idempotency_key, "op-", 3))
        return cc_result_error(CC_ERR_INVALID_STATE, "Missing operation id");
    tool->calls++;
    if (tool->fail_after) tool->fault->fail_sync = 1;
    if (tool->fail_remove) tool->fault->fail_remove = 1;
    out->ok = !tool->uncertain;
    if (tool->uncertain) out->error = cc_copy_string("outcome unknown");
    else out->text = cc_copy_string("done");
    if (!out->error && !out->text)
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate fake tool response");
    return cc_result_ok();
}
static const cc_tool_vtable_t tool_vtable = {tool_name, tool_description, tool_schema, tool_call, NULL};
static cc_cancel_source_t *cancel_during_policy;
/** @brief 允许测试工具并可在进入前触发取消。
 * @param[in] p 未使用借用。
 * @param[in] c 未使用调用借用。
 * @param[in] x 未使用上下文借用。
 * @param[out] d 必填新槽，清零后允许，无拥有成员。
 * @return 始终成功，全局取消源由测试线程串行保管。
 */
static cc_result_t allow(void *p, const cc_tool_call_t *c, const cc_tool_context_t *x, cc_policy_decision_t *d)
{
    (void)p; (void)c; (void)x; memset(d,0,sizeof(*d)); d->allowed=1;
    if (cancel_during_policy) cc_cancel_source_cancel(cancel_during_policy);
    return cc_result_ok();
}
static const cc_policy_engine_vtable_t policy_vtable = {allow, NULL};
typedef struct fake_model { int requests; char last_run_id[64]; int bad_request_identity; } fake_model_t;
/** @brief 第一次回复两个工具调用，后续回复最终文本并记录请求身份。
 * @param[in,out] p 独占假模型借用，计数及身份字段由本测试线程维护。
 * @param[in] request 必填同步请求借用，记录 run ID 副本。
 * @param[out] out 必填已初始化回复，成功数据归 runtime cleanup。
 * @return 回复构造结果，失败仍保留请求计数。
 */
static cc_result_t chat(void *p, const cc_llm_chat_request_t *request, cc_llm_response_t *out)
{
    fake_model_t *model = p;
    const char *run_id=cc_llm_chat_request_run_id(request);
    if (!run_id || !run_id[0] || !request->session_id) model->bad_request_identity=1;
    snprintf(model->last_run_id,sizeof(model->last_run_id),"%s",run_id ? run_id : "");
    model->requests++;
    out->finished = 1;
    if (model->requests == 1) {
        cc_result_t rc = cc_llm_response_add_tool_call(out, "provider-1", "first", "{}");
        if (rc.code == CC_OK) rc = cc_llm_response_add_tool_call(out, "provider-2", "second", "{}");
        return rc;
    }
    return cc_llm_response_set_text(out, "finished");
}
static const cc_llm_provider_vtable_t model_vtable = {chat, NULL, NULL, NULL};

typedef struct trace {
    char first_run[64], calls[2][96], operations[2][64];
    char resolved_run[64], resolved_call[96], resolved_operation[64];
    int starts, finishes, usages, finished, bad;
} trace_t;
/** @brief 解析观察事件并核对 session/run/call/operation 关联。
 * @param[in] type 同步事件种类借用。
 * @param[in] json 同步事件 JSON 借用。
 * @param[in,out] user 存活独占 trace 借用，解析失败记 bad，临时树内部销毁。
 * @note 订阅退订屏障覆盖上下文，测试事件同步分发。
 */
static void trace_event(const char *type,const char *json,void *user)
{
    trace_t *trace=user; cc_json_value_t *root=NULL;
    cc_result_t rc=cc_json_parse(json,&root);
    if (rc.code!=CC_OK || !root) { trace->bad=1; cc_result_free(&rc); cc_json_destroy(root); return; }
    const char *session=cc_json_string_value(cc_json_object_get(root,"session_id"));
    const char *run=cc_json_string_value(cc_json_object_get(root,"run_id"));
    cc_json_value_t *attrs=cc_json_object_get(root,"attributes");
    const char *call=cc_json_string_value(cc_json_object_get(attrs,"tool_call_id"));
    const char *operation=cc_json_string_value(cc_json_object_get(attrs,"operation_id"));
    if (session && strcmp(session,"s1")==0) {
        if (!run || !run[0]) trace->bad=1;
        else if (!trace->first_run[0]) snprintf(trace->first_run,sizeof(trace->first_run),"%s",run);
        else if (strcmp(trace->first_run,run)!=0) trace->bad=1;
        if (strcmp(type,CC_OBS_EVENT_LLM_USAGE)==0) trace->usages++;
        if (strcmp(type,CC_OBS_EVENT_RUN_FINISHED)==0) trace->finished++;
        if (strcmp(type,CC_OBS_EVENT_TOOL_START)==0) {
            if (!call || !operation || strncmp(operation,"op-",3)!=0 || trace->starts>=2) trace->bad=1;
            else {
                snprintf(trace->calls[trace->starts],sizeof(trace->calls[0]),"%s",call);
                snprintf(trace->operations[trace->starts],sizeof(trace->operations[0]),"%s",operation);
                trace->starts++;
            }
        }
        if (strcmp(type,CC_OBS_EVENT_TOOL_FINISH)==0) {
            int matched=0;
            for (int i=0;i<trace->starts;i++)
                if (call && operation && strcmp(trace->calls[i],call)==0 &&
                    strcmp(trace->operations[i],operation)==0) matched=1;
            if (!matched) trace->bad=1;
            trace->finishes++;
        }
    } else if (session && strcmp(session,"s2")==0 && strcmp(type,CC_OBS_EVENT_TOOL_RECOVERY_RESOLVED)==0) {
        if (!call || !operation) trace->bad=1;
        else {
            snprintf(trace->resolved_run,sizeof(trace->resolved_run),"%s",run ? run : "");
            snprintf(trace->resolved_call,sizeof(trace->resolved_call),"%s",call);
            snprintf(trace->resolved_operation,sizeof(trace->resolved_operation),"%s",operation);
        }
    }
    cc_result_free(&rc); cc_json_destroy(root);
}

/** @brief 递归清理本测试独占树。
 * @param[in] fs 存活文件系统借用。
 * @param[in] path 测试范围内路径借用。
 * @return 全部成功一，否则零，列名/路径/诊断内部释放，串行调用。
 */
static int remove_tree(cc_filesystem_t *fs, const char *path)
{
    cc_filesystem_stat_t info = {.size=sizeof(info)};
    if (!ok(fs->vtable->stat(fs->self,path,&info))) return 0;
    if (info.type == CC_FS_ENTRY_DIRECTORY) {
        char **names=NULL; size_t count=0;
        if (!ok(fs->vtable->list_dir(fs->self,path,&names,&count))) return 0;
        int success=1;
        for (size_t i=0;i<count;i++) {
            size_t n=strlen(path)+strlen(names[i])+2; char *child=malloc(n);
            if (!child) success=0;
            else { snprintf(child,n,"%s/%s",path,names[i]); if (!remove_tree(fs,child)) success=0; free(child); }
            free(names[i]);
        }
        free(names);
        if (!success) return 0;
    }
    return ok(fs->vtable->remove(fs->self,path));
}

/** @brief 取得唯一检查点 ID 并移交字符串。
 * @param[in] store 存活独占检查点后端借用。
 * @param[out] out 新槽，仅成功交调用者 free 的 ID。
 * @return 恰好一项且读取成功为一，否则零，成功只释放数组容器。
 */
static int get_only_run(cc_run_checkpoint_store_t *store, char **out)
{
    char **ids=NULL; size_t count=0;
    if (!ok(cc_run_checkpoint_list(store,&ids,&count)) || count != 1) {
        for (size_t i = 0; i < count; i++) free(ids[i]);
        free(ids);
        return 0;
    }
    *out=ids[0]; free(ids); return 1;
}
/** @brief 全量读取并统计原输入在 USER 历史中的出现次数。
 * @param[in] store 存活端口借用。
 * @param[in] session 会话名借用。
 * @param[in] input 预期正文借用。
 * @return 次数，加载失败为 -1，全部消息/摘要内部回收，串行读取。
 */
static int count_input(cc_session_store_t *store, const char *session, const char *input)
{
    cc_message_t *messages=NULL; size_t count=0; int found=0;
    if (!ok(store->vtable->load_messages(store->self,session,0,&messages,&count))) return -1;
    for (size_t i=0;i<count;i++) {
        char *text=NULL;
        if (messages[i].role == CC_ROLE_USER && ok(cc_message_get_text_summary(&messages[i],&text)) &&
            text && strcmp(text,input)==0) found++;
        free(text); cc_message_cleanup(&messages[i]);
    }
    free(messages); return found;
}
/** @brief 一个批次追加四十条消息以跨越恢复分页边界。
 * @param[in] store 存活端口借用。
 * @param[in] session 会话名借用。
 * @return 构造及追加成功一，否则零，全部消息内部销毁，串行调用。
 */
static int append_padding(cc_session_store_t *store, const char *session)
{
    cc_message_t *messages[40] = {0}; cc_session_record_t records[40] = {0};
    cc_result_t rc = cc_result_ok();
    for (int i = 0; i < 40 && rc.code == CC_OK; i++) {
        char id[40]; snprintf(id,sizeof(id),"padding-%d",i);
        rc = cc_message_create_text(id,session,CC_ROLE_USER,"later message",NULL,&messages[i]);
        records[i].type=CC_SESSION_RECORD_MESSAGE; records[i].session_id=session;
        records[i].data.message=messages[i];
    }
    if (rc.code == CC_OK) rc = store->vtable->append_records(store->self,records,40);
    for (int i = 0; i < 40; i++) cc_message_destroy(messages[i]);
    return ok(rc);
}
/** @brief 验证核对说明和结果制品消息各只持久化一次。
 * @param[in] store 存活后端借用。
 * @param[in] session 会话名借用。
 * @param[in] call_id 原调用 ID 借用。
 * @param[in] operation_id 操作 ID 借用。
 * @return 两类各一项为一，否则零，临时历史内部回收，串行调用。
 */
static int count_reconciliation(cc_session_store_t *store, const char *session,
                                const char *call_id, const char *operation_id)
{
    cc_message_t *messages=NULL; size_t count=0; int notes=0,artifacts=0;
    if (!ok(store->vtable->load_messages(store->self,session,-1,&messages,&count))) return 0;
    char artifact_id[96]; snprintf(artifact_id,sizeof(artifact_id),"%s-artifacts",operation_id);
    for (size_t i=0;i<count;i++) {
        if (messages[i].role==CC_ROLE_USER && messages[i].tool_call_id &&
            strcmp(messages[i].tool_call_id,call_id)==0 &&
            messages[i].tool_outcome==CC_MESSAGE_TOOL_OUTCOME_RECONCILED_EXECUTED) notes++;
        if (messages[i].id && strcmp(messages[i].id,artifact_id)==0) artifacts++;
        cc_message_cleanup(&messages[i]);
    }
    free(messages); return notes==1 && artifacts==1;
}

static int wrong_result_session_once, drop_intent_once;
/** @brief 转发查找并一次性注入错会话结果或缺失意图。
 * @param[in] store 存活端口借用。
 * @param[in] session_id 会话名借用。
 * @param[in] match 查找模式。
 * @param[in] id 消息/调用 ID 借用。
 * @param[out] message 必填空槽，命中拥有消息由 runtime cleanup。
 * @param[out] found 命中标志槽，意图丢弃注入时写零。
 * @return 原查找或注入分配结果，全局开关仅测试线程修改。
 */
static cc_result_t test_history_find(cc_session_store_t *store,const char *session_id,
    cc_run_history_match_t match,const char *id,cc_message_t *message,int *found)
{
    cc_result_t rc=cc_json_segmented_store_find_message(store,session_id,match,id,message,found);
    if (rc.code==CC_OK && match==CC_RUN_HISTORY_TOOL_RESULT && *found && wrong_result_session_once) {
        wrong_result_session_once=0;
        char *wrong=cc_copy_string("other-session");
        if (!wrong) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot inject wrong session");
        free(message->session_id); message->session_id=wrong;
    }
    if (rc.code==CC_OK && match==CC_RUN_HISTORY_TOOL_INTENT && *found && drop_intent_once) {
        drop_intent_once=0; cc_message_cleanup(message); *found=0;
    }
    return rc;
}

/** @brief 创建借用测试依赖的 runtime，安装故障历史查找与同步事件总线。
 * @param[in] sessions 存活端口借用。
 * @param[in] registry 冻结注册表借用。
 * @param[in] journal 操作账本借用。
 * @param[in] checkpoints 检查点后端借用。
 * @param[in,out] model 独占假模型借用。
 * @param[in] bus 同步总线借用。
 * @param[out] out 新槽，成功 runtime 由调用者 destroy。
 * @return 创建成功一，否则零，依赖覆盖 runtime 寿命，串行装配。
 */
static int make_runtime(cc_session_store_t *sessions, cc_tool_registry_t *registry,
                        cc_operation_journal_t *journal, cc_run_checkpoint_store_t *checkpoints,
                        fake_model_t *model, cc_event_bus_t *bus, cc_agent_runtime_t **out)
{
    cc_agent_runtime_deps_t deps={0}; deps.size=sizeof(deps);
    deps.llm.self=model; deps.llm.vtable=&model_vtable; deps.tool_registry=registry;
    deps.store=*sessions; deps.policy.vtable=&policy_vtable;
    deps.event_bus=bus;
    deps.operation_journal=journal; deps.require_operation_journal=1;
    deps.run_checkpoints=checkpoints; deps.run_history_find=test_history_find;
    deps.run_history_visit=cc_json_segmented_store_visit_messages;
    cc_agent_runtime_options_t options={0}; options.config.workspace_dir="."; options.config.max_steps=4;
    return ok(cc_agent_runtime_create(&deps,&options,out));
}

/** @brief 验证取消、未知结果、检查点失败、核对重试及恢复身份关联。
 * @return 全部通过零，否则一，成功按借用顺序销毁 runtime/订阅/依赖/文件，早退交进程回收。
 * @note 真实持久化端口配合同步假 provider/tool，故障开关串行切换。
 */
int main(void)
{
    cc_filesystem_t fs={0}; CHECK(ok(cc_filesystem_get_default(&fs)));
    char *root=NULL; CHECK(ok(cc_id_generate_uuid_v4("run-resume-",&root)));
    char sessions_dir[160], ops_dir[160], runs_dir[160];
    snprintf(sessions_dir,sizeof(sessions_dir),"%s/sessions",root);
    snprintf(ops_dir,sizeof(ops_dir),"%s/ops",root);
    snprintf(runs_dir,sizeof(runs_dir),"%s/runs",root);
    CHECK(ok(fs.vtable->make_dir(fs.self,ops_dir)) && ok(fs.vtable->make_dir(fs.self,runs_dir)));
    fault_fs_t fault={&fs,0,0}; cc_filesystem_vtable_t vt={0}; vt.size=sizeof(vt);
    vt.read_text=f_read; vt.write_text=f_write; vt.list_dir=f_list; vt.stat=f_stat;
    vt.remove=f_remove; vt.atomic_replace=f_replace; vt.sync_file=f_sync; vt.sync_dir=f_sync_dir;
    cc_filesystem_t wrapped=fs; wrapped.self=&fault; wrapped.vtable=&vt;
    fault_fs_t journal_fault={&fs,0,0};
    cc_filesystem_t journal_fs=fs; journal_fs.self=&journal_fault; journal_fs.vtable=&vt;
    cc_session_store_t sessions={0}; CHECK(ok(cc_json_segmented_store_create(sessions_dir,2048,"media",1024,&sessions)));
    cc_operation_journal_t *journal=NULL; CHECK(ok(cc_operation_journal_open(&journal_fs,ops_dir,64,&journal)));
    cc_run_checkpoint_store_t *checkpoints=NULL; CHECK(ok(cc_run_checkpoint_store_open(&wrapped,runs_dir,&checkpoints)));
    cc_tool_registry_t *registry=NULL; CHECK(ok(cc_tool_registry_create(&registry)));
    fake_tool_t first={"first",0,0,0,0,&fault}, second={"second",0,0,0,0,&fault};
    cc_tool_t t1={&first,&tool_vtable}, t2={&second,&tool_vtable};
    CHECK(ok(cc_tool_registry_add(registry,t1)) && ok(cc_tool_registry_add(registry,t2)) && ok(cc_tool_registry_freeze(registry)));
    fake_model_t model={0}; cc_agent_runtime_t *runtime=NULL; cc_event_bus_t *bus=NULL;
    trace_t trace={0}; cc_event_subscription_t subscription={0};
    CHECK(ok(cc_event_bus_create(&bus)) &&
          ok(cc_event_bus_subscribe(bus,NULL,trace_event,&trace,NULL,&subscription)));
    CHECK(make_runtime(&sessions,registry,journal,checkpoints,&model,bus,&runtime));

    /* 工具进入前取消可保留未执行证据，重新打开存储后原计划用新调用 ID 重试。 */
    CHECK(ok(cc_agent_runtime_create_session(runtime,"cancelled",".")));
    CHECK(ok(cc_cancel_source_create(&cancel_during_policy)));
    cc_agent_runtime_run_options_t cancelled_options={0};
    cancelled_options.size=sizeof(cancelled_options);
    cancelled_options.cancel_token=cc_cancel_source_token(cancel_during_policy);
    char *cancelled_answer=NULL;
    cc_result_t cancelled_rc=cc_agent_runtime_handle_message_with_options(
        runtime,"cancelled","cancel before tool entry",&cancelled_options,&cancelled_answer);
    CHECK(cancelled_rc.code==CC_ERR_CANCELLED && first.calls==0 && second.calls==0 && model.requests==1);
    cc_result_free(&cancelled_rc); free(cancelled_answer);
    cc_cancel_source_destroy(cancel_during_policy); cancel_during_policy=NULL;
    char *cancelled_run=NULL; CHECK(get_only_run(checkpoints,&cancelled_run));
    cc_run_checkpoint_t cancelled_checkpoint={0};
    CHECK(ok(cc_run_checkpoint_load(checkpoints,cancelled_run,&cancelled_checkpoint)) &&
          cancelled_checkpoint.next_call==0 && cancelled_checkpoint.planned_calls.count==2);
    char *cancelled_call=cc_copy_string(cancelled_checkpoint.planned_calls.items[0].id); CHECK(cancelled_call);
    cc_run_checkpoint_cleanup(&cancelled_checkpoint);
    cc_agent_runtime_destroy(runtime); cc_run_checkpoint_store_destroy(checkpoints);
    cc_operation_journal_destroy(journal); sessions.vtable->destroy(sessions.self);
    CHECK(ok(cc_json_segmented_store_create(sessions_dir,2048,"media",1024,&sessions)));
    CHECK(ok(cc_operation_journal_open(&journal_fs,ops_dir,64,&journal)));
    CHECK(ok(cc_run_checkpoint_store_open(&wrapped,runs_dir,&checkpoints)));
    size_t imported_cancelled=0;
    CHECK(ok(cc_json_segmented_store_import_interrupted_calls(&sessions,journal,&imported_cancelled)) &&
          imported_cancelled==0);
    CHECK(make_runtime(&sessions,registry,journal,checkpoints,&model,bus,&runtime));
    cc_message_t cancelled_result={0}; int cancelled_found=0;
    CHECK(ok(cc_json_segmented_store_find_message(&sessions,"cancelled",CC_RUN_HISTORY_TOOL_RESULT,
        cancelled_call,&cancelled_result,&cancelled_found)) && cancelled_found &&
        cancelled_result.tool_outcome==CC_MESSAGE_TOOL_OUTCOME_RECONCILED_NOT_EXECUTED);
    cc_message_cleanup(&cancelled_result);
    CHECK(ok(cc_agent_runtime_resume_run(runtime,cancelled_run,NULL,&cancelled_answer)) &&
          first.calls==1 && second.calls==1 && model.requests==2 && cancelled_answer);
    free(cancelled_answer); free(cancelled_run);
    cc_message_t *cancelled_history=NULL; size_t cancelled_count=0; int saw_fresh_call=0;
    CHECK(ok(sessions.vtable->load_messages(sessions.self,"cancelled",-1,&cancelled_history,&cancelled_count)));
    for (size_t i=0;i<cancelled_count;i++) {
        for (size_t j=0;j<cancelled_history[i].tool_calls.count;j++) {
            cc_tool_call_t *historical=&cancelled_history[i].tool_calls.items[j];
            if (strcmp(historical->name,"first")==0 && strcmp(historical->id,cancelled_call)!=0)
                saw_fresh_call=1;
        }
        cc_message_cleanup(&cancelled_history[i]);
    }
    free(cancelled_history); free(cancelled_call); CHECK(saw_fresh_call);
    /* 未执行说明已持久化而账本删除失败时，核对复用该事实并拒绝改为已执行。 */
    model.requests=0;
    CHECK(ok(cc_agent_runtime_create_session(runtime,"cancelled-cleanup",".")));
    CHECK(ok(cc_cancel_source_create(&cancel_during_policy)));
    cancelled_options.cancel_token=cc_cancel_source_token(cancel_during_policy);
    journal_fault.fail_remove=1;
    cancelled_answer=NULL;
    cancelled_rc=cc_agent_runtime_handle_message_with_options(runtime,"cancelled-cleanup",
        "cancel with cleanup failure",&cancelled_options,&cancelled_answer);
    CHECK(cancelled_rc.code==CC_ERR_IO && first.calls==1 && second.calls==1);
    cc_result_free(&cancelled_rc); free(cancelled_answer);
    cc_cancel_source_destroy(cancel_during_policy); cancel_during_policy=NULL;
    CHECK(get_only_run(checkpoints,&cancelled_run));
    CHECK(ok(cc_run_checkpoint_load(checkpoints,cancelled_run,&cancelled_checkpoint)));
    cancelled_call=cc_copy_string(cancelled_checkpoint.planned_calls.items[0].id); CHECK(cancelled_call);
    cc_run_checkpoint_cleanup(&cancelled_checkpoint);
    cc_tool_result_t impossible_execution={0}; impossible_execution.ok=1; impossible_execution.text="executed";
    cancelled_rc=cc_agent_runtime_resolve_pending_tool(runtime,"cancelled-cleanup",cancelled_call,
        CC_TOOL_RECOVERY_EXECUTED,&impossible_execution);
    CHECK(cancelled_rc.code==CC_ERR_INVALID_STATE); cc_result_free(&cancelled_rc);
    journal_fault.fail_remove=0;
    CHECK(ok(cc_agent_runtime_resolve_pending_tool(runtime,"cancelled-cleanup",cancelled_call,
        CC_TOOL_RECOVERY_NOT_EXECUTED,NULL)));
    CHECK(ok(cc_agent_runtime_resume_run(runtime,cancelled_run,NULL,&cancelled_answer)) &&
          first.calls==2 && second.calls==2 && model.requests==2);
    free(cancelled_answer); free(cancelled_run); free(cancelled_call);
    first.calls=second.calls=0; model.requests=0;

    /* 结果已持久化但检查点推进失败时，重启发现结果后跳过首个已执行工具。 */
    CHECK(ok(cc_agent_runtime_create_session(runtime,"s1",".")));
    first.fail_after=1; char *answer=NULL;
    cc_result_t rc=cc_agent_runtime_handle_message(runtime,"s1","first request",&answer);
    CHECK(rc.code==CC_ERR_IO && first.calls==1 && second.calls==0); cc_result_free(&rc); free(answer);
    char *run_id=NULL; CHECK(get_only_run(checkpoints,&run_id));
    CHECK(!model.bad_request_identity && strcmp(model.last_run_id,run_id)==0 &&
          strcmp(trace.first_run,run_id)==0 && trace.starts==1 && trace.finishes==1 && !trace.bad);
    cc_agent_runtime_destroy(runtime); cc_run_checkpoint_store_destroy(checkpoints); cc_operation_journal_destroy(journal);
    sessions.vtable->destroy(sessions.self);
    fault.fail_sync=0; first.fail_after=0;
    CHECK(ok(cc_json_segmented_store_create(sessions_dir,2048,"media",1024,&sessions)));
    CHECK(ok(cc_operation_journal_open(&journal_fs,ops_dir,64,&journal)));
    CHECK(ok(cc_run_checkpoint_store_open(&wrapped,runs_dir,&checkpoints)));
    CHECK(make_runtime(&sessions,registry,journal,checkpoints,&model,bus,&runtime));
    answer=NULL; CHECK(ok(cc_agent_runtime_resume_run(runtime,run_id,NULL,&answer)));
    CHECK(answer && strcmp(answer,"finished")==0 && first.calls==1 && second.calls==1 && model.requests==2);
    CHECK(count_input(&sessions,"s1","first request")==1 && !model.bad_request_identity &&
          strcmp(model.last_run_id,run_id)==0 && strcmp(trace.first_run,run_id)==0 &&
          trace.starts==2 && trace.finishes==2 && trace.usages==2 && trace.finished==1 && !trace.bad);
    free(answer); free(run_id);

    /* 未知结果跨重启维持禁用，人工确认未执行后计划重试获取新调用 ID。 */
    first.calls=second.calls=0; model.requests=0; second.uncertain=1;
    CHECK(ok(cc_agent_runtime_create_session(runtime,"s2",".")));
    answer=NULL; rc=cc_agent_runtime_handle_message(runtime,"s2","second request",&answer);
    CHECK(rc.code==CC_ERR_INVALID_STATE && first.calls==1 && second.calls==1); cc_result_free(&rc); free(answer);
    CHECK(get_only_run(checkpoints,&run_id));
    cc_pending_operation_t *pending=NULL; size_t pending_count=0;
    CHECK(ok(cc_operation_journal_list(journal,&pending,&pending_count)) && pending_count==1);
    char *old_call=cc_copy_string(pending[0].call.id); CHECK(old_call);
    /* 账本记录丢失时，持久未知占位仍触发恢复重建禁用。 */
    CHECK(ok(cc_operation_journal_finish(journal,pending[0].operation_id)));
    cc_pending_operation_array_free(pending,pending_count);
    answer=NULL; rc=cc_agent_runtime_resume_run(runtime,run_id,NULL,&answer);
    CHECK(rc.code==CC_ERR_INVALID_STATE && first.calls==1 && second.calls==1); cc_result_free(&rc); free(answer);
    /* 无关请求仍可完成，其他工具运行，禁用工具产生拒绝结果并保持零执行。 */
    model.requests=0;
    CHECK(ok(cc_agent_runtime_create_session(runtime,"other",".")));
    answer=NULL; CHECK(ok(cc_agent_runtime_handle_message(runtime,"other","continue",&answer)));
    CHECK(answer && first.calls==2 && second.calls==1); free(answer);
    /* 模拟重试检查点已保存但旧账本未删除，继续执行须等待完成核对。 */
    journal_fault.fail_remove=1;
    rc=cc_agent_runtime_resolve_pending_tool(runtime,"s2",old_call,CC_TOOL_RECOVERY_NOT_EXECUTED,NULL);
    CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc);
    cc_tool_result_t conflicting={0}; conflicting.ok=1; conflicting.text="verified done";
    rc=cc_agent_runtime_resolve_pending_tool(runtime,"s2",old_call,CC_TOOL_RECOVERY_EXECUTED,&conflicting);
    CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    answer=NULL; rc=cc_agent_runtime_resume_run(runtime,run_id,NULL,&answer);
    CHECK(rc.code==CC_ERR_INVALID_STATE && second.calls==1); cc_result_free(&rc); free(answer);
    journal_fault.fail_remove=0;
    CHECK(ok(cc_agent_runtime_resolve_pending_tool(runtime,"s2",old_call,CC_TOOL_RECOVERY_NOT_EXECUTED,NULL)));
    CHECK(!trace.resolved_run[0] && strcmp(trace.resolved_call,old_call)==0 &&
          strncmp(trace.resolved_operation,"op-",3)==0 && !trace.bad);
    second.uncertain=0; answer=NULL; CHECK(ok(cc_agent_runtime_resume_run(runtime,run_id,NULL,&answer)));
    CHECK(answer && strcmp(answer,"finished")==0 && first.calls==2 && second.calls==2 && model.requests==3);
    CHECK(count_input(&sessions,"s2","second request")==1); free(answer); free(run_id); free(old_call);

    /* 最终答复已持久化而检查点移除失败时，恢复复用答复并保持模型调用数。 */
    first.calls=second.calls=0; model.requests=0; second.fail_remove=1;
    CHECK(ok(cc_agent_runtime_create_session(runtime,"s3",".")));
    answer=NULL; rc=cc_agent_runtime_handle_message(runtime,"s3","third request",&answer);
    CHECK(rc.code==CC_ERR_IO && model.requests==2); cc_result_free(&rc); free(answer);
    fault.fail_remove=0; second.fail_remove=0; CHECK(get_only_run(checkpoints,&run_id));
    answer=NULL; CHECK(ok(cc_agent_runtime_resume_run(runtime,run_id,NULL,&answer)));
    CHECK(answer && strcmp(answer,"finished")==0 && model.requests==2 && first.calls==1 && second.calls==1);
    free(answer); free(run_id);

    /* 人工确认已执行须先记录结论再移除账本，继续时跳过该动作。 */
    first.calls=second.calls=0; model.requests=0; second.uncertain=1;
    CHECK(ok(cc_agent_runtime_create_session(runtime,"s4",".")));
    answer=NULL; rc=cc_agent_runtime_handle_message(runtime,"s4","fourth request",&answer);
    CHECK(rc.code==CC_ERR_INVALID_STATE && first.calls==1 && second.calls==1); cc_result_free(&rc); free(answer);
    CHECK(get_only_run(checkpoints,&run_id));
    pending=NULL; pending_count=0;
    CHECK(ok(cc_operation_journal_list(journal,&pending,&pending_count)) && pending_count==1);
    old_call=cc_copy_string(pending[0].call.id); CHECK(old_call);
    cc_pending_operation_array_free(pending,pending_count);
    cc_tool_result_t observed={0}; observed.ok=1; observed.text="verified done";
    fault.fail_sync=1;
    rc=cc_agent_runtime_resolve_pending_tool(runtime,"s4",old_call,CC_TOOL_RECOVERY_EXECUTED,&observed);
    CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc);
    fault.fail_sync=0;
    rc=cc_agent_runtime_resolve_pending_tool(runtime,"s4",old_call,CC_TOOL_RECOVERY_NOT_EXECUTED,NULL);
    CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    pending=NULL; pending_count=0;
    CHECK(ok(cc_operation_journal_list(journal,&pending,&pending_count)) && pending_count==1);
    CHECK(ok(cc_operation_journal_finish(journal,pending[0].operation_id)));
    cc_pending_operation_array_free(pending,pending_count);
    second.uncertain=0; answer=NULL; CHECK(ok(cc_agent_runtime_resume_run(runtime,run_id,NULL,&answer)));
    CHECK(answer && strcmp(answer,"finished")==0 && first.calls==1 && second.calls==1 && model.requests==2);
    free(answer); free(run_id); free(old_call);

    /* 检查点已推进而账本移除失败后重启，已执行结论继续作为核对依据。 */
    first.calls=second.calls=0; model.requests=0; second.uncertain=1;
    CHECK(ok(cc_agent_runtime_create_session(runtime,"s7",".")));
    answer=NULL; rc=cc_agent_runtime_handle_message(runtime,"s7","seventh request",&answer);
    CHECK(rc.code==CC_ERR_INVALID_STATE && first.calls==1 && second.calls==1); cc_result_free(&rc); free(answer);
    CHECK(get_only_run(checkpoints,&run_id));
    pending=NULL; pending_count=0;
    CHECK(ok(cc_operation_journal_list(journal,&pending,&pending_count)) && pending_count==1);
    old_call=cc_copy_string(pending[0].call.id); CHECK(old_call);
    char *s7_operation=cc_copy_string(pending[0].operation_id); CHECK(s7_operation);
    cc_pending_operation_array_free(pending,pending_count);
    cc_media_artifact_t artifact={0}; artifact.id="artifact-1"; artifact.kind=CC_MEDIA_FILE;
    artifact.mime="text/plain"; artifact.uri="https://example.invalid/observed.txt";
    CHECK(ok(cc_media_artifact_list_append(&observed.artifacts,&artifact)));
    journal_fault.fail_remove=1;
    rc=cc_agent_runtime_resolve_pending_tool(runtime,"s7",old_call,CC_TOOL_RECOVERY_EXECUTED,&observed);
    CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc);
    CHECK(append_padding(&sessions,"s7"));
    cc_agent_runtime_destroy(runtime); cc_run_checkpoint_store_destroy(checkpoints);
    cc_operation_journal_destroy(journal); sessions.vtable->destroy(sessions.self);
    journal_fault.fail_remove=0;
    CHECK(ok(cc_json_segmented_store_create(sessions_dir,2048,"media",1024,&sessions)));
    CHECK(ok(cc_operation_journal_open(&journal_fs,ops_dir,64,&journal)));
    CHECK(ok(cc_run_checkpoint_store_open(&wrapped,runs_dir,&checkpoints)));
    CHECK(make_runtime(&sessions,registry,journal,checkpoints,&model,bus,&runtime));
    rc=cc_agent_runtime_resolve_pending_tool(runtime,"s7",old_call,CC_TOOL_RECOVERY_NOT_EXECUTED,NULL);
    CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    CHECK(ok(cc_agent_runtime_resolve_pending_tool(runtime,"s7",old_call,CC_TOOL_RECOVERY_EXECUTED,&observed)));
    CHECK(count_reconciliation(&sessions,"s7",old_call,s7_operation));
    cc_media_artifact_list_cleanup(&observed.artifacts); free(s7_operation);
    second.uncertain=0; answer=NULL; CHECK(ok(cc_agent_runtime_resume_run(runtime,run_id,NULL,&answer)));
    CHECK(answer && first.calls==1 && second.calls==1 && model.requests==2);
    free(answer); free(run_id); free(old_call);

    /* 放弃任务保留未知副作用账本，后续明确核对才解除工具名禁用。 */
    first.calls=second.calls=0; model.requests=0; second.uncertain=1;
    CHECK(ok(cc_agent_runtime_create_session(runtime,"s5",".")));
    answer=NULL; rc=cc_agent_runtime_handle_message(runtime,"s5","fifth request",&answer);
    CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc); free(answer);
    CHECK(get_only_run(checkpoints,&run_id));
    pending=NULL; pending_count=0;
    CHECK(ok(cc_operation_journal_list(journal,&pending,&pending_count)) && pending_count==1);
    old_call=cc_copy_string(pending[0].call.id); CHECK(old_call);
    cc_pending_operation_array_free(pending,pending_count);
    CHECK(ok(cc_agent_runtime_abandon_run(runtime,run_id)));
    char **remaining=NULL; size_t remaining_count=0;
    CHECK(ok(cc_agent_runtime_list_interrupted_runs(runtime,&remaining,&remaining_count)) && remaining_count==0);
    free(remaining);
    int blocked=0; CHECK(ok(cc_operation_journal_is_blocked(journal,"second",&blocked)) && blocked);
    CHECK(ok(cc_agent_runtime_resolve_pending_tool(runtime,"s5",old_call,CC_TOOL_RECOVERY_EXECUTED,&observed)));
    CHECK(ok(cc_operation_journal_is_blocked(journal,"second",&blocked)) && !blocked);
    free(run_id); free(old_call);

    /* 已持久化的未执行结论可使旧检查点生成新调用 ID，覆盖旧账本记录丢失场景。 */
    first.calls=second.calls=0; model.requests=0; second.uncertain=1;
    CHECK(ok(cc_agent_runtime_create_session(runtime,"s6",".")));
    answer=NULL; rc=cc_agent_runtime_handle_message(runtime,"s6","sixth request",&answer);
    CHECK(rc.code==CC_ERR_INVALID_STATE && first.calls==1 && second.calls==1); cc_result_free(&rc); free(answer);
    CHECK(get_only_run(checkpoints,&run_id));
    pending=NULL; pending_count=0;
    CHECK(ok(cc_operation_journal_list(journal,&pending,&pending_count)) && pending_count==1);
    old_call=cc_copy_string(pending[0].call.id); CHECK(old_call);
    fault.fail_sync=1;
    rc=cc_agent_runtime_resolve_pending_tool(runtime,"s6",old_call,CC_TOOL_RECOVERY_NOT_EXECUTED,NULL);
    CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc); fault.fail_sync=0;
    CHECK(ok(cc_operation_journal_finish(journal,pending[0].operation_id)));
    cc_pending_operation_array_free(pending,pending_count);
    second.uncertain=0; answer=NULL; CHECK(ok(cc_agent_runtime_resume_run(runtime,run_id,NULL,&answer)));
    CHECK(answer && first.calls==1 && second.calls==2 && model.requests==2);
    free(answer); free(run_id); free(old_call);

    /* 检查点工具名或参数被改写时，重用 ID 的持久结果无法证明新调用已完成，须拒绝推进。 */
    first.calls=second.calls=0; model.requests=0; first.fail_after=1;
    CHECK(ok(cc_agent_runtime_create_session(runtime,"s8",".")));
    answer=NULL; rc=cc_agent_runtime_handle_message(runtime,"s8","eighth request",&answer);
    CHECK(rc.code==CC_ERR_IO && first.calls==1 && second.calls==0); cc_result_free(&rc); free(answer);
    fault.fail_sync=0; first.fail_after=0; CHECK(get_only_run(checkpoints,&run_id));
    cc_run_checkpoint_t baseline={0}; CHECK(ok(cc_run_checkpoint_load(checkpoints,run_id,&baseline)) &&
                                               baseline.phase==CC_RUN_CHECKPOINT_TOOLS && baseline.next_call==0);
    wrong_result_session_once=1;
    answer=NULL; rc=cc_agent_runtime_resume_run(runtime,run_id,NULL,&answer);
    CHECK(rc.code==CC_ERR_STORAGE && !answer && first.calls==1 && second.calls==0 &&
          !wrong_result_session_once); cc_result_free(&rc);
    drop_intent_once=1;
    answer=NULL; rc=cc_agent_runtime_resume_run(runtime,run_id,NULL,&answer);
    CHECK(rc.code==CC_ERR_STORAGE && !answer && first.calls==1 && second.calls==0 &&
          !drop_intent_once); cc_result_free(&rc);
    for (int mismatch=0;mismatch<2;mismatch++) {
        cc_run_checkpoint_t changed={0}; CHECK(ok(cc_run_checkpoint_load(checkpoints,run_id,&changed)));
        char **field=mismatch ? &changed.planned_calls.items[0].arguments_json : &changed.planned_calls.items[0].name;
        free(*field); *field=cc_copy_string(mismatch ? "{\"changed\":1}" : "second"); CHECK(*field);
        CHECK(ok(cc_run_checkpoint_save(checkpoints,&changed))); cc_run_checkpoint_cleanup(&changed);
        answer=NULL; rc=cc_agent_runtime_resume_run(runtime,run_id,NULL,&answer);
        CHECK(rc.code==CC_ERR_STORAGE && !answer && first.calls==1 && second.calls==0); cc_result_free(&rc);
        CHECK(ok(cc_run_checkpoint_load(checkpoints,run_id,&changed)) && changed.next_call==0);
        cc_run_checkpoint_cleanup(&changed); CHECK(ok(cc_run_checkpoint_save(checkpoints,&baseline)));
    }
    cc_run_checkpoint_cleanup(&baseline);
    answer=NULL; CHECK(ok(cc_agent_runtime_resume_run(runtime,run_id,NULL,&answer)) &&
                       answer && first.calls==1 && second.calls==1);
    free(answer); free(run_id);

    /* 检查点与账本的工具名或参数不符时，核对须拒绝修改检查点。 */
    first.calls=second.calls=0; model.requests=0; second.uncertain=1;
    CHECK(ok(cc_agent_runtime_create_session(runtime,"s9",".")));
    answer=NULL; rc=cc_agent_runtime_handle_message(runtime,"s9","ninth request",&answer);
    CHECK(rc.code==CC_ERR_INVALID_STATE && first.calls==1 && second.calls==1); cc_result_free(&rc); free(answer);
    CHECK(get_only_run(checkpoints,&run_id));
    pending=NULL; pending_count=0; CHECK(ok(cc_operation_journal_list(journal,&pending,&pending_count)) && pending_count==1);
    old_call=cc_copy_string(pending[0].call.id); CHECK(old_call);
    cc_pending_operation_array_free(pending,pending_count);
    memset(&baseline,0,sizeof(baseline)); CHECK(ok(cc_run_checkpoint_load(checkpoints,run_id,&baseline)) &&
        baseline.phase==CC_RUN_CHECKPOINT_TOOLS && baseline.next_call<baseline.planned_calls.count);
    for (int mismatch=0;mismatch<2;mismatch++) {
        cc_run_checkpoint_t changed={0}; CHECK(ok(cc_run_checkpoint_load(checkpoints,run_id,&changed)));
        cc_tool_call_t *planned=&changed.planned_calls.items[changed.next_call];
        char **field=mismatch ? &planned->arguments_json : &planned->name;
        free(*field); *field=cc_copy_string(mismatch ? "{\"changed\":1}" : "first"); CHECK(*field);
        CHECK(ok(cc_run_checkpoint_save(checkpoints,&changed))); cc_run_checkpoint_cleanup(&changed);
        rc=cc_agent_runtime_resolve_pending_tool(runtime,"s9",old_call,CC_TOOL_RECOVERY_EXECUTED,&observed);
        CHECK(rc.code==CC_ERR_STORAGE && first.calls==1 && second.calls==1); cc_result_free(&rc);
        CHECK(ok(cc_operation_journal_is_blocked(journal,"second",&blocked)) && blocked &&
              ok(cc_run_checkpoint_load(checkpoints,run_id,&changed)) && changed.next_call==baseline.next_call);
        cc_run_checkpoint_cleanup(&changed); CHECK(ok(cc_run_checkpoint_save(checkpoints,&baseline)));
    }
    cc_run_checkpoint_cleanup(&baseline);
    CHECK(ok(cc_agent_runtime_resolve_pending_tool(runtime,"s9",old_call,CC_TOOL_RECOVERY_EXECUTED,&observed)));
    second.uncertain=0; answer=NULL; CHECK(ok(cc_agent_runtime_resume_run(runtime,run_id,NULL,&answer)) &&
                                           answer && first.calls==1 && second.calls==1);
    free(answer); free(run_id); free(old_call);

    /* 派发后查到错会话结果须停止，后续正确查找恢复持久结果并保持执行计数。 */
    first.calls=second.calls=0; model.requests=0; wrong_result_session_once=1;
    CHECK(ok(cc_agent_runtime_create_session(runtime,"s10",".")));
    answer=NULL; rc=cc_agent_runtime_handle_message(runtime,"s10","tenth request",&answer);
    CHECK(rc.code==CC_ERR_STORAGE && first.calls==1 && second.calls==0 &&
          !wrong_result_session_once); cc_result_free(&rc); free(answer);
    CHECK(get_only_run(checkpoints,&run_id));
    answer=NULL; CHECK(ok(cc_agent_runtime_resume_run(runtime,run_id,NULL,&answer)) &&
                       answer && first.calls==1 && second.calls==1);
    free(answer); free(run_id);

    cc_agent_runtime_destroy(runtime); CHECK(ok(cc_event_bus_unsubscribe(bus,subscription,1000)));
    cc_event_bus_destroy(bus); cc_tool_registry_destroy(registry);
    sessions.vtable->destroy(sessions.self); cc_operation_journal_destroy(journal);
    cc_run_checkpoint_store_destroy(checkpoints); CHECK(remove_tree(&fs,root));
    fs.vtable->destroy(fs.self); free(root); return 0;
}
