#include "cc/core/cc_observability.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct capture {
    char payload[512];
    int seen;
} capture_t;

/**
 * @brief 在同步订阅回调内复制总线观测载荷。
 * @param[in] event_type 调用期借用的事件名，当前断言忽略。
 * @param[in] event_json 调用期借用的 JSON，NULL 按空串处理。
 * @param[in,out] user_data 借用的 capture 记录，复制到固定缓冲并递增 seen。
 * @post 本地记录在回调返回后仍可读，超长正文按 snprintf 的固定容量截断。
 */
static void on_event(const char *event_type, const char *event_json, void *user_data)
{
    (void)event_type;
    capture_t *capture = (capture_t *)user_data;
    snprintf(capture->payload, sizeof(capture->payload), "%s", event_json ? event_json : "");
    capture->seen++;
}

/**
 * @brief 检查统一观测字段、错误详情、属性对象、总线脱敏及事件族名称。
 * @return 全部断言通过为零，任一失败为一，返回前释放载荷、结果和总线资源。
 */
int main(void)
{
    cc_error_detail_t detail;
    memset(&detail, 0, sizeof(detail));
    detail.size = sizeof(detail);
    detail.http_status = 500;
    detail.recoverable = 1;
    detail.raw_redacted_body = "{\"authorization\":\"[REDACTED]\"}";
    cc_result_t err = cc_result_with_detail(CC_ERR_NETWORK, "provider failed", &detail);

    cc_observability_event_t event;
    memset(&event, 0, sizeof(event));
    event.size = sizeof(event);
    event.event = CC_OBS_EVENT_LLM_RESPONSE_FINISH;
    event.session_id = "s1";
    event.run_id = "r1";
    event.step = 2;
    event.status = "error";
    event.error = &err;
    event.message = "provider failed";
    event.attributes_json = "{\"provider\":\"fake\",\"attempt\":1}";

    char *json = cc_observability_event_json(&event);
    int failed = 0;
    if (!json) failed = 1;
    if (json && !strstr(json, "\"schema_version\":1")) failed = 1;
    if (json && !strstr(json, "\"event\":\"llm.response.finish\"")) failed = 1;
    if (json && !strstr(json, "\"session_id\":\"s1\"")) failed = 1;
    if (json && !strstr(json, "\"run_id\":\"r1\"")) failed = 1;
    if (json && !strstr(json, "\"status\":\"error\"")) failed = 1;
    if (json && !strstr(json, "\"code\":\"CC_ERR_NETWORK\"")) failed = 1;
    if (json && !strstr(json, "\"attributes\":{\"provider\":\"fake\",\"attempt\":1}")) failed = 1;
    free(json);

    event.attributes_json = "\"not-object\"";
    json = cc_observability_event_json(&event);
    if (json) failed = 1;
    free(json);
    event.attributes_json = "{\"authorization\":\"Bearer hidden\"}";

    cc_event_bus_t *bus = NULL;
    cc_result_t rc = cc_event_bus_create(&bus);
    if (rc.code != CC_OK) failed = 1;
    cc_result_free(&rc);
    capture_t capture;
    memset(&capture, 0, sizeof(capture));
    if (!failed) {
        cc_event_subscription_t subscription = {0};
        rc = cc_event_bus_subscribe(bus, CC_OBS_EVENT_LLM_RESPONSE_FINISH, on_event, &capture,
            NULL, &subscription);
        if (rc.code != CC_OK) failed = 1;
        cc_result_free(&rc);
        rc = cc_observability_publish(bus, &event);
        if (rc.code != CC_OK) failed = 1;
        cc_result_free(&rc);
        if (capture.seen != 1) failed = 1;
        if (strstr(capture.payload, "Bearer hidden")) failed = 1;
        if (!strstr(capture.payload, "[REDACTED]")) failed = 1;
    }

    const char *families[] = {
        CC_OBS_EVENT_RUN_FINISHED,
        CC_OBS_EVENT_LLM_REQUEST_START,
        CC_OBS_EVENT_TOOL_START,
        CC_OBS_EVENT_APPROVAL_REQUIRED,
        CC_OBS_EVENT_MEMORY_QUERY,
        CC_OBS_EVENT_STREAM_TEXT,
        CC_OBS_EVENT_ERROR_RUNTIME
    };
    for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); i++) {
        event.event = families[i];
        event.status = "ok";
        event.error = NULL;
        event.attributes_json = "{}";
        json = cc_observability_event_json(&event);
        if (!json || !strstr(json, families[i])) failed = 1;
        free(json);
    }
    cc_event_bus_destroy(bus);
    cc_result_free(&err);
    return failed ? 1 : 0;
}
