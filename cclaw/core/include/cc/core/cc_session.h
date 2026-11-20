



#ifndef CC_SESSION_H
#define CC_SESSION_H

#include "cc_result.h"
#include "cc_message.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * session 的高层状态。
 *
 * runtime 和 session store 用它表达一段会话是否仍可继续、是否已完成或进入错误态。
 * 并发准入和串行化由 session manager、run queue 及对应存储拥有者维护。
 */
typedef enum cc_session_status {
    CC_SESSION_ACTIVE,
    CC_SESSION_COMPLETED,
    CC_SESSION_ERROR
} cc_session_status_t;

/*
 * Agent 会话元数据。
 *
 * 字符串字段由 session 独立拥有，cleanup 释放字段，destroy 额外释放堆容器。
 * workspace_dir 记录工作目录，路径解释和访问由策略及文件端口负责。model 用于
 * 记录 provider/model 名称，状态与时间字段由使用方更新并同步共享访问。
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

/**
 * @brief 深拷贝可选标识、名称及工作目录，创建 ACTIVE 会话元数据。
 * @param[in] id 调用期间借用的可选 NUL 结尾标识。
 * @param[in] name 调用期间借用的可选会话名称。
 * @param[in] workspace_dir 调用期间借用的可选工作目录。
 * @param[out] out_session 必填空资源槽，成功取得独占堆对象并最终 destroy，失败为 NULL。
 * @return 成功为 CC_OK，参数或分配错误的拥有诊断由调用者释放。
 * @note 源字符串保持至返回，各字段独立复制，其他字段初始化为空。
 */
cc_result_t cc_session_create(
    const char *id,
    const char *name,
    const char *workspace_dir,
    cc_session_t **out_session
);

/**
 * @brief 释放会话拥有的全部字段并清零结构。
 * @param[in,out] session 独占、已初始化的对象，可为 NULL。
 * @post 字段资源释放且结构清零，结构容器仍归调用者，空对象可重复清理。
 * @note 调用者先排空全部借用，消息与记忆集合由其各自存储拥有者清理。
 */
void cc_session_cleanup(cc_session_t *session);

/**
 * @brief 消费堆上会话数组及每项拥有字段。
 * @param[in,out] sessions 兼容 free 的独占连续数组，可为 NULL。
 * @param[in] count 已初始化且可访问的元素数量。
 * @post 字段和数组容器均释放，原数组及字段借用失效。
 * @note 调用者先排空全部使用者，各元素中的字符串分别拥有。
 */
void cc_session_free_array(cc_session_t *sessions, size_t count);

/**
 * @brief 消费一个独占堆会话，清理字段后释放容器。
 * @param[in,out] session 兼容 free 的独占对象，可为 NULL。
 * @post 对象及其字段释放，旧句柄失效。
 * @note 调用前结束全部借用，栈对象和数组元素使用 cleanup。
 */
void cc_session_destroy(cc_session_t *session);

#ifdef __cplusplus
}
#endif

#endif
