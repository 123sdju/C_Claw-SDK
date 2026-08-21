



#ifndef CC_SESSION_H
#define CC_SESSION_H

#include "cc_result.h"
#include "cc_message.h"


/*
 * session 的高层状态。
 *
 * runtime 和 session store 用它表达一段会话是否仍可继续、是否已完成或进入错误态。
 * 这不是线程同步状态机；并发串行化由 session manager/run queue 负责。
 */
typedef enum cc_session_status {
    CC_SESSION_ACTIVE,
    CC_SESSION_COMPLETED,
    CC_SESSION_ERROR
} cc_session_status_t;

/*
 * Agent 会话元数据。
 *
 * 字符串字段由 session 拥有，cc_session_destroy() 负责释放。workspace_dir 是安全
 * 策略和文件工具的根目录，不应由工具层自行拼接绕过；model 允许上层记录当前会话
 * 使用的 provider/model 名称。
 */
typedef struct cc_session {
    char *id;
    char *name;
    char *workspace_dir;

    char *model;
    cc_session_status_t status;
    char *created_at;
    char *updated_at;
} cc_session_t;

/*
 * 创建堆上 session。
 *
 * id/name/workspace_dir 会被深拷贝，初始状态为 ACTIVE。成功后 *out_session 的所有权
 * 交给调用方，必须用 cc_session_destroy() 释放；失败时 *out_session 为 NULL。
 */
cc_result_t cc_session_create(
    const char *id,
    const char *name,
    const char *workspace_dir,
    cc_session_t **out_session
);

/* 销毁 session 及其所有字符串字段；允许传入 NULL。 */
void cc_session_destroy(cc_session_t *session);

#endif
