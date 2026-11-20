



#include "cc/app/cc_tool_executor.h"
#include "cc/app/cc_tool_executor_pool.h"
#include "cc/core/cc_observability.h"
#include "cc_agent_runtime_internal.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"
#include "cc/util/cc_tool_name.h"
#include "cc/ports/cc_platform.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/**
 * @brief 按运行时多模态配置校验工具产物，再累计检查全部 Base64 正文字节数。
 * @param[in] runtime 借用的有效运行时，限制及 MIME 前缀在调用期间稳定。
 * @param[in] result 借用的工具结果及产物数组，校验保持其拥有关系。
 * @return 成功为 CC_OK，空参数为 CC_ERR_INVALID_ARGUMENT，列表校验错误原样返回，累计溢出或超过 Base64 总预算为 CC_ERR_LIMIT_EXCEEDED。
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

/**
 * @brief 构造工具属性并通过统一观测层尽力发布事件，临时 JSON 在返回前释放。
 * @param[in] runtime 借用的运行时，允许 NULL，事件总线须覆盖同步发布及其订阅者调用。
 * @param[in] event_name 借用的事件名，NULL 或总线不可用时跳过。
 * @param[in] session_id 本次会话归因，同步借用，允许 NULL。
 * @param[in] run_id 本次运行归因，同步借用，允许 NULL。
 * @param[in] step 传给观测层的步骤号。
 * @param[in] operation_id 可选持久化操作 ID，仅非空正文写入属性。
 * @param[in] call 借用的调用身份及参数，允许 NULL，缺失字段写空串。
 * @param[in] status 可选状态正文，NULL 使用空串。
 * @param[in] ok 事件成功标志，按布尔值写入属性。
 * @param[in] message 可选消息，失败且无显式 reason 时也用作 reason。
 * @param[in] reason 可选的显式原因，优先作为 reason 属性。
 * @post 临时属性树及序列化正文已释放，分配或发布失败按观测丢失处理，业务返回值由调用方保管。
 */
static void publish_tool_observability(
    cc_agent_runtime_t *runtime,
    const char *event_name,
    const char *session_id,
    const char *run_id,
    int step,
    const char *operation_id,
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
    if (!cc_json_object_set_owned(attrs, "tool",
            cc_json_create_string(call && call->name ? call->name : "")) ||
        !cc_json_object_set_owned(attrs, "tool_call_id",
            cc_json_create_string(call && call->id ? call->id : "")) ||
        !cc_json_object_set_owned(attrs, "args",
            cc_json_create_string(call && call->arguments_json ? call->arguments_json : "")) ||
        !cc_json_object_set_owned(attrs, "ok", cc_json_create_bool(ok))) {
        cc_json_destroy(attrs);
        return;
    }
    if (operation_id && operation_id[0] &&
        !cc_json_object_set_owned(attrs, "operation_id", cc_json_create_string(operation_id))) {
        cc_json_destroy(attrs);
        return;
    }
    if (reason) {
        if (!cc_json_object_set_owned(attrs, "reason", cc_json_create_string(reason))) {
            cc_json_destroy(attrs);
            return;
        }
    } else if (!ok && message) {
        /* profile 转发保留 attributes，将工具错误连同调用 ID 放入 reason。 */
        if (!cc_json_object_set_owned(attrs, "reason", cc_json_create_string(message))) {
            cc_json_destroy(attrs);
            return;
        }
    }

    char *attrs_json = cc_json_stringify_unformatted(attrs);
    cc_json_destroy(attrs);
    if (!attrs_json) return;

    cc_observability_event_t event;
    memset(&event, 0, sizeof(event));
    event.size = sizeof(event);
    event.event = event_name;
    event.session_id = session_id;
    event.run_id = run_id;
    event.step = step;
    event.status = status ? status : "";
    event.message = message;
    event.attributes_json = attrs_json;
    cc_result_t rc = cc_observability_publish(runtime->event_bus, &event);
    cc_result_free(&rc);
    free(attrs_json);
}


