#include "cc/app/cc_memory_pending_queue.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_thread.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PENDING_SNAPSHOT_RESERVE 64U

struct cc_memory_pending_queue {
    cc_memory_pending_item_t *items; /**< 拥有的环形槽数组，仅 count 个已提交槽有效。 */
    size_t *charges; /**< 各槽的完整计费，commit 后仍包含整块答案预留。 */
    size_t capacity, max_bytes, count, head, bytes;
    size_t reserved_count; /**< 尚未提交的预留条数，与 count 共用 capacity。 */
    struct cc_memory_pending_reservation *reservations; /**< 拥有的预留链表，提交时移入 FIFO。 */
    cc_mutex_t mutex; /**< 保护槽、链表和计费，销毁由调用者先排空并发访问。 */
};

struct cc_memory_pending_reservation {
    cc_memory_pending_item_t item;
    size_t max_answer_bytes; /**< 答案正文容量，缓冲另含一个 NUL 字节。 */
    size_t charge; /**< 原输入、答案完整容量及快照 ID 预留的合计字节数。 */
    struct cc_memory_pending_reservation *next;
};

/**
 * @brief 清零拥有字符串的已使用正文后释放，调用者保证其为有效 NUL 结尾字符串。
 * @param[in,out] value 调用者交给本函数清零并释放的拥有字符串，可为 NULL。
 * @post 输入正文已清零并释放，来参值本身由调用方管理。
 */
static void wipe_free(char *value)
{
    if (!value) return;
    size_t length = strlen(value);
    volatile char *p = (volatile char *)value;
    while (length) p[--length] = 0;
    free(value);
}

/**
 * @brief 清理一个拥有项的全部字符串并清零结构，适用于副本与已移出的队列项。
 * @param[in,out] item 待清理的拥有项目，可为 NULL，各字符串须分别可由 free 释放。
 * @post 拥有字符串的正文已清零并释放，item 结构已清零。
 */
void cc_memory_pending_item_cleanup(cc_memory_pending_item_t *item)
{
    if (!item) return;
    wipe_free(item->turn_id); wipe_free(item->session_id);
    wipe_free(item->user_input); wipe_free(item->assistant_text); wipe_free(item->snapshot_id);
    memset(item, 0, sizeof(*item));
}

/**
 * @brief 将字符串长度及 NUL 加入计费值，空指针不计费，溢出时返回零。
 * @param[in,out] total 累加中的计费字节数，溢出前检查并返回失败。
 * @param[in] value 仅读取长度的借用字符串，可为 NULL。
 * @return 成功累加时为一，size_t 溢出时为零。
 */
static int add_length(size_t *total, const char *value)
{
    if (!value) return 1;
    size_t length = strlen(value);
    if (length == SIZE_MAX || *total > SIZE_MAX - length - 1U) return 0;
    *total += length + 1U;
    return 1;
}

/**
 * @brief 统计项字符串及固定 64 字节快照 ID 预留，实际 ID 含 NUL 需放入该预留。
 *
 * 此计费仅包含字符串和快照 ID 预留，结构体、数组、分配器开销及处理副本另占内存。
 * @param[in] item 调用期间借用的项目。
 * @param[out] out 累加中的计费字节，失败时可能只写入部分长度，调用者须丢弃该值。
 * @return 长度及快照预留合法时为一并输出计费值，溢出或 ID 过长时为零。
 */
static int item_bytes(const cc_memory_pending_item_t *item, size_t *out)
{
    *out = PENDING_SNAPSHOT_RESERVE;
    if (item->snapshot_id && strlen(item->snapshot_id) + 1U > PENDING_SNAPSHOT_RESERVE) return 0;
    return add_length(out,item->turn_id) && add_length(out,item->session_id) &&
        add_length(out,item->user_input) && add_length(out,item->assistant_text);
}

/**
 * @brief 深拷贝已经验证的输入项，成功副本归调用者 cleanup，失败清理部分字段。
 * @param[in] source 已验证的待复制项目，字符串在调用期间借用。
 * @param[out] out 无待释放资源的输出结构，先清零，成功拥有副本，失败保持空结构。
 * @return CC_OK 表示深拷贝成功，分配失败清理本次字段并返回 CC_ERR_OUT_OF_MEMORY。
 */
