#include "cc/core/cc_observability.h"
#include "cc/app/cc_cache_metrics.h"
#include "cc/util/cc_json.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief 条件失败时输出行号并返回一。 @param[in] expression 求值一次的条件。 */
#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "observability contract failed at %d\n", __LINE__); return 1; \
} } while (0)

#ifdef CC_TEST_OBSERVABILITY_FAILURES
/** @brief 借出真实分配入口。 @param[in] size 请求字节数。 @return 堆地址或 NULL。 */
void *__real_malloc(size_t size);
/** @brief 借出真实零分配入口。 @param[in] count 元素数量。 @param[in] size 单项字节。 @return 堆地址或 NULL。 */
void *__real_calloc(size_t count, size_t size);
/** @brief 借出真实扩容入口。 @param[in,out] data 旧分配。 @param[in] size 新容量。 @return 新地址或 NULL，失败保留旧块。 */
void *__real_realloc(void *data, size_t size);

static int count_allocations;
static size_t allocation_count;
static size_t allocation_failure = SIZE_MAX;

/** @brief 按单次故障序号分配。 @param[in] size 字节数。 @return 真实分配或故障 NULL。 */
void *__wrap_malloc(size_t size)
{
    if (count_allocations && allocation_count++ == allocation_failure) return NULL;
    return __real_malloc(size);
}

/** @brief 按单次故障序号零分配。 @param[in] count 元素数量。 @param[in] size 单项字节。 @return 真实分配或故障 NULL。 */
void *__wrap_calloc(size_t count, size_t size)
{
    if (count_allocations && allocation_count++ == allocation_failure) return NULL;
    return __real_calloc(count, size);
}

/** @brief 按单次故障序号扩容。 @param[in,out] data 旧分配。 @param[in] size 新容量。 @return 真实结果或 NULL，故障保留旧块。 */
void *__wrap_realloc(void *data, size_t size)
{
    if (count_allocations && allocation_count++ == allocation_failure) return NULL;
    return __real_realloc(data, size);
}

#endif

/** @brief 消费拥有结果并比较错误码。 @param[in] rc 接管结果。 @param[in] code 期望码。 @return 匹配为一。 */
static int result_is(cc_result_t rc, cc_error_code_t code)
{
    int matched = rc.code == code;
    cc_result_free(&rc);
    return matched;
}

/**
 * @brief 构造含可选字段、错误详情与属性树的稳定观测夹具。
 * @param[in] metrics 存活指标对象借用。
 * @param[in] kind 零选择事件构造，一选择指标快照。
 * @param[out] out_json 空拥有槽，成功字符串由测试 free。
 * @return 入口拥有结果，事件构造 NULL 转为 JSON 错误便于统一断言。
 */
static cc_result_t build_fixture(cc_cache_metrics_t *metrics, int kind, char **out_json)
{
    if (kind) return cc_cache_metrics_snapshot(metrics, out_json);
    cc_error_detail_t detail = {
        .size = sizeof(detail), .http_status = 503, .retry_after_ms = 10,
        .recoverable = 1, .provider_error_code = "busy", .raw_redacted_body = "body"
    };
    cc_result_t error = {.code = CC_ERR_NETWORK, .message = "borrowed diagnostic", .detail = &detail};
    cc_observability_event_t event = {
        .size = sizeof(event), .event = "unit\"event", .session_id = "session",
        .run_id = "run", .step = 3, .status = "error", .message = "message",
        .error = &error, .attributes_json = "{\"model\":\"model\",\"nested\":{\"value\":1}}"
    };
    *out_json = cc_observability_event_json(&event);
    return *out_json ? cc_result_ok() : cc_result_error(CC_ERR_JSON, "Event build failed");
}

/**
 * @brief 逐次使两条构造链的每个堆分配失败，要求完整撤销输出。
 * @return 全部断言通过为零，失败为一，成功路径释放对象、JSON 与诊断。
 */
static int test_allocation_failures(void)
{
    cc_cache_metrics_t *metrics = NULL;
    CHECK(result_is(cc_cache_metrics_create(&metrics), CC_OK));
    CHECK(result_is(cc_cache_metrics_set_provider(metrics, "provider"), CC_OK));
    CHECK(result_is(cc_cache_metrics_observe(metrics, CC_OBS_EVENT_LLM_USAGE,
        "{\"attributes\":{\"model\":\"model\",\"prompt_tokens\":100,\"cached_tokens_reported\":true,\"cached_tokens\":25}}"), CC_OK));
    for (int kind = 0; kind < 2; kind++) {
        char *json = NULL;
#ifdef CC_TEST_OBSERVABILITY_FAILURES
        allocation_count = 0;
        allocation_failure = SIZE_MAX;
        count_allocations = 1;
#endif
        cc_result_t rc = build_fixture(metrics, kind, &json);
#ifdef CC_TEST_OBSERVABILITY_FAILURES
        count_allocations = 0;
        size_t allocations = allocation_count;
#endif
        int complete = rc.code == CC_OK && json && strstr(json, kind ? "cache_hit_basis_points" : "provider_error_code");
        cc_result_free(&rc);
        free(json);
        CHECK(complete);
#ifdef CC_TEST_OBSERVABILITY_FAILURES
        CHECK(allocations > 0);
        for (size_t failure = 0; failure < allocations; failure++) {
            json = (char *)(uintptr_t)1;
            allocation_count = 0;
            allocation_failure = failure;
            count_allocations = 1;
            rc = build_fixture(metrics, kind, &json);
            count_allocations = 0;
            int rejected = rc.code != CC_OK && json == NULL;
            cc_result_free(&rc);
            if (json && json != (char *)(uintptr_t)1) free(json);
            if (!rejected) fprintf(stderr, "kind=%d allocation=%zu\n", kind, failure);
            CHECK(rejected);
        }
        allocation_failure = SIZE_MAX;
#endif
    }
    cc_cache_metrics_destroy(metrics);
    return 0;
}

/**
 * @brief 核对属性与指标文本的完整尾部、NUL、对象类型和可选字段默认。
 * @return 全部断言通过为零，失败为一，成功路径销毁全部树及对象。
 */
static int test_input_contracts(void)
{
    const char *invalid[] = {"{} trailing", "[]", "{broken", "{\"name\":\"a\\u0000b\"}"};
    cc_observability_event_t event = {.size = sizeof(event), .event = "event"};
    cc_cache_metrics_t *metrics = NULL;
    CHECK(result_is(cc_cache_metrics_create(&metrics), CC_OK));
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        event.attributes_json = invalid[i];
        CHECK(cc_observability_event_json(&event) == NULL);
        CHECK(result_is(cc_cache_metrics_observe(metrics, CC_OBS_EVENT_LLM_USAGE, invalid[i]), CC_ERR_JSON));
    }
    event.attributes_json = "";
    char *json = cc_observability_event_json(&event);
    CHECK(json && strstr(json, "\"session_id\":\"\"") && !strstr(json, "attributes"));
    free(json);
    CHECK(result_is(cc_cache_metrics_observe(metrics, "other", "not JSON"), CC_OK));
    json = (char *)(uintptr_t)1;
    CHECK(result_is(cc_cache_metrics_snapshot(NULL, &json), CC_ERR_INVALID_ARGUMENT) && !json);
    cc_cache_metrics_destroy(metrics);
    return 0;
}

/** @brief 顺序执行构造分配失败与完整输入回归。 @return 全部通过为零，任一失败为一。 */
int main(void)
{
    CHECK(test_allocation_failures() == 0);
    CHECK(test_input_contracts() == 0);
    return 0;
}