/**
 * @brief 清零结果并复制策略拒绝原因，供调用方表达工具级失败。
 * @param[out] out_result 必填且无待释放内容的独立结果，置 ok 为零。
 * @param[in] message 同步借用的原因，NULL 使用默认拒绝正文。
 * @post error 成功时为结果拥有的副本，分配失败保持 NULL，结果由调用者 cleanup。
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

/**
 * @brief 清零结果并复制参数校验失败原因。
 * @param[out] out_result 必填且无待释放内容的结果，置 ok 为零。
 * @param[in] message 同步借用的校验消息，NULL 使用默认 schema 错误正文。
 * @post error 成功时为独立拥有的副本，分配失败保持 NULL，原校验结果仍由调用者释放。
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

/**
 * @brief 匹配基础 schema 类型，integer 按当前 double 表示检查有限且小数部分为零。
 * @param[in] value 借用的待检查节点，NULL 可参与类型判断。
 * @param[in] type 借用的类型名，NULL、空串及未知类型名沿用放行行为。
 * @return 已知类型按匹配结果返回一或零，缺失或未知类型名返回一；integer 接受负整数及超出 int 范围的有限整数值。
 */
static int schema_type_matches(const cc_json_value_t *value, const char *type)
{
    if (!type || !type[0]) return 1;
    if (strcmp(type, "string") == 0) return cc_json_is_string(value);
    if (strcmp(type, "number") == 0) return cc_json_is_number(value);
    if (strcmp(type, "integer") == 0) {
        if (!cc_json_is_number(value)) return 0;
        double number = cc_json_number_value(value);
        double integer_part;
        return isfinite(number) && modf(number, &integer_part) == 0.0;
    }
    if (strcmp(type, "boolean") == 0) return cc_json_is_bool(value);
    if (strcmp(type, "object") == 0) return cc_json_is_object(value);
    if (strcmp(type, "array") == 0) return cc_json_is_array(value);
    if (strcmp(type, "null") == 0) return cc_json_is_null(value);
    return 1;
}

/**
 * @brief 读取字符串类型声明，或递归检查数组中任一类型声明是否匹配。
 * @param[in] value 同步借用的待检查节点。
 * @param[in] type_spec 借用的类型规格，字符串交给类型匹配，数组递归，其他形式沿用放行行为。
 * @return 任一数组项匹配或非字符串/数组规格为一，空数组或全部不匹配为零。
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

/**
 * @brief 将输入与 enum 数组中的字符串、数字、布尔或 null 标量逐项比较。
 * @param[in] value 同步借用的待检查节点。
 * @param[in] enum_values 借用的候选节点，NULL 或非数组沿用无枚举限制行为。
 * @return 无限制或至少一个标量相同为一，其余为零，数组/对象候选按本子集保持不匹配。
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

/**
 * @brief 在存在实际 schema 时经 cc_json_parse_strict 消费完整正文并校验根对象、required、属性类型及标量 enum。
 *
 * 缺少 schema 回调、返回 NULL/空串或精确的 {} 时直接成功。其余参数空串按 {} 校验，
 * 解析树只在本次拥有并清理，工具最终仍接收原 arguments_json。字段读取沿用 JSON helper 的
 * 忽略大小写查找，additionalProperties=false 只在 properties 和参数均为对象时执行。
 * @param[in] tool 借用的工具及 vtable，schema_json 回调和返回文本在调用期间有效。
 * @param[in] arguments_json 借用的原参数文本，NULL 或空串在校验阶段使用 {}。
 * @return 通过当前子集检查为 CC_OK，schema/参数解析或约束不满足为 CC_ERR_TOOL，所需动态错误消息由返回结果拥有。
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
    cc_result_t rc = cc_json_parse_strict(schema_text, &schema);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_TOOL, "Tool schema JSON is invalid");
    }

    cc_json_value_t *args = NULL;
    rc = cc_json_parse_strict(arguments_json && arguments_json[0] ? arguments_json : "{}", &args);
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

/**
 * @brief 借用已构造的全长 lane，执行实现查找、参数/策略/审批、池准入、意图和结果检查。
 *
 * 本函数借用运行时依赖，外层维持 generation/对象寿命并协调共享工具。executor 全程在本次
 * 同步调用栈执行，pool 票据覆盖意图落盘、开始事件及实际工具调用；同期观测回调或工具嵌套
 * 执行同一受限 lane 时须由外层协调等待关系。审批函数与 user_data 在 runtime mutex 内
 * 成对复制，解锁后调用，旧上下文须由拥有者覆盖已取得快照的调用。
 * 工具进入前 not_dispatched 仍为一。工具错误、非只读业务失败或返回后超时可标 UNKNOWN；
 * 正文/产物超限在调用完成后生成结果错误，外层继续保管实际副作用与持久化结果。
 * 开始事件返回后用同一次时钟读数检查截止并收紧 ctx 预算，策略、审批、池等待和账本耗时计入剩余预算。
 * @param[in,out] runtime 借用的运行时，registry、policy、services、pool、journal 及配置在调用期间有效。
 * @param[in] session_id 借用的会话标识，须非 NULL，账本启用时还须非空。
 * @param[in] call 借用的调用，name 非空、arguments_json 非 NULL，启用账本时 id 须非空。
 * @param[in] options 可选同步借用选项，取消 token 须覆盖本次调用，deadline 为单调绝对毫秒时间。
 * @param[out] out_result 外层已清零并初始化产物列表，拥有的正文/错误/产物由调用者 cleanup，错误时也须清理。
 * @param[out] out_checkpoint 外层已清零并置 not_dispatched 为一，operation_id 副本和持久化收尾由 runtime 调用方处理。
 * @param[in] trusted_tool 可选的手动实现替代，名称须匹配 call，默认按有副作用工具处理，调用期借用。
 * @param[in] run_id 可选的观测归因运行 ID，同步借用。
 * @param[in] step 观测事件的步骤号。
 * @param[in] lane_name 外层拥有的非 NULL 全长 lane，覆盖策略、池查询、实际工具及同步观测调用，ctx 仅借用。
 * @return 多数工具拒绝/业务错误返回 CC_OK 且 ok 为零，仍须检查完整输出；参数、取消、部分超时、账本或分配等错误返回非 OK。
 * 后置超时清理工具输出，账本可能已落盘且动作已执行；错误消息复制失败的部分业务分支可保留 NULL error。
 */
