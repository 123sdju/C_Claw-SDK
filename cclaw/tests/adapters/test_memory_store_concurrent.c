



#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 4
#define LOOPS 100


/** @brief 创建供所有 worker 共享的内存会话端口。
 * @param[out] out_store 必填新拥有槽，成功由主线程在 join 后 destroy。
 * @return 拥有结果，调用者逐次释放诊断，工厂失败返回空端口。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);


/*
 * 每个线程独享一个上下文，避免测试代码自己的共享状态干扰被测对象。
 *
 * store 是所有线程共享的被测端口，index 用来生成不同 session_id。
 * failed 由所属线程写入，主线程在 join 后读取。
 */
typedef struct {
    cc_session_store_t *store;
    int index;
    int failed;
} store_ctx_t;


/** @brief 在线程独有会话中追加一百条消息，写后销毁调用方副本。
 * @param[in,out] arg 必填 store_ctx_t 借用，主线程保管到 join 完成，共享 store 覆盖所有线程。
 * @return NULL，结果通过仅本线程写入的 failed 字段交付给 join 后的主线程。
 * @note 每次结果诊断本层释放，失败停止本 worker，消息临时对象在追加后销毁。
 */
static void *worker(void *arg)
{
    store_ctx_t *ctx = (store_ctx_t *)arg;
    char sid[32];
    snprintf(sid, sizeof(sid), "ses_%d", ctx->index);
    cc_result_t result = ctx->store->vtable->create_session(ctx->store->self, sid, ".");
    ctx->failed = result.code != CC_OK;
    cc_result_free(&result);
    if (ctx->failed) return NULL;

    for (int i = 0; i < LOOPS; i++) {
        char mid[64];
        snprintf(mid, sizeof(mid), "msg_%d_%d", ctx->index, i);
        cc_message_t *msg = NULL;
        result = cc_message_create_text(mid, sid, CC_ROLE_USER, "hello", NULL, &msg);
        ctx->failed = result.code != CC_OK;
        cc_result_free(&result);
        if (ctx->failed) break;
        cc_session_record_t record = {
            .type = CC_SESSION_RECORD_MESSAGE,
            .session_id = sid,
            .data.message = msg,
        };
        result = ctx->store->vtable->append_records(ctx->store->self, &record, 1);
        ctx->failed = result.code != CC_OK;
        cc_result_free(&result);
        cc_message_destroy(msg);
        if (ctx->failed) break;
    }
    return NULL;
}


/** @brief 创建共享内存 store 并验证四线程独立会话各保留一百条消息。
 * @return 创建、写入、读取、线程或计数错误为一，全部通过为零。
 * @note 只 join 已创建句柄，所有诊断逐次释放，join 完成后才读取 ctx 并销毁 store。
 * join 原生失败时结束测试进程，继续保管尚未确认退出的线程依赖。
 */
int main(void)
{
    cc_session_store_t store = {0};
    cc_result_t result = cc_memory_session_store_create(&store);
    int failed = result.code != CC_OK;
    cc_result_free(&result);
    if (failed) return 1;


    store_ctx_t ctx[THREADS] = {0};
    cc_thread_t threads[THREADS] = {0};
    int started = 0;
    for (int i = 0; i < THREADS; i++) {
        ctx[i].store = &store;
        ctx[i].index = i;
        ctx[i].failed = 0;
        result = cc_thread_create(worker, &ctx[i], &threads[i]);
        failed = result.code != CC_OK;
        cc_result_free(&result);
        if (failed) break;
        started++;
    }
    for (int i = 0; i < started; i++) {
        result = cc_thread_join(threads[i]);
        int joined = result.code == CC_OK;
        cc_result_free(&result);
        if (!joined) return 1;
    }


    for (int i = 0; i < started; i++) {
        char sid[32];
        cc_message_t *messages = NULL;
        size_t count = 0;
        snprintf(sid, sizeof(sid), "ses_%d", i);
        result = store.vtable->load_messages(store.self, sid, 0, &messages, &count);
        if (result.code != CC_OK) ctx[i].failed = 1;
        cc_result_free(&result);
        if (count != LOOPS) ctx[i].failed = 1;

        for (size_t j = 0; j < count; j++) {
            cc_message_cleanup(&messages[j]);
        }
        free(messages);
    }


    for (int i = 0; i < started; i++) failed |= ctx[i].failed;
    store.vtable->destroy(store.self);
    return failed ? 1 : 0;
}
