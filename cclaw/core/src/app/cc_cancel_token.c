



#include "cc/app/cc_cancel_token.h"
#include "cc/ports/cc_thread.h"

#include <stdlib.h>


/*
 * 取消源内部状态。
 *
 * cancelled 受 mutex 保护，token 由 source 拥有。这样多个线程可以查询取消状态，
 * runtime 拥有者可以在另一个线程发出取消。
 */
struct cc_cancel_source {
    cc_mutex_t mutex;
    int cancelled;
    cc_cancel_token_t *token;
};

/* token 只保存回指 source，不拥有任何同步原语。 */
struct cc_cancel_token {
    cc_cancel_source_t *source;
};

/*
 * 创建取消源和配套 token。
 *
 * 构造顺序是 source -> token -> mutex；失败时按反向顺序释放。成功后 token->source
 * 指向 source，查询函数可以通过它读取共享取消状态。
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

/*
 * 销毁取消源。
 *
 * 这里不做引用计数，调用方必须保证 provider/tool/process 不再持有 token。嵌入式项目
 * 通常用运行队列或 join/flush 保证这个生命周期边界。
 */
void cc_cancel_source_destroy(cc_cancel_source_t *source)
{
    if (!source) return;
    cc_mutex_destroy(source->mutex);
    free(source->token);
    free(source);
}

/*
 * 发出取消信号。
 *
 * cancelled 是单调状态：从 0 变成 1 后不会恢复。加锁保证其他线程查询时能看到一致值。
 */
void cc_cancel_source_cancel(cc_cancel_source_t *source)
{
    if (!source) return;


    cc_mutex_lock(source->mutex);
    source->cancelled = 1;
    cc_mutex_unlock(source->mutex);
}

/*
 * 获取只读 token。
 *
 * token 仍由 source 拥有，调用方不能释放它；NULL source 返回 NULL，便于可选取消参数。
 */
cc_cancel_token_t *cc_cancel_source_token(cc_cancel_source_t *source)
{
    return source ? source->token : NULL;
}

/*
 * 查询取消状态。
 *
 * NULL token 视为未取消，这让很多 API 可以接受可选 token 而不用在调用点加分支。
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