static cc_result_t item_copy(const cc_memory_pending_item_t *source, cc_memory_pending_item_t *out)
{
    memset(out, 0, sizeof(*out));
    out->turn_id = cc_copy_string(source->turn_id);
    out->session_id = cc_copy_string(source->session_id);
    out->user_input = cc_copy_string(source->user_input);
    out->assistant_text = cc_copy_string(source->assistant_text);
    out->snapshot_id = source->snapshot_id ? cc_copy_string(source->snapshot_id) : NULL;
    if (!out->turn_id || !out->session_id || !out->user_input || !out->assistant_text ||
        (source->snapshot_id && !out->snapshot_id)) {
        cc_memory_pending_item_cleanup(out);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy pending memory turn");
    }
    return cc_result_ok();
}

/**
 * @brief 创建固定槽数组、计费数组及 mutex，字节限制由调用者提供。
 *
 * 成功输出拥有队列，失败归还本次分配。
 * 队列须覆盖所有预留句柄寿命。
 * @param[in] max_items FIFO 项与尚未提交预留合计的最大条数，必须大于零。
 * @param[in] max_bytes 对话字符串及 snapshot ID 预留的总计费上限，必须大于零。
 * @param[out] out_queue 参数合法后先置 NULL，成功取得拥有的队列，其寿命须覆盖所有预留句柄。
 * @return CC_OK 表示成功，非法限制为 CC_ERR_INVALID_ARGUMENT，分配失败为 CC_ERR_OUT_OF_MEMORY，锁创建错误原样返回。
 */
