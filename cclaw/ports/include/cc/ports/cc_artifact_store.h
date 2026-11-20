

#ifndef CC_ARTIFACT_STORE_H
#define CC_ARTIFACT_STORE_H

#include "cc/core/cc_media.h"
#include "cc/core/cc_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 媒体元数据端口，实际内容由 path/uri/data_base64 引用，内容管理能力依后端。 */
typedef struct cc_artifact_store cc_artifact_store_t;

/**
 * @brief 媒体方法表，输入同步借用，返回结果的动态诊断由调用者释放。
 * @note 同步策略由后端说明，内存实现没有内部锁，owner 串行调用并在销毁前关闭入口、排空方法。
 */
typedef struct cc_artifact_store_vtable {
    /**
     * @brief 保存媒体元数据，成功后后端不保留调用者临时字段指针。
     * @param[in,out] self 存活后端实例。
     * @param[in] session_id 可空调用期借用过滤键，具体会话关联由后端保存。
     * @param[in] artifact 调用期稳定输入，ID 要求及策略校验由后端规定。
     * @return 成功或具体后端错误，输入拥有权始终归调用者。
     */
    cc_result_t (*put)(
        void *self,
        const char *session_id,
        const cc_media_artifact_t *artifact
    );
    /**
     * @brief 按 ID 取得独立媒体元数据。
     * @param[in] self 存活后端实例。
     * @param[in] artifact_id 调用期借用 ID。
     * @param[out] out_artifact 新或已清理槽，成功字段由调用者 media_artifact_cleanup。
     * @return 成功、NOT_FOUND 或具体后端错误，失败输出初始化方式依实现契约。
     */
    cc_result_t (*get)(
        void *self,
        const char *artifact_id,
        cc_media_artifact_t *out_artifact
    );
    /**
     * @brief 取得会话媒体列表的独立副本。
     * @param[in] self 存活后端实例。
     * @param[in] session_id 可空调用期借用过滤键，内存实现 NULL 选全部，空串为实际键。
     * @param[out] out_artifacts 新或已清理拥有包装，成功由调用者 media_artifact_list_cleanup。
     * @return 成功或具体错误，部分输出清理和排序按后端契约。
     */
    cc_result_t (*list)(
        void *self,
        const char *session_id,
        cc_media_artifact_list_t *out_artifacts
    );
    /**
     * @brief 删除指定 ID 的记录，实际文件处理依后端能力。
     * @param[in,out] self 存活后端实例。
     * @param[in] artifact_id 调用期借用 ID。
     * @return 成功、NOT_FOUND 或具体后端错误，内存实现仅清理元数据。
     */
    cc_result_t (*remove)(void *self, const char *artifact_id);
    /**
     * @brief 消费后端实例与其拥有资源。
     * @param[in,out] self 独占实例，owner 已关闭入口并排空方法与内部字段借用。
     * @post self 和旧别名失效，描述符存储由调用者清空。
     */
    void (*destroy)(void *self);
} cc_artifact_store_vtable_t;

/** @brief 拥有实例与借用方法表的描述符，浅复制只产生别名，销毁责任须保持唯一。 */
struct cc_artifact_store {
    void *self; /**< 后端拥有状态。 */
    const cc_artifact_store_vtable_t *vtable; /**< 至少覆盖实例寿命的方法表。 */
};

#ifdef __cplusplus
}
#endif

#endif
