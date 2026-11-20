#include "cc/adapters/cc_recovery_bootstrap.h"
#include "cc/adapters/cc_json_segmented_history.h"
#include "cc/core/cc_id.h"
#include "cc/internal/cc_alloc.h"
#include "../../adapters/src/storage/cc_json_syntax_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief 创建测试分段后端。
 * @param[in] root_path 根路径借用。
 * @param[in] segment_bytes 分段阈值。
 * @param[in] media_dir 媒体目录借用。
 * @param[in] max_base64_bytes 回填预算。
 * @param[out] out_store 新槽，成功后最终 destroy。
 * @return 创建结果，测试消费诊断。
 */
extern cc_result_t cc_json_segmented_store_create(const char *root_path, size_t segment_bytes,
    const char *media_dir, size_t max_base64_bytes, cc_session_store_t *out_store);
/** @brief 创建不支持恢复遍历的内存对照后端。
 * @param[out] out_store 新槽，成功后最终 destroy。
 * @return 创建结果，测试消费诊断。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)
/** @brief 消费结果并检查成功。
 * @param[in] rc 按值移入拥有结果。
 * @return 成功一，否则报告并零，串行调用。
 */
static int ok(cc_result_t rc) { int good=rc.code==CC_OK; if (!good) fprintf(stderr,"%s\n",rc.message ? rc.message : "error"); cc_result_free(&rc); return good; }
/** @brief 结束无资源 provider 生命周期。
 * @param[in] self 未解引用的哨兵借用，不执行释放，调用者已排空借用。
 */
static void provider_destroy(void *self) { (void)self; }
static const cc_llm_provider_vtable_t provider_vt={NULL,NULL,provider_destroy,NULL};
/** @brief 创建无聊天能力的装配测试 provider。
 * @param[in] c 未使用配置借用。
 * @param[in] h 未使用 HTTP 端口借用。
 * @param[out] out 必填新槽，接收静态表及无资源哨兵。
 * @return 始终成功，装配 owner 最终 destroy，串行调用。
 */
static cc_result_t provider_create(const cc_config_t *c,cc_http_client_t *h,cc_llm_provider_t *out)
{ (void)c; (void)h; out->self=(void *)1; out->vtable=&provider_vt; return cc_result_ok(); }
/** @brief 用配置路径创建测试分段后端。
 * @param[in] config 稳定配置借用。
 * @param[out] out 新端口槽，成功归 builder destroy。
 * @return 创建原结果，串行装配。
 */
static cc_result_t segmented_create(const cc_config_t *config,cc_session_store_t *out)
{ return cc_json_segmented_store_create(config->storage_path,2048,"media",1024,out); }
/** @brief 创建恢复不支持场景的内存端口。
 * @param[in] config 未使用借用。
 * @param[out] out 新端口槽，成功归 builder destroy。
 * @return 创建原结果，串行装配。
 */
static cc_result_t memory_create(const cc_config_t *config,cc_session_store_t *out)
{ (void)config; return cc_memory_session_store_create(out); }
/** @brief 为测试调用写入允许策略。
 * @param[in] self 未使用借用。
 * @param[in] call 未使用调用借用。
 * @param[in] ctx 未使用上下文借用。
 * @param[out] out 必填空槽，先清零再允许，不产生拥有资源。
 * @return 始终成功，串行调用。
 */
static cc_result_t allow(void *self,const cc_tool_call_t *call,const cc_tool_context_t *ctx,cc_policy_decision_t *out)
{ (void)self; (void)call; (void)ctx; memset(out,0,sizeof(*out)); out->allowed=1; return cc_result_ok(); }
static const cc_policy_engine_vtable_t policy_vt={allow,NULL};
/** @brief 安装静态无状态策略表。
 * @param[in] config 未使用配置借用。
 * @param[out] out 必填新槽，清零并接收静态表借用。
 * @return 始终成功，串行装配，无释放责任。
 */
static cc_result_t policy_create(const cc_config_t *config,cc_policy_engine_t *out)
{ (void)config; memset(out,0,sizeof(*out)); out->vtable=&policy_vt; return cc_result_ok(); }
typedef struct tool { const char *name; int calls; } tool_t;
static tool_t act={"act",0},other={"other",0};
/** @brief 借用工具名。
 * @param[in] self 存活静态工具借用。
 * @return 名称借用，覆盖测试寿命，无分配。
 */
static const char *name(void *self) { return ((tool_t *)self)->name; }
/** @brief 返回固定描述。
 * @param[in] self 未使用借用。
 * @return 进程期静态文本借用。
 */
static const char *description(void *self) { (void)self; return "test"; }
/** @brief 返回固定空 schema。
 * @param[in] self 未使用借用。
 * @return 进程期静态 JSON 借用。
 */
