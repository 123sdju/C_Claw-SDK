#ifndef CC_EVENT_BUS_H
#define CC_EVENT_BUS_H

#include "cc/core/cc_result.h"
#include <stddef.h>
#include <stdint.h>

typedef void (*cc_event_handler_fn)(
    const char *event_type,
    const char *event_json,
    void *user_data
);

typedef enum cc_event_bus_mode {
    CC_EVENT_BUS_MODE_SYNC = 0,
    CC_EVENT_BUS_MODE_ASYNC = 1,
} cc_event_bus_mode_t;

typedef enum cc_event_overflow_policy {
    CC_EVENT_DROP_NEWEST = 0,
    CC_EVENT_DROP_OLDEST,
    CC_EVENT_COALESCE_LATEST,
    CC_EVENT_BLOCK_WITH_TIMEOUT,
} cc_event_overflow_policy_t;

typedef struct cc_event_subscription {
    uint16_t slot;
    uint16_t generation;
} cc_event_subscription_t;

typedef struct cc_event_subscription_options {
    size_t size;
    cc_event_overflow_policy_t overflow_policy;
    size_t mailbox_capacity;
    int block_timeout_ms;
} cc_event_subscription_options_t;

typedef struct cc_event_bus_config {
    size_t size;
    cc_event_bus_mode_t mode;
    size_t worker_count;
    size_t mailbox_capacity;
} cc_event_bus_config_t;

typedef struct cc_event_bus_metrics {
    size_t size;
    uint64_t published;
    uint64_t matched;
    uint64_t hash_collisions;
    uint64_t delivered;
    uint64_t dropped_newest;
    uint64_t dropped_oldest;
    uint64_t coalesced;
    uint64_t block_time_ms;
    uint64_t delivery_latency_sum_ms;
    uint64_t delivery_latency_max_ms;
    size_t mailbox_high_water;
    size_t active_subscribers;
    size_t active_topics;
} cc_event_bus_metrics_t;

typedef struct cc_event_bus cc_event_bus_t;

cc_event_bus_config_t cc_event_bus_default_config(void);
cc_result_t cc_event_bus_create(cc_event_bus_t **out_bus);
cc_result_t cc_event_bus_create_with_config(
    const cc_event_bus_config_t *config,
    cc_event_bus_t **out_bus
);

cc_result_t cc_event_bus_subscribe(
    cc_event_bus_t *bus,
    const char *event_type,
    cc_event_handler_fn handler,
    void *user_data,
    const cc_event_subscription_options_t *options,
    cc_event_subscription_t *out_subscription
);

cc_result_t cc_event_bus_unsubscribe(
    cc_event_bus_t *bus,
    cc_event_subscription_t subscription,
    int timeout_ms
);

cc_result_t cc_event_bus_publish(
    cc_event_bus_t *bus,
    const char *event_type,
    const char *event_json
);

cc_result_t cc_event_bus_flush(cc_event_bus_t *bus, int timeout_ms);
cc_result_t cc_event_bus_shutdown(cc_event_bus_t *bus, int timeout_ms);
void cc_event_bus_get_metrics(cc_event_bus_t *bus, cc_event_bus_metrics_t *out_metrics);
void cc_event_bus_destroy(cc_event_bus_t *bus);

#endif