cc_result_t cc_memory_pending_queue_create(size_t max_items, size_t max_bytes,
                                            cc_memory_pending_queue_t **out_queue)
{
    if (!out_queue || !max_items || !max_bytes || max_items > SIZE_MAX / sizeof(cc_memory_pending_item_t))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pending memory queue limits");
    *out_queue = NULL;
    cc_memory_pending_queue_t *queue = calloc(1, sizeof(*queue));
    if (!queue) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate pending memory queue");
    queue->items = calloc(max_items, sizeof(*queue->items));
    if (!queue->items) { free(queue); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate pending memory items"); }
    queue->charges = calloc(max_items, sizeof(*queue->charges));
    if (!queue->charges) { free(queue->items); free(queue); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate pending memory accounting"); }
    cc_result_t rc = cc_mutex_create(&queue->mutex);
    if (rc.code != CC_OK) { free(queue->charges); free(queue->items); free(queue); return rc; }
    queue->capacity = max_items; queue->max_bytes = max_bytes;
    *out_queue = queue;
    return cc_result_ok();
}

/**
 * @brief 外层关闭入口并排空调用/句柄使用者后销毁，清理 FIFO 与尚存 RAM 预留。
 *
 * 销毁后所有旧句柄及队列指针结束寿命。
 * @pre 队列调用已排空，外层已停止继续使用所有预留句柄。
 * @param[in,out] queue 要销毁的队列，可为 NULL，进入前已排空全部使用者。
 * @post 所有 FIFO 项、预留、数组及 mutex 已释放，旧句柄结束寿命。
 */
void cc_memory_pending_queue_destroy(cc_memory_pending_queue_t *queue)
{
    if (!queue) return;
    for (size_t i = 0; i < queue->count; i++)
        cc_memory_pending_item_cleanup(&queue->items[(queue->head + i) % queue->capacity]);
    while (queue->reservations) {
        cc_memory_pending_reservation_t *reservation=queue->reservations;
        queue->reservations=reservation->next;
        cc_memory_pending_item_cleanup(&reservation->item); free(reservation);
    }
    free(queue->charges); free(queue->items); cc_mutex_destroy(queue->mutex); free(queue);
}

/**
 * @brief 在队列锁内查重、核算容量并深拷贝已完成输入/答案。
 *
 * FIFO 中相同 turn ID 且正文相同返回成功，内容冲突或尚有同 ID 预留报错。
 * 成功新项按尾部提交顺序排列，失败保留既有队列与调用者输入。
 * @param[in,out] queue 锁内查重、复制并追加已完成对话的目标队列。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @param[in] session_id 原输入所属会话标识，调用期间借用。
 * @param[in] user_input 原始用户输入，调用期间借用，复制或比对规则见函数说明。
 * @param[in] assistant_text 借用的完成答复，可为空字符串，接纳新项时深拷贝。
 * @return CC_OK 表示 FIFO 已接纳或当前队列中已有相同内容，参数、容量、身份或分配错误向上传递。
 */
cc_result_t cc_memory_pending_enqueue(cc_memory_pending_queue_t *queue,
                                      const char *turn_id, const char *session_id,
                                      const char *user_input, const char *assistant_text)
{
    if (!queue || !turn_id || !turn_id[0] || !session_id || !session_id[0] ||
        !user_input || !user_input[0] || !assistant_text)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pending memory turn");
    cc_memory_pending_item_t incoming = {(char *)turn_id,(char *)session_id,(char *)user_input,(char *)assistant_text,NULL};
    size_t bytes = 0;
    if (!item_bytes(&incoming, &bytes)) return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Pending memory size overflow");
    cc_mutex_lock(queue->mutex);
    for (size_t i = 0; i < queue->count; i++) {
        cc_memory_pending_item_t *item = &queue->items[(queue->head + i) % queue->capacity];
        if (strcmp(item->turn_id, turn_id) != 0) continue;
        int same = strcmp(item->session_id, session_id) == 0 && strcmp(item->user_input, user_input) == 0 &&
                   strcmp(item->assistant_text, assistant_text) == 0;
        cc_mutex_unlock(queue->mutex);
        return same ? cc_result_ok() : cc_result_error(CC_ERR_INVALID_STATE, "Pending memory turn id was reused");
    }
    for (cc_memory_pending_reservation_t *r=queue->reservations;r;r=r->next) {
        if (strcmp(r->item.turn_id,turn_id)==0) {
            cc_mutex_unlock(queue->mutex);
            return cc_result_error(CC_ERR_INVALID_STATE,"Pending memory turn is already reserved");
        }
    }
    if (queue->count + queue->reserved_count == queue->capacity || bytes > queue->max_bytes - queue->bytes) {
        cc_mutex_unlock(queue->mutex);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Pending memory queue is full");
    }
    size_t index = (queue->head + queue->count) % queue->capacity;
    cc_result_t rc = item_copy(&incoming, &queue->items[index]);
    if (rc.code == CC_OK) { queue->charges[index]=bytes; queue->count++; queue->bytes += bytes; }
    cc_mutex_unlock(queue->mutex);
    return rc;
}

/**
 * @brief 模型运行前在队列锁内预分配输入副本及 max_answer_bytes+1 答案缓冲。
 *
 * 预留同时占用项数与字节上限，并登记到独立链表。
 * 分配失败清理本次内容。
 * 成功句柄交给调用者，直到 commit/cancel 消费前需串行管理。
 * @param[in,out] queue 锁内分配预留并登记完整计费的队列。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @param[in] session_id 原输入所属会话标识，调用期间借用。
 * @param[in] user_input 原始用户输入，调用期间借用，复制或比对规则见函数说明。
 * @param[in] max_answer_bytes 答复正文的预留或扩容字节数，额外分配一个 NUL 字节。
 * @param[out] out_reservation 非 NULL 时先置空，成功取得预留句柄，调用者串行使用并通过提交或取消消费。
 * @return CC_OK 表示预留成功，参数、重复身份、容量或内存错误向上传递。
 */
cc_result_t cc_memory_pending_reserve(cc_memory_pending_queue_t *queue,
    const char *turn_id, const char *session_id, const char *user_input,
    size_t max_answer_bytes, cc_memory_pending_reservation_t **out_reservation)
{
    if (!out_reservation) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid pending memory reservation output");
    *out_reservation=NULL;
    if (!queue || !turn_id || !turn_id[0] || !session_id || !session_id[0] ||
        !user_input || !user_input[0] || max_answer_bytes==SIZE_MAX)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid pending memory reservation");
    cc_memory_pending_item_t incoming={(char *)turn_id,(char *)session_id,(char *)user_input,"",NULL};
    size_t charge=0;
    if (!item_bytes(&incoming,&charge) || charge>SIZE_MAX-max_answer_bytes)
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED,"Pending memory reservation size overflow");
    charge+=max_answer_bytes;
    /* 先检查单项预算，容量必然超限时直接返回。 */
    if (charge>queue->max_bytes) return cc_result_error(CC_ERR_LIMIT_EXCEEDED,"Pending memory queue is full");
    cc_mutex_lock(queue->mutex);
    int duplicate=0;
    for (size_t i=0;i<queue->count;i++)
        if (strcmp(queue->items[(queue->head+i)%queue->capacity].turn_id,turn_id)==0) duplicate=1;
    for (cc_memory_pending_reservation_t *r=queue->reservations;r;r=r->next)
        if (strcmp(r->item.turn_id,turn_id)==0) duplicate=1;
    if (duplicate || queue->count+queue->reserved_count==queue->capacity ||
        charge>queue->max_bytes-queue->bytes) {
        cc_mutex_unlock(queue->mutex);
        return duplicate ? cc_result_error(CC_ERR_INVALID_STATE,"Pending memory turn id already exists") :
            cc_result_error(CC_ERR_LIMIT_EXCEEDED,"Pending memory queue is full");
    }
    /* 持队列锁分配，确保并发预留的答复缓冲同时服从总预算。 */
    cc_memory_pending_reservation_t *reservation=calloc(1,sizeof(*reservation));
    if (!reservation) { cc_mutex_unlock(queue->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot allocate pending memory reservation"); }
    reservation->item.turn_id=cc_copy_string(turn_id);
    reservation->item.session_id=cc_copy_string(session_id);
    reservation->item.user_input=cc_copy_string(user_input);
    reservation->item.assistant_text=malloc(max_answer_bytes+1U);
    if (reservation->item.assistant_text) reservation->item.assistant_text[0]='\0';
    if (!reservation->item.turn_id || !reservation->item.session_id || !reservation->item.user_input ||
        !reservation->item.assistant_text) {
        cc_mutex_unlock(queue->mutex); cc_memory_pending_item_cleanup(&reservation->item); free(reservation);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot allocate pending memory turn");
    }
    reservation->max_answer_bytes=max_answer_bytes; reservation->charge=charge;
    reservation->next=queue->reservations; queue->reservations=reservation;
    queue->reserved_count++; queue->bytes+=charge;
    cc_mutex_unlock(queue->mutex);
    *out_reservation=reservation; return cc_result_ok();
}

/**
 * @brief 队列锁内按句柄身份查找链表链接槽，未找到返回指向尾部 NULL 的链接。
 *
 * 返回的内部地址只在持锁期间借用。
 * @pre 调用者持有 queue 的 mutex。
 * @param[in] queue 已持锁的队列，从其预留链表头开始查找。
 * @param[in] reservation 仍属于该队列的预留句柄。
 * @return 匹配句柄的链表链接槽，未找到时指向尾部 NULL，仅持锁期间借用。
 */
static cc_memory_pending_reservation_t **find_reservation(cc_memory_pending_queue_t *queue,
                                                           cc_memory_pending_reservation_t *reservation)
{
    cc_memory_pending_reservation_t **cursor=&queue->reservations;
    while (*cursor && *cursor!=reservation) cursor=&(*cursor)->next;
    return cursor;
}

/**
 * @brief 核对预留归属，在字节预算内 realloc 答案缓冲并同步增加计费。
 *
 * 请求容量未增大直接成功，失败保留原缓冲、容量与预留身份。
 * @param[in,out] queue 预留所属队列，在其锁内调整缓冲和计费。
 * @param[in,out] reservation 仍属于该队列的预留句柄。
 * @param[in] max_answer_bytes 答复正文的预留或扩容字节数，额外分配一个 NUL 字节。
 * @return CC_OK 表示容量已足够或扩容成功，错误时保留既有预留、容量和计费。
 */
cc_result_t cc_memory_pending_expand(cc_memory_pending_queue_t *queue,
    cc_memory_pending_reservation_t *reservation, size_t max_answer_bytes)
{
    if (!queue || !reservation || max_answer_bytes==SIZE_MAX)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid pending memory expansion");
    cc_mutex_lock(queue->mutex);
    cc_memory_pending_reservation_t **slot=find_reservation(queue,reservation);
    if (!*slot) { cc_mutex_unlock(queue->mutex); return cc_result_error(CC_ERR_INVALID_STATE,"Reservation does not belong to this queue"); }
    cc_memory_pending_reservation_t *item=*slot;
    if (max_answer_bytes<=item->max_answer_bytes) { cc_mutex_unlock(queue->mutex); return cc_result_ok(); }
    size_t extra=max_answer_bytes-item->max_answer_bytes;
    if (extra>queue->max_bytes-queue->bytes || extra>SIZE_MAX-item->charge) {
        cc_mutex_unlock(queue->mutex);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED,"Pending memory queue is full");
    }
    char *answer=realloc(item->item.assistant_text,max_answer_bytes+1U);
    if (!answer) { cc_mutex_unlock(queue->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot expand pending memory answer"); }
    item->item.assistant_text=answer; item->max_answer_bytes=max_answer_bytes;
    item->charge+=extra; queue->bytes+=extra;
    cc_mutex_unlock(queue->mutex); return cc_result_ok();
}

/**
 * @brief 锁内核对预留身份及原 turn/session/输入，参数调用期借用。
 * @param[in] queue 预留应归属的队列，锁内核对句柄和内容。
 * @param[in] reservation 仍属于该队列的预留句柄。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @param[in] session_id 原输入所属会话标识，调用期间借用。
 * @param[in] user_input 原始用户输入，调用期间借用，复制或比对规则见函数说明。
 * @return CC_OK 表示预留身份和原输入匹配，句柄无效或内容冲突返回相应错误。
 */
cc_result_t cc_memory_pending_verify(cc_memory_pending_queue_t *queue,
    cc_memory_pending_reservation_t *reservation, const char *turn_id,
    const char *session_id, const char *user_input)
{
    if (!queue || !reservation || !turn_id || !session_id || !user_input)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid pending memory verification");
    cc_mutex_lock(queue->mutex);
    cc_memory_pending_reservation_t **slot=find_reservation(queue,reservation);
    int matches=*slot && strcmp((*slot)->item.turn_id,turn_id)==0 &&
        strcmp((*slot)->item.session_id,session_id)==0 && strcmp((*slot)->item.user_input,user_input)==0;
    cc_mutex_unlock(queue->mutex);
    return matches ? cc_result_ok() : cc_result_error(CC_ERR_INVALID_STATE,"Reservation does not match the checkpoint");
}

/**
 * @brief 锁内复制仍登记的预留内容，成功拥有副本由调用者 cleanup。
 * @param[in] queue 预留所属队列，锁内复制该预留的输入。
 * @param[in] reservation 仍属于该队列的预留句柄。
 * @param[out] out_item 无待释放资源的输出结构，参数合法后先清零，成功拥有原输入副本，答案为空字符串。
 * @return CC_OK 表示输出拥有副本，参数、句柄或分配错误向上传递。
 */
cc_result_t cc_memory_pending_inspect_reservation(cc_memory_pending_queue_t *queue,
    cc_memory_pending_reservation_t *reservation, cc_memory_pending_item_t *out_item)
{
    if (!queue || !reservation || !out_item)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid pending memory inspection");
    memset(out_item,0,sizeof(*out_item));
    cc_mutex_lock(queue->mutex);
    cc_memory_pending_reservation_t **slot=find_reservation(queue,reservation);
    cc_result_t rc=*slot ? item_copy(&(*slot)->item,out_item) :
        cc_result_error(CC_ERR_INVALID_STATE,"Reservation does not belong to this queue");
    cc_mutex_unlock(queue->mutex); return rc;
}

/**
 * @brief 答案放入既有缓冲后，把项的拥有字段从预留移到 FIFO 尾部，无新增分配。
 *
 * 成功清空调用者句柄并释放预留壳，计费仍保留原容量，ack 时统一归还。
 * 超出答案容量或归属/计费错误时保留预留，输入答案为调用期间借用的字符串。
 * @param[in,out] queue 预留所属队列，锁内接收项字段并将预留转为 FIFO 项。
 * @param[in,out] reservation 仍属于该队列的预留句柄，成功消费后置空。
 * @param[in] assistant_text 借用的完成答复，可为空字符串，复制到已有答案缓冲。
 * @return CC_OK 表示句柄已消费并转入 FIFO，答复超长或句柄无效时返回错误并保留原预留。
 */
cc_result_t cc_memory_pending_commit(cc_memory_pending_queue_t *queue,
    cc_memory_pending_reservation_t **reservation, const char *assistant_text)
{
    if (!queue || !reservation || !*reservation || !assistant_text)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid pending memory commit");
    size_t length=strlen(assistant_text);
    cc_mutex_lock(queue->mutex);
    cc_memory_pending_reservation_t **slot=find_reservation(queue,*reservation);
    if (!*slot) { cc_mutex_unlock(queue->mutex); return cc_result_error(CC_ERR_INVALID_STATE,"Reservation does not belong to this queue"); }
    cc_memory_pending_reservation_t *item=*slot;
    if (length>item->max_answer_bytes) {
        cc_mutex_unlock(queue->mutex);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED,"Answer exceeds the reserved memory capacity");
    }
    if (queue->count>=queue->capacity || !queue->reserved_count || item->charge>queue->bytes) {
        cc_mutex_unlock(queue->mutex);
        return cc_result_error(CC_ERR_INVALID_STATE,"Pending memory reservation accounting is invalid");
    }
    memcpy(item->item.assistant_text,assistant_text,length+1U);
    size_t index=(queue->head+queue->count)%queue->capacity;
    queue->items[index]=item->item; memset(&item->item,0,sizeof(item->item));
    queue->charges[index]=item->charge; queue->count++; queue->reserved_count--;
    *slot=item->next; *reservation=NULL;
    cc_mutex_unlock(queue->mutex); free(item); return cc_result_ok();
}

/**
 * @brief 取消仍登记的预留，锁内摘链并归还计费，锁外清理拥有字符串及壳对象。
 *
 * 成功清空句柄，已消费的空句柄再次调用按参数错误处理。
 * @param[in,out] queue 预留所属队列，锁内摘链并归还容量。
 * @param[in,out] reservation 仍属于该队列的预留句柄，成功消费后置空。
 * @return CC_OK 表示预留已释放且句柄置空，句柄无效时返回错误。
 */
cc_result_t cc_memory_pending_cancel(cc_memory_pending_queue_t *queue,
    cc_memory_pending_reservation_t **reservation)
{
    if (!queue || !reservation || !*reservation)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid pending memory cancellation");
    cc_mutex_lock(queue->mutex);
    cc_memory_pending_reservation_t **slot=find_reservation(queue,*reservation);
    if (!*slot) { cc_mutex_unlock(queue->mutex); return cc_result_error(CC_ERR_INVALID_STATE,"Reservation does not belong to this queue"); }
    cc_memory_pending_reservation_t *item=*slot;
    if (!queue->reserved_count || item->charge>queue->bytes) {
        cc_mutex_unlock(queue->mutex);
        return cc_result_error(CC_ERR_INVALID_STATE,"Pending memory reservation accounting is invalid");
    }
    *slot=item->next; queue->reserved_count--; queue->bytes-=item->charge; *reservation=NULL;
    cc_mutex_unlock(queue->mutex); cc_memory_pending_item_cleanup(&item->item); free(item); return cc_result_ok();
}

/**
 * @brief 锁内深拷贝 FIFO 队首，成功副本由调用者 cleanup，空 FIFO 返回 NOT_FOUND。
 *
 * 副本独立于队列寿命，其瞬时分配另计峰值 RAM。
 * @param[in] queue 待读取队首的队列，锁内复制全部字段。
 * @param[out] out_item 无待释放资源的输出结构，参数合法后先清零，成功副本由调用者 cleanup。
 * @return CC_OK 表示返回拥有副本，空队列为 CC_ERR_NOT_FOUND，参数或复制错误向上传递。
 */
cc_result_t cc_memory_pending_peek(cc_memory_pending_queue_t *queue, cc_memory_pending_item_t *out_item)
{
    if (!queue || !out_item) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pending memory peek");
    memset(out_item, 0, sizeof(*out_item));
    cc_mutex_lock(queue->mutex);
    cc_result_t rc = queue->count ? item_copy(&queue->items[queue->head], out_item) :
        cc_result_error(CC_ERR_NOT_FOUND, "No pending memory turns");
    cc_mutex_unlock(queue->mutex);
    return rc;
}

/**
 * @brief 先复制借用 snapshot ID，再调用 take 关联到已提交项，归还未转移的副本。
 *
 * 同项同 ID 可重复确认，实际 ID 长度在 take 中检查。
 * @param[in,out] queue 通过 take_snapshot 接收快照副本的队列。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @param[in] snapshot_id 要复制关联的快照 ID，调用期间借用。
 * @return CC_OK 表示相同关联已存在或新关联成功，ID 冲突、超长或分配错误向上传递。
 */
cc_result_t cc_memory_pending_set_snapshot(cc_memory_pending_queue_t *queue,
                                            const char *turn_id, const char *snapshot_id)
{
    if (!queue || !turn_id || !turn_id[0] || !snapshot_id || !snapshot_id[0])
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pending snapshot");
    char *copy = cc_copy_string(snapshot_id);
    if (!copy) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy pending snapshot id");
    cc_result_t rc = cc_memory_pending_take_snapshot(queue, turn_id, &copy);
    wipe_free(copy);
    return rc;
}

/**
 * @brief 锁内给指定 FIFO 项关联拥有 ID，新关联成功时移交指针并清空输入槽。
 *
 * 已有关联且正文相同时也返回成功，此时输入仍归调用者清理。
 * 不同 ID 报错。
 * 所有失败保留输入拥有关系，长度含 NUL 最多 64 字节。
 * @param[in,out] queue 锁内查找目标已提交项并接管新 ID 的队列。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @param[in,out] snapshot_id 指向独立分配、可由 free 释放的非空 ID。新关联时置 NULL，已有相同 ID 时保留原指针。
 * @return 新关联或同 ID 为 CC_OK，目标缺失为 CC_ERR_NOT_FOUND，冲突为 CC_ERR_INVALID_STATE，超长为 CC_ERR_LIMIT_EXCEEDED，非法参数为 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t cc_memory_pending_take_snapshot(cc_memory_pending_queue_t *queue,
                                             const char *turn_id, char **snapshot_id)
{
    if (!queue || !turn_id || !turn_id[0] || !snapshot_id || !*snapshot_id || !(*snapshot_id)[0])
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pending snapshot");
    cc_mutex_lock(queue->mutex);
    for (size_t i = 0; i < queue->count; i++) {
        cc_memory_pending_item_t *item = &queue->items[(queue->head + i) % queue->capacity];
        if (strcmp(item->turn_id, turn_id) != 0) continue;
        if (item->snapshot_id) {
            int same = strcmp(item->snapshot_id, *snapshot_id) == 0;
            cc_mutex_unlock(queue->mutex);
            return same ? cc_result_ok() : cc_result_error(CC_ERR_INVALID_STATE, "Pending turn already has a different snapshot");
        }
        size_t bytes = strlen(*snapshot_id) + 1U;
        if (!bytes || bytes > PENDING_SNAPSHOT_RESERVE) {
            cc_mutex_unlock(queue->mutex);
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Pending memory queue is full");
        }
        item->snapshot_id = *snapshot_id;
        *snapshot_id = NULL;
        cc_mutex_unlock(queue->mutex);
        return cc_result_ok();
    }
    cc_mutex_unlock(queue->mutex);
    return cc_result_error(CC_ERR_NOT_FOUND, "Pending memory turn not found");
}

/**
 * @brief 仅当当前关联仍等于期望 ID 时解除并释放该字符串，历史文件由快照存储管理。
 * @param[in,out] queue 锁内核对期望 ID 并解除关联的队列。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @param[in] expected_snapshot_id 调用者预期解除的当前快照 ID，匹配后才清关联。
 * @return CC_OK 表示按预期解除关联，目标缺失或关联冲突返回相应错误。
 */
cc_result_t cc_memory_pending_clear_snapshot(cc_memory_pending_queue_t *queue,
                                              const char *turn_id, const char *expected_snapshot_id)
{
    if (!queue || !turn_id || !turn_id[0] || !expected_snapshot_id || !expected_snapshot_id[0])
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pending snapshot clear");
    cc_mutex_lock(queue->mutex);
    for (size_t i = 0; i < queue->count; i++) {
        cc_memory_pending_item_t *item = &queue->items[(queue->head + i) % queue->capacity];
        if (strcmp(item->turn_id, turn_id) != 0) continue;
        if (!item->snapshot_id || strcmp(item->snapshot_id, expected_snapshot_id) != 0) {
            cc_mutex_unlock(queue->mutex);
            return cc_result_error(CC_ERR_INVALID_STATE, "Pending snapshot changed");
        }
        wipe_free(item->snapshot_id); item->snapshot_id = NULL;
        cc_mutex_unlock(queue->mutex);
        return cc_result_ok();
    }
    cc_mutex_unlock(queue->mutex);
    return cc_result_error(CC_ERR_NOT_FOUND, "Pending memory turn not found");
}

/**
 * @brief 只确认匹配 ID 的当前 FIFO 队首，清理字符串并归还其完整计费容量。
 *
 * 乱序或计费异常返回错误，已完成写入与核对由调用者在 ack 前保证。
 * @param[in,out] queue 锁内清理匹配队首并归还完整计费的队列。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @return CC_OK 表示匹配队首已移除并归还计费，空队列或 ID 不匹配返回错误。
 */
cc_result_t cc_memory_pending_ack(cc_memory_pending_queue_t *queue, const char *turn_id)
{
    if (!queue || !turn_id || !turn_id[0]) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pending memory acknowledgement");
    cc_mutex_lock(queue->mutex);
    if (!queue->count || strcmp(queue->items[queue->head].turn_id, turn_id) != 0) {
        cc_mutex_unlock(queue->mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Pending memory acknowledgement is out of order");
    }
    size_t bytes = queue->charges[queue->head];
    if (!bytes || bytes > queue->bytes) {
        cc_mutex_unlock(queue->mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Pending memory queue accounting is invalid");
    }
    cc_memory_pending_item_cleanup(&queue->items[queue->head]);
    queue->charges[queue->head]=0;
    queue->bytes -= bytes; queue->count--; queue->head = (queue->head + 1U) % queue->capacity;
    cc_mutex_unlock(queue->mutex);
    return cc_result_ok();
}

/**
 * @brief 锁内取得 FIFO 与预留的总项数及计费字节，queue 为 NULL 时输出零。
 * @param[in] queue 待查询队列，可为 NULL。
 * @param[out] out_count 可选输出，FIFO 项与预留合计数量。
 * @param[out] out_bytes 可选输出，包含完整答复预留的计费字节数。
 * @post 非 NULL 输出已填写同一次队列锁下的快照，queue 为 NULL 时填写零。
 */
void cc_memory_pending_status(cc_memory_pending_queue_t *queue, size_t *out_count, size_t *out_bytes)
{
    if (!queue) { if (out_count) *out_count = 0; if (out_bytes) *out_bytes = 0; return; }
    cc_mutex_lock(queue->mutex);
    if (out_count) *out_count = queue->count + queue->reserved_count;
    if (out_bytes) *out_bytes = queue->bytes;
    cc_mutex_unlock(queue->mutex);
}

/**
 * @brief 同一队列锁下取得 FIFO 项、预留项与字节快照，输出指针可以分别省略。
 * @param[in] queue 待查询队列，可为 NULL。
 * @param[out] out_ready 可选输出，FIFO 中已提交项数。
 * @param[out] out_reserved 可选输出，尚未提交的预留数。
 * @param[out] out_bytes 可选输出，包含完整答复预留的计费字节数。
 * @post 非 NULL 输出已填写同一次队列锁下的快照，queue 为 NULL 时填写零。
 */
void cc_memory_pending_status_detail(cc_memory_pending_queue_t *queue,
    size_t *out_ready, size_t *out_reserved, size_t *out_bytes)
{
    if (!queue) { if (out_ready) *out_ready=0; if (out_reserved) *out_reserved=0; if (out_bytes) *out_bytes=0; return; }
    cc_mutex_lock(queue->mutex);
    if (out_ready) *out_ready=queue->count;
    if (out_reserved) *out_reserved=queue->reserved_count;
    if (out_bytes) *out_bytes=queue->bytes;
    cc_mutex_unlock(queue->mutex);
}
