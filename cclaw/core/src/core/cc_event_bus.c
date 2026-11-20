#include "cc/ports/cc_event_bus.h"

#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_platform.h"
#include "cc/ports/cc_thread.h"
#include "cc/util/cc_redaction.h"

#include <stdlib.h>
#include <string.h>

#define CC_EVENT_MAX_SUBSCRIBERS 64U
#define CC_EVENT_TOPIC_CAPACITY 128U
#define CC_EVENT_MAILBOX_MAX 16U

#if CC_PLATFORM == CC_PLATFORM_ESP32 || CC_PLATFORM == CC_PLATFORM_FREERTOS
#define DEFAULT_ASYNC_WORKERS 1U
#define DEFAULT_MAILBOX_CAPACITY 8U
#else
#define DEFAULT_ASYNC_WORKERS 4U
#define DEFAULT_MAILBOX_CAPACITY 16U
#endif

/** @brief 独立拥有主题和脱敏正文的信封，发布者及邮箱引用在总线锁内协调。 */
typedef struct cc_event_envelope {
    char *event_type;
    char *event_json;
    size_t refs;
    uint64_t published_ms;
} cc_event_envelope_t;

/**
 * @brief 固定订阅槽，主题副本归总线拥有，handler 和 user_data 借用至全部已接纳回调退出。
 * 锁内维护邮箱、ready/busy、inflight 及代际；公开 generation 为十六位，长调用使用内部宽 epoch。
 */
typedef struct cc_event_subscriber_slot {
    int active;
    uint16_t generation;
    uint64_t epoch;
    char *event_type;
    cc_event_handler_fn handler;
    void *user_data;
    cc_event_overflow_policy_t overflow_policy;
    size_t mailbox_capacity;
    int block_timeout_ms;
    cc_event_envelope_t *mailbox[CC_EVENT_MAILBOX_MAX];
    size_t mailbox_head;
    size_t mailbox_count;
    size_t inflight;
    int ready;
    int busy;
} cc_event_subscriber_slot_t;

typedef struct cc_event_topic_entry {
    /* 零为未使用，一为有效目录项，二为保留线性探测链的墓碑。 */
    int used;
    uint64_t hash;
    char *topic;
    uint64_t subscribers;
} cc_event_topic_entry_t;

/** @brief 同步发布预留的回调快照，inflight 保证其借用上下文持续到本次调用结束。 */
typedef struct cc_event_sync_snapshot {
    size_t slot;
    uint16_t generation;
    cc_event_handler_fn handler;
    void *user_data;
} cc_event_sync_snapshot_t;

/**
 * @brief 总线拥有的固定槽、主题索引、ready 环、worker 和同步资源。
 * mutex 保护共享状态，workers_joining 将实际 join 的句柄操作交给唯一 shutdown 调用。
 * publishers 覆盖锁外脱敏及分发，lifecycle_users 覆盖已登记的退订、flush 和 shutdown 等待。
 */
struct cc_event_bus {
    cc_event_subscriber_slot_t subscribers[CC_EVENT_MAX_SUBSCRIBERS];
    cc_event_topic_entry_t topics[CC_EVENT_TOPIC_CAPACITY];
    uint64_t wildcard_subscribers;

    size_t ready_ring[CC_EVENT_MAX_SUBSCRIBERS];
    size_t ready_head;
    size_t ready_count;

    cc_event_bus_mode_t mode;
    size_t default_mailbox_capacity;
    cc_thread_t *workers;
    size_t worker_count;
    int workers_joined;
    int workers_joining;
    int shutting_down;
    size_t publishers;
    size_t lifecycle_users;

    cc_event_bus_metrics_t metrics;
    cc_mutex_t mutex;
    cc_cond_t cond;
};

/**
 * @brief 逐字节计算主题的固定散列，用于目录线性探测。
 * @param[in] text 同步借用的 NUL 结尾主题，NULL 按空串处理。
 * @return 无分配的 uint64_t 散列值，相等主题仍由 strcmp 确认。
 */
static uint64_t topic_hash(const char *text)
{
    uint64_t hash = 1469598103934665603ULL;
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    while (*p) {
        hash ^= (uint64_t)*p++;
        hash *= 1099511628211ULL;
    }
    return hash;
}

/**
 * @brief 将零 worker 数归一到当前平台默认值。
 * @param[in] value 创建入口已检查上界的数量，单位线程。
 * @return 正值原样返回，零在 MCU 平台返回一，其余平台返回四。
 */
static size_t normalized_worker_count(size_t value)
{
    return value ? value : DEFAULT_ASYNC_WORKERS;
}

/**
 * @brief 将邮箱容量归一到平台默认值和固定十六项上界。
 * @param[in] value 请求条数，零采用平台默认值。
 * @return 一至十六项，当前平台默认值为八或十六。
 */
static size_t normalized_mailbox_capacity(size_t value)
{
    if (value == 0) value = DEFAULT_MAILBOX_CAPACITY;
    return value > CC_EVENT_MAILBOX_MAX ? CC_EVENT_MAILBOX_MAX : value;
}

/**
 * @brief 释放信封及其独立字符串。
 * @pre 非空信封由当前路径独占，或其引用已在总线锁内归零。
 * @param[in] envelope 消费拥有的信封，允许 NULL。
 * @post 信封和字符串均失效。
 */
static void envelope_destroy(cc_event_envelope_t *envelope)
{
    if (!envelope) return;
    free(envelope->event_type);
    free(envelope->event_json);
    free(envelope);
}

/**
 * @brief 归还发布者或邮箱的一份信封引用，归零时销毁。
 * @pre 调用者持所属总线 mutex，并保管一份待归还引用。
 * @param[in] envelope 待消费的引用，允许 NULL。
 * @post 本次引用结束，其他引用保持有效。
 */
static void envelope_release_locked(cc_event_envelope_t *envelope)
{
    if (!envelope) return;
    if (envelope->refs > 0) envelope->refs--;
    if (envelope->refs == 0) envelope_destroy(envelope);
}

/**
 * @brief 深拷贝主题及脱敏正文，并记录单调毫秒创建时刻。
 * @param[in] event_type 调用期间借用的主题，NULL 按空串复制。
 * @param[in] event_json 调用期间借用的已脱敏正文，NULL 按空串复制。
 * @return 成功交付拥有初始引用一的信封，任一分配失败清理本次资源并返回 NULL。
 */
