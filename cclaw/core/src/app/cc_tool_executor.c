



#include "cc/app/cc_tool_executor.h"
#include "cc/app/cc_tool_executor_pool.h"
#include "cc/core/cc_observability.h"
#include "cc_agent_runtime_internal.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"
#include "cc/ports/cc_platform.h"
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/*
 * 工具返回的 artifact 与 provider 请求使用同一套多模态限制。文本限制不能替代
 * artifact 校验，否则 camera/file 工具可以通过 Base64 或 URI 元数据绕过 runtime
 * 的资源预算。
 */
static cc_result_t validate_tool_artifacts(
    const cc_agent_runtime_t *runtime,
    const cc_tool_result_t *result
)
{
    if (!runtime || !result) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid tool artifact validation input");
    }

    cc_media_limits_t limits;
    memset(&limits, 0, sizeof(limits));
    limits.max_artifacts = runtime->config.multimodal.limits.max_artifacts;
    limits.max_artifact_bytes = runtime->config.multimodal.limits.max_artifact_bytes;
    limits.max_base64_bytes = runtime->config.multimodal.limits.max_base64_bytes;
    limits.allow_inline_base64 = runtime->config.multimodal.limits.allow_inline_base64;
    limits.allowed_mime_prefixes =
        (const char **)runtime->config.multimodal.limits.allowed_mime_prefixes.items;
    limits.allowed_mime_prefix_count =
        runtime->config.multimodal.limits.allowed_mime_prefixes.count;

    cc_result_t rc = cc_media_artifact_list_validate(&result->artifacts, &limits);
    if (rc.code != CC_OK) return rc;

    size_t total_base64 = 0;
    for (size_t i = 0; i < result->artifacts.count; i++) {
        const char *encoded = result->artifacts.items[i].data_base64;
        if (!encoded) continue;
        size_t encoded_len = strlen(encoded);
        if (encoded_len > SIZE_MAX - total_base64) {
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED,
                "Tool artifact Base64 size overflow");
        }
        total_base64 += encoded_len;
    }
    if (limits.max_base64_bytes > 0 && total_base64 > limits.max_base64_bytes) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED,
            "Tool artifacts exceed max_base64_bytes");
    }
    return cc_result_ok();
}

/*
 * 发布工具相关观测事件。
 *
 * 工具事件需要携带 tool 名称、tool_call id、参数和执行结果，但这些字段不是统一
 * schema 的顶层字段，所以统一放到 attributes object 中。这里不直接调用
 * event bus 的底层发布函数，保证业务路径只依赖 observability 层。
 */
static void publish_tool_observability(
    cc_agent_runtime_t *runtime,
    const char *event_name,
    const char *session_id,
    const cc_tool_call_t *call,
    const char *status,
    int ok,
    const char *message,
    const char *reason
)
{
    if (!runtime || !runtime->event_bus || !event_name) return;

    cc_json_value_t *attrs = cc_json_create_object();
    if (!attrs) return;
    cc_json_object_set(attrs, "tool",
        cc_json_create_string(call && call->name ? call->name : ""));
    cc_json_object_set(attrs, "tool_call_id",
        cc_json_create_string(call && call->id ? call->id : ""));
    cc_json_object_set(attrs, "args",
        cc_json_create_string(call && call->arguments_json ? call->arguments_json : ""));
    cc_json_object_set(attrs, "ok", cc_json_create_bool(ok));
    if (reason) {
        cc_json_object_set(attrs, "reason", cc_json_create_string(reason));
    }

    char *attrs_json = cc_json_stringify_unformatted(attrs);
    cc_json_destroy(attrs);
    if (!attrs_json) return;

    cc_observability_event_t event;
    memset(&event, 0, sizeof(event));
    event.size = sizeof(event);
    event.event = event_name;
    event.session_id = session_id;
    event.status = status ? status : "";
    event.message = message;
    event.attributes_json = attrs_json;
    cc_result_t rc = cc_observability_publish(runtime->event_bus, &event);
    cc_result_free(&rc);
    free(attrs_json);
}


