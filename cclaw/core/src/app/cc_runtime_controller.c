#include "cc/app/cc_runtime_controller.h"

#include "cc/app/cc_cancel_token.h"
#include "cc/app/cc_runtime_builder.h"
#include "cc/core/cc_id.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_platform.h"
#include "cc/ports/cc_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cc_runtime_controller {
    cc_runtime_builder_t *builder;
    char *session_base;
    char *workspace_dir;
    char session_id[96];
    unsigned long session_generation;
    unsigned long active_runtime_generation;
    cc_runtime_controller_run_id_t active_run_id;
    int drop_late_events;
    int session_needs_create;
    cc_runtime_controller_lifecycle_t lifecycle;
    cc_cancel_source_t *active_cancel;
    cc_mutex_t mutex;
    cc_cond_t state_changed;
};

typedef struct controller_stream_ctx {
    cc_runtime_controller_t *controller;
    cc_runtime_controller_stream_fn on_stream;
    void *user_data;
    cc_runtime_controller_run_id_t run_id;
    unsigned long runtime_generation;
    char session_id[96];
    cc_cancel_token_t *cancel_token;
} controller_stream_ctx_t;

static const char *session_base_or_default(const char *value)
{
    return value && value[0] ? value : "default";
}

static cc_result_t replace_owned_string(char **field, const char *value)
{
    char *copy = cc_copy_string(value ? value : "");
    if (!copy) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy controller config");
    free(*field);
    *field = copy;
    return cc_result_ok();
}

static void update_session_id_locked(cc_runtime_controller_t *controller)
{
    snprintf(controller->session_id, sizeof(controller->session_id), "%s-%lu",
        session_base_or_default(controller->session_base), controller->session_generation);
}

static cc_result_t create_session_on_generation(
    cc_runtime_controller_t *controller,
    cc_runtime_generation_t *generation,
    const char *session_id)
{
    cc_agent_runtime_t *runtime = cc_runtime_generation_runtime(generation);
    if (!runtime) return cc_result_error(CC_ERR_INVALID_STATE, "Runtime generation is unavailable");
    return cc_agent_runtime_create_session(runtime, session_id,
        controller->workspace_dir && controller->workspace_dir[0] ? controller->workspace_dir : NULL);
}

cc_result_t cc_runtime_controller_create(
    cc_runtime_builder_t *builder,
    const cc_runtime_controller_config_t *config,
    cc_runtime_controller_t **out_controller)
{
    if (!builder || !out_controller) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid runtime controller create request");
    }
    *out_controller = NULL;
    cc_runtime_controller_t *controller = calloc(1, sizeof(*controller));
    if (!controller) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate runtime controller");
    controller->builder = builder;
    controller->session_generation = 1;
    controller->drop_late_events = config ? config->drop_late_events != 0 : 1;
    controller->lifecycle = CC_RUNTIME_CONTROLLER_STARTING;

    cc_result_t rc = cc_mutex_create(&controller->mutex);
    if (rc.code != CC_OK) goto fail;
    rc = cc_cond_create(&controller->state_changed);
    if (rc.code != CC_OK) goto fail;
    rc = replace_owned_string(&controller->session_base,
        config ? config->session_base : "default");
    if (rc.code == CC_OK) {
        rc = replace_owned_string(&controller->workspace_dir,
            config ? config->workspace_dir : NULL);
    }
    if (rc.code != CC_OK) goto fail;
    update_session_id_locked(controller);

    cc_runtime_generation_t *generation = NULL;
    rc = cc_runtime_builder_acquire_generation(builder, &generation);
    if (rc.code == CC_OK) {
        controller->active_runtime_generation = cc_runtime_generation_id(generation);
        rc = create_session_on_generation(controller, generation, controller->session_id);
        cc_runtime_generation_release(generation);
    }
    if (rc.code != CC_OK) goto fail;
    controller->lifecycle = CC_RUNTIME_CONTROLLER_RUNNING;
    *out_controller = controller;
    return cc_result_ok();