static cc_event_envelope_t *envelope_create(const char *event_type, const char *event_json)
{
    cc_event_envelope_t *envelope = calloc(1, sizeof(*envelope));
    if (!envelope) return NULL;
    envelope->event_type = cc_copy_string(event_type ? event_type : "");
    envelope->event_json = cc_copy_string(event_json ? event_json : "");
    if (!envelope->event_type || !envelope->event_json) {
        envelope_destroy(envelope);
        return NULL;
    }
    envelope->refs = 1;
    envelope->published_ms = cc_platform_monotonic_ms();
    return envelope;
}

/**
 * @brief 在线性探测目录查找主题，可复用墓碑建立拥有的主题副本。
 * @pre 调用者持 bus->mutex。
 * @param[in,out] bus 非空总线，查询累计碰撞数，创建成功增加 active_topics。
 * @param[in] topic 必填的同步借用主题，允许空串。
 * @param[in] create 非零时允许建立缺失项，零只查找。
 * @param[out] out_allocation_failed 可选，进入时置零，复制主题失败时置一。
 * @return 借用目录项，寿命由当前锁和后续订阅持有关系约束；缺失、目录满或分配失败返回 NULL。
 */
static cc_event_topic_entry_t *topic_lookup_locked(
    cc_event_bus_t *bus,
    const char *topic,
    int create,
    int *out_allocation_failed)
{
    if (out_allocation_failed) *out_allocation_failed = 0;
    uint64_t hash = topic_hash(topic);
    size_t start = (size_t)(hash % CC_EVENT_TOPIC_CAPACITY);
    cc_event_topic_entry_t *first_tombstone = NULL;
    for (size_t probe = 0; probe < CC_EVENT_TOPIC_CAPACITY; probe++) {
        cc_event_topic_entry_t *entry = &bus->topics[(start + probe) % CC_EVENT_TOPIC_CAPACITY];
        if (entry->used == 0) {
            if (!create) return NULL;
            cc_event_topic_entry_t *destination = first_tombstone ? first_tombstone : entry;
            destination->topic = cc_copy_string(topic);
            if (!destination->topic) {
                if (out_allocation_failed) *out_allocation_failed = 1;
                return NULL;
            }
            destination->used = 1;
            destination->hash = hash;
            destination->subscribers = 0;
            bus->metrics.active_topics++;
            return destination;
        }
        if (entry->used == 2) {
            if (!first_tombstone) first_tombstone = entry;
            continue;
        }
        if (entry->hash == hash && strcmp(entry->topic, topic) == 0) return entry;
        bus->metrics.hash_collisions++;
    }
    if (create && first_tombstone) {
        first_tombstone->topic = cc_copy_string(topic);
        if (!first_tombstone->topic) {
            if (out_allocation_failed) *out_allocation_failed = 1;
            return NULL;
        }
        first_tombstone->used = 1;
        first_tombstone->hash = hash;
        first_tombstone->subscribers = 0;
        bus->metrics.active_topics++;
        return first_tombstone;
    }
    return NULL;
}

/**
 * @brief 将有排队事件且未 busy/ready 的活动槽加入 ready 环，并唤醒一个 worker。
 * @pre 调用者持 bus->mutex，索引位于固定槽数组内。
 * @param[in,out] bus 非空总线，保管 ready 环和条件变量。
 * @param[in] slot_index 待调度槽索引。
 * @post 满足条件时槽至多占一个 ready 项，其余情况保持现状。
 */
static void ready_push_locked(cc_event_bus_t *bus, size_t slot_index)
{
    cc_event_subscriber_slot_t *slot = &bus->subscribers[slot_index];
    if (slot->ready || slot->busy || !slot->active || slot->mailbox_count == 0) return;
    if (bus->ready_count >= CC_EVENT_MAX_SUBSCRIBERS) return;
    size_t tail = (bus->ready_head + bus->ready_count) % CC_EVENT_MAX_SUBSCRIBERS;
    bus->ready_ring[tail] = slot_index;
    bus->ready_count++;
    slot->ready = 1;
    cc_cond_signal(bus->cond);
}

/**
 * @brief 依次消费 ready 项并跳过已退订、busy 或空邮箱槽。
 * @pre 调用者持 bus->mutex。
 * @param[in,out] bus 非空总线，消费环头并清除被取槽的 ready 标志。
 * @param[out] out_slot 必填，找到可调度项时写槽索引，未找到时保持原值。
 * @return 找到为一，环耗尽为零。
 */
static int ready_pop_locked(cc_event_bus_t *bus, size_t *out_slot)
{
    while (bus->ready_count > 0) {
        size_t slot_index = bus->ready_ring[bus->ready_head];
        bus->ready_head = (bus->ready_head + 1) % CC_EVENT_MAX_SUBSCRIBERS;
        bus->ready_count--;
        cc_event_subscriber_slot_t *slot = &bus->subscribers[slot_index];
        slot->ready = 0;
        if (slot->active && !slot->busy && slot->mailbox_count > 0) {
            *out_slot = slot_index;
            return 1;
        }
    }
    return 0;
}

/**
 * @brief 退订时压紧 ready 环，移除目标槽的全部排队索引。
 * @pre 调用者持 bus->mutex，slot_index 有效。
 * @param[in,out] bus 非空总线，其他 ready 项保持相对顺序。
 * @param[in] slot_index 待移除槽索引。
 * @post 目标 ready 标志清零，环头归零并保留其余项。
 */
static void ready_remove_slot_locked(cc_event_bus_t *bus, size_t slot_index)
{
    size_t kept[CC_EVENT_MAX_SUBSCRIBERS];
    size_t kept_count = 0;
    while (bus->ready_count > 0) {
        size_t index = bus->ready_ring[bus->ready_head];
        bus->ready_head = (bus->ready_head + 1) % CC_EVENT_MAX_SUBSCRIBERS;
        bus->ready_count--;
        if (index != slot_index) kept[kept_count++] = index;
    }
    bus->ready_head = 0;
    for (size_t i = 0; i < kept_count; i++) bus->ready_ring[i] = kept[i];
    bus->ready_count = kept_count;
    bus->subscribers[slot_index].ready = 0;
}