/*
 * 填充 policy 拒绝结果。
 *
 * policy 拒绝是工具级可恢复错误，返回给模型后模型可以改用其他路径；因此这里写入
 * out_result->ok=0，而不是让 executor 返回非 OK。
 */
static void set_policy_error_result(
    cc_tool_result_t *out_result,
    const char *message
)
{
    memset(out_result, 0, sizeof(cc_tool_result_t));
    out_result->ok = 0;
    out_result->error = cc_copy_string(message ? message : "Tool call denied by policy");
}

/*
 * 把 schema 校验失败转换成“可恢复工具错误”。
 *
 * 参数错误通常是模型生成的 tool_call 不符合 schema，让模型看到错误文本比中断整个 run
 * 更有用，因为它可以修正参数后重试。
 */
static void set_tool_argument_error_result(
    cc_tool_result_t *out_result,
    const char *message
)
{
    memset(out_result, 0, sizeof(cc_tool_result_t));
    out_result->ok = 0;
    out_result->error = cc_copy_string(message ? message : "Tool arguments failed schema validation");
}

/*
 * 最小 JSON Schema 类型匹配。
 *
 * 只实现工具入参执行前必须依赖的基础类型，不支持完整 JSON Schema，这是核心 SDK 和
 * 业务工具之间的轻量安全边界。
 */
static int schema_type_matches(const cc_json_value_t *value, const char *type)
{
    if (!type || !type[0]) return 1;
    if (strcmp(type, "string") == 0) return cc_json_is_string(value);
    if (strcmp(type, "number") == 0) return cc_json_is_number(value);
    if (strcmp(type, "integer") == 0) return cc_json_is_number(value);
    if (strcmp(type, "boolean") == 0) return cc_json_is_bool(value);
    if (strcmp(type, "object") == 0) return cc_json_is_object(value);
    if (strcmp(type, "array") == 0) return cc_json_is_array(value);
    if (strcmp(type, "null") == 0) return cc_json_is_null(value);
    return 1;
}

/*
 * 校验 JSON Schema type 规格。
 *
 * type 可以是字符串，也可以是字符串数组；数组中任一类型匹配即可。
 */
static int schema_value_matches_type_spec(
    const cc_json_value_t *value,
    const cc_json_value_t *type_spec
)
{
    const char *type = cc_json_string_value(type_spec);
    if (type) return schema_type_matches(value, type);
    if (cc_json_is_array(type_spec)) {
        int count = cc_json_array_size(type_spec);
        for (int i = 0; i < count; i++) {
            if (schema_value_matches_type_spec(value, cc_json_array_get(type_spec, i))) {
                return 1;
            }
        }
        return 0;
    }
    return 1;
}

/*
 * 校验 enum 值。
 *
 * 当前只比较字符串、数字、布尔和 null 等标量；复杂对象枚举不纳入核心最小契约，留给
 * 具体工具实现更严格校验。
 */
static int schema_enum_matches(
    const cc_json_value_t *value,
    const cc_json_value_t *enum_values
)
{
    if (!enum_values || !cc_json_is_array(enum_values)) return 1;
    int count = cc_json_array_size(enum_values);
    const char *actual_string = cc_json_string_value(value);
    for (int i = 0; i < count; i++) {
        cc_json_value_t *candidate = cc_json_array_get(enum_values, i);
        const char *candidate_string = cc_json_string_value(candidate);
        if (actual_string && candidate_string &&
            strcmp(actual_string, candidate_string) == 0) {
            return 1;
        }
        if (cc_json_is_number(value) && cc_json_is_number(candidate) &&
            cc_json_number_value(value) == cc_json_number_value(candidate)) {
            return 1;
        }
        if (cc_json_is_bool(value) && cc_json_is_bool(candidate) &&
            cc_json_bool_value(value) == cc_json_bool_value(candidate)) {
            return 1;
        }
        if (cc_json_is_null(value) && cc_json_is_null(candidate)) {
            return 1;
        }
    }
    return 0;
}

