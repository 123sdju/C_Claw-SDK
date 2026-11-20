#include "cc/adapters/cc_json_segmented_history.h"
#include "cc/core/cc_id.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/util/cc_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief 创建独占测试分段端口。
 * @param[in] root_path 根路径借用。
 * @param[in] segment_bytes 软分段字节阈值。
 * @param[in] media_dir 安全相对目录借用。
 * @param[in] max_base64_bytes 媒体回填限制。
 * @param[out] out_store 新端口槽，成功最终 destroy。
 * @return 创建结果，诊断由测试消费。
 */
extern cc_result_t cc_json_segmented_store_create(const char *root_path, size_t segment_bytes,
    const char *media_dir, size_t max_base64_bytes, cc_session_store_t *out_store);

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "check failed at line %d\n", __LINE__); return 1; } } while (0)
/** @brief 检查成功并消费独占结果诊断。
 * @param[in] rc 按值移入的拥有结果。
 * @return CC_OK 为一，否则报告并返回零，串行调用。
 */
static int ok(cc_result_t rc) { int good = rc.code == CC_OK; if (!good) fprintf(stderr, "%s\n", rc.message ? rc.message : "error"); cc_result_free(&rc); return good; }

/** @brief 构造独占消息，可附一个工具调用，再同步追加并销毁。
 * @param[in] store 存活测试端口借用。
 * @param[in] session 会话名借用。
 * @param[in] id 消息名借用。
 * @param[in] role 消息角色。
 * @param[in] text 正文借用。
 * @param[in] call 可为 NULL 的工具调用借用，消息深复制。
 * @param[in] tool_call_id 可为 NULL 的结果关联 ID 借用。
 * @return 构造及追加成功为一，否则零，临时消息和诊断内部消费，串行调用。
 */
static int append_message(cc_session_store_t *store, const char *session, const char *id,
                          cc_message_role_t role, const char *text, const cc_tool_call_t *call,
                          const char *tool_call_id)
{
    cc_message_t *message = NULL;
    cc_result_t rc = cc_message_create_text(id, session, role, text, tool_call_id, &message);
    if (rc.code == CC_OK && call) rc = cc_message_add_tool_call(message, call);
    if (rc.code == CC_OK) {
        cc_session_record_t record = {0};
        record.type = CC_SESSION_RECORD_MESSAGE;
        record.session_id = session;
        record.data.message = message;
        rc = store->vtable->append_records(store->self, &record, 1);
    }
    cc_message_destroy(message);
    return ok(rc);
}

/** @brief 构造指定工具状态的消息并同步追加。
 * @param[in] store 存活端口借用。
 * @param[in] session 会话名借用。
 * @param[in] id 消息名借用。
 * @param[in] role 消息角色。
 * @param[in] text 正文借用。
 * @param[in] call_id 关联调用 ID 借用。
 * @param[in] outcome 所需状态。
 * @return 成功为一，否则零，消息与诊断内部消费，串行调用。
 */
static int append_tagged(cc_session_store_t *store, const char *session, const char *id,
                         cc_message_role_t role, const char *text, const char *call_id,
                         cc_message_tool_outcome_t outcome)
{
    cc_message_t *message = NULL;
    cc_result_t rc = cc_message_create_text(id, session, role, text, call_id, &message);
    if (rc.code == CC_OK) {
        message->tool_outcome = outcome;
        cc_session_record_t record = {0};
        record.type = CC_SESSION_RECORD_MESSAGE; record.session_id = session; record.data.message = message;
        rc = store->vtable->append_records(store->self, &record, 1);
    }
    cc_message_destroy(message);
    return ok(rc);
}

typedef struct visit_state {
    cc_session_store_t *store;
    const char *write_session;
    size_t count;
    size_t repeated_ids;
    int wrote;
} visit_state_t;

/** @brief 在锁外 visitor 中计数，并可重入追加测试会话。
 * @param[in] message 只在本回调有效的消息借用。
 * @param[in,out] context 存活的独占 visit_state 借用，store 覆盖扫描寿命。
 * @return 成功或追加错误，结果归扫描器消费，同会话追加应使后续游标页失效。
 */
static cc_result_t count_and_optionally_write(const cc_message_t *message, void *context)
{
    visit_state_t *state = context;
    state->count++;
    if (message->id && strcmp(message->id, "repeated-page-id") == 0) state->repeated_ids++;
    if (state->write_session && !state->wrote) {
        state->wrote = 1;
        if (!append_message(state->store, state->write_session, "during-scan", CC_ROLE_USER,
                            "new text", NULL, NULL))
            return cc_result_error(CC_ERR_STORAGE, "Cannot append during scan test");
    }
    return cc_result_ok();
}

