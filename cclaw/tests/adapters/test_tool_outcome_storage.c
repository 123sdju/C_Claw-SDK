#include "cc/core/cc_id.h"
#include "cc/core/cc_message.h"
#include "cc/adapters/cc_json_file_session_store.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_session_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if CC_STORAGE_SQLITE
#include <sqlite3.h>
#endif

/** @brief 创建测试内存后端。
 * @param[out] out_store 新端口槽，成功最终 destroy。
 * @return 创建结果，测试串行消费诊断。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);
#if CC_STORAGE_SQLITE
/** @brief 创建独占测试 SQLite 后端。
 * @param[in] path 数据库路径借用。
 * @param[out] out_store 新端口槽，成功最终 destroy。
 * @return 创建结果，测试串行消费诊断。
 */
extern cc_result_t cc_sqlite_session_store_create(const char *path, cc_session_store_t *out_store);
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)
/** @brief 检查并消费独占结果。
 * @param[in] rc 按值移入的结果，诊断在内部释放。
 * @return 成功一，失败报告后零，串行调用。
 */
static int ok(cc_result_t rc) { int good=rc.code==CC_OK; if (!good) fprintf(stderr,"%s\n",rc.message ? rc.message : "error"); cc_result_free(&rc); return good; }

/** @brief 创建固定 s1 的带状态消息，同步保存后回收。
 * @param[in] store 存活后端借用。
 * @param[in] id 消息名借用。
 * @param[in] role 角色。
 * @param[in] call 可为 NULL 的调用 ID 借用。
 * @param[in] outcome 工具状态。
 * @return 全部成功一，否则零，临时消息和诊断内部消费，串行调用。
 */
static int add(cc_session_store_t *store,const char *id,cc_message_role_t role,
               const char *call,cc_message_tool_outcome_t outcome)
{
    cc_message_t *msg=NULL; cc_result_t rc=cc_message_create_text(id,"s1",role,"message",call,&msg);
    if (rc.code==CC_OK) {
        msg->tool_outcome=outcome;
        cc_session_record_t record={0}; record.type=CC_SESSION_RECORD_MESSAGE; record.session_id="s1"; record.data.message=msg;
        rc=store->vtable->append_records(store->self,&record,1);
    }
    cc_message_destroy(msg); return ok(rc);
}

/** @brief 核对三种持久工具状态，并检查 provider JSON 隐去内部状态。
 * @param[in] store 存活测试后端借用，测试期间无并发写入。
 * @return 所需状态全部正确为一，否则零，临时消息数组及 JSON 内部释放。
 */
static int check_messages(cc_session_store_t *store)
{
    cc_message_t *messages=NULL; size_t count=0; int unknown=0,executed=0,not_executed=0,success=1;
    if (!ok(store->vtable->load_messages(store->self,"s1",50,&messages,&count))) return 0;
    for (size_t i=0;i<count;i++) {
        cc_message_t *m=&messages[i];
        if (strcmp(m->id,"unknown")==0) unknown=(m->tool_outcome==CC_MESSAGE_TOOL_OUTCOME_UNKNOWN && cc_message_tool_outcome_is_unknown(m));
        if (strcmp(m->id,"executed")==0) executed=(m->tool_outcome==CC_MESSAGE_TOOL_OUTCOME_RECONCILED_EXECUTED && m->tool_call_id && strcmp(m->tool_call_id,"c1")==0);
        if (strcmp(m->id,"not-executed")==0) not_executed=(m->tool_outcome==CC_MESSAGE_TOOL_OUTCOME_RECONCILED_NOT_EXECUTED);
        char *json=NULL; if (!ok(cc_message_to_json(m,0,&json)) || strstr(json,"tool_outcome")) success=0;
        free(json); cc_message_cleanup(m);
    }
    free(messages); return success && unknown && executed && not_executed;
}

/** @brief 创建 s1 并保存三种状态，核对全量和最新条数语义。
 * @param[in] store 独占测试端口借用。
 * @return 全部通过一，否则零，输出消息内部清理，已有持久内容由测试结束清除。
 */
