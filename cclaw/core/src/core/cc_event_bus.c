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

typedef struct cc_event_envelope {
    char *event_type;
    char *event_json;
    size_t refs;
    uint64_t published_ms;
} cc_event_envelope_t;

typedef struct cc_event_subscriber_slot {
    int active;
    uint16_t generation;
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
    /* 0 = never used, 1 = occupied, 2 = tombstone (keeps probe chains intact). */
    int used;
    uint64_t hash;
    char *topic;
    uint64_t subscribers;
} cc_event_topic_entry_t;

typedef struct cc_event_sync_snapshot {
    size_t slot;
    uint16_t generation;
    cc_event_handler_fn handler;
    void *user_data;
} cc_event_sync_snapshot_t;

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
    int shutting_down;

    cc_event_bus_metrics_t metrics;
    cc_mutex_t mutex;
    cc_cond_t cond;
};

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

static size_t normalized_worker_count(size_t value)
{
    return value ? value : DEFAULT_ASYNC_WORKERS;
}

static size_t normalized_mailbox_capacity(size_t value)
{
    if (value == 0) value = DEFAULT_MAILBOX_CAPACITY;
    return value > CC_EVENT_MAILBOX_MAX ? CC_EVENT_MAILBOX_MAX : value;
}

static void envelope_destroy(cc_event_envelope_t *envelope)
{
    if (!envelope) return;
    free(envelope->event_type);
    free(envelope->event_json);
    free(envelope);
}

static void envelope_release_locked(cc_event_envelope_t *envelope)
{
    if (!envelope) return;
    if (envelope->refs > 0) envelope->refs--;
    if (envelope->refs == 0) envelope_destroy(envelope);
}

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