static cc_result_t execute_tool_in_lane(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_call_t *call,
    const cc_tool_executor_options_t *options,
    cc_tool_result_t *out_result,
    cc_tool_dispatch_checkpoint_t *out_checkpoint,
    const cc_tool_t *trusted_tool,
    const char *run_id,
    int step,
    const char *lane_name
)
{
    cc_tool_t tool;
    unsigned tool_capabilities = 0;
#if CC_ENABLE_TOOL_POOL
    cc_tool_executor_pool_ticket_t pool_ticket;
    int pool_acquired = 0;
#endif
    cc_cancel_token_t *cancel_token = options ? options->cancel_token : NULL;
    uint64_t deadline_ms = options ? options->deadline_ms : 0;
    cc_result_t rc = cc_result_ok();
    if (trusted_tool) {
        if (!trusted_tool->vtable || !trusted_tool->vtable->name || !trusted_tool->vtable->call)
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid trusted manual tool");
        const char *name = trusted_tool->vtable->name(trusted_tool->self);
        if (!name || strcmp(name, call->name) != 0)
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Manual tool name does not match the call");
        tool = *trusted_tool;
    } else rc = cc_tool_registry_find_with_capabilities(runtime->tool_registry, call->name, &tool,
                                                        &tool_capabilities);



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
            session_id, run_id, step, out_checkpoint->operation_id, call, "not_found", 0, out_result->error, NULL);
        cc_result_free(&rc);
        return cc_result_ok();
    }

    if (!tool.vtable || !tool.vtable->call)
        return cc_result_error(CC_ERR_TOOL, "Tool implementation is unavailable");

    rc = validate_tool_arguments_against_schema(&tool, call->arguments_json);
    if (rc.code != CC_OK) {
        set_tool_argument_error_result(out_result, rc.message);
        publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_ERROR,
            session_id, run_id, step, out_checkpoint->operation_id, call, "invalid_arguments", 0, rc.message, NULL);
        cc_result_free(&rc);
        return cc_result_ok();
    }
    cc_result_free(&rc);
    cc_tool_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.session_id = session_id;
    ctx.workspace_dir = runtime->config.workspace_dir;
    ctx.user_id = NULL;
    ctx.services = &runtime->services;
    ctx.cancel_token = cancel_token;
    ctx.max_result_bytes = runtime->config.limits.max_tool_result_bytes;
    ctx.idempotency_key = call->id;
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
            session_id, run_id, step, out_checkpoint->operation_id, call, "policy_unavailable", 0, out_result->error, NULL);
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
                session_id, run_id, step, out_checkpoint->operation_id, call, "policy_error", 0, out_result->error, reason);
            cc_result_free(&rc);
            cc_policy_decision_free(&decision);
            return cc_result_ok();
        }
        cc_result_free(&rc);
        if (!decision.allowed) {
                set_policy_error_result(out_result, decision.reason);
                publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_ERROR,
                    session_id, run_id, step, out_checkpoint->operation_id, call, "policy_denied", 0, out_result->error,
                    decision.reason);
                cc_policy_decision_free(&decision);
                return cc_result_ok();
        }

        if (decision.require_approval) {
                const char *reason = decision.reason ?
                    decision.reason : "Tool call requires user approval";
                publish_tool_observability(runtime, CC_OBS_EVENT_APPROVAL_REQUIRED,
                    session_id, run_id, step, out_checkpoint->operation_id, call, "required", 0, reason, reason);
                cc_mutex_lock(runtime->mutex);
                cc_tool_approval_fn approval_callback = runtime->services.approve_tool_call;
                void *approval_user_data = runtime->services.approval_user_data;
                cc_mutex_unlock(runtime->mutex);
                if (!approval_callback) {
                    set_policy_error_result(
                        out_result,
                        "Tool call requires user approval, but no approval handler is registered"
                    );
                    publish_tool_observability(runtime, CC_OBS_EVENT_APPROVAL_DENIED,
                        session_id, run_id, step, out_checkpoint->operation_id, call, "denied", 0, out_result->error, reason);
                    cc_policy_decision_free(&decision);
                    return cc_result_ok();
                }

                if (!approval_callback(
                        call->name,
                        call->arguments_json,
                        reason,
                        approval_user_data)) {
                    set_policy_error_result(out_result, "Tool call denied by user");
                    publish_tool_observability(runtime, CC_OBS_EVENT_APPROVAL_DENIED,
                        session_id, run_id, step, out_checkpoint->operation_id, call, "denied", 0, out_result->error, reason);
                    cc_policy_decision_free(&decision);
                    return cc_result_ok();
                }
                publish_tool_observability(runtime, CC_OBS_EVENT_APPROVAL_APPROVED,
                    session_id, run_id, step, out_checkpoint->operation_id, call, "approved", 1, reason, reason);
        }
        cc_policy_decision_free(&decision);
    }



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
                session_id, run_id, step, out_checkpoint->operation_id, call, "pool_failed", 0, out_result->error, NULL);
            cc_result_free(&rc);
            return cc_result_ok();
        }
        cc_result_free(&rc);
        pool_acquired = 1;
    }
