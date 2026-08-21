



#ifndef CC_SESSION_STORE_H
#define CC_SESSION_STORE_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_message.h"
#include "cc/core/cc_tool_call.h"
#include "cc/core/cc_session.h"


/* session store vtable 前置声明。 */
typedef struct cc_session_store_vtable cc_session_store_vtable_t;

/* session store port 前置声明。 */
typedef struct cc_session_store cc_session_store_t;

typedef enum cc_session_record_type {
    CC_SESSION_RECORD_MESSAGE = 0,
    CC_SESSION_RECORD_TOOL_CALL = 1,
    CC_SESSION_RECORD_TOOL_RESULT = 2,
} cc_session_record_type_t;

/* append_records 的借用记录；store 必须把整组记录作为一个逻辑批次提交。 */
typedef struct cc_session_record {
    cc_session_record_type_t type;
    const char *session_id;
    union {
        const cc_message_t *message;
        const cc_tool_call_t *tool_call;
        struct {
            const char *tool_call_id;
            const cc_tool_result_t *result;
        } tool_result;
    } data;
} cc_session_record_t;

/*
 * session store 接口对象。
 *
 * self 指向 JSON 文件、内存、SQLite 等具体实现，vtable 提供会话和消息持久化操作。
 * 核心 runtime 不直接依赖文件/数据库，便于 MCU profile 换成轻量存储。
 */
struct cc_session_store {
    void *self;
    const cc_session_store_vtable_t *vtable;
};


/*
 * session store vtable。
 *
 * 所有输入对象都是借用指针；返回的 session/message 数组由调用方负责逐项 cleanup 后
 * free。append_* 应深拷贝需要持久化的数据，不能保存调用方临时指针。
 */
struct cc_session_store_vtable {


    /* 创建或打开一个 session 记录。 */
    cc_result_t (*create_session)(
        void *self,
        const char *session_id,
        const char *workspace_dir
    );



    /* 原子追加一组 message/tool call/tool result；失败时不得发布部分批次。 */
    cc_result_t (*append_records)(
        void *self,
        const cc_session_record_t *records,
        size_t count
    );



    /* 加载最近 limit 条消息；out_messages 数组由调用方释放。 */
    cc_result_t (*load_messages)(
        void *self,
        const char *session_id,
        int limit,
        cc_message_t **out_messages,
        size_t *out_count
    );



    /* 列出已有 session；out_sessions 数组由调用方销毁。 */
    cc_result_t (*list_sessions)(
        void *self,
        cc_session_t **out_sessions,
        size_t *out_count
    );



    /* 清空某个 session 的持久化数据。 */
    cc_result_t (*clear_session)(
        void *self,
        const char *session_id
    );



    /* 销毁 store self。 */
    void (*destroy)(void *self);
};

#endif