fail:
    controller->lifecycle = CC_RUNTIME_CONTROLLER_FAILED;
    if (controller->state_changed) cc_cond_destroy(controller->state_changed);
    if (controller->mutex) cc_mutex_destroy(controller->mutex);
    free(controller->session_base);
    free(controller->workspace_dir);
    free(controller);
    return rc;
}

static int should_forward_chunk(controller_stream_ctx_t *ctx)
{
    if (!ctx || !ctx->controller || cc_cancel_token_is_cancelled(ctx->cancel_token)) return 0;
    int forward = 1;
    cc_mutex_lock(ctx->controller->mutex);
    if (ctx->controller->drop_late_events &&
        (ctx->controller->active_run_id != ctx->run_id ||
         ctx->controller->active_runtime_generation != ctx->runtime_generation)) {
        forward = 0;
    }
    cc_mutex_unlock(ctx->controller->mutex);
    return forward;
}

static cc_result_t controller_stream_cb(const cc_stream_chunk_t *chunk, void *user_data)
{
    controller_stream_ctx_t *ctx = (controller_stream_ctx_t *)user_data;
    if (!chunk || !ctx || !ctx->on_stream) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid controller stream callback");
    }
    if (cc_cancel_token_is_cancelled(ctx->cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "Runtime controller stream cancelled");
    }
    if (!should_forward_chunk(ctx)) return cc_result_ok();
    cc_runtime_controller_stream_event_t event = {0};
    event.size = sizeof(event);
    event.chunk = chunk;
    event.session_id = ctx->session_id;
    event.run_id = ctx->run_id;
    event.runtime_generation = ctx->runtime_generation;
    ctx->on_stream(&event, ctx->user_data);
    return cc_result_ok();
}