/**
 * @brief 在退订且全部已接纳回调退出后自动归还槽位。
 * @pre 调用者持总线 mutex，退订已丢弃该槽的邮箱和 ready 项。
 * @param[in,out] slot 待检查槽，允许 NULL，活动或仍有 inflight 时保持原状。
 * @post 可回收时释放主题、清除借用 handler/user_data，公开 generation 与内部 epoch 递增并跳过零。
 */
static void subscriber_slot_reclaim_locked(cc_event_subscriber_slot_t *slot)
{
    if (!slot || slot->active || slot->inflight != 0 ||
        (!slot->event_type && !slot->handler && !slot->user_data)) {
        return;
    }
    free(slot->event_type);
    slot->event_type = NULL;
    slot->handler = NULL;
    slot->user_data = NULL;
    slot->generation++;
    if (slot->generation == 0) slot->generation = 1;
    slot->epoch++;
    if (slot->epoch == 0) slot->epoch = 1;
}

/**
 * @brief 从邮箱头取出一份已有信封引用。
 * @pre 调用者持所属总线 mutex。
 * @param[in,out] slot 借用槽，允许 NULL，成功时清空头指针并递减计数。
 * @return 交付原邮箱拥有的信封引用，空槽或空邮箱返回 NULL，调用者最终 release。
 */
static cc_event_envelope_t *mailbox_pop_locked(cc_event_subscriber_slot_t *slot)
{
    if (!slot || slot->mailbox_count == 0) return NULL;
    cc_event_envelope_t *envelope = slot->mailbox[slot->mailbox_head];
    slot->mailbox[slot->mailbox_head] = NULL;
    slot->mailbox_head = (slot->mailbox_head + 1) % CC_EVENT_MAILBOX_MAX;
    slot->mailbox_count--;
    return envelope;
}

/**
 * @brief 向邮箱尾追加信封并取得一份引用。
 * @pre 调用者持总线 mutex，槽和信封非 NULL，邮箱未达本槽容量且不超过固定数组。
 * @param[in,out] slot 借用目标槽，追加后计数增加。
 * @param[in,out] envelope 借用的存活信封，refs 增加，原拥有者继续保管其原引用。
 * @post 邮箱保管新增引用，后续 pop/退订负责归还。
 */
static void mailbox_push_locked(
    cc_event_subscriber_slot_t *slot,
    cc_event_envelope_t *envelope)
{
    size_t tail = (slot->mailbox_head + slot->mailbox_count) % CC_EVENT_MAILBOX_MAX;
    slot->mailbox[tail] = envelope;
    slot->mailbox_count++;
    envelope->refs++;
}

/**
 * @brief 在指定内部代际的邮箱尝试投递，并执行满邮箱策略。
 * @pre 调用者持 bus->mutex 并保管发布者信封引用，条件等待期间允许其他 API 改变槽位。
 * @param[in,out] bus 非空总线，关闭后拒绝新入队，锁内更新投递与背压指标。
 * @param[in] slot_index 路由快照中的有效槽索引。
 * @param[in] expected_epoch 同次路由快照的宽代际，槽复用后拒绝沿旧快照入队。
 * @param[in,out] envelope 本次发布借用的非空信封，实际追加或替换时取得邮箱引用。
 * @return 入队、丢新或合并为 CC_OK，订阅改变/关闭为 CANCELLED，满邮箱等待到期为 TIMEOUT，worker 自依赖为 QUEUE_FULL。
 */
static cc_result_t enqueue_for_subscriber_locked(
    cc_event_bus_t *bus,
    size_t slot_index,
    uint64_t expected_epoch,
    cc_event_envelope_t *envelope)
{
    cc_event_subscriber_slot_t *slot = &bus->subscribers[slot_index];
    if (bus->shutting_down) {
        return cc_result_error(CC_ERR_CANCELLED, "Event bus is shutting down");
    }
    uint64_t wait_start = cc_platform_monotonic_ms();
    while (slot->active && slot->epoch == expected_epoch &&
           slot->mailbox_count >= slot->mailbox_capacity &&
           slot->overflow_policy == CC_EVENT_BLOCK_WITH_TIMEOUT) {
        size_t busy_workers = 0;
        for (size_t i = 0; i < CC_EVENT_MAX_SUBSCRIBERS; i++) {
            if (bus->subscribers[i].busy) busy_workers++;
        }
        int current_is_worker = 0;
        for (size_t i = 0; i < bus->worker_count; i++) {
            if (cc_thread_is_current(bus->workers[i])) {
                current_is_worker = 1;
                break;
            }
        }
        /* 外部发布者可等待 worker 腾出空间；worker 向 busy 邮箱或全部 worker 忙时发布，直接拒绝自依赖等待。 */
        if (current_is_worker && (slot->busy || busy_workers >= bus->worker_count)) {
            bus->metrics.dropped_newest++;
            return cc_result_error(CC_ERR_QUEUE_FULL,
                                   "Event workers are busy; publish was rejected to avoid deadlock");
        }
        int timeout = slot->block_timeout_ms > 0 ? slot->block_timeout_ms : 1;
        uint64_t elapsed = cc_platform_monotonic_ms() - wait_start;
        if (elapsed >= (uint64_t)timeout) {
            bus->metrics.block_time_ms += elapsed;
            return cc_result_error(CC_ERR_TIMEOUT, "Event subscriber mailbox wait timed out");
        }
        int remaining_ms = timeout - (int)elapsed;
        (void)cc_cond_timedwait(bus->cond, bus->mutex, remaining_ms);
        elapsed = cc_platform_monotonic_ms() - wait_start;
        if (elapsed >= (uint64_t)timeout) {
            bus->metrics.block_time_ms += elapsed;
            return cc_result_error(CC_ERR_TIMEOUT, "Event subscriber mailbox wait timed out");
        }
        if (bus->shutting_down) return cc_result_error(CC_ERR_CANCELLED, "Event bus is shutting down");
    }
    bus->metrics.block_time_ms += cc_platform_monotonic_ms() - wait_start;
    if (bus->shutting_down) {
        return cc_result_error(CC_ERR_CANCELLED, "Event bus is shutting down");
    }
    if (!slot->active || slot->epoch != expected_epoch) {
        return cc_result_error(CC_ERR_CANCELLED, "Event subscription changed while publishing");
    }

    if (slot->mailbox_count >= slot->mailbox_capacity) {
        if (slot->overflow_policy == CC_EVENT_DROP_NEWEST) {
            bus->metrics.dropped_newest++;
            return cc_result_ok();
        }
        if (slot->overflow_policy == CC_EVENT_DROP_OLDEST) {
            envelope_release_locked(mailbox_pop_locked(slot));
            bus->metrics.dropped_oldest++;
        } else if (slot->overflow_policy == CC_EVENT_COALESCE_LATEST) {
            size_t latest = (slot->mailbox_head + slot->mailbox_count - 1) % CC_EVENT_MAILBOX_MAX;
            envelope_release_locked(slot->mailbox[latest]);
            slot->mailbox[latest] = envelope;
            envelope->refs++;
            bus->metrics.coalesced++;
            return cc_result_ok();
        }
    }

    mailbox_push_locked(slot, envelope);
    if (slot->mailbox_count > bus->metrics.mailbox_high_water) {
        bus->metrics.mailbox_high_water = slot->mailbox_count;
    }
    ready_push_locked(bus, slot_index);
    return cc_result_ok();
}

