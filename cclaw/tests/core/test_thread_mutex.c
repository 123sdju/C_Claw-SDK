



#include "cc/ports/cc_thread.h"

#include <stdio.h>

#define THREADS 8
#define LOOPS 10000

/*
 * 测试用共享计数器。
 *
 * value 是故意共享的临界资源，mutex 用来验证平台 thread port 的互斥语义。
 */
typedef struct {
    cc_mutex_t mutex;
    int value;
} counter_t;

/* 每个 worker 重复加锁递增计数器，用高循环次数放大数据竞争问题。 */
static void *worker(void *arg)
{
    counter_t *counter = (counter_t *)arg;
    for (int i = 0; i < LOOPS; i++) {
        cc_mutex_lock(counter->mutex);
        counter->value++;
        cc_mutex_unlock(counter->mutex);
    }
    return NULL;
}

/*
 * 验证 cc_thread/cc_mutex 基本契约。
 *
 * 如果 mutex 实现失效，最终计数通常会小于 THREADS * LOOPS；该测试是所有平台 profile
 * 的基础并发健康检查。
 */
int main(void)
{
    counter_t counter = {0};

    if (cc_mutex_create(&counter.mutex).code != CC_OK) return 1;


    cc_thread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++) {
        if (cc_thread_create(worker, &counter, &threads[i]).code != CC_OK) return 1;
    }


    for (int i = 0; i < THREADS; i++) {
        if (cc_thread_join(threads[i]).code != CC_OK) return 1;
    }


    cc_mutex_destroy(counter.mutex);


    if (counter.value != THREADS * LOOPS) {
        fprintf(stderr, "counter=%d\n", counter.value);
        return 1;
    }
    return 0;
}