cc_result_t cc_runtime_controller_submit_text(
    cc_runtime_controller_t *controller,
    const char *user_input,
    cc_runtime_controller_stream_fn on_stream,
    void *user_data,
    char **out_response)
{
    if (!controller || !user_input || !out_response) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid runtime controller submit request");
    }
    *out_response = NULL;
    cc_runtime_generation_t *generation = NULL;
    cc_result_t rc = cc_runtime_builder_acquire_generation(controller->builder, &generation);
    if (rc.code != CC_OK) return rc;

    cc_cancel_source_t *cancel_source = NULL;
    rc = cc_cancel_source_create(&cancel_source);
    if (rc.code != CC_OK) {
        cc_runtime_generation_release(generation);
        return rc;
    }

    char session_id[sizeof(controller->session_id)];
    int create_session = 0;
    cc_runtime_controller_run_id_t run_id = 0;
    unsigned long runtime_generation = cc_runtime_generation_id(generation);
    rc = cc_id_generate_u64(&run_id);
    if (rc.code != CC_OK) {
        cc_cancel_source_destroy(cancel_source);
        cc_runtime_generation_release(generation);
        return rc;
    }
    cc_result_free(&rc);
    cc_mutex_lock(controller->mutex);
    if (controller->lifecycle != CC_RUNTIME_CONTROLLER_RUNNING) {
        cc_mutex_unlock(controller->mutex);
        cc_cancel_source_destroy(cancel_source);
        cc_runtime_generation_release(generation);
        return cc_result_error(CC_ERR_INVALID_STATE, "Runtime controller is not accepting runs");
    }
    if (controller->active_run_id != 0) {
        cc_mutex_unlock(controller->mutex);
        cc_cancel_source_destroy(cancel_source);
        cc_runtime_generation_release(generation);
        return cc_result_error(CC_ERR_QUEUE_FULL, "Runtime controller already has an active run");
    }
    update_session_id_locked(controller);
    snprintf(session_id, sizeof(session_id), "%s", controller->session_id);
    create_session = controller->session_needs_create;
    controller->session_needs_create = 0;
    controller->active_run_id = run_id;
    controller->active_runtime_generation = runtime_generation;
    controller->active_cancel = cancel_source;
    cc_mutex_unlock(controller->mutex);

    if (create_session) rc = create_session_on_generation(controller, generation, session_id);
    cc_agent_runtime_t *runtime = cc_runtime_generation_runtime(generation);
    cc_cancel_token_t *cancel_token = cc_cancel_source_token(cancel_source);
    if (rc.code == CC_OK && runtime && cc_agent_runtime_supports_stream(runtime)) {
        controller_stream_ctx_t stream_ctx = {0};
        stream_ctx.controller = controller;
        stream_ctx.on_stream = on_stream;
        stream_ctx.user_data = user_data;
        stream_ctx.run_id = run_id;
        stream_ctx.runtime_generation = runtime_generation;
        stream_ctx.cancel_token = cancel_token;
        snprintf(stream_ctx.session_id, sizeof(stream_ctx.session_id), "%s", session_id);
        cc_agent_runtime_stream_options_t options = {0};
        options.size = sizeof(options);
        options.cancel_token = cancel_token;
        options.on_chunk = controller_stream_cb;
        options.user_data = &stream_ctx;
        rc = cc_agent_runtime_handle_message_stream_cb(
            runtime, session_id, user_input, &options, out_response);
    } else if (rc.code == CC_OK && runtime) {
        cc_agent_runtime_run_options_t options = {0};
        options.size = sizeof(options);
        options.cancel_token = cancel_token;
        rc = cc_agent_runtime_handle_message_with_options(
            runtime, session_id, user_input, &options, out_response);
    } else if (rc.code == CC_OK) {
        rc = cc_result_error(CC_ERR_INVALID_STATE, "Runtime generation has no runtime");
    }
    int cancelled = cc_cancel_token_is_cancelled(cancel_token);

    cc_mutex_lock(controller->mutex);
    if (controller->active_run_id == run_id) {
        controller->active_run_id = 0;
        controller->active_cancel = NULL;
    }
    cc_cond_broadcast(controller->state_changed);
    cc_mutex_unlock(controller->mutex);
    cc_cancel_source_destroy(cancel_source);
    cc_runtime_generation_release(generation);

    if (rc.code == CC_OK && cancelled) {
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_CANCELLED, "Runtime controller run cancelled");
    }
    return rc;
}

cc_result_t cc_runtime_controller_cancel(
    cc_runtime_controller_t *controller,
    cc_runtime_controller_run_id_t run_id)
{
    if (!controller || run_id == 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid runtime cancel request");
    }
    cc_mutex_lock(controller->mutex);
    if (controller->active_run_id != run_id || !controller->active_cancel) {
        cc_mutex_unlock(controller->mutex);
        return cc_result_error(CC_ERR_NOT_FOUND, "Runtime run is not active");
    }
    cc_cancel_source_cancel(controller->active_cancel);
    cc_mutex_unlock(controller->mutex);
    return cc_result_ok();
}

cc_result_t cc_runtime_controller_shutdown(cc_runtime_controller_t *controller, int timeout_ms)
{
    if (!controller || timeout_ms < 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid runtime shutdown request");
    }
    cc_mutex_lock(controller->mutex);
    if (controller->lifecycle == CC_RUNTIME_CONTROLLER_STOPPED) {
        cc_mutex_unlock(controller->mutex);
        return cc_result_ok();
    }
    controller->lifecycle = CC_RUNTIME_CONTROLLER_DRAINING;
    if (controller->active_cancel) cc_cancel_source_cancel(controller->active_cancel);
    uint64_t wait_started_ms = cc_platform_monotonic_ms();
    while (controller->active_run_id != 0) {
        uint64_t elapsed_ms = cc_platform_monotonic_ms() - wait_started_ms;
        if (elapsed_ms >= (uint64_t)timeout_ms) break;
        uint64_t remaining_ms = (uint64_t)timeout_ms - elapsed_ms;
        int slice = remaining_ms > 250U ? 250 : (int)remaining_ms;
        (void)cc_cond_timedwait(controller->state_changed, controller->mutex, slice);
    }
    if (controller->active_run_id != 0) {
        cc_mutex_unlock(controller->mutex);
        return cc_result_error(CC_ERR_TIMEOUT, "Runtime controller shutdown timed out");
    }
    controller->lifecycle = CC_RUNTIME_CONTROLLER_STOPPED;
    cc_cond_broadcast(controller->state_changed);
    cc_mutex_unlock(controller->mutex);
    return cc_result_ok();
}