/** @brief 递归移除独占测试目录。
 * @param[in] fs 存活文件系统借用。
 * @param[in] path 必填测试路径借用，调用方限定范围。
 * @return 全部操作成功为一，否则零，列名/路径/诊断内部回收，串行调用。
 */
static int remove_tree(cc_filesystem_t *fs, const char *path)
{
    cc_filesystem_stat_t info = {.size = sizeof(info)};
    if (!ok(fs->vtable->stat(fs->self, path, &info))) return 0;
    if (info.type == CC_FS_ENTRY_DIRECTORY) {
        char **names = NULL;
        size_t count = 0;
        if (!ok(fs->vtable->list_dir(fs->self, path, &names, &count))) return 0;
        int success = 1;
        for (size_t i = 0; i < count; i++) {
            size_t length = strlen(path) + strlen(names[i]) + 2U;
            char *child = malloc(length);
            if (!child) success = 0;
            else {
                snprintf(child, length, "%s/%s", path, names[i]);
                if (!remove_tree(fs, child)) success = 0;
                free(child);
            }
            free(names[i]);
        }
        free(names);
        if (!success) return 0;
    }
    return ok(fs->vtable->remove(fs->self, path));
}

/** @brief 验证分页重复 ID、写入失效、历史导入、未知状态和核对冲突。
 * @return 全部通过零，否则一，成功清理所有端口/快照/路径，失败早退交进程回收。
 */