/**
 * @brief 按 ready 环串行处理各槽邮箱，在锁外执行回调并在返回后归还引用。
 * @param[in,out] arg 创建时借用的非空总线，覆盖线程 join 返回。
 * @return 关闭且当前无 ready 项时返回 NULL。
 * @note 同槽 busy 阻止其他 worker 同时调用该订阅，回调退出会自动回收已退订槽并广播等待者。
 */
static void *event_worker_main(void *arg)
{
    cc_event_bus_t *bus = (cc_event_bus_t *)arg;
    for (;;) {
        cc_mutex_lock(bus->mutex);
        size_t slot_index = 0;
        while (!ready_pop_locked(bus, &slot_index)) {
            if (bus->shutting_down && bus->ready_count == 0) {
                cc_mutex_unlock(bus->mutex);
                return NULL;
            }
            cc_cond_wait(bus->cond, bus->mutex);
        }
        cc_event_subscriber_slot_t *slot = &bus->subscribers[slot_index];
        cc_event_envelope_t *envelope = mailbox_pop_locked(slot);
        cc_event_handler_fn handler = slot->handler;
        void *user_data = slot->user_data;
        slot->busy = 1;
        slot->inflight++;
        cc_cond_broadcast(bus->cond);
        cc_mutex_unlock(bus->mutex);

        if (envelope && handler) handler(envelope->event_type, envelope->event_json, user_data);

        cc_mutex_lock(bus->mutex);
        slot->busy = 0;
        if (slot->inflight > 0) slot->inflight--;
        subscriber_slot_reclaim_locked(slot);
        bus->metrics.delivered++;
        if (envelope) {
            uint64_t latency = cc_platform_monotonic_ms() - envelope->published_ms;
            bus->metrics.delivery_latency_sum_ms += latency;
            if (latency > bus->metrics.delivery_latency_max_ms) {
                bus->metrics.delivery_latency_max_ms = latency;
            }
        }
        envelope_release_locked(envelope);
        ready_push_locked(bus, slot_index);
        cc_cond_broadcast(bus->cond);
        cc_mutex_unlock(bus->mutex);
    }
}

/**
 * @brief 构造默认同步配置，worker 和邮箱零值留给创建入口归一。
 * @return 按值返回完整 size、SYNC 模式及其余零字段，无动态资源。
 */
cc_event_bus_config_t cc_event_bus_default_config(void)
{
    cc_event_bus_config_t config = {0};
    config.size = sizeof(config);
    config.mode = CC_EVENT_BUS_MODE_SYNC;
    return config;
}

/**
 * @brief 用 NULL 配置委托创建默认同步总线。
 * @param[out] out_bus 必填，带配置入口先置 NULL，成功移交拥有句柄。
 * @return 原样返回创建的参数、分配或平台结果。
 */
cc_result_t cc_event_bus_create(cc_event_bus_t **out_bus)
{
    return cc_event_bus_create_with_config(NULL, out_bus);
}

/**
 * @brief 创建固定槽、主题目录和同步资源，异步模式另创建归一数量的 worker。
 * @param[in] config 可选的完整配置，当前按完整结构读取，NULL 使用同步默认配置。
 * @param[out] out_bus 必填，存在时先置 NULL，成功由 owner destroy，失败清理本次创建资源。
 * @return 成功为 CC_OK，模式或 worker 上界非法为 INVALID_ARGUMENT，其余分配、mutex/cond/thread 错误向上传递。
 */
cc_result_t cc_event_bus_create_with_config(
    const cc_event_bus_config_t *config,
    cc_event_bus_t **out_bus)
{
    if (!out_bus) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null event bus output");
    *out_bus = NULL;
    cc_event_bus_config_t defaults = cc_event_bus_default_config();
    const cc_event_bus_config_t *effective = config ? config : &defaults;
    if (effective->mode != CC_EVENT_BUS_MODE_SYNC && effective->mode != CC_EVENT_BUS_MODE_ASYNC) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid event bus mode");
    }
    if (effective->worker_count > CC_EVENT_MAX_SUBSCRIBERS) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Event bus worker count exceeds fixed capacity");
    }
    cc_event_bus_t *bus = calloc(1, sizeof(*bus));
    if (!bus) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate event bus");
    bus->mode = effective->mode;
    bus->default_mailbox_capacity = normalized_mailbox_capacity(effective->mailbox_capacity);
    bus->metrics.size = sizeof(bus->metrics);
    for (size_t i = 0; i < CC_EVENT_MAX_SUBSCRIBERS; i++) {
        bus->subscribers[i].generation = 1;
        bus->subscribers[i].epoch = 1;
    }

    cc_result_t rc = cc_mutex_create(&bus->mutex);
    if (rc.code != CC_OK) { free(bus); return rc; }
    rc = cc_cond_create(&bus->cond);
    if (rc.code != CC_OK) {
        cc_mutex_destroy(bus->mutex);
        free(bus);
        return rc;
    }
    if (bus->mode == CC_EVENT_BUS_MODE_ASYNC) {
        bus->worker_count = normalized_worker_count(effective->worker_count);
        bus->workers = calloc(bus->worker_count, sizeof(*bus->workers));
        if (!bus->workers) {
            cc_cond_destroy(bus->cond);
            cc_mutex_destroy(bus->mutex);
            free(bus);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate event workers");
        }
        for (size_t i = 0; i < bus->worker_count; i++) {
            rc = cc_thread_create(event_worker_main, bus, &bus->workers[i]);
            if (rc.code != CC_OK) {
                cc_mutex_lock(bus->mutex);
                bus->shutting_down = 1;
                cc_cond_broadcast(bus->cond);
                cc_mutex_unlock(bus->mutex);
                for (size_t j = 0; j < i; j++) cc_thread_join(bus->workers[j]);
                free(bus->workers);
                cc_cond_destroy(bus->cond);
                cc_mutex_destroy(bus->mutex);
                free(bus);
                return rc;
            }
        }
    }
    *out_bus = bus;
    return cc_result_ok();
}