#endif



    if (runtime->operation_journal) {
        rc = (tool_capabilities & CC_TOOL_CAPABILITY_READ_ONLY) ?
            cc_operation_journal_begin_readonly(runtime->operation_journal, session_id,
                                                call, &out_checkpoint->operation_id) :
            cc_operation_journal_begin(runtime->operation_journal, session_id,
                                       call, &out_checkpoint->operation_id);
        if (rc.code != CC_OK) {
#if CC_ENABLE_TOOL_POOL
            if (pool_acquired) cc_tool_executor_pool_release(runtime->tool_pool, pool_ticket);
#endif
            if (rc.code == CC_ERR_INVALID_STATE || rc.code == CC_ERR_LIMIT_EXCEEDED) {
                out_result->ok = 0;
                out_result->error = cc_copy_string(rc.message ? rc.message : "Tool operation is blocked");
                if (!out_result->error) {
                    cc_result_free(&rc);
                    return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate tool block reason");
                }
                publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_ERROR,
                    session_id, run_id, step, out_checkpoint->operation_id, call, "recovery_blocked", 0, out_result->error, NULL);
                cc_result_free(&rc);
                return cc_result_ok();
            }
            return rc;
        }
        cc_result_free(&rc);
        ctx.idempotency_key = out_checkpoint->operation_id;
    }

    publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_START,
        session_id, run_id, step, out_checkpoint->operation_id, call, "started", 1, NULL, NULL);
    /* 同一时钟读数同时决定派发门禁与剩余预算，前序回调和等待耗时全部扣除。 */
    uint64_t dispatch_now_ms = deadline_ms > 0 ? cc_platform_monotonic_ms() : 0;
    cc_error_code_t interrupted = cc_cancel_token_is_cancelled(cancel_token) ? CC_ERR_CANCELLED :
        (deadline_ms > 0 && dispatch_now_ms >= deadline_ms ? CC_ERR_TIMEOUT : CC_OK);
    if (interrupted != CC_OK) {
#if CC_ENABLE_TOOL_POOL
        if (pool_acquired) cc_tool_executor_pool_release(runtime->tool_pool, pool_ticket);
#endif
        /* runtime 调用方先持久化已知未执行结果，再清理意图账本。 */
        const char *reason = interrupted == CC_ERR_CANCELLED ?
            "Tool call cancelled before dispatch" : "Tool call deadline exceeded before dispatch";
        publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_ERROR,
            session_id, run_id, step, out_checkpoint->operation_id, call, "not_dispatched", 0, reason, NULL);
        return cc_result_error(interrupted, reason);
    }
    if (deadline_ms > 0) {
        uint64_t remaining_ms = deadline_ms - dispatch_now_ms;
        int bounded_ms = remaining_ms > (uint64_t)INT_MAX ? INT_MAX : (int)remaining_ms;
        if (ctx.timeout_ms <= 0 || bounded_ms < ctx.timeout_ms) ctx.timeout_ms = bounded_ms;
    }
    out_checkpoint->not_dispatched = 0;
    rc = tool.vtable->call(
        tool.self,
        call->arguments_json,
        &ctx,
        out_result
    );
    if (out_checkpoint->operation_id &&
        (rc.code != CC_OK ||
         (!out_result->ok && !(tool_capabilities & CC_TOOL_CAPABILITY_READ_ONLY)))) {
        out_checkpoint->outcome_unknown = 1;
        cc_result_t mark_rc = cc_operation_journal_mark_unknown(
            runtime->operation_journal, out_checkpoint->operation_id);
        cc_result_free(&mark_rc);
    }
