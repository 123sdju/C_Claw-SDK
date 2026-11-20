#include "cc/core/cc_observability.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_json_syntax_check.h"

#include <stdlib.h>

/**
 * @brief 为非 NULL 可选字符串创建并挂接完整 JSON 字段。
 * @param[in,out] obj 借用的目标对象树，须由调用者独占构造。
 * @param[in] key 调用期间借用的字段名。
 * @param[in] value 调用期间借用的可选正文，NULL 时省略字段。
 * @return 成功或省略为一，参数、分配或挂接失败为零，失败节点由拥有 setter 消费。
 */
static int json_object_set_string_if(
    cc_json_value_t *obj,
    const char *key,
    const char *value
)
{
    if (!obj || !key) return 0;
    return !value || cc_json_object_set_owned(obj, key, cc_json_create_string(value));
}

/**
 * @brief 创建并挂接必需的字符串字段，NULL 正文写为空串。
 * @param[in,out] obj 独占构造期的对象树借用。
 * @param[in] key 调用期借用的必需字段名。
 * @param[in] value 调用期借用的可选正文。
 * @return 字段完整挂接为一，参数、分配或挂接失败为零，临时节点沿 setter 消费。
 */
static int json_object_set_string_required(
    cc_json_value_t *obj,
    const char *key,
    const char *value
)
{
    return obj && key && cc_json_object_set_owned(obj, key, cc_json_create_string(value ? value : ""));
}

/**
 * @brief 完整解析可选的业务属性对象。
 * @param[in] attributes_json 调用期借用的 JSON 文本，NULL 或空串表示省略。
 * @return 成功返回独占对象树，省略、坏文本、解码 NUL、类型或分配失败返回 NULL。
 * @note 解析结果诊断在本层消费，调用者根据输入是否省略区分空结果并最终销毁树。
 */
static cc_json_value_t *parse_attributes_object(const char *attributes_json)
{
    if (!attributes_json || !attributes_json[0]) return NULL;
    if (cc_storage_json_has_nul_escape(attributes_json)) return NULL;
    cc_json_value_t *attrs = NULL;
    cc_result_t rc = cc_json_parse_strict(attributes_json, &attrs);
    int valid = rc.code == CC_OK && attrs && cc_json_is_object(attrs);
    cc_result_free(&rc);
    if (!valid) {
        if (attrs) cc_json_destroy(attrs);
        return NULL;
    }
    return attrs;
}

/**
 * @brief 构造包含全部应有字段的统一观测事件 JSON。
 * @param[in] event 调用期间稳定的完整布局描述及其嵌套字段借用，event 名称须非 NULL。
 * @return 成功交付独占字符串并最终 free，参数、属性或任一构造分配失败返回 NULL。
 * @note 字符串复制到临时树，失败撤销全部部分节点。此入口保留原文，publish 通过总线执行脱敏。
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

    int good = cc_json_object_set_owned(root, "schema_version",
        cc_json_create_number(CC_OBSERVABILITY_SCHEMA_VERSION)) &&
        json_object_set_string_required(root, "event", event->event) &&
        json_object_set_string_required(root, "session_id", event->session_id) &&
        json_object_set_string_required(root, "run_id", event->run_id) &&
        cc_json_object_set_owned(root, "step", cc_json_create_number(event->step)) &&
        json_object_set_string_required(root, "status", event->status) &&
        json_object_set_string_if(root, "message", event->message);

    if (good && event->error && event->error->code != CC_OK) {
        cc_json_value_t *err = cc_json_create_object();
        good = err && cc_json_object_set_owned(err, "code",
            cc_json_create_string(cc_error_code_name(event->error->code))) &&
            json_object_set_string_if(err, "message", event->error->message);
        if (good && event->error->detail) {
            good = cc_json_object_set_owned(err, "http_status",
                cc_json_create_number((double)event->error->detail->http_status)) &&
                cc_json_object_set_owned(err, "retry_after_ms",
                cc_json_create_number((double)event->error->detail->retry_after_ms)) &&
                cc_json_object_set_owned(err, "recoverable",
                cc_json_create_bool(event->error->detail->recoverable)) &&
                json_object_set_string_if(err, "provider_error_code",
                    event->error->detail->provider_error_code) &&
                json_object_set_string_if(err, "raw_redacted_body",
                    event->error->detail->raw_redacted_body);
        }
        if (good) good = cc_json_object_set_owned(root, "error", err);
        else cc_json_destroy(err);
    }
    if (good && attrs) {
        good = cc_json_object_set_owned(root, "attributes", attrs);
        attrs = NULL;
    }

    char *json = good ? cc_json_stringify_unformatted(root) : NULL;
    cc_json_destroy(root);
    if (attrs) cc_json_destroy(attrs);
    return json;
}

/**
 * @brief 构造完整观测载荷并交给事件总线分发。
 * @param[in,out] bus 调用期间存活的总线借用，拥有者覆盖本次发布及回调寿命。
 * @param[in] event 调用期间稳定的完整事件描述及其嵌套借用。
 * @return 参数错误为 INVALID_ARGUMENT，构造失败为 JSON，其余转交总线拥有结果，调用者释放。
 * @note 临时载荷在发布返回后释放，总线按自身契约保管异步副本并脱敏，业务方单独处理观测失败。
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