/**
 * @brief 在空闲槽复制主题并登记借用回调，成功将槽加入精确或通配索引。
 * @param[in,out] bus 借用的存活总线，锁内排除关闭和槽位争用。
 * @param[in] event_type 同步借用的主题，NULL 为通配订阅，空串为精确空主题。
 * @param[in] handler 必填，代码保持至全部已接纳回调结束，在总线锁外调用。
 * @param[in,out] user_data 可选借用上下文，退订超时后仍覆盖原订阅已接纳回调。
 * @param[in] options 可选完整配置，本次读取策略、邮箱条数及毫秒等待预算。
 * @param[out] out_subscription 必填值输出，基础参数和策略有效后清零，成功写索引和十六位代际。
 * @return CC_OK，参数、关闭、槽/目录容量或分配错误；失败前已创建的主题副本会清理。
 */
cc_result_t cc_event_bus_subscribe(
    cc_event_bus_t *bus,
    const char *event_type,
    cc_event_handler_fn handler,
    void *user_data,
    const cc_event_subscription_options_t *options,
    cc_event_subscription_t *out_subscription)
{
    if (!bus || !handler || !out_subscription) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid event subscription request");
    }
    if (options && (unsigned)options->overflow_policy > (unsigned)CC_EVENT_BLOCK_WITH_TIMEOUT) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid event mailbox overflow policy");
    }
    memset(out_subscription, 0, sizeof(*out_subscription));
    cc_mutex_lock(bus->mutex);
    if (bus->shutting_down) {
        cc_mutex_unlock(bus->mutex);
        return cc_result_error(CC_ERR_CANCELLED, "Event bus is shutting down");
    }
    size_t slot_index = CC_EVENT_MAX_SUBSCRIBERS;
    for (size_t i = 0; i < CC_EVENT_MAX_SUBSCRIBERS; i++) {
        /* 退订已超时且回调仍在途的槽，持续保留到最后一个回调返回并自动回收。 */
        if (!bus->subscribers[i].active &&
            bus->subscribers[i].inflight == 0 &&
            bus->subscribers[i].handler == NULL &&
            bus->subscribers[i].event_type == NULL) {
            slot_index = i;
            break;
        }
    }
    if (slot_index == CC_EVENT_MAX_SUBSCRIBERS) {
        cc_mutex_unlock(bus->mutex);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Event subscriber slots are full");
    }
    cc_event_subscriber_slot_t *slot = &bus->subscribers[slot_index];
    char *owned_topic = event_type ? cc_copy_string(event_type) : NULL;
    if (event_type && !owned_topic) {
        cc_mutex_unlock(bus->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy event topic");
    }
    uint64_t bit = 1ULL << slot_index;
    if (event_type) {
        int allocation_failed = 0;
        cc_event_topic_entry_t *topic = topic_lookup_locked(bus, event_type, 1, &allocation_failed);
        if (!topic) {
            free(owned_topic);
            cc_mutex_unlock(bus->mutex);
            return cc_result_error(allocation_failed ? CC_ERR_OUT_OF_MEMORY : CC_ERR_LIMIT_EXCEEDED,
                allocation_failed ? "Failed to copy event directory topic" : "Event topic directory is full");
        }
        topic->subscribers |= bit;
    } else {
        bus->wildcard_subscribers |= bit;
    }
    slot->active = 1;
    slot->event_type = owned_topic;
    slot->handler = handler;
    slot->user_data = user_data;
    slot->overflow_policy = options ? options->overflow_policy : CC_EVENT_BLOCK_WITH_TIMEOUT;
    size_t requested_capacity = options && options->mailbox_capacity ?
        options->mailbox_capacity : bus->default_mailbox_capacity;
    slot->mailbox_capacity = normalized_mailbox_capacity(requested_capacity);
    slot->block_timeout_ms = options && options->block_timeout_ms > 0 ?
        options->block_timeout_ms : 30000;
    out_subscription->slot = (uint16_t)slot_index;
    out_subscription->generation = slot->generation;
    bus->metrics.active_subscribers++;
    cc_mutex_unlock(bus->mutex);
    return cc_result_ok();
}

/**
 * @brief 归还一个已登记的长调用使用者，并通知等待销毁的 owner。
 * @pre 调用者持 bus->mutex，当前路径恰好保管一份 lifecycle_users 登记。
 * @param[in,out] bus 非空总线，计数减一并广播条件变量。
 * @post 本次 API 结束后不再跨锁使用总线，destroy 可继续检查其余登记。
 */
static void lifecycle_user_finished_locked(cc_event_bus_t *bus)
{
    bus->lifecycle_users--;
    cc_cond_broadcast(bus->cond);
}

