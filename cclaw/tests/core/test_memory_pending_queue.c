#include "cc/app/cc_memory_pending_queue.h"
#include "cc/ports/cc_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)
/**
 * @brief 检查成功并消费结果诊断。
 * @param[in] rc 按值移交的拥有结果。
 * @return 成功一，失败零，输入拥有字段在返回前释放。
 */
static int ok(cc_result_t rc) { int good=rc.code==CC_OK; cc_result_free(&rc); return good; }
typedef struct reserve_race {
    cc_memory_pending_queue_t *queue; const char *turn;
    cc_memory_pending_reservation_t *reservation; cc_error_code_t code;
} reserve_race_t;
/**
 * @brief 在共享队列上争抢一次预留并保存独占线程结果。
 * @param[in,out] arg reserve_race_t 借用，queue/turn 覆盖线程期，结果仅本线程写。
 * @return NULL，拥有 reservation 成功时交给主线程取消，结果诊断在本线程消费。
 * @note 队列内部串行化预留竞争，主线程 join 后读取输出并销毁队列。
 */
static void *reserve_thread(void *arg)
{
    reserve_race_t *race=arg;
    cc_result_t rc=cc_memory_pending_reserve(race->queue,race->turn,"s","input",16,&race->reservation);
    race->code=rc.code; cc_result_free(&rc); return NULL;
}
/**
 * @brief 验证 FIFO、去重、快照转移、字节计费、预留扩容提交和单槽并发争抢。
 * @return 全部断言通过零，失败一，成功路径清理消息副本、快照、预留及两个队列。
 * @note 两线程使用各自结果槽，join 后验证恰一成功，再排空预留并销毁队列。
 */
