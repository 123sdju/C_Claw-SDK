#include "cc/core/cc_observability.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>

/*
 * 只有在字段存在时才写入 string。
 *
 * 可选字段如果被写成空字符串，下游很难区分“调用方明确给了空值”和“字段不存在”。
 * 这里选择省略 NULL，保持 payload 更接近真实语义。
 */
static void json_object_set_string_if(
    cc_json_value_t *obj,
    const char *key,
    const char *value
)
{
    if (!obj || !key || !value) return;
    cc_json_object_set(obj, key, cc_json_create_string(value));
}

/*
 * 写入 schema 必需的字符串字段。
 *
 * session_id/run_id/status 这类关联字段即使调用方没有提供，也输出空字符串；这样
 * 日志管道和调试 UI 可以按固定 schema 解析，不需要检查字段是否存在。
 */
static void json_object_set_string_required(
    cc_json_value_t *obj,
    const char *key,
    const char *value
)
{
    if (!obj || !key) return;
    cc_json_object_set(obj, key, cc_json_create_string(value ? value : ""));
}

/*
 * 解析 attributes_json。
 *
 * attributes 是业务侧补充字段，但必须保持 object 形态，不能是裸字符串或数组。
 * 这样下游按 JSON object 合并、筛选、索引时不会遇到类型漂移。
 */
static cc_json_value_t *parse_attributes_object(const char *attributes_json)
{
    if (!attributes_json || !attributes_json[0]) return NULL;
    cc_json_value_t *attrs = NULL;
    cc_result_t rc = cc_json_parse(attributes_json, &attrs);
    if (rc.code != CC_OK || !attrs || !cc_json_is_object(attrs)) {
        cc_result_free(&rc);
        if (attrs) cc_json_destroy(attrs);
        return NULL;
    }
    return attrs;
}

/*
 * 构造统一 observability payload。
 *
 * 函数先验证 attributes，再创建固定 schema 字段，最后可选嵌入 error/detail 和
 * attributes。这里不直接 redaction，事件发布到 bus 时会经过 event bus 的统一脱敏，
 * 避免每个业务路径重复维护敏感字段规则。
 */
char *cc_observability_event_json(const cc_observability_event_t *event)
{
    if (!event || !event->event) return NULL;

    cc_json_value_t *attrs = parse_attributes_object(event->attributes_json);
    if (event->attributes_json && event->attributes_json[0] && !attrs) return NULL;

    cc_json_value_t *root = cc_json_create_object();
    if (!root) {
        if (attrs) cc_json_destroy(attrs);
        return NULL;
    }

    cc_json_object_set(root, "schema_version",
        cc_json_create_number(CC_OBSERVABILITY_SCHEMA_VERSION));
    json_object_set_string_required(root, "event", event->event);
    json_object_set_string_required(root, "session_id", event->session_id);
    json_object_set_string_required(root, "run_id", event->run_id);
    cc_json_object_set(root, "step", cc_json_create_number(event->step));
    json_object_set_string_required(root, "status", event->status);
    json_object_set_string_if(root, "message", event->message);

    if (event->error && event->error->code != CC_OK) {
        cc_json_value_t *err = cc_json_create_object();
        if (err) {
            cc_json_object_set(err, "code",
                cc_json_create_string(cc_error_code_name(event->error->code)));
            json_object_set_string_if(err, "message", event->error->message);
            if (event->error->detail) {
                cc_json_object_set(err, "http_status",
                    cc_json_create_number((double)event->error->detail->http_status));
                cc_json_object_set(err, "retry_after_ms",
                    cc_json_create_number((double)event->error->detail->retry_after_ms));
                cc_json_object_set(err, "recoverable",
                    cc_json_create_bool(event->error->detail->recoverable));
                json_object_set_string_if(err, "provider_error_code",
                    event->error->detail->provider_error_code);
                json_object_set_string_if(err, "raw_redacted_body",
                    event->error->detail->raw_redacted_body);
            }
            cc_json_object_set(root, "error", err);
        }
    }
    if (attrs) {
        cc_json_object_set(root, "attributes", attrs);
        attrs = NULL;
    }

    char *json = cc_json_stringify_unformatted(root);
    cc_json_destroy(root);
    if (attrs) cc_json_destroy(attrs);
    return json;
}

/*
 * 发布统一观测事件。
 *
 * 这是 runtime/tool/provider 路径唯一应该调用的观测发布入口；底层 event bus 仍然
 * 是订阅分发机制，但业务代码不再手写 payload，降低事件名和字段漂移风险。
 */
cc_result_t cc_observability_publish(
    cc_event_bus_t *bus,
    const cc_observability_event_t *event
)
{
    if (!bus || !event || !event->event) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid observability event");
    }

    char *payload = cc_observability_event_json(event);
    if (!payload) {
        return cc_result_error(CC_ERR_JSON, "Failed to build observability event");
    }
    cc_result_t rc = cc_event_bus_publish(bus, event->event, payload);
    free(payload);
    return rc;
}