/**
 * @brief 撤销订阅索引和排队事件，按原内部 epoch 等待已接纳回调退出。
 * @param[in,out] bus 借用的存活总线，加锁后登记等待寿命，退出前归还。
 * @param[in] subscription 本总线值句柄，先核对公开代际，再捕获内部宽 epoch。
 * @param[in] timeout_ms 非负毫秒预算，零允许立即返回仍在途的超时结果。
 * @return 排空或原 epoch 已自动回收为 CC_OK，参数非法为 INVALID_ARGUMENT，旧公开句柄为 NOT_FOUND，仍在途为 TIMEOUT。
 * @note 条件等待释放 mutex，槽复用后只结束本次旧订阅等待。预留的同步快照回调可在退订后才开始。
 */
cc_result_t cc_event_bus_unsubscribe(
    cc_event_bus_t *bus,
    cc_event_subscription_t subscription,
    int timeout_ms)
{
    if (!bus || subscription.slot >= CC_EVENT_MAX_SUBSCRIBERS || timeout_ms < 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid event unsubscribe request");
    }
    cc_mutex_lock(bus->mutex);
    bus->lifecycle_users++;
    cc_event_subscriber_slot_t *slot = &bus->subscribers[subscription.slot];
    if (slot->generation != subscription.generation) {
        lifecycle_user_finished_locked(bus);
        cc_mutex_unlock(bus->mutex);
        return cc_result_error(CC_ERR_NOT_FOUND, "Event subscription handle is stale");
    }
    uint64_t expected_epoch = slot->epoch;
    if (slot->active) {
        slot->active = 0;
        uint64_t bit = ~(1ULL << subscription.slot);
        bus->wildcard_subscribers &= bit;
        for (size_t i = 0; i < CC_EVENT_TOPIC_CAPACITY; i++) {
            cc_event_topic_entry_t *topic = &bus->topics[i];
            if (topic->used != 1) continue;
            topic->subscribers &= bit;
            if (topic->subscribers == 0) {
                free(topic->topic);
                topic->topic = NULL;
                topic->hash = 0;
                topic->used = 2;
                if (bus->metrics.active_topics > 0) bus->metrics.active_topics--;
            }
        }
        ready_remove_slot_locked(bus, subscription.slot);
        while (slot->mailbox_count > 0) envelope_release_locked(mailbox_pop_locked(slot));
        if (bus->metrics.active_subscribers > 0) bus->metrics.active_subscribers--;
        cc_cond_broadcast(bus->cond);
    }
    uint64_t wait_started_ms = cc_platform_monotonic_ms();
    while (slot->epoch == expected_epoch && slot->inflight > 0) {
        uint64_t elapsed_ms = cc_platform_monotonic_ms() - wait_started_ms;
        if (elapsed_ms >= (uint64_t)timeout_ms) break;
        uint64_t remaining_ms = (uint64_t)timeout_ms - elapsed_ms;
        int slice = remaining_ms > 100U ? 100 : (int)remaining_ms;
        (void)cc_cond_timedwait(bus->cond, bus->mutex, slice);
    }
    if (slot->epoch == expected_epoch && slot->inflight > 0) {
        lifecycle_user_finished_locked(bus);
        cc_mutex_unlock(bus->mutex);
        return cc_result_error(CC_ERR_TIMEOUT, "Event unsubscribe timed out");
    }
    if (slot->epoch == expected_epoch) subscriber_slot_reclaim_locked(slot);
    lifecycle_user_finished_locked(bus);
    cc_mutex_unlock(bus->mutex);
    return cc_result_ok();
}

/**
 * @brief 锁内快照匹配回调并预留 inflight，随后逐个锁外调用。
 * @param[in,out] bus 借用总线，外层 publisher 登记覆盖本函数及锁外回调。
 * @param[in] envelope 同步借用的非空信封，覆盖全部快照回调及指标更新。
 * @return 已关闭为 CANCELLED，全部快照回调返回为 CC_OK。
 * @note 快照一次接纳全部匹配项，退订会等待这些预留项；并发发布可同时进入同一同步 handler。
 */
static cc_result_t publish_sync(
    cc_event_bus_t *bus,
    cc_event_envelope_t *envelope)
{
    cc_event_sync_snapshot_t snapshots[CC_EVENT_MAX_SUBSCRIBERS];
    size_t count = 0;
    cc_mutex_lock(bus->mutex);
    if (bus->shutting_down) {
        cc_mutex_unlock(bus->mutex);
        return cc_result_error(CC_ERR_CANCELLED, "Event bus is shutting down");
    }
    cc_event_topic_entry_t *topic = topic_lookup_locked(bus, envelope->event_type, 0, NULL);
    uint64_t subscribers = bus->wildcard_subscribers | (topic ? topic->subscribers : 0);
    for (size_t i = 0; i < CC_EVENT_MAX_SUBSCRIBERS; i++) {
        if ((subscribers & (1ULL << i)) == 0) continue;
        cc_event_subscriber_slot_t *slot = &bus->subscribers[i];
        if (!slot->active) continue;
        slot->inflight++;
        snapshots[count++] = (cc_event_sync_snapshot_t){
            .slot = i,
            .generation = slot->generation,
            .handler = slot->handler,
            .user_data = slot->user_data,
        };
    }
    bus->metrics.matched += count;
    cc_mutex_unlock(bus->mutex);

    for (size_t i = 0; i < count; i++) {
        snapshots[i].handler(envelope->event_type, envelope->event_json, snapshots[i].user_data);
        cc_mutex_lock(bus->mutex);
        cc_event_subscriber_slot_t *slot = &bus->subscribers[snapshots[i].slot];
        if (slot->generation == snapshots[i].generation && slot->inflight > 0) slot->inflight--;
        subscriber_slot_reclaim_locked(slot);
        bus->metrics.delivered++;
        uint64_t latency = cc_platform_monotonic_ms() - envelope->published_ms;
        bus->metrics.delivery_latency_sum_ms += latency;
        if (latency > bus->metrics.delivery_latency_max_ms) {
            bus->metrics.delivery_latency_max_ms = latency;
        }
        cc_cond_broadcast(bus->cond);
        cc_mutex_unlock(bus->mutex);
    }
    return cc_result_ok();
}

