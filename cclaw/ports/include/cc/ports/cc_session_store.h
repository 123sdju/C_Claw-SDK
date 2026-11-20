



#ifndef CC_SESSION_STORE_H
#define CC_SESSION_STORE_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_message.h"
#include "cc/core/cc_tool_call.h"
#include "cc/core/cc_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 会话后端同步方法表。 */
typedef struct cc_session_store_vtable cc_session_store_vtable_t;

/** @brief 会话端口描述符，拥有关系由创建者交付给唯一 owner。 */
typedef struct cc_session_store cc_session_store_t;

/** @brief 一个逻辑批次中记录的输入种类。 */
typedef enum cc_session_record_type {
    CC_SESSION_RECORD_MESSAGE = 0,
    CC_SESSION_RECORD_TOOL_CALL = 1,
    CC_SESSION_RECORD_TOOL_RESULT = 2,
} cc_session_record_type_t;

/**
 * @brief append_records 的同步借用记录，输入数组和其字段覆盖整个调用。
 * @note message 的会话身份保存在消息自身，工具记录使用本结构 session_id。
 * 后端须复制需长期保管的字段，调用者始终拥有源对象。
 */
typedef struct cc_session_record {
    cc_session_record_type_t type; /**< 决定本次读取的 union 成员。 */
    const char *session_id; /**< 工具记录的会话键，消息记录的后端实现可使用消息自身键。 */
    union {
        const cc_message_t *message;
        const cc_tool_call_t *tool_call;
        struct {
            const char *tool_call_id;
            const cc_tool_result_t *result;
        } tool_result;
    } data;
} cc_session_record_t;

/** @brief 拥有后端状态并借用其方法表，浅复制只产生别名，不能重复销毁。 */
struct cc_session_store {
    void *self; /**< 后端实例，使用与销毁的寿命由 owner 管理。 */
    const cc_session_store_vtable_t *vtable; /**< 至少覆盖实例寿命的稳定方法表。 */
};


/**
 * @brief 会话读写同步接口，cc_result_t 的动态诊断由接收者释放。
 * @note 返回消息逐项 message_cleanup 后 free，会话数组通过 session_free_array 归还。
 * 内部同步和存储恢复范围依后端，销毁前 owner 关闭入口并排空全部在途方法。
 */
struct cc_session_store_vtable {


    /**
     * @brief 确保会话记录存在，工作目录处理及重复 ID 行为由后端规定。
     * @param[in,out] self 存活后端实例。
     * @param[in] session_id 非 NULL 调用期借用 ID，空串支持依后端。
     * @param[in] workspace_dir 可空调用期借用路径，NULL 的默认由后端选择。
     * @return 成功或具体错误，输入拥有权保持不变，实际目录建立依后端。
     */
    cc_result_t (*create_session)(
        void *self,
        const char *session_id,
        const char *workspace_dir
    );



    /**
     * @brief 以一个逻辑提交单位追加消息、工具调用和工具结果。
     * @param[in,out] self 存活后端实例。
     * @param[in] records 调用期稳定借用数组，count 为零时可为 NULL。
     * @param[in] count 数组真实记录数，每项须提供 type 对应的有效输入。
     * @return 成功或具体后端错误，禁止发布批次前缀，持久后端的提交未知由其恢复契约解释。
     * @note 输入始终归调用者，失败后的重试须结合后端是否已安装整批判断。
     */
    cc_result_t (*append_records)(
        void *self,
        const cc_session_record_t *records,
        size_t count
    );



    /**
     * @brief 读取指定会话最近若干消息，返回顺序遵循历史顺序。
     * @param[in] self 存活后端实例。
     * @param[in] session_id 非 NULL 调用期稳定过滤键。
     * @param[in] limit 正数限制项数，零或负数读取全部。
     * @param[out] out_messages 必填新拥有槽，成功为独立消息数组，空结果为 NULL。
     * @param[out] out_count 必填成功项数槽，失败输出初始化时机由后端契约说明。
     * @return 成功或具体错误，成功数组逐项 cleanup 后 free，字段完整加载语义由后端提供。
     */
    cc_result_t (*load_messages)(
        void *self,
        const char *session_id,
        int limit,
        cc_message_t **out_messages,
        size_t *out_count
    );



    /**
     * @brief 列举后端可见的会话描述。
     * @param[in] self 存活后端实例。
     * @param[out] out_sessions 必填新拥有槽，成功为独立数组，空结果为 NULL。
     * @param[out] out_count 必填成功项数槽。
     * @return 成功或具体错误，成功数组交调用者 session_free_array，排序及失败输出依后端。
     */
    cc_result_t (*list_sessions)(
        void *self,
        cc_session_t **out_sessions,
        size_t *out_count
    );



    /**
     * @brief 清空指定会话的消息和工具历史。
     * @param[in,out] self 存活后端实例。
     * @param[in] session_id 非 NULL 调用期借用 ID。
     * @return 成功或具体后端错误，未命中、会话描述与媒体文件处理依后端。
     */
    cc_result_t (*clear_session)(
        void *self,
        const char *session_id
    );



    /**
     * @brief 消费实例拥有资源并结束其寿命。
     * @param[in,out] self 独占实例，owner 已关闭入口并排空全部方法。
     * @post self 及内部借用失效，端口描述符存储由调用者清空。
     */
    void (*destroy)(void *self);

    /**
     * @brief 可选地先读取结构和媒体引用，配合 hydrate_context_media 在筛选后加载内容。
     * @param[in] self 存活后端实例。
     * @param[in] session_id 非 NULL 调用期借用 ID。
     * @param[in] limit 正数限制最近项数，零或负数读取全部。
     * @param[out] out_messages 必填新拥有槽，成功交付元数据消息数组。
     * @param[out] out_count 必填实际项数槽。
     * @return 成功或具体后端错误，成功数组逐项 message_cleanup 后 free。
     * @note 本指针可为 NULL，调用者仅在两个媒体扩展均存在时采用延迟加载链路。
     */
    cc_result_t (*load_message_metadata)(
        void *self, const char *session_id, int limit,
        cc_message_t **out_messages, size_t *out_count);

    /**
     * @brief 可选地为已筛选消息补充媒体，先保留必需消息再从较新内容使用余量。
     * @param[in] self 存活后端实例。
     * @param[in] session_id 非 NULL 调用期借用 ID。
     * @param[in,out] messages 调用者独占消息数组，count 为零时可为 NULL。
     * @param[in] count 数组实际项数。
     * @param[in] required_message 必须保留的当前消息下标，等于 count 表示无必需项。
     * @param[in] max_base64_bytes 全部选中媒体的编码字节预算，零表示本入口不限。
     * @return 成功或媒体/IO/容量错误，失败可已修改部分消息，整个数组仍由调用者清理。
     * @note 非必需媒体超预算时可转为文字描述，必需项无法满足时失败。本指针可为 NULL。
     */
    cc_result_t (*hydrate_context_media)(
        void *self, const char *session_id, cc_message_t *messages, size_t count,
        size_t required_message, size_t max_base64_bytes);
};

#ifdef __cplusplus
}
#endif

#endif