cc_result_t cc_runtime_controller_destroy(cc_runtime_controller_t *controller, int timeout_ms)
{
    if (!controller) return cc_result_ok();
    cc_result_t rc = cc_runtime_controller_shutdown(controller, timeout_ms);
    if (rc.code != CC_OK) return rc;
    cc_cancel_source_destroy(controller->active_cancel);
    cc_cond_destroy(controller->state_changed);
    cc_mutex_destroy(controller->mutex);
    free(controller->session_base);
    free(controller->workspace_dir);
    free(controller);
    return cc_result_ok();
}

cc_result_t cc_runtime_controller_clear_session(cc_runtime_controller_t *controller)
{
    if (!controller) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null runtime controller");
    cc_mutex_lock(controller->mutex);
    if (controller->lifecycle != CC_RUNTIME_CONTROLLER_RUNNING) {
        cc_mutex_unlock(controller->mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Runtime controller is not running");
    }
    if (controller->active_run_id != 0) {
        cc_mutex_unlock(controller->mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Cannot clear an active session");
    }
    controller->session_generation++;
    controller->session_needs_create = 1;
    update_session_id_locked(controller);
    cc_mutex_unlock(controller->mutex);
    return cc_result_ok();
}

cc_result_t cc_runtime_controller_apply_config(
    cc_runtime_controller_t *controller,
    const cc_runtime_controller_config_t *config)
{
    if (!controller || !config) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid runtime controller config request");
    }
    char *new_base = cc_copy_string(config->session_base ? config->session_base : controller->session_base);
    char *new_workspace = cc_copy_string(config->workspace_dir ? config->workspace_dir : controller->workspace_dir);
    if (!new_base || !new_workspace) {
        free(new_base);
        free(new_workspace);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to prepare runtime controller config");
    }
    cc_mutex_lock(controller->mutex);
    if (controller->lifecycle != CC_RUNTIME_CONTROLLER_RUNNING || controller->active_run_id != 0) {
        cc_mutex_unlock(controller->mutex);
        free(new_base);
        free(new_workspace);
        return cc_result_error(CC_ERR_INVALID_STATE, "Controller config requires an idle running controller");
    }
    free(controller->session_base);
    free(controller->workspace_dir);
    controller->session_base = new_base;
    controller->workspace_dir = new_workspace;
    controller->drop_late_events = config->drop_late_events != 0;
    controller->session_generation++;
    controller->session_needs_create = 1;
    update_session_id_locked(controller);
    cc_mutex_unlock(controller->mutex);
    return cc_result_ok();
}

void cc_runtime_controller_get_status(
    cc_runtime_controller_t *controller,
    cc_runtime_controller_status_t *out_status)
{
    if (!out_status) return;
    memset(out_status, 0, sizeof(*out_status));
    out_status->size = sizeof(*out_status);
    if (!controller) return;
    cc_mutex_lock(controller->mutex);
    out_status->lifecycle = controller->lifecycle;
    out_status->active_run_id = controller->active_run_id;
    out_status->runtime_generation = controller->active_runtime_generation;
    out_status->session_generation = controller->session_generation;
    snprintf(out_status->session_id, sizeof(out_status->session_id), "%s", controller->session_id);
    cc_mutex_unlock(controller->mutex);
}

const char *cc_runtime_controller_session_id(cc_runtime_controller_t *controller)
{
    return controller ? controller->session_id : NULL;
}