/**
 * @brief 快照异步路由的位图及内部宽代际，逐订阅投递并消费初始信封引用。
 * @param[in,out] bus 借用总线，外层 publisher 登记覆盖条件等待和部分投递。
 * @param[in,out] envelope 消费拥有的非空初始引用，实际邮箱继续保管其各自引用。
 * @return 已关闭为 CANCELLED，其余返回逐项投递的首个错误或 CC_OK，全部分支归还初始信封引用。
 * @note 宽代际数组保存在本异步帧，同步发布使用自身 inflight 预留及紧凑快照。
 */
static cc_result_t publish_async(cc_event_bus_t *bus, cc_event_envelope_t *envelope)
{
    cc_mutex_lock(bus->mutex);
    if (bus->shutting_down) {
        envelope_release_locked(envelope);
        cc_mutex_unlock(bus->mutex);
        return cc_result_error(CC_ERR_CANCELLED, "Event bus is shutting down");
    }
    cc_event_topic_entry_t *topic = topic_lookup_locked(bus, envelope->event_type, 0, NULL);
    uint64_t subscribers = bus->wildcard_subscribers | (topic ? topic->subscribers : 0);
    uint64_t epochs[CC_EVENT_MAX_SUBSCRIBERS];
    for (size_t i = 0; i < CC_EVENT_MAX_SUBSCRIBERS; i++) {
        epochs[i] = bus->subscribers[i].epoch;
    }
    bus->metrics.published++;
    cc_result_t first_error = cc_result_ok();
    for (size_t i = 0; i < CC_EVENT_MAX_SUBSCRIBERS; i++) {
        if ((subscribers & (1ULL << i)) == 0 || !bus->subscribers[i].active ||
            bus->subscribers[i].epoch != epochs[i]) continue;
        bus->metrics.matched++;
        cc_result_t enqueue_rc = enqueue_for_subscriber_locked(bus, i, epochs[i], envelope);
        if (enqueue_rc.code != CC_OK && first_error.code == CC_OK) {
            first_error = enqueue_rc;
        } else {
            cc_result_free(&enqueue_rc);
        }
    }
    envelope_release_locked(envelope);
    cc_mutex_unlock(bus->mutex);
    return first_error;
}

/**
 * @brief 在锁内归还外层发布者登记并唤醒排空等待者。
 * @param[in,out] bus 非空总线，当前路径保管一次 publishers 登记。
 * @post 计数减一，本次发布后续不再访问总线。
 */
static void publisher_finished(cc_event_bus_t *bus)
{
    cc_mutex_lock(bus->mutex);
    bus->publishers--;
    cc_cond_broadcast(bus->cond);
    cc_mutex_unlock(bus->mutex);
}

/**
 * @brief 登记发布寿命，复制脱敏信封，并按同步快照或异步路由快照分发。
 * @param[in,out] bus 借用的存活总线，关闭后拒绝接纳，已登记发布者覆盖脱敏和背压等待。
 * @param[in] event_type 必填同步借用主题，按字节匹配，空串可用。
 * @param[in] event_json 可选同步借用正文，NULL 交脱敏器按空串处理，回调取得信封副本借用。
 * @return 同步调用或异步投递尝试完成为 CC_OK，分配/关闭/背压错误返回首个错误，其他错误结果消息在内部释放。
 * @note 异步快照同时保管位图和内部 epoch，单个订阅错误后继续其余投递，已投递项持续有效。
 */
cc_result_t cc_event_bus_publish(
    cc_event_bus_t *bus,
    const char *event_type,
    const char *event_json)
{
    if (!bus || !event_type) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid event publish request");
    cc_mutex_lock(bus->mutex);
    if (bus->shutting_down) {
        cc_mutex_unlock(bus->mutex);
        return cc_result_error(CC_ERR_CANCELLED, "Event bus is shutting down");
    }
    bus->publishers++;
    cc_mutex_unlock(bus->mutex);
    char *redacted = cc_redact_secrets(event_json);
    if (!redacted) {
        publisher_finished(bus);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Event redaction failed; event dropped");
    }
    cc_event_envelope_t *envelope = envelope_create(event_type, redacted);
    free(redacted);
    if (!envelope) {
        publisher_finished(bus);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate event envelope");
    }

    cc_result_t rc = cc_result_ok();
    if (bus->mode == CC_EVENT_BUS_MODE_SYNC) {
        cc_mutex_lock(bus->mutex);
        if (!bus->shutting_down) bus->metrics.published++;
        int shutting_down = bus->shutting_down;
        cc_mutex_unlock(bus->mutex);
        rc = shutting_down ? cc_result_error(CC_ERR_CANCELLED, "Event bus is shutting down") :
            publish_sync(bus, envelope);
        envelope_destroy(envelope);
    } else {
        rc = publish_async(bus, envelope);
    }
    publisher_finished(bus);
    return rc;
}

/**
 * @brief 在总预算内等待邮箱、回调及已接纳发布者归零，保持发布入口状态。
 * @param[in,out] bus 借用总线，本次条件等待登记为 lifecycle_users，退出前广播归还。
 * @param[in] timeout_ms 非负毫秒预算，按不超过一百毫秒片段重新核对。
 * @return 当前空闲为 CC_OK，空总线或负预算为 INVALID_ARGUMENT，未排空为 TIMEOUT。
 * @note owner 关闭入口后得到稳定排空屏障，回调中调用会等到本回调本身退出。
 */
cc_result_t cc_event_bus_flush(cc_event_bus_t *bus, int timeout_ms)
{
    if (!bus || timeout_ms < 0) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid event flush request");
    cc_mutex_lock(bus->mutex);
    bus->lifecycle_users++;
    uint64_t wait_started_ms = cc_platform_monotonic_ms();
    for (;;) {
        size_t pending = 0;
        size_t inflight = 0;
        for (size_t i = 0; i < CC_EVENT_MAX_SUBSCRIBERS; i++) {
            pending += bus->subscribers[i].mailbox_count;
            inflight += bus->subscribers[i].inflight;
        }
        if (pending == 0 && inflight == 0 && bus->publishers == 0) break;
        uint64_t elapsed_ms = cc_platform_monotonic_ms() - wait_started_ms;
        if (elapsed_ms >= (uint64_t)timeout_ms) {
            lifecycle_user_finished_locked(bus);
            cc_mutex_unlock(bus->mutex);
            return cc_result_error(CC_ERR_TIMEOUT, "Event bus flush timed out");
        }
        uint64_t remaining_ms = (uint64_t)timeout_ms - elapsed_ms;
        int slice = remaining_ms > 100U ? 100 : (int)remaining_ms;
        (void)cc_cond_timedwait(bus->cond, bus->mutex, slice);
    }
    lifecycle_user_finished_locked(bus);
    cc_mutex_unlock(bus->mutex);
    return cc_result_ok();
}