/*
 * 在工具真正执行前做核心层参数校验。
 *
 * 这是安全边界的一部分：provider 生成的 tool_call 不能直接信任，必须先按工具声明的
 * schema 检查必填字段、类型、未知字段和 enum。这里故意只做最小 JSON Schema 子集，
 * 复杂业务规则仍留给具体工具实现。
 */
static cc_result_t validate_tool_arguments_against_schema(
    cc_tool_t *tool,
    const char *arguments_json
)
{
    if (!tool || !tool->vtable || !tool->vtable->schema_json) {
        return cc_result_ok();
    }

    const char *schema_text = tool->vtable->schema_json(tool->self);
    if (!schema_text || !schema_text[0] || strcmp(schema_text, "{}") == 0) {
        return cc_result_ok();
    }

    cc_json_value_t *schema = NULL;
    cc_result_t rc = cc_json_parse(schema_text, &schema);
    if (rc.code != CC_OK) {
        return cc_result_error(CC_ERR_TOOL, "Tool schema JSON is invalid");
    }

    cc_json_value_t *args = NULL;
    rc = cc_json_parse(arguments_json && arguments_json[0] ? arguments_json : "{}", &args);
    if (rc.code != CC_OK) {
        cc_json_destroy(schema);
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_TOOL, "Tool arguments are not valid JSON");
    }

    const char *root_type = cc_json_string_value(cc_json_object_get(schema, "type"));
    if (root_type && strcmp(root_type, "object") == 0 && !cc_json_is_object(args)) {
        cc_json_destroy(schema);
        cc_json_destroy(args);
        return cc_result_error(CC_ERR_TOOL, "Tool arguments must be a JSON object");
    }

    cc_json_value_t *required = cc_json_object_get(schema, "required");
    if (required && cc_json_is_array(required)) {
        int count = cc_json_array_size(required);
        for (int i = 0; i < count; i++) {
            const char *name = cc_json_string_value(cc_json_array_get(required, i));
            if (name && !cc_json_object_get(args, name)) {
                cc_result_t err = cc_result_errf(CC_ERR_TOOL,
                    "Missing required tool argument: %s", name);
                cc_json_destroy(schema);
                cc_json_destroy(args);
                return err;
            }
        }
    }

    cc_json_value_t *properties = cc_json_object_get(schema, "properties");
    if (properties && cc_json_is_object(properties) && cc_json_is_object(args)) {
        int arg_count = cc_json_object_size(args);
        int additional_allowed = 1;
        cc_json_value_t *additional = cc_json_object_get(schema, "additionalProperties");
        if (additional && cc_json_is_bool(additional)) {
            additional_allowed = cc_json_bool_value(additional);
        }
        for (int i = 0; i < arg_count; i++) {
            const char *key = cc_json_object_key_at(args, i);
            cc_json_value_t *value = cc_json_object_value_at(args, i);
            cc_json_value_t *property = key ? cc_json_object_get(properties, key) : NULL;
            if (!property) {
                if (!additional_allowed) {
                    cc_result_t err = cc_result_errf(CC_ERR_TOOL,
                        "Unknown tool argument: %s", key ? key : "(null)");
                    cc_json_destroy(schema);
                    cc_json_destroy(args);
                    return err;
                }
                continue;
            }

            cc_json_value_t *type_spec = cc_json_object_get(property, "type");
            if (type_spec && !schema_value_matches_type_spec(value, type_spec)) {
                cc_result_t err = cc_result_errf(CC_ERR_TOOL,
                    "Tool argument has wrong type: %s", key ? key : "(null)");
                cc_json_destroy(schema);
                cc_json_destroy(args);
                return err;
            }

            cc_json_value_t *enum_values = cc_json_object_get(property, "enum");
            if (!schema_enum_matches(value, enum_values)) {
                cc_result_t err = cc_result_errf(CC_ERR_TOOL,
                    "Tool argument is not an allowed enum value: %s",
                    key ? key : "(null)");
                cc_json_destroy(schema);
                cc_json_destroy(args);
                return err;
            }
        }
    }

    cc_json_destroy(schema);
    cc_json_destroy(args);
    return cc_result_ok();
}