static const char *schema(void *self) { (void)self; return "{}"; }
/** @brief 计数实际执行并产生拥有结果。
 * @param[in,out] self 必填独占静态工具借用。
 * @param[in] args 未使用参数借用。
 * @param[in] ctx 未使用上下文借用。
 * @param[out] out 必填空槽，成功 text 由 runtime cleanup。
 * @return 成功或 OOM，失败计数仍增加，串行调用。
 */
static cc_result_t call_tool(void *self,const char *args,const cc_tool_context_t *ctx,cc_tool_result_t *out)
{ (void)args; (void)ctx; ((tool_t *)self)->calls++; out->ok=1; out->text=cc_copy_string("done"); return out->text ? cc_result_ok() : cc_result_error(CC_ERR_OUT_OF_MEMORY,"no result"); }
static const cc_tool_vtable_t tool_vt={name,description,schema,call_tool,NULL};
/** @brief 发布 act 静态工具借用。
 * @param[in] ctx 未使用工厂上下文借用。
 * @param[out] out 必填新槽，self/table 为静态借用，无销毁资源。
 * @return 始终成功，串行装配。
 */
static cc_result_t make_act(const cc_runtime_tool_factory_ctx_t *ctx,cc_tool_t *out)
{ (void)ctx; out->self=&act; out->vtable=&tool_vt; return cc_result_ok(); }
/** @brief 发布 other 静态工具借用。
 * @param[in] ctx 未使用工厂上下文借用。
 * @param[out] out 必填新槽，self/table 为静态借用，无销毁资源。
 * @return 始终成功，串行装配。
 */
static cc_result_t make_other(const cc_runtime_tool_factory_ctx_t *ctx,cc_tool_t *out)
{ (void)ctx; out->self=&other; out->vtable=&tool_vt; return cc_result_ok(); }
/** @brief 串行递归清理独占测试树。
 * @param[in] fs 存活文件系统端口借用。
 * @param[in] path 测试范围内路径借用。
 * @return 全部成功一，否则零，列名、路径和诊断内部释放。
 */
static int remove_tree(cc_filesystem_t *fs,const char *path)
{
    cc_filesystem_stat_t info={.size=sizeof(info)}; if (!ok(fs->vtable->stat(fs->self,path,&info))) return 0;
    if (info.type==CC_FS_ENTRY_DIRECTORY) {
        char **names=NULL; size_t count=0; if (!ok(fs->vtable->list_dir(fs->self,path,&names,&count))) return 0;
        int good=1; for (size_t i=0;i<count;i++) { size_t n=strlen(path)+strlen(names[i])+2; char *child=malloc(n);
            if (!child) good=0; else { snprintf(child,n,"%s/%s",path,names[i]); if (!remove_tree(fs,child)) good=0; free(child); } free(names[i]); }
        free(names); if (!good) return 0;
    }
    return ok(fs->vtable->remove(fs->self,path));
}
/** @brief 验证启动历史导入、同名工具禁用、失败强制账本和语法分类。
 * @return 全部通过零，否则一，成功清理全部租约/装配/目录，早退交进程回收。
 * @note 静态假工具和 provider 仅由本测试线程访问。
 */