/**
 * @brief 设置关闭状态、排空事件，并通过唯一 joining 拥有者归还 worker 句柄。
 * @param[in,out] bus 借用总线，跨锁登记 lifecycle_users，允许与其他已登记 shutdown 重叠。
 * @param[in] timeout_ms 非负毫秒预算，排空及等待其他 join 拥有者共用预算，实际 join 沿线程端口等待。
 * @return CC_OK 或参数、排空/仲裁超时及 join 错误，失败保留关闭状态和未成功 join 的句柄供重试。
 * @note 成功 join 的句柄逐个置 NULL，全部成功才置 workers_joined，等待者由同一 cond 唤醒。
 */
cc_result_t cc_event_bus_shutdown(cc_event_bus_t *bus, int timeout_ms)
{
    if (!bus || timeout_ms < 0) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid event shutdown request");
    cc_mutex_lock(bus->mutex);
    bus->lifecycle_users++;
    uint64_t started_ms = cc_platform_monotonic_ms();
    bus->shutting_down = 1;
    cc_cond_broadcast(bus->cond);
    cc_mutex_unlock(bus->mutex);
    uint64_t elapsed_before_flush = cc_platform_monotonic_ms() - started_ms;
    int flush_budget = elapsed_before_flush >= (uint64_t)timeout_ms ?
        0 : timeout_ms - (int)elapsed_before_flush;
    cc_result_t rc = cc_event_bus_flush(bus, flush_budget);
    cc_mutex_lock(bus->mutex);
    while (rc.code == CC_OK && bus->workers_joining) {
        uint64_t elapsed_ms = cc_platform_monotonic_ms() - started_ms;
        if (elapsed_ms >= (uint64_t)timeout_ms) {
            rc = cc_result_error(CC_ERR_TIMEOUT, "Event worker join wait timed out");
            break;
        }
        uint64_t remaining_ms = (uint64_t)timeout_ms - elapsed_ms;
        int slice = remaining_ms > 100U ? 100 : (int)remaining_ms;
        (void)cc_cond_timedwait(bus->cond, bus->mutex, slice);
    }
    if (rc.code == CC_OK && !bus->workers_joined) {
        bus->workers_joining = 1;
        cc_mutex_unlock(bus->mutex);
        for (size_t i = 0; i < bus->worker_count; i++) {
            if (!bus->workers[i]) continue;
            rc = cc_thread_join(bus->workers[i]);
            if (rc.code != CC_OK) break;
            bus->workers[i] = NULL;
            cc_result_free(&rc);
        }
        cc_mutex_lock(bus->mutex);
        bus->workers_joining = 0;
        if (rc.code == CC_OK) bus->workers_joined = 1;
        cc_cond_broadcast(bus->cond);
    }
    lifecycle_user_finished_locked(bus);
    cc_mutex_unlock(bus->mutex);
    return rc;
}

/**
 * @brief 复制一次锁内指标快照，空总线仍输出有效 size。
 * @param[in] bus 可选借用总线，非 NULL 时保持首次加锁窗口的寿命。
 * @param[out] out_metrics 可选独立输出，NULL 直接返回，非 NULL 先清零后复制。
 * @post 输出按值持有计数，继续运行的总线可在返回后改变指标。
 */
void cc_event_bus_get_metrics(cc_event_bus_t *bus, cc_event_bus_metrics_t *out_metrics)
{
    if (!out_metrics) return;
    memset(out_metrics, 0, sizeof(*out_metrics));
    out_metrics->size = sizeof(*out_metrics);
    if (!bus) return;
    cc_mutex_lock(bus->mutex);
    *out_metrics = bus->metrics;
    out_metrics->size = sizeof(*out_metrics);
    cc_mutex_unlock(bus->mutex);
}

/**
 * @brief 用三十秒总预算关闭事件处理，等待已登记长调用退出后释放总线。
 * @pre 单一 owner 已关闭新 API 入口并保护尚未完成首次加锁登记的调用，当前位于回调之外。
 * @param[in,out] bus 待消费拥有句柄，NULL 为空操作。
 * @post 成功释放信封、主题、worker 数组、cond/mutex 和对象；排空或 join 失败则保持关闭对象供 owner 重试。
 * @note 最后一次 lifecycle_users 等待也计入总预算，已登记 unsubscribe/flush/shutdown 持续保管其借用。
 */
void cc_event_bus_destroy(cc_event_bus_t *bus)
{
    if (!bus) return;
    uint64_t started_ms = cc_platform_monotonic_ms();
    cc_result_t rc = cc_event_bus_shutdown(bus, 30000);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return;
    }
    cc_result_free(&rc);
    cc_mutex_lock(bus->mutex);
    while (bus->lifecycle_users > 0) {
        uint64_t elapsed_ms = cc_platform_monotonic_ms() - started_ms;
        if (elapsed_ms >= 30000U) {
            cc_mutex_unlock(bus->mutex);
            return;
        }
        uint64_t remaining_ms = 30000U - elapsed_ms;
        int slice = remaining_ms > 100U ? 100 : (int)remaining_ms;
        (void)cc_cond_timedwait(bus->cond, bus->mutex, slice);
    }
    for (size_t i = 0; i < CC_EVENT_MAX_SUBSCRIBERS; i++) {
        cc_event_subscriber_slot_t *slot = &bus->subscribers[i];
        while (slot->mailbox_count > 0) envelope_release_locked(mailbox_pop_locked(slot));
        free(slot->event_type);
    }
    for (size_t i = 0; i < CC_EVENT_TOPIC_CAPACITY; i++) {
        if (bus->topics[i].used == 1) free(bus->topics[i].topic);
    }
    cc_mutex_unlock(bus->mutex);
    free(bus->workers);
    cc_cond_destroy(bus->cond);
    cc_mutex_destroy(bus->mutex);
    free(bus);
}