/*
 * 根据工具名映射执行 lane。
 *
 * tool pool 使用 lane 做并发隔离：普通工具按 tool.<name> 分组，plugin/MCP 工具按
 * 前缀分组，避免某个扩展工具阻塞整个 runtime。
 */
static void build_tool_lane_name(const char *tool_name, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) return;
    buf[0] = '\0';
    if (!tool_name || !tool_name[0]) {
        snprintf(buf, buf_size, "tool.unknown");
        return;
    }

    if (strncmp(tool_name, "mcp.", 4) == 0 || strncmp(tool_name, "plugin.", 7) == 0) {
        const char *second_dot = strchr(tool_name + (tool_name[0] == 'm' ? 4 : 7), '.');
        if (second_dot) {
            size_t len = (size_t)(second_dot - tool_name);
            if (len >= buf_size) len = buf_size - 1;
            memcpy(buf, tool_name, len);
            buf[len] = '\0';
            return;
        }
        snprintf(buf, buf_size, "%s", tool_name);
        return;
    }

    snprintf(buf, buf_size, "tool.%s", tool_name);
}


/*
 * 工具执行主入口。
 *
 * 调用顺序是：查 registry -> schema 校验 -> policy/approval -> pool 限流 ->
 * vtable call -> result limit -> observability。每个失败点都尽量转成可恢复
 * cc_tool_result_t，而不是让整个 agent run 直接崩掉。
 */
cc_result_t cc_tool_executor_execute_with_options(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_call_t *call,
    const cc_tool_executor_options_t *options,
    cc_tool_result_t *out_result
)
{
    if (!runtime || !runtime->tool_registry || !session_id || !call ||
        !call->name || !call->name[0] || !call->arguments_json || !out_result) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid tool execution request");
    }
    memset(out_result, 0, sizeof(*out_result));
    cc_media_artifact_list_init(&out_result->artifacts);

    cc_tool_t tool;
#if CC_ENABLE_TOOL_POOL
    cc_tool_executor_pool_ticket_t pool_ticket;
    int pool_acquired = 0;
#endif
    char lane_name[256];
    build_tool_lane_name(call ? call->name : NULL, lane_name, sizeof(lane_name));
    cc_cancel_token_t *cancel_token = options ? options->cancel_token : NULL;
    uint64_t deadline_ms = options ? options->deadline_ms : 0;
    if (cc_cancel_token_is_cancelled(cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "Tool call cancelled before lookup");
    }
    if (deadline_ms > 0 && cc_platform_monotonic_ms() >= deadline_ms) {
        return cc_result_error(CC_ERR_TIMEOUT, "Tool call deadline exceeded before lookup");
    }



    cc_result_t rc = cc_tool_registry_find(
        runtime->tool_registry,
        call->name,
        &tool
    );



    if (rc.code != CC_OK) {
        if (rc.code == CC_ERR_CANCELLED) {
            return rc;
        }
        memset(out_result, 0, sizeof(cc_tool_result_t));
        cc_string_builder_t sb;
        cc_result_t builder_rc = cc_string_builder_init(&sb);
        if (builder_rc.code == CC_OK) {
            cc_result_free(&builder_rc);
            builder_rc = cc_string_builder_appendf(&sb, "Tool not found: %s", call->name);
        }
        out_result->ok = 0;
        if (builder_rc.code == CC_OK) out_result->error = cc_string_builder_take(&sb);
        else cc_string_builder_deinit(&sb);
        cc_result_free(&builder_rc);
        if (!out_result->error) out_result->error = cc_copy_string("Tool not found");
        if (!out_result->error) {
            cc_result_free(&rc);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate tool error");
        }
        publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_ERROR,
            session_id, call, "not_found", 0, out_result->error, NULL);
        cc_result_free(&rc);
        return cc_result_ok();
    }

    rc = validate_tool_arguments_against_schema(&tool, call->arguments_json);
    if (rc.code != CC_OK) {
        set_tool_argument_error_result(out_result, rc.message);
        publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_ERROR,
            session_id, call, "invalid_arguments", 0, rc.message, NULL);
        cc_result_free(&rc);
        return cc_result_ok();
    }



    cc_tool_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.session_id = session_id;
    ctx.workspace_dir = runtime->config.workspace_dir;
    ctx.user_id = NULL;
    ctx.services = &runtime->services;
    ctx.cancel_token = cancel_token;
