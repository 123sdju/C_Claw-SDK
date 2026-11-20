



#include "cc/app/cc_session_manager.h"
#include "cc/core/cc_id.h"
#include "cc/internal/cc_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * @brief 使用 CSPRNG 生成带 ses_ 前缀的 UUID 消息 ID。
 * @param[out] out_id 必填拥有字符串输出，由 UUID 接口初始化，调用者 free。
 * @return 原样返回 UUID 生成结果，随机源失败继续向上传递。
 */
static cc_result_t generate_id(char **out_id)
{
    return cc_id_generate_uuid_v4("ses_", out_id);
}


/**
 * @brief 使用线程安全的本地时间转换生成当前时间副本。
 * @return 拥有的 YYYY-MM-DDTHH:MM:SS 字符串，由调用者 free，时间转换、格式化或复制失败返回 NULL。
 * 时间文本使用本地时区并省略时区后缀。
 */
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


/**
 * @brief 分配管理器容器并按值保存借用的存储句柄。
 * @param[in] store 后端及虚表借用，空虚表也可创建，由各业务入口核对可选能力。
 * @param[out] out_manager 必填输出，入口置 NULL，成功取得拥有容器。
 * @return 空输出返回 CC_ERR_INVALID_ARGUMENT，分配失败返回 CC_ERR_OUT_OF_MEMORY，成功返回 CC_OK。
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


/**
 * @brief 释放管理器自身容器，后端保留给原 owner。
 * @pre 外层已排空 manager 调用，store 寿命由外层协调。
 * @param[in,out] manager 待销毁对象，可为 NULL。
 * @post manager 已释放，借用后端继续由其 owner 管理。
 */
void cc_session_manager_destroy(cc_session_manager_t *manager)
{
    free(manager);
}


/**
 * @brief 后端具备创建能力时转交会话 ID 与工作区。
 * @param[in] manager 非空借用管理器。
 * @param[in] session_id 非空指针的会话 ID，内容由后端核对，调用期间借用。
 * @param[in] workspace_dir 可选借用工作区路径，原样转交。
 * @return 空管理器或 ID 返回 CC_ERR_INVALID_ARGUMENT，缺少 create_session 返回 CC_OK，其他返回后端结果。
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


/**
 * @brief 构造带消息 ID 和本地时间的用户消息，同步追加后释放临时消息。
 *
 * now 的所有权交给 message，生成 ID 在消息深拷贝后释放，record 仅在本次栈中借用这些字段。
 * @param[in] manager 非空借用管理器，后端须提供 append_records 并在调用内复制或序列化。
 * @param[in] session_id 非空指针的借用会话 ID。
 * @param[in] content 非空指针的借用正文，空字符串也会传给消息构造器。
 * @return 缺少参数返回 CC_ERR_INVALID_ARGUMENT，缺少追加入口返回 CC_ERR_STORAGE，
 * ID 和消息构造错误向上传递，时间创建失败统一返回 CC_ERR_OUT_OF_MEMORY，其余返回后端结果。
 * 失败也可能已有持久化写入，所有本次临时拥有资源在返回前释放。
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


/**
 * @brief 验证列表输出后转交后端，缺少管理器或列表能力时返回空结果。
 * @param[in] manager 借用对象，可为 NULL。
 * @param[out] out_sessions 必填输出，空能力分支置 NULL，后端交出的拥有数组由调用者 cc_session_free_array。
 * @param[out] out_count 必填输出，空能力分支置零，其余由后端填写。
 * @return 输出缺失返回 CC_ERR_INVALID_ARGUMENT 并保留其他输出原值，空能力返回 CC_OK，其他原样返回后端结果。
 */
cc_result_t cc_session_manager_list_sessions(
    cc_session_manager_t *manager,
    cc_session_t **out_sessions,
    size_t *out_count
)
{
    if (!out_sessions || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null session list output");
    if (!manager || !manager->store.vtable || !manager->store.vtable->list_sessions) {
        *out_sessions = NULL;
        *out_count = 0;
        return cc_result_ok();
    }
    return manager->store.vtable->list_sessions(manager->store.self, out_sessions, out_count);
}