static int populate(cc_session_store_t *store)
{
    if (!(ok(store->vtable->create_session(store->self,"s1",".")) &&
        add(store,"unknown",CC_ROLE_TOOL,"c1",CC_MESSAGE_TOOL_OUTCOME_UNKNOWN) &&
        add(store,"executed",CC_ROLE_USER,"c1",CC_MESSAGE_TOOL_OUTCOME_RECONCILED_EXECUTED) &&
        add(store,"not-executed",CC_ROLE_USER,"c2",CC_MESSAGE_TOOL_OUTCOME_RECONCILED_NOT_EXECUTED) &&
        check_messages(store))) return 0;
    cc_message_t *messages=NULL; size_t count=0;
    if (!ok(store->vtable->load_messages(store->self,"s1",1,&messages,&count))) return 0;
    int recent=count==1 && messages[0].id && strcmp(messages[0].id,"not-executed")==0;
    for (size_t i=0;i<count;i++) cc_message_cleanup(&messages[i]);
    free(messages); return recent;
}

/** @brief 核对内存、JSON 和可选 SQLite 的当前格式工具状态往返与损坏拒绝。
 * @return 全部通过零，否则一，成功清理各端口/输出/文件，早退交进程回收，串行执行。
 */
int main(void)
{
    cc_filesystem_t fs={0}; CHECK(ok(cc_filesystem_get_default(&fs)));
    char *root=NULL; CHECK(ok(cc_id_generate_uuid_v4("outcome-store-",&root)));
    CHECK(ok(fs.vtable->make_dir(fs.self,root)));
    cc_session_store_t store={0}; CHECK(ok(cc_memory_session_store_create(&store)) && populate(&store));
    store.vtable->destroy(store.self);
    char path[160]; snprintf(path,sizeof(path),"%s/sessions.json",root);
    CHECK(ok(cc_json_file_store_create(path,&store)));
    cc_session_t *listed=NULL; size_t listed_count=0;
    CHECK(ok(store.vtable->list_sessions(store.self,&listed,&listed_count)) && !listed && listed_count==0);
    CHECK(populate(&store));
    cc_message_t *wrong=NULL;
    CHECK(ok(cc_message_create_text("wrong","s2",CC_ROLE_USER,"data",NULL,&wrong)));
    cc_session_record_t wrong_record={0};
    wrong_record.type=CC_SESSION_RECORD_MESSAGE; wrong_record.session_id="s1"; wrong_record.data.message=wrong;
    cc_result_t invalid=store.vtable->append_records(store.self,&wrong_record,1);
    CHECK(invalid.code==CC_ERR_INVALID_ARGUMENT); cc_result_free(&invalid);
    wrong_record.session_id="s2";
    invalid=store.vtable->append_records(store.self,&wrong_record,1);
    CHECK(invalid.code==CC_ERR_STORAGE); cc_result_free(&invalid); cc_message_destroy(wrong);
    CHECK(check_messages(&store));
    CHECK(ok(store.vtable->list_sessions(store.self,&listed,&listed_count)) && listed_count==1 &&
          strcmp(listed[0].id,"s1")==0 && strcmp(listed[0].name,"s1")==0 &&
          strcmp(listed[0].workspace_dir,".")==0);
    cc_session_free_array(listed,listed_count);
    cc_message_t *timed=NULL;
    CHECK(ok(cc_message_create_text("timed","s1",CC_ROLE_USER,"with timestamp",NULL,&timed)));
    const char *timestamp="2026-11-09T08:00:00Z";
    timed->created_at=malloc(strlen(timestamp)+1U); CHECK(timed->created_at);
    strcpy(timed->created_at,timestamp);
    cc_session_record_t timed_record={0};
    timed_record.type=CC_SESSION_RECORD_MESSAGE; timed_record.session_id="s1"; timed_record.data.message=timed;
    CHECK(ok(store.vtable->append_records(store.self,&timed_record,1)));
    cc_message_destroy(timed);
    store.vtable->destroy(store.self); CHECK(ok(cc_json_file_store_create(path,&store)) && check_messages(&store));
    cc_message_t *recent=NULL; size_t recent_count=0;
    CHECK(ok(store.vtable->load_messages(store.self,"s1",1,&recent,&recent_count)) && recent_count==1 &&
          recent[0].created_at && strcmp(recent[0].created_at,timestamp)==0);
    cc_message_cleanup(&recent[0]); free(recent);
    cc_message_t *complex=NULL;
    CHECK(ok(cc_message_create_text("complex","s1",CC_ROLE_ASSISTANT,"look",NULL,&complex)));
    cc_media_artifact_t image={.id="image-1",.kind=CC_MEDIA_IMAGE,.mime="image/png",.path="old/image.png",
        .uri="https://example.invalid/image",.data_base64="aW1hZ2U="};
    cc_media_artifact_t missing_image={.id="missing-image",.kind=CC_MEDIA_IMAGE,.mime="image/png"};
    CHECK(ok(cc_content_parts_append_artifact(&complex->content,&image,CC_CONTENT_PART_OUTPUT)) &&
          ok(cc_content_parts_append_artifact(&complex->content,&missing_image,CC_CONTENT_PART_OUTPUT)) &&
          ok(cc_tool_call_list_append_values(&complex->tool_calls,"call-1","camera","{}")));
    cc_session_record_t complex_record={.type=CC_SESSION_RECORD_MESSAGE,.session_id="s1",.data.message=complex};
    CHECK(ok(store.vtable->append_records(store.self,&complex_record,1))); cc_message_destroy(complex);
    recent=NULL; recent_count=0;
    CHECK(ok(store.vtable->load_messages(store.self,"s1",1,&recent,&recent_count)) && recent_count==1 &&
          recent[0].content.count==3 && recent[0].content.items[1].artifact.data_base64 &&
          strcmp(recent[0].content.items[1].artifact.data_base64,"aW1hZ2U=")==0 && recent[0].tool_calls.count==1 &&
          strcmp(recent[0].tool_calls.items[0].name,"camera")==0);
    cc_message_cleanup(&recent[0]); free(recent);
    /* 当前格式多会话交错追加后，尾页和完整历史仍按会话隔离。 */
    CHECK(ok(store.vtable->create_session(store.self,"s2",".")));
    const char *interleaved_sessions[]={"s1","s2","s1","s2"};
    const char *interleaved_ids[]={"a1","b1","a2","b2"};
    for (size_t i=0;i<4;i++) {
        cc_message_t *message=NULL;
        CHECK(ok(cc_message_create_text(interleaved_ids[i],interleaved_sessions[i],CC_ROLE_USER,
                                        interleaved_ids[i],NULL,&message)));
        cc_session_record_t record={.type=CC_SESSION_RECORD_MESSAGE,.session_id=interleaved_sessions[i],
                                    .data.message=message};
        CHECK(ok(store.vtable->append_records(store.self,&record,1)));
        cc_message_destroy(message);
    }
    recent=NULL; recent_count=0;
    CHECK(ok(store.vtable->load_messages(store.self,"s1",2,&recent,&recent_count)) &&
          recent_count==2 && strcmp(recent[0].id,"a1")==0 && strcmp(recent[1].id,"a2")==0);
    for (size_t i=0;i<recent_count;i++) cc_message_cleanup(&recent[i]);
    free(recent);
    recent=NULL; recent_count=0;
    CHECK(ok(store.vtable->load_messages(store.self,"s2",-1,&recent,&recent_count)) &&
          recent_count==2 && strcmp(recent[0].id,"b1")==0 && strcmp(recent[1].id,"b2")==0);
    for (size_t i=0;i<recent_count;i++) cc_message_cleanup(&recent[i]);
    free(recent);
    const char *metadata="{\"sessions\":[{\"id\":\"s1\",\"name\":\"Project Alpha\",\"workspace_dir\":\"/workspace\",\"model\":\"model-a\",\"status\":\"completed\",\"created_at\":\"created\",\"updated_at\":\"updated\"}],\"messages\":[{\"id\":\"message\",\"session_id\":\"s1\",\"role\":\"user\",\"content_parts\":\"[]\",\"tool_calls\":\"[]\",\"created_at\":\"old-time\",\"custom\":{\"nested\":[1,2]}}],\"tool_calls\":[{\"id\":\"a\",\"session_id\":\"s1\",\"custom_call\":23}],\"tool_results\":[{\"id\":\"a\",\"session_id\":\"s1\",\"ok\":true,\"custom_result\":{\"v\":1}}]}";
    CHECK(ok(fs.vtable->write_text(fs.self,path,metadata)));
    listed=NULL; listed_count=0;
    CHECK(ok(store.vtable->list_sessions(store.self,&listed,&listed_count)) && listed_count==1 &&
          strcmp(listed[0].name,"Project Alpha")==0 && strcmp(listed[0].workspace_dir,"/workspace")==0 &&
          listed[0].status==CC_SESSION_COMPLETED && strcmp(listed[0].model,"model-a")==0 &&
          strcmp(listed[0].created_at,"created")==0 && strcmp(listed[0].updated_at,"updated")==0);
    cc_session_free_array(listed,listed_count);
    recent=NULL; recent_count=0;
    CHECK(ok(store.vtable->load_messages(store.self,"s1",-1,&recent,&recent_count)) && recent_count==1 &&
          recent[0].created_at && strcmp(recent[0].created_at,"old-time")==0 &&
          recent[0].content.count==0);
    cc_message_cleanup(&recent[0]); free(recent);
    CHECK(ok(store.vtable->clear_session(store.self,"another-session")));
    recent=NULL; recent_count=0;
    CHECK(ok(store.vtable->load_messages(store.self,"s1",-1,&recent,&recent_count)) && recent_count==1 &&
          recent[0].created_at && strcmp(recent[0].created_at,"old-time")==0 &&
          recent[0].content.count==0);
    cc_message_cleanup(&recent[0]); free(recent);
    char *preserved_metadata=NULL;
    CHECK(ok(fs.vtable->read_text(fs.self,path,&preserved_metadata)) && strstr(preserved_metadata,"\"custom\"") &&
          strstr(preserved_metadata,"\"custom_call\"") && strstr(preserved_metadata,"\"custom_result\""));
    free(preserved_metadata);
    char sidecar[170]; snprintf(sidecar,sizeof(sidecar),"%s.tmp",path);
    CHECK(ok(fs.vtable->write_text(fs.self,sidecar,"unfinished session data")));
    listed=NULL; listed_count=0;
    cc_result_t rc=store.vtable->list_sessions(store.self,&listed,&listed_count);
    CHECK(rc.code==CC_ERR_STORAGE && !listed && listed_count==0); cc_result_free(&rc);
    rc=store.vtable->create_session(store.self,"new-session",".");
    CHECK(rc.code==CC_ERR_STORAGE); cc_result_free(&rc);
    CHECK(ok(fs.vtable->remove(fs.self,sidecar)));
    CHECK(ok(store.vtable->list_sessions(store.self,&listed,&listed_count)) && listed_count==1);
    cc_session_free_array(listed,listed_count);
    const char *bad_files[]={
        "{\"sessions\":[{\"id\":\"s1\"}],\"messages\":[{\"id\":\"old\",\"session_id\":\"s1\",\"role\":\"user\",\"content\":\"old text\"}]}",
        "{\"sessions\":[],\"SESSIONS\":[],\"messages\":[]}",
        "{\"sessions\":[{\"id\":\"s1\",\"ID\":\"other\"}],\"messages\":[]}",
        "{\"sessions\":[],\"messages\":[{\"id\":\"x\",\"session_id\":\"s1\",\"role\":\"user\",\"content\":7}]}",
        "{\"sessions\":[],\"messages\":[{\"id\":\"x\",\"session_id\":\"s1\",\"role\":\"assistant\",\"content_parts\":\"[null]\",\"tool_calls\":\"[]\"}]}",
        "{\"sessions\":[],\"messages\":[{\"id\":\"x\",\"session_id\":\"s1\",\"role\":\"assistant\",\"content_parts\":\"[]\",\"tool_calls\":\"[{}]\"}]}",
        "{\"sessions\":[{\"id\":\"s1\"}],\"messages\":[{\"id\":\"x\",\"session_id\":\"s1\",\"role\":\"assistant\",\"content_parts\":\"[]\",\"tool_calls\":\"[{\\\"id\\\":\\\"a\\\",\\\"function\\\":{\\\"name\\\":\\\"write\\\",\\\"arguments\\\":\\\"{}\\\"}},{\\\"id\\\":\\\"a\\\",\\\"function\\\":{\\\"name\\\":\\\"write\\\",\\\"arguments\\\":\\\"{}\\\"}}]\"}]}",
        "{\"sessions\":[{\"id\":\"s1\"}],\"messages\":[{\"id\":\"x\",\"session_id\":\"s1\",\"role\":\"user\",\"content_parts\":\"[{\\\"type\\\":\\\"file\\\",\\\"artifact\\\":{\\\"kind\\\":\\\"file\\\",\\\"bytes\\\":1e99,\\\"data_base64\\\":\\\"Zg==\\\"}}]\",\"tool_calls\":\"[]\"}]}",
        "{\"sessions\":[{\"id\":\"s1\"}],\"messages\":[{\"id\":\"x\",\"session_id\":\"s1\",\"role\":\"user\",\"content_parts\":\"[{\\\"type\\\":\\\"image\\\",\\\"artifact\\\":{\\\"kind\\\":\\\"audio\\\",\\\"data_base64\\\":\\\"Zg==\\\"}}]\",\"tool_calls\":\"[]\"}]}",
        "{\"sessions\":[{\"id\":\"s1\"}],\"messages\":[{\"id\":\"x\",\"session_id\":\"s1\",\"role\":\"user\",\"content_parts\":\"[{\\\"type\\\":\\\"file\\\",\\\"artifact\\\":{\\\"path\\\":\\\"one\\\",\\\"PATH\\\":\\\"two\\\"}}]\",\"tool_calls\":\"[]\"}]}",
        "{\"sessions\":[],\"messages\":[{\"id\":\"x\",\"session_id\":\"s1\",\"role\":\"assistant\",\"content_parts\":\"[]\"}]}",
        "{\"sessions\":[],\"messages\":[{\"id\":\"x\\u0000hidden\",\"session_id\":\"s1\",\"role\":\"user\",\"content\":\"text\"}]}",
        "{\"sessions\":[],\"messages\":[{\"id\":\"older\",\"session_id\":\"s2\",\"role\":\"user\",\"content\":12},{\"id\":\"recent\",\"session_id\":\"s1\",\"role\":\"user\",\"content\":\"ok\"}]}",
        "{\"sessions\":[{\"id\":\"s1\"}],\"messages\":[{\"id\":\"orphan\",\"session_id\":\"s2\",\"role\":\"user\",\"content\":\"must be preserved\"}]}",
        "{\"sessions\":[{\"id\":\"s1\"}],\"messages\":[],\"tool_calls\":[{\"id\":\"call\",\"session_id\":\"s2\"}]}"
    };
    for (size_t i=0;i<sizeof(bad_files)/sizeof(bad_files[0]);i++) {
        CHECK(ok(fs.vtable->write_text(fs.self,path,bad_files[i])));
        listed=NULL; listed_count=0;
        rc=store.vtable->list_sessions(store.self,&listed,&listed_count);
        CHECK(rc.code!=CC_OK && !listed && listed_count==0); cc_result_free(&rc);
        rc=store.vtable->create_session(store.self,"unrelated",".");
        CHECK(rc.code!=CC_OK); cc_result_free(&rc);
        cc_message_t *page=NULL; size_t page_count=0;
        rc=store.vtable->load_messages(store.self,"s1",1,&page,&page_count);
        CHECK(rc.code!=CC_OK && !page && page_count==0); cc_result_free(&rc);
        char *original=NULL;
        CHECK(ok(fs.vtable->read_text(fs.self,path,&original)) && strcmp(original,bad_files[i])==0);
        free(original);
    }
    CHECK(ok(fs.vtable->write_text(fs.self,path,"{\"sessions\":[],\"messages\":[],\"tool_calls\":[],\"tool_results\":[]}")) &&
          ok(store.vtable->create_session(store.self,"s1",".")));
    cc_message_t *literal_message=NULL;
    CHECK(ok(cc_message_create_text("literal","s1",CC_ROLE_USER,"literal \\u0000",NULL,&literal_message)));
    cc_session_record_t literal_record={.type=CC_SESSION_RECORD_MESSAGE,.session_id="s1",.data.message=literal_message};
    CHECK(ok(store.vtable->append_records(store.self,&literal_record,1)));
    cc_message_destroy(literal_message);
    recent=NULL; recent_count=0;
    CHECK(ok(store.vtable->load_messages(store.self,"s1",-1,&recent,&recent_count)) && recent_count==1 &&
          strcmp(recent[0].content.items[0].text,"literal \\u0000")==0);
    cc_message_cleanup(&recent[0]); free(recent);
    const char *duplicate_sessions="{\"sessions\":[{\"id\":\"dup\"},{\"id\":\"dup\"}],\"messages\":[],\"tool_calls\":[],\"tool_results\":[]}";
    CHECK(ok(fs.vtable->write_text(fs.self,path,duplicate_sessions)));
    listed=NULL; listed_count=0;
    rc=store.vtable->list_sessions(store.self,&listed,&listed_count);
    CHECK(rc.code==CC_ERR_STORAGE && !listed && listed_count==0); cc_result_free(&rc);
    rc=store.vtable->create_session(store.self,"another",".");
    CHECK(rc.code==CC_ERR_STORAGE); cc_result_free(&rc);
    char *unchanged=NULL; CHECK(ok(fs.vtable->read_text(fs.self,path,&unchanged)) && strcmp(unchanged,duplicate_sessions)==0); free(unchanged);
    const char *malformed="{\"sessions\":[],\"messages\":[{\"id\":\"bad\",\"session_id\":\"s1\",\"role\":\"user\",\"content_parts\":\"not-json\"}],\"tool_calls\":[],\"tool_results\":[]}";
    CHECK(ok(fs.vtable->write_text(fs.self,path,malformed)));
    cc_message_t *broken=NULL; size_t broken_count=0;
    rc=store.vtable->load_messages(store.self,"s1",-1,&broken,&broken_count);
    CHECK(rc.code!=CC_OK && !broken && broken_count==0); cc_result_free(&rc);
    const char nul_history[]="{\"sessions\":[],\"messages\":[]}\0hidden";
    CHECK(ok(fs.vtable->write_bytes(fs.self,path,nul_history,sizeof(nul_history)-1U)));
    rc=store.vtable->load_messages(store.self,"s1",-1,&broken,&broken_count);
    CHECK(rc.code!=CC_OK && !broken && broken_count==0); cc_result_free(&rc);
    CHECK(ok(fs.vtable->write_text(fs.self,path,"[]")));
    rc=store.vtable->load_messages(store.self,"s1",-1,&broken,&broken_count);
    CHECK(rc.code!=CC_OK && !broken && broken_count==0); cc_result_free(&rc);
    rc=store.vtable->create_session(store.self,"s2",".");
    CHECK(rc.code!=CC_OK); cc_result_free(&rc);
    char *preserved=NULL; CHECK(ok(fs.vtable->read_text(fs.self,path,&preserved)) && strcmp(preserved,"[]")==0); free(preserved);
    store.vtable->destroy(store.self); CHECK(ok(fs.vtable->remove(fs.self,path)));
    snprintf(sidecar,sizeof(sidecar),"%s.bak",path);
    CHECK(ok(fs.vtable->write_text(fs.self,sidecar,"recoverable backup")));
    memset(&store,0,sizeof(store));
    rc=cc_json_file_store_create(path,&store);
    CHECK(rc.code==CC_ERR_STORAGE && !store.self && !store.vtable); cc_result_free(&rc);
    int primary_exists=0;
    CHECK(ok(fs.vtable->exists(fs.self,path,&primary_exists)) && !primary_exists);
    char *backup=NULL; CHECK(ok(fs.vtable->read_text(fs.self,sidecar,&backup)) && strcmp(backup,"recoverable backup")==0); free(backup);
    CHECK(ok(fs.vtable->remove(fs.self,sidecar)));
#if CC_STORAGE_SQLITE
    snprintf(path,sizeof(path),"%s/sessions.db",root);
    CHECK(ok(cc_sqlite_session_store_create(path,&store)) && populate(&store));
    timed=NULL;
    CHECK(ok(cc_message_create_text("sqlite-timed","s1",CC_ROLE_USER,"timestamp",NULL,&timed)));
    timed->created_at=malloc(strlen(timestamp)+1U); CHECK(timed->created_at); strcpy(timed->created_at,timestamp);
    timed_record.data.message=timed;
    CHECK(ok(store.vtable->append_records(store.self,&timed_record,1))); cc_message_destroy(timed);
    store.vtable->destroy(store.self); CHECK(ok(cc_sqlite_session_store_create(path,&store)) && check_messages(&store));
    recent=NULL; recent_count=0;
    CHECK(ok(store.vtable->load_messages(store.self,"s1",-1,&recent,&recent_count)));
    int sqlite_time_found=0;
    for (size_t i=0;i<recent_count;i++) {
        if (strcmp(recent[i].id,"sqlite-timed")==0)
            sqlite_time_found=recent[i].created_at && strcmp(recent[i].created_at,timestamp)==0;
        cc_message_cleanup(&recent[i]);
    }
    free(recent); CHECK(sqlite_time_found);
    sqlite3 *db=NULL; CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    CHECK(sqlite3_exec(db,"UPDATE messages SET content_parts=NULL WHERE id='executed'",NULL,NULL,NULL)==SQLITE_OK);
    CHECK(sqlite3_close(db)==SQLITE_OK);
    rc=store.vtable->load_messages(store.self,"s1",-1,&broken,&broken_count);
    CHECK(rc.code==CC_ERR_STORAGE && !broken && broken_count==0); cc_result_free(&rc);
    CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    CHECK(sqlite3_exec(db,"UPDATE messages SET content_parts='not-json' WHERE id='executed'",NULL,NULL,NULL)==SQLITE_OK);
    CHECK(sqlite3_close(db)==SQLITE_OK);
    rc=store.vtable->load_messages(store.self,"s1",-1,&broken,&broken_count);
    CHECK(rc.code!=CC_OK && !broken && broken_count==0); cc_result_free(&rc);
    CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    CHECK(sqlite3_exec(db,"UPDATE messages SET content_parts='[{\"type\":\"file\",\"artifact\":{\"kind\":\"file\",\"bytes\":1e99,\"data_base64\":\"Zg==\"}}]' WHERE id='executed'",NULL,NULL,NULL)==SQLITE_OK);
    CHECK(sqlite3_close(db)==SQLITE_OK);
    rc=store.vtable->load_messages(store.self,"s1",-1,&broken,&broken_count);
    CHECK(rc.code==CC_ERR_STORAGE && !broken && broken_count==0); cc_result_free(&rc);
    CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    CHECK(sqlite3_exec(db,"UPDATE messages SET content_parts='[null]' WHERE id='executed'",NULL,NULL,NULL)==SQLITE_OK);
    CHECK(sqlite3_close(db)==SQLITE_OK);
    rc=store.vtable->load_messages(store.self,"s1",-1,&broken,&broken_count);
    CHECK(rc.code!=CC_OK && !broken && broken_count==0); cc_result_free(&rc);
    CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    CHECK(sqlite3_exec(db,"UPDATE messages SET content_parts='[]',tool_calls='[{}]' WHERE id='executed'",NULL,NULL,NULL)==SQLITE_OK);
    CHECK(sqlite3_close(db)==SQLITE_OK);
    rc=store.vtable->load_messages(store.self,"s1",-1,&broken,&broken_count);
    CHECK(rc.code!=CC_OK && !broken && broken_count==0); cc_result_free(&rc);
    CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    CHECK(sqlite3_exec(db,"UPDATE messages SET tool_calls='[{\"id\":\"a\",\"function\":{\"name\":\"write\",\"arguments\":\"{}\"}},{\"id\":\"a\",\"function\":{\"name\":\"write\",\"arguments\":\"{}\"}}]' WHERE id='executed'",NULL,NULL,NULL)==SQLITE_OK);
    CHECK(sqlite3_close(db)==SQLITE_OK);
    rc=store.vtable->load_messages(store.self,"s1",-1,&broken,&broken_count);
    CHECK(rc.code!=CC_OK && !broken && broken_count==0); cc_result_free(&rc);
    CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    CHECK(sqlite3_exec(db,"UPDATE messages SET tool_calls='[]' WHERE id='executed'",NULL,NULL,NULL)==SQLITE_OK);
    CHECK(sqlite3_exec(db,"UPDATE messages SET content_parts=CAST(X'5B5D0068696464656E' AS TEXT) WHERE id='executed'",NULL,NULL,NULL)==SQLITE_OK);
    CHECK(sqlite3_close(db)==SQLITE_OK);
    rc=store.vtable->load_messages(store.self,"s1",-1,&broken,&broken_count);
    CHECK(rc.code!=CC_OK && !broken && broken_count==0); cc_result_free(&rc);
    store.vtable->destroy(store.self); CHECK(ok(fs.vtable->remove(fs.self,path)));
#endif
    CHECK(ok(fs.vtable->remove(fs.self,root))); fs.vtable->destroy(fs.self); free(root); return 0;
}