#if CC_ENABLE_TOOL_POOL
    ctx.timeout_ms = runtime->tool_pool ?
        cc_tool_executor_pool_timeout_ms(runtime->tool_pool, lane_name) : 0;
    if (ctx.timeout_ms <= 0) ctx.timeout_ms = runtime->config.limits.tool_timeout_ms;
#else
    ctx.timeout_ms = runtime->config.limits.tool_timeout_ms;
#endif
    if (deadline_ms > 0) {
        uint64_t now_ms = cc_platform_monotonic_ms();
        if (now_ms >= deadline_ms) {
            return cc_result_error(CC_ERR_TIMEOUT, "Tool call deadline exceeded");
        }
        uint64_t remaining_ms = deadline_ms - now_ms;
        int bounded_ms = remaining_ms > (uint64_t)INT_MAX ? INT_MAX : (int)remaining_ms;
        if (ctx.timeout_ms <= 0 || bounded_ms < ctx.timeout_ms) ctx.timeout_ms = bounded_ms;
    }
    ctx.lane_name = lane_name;
    ctx.generation = 0;



    if (cc_cancel_token_is_cancelled(cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "Tool call cancelled before policy check");
    }
    if (deadline_ms > 0 && cc_platform_monotonic_ms() >= deadline_ms) {
        return cc_result_error(CC_ERR_TIMEOUT, "Tool call deadline exceeded before policy check");
    }

    if (!runtime->policy.vtable || !runtime->policy.vtable->check_tool_call) {
        set_policy_error_result(out_result, "Tool policy engine is unavailable");
        publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_ERROR,
            session_id, call, "policy_unavailable", 0, out_result->error, NULL);
        return cc_result_ok();
    }

    {
        cc_policy_decision_t decision;
        memset(&decision, 0, sizeof(decision));
        rc = runtime->policy.vtable->check_tool_call(
            runtime->policy.self,
            call,
            &ctx,
            &decision
        );

        if (rc.code != CC_OK) {
            const char *reason = rc.message ? rc.message : "Tool policy check failed";
            set_policy_error_result(out_result, reason);
            publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_ERROR,
                session_id, call, "policy_error", 0, out_result->error, reason);
            cc_result_free(&rc);
            cc_policy_decision_free(&decision);
            return cc_result_ok();
        }

        if (!decision.allowed) {
                set_policy_error_result(out_result, decision.reason);
                publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_ERROR,
                    session_id, call, "policy_denied", 0, out_result->error,
                    decision.reason);
                cc_policy_decision_free(&decision);
                return cc_result_ok();
        }

        if (decision.require_approval) {
                const char *reason = decision.reason ?
                    decision.reason : "Tool call requires user approval";
                publish_tool_observability(runtime, CC_OBS_EVENT_APPROVAL_REQUIRED,
                    session_id, call, "required", 0, reason, reason);
                if (!ctx.services || !ctx.services->approve_tool_call) {
                    set_policy_error_result(
                        out_result,
                        "Tool call requires user approval, but no approval handler is registered"
                    );
                    publish_tool_observability(runtime, CC_OBS_EVENT_APPROVAL_DENIED,
                        session_id, call, "denied", 0, out_result->error, reason);
                    cc_policy_decision_free(&decision);
                    return cc_result_ok();
                }

                if (!ctx.services->approve_tool_call(
                        call->name,
                        call->arguments_json,
                        reason,
                        ctx.services->approval_user_data)) {
                    set_policy_error_result(out_result, "Tool call denied by user");
                    publish_tool_observability(runtime, CC_OBS_EVENT_APPROVAL_DENIED,
                        session_id, call, "denied", 0, out_result->error, reason);
                    cc_policy_decision_free(&decision);
                    return cc_result_ok();
                }
                publish_tool_observability(runtime, CC_OBS_EVENT_APPROVAL_APPROVED,
                    session_id, call, "approved", 1, reason, reason);
        }
        cc_policy_decision_free(&decision);
    }



    publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_START,
        session_id, call, "started", 1, NULL, NULL);



