



#include "cc/core/cc_session.h"
#include "cc/internal/cc_alloc.h"
#include <stdlib.h>
#include <string.h>

/**
 * @brief 深拷贝三个可选字段，创建初始状态为 ACTIVE 的会话元数据。
 * @param[in] id 调用期间借用的可选 NUL 结尾标识，NULL 字段保留为空。
 * @param[in] name 调用期间借用的可选会话名称。
 * @param[in] workspace_dir 调用期间借用的可选工作目录。
 * @param[out] out_session 必填空资源槽，成功接管堆对象，调用者 destroy，失败为 NULL。
 * @return 成功为 CC_OK，空输出或分配失败为相应拥有结果，调用者释放诊断。
 * @note 每个字符串独立复制，源字段在调用期间保持稳定，失败清理全部部分对象。
 */
cc_result_t cc_session_create(
    const char *id,
    const char *name,
    const char *workspace_dir,
    cc_session_t **out_session
)
{
    if (!out_session) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null session output");
    }
    *out_session = NULL;
    cc_session_t *session = calloc(1, sizeof(cc_session_t));
    if (!session) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate session");

    session->id = id ? cc_copy_string(id) : NULL;
    session->name = name ? cc_copy_string(name) : NULL;
    session->workspace_dir = workspace_dir ? cc_copy_string(workspace_dir) : NULL;
    session->status = CC_SESSION_ACTIVE;
    if ((id && !session->id) ||
        (name && !session->name) ||
        (workspace_dir && !session->workspace_dir)) {
        cc_session_destroy(session);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy session fields");
    }

    *out_session = session;
    return cc_result_ok();
}

/**
 * @brief 释放会话元数据拥有的全部字符串并清零结构。
 * @param[in,out] session 独占、已初始化的会话对象，可为 NULL。
 * @post 字段资源释放且结构清零，容器内存仍归调用者，空对象允许重复清理。
 * @note 调用前排空该元数据的全部借用，消息和记忆集合由对应存储拥有者管理。
 */
void cc_session_cleanup(cc_session_t *session)
{
    if (!session) return;
    free(session->id);
    free(session->name);
    free(session->workspace_dir);
    free(session->model);
    free(session->created_at);
    free(session->updated_at);
    memset(session, 0, sizeof(*session));
}

/**
 * @brief 消费堆上会话数组，逐项清理字段后释放数组。
 * @param[in,out] sessions 独占且兼容 free 的连续数组，NULL 时跳过。
 * @param[in] count 已初始化元素数量，调用者保证实际容量覆盖。
 * @post 全部字段和容器均释放，原数组及字段借用失效。
 * @note 调用者先排空数组及元素的使用者，元素中的各字符串分别拥有。
 */
void cc_session_free_array(cc_session_t *sessions, size_t count)
{
    if (!sessions) return;
    for (size_t i = 0; i < count; i++) cc_session_cleanup(&sessions[i]);
    free(sessions);
}

/**
 * @brief 消费单个堆会话，清理字段后释放结构。
 * @param[in,out] session 兼容 free 的独占会话对象，可为 NULL。
 * @post 对象和字段均释放，旧句柄失效。
 * @note 调用者先结束全部借用，栈对象或数组元素使用 cleanup 清理。
 */
void cc_session_destroy(cc_session_t *session)
{
    if (!session) return;
    cc_session_cleanup(session);
    free(session);
}
