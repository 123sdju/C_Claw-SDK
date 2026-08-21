



#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 4
#define LOOPS 100


/*
 * 引用内存 session store 的 adapter 工厂。
 *
 * 测试直接声明该符号，是为了只验证 adapter 的端口契约，不把 runtime builder
 * 或配置系统引入进来。面试讲解时可以说明这是 C 项目常见的“按端口测试实现”方法。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);


/*
 * 每个线程独享一个上下文，避免测试代码自己的共享状态干扰被测对象。
 *
 * store 是所有线程共享的被测端口；index 用来生成不同 session_id；
 * failed 只由所属线程写入，主线程在 join 后读取，因此不需要额外加锁。
 */
typedef struct {
    cc_session_store_t *store;
    int index;
    int failed;
} store_ctx_t;


/*
 * 并发写入 worker：每个线程创建独立 session，并向同一个 store 实例追加消息。
 *
 * 这里覆盖的是内存 session store 的 mutex 临界区契约：append_message 必须深拷贝
 * 调用方传入的 message，worker 随后销毁本地 msg 不应影响 store 中的数据。
 */
static void *worker(void *arg)
{
    store_ctx_t *ctx = (store_ctx_t *)arg;
    char sid[32];
    snprintf(sid, sizeof(sid), "ses_%d", ctx->index);
    if (ctx->store->vtable->create_session(ctx->store->self, sid, ".").code != CC_OK) ctx->failed = 1;

    for (int i = 0; i < LOOPS; i++) {
        char mid[64];
        snprintf(mid, sizeof(mid), "msg_%d_%d", ctx->index, i);
        cc_message_t *msg = NULL;
        cc_message_create_text(mid, sid, CC_ROLE_USER, "hello", NULL, &msg);
        cc_session_record_t record = {
            .type = CC_SESSION_RECORD_MESSAGE,
            .session_id = sid,
            .data.message = msg,
        };
        if (ctx->store->vtable->append_records(ctx->store->self, &record, 1).code != CC_OK) ctx->failed = 1;
        cc_message_destroy(msg);
    }
    return NULL;
}


/*
 * 验证内存 session store 在多线程追加场景下不会丢消息或发生所有权错误。
 *
 * 主线程负责创建共享 store、启动 worker、join 后逐 session 读取消息数量。
 * load_messages 返回的是调用方拥有的数组，测试逐项 cleanup 再 free，正好覆盖
 * adapter 对“返回数组由调用方释放”的 public contract。
 */
int main(void)
{
    cc_session_store_t store;
    if (cc_memory_session_store_create(&store).code != CC_OK) return 1;


    store_ctx_t ctx[THREADS];
    cc_thread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++) {
        ctx[i].store = &store;
        ctx[i].index = i;
        ctx[i].failed = 0;
        cc_thread_create(worker, &ctx[i], &threads[i]);
    }
    for (int i = 0; i < THREADS; i++) cc_thread_join(threads[i]);


    for (int i = 0; i < THREADS; i++) {
        char sid[32];
        cc_message_t *messages = NULL;
        size_t count = 0;
        snprintf(sid, sizeof(sid), "ses_%d", i);
        store.vtable->load_messages(store.self, sid, 0, &messages, &count);
        if (count != LOOPS) ctx[i].failed = 1;

        for (size_t j = 0; j < count; j++) {
            cc_message_cleanup(&messages[j]);
        }
        free(messages);
    }


    int failed = 0;
    for (int i = 0; i < THREADS; i++) failed |= ctx[i].failed;
    store.vtable->destroy(store.self);
    return failed ? 1 : 0;
}
