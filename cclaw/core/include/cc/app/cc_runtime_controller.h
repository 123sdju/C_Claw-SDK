#ifndef CC_RUNTIME_CONTROLLER_H
#define CC_RUNTIME_CONTROLLER_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_stream_chunk.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cc_runtime_controller cc_runtime_controller_t;
typedef struct cc_runtime_builder cc_runtime_builder_t;
typedef uint64_t cc_runtime_controller_run_id_t;

typedef enum cc_runtime_controller_lifecycle {
    CC_RUNTIME_CONTROLLER_STARTING = 0,
    CC_RUNTIME_CONTROLLER_RUNNING,
    CC_RUNTIME_CONTROLLER_DRAINING,
    CC_RUNTIME_CONTROLLER_STOPPED,
    CC_RUNTIME_CONTROLLER_FAILED,
} cc_runtime_controller_lifecycle_t;

typedef struct cc_runtime_controller_config {
    size_t size;
    const char *session_base;
    const char *workspace_dir;
    int drop_late_events;
} cc_runtime_controller_config_t;

typedef struct cc_runtime_controller_status {
    size_t size;
    cc_runtime_controller_lifecycle_t lifecycle;
    cc_runtime_controller_run_id_t active_run_id;
    unsigned long runtime_generation;
    unsigned long session_generation;
    char session_id[96];
} cc_runtime_controller_status_t;

typedef struct cc_runtime_controller_stream_event {
    size_t size;
    const cc_stream_chunk_t *chunk;
    const char *agent_id;
    const char *session_id;
    cc_runtime_controller_run_id_t run_id;
    unsigned long runtime_generation;
} cc_runtime_controller_stream_event_t;

typedef void (*cc_runtime_controller_stream_fn)(
    const cc_runtime_controller_stream_event_t *event,
    void *user_data
);

/* Controller borrows the builder; the builder must outlive the controller. */
cc_result_t cc_runtime_controller_create(
    cc_runtime_builder_t *builder,
    const cc_runtime_controller_config_t *config,
    cc_runtime_controller_t **out_controller
);

cc_result_t cc_runtime_controller_submit_text(
    cc_runtime_controller_t *controller,
    const char *user_input,
    cc_runtime_controller_stream_fn on_stream,
    void *user_data,
    char **out_response
);

/* Cancels exactly the requested active run. */
cc_result_t cc_runtime_controller_cancel(
    cc_runtime_controller_t *controller,
    cc_runtime_controller_run_id_t run_id
);

cc_result_t cc_runtime_controller_shutdown(
    cc_runtime_controller_t *controller,
    int timeout_ms
);

/* Frees the controller only after shutdown succeeds. */
cc_result_t cc_runtime_controller_destroy(
    cc_runtime_controller_t *controller,
    int timeout_ms
);

cc_result_t cc_runtime_controller_clear_session(cc_runtime_controller_t *controller);

cc_result_t cc_runtime_controller_apply_config(
    cc_runtime_controller_t *controller,
    const cc_runtime_controller_config_t *config
);

void cc_runtime_controller_get_status(
    cc_runtime_controller_t *controller,
    cc_runtime_controller_status_t *out_status
);

const char *cc_runtime_controller_session_id(cc_runtime_controller_t *controller);

#ifdef __cplusplus
}
#endif

#endif