static cc_event_topic_entry_t *topic_lookup_locked(
    cc_event_bus_t *bus,
    const char *topic,
    int create)
{
    uint64_t hash = topic_hash(topic);
    size_t start = (size_t)(hash % CC_EVENT_TOPIC_CAPACITY);
    cc_event_topic_entry_t *first_tombstone = NULL;
    for (size_t probe = 0; probe < CC_EVENT_TOPIC_CAPACITY; probe++) {
        cc_event_topic_entry_t *entry = &bus->topics[(start + probe) % CC_EVENT_TOPIC_CAPACITY];
        if (entry->used == 0) {
            if (!create) return NULL;
            cc_event_topic_entry_t *destination = first_tombstone ? first_tombstone : entry;
            destination->topic = cc_copy_string(topic);
            if (!destination->topic) return NULL;
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
        if (!first_tombstone->topic) return NULL;
        first_tombstone->used = 1;
        first_tombstone->hash = hash;
        first_tombstone->subscribers = 0;
        bus->metrics.active_topics++;
        return first_tombstone;
    }
    return NULL;
}

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

static cc_event_envelope_t *mailbox_pop_locked(cc_event_subscriber_slot_t *slot)
{
    if (!slot || slot->mailbox_count == 0) return NULL;
    cc_event_envelope_t *envelope = slot->mailbox[slot->mailbox_head];
    slot->mailbox[slot->mailbox_head] = NULL;
    slot->mailbox_head = (slot->mailbox_head + 1) % CC_EVENT_MAILBOX_MAX;
    slot->mailbox_count--;
    return envelope;
}

static void mailbox_push_locked(
    cc_event_subscriber_slot_t *slot,
    cc_event_envelope_t *envelope)
{
    size_t tail = (slot->mailbox_head + slot->mailbox_count) % CC_EVENT_MAILBOX_MAX;
    slot->mailbox[tail] = envelope;
    slot->mailbox_count++;
    envelope->refs++;
}

static cc_result_t enqueue_for_subscriber_locked(
    cc_event_bus_t *bus,
    size_t slot_index,
    uint16_t expected_generation,
    cc_event_envelope_t *envelope)
{
    cc_event_subscriber_slot_t *slot = &bus->subscribers[slot_index];
    uint64_t wait_start = cc_platform_monotonic_ms();
    while (slot->active && slot->generation == expected_generation &&
           slot->mailbox_count >= slot->mailbox_capacity &&
           slot->overflow_policy == CC_EVENT_BLOCK_WITH_TIMEOUT) {
        int timeout = slot->block_timeout_ms > 0 ? slot->block_timeout_ms : 1;
        int signalled = cc_cond_timedwait(bus->cond, bus->mutex, timeout);
        (void)signalled;
        uint64_t elapsed = cc_platform_monotonic_ms() - wait_start;
        if (elapsed >= (uint64_t)timeout) {
            bus->metrics.block_time_ms += elapsed;
            return cc_result_error(CC_ERR_TIMEOUT, "Event subscriber mailbox wait timed out");
        }
        if (bus->shutting_down) return cc_result_error(CC_ERR_CANCELLED, "Event bus is shutting down");
    }
    bus->metrics.block_time_ms += cc_platform_monotonic_ms() - wait_start;
    if (!slot->active || slot->generation != expected_generation) {
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

cc_event_bus_config_t cc_event_bus_default_config(void)
{
    cc_event_bus_config_t config = {0};
    config.size = sizeof(config);
    config.mode = CC_EVENT_BUS_MODE_SYNC;
    return config;
}

cc_result_t cc_event_bus_create(cc_event_bus_t **out_bus)
{
    return cc_event_bus_create_with_config(NULL, out_bus);
}

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
    for (size_t i = 0; i < CC_EVENT_MAX_SUBSCRIBERS; i++) bus->subscribers[i].generation = 1;

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
    if (options && options->overflow_policy > CC_EVENT_BLOCK_WITH_TIMEOUT) {
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
        /* A timed-out unsubscribe keeps its slot reserved until retry cleanup. */
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
        cc_event_topic_entry_t *topic = topic_lookup_locked(bus, event_type, 1);
        if (!topic) {
            free(owned_topic);
            cc_mutex_unlock(bus->mutex);
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Event topic directory is full");
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

cc_result_t cc_event_bus_unsubscribe(
    cc_event_bus_t *bus,
    cc_event_subscription_t subscription,
    int timeout_ms)
{
    if (!bus || subscription.slot >= CC_EVENT_MAX_SUBSCRIBERS || timeout_ms < 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid event unsubscribe request");
    }
    cc_mutex_lock(bus->mutex);
    cc_event_subscriber_slot_t *slot = &bus->subscribers[subscription.slot];
    if (slot->generation != subscription.generation) {
        cc_mutex_unlock(bus->mutex);
        return cc_result_error(CC_ERR_NOT_FOUND, "Event subscription handle is stale");
    }
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
    while (slot->inflight > 0) {
        uint64_t elapsed_ms = cc_platform_monotonic_ms() - wait_started_ms;
        if (elapsed_ms >= (uint64_t)timeout_ms) break;
        uint64_t remaining_ms = (uint64_t)timeout_ms - elapsed_ms;
        int slice = remaining_ms > 100U ? 100 : (int)remaining_ms;
        (void)cc_cond_timedwait(bus->cond, bus->mutex, slice);
    }
    if (slot->inflight > 0) {
        cc_mutex_unlock(bus->mutex);
        return cc_result_error(CC_ERR_TIMEOUT, "Event unsubscribe timed out");
    }
    free(slot->event_type);
    slot->event_type = NULL;
    slot->handler = NULL;
    slot->user_data = NULL;
    slot->generation++;
    if (slot->generation == 0) slot->generation = 1;
    cc_mutex_unlock(bus->mutex);
    return cc_result_ok();
}

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
    cc_event_topic_entry_t *topic = topic_lookup_locked(bus, envelope->event_type, 0);
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

cc_result_t cc_event_bus_publish(
    cc_event_bus_t *bus,
    const char *event_type,
    const char *event_json)
{
    if (!bus || !event_type) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid event publish request");
    char *redacted = cc_redact_secrets(event_json);
    cc_event_envelope_t *envelope = envelope_create(event_type, redacted ? redacted : event_json);
    free(redacted);
    if (!envelope) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate event envelope");

    cc_result_t rc = cc_result_ok();
    if (bus->mode == CC_EVENT_BUS_MODE_SYNC) {
        cc_mutex_lock(bus->mutex);
        if (!bus->shutting_down) bus->metrics.published++;
        int shutting_down = bus->shutting_down;
        cc_mutex_unlock(bus->mutex);
        rc = shutting_down ? cc_result_error(CC_ERR_CANCELLED, "Event bus is shutting down") :
            publish_sync(bus, envelope);
    } else {
        cc_mutex_lock(bus->mutex);
        if (bus->shutting_down) {
            cc_mutex_unlock(bus->mutex);
            envelope_destroy(envelope);
            return cc_result_error(CC_ERR_CANCELLED, "Event bus is shutting down");
        }
        cc_event_topic_entry_t *topic = topic_lookup_locked(bus, event_type, 0);
        uint64_t subscribers = bus->wildcard_subscribers | (topic ? topic->subscribers : 0);
        bus->metrics.published++;
        cc_result_t first_error = cc_result_ok();
        for (size_t i = 0; i < CC_EVENT_MAX_SUBSCRIBERS; i++) {
            if ((subscribers & (1ULL << i)) == 0 || !bus->subscribers[i].active) continue;
            bus->metrics.matched++;
            uint16_t generation = bus->subscribers[i].generation;
            cc_result_t enqueue_rc = enqueue_for_subscriber_locked(bus, i, generation, envelope);
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
    envelope_destroy(envelope);
    return rc;
}

cc_result_t cc_event_bus_flush(cc_event_bus_t *bus, int timeout_ms)
{
    if (!bus || timeout_ms < 0) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid event flush request");
    if (bus->mode == CC_EVENT_BUS_MODE_SYNC) return cc_result_ok();
    cc_mutex_lock(bus->mutex);
    uint64_t wait_started_ms = cc_platform_monotonic_ms();
    for (;;) {
        size_t pending = 0;
        size_t inflight = 0;
        for (size_t i = 0; i < CC_EVENT_MAX_SUBSCRIBERS; i++) {
            pending += bus->subscribers[i].mailbox_count;
            inflight += bus->subscribers[i].inflight;
        }
        if (pending == 0 && inflight == 0) break;
        uint64_t elapsed_ms = cc_platform_monotonic_ms() - wait_started_ms;
        if (elapsed_ms >= (uint64_t)timeout_ms) {
            cc_mutex_unlock(bus->mutex);
            return cc_result_error(CC_ERR_TIMEOUT, "Event bus flush timed out");
        }
        uint64_t remaining_ms = (uint64_t)timeout_ms - elapsed_ms;
        int slice = remaining_ms > 100U ? 100 : (int)remaining_ms;
        (void)cc_cond_timedwait(bus->cond, bus->mutex, slice);
    }
    cc_mutex_unlock(bus->mutex);
    return cc_result_ok();
}

cc_result_t cc_event_bus_shutdown(cc_event_bus_t *bus, int timeout_ms)
{
    if (!bus || timeout_ms < 0) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid event shutdown request");
    cc_mutex_lock(bus->mutex);
    bus->shutting_down = 1;
    cc_cond_broadcast(bus->cond);
    cc_mutex_unlock(bus->mutex);
    cc_result_t rc = cc_event_bus_flush(bus, timeout_ms);
    if (rc.code != CC_OK) return rc;
    if (!bus->workers_joined) {
        cc_mutex_lock(bus->mutex);
        cc_cond_broadcast(bus->cond);
        cc_mutex_unlock(bus->mutex);
        for (size_t i = 0; i < bus->worker_count; i++) {
            if (bus->workers[i]) cc_thread_join(bus->workers[i]);
        }
        bus->workers_joined = 1;
    }
    return cc_result_ok();
}

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

void cc_event_bus_destroy(cc_event_bus_t *bus)
{
    if (!bus) return;
    cc_result_t rc = cc_event_bus_shutdown(bus, 30000);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return;
    }
    cc_result_free(&rc);
    cc_mutex_lock(bus->mutex);
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
