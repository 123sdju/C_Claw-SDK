#include "cc/ports/cc_event_bus.h"
#include "cc/util/cc_redaction.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct capture {
    char payload[256];
} capture_t;

/* 捕获 event bus 发布后的 payload，用来验证发布路径也会脱敏。 */
static void on_event(const char *event_type, const char *event_json, void *user_data)
{
    (void)event_type;
    capture_t *capture = (capture_t *)user_data;
    snprintf(capture->payload, sizeof(capture->payload), "%s", event_json ? event_json : "");
}

/*
 * 验证 JSON-aware redaction 和 fallback 文本扫描。
 *
 * 覆盖 object、嵌套 object、array、大小写 key、非字符串 value，以及 event bus payload
 * 发布前统一脱敏的契约。
 */
int main(void)
{
    int failed = 0;
    char *redacted = cc_redact_secrets("{\"api_key\":\"sk-test\",\"password\":\"pw\",\"ok\":1}");
    if (!redacted || strstr(redacted, "sk-test") || strstr(redacted, "pw")) failed = 1;
    free(redacted);

    redacted = cc_redact_secrets(
        "{\"outer\":{\"Token\":\"nested-secret\"},\"items\":[{\"passwd\":\"array-secret\"}],\"secret\":123}");
    if (!redacted) failed = 1;
    if (redacted && (strstr(redacted, "nested-secret") ||
        strstr(redacted, "array-secret") ||
        strstr(redacted, "\"secret\":123"))) {
        failed = 1;
    }
    if (redacted && strstr(redacted, "[REDACTED]") == NULL) failed = 1;
    free(redacted);

    redacted = cc_redact_secrets("token=fallback-token");
    if (!redacted || strstr(redacted, "fallback-token")) failed = 1;
    free(redacted);

    cc_event_bus_t *bus = NULL;
    cc_result_t rc = cc_event_bus_create(&bus);
    if (rc.code != CC_OK) return 1;
    capture_t capture;
    memset(&capture, 0, sizeof(capture));
    cc_event_subscription_t subscription = {0};
    rc = cc_event_bus_subscribe(bus, "test.secret", on_event, &capture, NULL, &subscription);
    if (rc.code != CC_OK) failed = 1;
    rc = cc_event_bus_publish(bus, "test.secret",
        "{\"authorization\":\"Bearer abc\",\"token\":\"xyz\"}");
    if (rc.code != CC_OK) failed = 1;
    if (strstr(capture.payload, "Bearer abc") || strstr(capture.payload, "xyz")) failed = 1;
    cc_event_bus_destroy(bus);
    return failed ? 1 : 0;
}
