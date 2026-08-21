



#include "cc/app/cc_session_manager.h"
#include "cc/core/cc_id.h"
#include "cc/internal/cc_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* CSPRNG UUID 失败必须上抛，禁止回退到时间或计数器。 */
static cc_result_t generate_id(char **out_id)
{
    return cc_id_generate_uuid_v4("ses_", out_id);
}


/* 生成 ISO-like 当前时间字符串；返回值由调用方 free()。 */
static char *now_string(void)
{
    time_t t = time(NULL);
    char buf[64];
    struct tm tm_info;
#if defined(_WIN32)
    if (localtime_s(&tm_info, &t) != 0) return NULL;
#else
    if (!localtime_r(&t, &tm_info)) return NULL;
#endif
    if (strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_info) == 0) return NULL;
    return cc_copy_string(buf);
}


/*
 * 创建 session manager。
 *
 * store 按值保存，manager 后续通过其 vtable 访问持久化层。调用方仍需要保证 store self
 * 在 manager 生命周期内有效。
 */
cc_result_t cc_session_manager_create(
    cc_session_store_t store,
    cc_session_manager_t **out_manager
)
{
    if (!out_manager) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null session manager output");
    *out_manager = NULL;
    cc_session_manager_t *manager = calloc(1, sizeof(cc_session_manager_t));
    if (!manager) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create session manager");

    manager->store = store;
    *out_manager = manager;
    return cc_result_ok();
}


/* 销毁 manager 容器；当前不销毁 store self，避免与 runtime builder 所有权冲突。 */
void cc_session_manager_destroy(cc_session_manager_t *manager)
{
    free(manager);
}


/*
 * 确保 session 存在。
 *
 * 如果底层 store 没有 create_session 能力，函数按 no-op 成功处理，让轻量 store 可以只实现
 * append/load。
 */
cc_result_t cc_session_manager_ensure_session(
    cc_session_manager_t *manager,
    const char *session_id,
    const char *workspace_dir
)
{
    if (!manager || !session_id) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null argument");

    if (manager->store.vtable && manager->store.vtable->create_session) {
        return manager->store.vtable->create_session(
            manager->store.self, session_id, workspace_dir);
    }
    return cc_result_ok();
}


/*
 * 追加用户消息。
 *
 * manager 负责生成消息 id/created_at 并构造 cc_message_t；append 后立即销毁临时 message，
 * store 必须在 append_records 内部完成深拷贝或序列化。
 */
cc_result_t cc_session_manager_append_user_message(
    cc_session_manager_t *manager,
    const char *session_id,
    const char *content
)
{
    if (!manager || !session_id || !content)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null argument");


    if (!manager->store.vtable || !manager->store.vtable->append_records)
        return cc_result_error(CC_ERR_STORAGE, "Session store cannot append records");

    cc_message_t *msg = NULL;
    char *id = NULL;
    cc_result_t rc = generate_id(&id);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    char *now = now_string();
    if (!now) {
        free(id);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create message timestamp");
    }

    rc = cc_message_create_text(id, session_id, CC_ROLE_USER, content, NULL, &msg);
    if (rc.code != CC_OK) {
        free(id);
        free(now);
        return rc;
    }


    msg->created_at = now;
    free(id);


    cc_session_record_t record;
    memset(&record, 0, sizeof(record));
    record.type = CC_SESSION_RECORD_MESSAGE;
    record.session_id = session_id;
    record.data.message = msg;
    rc = manager->store.vtable->append_records(manager->store.self, &record, 1);
    cc_message_destroy(msg);
    return rc;
}


/*
 * 列出 session。
 *
 * 缺少 list_sessions 能力时返回空列表而不是错误，便于最小 store 实现通过核心路径。
 */
cc_result_t cc_session_manager_list_sessions(
    cc_session_manager_t *manager,
    cc_session_t **out_sessions,
    size_t *out_count
)
{
    if (!manager || !manager->store.vtable || !manager->store.vtable->list_sessions) {
        *out_sessions = NULL;
        *out_count = 0;
        return cc_result_ok();
    }
    return manager->store.vtable->list_sessions(manager->store.self, out_sessions, out_count);
}