int main(void)
{
    cc_memory_pending_queue_t *queue=NULL; CHECK(ok(cc_memory_pending_queue_create(2,256,&queue)));
    CHECK(ok(cc_memory_pending_enqueue(queue,"t1","s1","input 1","answer 1")));
    CHECK(ok(cc_memory_pending_enqueue(queue,"t1","s1","input 1","answer 1")));
    cc_result_t rc=cc_memory_pending_enqueue(queue,"t1","s1","changed","answer 1");
    CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    CHECK(ok(cc_memory_pending_enqueue(queue,"t2","s2","input 2","answer 2")));
    rc=cc_memory_pending_enqueue(queue,"t3","s3","input 3","answer 3");
    CHECK(rc.code==CC_ERR_LIMIT_EXCEEDED); cc_result_free(&rc);
    size_t count=0,bytes=0; cc_memory_pending_status(queue,&count,&bytes); CHECK(count==2 && bytes>0);
    CHECK(ok(cc_memory_pending_set_snapshot(queue,"t1","rewrite-id")));
    CHECK(ok(cc_memory_pending_set_snapshot(queue,"t1","rewrite-id")));
    rc=cc_memory_pending_set_snapshot(queue,"t1","different"); CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    cc_memory_pending_item_t item={0}; CHECK(ok(cc_memory_pending_peek(queue,&item)));
    CHECK(strcmp(item.turn_id,"t1")==0 && strcmp(item.snapshot_id,"rewrite-id")==0); cc_memory_pending_item_cleanup(&item);
    rc=cc_memory_pending_clear_snapshot(queue,"t1","wrong"); CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    CHECK(ok(cc_memory_pending_clear_snapshot(queue,"t1","rewrite-id")));
    char *owned=malloc(16); CHECK(owned); strcpy(owned,"rewrite-owned");
    CHECK(ok(cc_memory_pending_take_snapshot(queue,"t1",&owned)) && !owned);
    CHECK(ok(cc_memory_pending_peek(queue,&item)) && strcmp(item.snapshot_id,"rewrite-owned")==0);
    cc_memory_pending_item_cleanup(&item);
    rc=cc_memory_pending_ack(queue,"t2"); CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    CHECK(ok(cc_memory_pending_ack(queue,"t1")));
    CHECK(ok(cc_memory_pending_enqueue(queue,"t3","s3","input 3","answer 3")));
    CHECK(ok(cc_memory_pending_peek(queue,&item)) && strcmp(item.turn_id,"t2")==0); cc_memory_pending_item_cleanup(&item);
    CHECK(ok(cc_memory_pending_ack(queue,"t2")) && ok(cc_memory_pending_ack(queue,"t3")));
    cc_memory_pending_status(queue,&count,&bytes); CHECK(count==0 && bytes==0);
    rc=cc_memory_pending_peek(queue,&item); CHECK(rc.code==CC_ERR_NOT_FOUND); cc_result_free(&rc);
    cc_memory_pending_reservation_t *reservation=NULL;
    CHECK(ok(cc_memory_pending_reserve(queue,"t4","s4","input 4",32,&reservation)) && reservation);
    CHECK(ok(cc_memory_pending_verify(queue,reservation,"t4","s4","input 4")));
    cc_result_t mismatch=cc_memory_pending_verify(queue,reservation,"t4","s4","different");
    CHECK(mismatch.code==CC_ERR_INVALID_STATE); cc_result_free(&mismatch);
    size_t ready=0,reserved=0,charged=0;
    cc_memory_pending_status_detail(queue,&ready,&reserved,&charged); CHECK(!ready && reserved==1 && charged>0);
    cc_memory_pending_status(queue,&count,&bytes); CHECK(count==1 && bytes==charged);
    rc=cc_memory_pending_peek(queue,&item); CHECK(rc.code==CC_ERR_NOT_FOUND); cc_result_free(&rc);
    rc=cc_memory_pending_enqueue(queue,"t4","s4","input 4","answer"); CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    cc_memory_pending_reservation_t *duplicate=NULL;
    rc=cc_memory_pending_reserve(queue,"t4","s4","input 4",8,&duplicate);
    CHECK(rc.code==CC_ERR_INVALID_STATE && !duplicate); cc_result_free(&rc);
    cc_memory_pending_queue_t *other=NULL; CHECK(ok(cc_memory_pending_queue_create(1,256,&other)));
    rc=cc_memory_pending_commit(other,&reservation,"answer"); CHECK(rc.code==CC_ERR_INVALID_STATE && reservation); cc_result_free(&rc);
    CHECK(ok(cc_memory_pending_enqueue(queue,"t5","s5","input 5","answer 5")));
    rc=cc_memory_pending_commit(queue,&reservation,"012345678901234567890123456789012");
    CHECK(rc.code==CC_ERR_LIMIT_EXCEEDED && reservation); cc_result_free(&rc);
    rc=cc_memory_pending_expand(other,reservation,33); CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    rc=cc_memory_pending_expand(queue,reservation,256); CHECK(rc.code==CC_ERR_LIMIT_EXCEEDED); cc_result_free(&rc);
    cc_memory_pending_status(queue,&count,&bytes); CHECK(count==2 && bytes>charged);
    CHECK(ok(cc_memory_pending_ack(queue,"t5")));
    CHECK(ok(cc_memory_pending_expand(queue,reservation,33)));
    charged++;
    CHECK(ok(cc_memory_pending_commit(queue,&reservation,"012345678901234567890123456789012")) && !reservation);
    cc_memory_pending_status_detail(queue,&ready,&reserved,&bytes); CHECK(ready==1 && !reserved && bytes==charged);
    CHECK(ok(cc_memory_pending_peek(queue,&item)) && strcmp(item.turn_id,"t4")==0 &&
          strcmp(item.assistant_text,"012345678901234567890123456789012")==0);
    cc_memory_pending_item_cleanup(&item); CHECK(ok(cc_memory_pending_ack(queue,"t4")));
    CHECK(ok(cc_memory_pending_reserve(queue,"t6","s6","input 6",16,&reservation)));
    CHECK(ok(cc_memory_pending_cancel(queue,&reservation)) && !reservation);
    cc_memory_pending_status(queue,&count,&bytes); CHECK(!count && !bytes);
    cc_memory_pending_queue_destroy(queue); cc_memory_pending_queue_destroy(other);

    CHECK(ok(cc_memory_pending_queue_create(1,256,&queue)));
    reserve_race_t first={queue,"race-a",NULL,CC_OK}, second={queue,"race-b",NULL,CC_OK};
    cc_thread_t a=NULL,b=NULL;
    CHECK(ok(cc_thread_create(reserve_thread,&first,&a)) && ok(cc_thread_create(reserve_thread,&second,&b)));
    CHECK(ok(cc_thread_join(a)) && ok(cc_thread_join(b)));
    CHECK((first.code==CC_OK && second.code==CC_ERR_LIMIT_EXCEEDED) ||
          (second.code==CC_OK && first.code==CC_ERR_LIMIT_EXCEEDED));
    cc_memory_pending_status_detail(queue,&ready,&reserved,&bytes); CHECK(!ready && reserved==1 && bytes>0);
    if (first.reservation) CHECK(ok(cc_memory_pending_cancel(queue,&first.reservation)));
    if (second.reservation) CHECK(ok(cc_memory_pending_cancel(queue,&second.reservation)));
    cc_memory_pending_status(queue,&count,&bytes); CHECK(!count && !bytes);
    cc_memory_pending_queue_destroy(queue); return 0;
}
