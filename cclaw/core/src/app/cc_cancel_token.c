



#include "cc/app/cc_cancel_token.h"
#include "cc/ports/cc_thread.h"

#include <stdlib.h>


/** 取消源拥有状态、锁和 token，外层协调所有使用者与最终销毁。 */
struct cc_cancel_source {
    cc_mutex_t mutex;         /**< 对象拥有，保护 cancelled 的读写。 */
    int cancelled;           /**< 初始为零，cancel 后单调保持一。 */
    cc_cancel_token_t *token; /**< 对象拥有的独立堆分配 token，创建后身份固定。 */
};

/** token 以借用回指访问取消源的共享状态，其内存由 source 统一释放。 */
struct cc_cancel_token {
    cc_cancel_source_t *source; /**< 创建成功后固定指向拥有该 token 的源。 */
};

/**
 * @brief 按 source、token、mutex 的顺序构造取消对象，全部成功后绑定回指并发布输出。
 * @param[out] out_source 必填且无待释放对象的输出槽，仅成功时填写，失败保持原值。
 * @return 成功为 CC_OK，空输出为 CC_ERR_INVALID_ARGUMENT，两次堆分配失败为 CC_ERR_OUT_OF_MEMORY，mutex 创建错误原样返回并清理已分配对象。
 */
cc_result_t cc_cancel_source_create(cc_cancel_source_t **out_source)
{
    if (!out_source) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null cancel source output");
    }
    cc_cancel_source_t *source = calloc(1, sizeof(cc_cancel_source_t));
    if (!source) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate cancel source");
    }
    source->token = calloc(1, sizeof(cc_cancel_token_t));
    if (!source->token) {
        free(source);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate cancel token");
    }
    cc_result_t rc = cc_mutex_create(&source->mutex);
    if (rc.code != CC_OK) {
        free(source->token);
        free(source);
        return rc;
    }
    source->token->source = source;
    *out_source = source;
    return cc_result_ok();
}

/**
 * @brief 销毁 mutex 后释放 token 和取消源。
 * @pre 拥有者已排空全部查询、取消、token 获取及 provider/tool 借用，所有锁持有者和等待者均已退出。
 * @param[in,out] source 拥有的有效源，允许 NULL。
 * @post 非空源及其配套 token 已释放，外层借用指针失效。
 */
void cc_cancel_source_destroy(cc_cancel_source_t *source)
{
    if (!source) return;
    cc_mutex_destroy(source->mutex);
    free(source->token);
    free(source);
}

/**
 * @brief 锁内将 cancelled 置一，重复取消继续保持一。
 * @param[in,out] source 调用期间保持存活的取消源，允许 NULL，销毁由外层另行排空。
 * @post 非空源的取消状态已发布，查询会通过同一 mutex 读取，下游退出时机由其自身检查决定。
 */
void cc_cancel_source_cancel(cc_cancel_source_t *source)
{
    if (!source) return;


    cc_mutex_lock(source->mutex);
    source->cancelled = 1;
    cc_mutex_unlock(source->mutex);
}

/**
 * @brief 直接读取创建成功后固定的 token 指针。
 * @param[in] source 借用的存活源，允许 NULL，外层保证读取期间持续有效。
 * @return source 拥有的 token 借用指针，使用期须在源销毁前结束，空源返回 NULL。
 */
cc_cancel_token_t *cc_cancel_source_token(cc_cancel_source_t *source)
{
    return source ? source->token : NULL;
}

/**
 * @brief 读取 token 的源并在源 mutex 内取得 cancelled 快照。
 * @param[in] token 同步借用的查询句柄，允许 NULL，外层维持 token 及 source 的寿命。
 * @return 源已取消返回一，未取消、空 token 或空源回指返回零。
 */
int cc_cancel_token_is_cancelled(cc_cancel_token_t *token)
{
    if (!token || !token->source) return 0;
    cc_cancel_source_t *source = token->source;
    cc_mutex_lock(source->mutex);
    int cancelled = source->cancelled;
    cc_mutex_unlock(source->mutex);
    return cancelled;
}