#if CC_ENABLE_TOOL_POOL
    if (pool_acquired) {
        cc_tool_executor_pool_release(runtime->tool_pool, pool_ticket);
        pool_acquired = 0;
    }
#endif

    if (deadline_ms > 0 && cc_platform_monotonic_ms() >= deadline_ms) {
        if (out_checkpoint->operation_id) {
            out_checkpoint->outcome_unknown = 1;
            cc_result_t mark_rc = cc_operation_journal_mark_unknown(
                runtime->operation_journal, out_checkpoint->operation_id);
            cc_result_free(&mark_rc);
        }
        cc_tool_result_cleanup(out_result);
        cc_result_free(&rc);
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
            session_id, run_id, step, out_checkpoint->operation_id, call, "execution_failed", 0, out_result->error, NULL);
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
            out_result->error = cc_copy_string(
                "Tool completed, but its result exceeds max_tool_result_bytes; "
                "do not repeat the action solely because its output is unavailable");
            publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_ERROR,
                session_id, run_id, step, out_checkpoint->operation_id, call, "limit_exceeded", 0, out_result->error, NULL);
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
            cc_copy_string("Tool completed, but its artifacts exceed runtime limits");
        publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_ERROR,
            session_id, run_id, step, out_checkpoint->operation_id, call, "artifact_limit_exceeded", 0, out_result->error, NULL);
        return cc_result_ok();
    }



    publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_FINISH,
        session_id, run_id, step, out_checkpoint->operation_id, call, out_result->ok ? "ok" : "error", out_result->ok,
        out_result->ok ? out_result->text : out_result->error, NULL);

    return cc_result_ok();
}

/**
 * @brief 保留入口检查和恢复门禁优先级，拥有全长 lane 并统一在执行返回后释放。
 * @param[in,out] runtime 借用有效运行时，依赖与 generation 寿命覆盖同步执行和回调。
 * @param[in] session_id 必需借用会话标识，账本启用时进一步要求非空。
 * @param[in] call 必需借用调用，name 非空、arguments_json 非 NULL，账本启用时 id 非空。
 * @param[in] options 可选借用取消与截止选项，token 覆盖执行期。
 * @param[out] out_result 必需无已有资源输出，基础参数通过后清零并初始化，所有返回路径由调用者 cleanup。
 * @param[out] out_checkpoint 必需无已有资源输出，基础参数通过后清零并置 not_dispatched，拥有 operation_id 由调用者 free 并完成账本收尾。
 * @param[in] trusted_tool 可选借用手动实现，名称匹配与元数据检查沿内部执行器。
 * @param[in] run_id 可选借用观测运行 ID。
 * @param[in] step 观测步骤号。
 * @return 参数、取消、截止及恢复门禁先于 lane 分配，名称构造失败返回对应错误，其他结果沿内部执行器，CC_OK 后仍需检查 out_result->ok。
 * @note 普通工具使用 tool.全名，plugin/MCP 保留完整已编码来源前缀。lane 只在本次执行期借给策略、工具上下文和池。
 */
