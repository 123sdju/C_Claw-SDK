



#ifndef CC_SESSION_MANAGER_H
#define CC_SESSION_MANAGER_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_session_store.h"

/*
 * session manager。
 *
 * manager 持有 session_store 句柄，向 runtime 提供更高层的会话创建、用户消息追加和
 * session 列举操作。store self 的最终销毁由 manager destroy 触发。
 */
typedef struct cc_session_manager {
    cc_session_store_t store;
} cc_session_manager_t;

/* 创建 session manager；store 按值保存到 manager 内部。 */
cc_result_t cc_session_manager_create(
    cc_session_store_t store,
    cc_session_manager_t **out_manager
);

/* 销毁 manager，并通过 store vtable 销毁底层 session store。 */
void cc_session_manager_destroy(cc_session_manager_t *manager);

/* 确保 session 存在；不存在时由 store 创建，workspace_dir 作为安全工作目录记录。 */
cc_result_t cc_session_manager_ensure_session(
    cc_session_manager_t *manager,
    const char *session_id,
    const char *workspace_dir
);

/* 追加用户文本消息；manager 负责构造 cc_message_t 并交给 store 持久化。 */
cc_result_t cc_session_manager_append_user_message(
    cc_session_manager_t *manager,
    const char *session_id,
    const char *content
);

/* 列出 session；out_sessions 数组成功后由调用方逐项 destroy/cleanup 并 free。 */
cc_result_t cc_session_manager_list_sessions(
    cc_session_manager_t *manager,
    cc_session_t **out_sessions,
    size_t *out_count
);

#endif