int main(void)
{
    const char *valid_json[]={"null", " [1,-0.25e+4,true,false,null] ",
        "{\"x\":[\"escaped\\n\",{},[]],\"y\":\"\\u0020\"}"};
    const char *invalid_json[]={"{\n", "[1,]", "{\"x\":}", "[true false]", "\"\\q\"", "01"};
    for (size_t i=0;i<sizeof(valid_json)/sizeof(valid_json[0]);i++)
        CHECK(cc_json_syntax_check(valid_json[i])==CC_JSON_SYNTAX_VALID);
    for (size_t i=0;i<sizeof(invalid_json)/sizeof(invalid_json[0]);i++)
        CHECK(cc_json_syntax_check(invalid_json[i])==CC_JSON_SYNTAX_INVALID);
    char deeply_nested[132];
    memset(deeply_nested,'[',65); memset(deeply_nested+65,']',65); deeply_nested[130]='\0';
    CHECK(cc_json_syntax_check(deeply_nested)==CC_JSON_SYNTAX_INCONCLUSIVE);
    cc_filesystem_t fs={0}; CHECK(ok(cc_filesystem_get_default(&fs)));
    char *root=NULL; CHECK(ok(cc_id_generate_uuid_v4("recovery-boot-",&root)));
    char sessions[160],operations[160],runs[160],missing[160];
    snprintf(sessions,sizeof(sessions),"%s/sessions",root); snprintf(operations,sizeof(operations),"%s/ops",root);
    snprintf(runs,sizeof(runs),"%s/runs",root); snprintf(missing,sizeof(missing),"%s/missing",root);
    CHECK(ok(fs.vtable->make_dir(fs.self,operations)) && ok(fs.vtable->make_dir(fs.self,runs)));
    cc_config_t config={0}; config.provider="fake"; config.model="fake"; config.data_dir=root; config.workspace_path="."; config.storage_path=sessions;
    const cc_llm_provider_descriptor_t providers[]={{"fake",1,provider_create}};
    const cc_tool_descriptor_t tools[]={{"act",NULL,1,make_act},{"other",NULL,1,make_other}};
    cc_runtime_feature_set_t features={0}; features.llm_providers=providers; features.llm_provider_count=1;
    features.tools=tools; features.tool_count=2; features.create_policy_engine=policy_create; features.create_session_store=segmented_create;
    cc_runtime_builder_t *builder=NULL; CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)));
    cc_runtime_generation_t *generation=NULL; CHECK(ok(cc_runtime_builder_acquire_generation(builder,&generation)));
    cc_agent_runtime_t *runtime=cc_runtime_generation_runtime(generation); cc_session_store_t *store=cc_agent_runtime_session_store(runtime);
    CHECK(cc_json_segmented_store_is_instance(store) && ok(store->vtable->create_session(store->self,"old",".")));
    cc_tool_call_t prior={"call-1","act","{}"}; cc_message_t *message=NULL;
    CHECK(ok(cc_message_create_text("intent-1","old",CC_ROLE_ASSISTANT,"",NULL,&message)) && ok(cc_message_add_tool_call(message,&prior)));
    cc_session_record_t record={0}; record.type=CC_SESSION_RECORD_MESSAGE; record.session_id="old"; record.data.message=message;
    CHECK(ok(store->vtable->append_records(store->self,&record,1))); cc_message_destroy(message);
    cc_runtime_generation_release(generation);
    cc_recovery_bootstrap_t *recovery=NULL; size_t unresolved=0;
    CHECK(ok(cc_recovery_bootstrap_open(builder,&fs,operations,runs,64,&recovery,&unresolved)) && unresolved==1);
    int blocked=0; CHECK(ok(cc_operation_journal_is_blocked(cc_recovery_bootstrap_journal(recovery),"act",&blocked)) && blocked);
    cc_result_t rc=cc_runtime_builder_set_operation_journal(builder,NULL,1); CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    CHECK(ok(cc_runtime_builder_acquire_generation(builder,&generation))); runtime=cc_runtime_generation_runtime(generation);
    CHECK(ok(cc_agent_runtime_create_session(runtime,"new",".")));
    cc_tool_call_t blocked_call={"new-act","act","{}"}; cc_tool_result_t result={0};
    CHECK(ok(cc_agent_runtime_execute_manual_tool(runtime,"new",&blocked_call,&result)) && !result.ok && act.calls==0);
    cc_tool_result_cleanup(&result);
    cc_tool_call_t allowed_call={"new-other","other","{}"};
    CHECK(ok(cc_agent_runtime_execute_manual_tool(runtime,"new",&allowed_call,&result)) && result.ok && other.calls==1);
    cc_tool_result_cleanup(&result); cc_runtime_generation_release(generation);
    CHECK(ok(cc_runtime_builder_destroy(builder,1000))); cc_recovery_bootstrap_destroy(recovery);

    /* 缺失账本目录或不支持的会话后端仍须先进入强制账本状态再报告错误。 */
    features.create_session_store=memory_create; builder=NULL;
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)));
    recovery=NULL; rc=cc_recovery_bootstrap_open(builder,&fs,missing,runs,64,&recovery,&unresolved);
    CHECK(rc.code!=CC_OK && !recovery); cc_result_free(&rc);
    CHECK(ok(cc_runtime_builder_acquire_generation(builder,&generation))); runtime=cc_runtime_generation_runtime(generation);
    CHECK(ok(cc_agent_runtime_create_session(runtime,"new",".")));
    memset(&result,0,sizeof(result)); rc=cc_agent_runtime_execute_manual_tool(runtime,"new",&allowed_call,&result);
    CHECK(rc.code==CC_ERR_INVALID_STATE && other.calls==1); cc_result_free(&rc);
    cc_tool_result_cleanup(&result); cc_runtime_generation_release(generation);
    cc_recovery_bootstrap_failure_t failure=CC_RECOVERY_BOOTSTRAP_FAILURE_HISTORY_INTEGRITY;
    rc=cc_recovery_bootstrap_open_detailed(builder,&fs,operations,runs,64,&recovery,&unresolved,&failure);
    CHECK(rc.code==CC_ERR_UNSUPPORTED && !recovery && failure==CC_RECOVERY_BOOTSTRAP_FAILURE_OTHER); cc_result_free(&rc);
    CHECK(ok(cc_runtime_builder_destroy(builder,1000)) && remove_tree(&fs,root));
    fs.vtable->destroy(fs.self); free(root); return 0;
}