int main(void)
{
    cc_filesystem_t fs = {0};
    CHECK(ok(cc_filesystem_get_default(&fs)));
    char *root = NULL;
    CHECK(ok(cc_id_generate_uuid_v4("legacy-scan-", &root)));
    char session_dir[128], journal_dir[128];
    CHECK(snprintf(session_dir, sizeof(session_dir), "%s/sessions", root) > 0);
    CHECK(snprintf(journal_dir, sizeof(journal_dir), "%s/operations", root) > 0);
    CHECK(ok(fs.vtable->make_dir(fs.self, journal_dir)));
    cc_session_store_t store = {0};
    CHECK(ok(cc_json_segmented_store_create(session_dir, 2048, "media", 1024, &store)));
    CHECK(ok(store.vtable->create_session(store.self, "s1", ".")));
    CHECK(ok(store.vtable->create_session(store.self, "s2", ".")));
    CHECK(ok(store.vtable->create_session(store.self, "page-dupes", ".")));
    for (int i = 0; i < 70; i++) {
        char id[32];
        snprintf(id, sizeof(id), "page-%d", i);
        CHECK(append_message(&store, "page-dupes", (i == 38 || i == 68) ? "repeated-page-id" : id,
                             CC_ROLE_USER, "page text", NULL, NULL));
    }
    visit_state_t visit = {&store, NULL, 0, 0, 0};
    CHECK(ok(cc_json_segmented_store_visit_messages(&store, "page-dupes", count_and_optionally_write, &visit)));
    CHECK(visit.count == 70 && visit.repeated_ids == 2);
    cc_message_t *page = NULL; size_t page_count = 0; int page_more = 0;
    cc_result_t ambiguous = cc_json_segmented_store_load_page(&store, "page-dupes", "repeated-page-id",
                                                               8, &page, &page_count, &page_more);
    CHECK(ambiguous.code == CC_ERR_STORAGE && !page && page_count == 0); cc_result_free(&ambiguous);
    cc_message_t duplicated_message = {0}; int duplicated_found = 0;
    ambiguous = cc_json_segmented_store_find_message(&store, "page-dupes", CC_RUN_HISTORY_MESSAGE_ID,
                                                      "repeated-page-id", &duplicated_message, &duplicated_found);
    CHECK(ambiguous.code == CC_ERR_STORAGE && !duplicated_found); cc_result_free(&ambiguous);
    cc_message_cleanup(&duplicated_message);
    visit = (visit_state_t){&store, "s2", 0, 0, 0};
    CHECK(ok(cc_json_segmented_store_visit_messages(&store, "page-dupes", count_and_optionally_write, &visit)));
    CHECK(visit.wrote && visit.count == 70);
    visit = (visit_state_t){&store, "page-dupes", 0, 0, 0};
    cc_result_t mutation = cc_json_segmented_store_visit_messages(&store, "page-dupes",
                                                                  count_and_optionally_write, &visit);
    CHECK(mutation.code == CC_ERR_STORAGE && visit.wrote);
    cc_result_free(&mutation);
    CHECK(ok(store.vtable->clear_session(store.self, "page-dupes")));

    /* 单个 JSONL 批次包含多页消息时仍须完整遍历。 */
    CHECK(ok(store.vtable->create_session(store.self, "many-in-batch", ".")));
    cc_message_t *batch_messages[70] = {0};
    cc_session_record_t batch_records[70] = {0};
    for (int i = 0; i < 70; i++) {
        char id[32]; snprintf(id, sizeof(id), "batch-%d", i);
        CHECK(ok(cc_message_create_text(id, "many-in-batch", CC_ROLE_USER, "batch text", NULL, &batch_messages[i])));
        batch_records[i].type = CC_SESSION_RECORD_MESSAGE;
        batch_records[i].session_id = "many-in-batch";
        batch_records[i].data.message = batch_messages[i];
    }
    CHECK(ok(store.vtable->append_records(store.self, batch_records, 70)));
    for (size_t i = 0; i < 70; i++) cc_message_destroy(batch_messages[i]);
    visit = (visit_state_t){&store, NULL, 0, 0, 0};
    CHECK(ok(cc_json_segmented_store_visit_messages(&store, "many-in-batch", count_and_optionally_write, &visit)));
    CHECK(visit.count == 70);
    CHECK(ok(store.vtable->clear_session(store.self, "many-in-batch")));
    cc_tool_call_t old1 = {"old-call-1", "write", "{\"a\":1}"};
    cc_tool_call_t old2 = {"old-call-2", "write", "{\"a\":2}"};
    cc_tool_call_t completed = {"completed-call", "read", "{}"};
    CHECK(append_message(&store, "s1", "old-message", CC_ROLE_ASSISTANT, "", &old1, NULL));
    for (int i = 0; i < 80; i++) {
        char id[32];
        snprintf(id, sizeof(id), "user-%d", i);
        CHECK(append_message(&store, "s1", id, CC_ROLE_USER, "historical message", NULL, NULL));
    }
    CHECK(append_message(&store, "s1", "completed-assistant", CC_ROLE_ASSISTANT, "", &completed, NULL));
    CHECK(append_message(&store, "s1", "completed-result", CC_ROLE_TOOL, "done", NULL, completed.id));
    CHECK(append_message(&store, "s2", "old-message-2", CC_ROLE_ASSISTANT, "", &old2, NULL));
    cc_message_t found_message = {0};
    int found = 0;
    CHECK(ok(cc_json_segmented_store_find_message(&store, "s1", CC_RUN_HISTORY_MESSAGE_ID,
                                                   "old-message", &found_message, &found)));
    CHECK(found && strcmp(found_message.id, "old-message") == 0);
    cc_message_cleanup(&found_message);
    CHECK(ok(cc_json_segmented_store_find_message(&store, "s1", CC_RUN_HISTORY_TOOL_INTENT,
                                                   old1.id, &found_message, &found)));
    CHECK(found && found_message.tool_calls.count == 1);
    cc_message_cleanup(&found_message);
    CHECK(ok(cc_json_segmented_store_find_message(&store, "s1", CC_RUN_HISTORY_TOOL_RESULT,
                                                   completed.id, &found_message, &found)));
    CHECK(found && strcmp(found_message.id, "completed-result") == 0);
    cc_message_cleanup(&found_message);
    CHECK(ok(cc_json_segmented_store_find_message(&store, "s1", CC_RUN_HISTORY_TOOL_RESULT,
                                                   old1.id, &found_message, &found)) && !found);
    cc_message_cleanup(&found_message);
    cc_operation_journal_t *journal = NULL;
    CHECK(ok(cc_operation_journal_open(&fs, journal_dir, 1, &journal)));
    size_t unresolved = 0;
    CHECK(ok(cc_json_segmented_store_import_interrupted_calls(&store, journal, &unresolved)));
    CHECK(unresolved == 2);
    cc_pending_operation_t *items = NULL;
    size_t count = 0;
    CHECK(ok(cc_operation_journal_list(journal, &items, &count)) && count == 2);
    for (size_t i = 0; i < count; i++) CHECK(strcmp(items[i].call.name, "write") == 0 && !items[i].in_progress);
    int blocked = 0;
    CHECK(ok(cc_operation_journal_is_blocked(journal, "write", &blocked)) && blocked);
    char *denied = NULL;
    cc_tool_call_t another = {"new-call", "other", "{}"};
    cc_result_t rc = cc_operation_journal_begin(journal, "s2", &another, &denied);
    CHECK(rc.code == CC_ERR_LIMIT_EXCEEDED && denied == NULL);
    cc_result_free(&rc);
    CHECK(ok(cc_json_segmented_store_import_interrupted_calls(&store, journal, &unresolved)) && unresolved == 2);
    cc_pending_operation_t *again = NULL;
    size_t again_count = 0;
    CHECK(ok(cc_operation_journal_list(journal, &again, &again_count)) && again_count == 2);
    cc_pending_operation_array_free(again, again_count);

    /* 两个历史调用重用 ID 时拒绝把结果任意配对，保留已有账本禁用状态。 */
    CHECK(ok(store.vtable->create_session(store.self, "ambiguous-call", ".")));
    cc_tool_call_t ambiguous_a = {"reused-call", "first-tool", "{}"};
    cc_tool_call_t ambiguous_b = {"reused-call", "second-tool", "{}"};
    CHECK(append_message(&store, "ambiguous-call", "a", CC_ROLE_ASSISTANT, "", &ambiguous_a, NULL));
    for (int i = 0; i < 40; i++) {
        char id[32]; snprintf(id, sizeof(id), "spacer-%d", i);
        CHECK(append_message(&store, "ambiguous-call", id, CC_ROLE_USER, "spacer", NULL, NULL));
    }
    CHECK(append_message(&store, "ambiguous-call", "b", CC_ROLE_ASSISTANT, "", &ambiguous_b, NULL));
    CHECK(append_message(&store, "ambiguous-call", "result", CC_ROLE_TOOL, "done", NULL, "reused-call"));
    ambiguous = cc_json_segmented_store_find_message(&store, "ambiguous-call", CC_RUN_HISTORY_TOOL_INTENT,
                                                      "reused-call", &duplicated_message, &duplicated_found);
    CHECK(ambiguous.code == CC_ERR_STORAGE && !duplicated_found); cc_result_free(&ambiguous);
    cc_message_cleanup(&duplicated_message);
    rc = cc_json_segmented_store_import_interrupted_calls(&store, journal, &unresolved);
    CHECK(rc.code == CC_ERR_STORAGE); cc_result_free(&rc);
    CHECK(ok(cc_operation_journal_is_blocked(journal, "write", &blocked)) && blocked);
    CHECK(ok(store.vtable->clear_session(store.self, "ambiguous-call")));

    cc_message_t *multi = NULL;
    CHECK(ok(cc_message_create_text("multi", "ambiguous-call", CC_ROLE_ASSISTANT, "", NULL, &multi)));
    CHECK(ok(cc_message_add_tool_call(multi, &ambiguous_a)));
    CHECK(ok(cc_message_add_tool_call(multi, &ambiguous_b)));
    cc_session_record_t multi_record = {
        .type = CC_SESSION_RECORD_MESSAGE, .session_id = "ambiguous-call", .data.message = multi
    };
    rc = store.vtable->append_records(store.self, &multi_record, 1);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT); cc_result_free(&rc);
    cc_message_destroy(multi);
    CHECK(ok(cc_json_segmented_store_import_interrupted_calls(&store, journal, &unresolved)) && unresolved == 2);

    /* 同调用的未知占位及第二个结果形成歧义，恢复必须拒绝。 */
    CHECK(append_message(&store, "ambiguous-call", "one-intent", CC_ROLE_ASSISTANT, "", &ambiguous_a, NULL));
    CHECK(append_tagged(&store, "ambiguous-call", "unknown-result", CC_ROLE_TOOL, "unknown",
                        ambiguous_a.id, CC_MESSAGE_TOOL_OUTCOME_UNKNOWN));
    CHECK(append_message(&store, "ambiguous-call", "second-result", CC_ROLE_TOOL, "done", NULL, ambiguous_a.id));
    rc = cc_json_segmented_store_find_message(&store, "ambiguous-call", CC_RUN_HISTORY_TOOL_RESULT,
                                               ambiguous_a.id, &duplicated_message, &duplicated_found);
    CHECK(rc.code == CC_ERR_STORAGE && !duplicated_found); cc_result_free(&rc);
    cc_message_cleanup(&duplicated_message);
    rc = cc_json_segmented_store_import_interrupted_calls(&store, journal, &unresolved);
    CHECK(rc.code == CC_ERR_STORAGE); cc_result_free(&rc);
    CHECK(ok(cc_operation_journal_is_blocked(journal, "write", &blocked)) && blocked);
    CHECK(ok(store.vtable->clear_session(store.self, "ambiguous-call")));

    /* 这些不同 ID 设置目标的四个过滤位，精确重扫应排除假阳性并保留合法历史。 */
    CHECK(ok(store.vtable->create_session(store.self, "filter-collision", ".")));
    const char *filter_ids[] = {"filter-target", "filter-primer-14428", "filter-primer-14001",
                                "filter-primer-18922", "filter-primer-3283"};
    for (size_t i = 0; i < sizeof(filter_ids) / sizeof(filter_ids[0]); i++) {
        char intent_id[32], result_id[32];
        snprintf(intent_id, sizeof(intent_id), "filter-intent-%zu", i);
        snprintf(result_id, sizeof(result_id), "filter-result-%zu", i);
        cc_tool_call_t call = {(char *)filter_ids[i], "filter-tool", "{}"};
        CHECK(append_message(&store, "filter-collision", intent_id, CC_ROLE_ASSISTANT, "", &call, NULL));
        CHECK(append_message(&store, "filter-collision", result_id, CC_ROLE_TOOL, "done", NULL, call.id));
    }
    CHECK(ok(cc_json_segmented_store_import_interrupted_calls(&store, journal, &unresolved)) && unresolved == 2);
    CHECK(ok(store.vtable->clear_session(store.self, "filter-collision")));

    /* 账本文件丢失后未知占位仍待核对，同时覆盖当前元数据和既有正文前缀识别。 */
    CHECK(ok(store.vtable->create_session(store.self, "s3", ".")));
    cc_tool_call_t uncertain = {"unknown-call", "act", "{}"};
    cc_tool_call_t legacy_unknown = {"legacy-unknown", "old-act", "{}"};
    cc_tool_call_t done = {"done-with-note", "done-tool", "{}"};
    CHECK(append_message(&store, "s3", "u-intent", CC_ROLE_ASSISTANT, "", &uncertain, NULL));
    CHECK(append_tagged(&store, "s3", "u-result", CC_ROLE_TOOL, "uncertain", uncertain.id,
                        CC_MESSAGE_TOOL_OUTCOME_UNKNOWN));
    CHECK(append_message(&store, "s3", "l-intent", CC_ROLE_ASSISTANT, "", &legacy_unknown, NULL));
    CHECK(append_message(&store, "s3", "l-result", CC_ROLE_TOOL,
        "The tool outcome is unknown and its name is disabled until a user verifies it. failure", NULL, legacy_unknown.id));
    CHECK(append_message(&store, "s3", "d-intent", CC_ROLE_ASSISTANT, "", &done, NULL));
    CHECK(append_message(&store, "s3", "d-result", CC_ROLE_TOOL, "done", NULL, done.id));
    CHECK(append_tagged(&store, "s3", "d-note", CC_ROLE_USER, "verified", done.id,
                        CC_MESSAGE_TOOL_OUTCOME_RECONCILED_EXECUTED));
    CHECK(ok(cc_json_segmented_store_find_message(&store, "s3", CC_RUN_HISTORY_TOOL_RESULT,
                                                   uncertain.id, &found_message, &found)) && !found);
    cc_message_cleanup(&found_message);
    CHECK(ok(cc_json_segmented_store_import_interrupted_calls(&store, journal, &unresolved)) && unresolved == 4);
    again=NULL; again_count=0;
    CHECK(ok(cc_operation_journal_list(journal, &again, &again_count)) && again_count==4);
    for (size_t i=0;i<again_count;i++) if (strcmp(again[i].call.id, uncertain.id)==0)
        CHECK(ok(cc_operation_journal_finish(journal, again[i].operation_id)));
    cc_pending_operation_array_free(again, again_count);
    CHECK(ok(cc_json_segmented_store_import_interrupted_calls(&store, journal, &unresolved)) && unresolved==4);
    CHECK(ok(cc_operation_journal_is_blocked(journal, "act", &blocked)) && blocked);
    CHECK(append_tagged(&store, "s3", "u-note", CC_ROLE_USER, "operator verified", uncertain.id,
                        CC_MESSAGE_TOOL_OUTCOME_RECONCILED_EXECUTED));
    CHECK(ok(cc_json_segmented_store_find_message(&store, "s3", CC_RUN_HISTORY_TOOL_RESULT,
                                                   uncertain.id, &found_message, &found)) && found &&
          found_message.role==CC_ROLE_USER && found_message.tool_outcome==CC_MESSAGE_TOOL_OUTCOME_RECONCILED_EXECUTED);
    char *provider_json=NULL; CHECK(ok(cc_message_to_json(&found_message,0,&provider_json)) &&
          !strstr(provider_json,"tool_outcome") && !strstr(provider_json,"tool_call_id"));
    free(provider_json); cc_message_cleanup(&found_message);
    again=NULL; again_count=0;
    CHECK(ok(cc_operation_journal_list(journal,&again,&again_count)) && again_count==4);
    for (size_t i=0;i<again_count;i++) if (strcmp(again[i].call.id, uncertain.id)==0)
        CHECK(ok(cc_operation_journal_finish(journal, again[i].operation_id)));
    cc_pending_operation_array_free(again, again_count);
    CHECK(ok(cc_json_segmented_store_import_interrupted_calls(&store,journal,&unresolved)) && unresolved==3);
    CHECK(ok(cc_operation_journal_is_blocked(journal,"act",&blocked)) && !blocked);
    CHECK(ok(cc_operation_journal_is_blocked(journal,"old-act",&blocked)) && blocked);

    /* 重新加载时拒绝相互矛盾的人工结论。 */
    CHECK(append_tagged(&store,"s3","u-conflict",CC_ROLE_USER,"different verification",uncertain.id,
                        CC_MESSAGE_TOOL_OUTCOME_RECONCILED_NOT_EXECUTED));
    rc=cc_json_segmented_store_find_message(&store,"s3",CC_RUN_HISTORY_TOOL_RESULT,uncertain.id,
                                            &duplicated_message,&duplicated_found);
    CHECK(rc.code==CC_ERR_STORAGE && !duplicated_found); cc_result_free(&rc);
    cc_message_cleanup(&duplicated_message);
    rc=cc_json_segmented_store_import_interrupted_calls(&store,journal,&unresolved);
    CHECK(rc.code==CC_ERR_STORAGE); cc_result_free(&rc);
    CHECK(append_message(&store, "s2", "bad-result", CC_ROLE_TOOL, "bad", NULL, NULL));
    rc = cc_json_segmented_store_import_interrupted_calls(&store, journal, &unresolved);
    CHECK(rc.code == CC_ERR_STORAGE);
    cc_result_free(&rc);
    cc_pending_operation_array_free(items, count);
    again=NULL; again_count=0; CHECK(ok(cc_operation_journal_list(journal,&again,&again_count)));
    for (size_t i=0;i<again_count;i++) CHECK(ok(cc_operation_journal_finish(journal,again[i].operation_id)));
    cc_pending_operation_array_free(again,again_count);
    cc_operation_journal_destroy(journal);
    /* 清单被改为其他会话身份时，枚举必须拒绝，避免恢复扫描遗漏会话。 */
    char **directories=NULL; size_t directory_count=0; int changed=0;
    CHECK(ok(fs.vtable->list_dir(fs.self,session_dir,&directories,&directory_count)));
    for (size_t i=0;i<directory_count;i++) {
        char path[512]; snprintf(path,sizeof(path),"%s/%s/manifest.json",session_dir,directories[i]);
        char *json=NULL; cc_result_t read=fs.vtable->read_text(fs.self,path,&json);
        if (read.code==CC_OK && json) {
            cc_json_value_t *root_json=NULL;
            if (ok(cc_json_parse_strict(json,&root_json))) {
                const char *id=cc_json_string_value(cc_json_object_get(root_json,"id"));
                if (id && strcmp(id,"s3")==0) {
                    cc_json_object_set(root_json,"id",cc_json_create_string("s1"));
                    char *modified=cc_json_stringify_unformatted(root_json);
                    CHECK(modified && ok(fs.vtable->write_text(fs.self,path,modified))); free(modified); changed=1;
                }
                cc_json_destroy(root_json);
            }
        }
        cc_result_free(&read); free(json); free(directories[i]);
    }
    free(directories); CHECK(changed);
    cc_session_t *listed=NULL; size_t listed_count=0;
    rc=store.vtable->list_sessions(store.self,&listed,&listed_count);
    CHECK(rc.code==CC_ERR_STORAGE && !listed && listed_count==0); cc_result_free(&rc);
    store.vtable->destroy(store.self);
    CHECK(remove_tree(&fs, root));
    fs.vtable->destroy(fs.self);
    free(root);
    return 0;
}
