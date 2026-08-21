



#include "cc/core/cc_session.h"
#include "cc/internal/cc_alloc.h"
#include <stdlib.h>
#include <string.h>

/*
 * 创建堆上 session 元数据对象。
 *
 * session 是 runtime、session store 和工具安全策略之间共享的身份边界。函数会深拷贝
 * 外部字符串，并在任一字段复制失败时销毁半成品，保证调用方只处理“成功完整对象”
 * 或“失败无对象”两种状态。
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

/*
 * 销毁 session。
 *
 * 这里释放的是 session 元数据本身，不负责释放该 session 下的 message 或 memory。
 * 那些集合由 session store / memory store 各自管理，避免一个 destroy 函数跨越太多
 * 存储边界。
 */
void cc_session_destroy(cc_session_t *session)
{
    if (!session) return;
    free(session->id);
    free(session->name);
    free(session->workspace_dir);
    free(session->model);
    free(session->created_at);
    free(session->updated_at);
    free(session);
}
