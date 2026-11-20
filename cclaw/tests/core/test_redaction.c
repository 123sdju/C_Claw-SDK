#include "cc/ports/cc_event_bus.h"
#include "cc/util/cc_redaction.h"
#include "cJSON.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct capture {
    char payload[256];
} capture_t;

/**
 * @brief 复制事件总线交付的正文，检查发布路径的脱敏结果。
 * @param[in] event_type 本次主题借用，测试不使用。
 * @param[in] event_json 本次正文借用，NULL 按空串。
 * @param[in,out] user_data 有效 capture_t 借用，回调复制到其有界缓冲。
 * @post 正文被截断为最多 255 字节并 NUL 结尾，fixture 在总线销毁前存活。
 */
static void on_event(const char *event_type, const char *event_json, void *user_data)
{
    (void)event_type;
    capture_t *capture = (capture_t *)user_data;
    snprintf(capture->payload, sizeof(capture->payload), "%s", event_json ? event_json : "");
}

static size_t allocation_number;
static size_t fail_allocation;

/**
 * @brief 为独立串行场景在指定 cJSON 分配序号注入失败。
 * @param[in] size 请求字节数。
 * @return 指定序号返回 NULL，其他返回与 free 匹配的拥有缓冲。
 */
static void *redaction_test_allocate(size_t size)
{
    allocation_number++;
    if (allocation_number == fail_allocation) return NULL;
    return malloc(size);
}

/**
 * @brief 枚举结构化脱敏分配失败，核对转义键及嵌套 JSON 不能退回原值。
 * @return 全部结果为脱敏副本或 NULL 时为零，出现原始值或循环未结束为一。
 * @note 钩子只在创建事件总线前的单线程窗口安装，返回前恢复默认钩子。
 */
static int test_redaction_allocation_failures(void)
{
    const char *inputs[] = {
        "{\"\\u0061pi_key\":\"hidden-credential\",\"ok\":1}",
        "{\"nested\":\"{\\\"\\\\u0061pi_key\\\":\\\"hidden-credential\\\"}\"}"
    };
    cJSON_Hooks hooks = {redaction_test_allocate, free};
    int failed = 0;
    for (size_t input = 0; input < sizeof(inputs) / sizeof(inputs[0]); input++) {
        size_t fail_at;
        for (fail_at = 1; fail_at < 256; fail_at++) {
            allocation_number = 0;
            fail_allocation = fail_at;
            cJSON_InitHooks(&hooks);
            char *redacted = cc_redact_secrets(inputs[input]);
            cJSON_InitHooks(NULL);
            if (redacted && strstr(redacted, "hidden-credential")) failed = 1;
            free(redacted);
            if (allocation_number < fail_at) break;
        }
        if (fail_at == 256) failed = 1;
    }
    return failed;
}

/**
 * @brief 验证结构化、普通文本、指标例外、故障处理及事件发布脱敏。
 * @return 所有断言通过为零，失败为一，成功路径清理副本、结果和总线。
 */
int main(void)
{
    int failed = test_redaction_allocation_failures();
    char *redacted = cc_redact_secrets("{\"api_key\":\"sk-test\",\"password\":\"pw\",\"ok\":1}");
    if (!redacted || strstr(redacted, "sk-test") || strstr(redacted, "pw")) failed = 1;
    free(redacted);

    redacted = cc_redact_secrets(
        "{\"runtime\":{\"context_window_tokens\":8192,\"max_completion_tokens\":1024},"
        "\"context_window_tokens\":\"private-value\",\"api_key\":\"private-key\"}");
    if (!redacted || !strstr(redacted, "\"context_window_tokens\":8192") ||
        !strstr(redacted, "\"max_completion_tokens\":1024") ||
        strstr(redacted, "private-value") || strstr(redacted, "private-key")) failed = 1;
    free(redacted);

    redacted = cc_redact_secrets(
        "{\"prompt_tokens\":100,\"cached_tokens\":25,\"cached_tokens_reported\":true,"
        "\"completion_tokens\":\"private-string\",\"cache_write_tokens\":{\"token\":\"secret\"}}"
    );
    if (!redacted || !strstr(redacted, "\"prompt_tokens\":100") ||
        !strstr(redacted, "\"cached_tokens\":25") ||
        !strstr(redacted, "\"cached_tokens_reported\":true") ||
        strstr(redacted, "private-string") || strstr(redacted, "secret")) failed = 1;
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
    cc_event_bus_config_t config = cc_event_bus_default_config();
#if CC_ENABLE_ASYNC_OBSERVABILITY
    config.mode = CC_EVENT_BUS_MODE_ASYNC;
#endif
    cc_result_t rc = cc_event_bus_create_with_config(&config, &bus);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return 1;
    }
    cc_result_free(&rc);
    capture_t capture;
    memset(&capture, 0, sizeof(capture));
    cc_event_subscription_t subscription = {0};
    rc = cc_event_bus_subscribe(bus, "test.secret", on_event, &capture, NULL, &subscription);
    if (rc.code != CC_OK) failed = 1;
    cc_result_free(&rc);
    rc = cc_event_bus_publish(bus, "test.secret",
        "{\"authorization\":\"Bearer abc\",\"token\":\"xyz\"}");
    if (rc.code != CC_OK) failed = 1;
    cc_result_free(&rc);
    rc = cc_event_bus_flush(bus, 1000);
    int delivered = rc.code == CC_OK;
    cc_result_free(&rc);
    if (!delivered) {
        failed = 1;
    } else if (!capture.payload[0] || !strstr(capture.payload, "[REDACTED]") ||
               strstr(capture.payload, "Bearer abc") || strstr(capture.payload, "xyz")) {
        failed = 1;
    }
    cc_event_bus_destroy(bus);
    return failed ? 1 : 0;
}