#if CC_ENABLE_TOOL_POOL
    if (runtime->tool_pool) {
        rc = cc_tool_executor_pool_acquire_with_deadline(
            runtime->tool_pool,
            lane_name,
            cancel_token,
            deadline_ms,
            &pool_ticket);
        if (rc.code != CC_OK) {
            if (rc.code == CC_ERR_CANCELLED) {
                return rc;
            }
            memset(out_result, 0, sizeof(cc_tool_result_t));
            out_result->ok = 0;
            out_result->error = cc_copy_string(rc.message ? rc.message : "Tool executor pool acquire failed");
            publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_ERROR,
                session_id, call, "pool_failed", 0, out_result->error, NULL);
            cc_result_free(&rc);
            return cc_result_ok();
        }
        pool_acquired = 1;
    }
#endif



    rc = tool.vtable->call(
        tool.self,
        call->arguments_json,
        &ctx,
        out_result
    );
#if CC_ENABLE_TOOL_POOL
    if (pool_acquired) {
        cc_tool_executor_pool_release(runtime->tool_pool, pool_ticket);
        pool_acquired = 0;
    }
#endif

    if (deadline_ms > 0 && cc_platform_monotonic_ms() >= deadline_ms) {
        cc_tool_result_cleanup(out_result);
        return cc_result_error(CC_ERR_TIMEOUT, "Tool call deadline exceeded");
    }



    if (rc.code != CC_OK) {
        if (rc.code == CC_ERR_CANCELLED) {
            cc_tool_result_cleanup(out_result);
            return rc;
        }
        cc_tool_result_cleanup(out_result);
        memset(out_result, 0, sizeof(cc_tool_result_t));
        cc_media_artifact_list_init(&out_result->artifacts);
        out_result->ok = 0;
        out_result->error = cc_copy_string(rc.message ? rc.message : "Tool execution failed");
        publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_ERROR,
            session_id, call, "execution_failed", 0, out_result->error, NULL);
        cc_result_free(&rc);
        return cc_result_ok();
    }

    if (runtime->config.limits.max_tool_result_bytes > 0) {
        const char *tool_output = out_result->ok ? out_result->text : out_result->error;
        if (tool_output &&
            strlen(tool_output) > runtime->config.limits.max_tool_result_bytes) {
            cc_tool_result_cleanup(out_result);
            memset(out_result, 0, sizeof(*out_result));
            out_result->ok = 0;
            out_result->error = cc_copy_string("Tool result exceeds max_tool_result_bytes");
            publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_ERROR,
                session_id, call, "limit_exceeded", 0, out_result->error, NULL);
            return cc_result_ok();
        }
    }

    rc = validate_tool_artifacts(runtime, out_result);
    if (rc.code != CC_OK) {
        const char *reason = rc.message ? rc.message : "Tool artifacts exceed runtime limits";
        char *owned_reason = cc_copy_string(reason);
        cc_result_free(&rc);
        cc_tool_result_cleanup(out_result);
        memset(out_result, 0, sizeof(*out_result));
        cc_media_artifact_list_init(&out_result->artifacts);
        out_result->ok = 0;
        out_result->error = owned_reason ? owned_reason :
            cc_copy_string("Tool artifacts exceed runtime limits");
        publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_ERROR,
            session_id, call, "artifact_limit_exceeded", 0, out_result->error, NULL);
        return cc_result_ok();
    }



    publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_FINISH,
        session_id, call, out_result->ok ? "ok" : "error", out_result->ok,
        out_result->ok ? out_result->text : out_result->error, NULL);

    return cc_result_ok();
}

/* 不带 options 的兼容入口，直接复用完整执行路径。 */
cc_result_t cc_tool_executor_execute(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_call_t *call,
    cc_tool_result_t *out_result
)
{
    return cc_tool_executor_execute_with_options(
        runtime,
        session_id,
        call,
        NULL,
        out_result
    );
}