cc_result_t cc_tool_executor_execute_guarded(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_call_t *call,
    const cc_tool_executor_options_t *options,
    cc_tool_result_t *out_result,
    cc_tool_dispatch_checkpoint_t *out_checkpoint,
    const cc_tool_t *trusted_tool,
    const char *run_id,
    int step)
{
    if (!runtime || !runtime->tool_registry || !session_id || !call ||
        !call->name || !call->name[0] || !call->arguments_json || !out_result ||
        !out_checkpoint) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid tool execution request");
    }
    memset(out_checkpoint, 0, sizeof(*out_checkpoint));
    out_checkpoint->not_dispatched = 1;
    memset(out_result, 0, sizeof(*out_result));
    cc_media_artifact_list_init(&out_result->artifacts);
    cc_cancel_token_t *cancel_token = options ? options->cancel_token : NULL;
    uint64_t deadline_ms = options ? options->deadline_ms : 0;
    if (cc_cancel_token_is_cancelled(cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "Tool call cancelled before lookup");
    }
    if (deadline_ms > 0 && cc_platform_monotonic_ms() >= deadline_ms) {
        return cc_result_error(CC_ERR_TIMEOUT, "Tool call deadline exceeded before lookup");
    }
    /* 恢复门禁仍先于元数据、策略、审批及新增的 lane 分配。 */
    if (cc_agent_runtime_recovery_blocks_tools(runtime)) {
        out_result->ok = 0;
        out_result->error = cc_copy_string(
            "Tool execution is unavailable because operation recovery storage is not ready");
        publish_tool_observability(runtime, CC_OBS_EVENT_TOOL_ERROR,
            session_id, run_id, step, out_checkpoint->operation_id, call, "recovery_unavailable", 0, out_result->error, NULL);
        return out_result->error ? cc_result_ok() :
            cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate tool block reason");
    }

    char *lane_name = NULL;
    cc_result_t rc = cc_tool_name_lane(call->name, &lane_name);
    if (rc.code != CC_OK) return rc;
    rc = execute_tool_in_lane(runtime, session_id, call, options, out_result,
        out_checkpoint, trusted_tool, run_id, step, lane_name);
    free(lane_name);
    return rc;
}

/**
 * @brief 提供无操作账本的直接执行入口，带取消及绝对截止选项。
 * @param[in,out] runtime 借用的存活运行时，已配置或要求操作账本时拒绝直接执行。
 * @param[in] session_id 借用的非 NULL 会话 ID。
 * @param[in] call 借用的调用，name 非空、arguments_json 非 NULL，工具 self 由 registry 拥有。
 * @param[in] options 可选的同步借用选项及 token。
 * @param[out] out_result 必填且无待释放内容，进入 guarded 且参数有效后清零，成功或中途错误均清理其中拥有字段；入口拒绝时保持原值。
 * @return 需要账本时为 CC_ERR_UNSUPPORTED，其余返回 guarded 结果，CC_OK 时仍须检查 out_result->ok。
 */
cc_result_t cc_tool_executor_execute_with_options(
    cc_agent_runtime_t *runtime, const char *session_id, const cc_tool_call_t *call,
    const cc_tool_executor_options_t *options, cc_tool_result_t *out_result)
{
    /* 直接入口缺少持久化结果的运行步骤，启用安全账本时转由 runtime 组合入口执行。 */
    if (runtime && (runtime->operation_journal || runtime->require_operation_journal))
        return cc_result_error(CC_ERR_UNSUPPORTED,
            "Use a journaled agent runtime step for tool execution");
    cc_tool_dispatch_checkpoint_t checkpoint = {0};
    return cc_tool_executor_execute_guarded(runtime, session_id, call, options,
                                            out_result, &checkpoint, NULL, NULL, 0);
}

/**
 * @brief 省略取消及截止选项，复用直接执行入口。
 * @param[in,out] runtime 借用的运行时及同步调用依赖。
 * @param[in] session_id 借用的非 NULL 会话 ID。
 * @param[in] call 借用的工具调用及其字符串，name 非空、arguments_json 非 NULL。
 * @param[out] out_result 无待释放内容的输出，初始化及清理契约沿用带选项入口。
 * @return 原样返回带选项入口的结果，返回 CC_OK 后继续检查业务 ok 标志。
 */
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
