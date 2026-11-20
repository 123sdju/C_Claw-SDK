



#ifndef CC_SESSION_MANAGER_H
#define CC_SESSION_MANAGER_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_session_store.h"

/**
 * @brief 提供会话创建、用户消息追加和列表读取的轻量包装器。
 *
 * store 句柄按值复制，self 和 vtable 借用至 manager 销毁，底层存储由外层 owner 释放。
 * manager 没有内部锁，共享后端的并发由其实现及外层调用约定协调。
 */
typedef struct cc_session_manager {
    cc_session_store_t store; /**< 按值保存的借用后端，manager 仅拥有自身容器。 */
} cc_session_manager_t;

/**
 * @brief 分配管理器并按值复制存储句柄。
 * @param[in] store 借用后端，允许空虚表，各入口分别处理可选能力，外层保持其寿命。
 * @param[out] out_manager 必填，先置 NULL，成功取得拥有对象，使用 cc_session_manager_destroy 释放。
 * @return CC_OK 表示创建成功，空输出返回 CC_ERR_INVALID_ARGUMENT，分配失败返回 CC_ERR_OUT_OF_MEMORY。
 */
cc_result_t cc_session_manager_create(
    cc_session_store_t store,
    cc_session_manager_t **out_manager
);

/**
 * @brief 释放管理器容器，存储后端仍归外层 owner 管理。
 * @pre 外层已排空使用该 manager 的调用。
 * @param[in,out] manager 待销毁对象，可为 NULL。
 * @post manager 容器已释放，原 store self 和 vtable 的寿命继续由外层维护。
 */
void cc_session_manager_destroy(cc_session_manager_t *manager);

/**
 * @brief 后端提供 create_session 时转交会话创建请求。
 * @param[in] manager 非空借用管理器，调用期间有效。
 * @param[in] session_id 非空指针的会话 ID，具体内容合法性由后端核对，调用期间借用。
 * @param[in] workspace_dir 可选工作区路径，原样传给后端，调用期间借用。
 * @return 参数缺失返回 CC_ERR_INVALID_ARGUMENT，缺少创建能力返回 CC_OK，其他原样返回后端结果。
 * 底层失败时可能已有持久化进展，manager 保持借用句柄。
 */
cc_result_t cc_session_manager_ensure_session(
    cc_session_manager_t *manager,
    const char *session_id,
    const char *workspace_dir
);

/**
 * @brief 生成消息 UUID 和本地时间，构造用户消息后同步追加一条存储记录。
 * @param[in] manager 非空借用管理器，后端须提供 append_records。
 * @param[in] session_id 非空指针的会话 ID，调用期间借用，空 ID 的合法性由后端核对。
 * @param[in] content 非空指针的消息正文，允许空字符串，复制进临时消息。
 * @return 参数缺失返回 CC_ERR_INVALID_ARGUMENT，缺少追加能力返回 CC_ERR_STORAGE，ID 或消息构造错误向上传递。
 * 时间转换、格式化或复制失败统一返回 CC_ERR_OUT_OF_MEMORY，其他返回后端追加结果。
 * 临时消息在后端返回后销毁，后端须在调用内完成深拷贝或序列化，失败也可能已有磁盘写入。
 */
cc_result_t cc_session_manager_append_user_message(
    cc_session_manager_t *manager,
    const char *session_id,
    const char *content
);

/**
 * @brief 读取后端会话列表，空管理器或缺少列表能力时返回空列表。
 * @param[in] manager 借用管理器，可为 NULL。
 * @param[out] out_sessions 必填独立输出，缺少能力时置 NULL，否则由后端填写拥有数组，调用者 cc_session_free_array。
 * @param[out] out_count 必填独立输出，缺少能力时置零，否则由后端填写条目数。
 * @return 输出参数缺失返回 CC_ERR_INVALID_ARGUMENT 并保持其他输出原值，缺少能力返回 CC_OK，其他返回后端结果。
 * 后端失败时输出责任遵守后端契约，调用者清理实际交出的拥有数组。
 */
cc_result_t cc_session_manager_list_sessions(
    cc_session_manager_t *manager,
    cc_session_t **out_sessions,
    size_t *out_count
);

#endif
