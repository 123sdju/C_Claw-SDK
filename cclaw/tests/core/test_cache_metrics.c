#include "cc/app/cc_cache_metrics.h"
#include "cc/core/cc_observability.h"
#include "cc/util/cc_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief 失败时输出定位并从当前测试返回一。 @param[in] expr 求值一次的条件。 */
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "check failed at line %d\n", __LINE__); return 1; } } while (0)
/** @brief 消费拥有结果并判断成功。 @param[in] rc 按值接管的结果。 @return 成功为一，其余为零。 */
static int ok(cc_result_t rc) { int good = rc.code == CC_OK; cc_result_free(&rc); return good; }
/** @brief 读取测试快照中的小整数。 @param[in] obj 借用对象。 @param[in] key 借用字段名。 @return JSON 整数缓存值。 */
static int number(const cc_json_value_t *obj, const char *key)
{ return cc_json_int_value(cc_json_object_get(obj, key)); }

/**
 * @brief 验证缓存缺失/无效/零、加权命中率、迟到 provider 身份和摘要计数。
 * @return 全部断言通过为零，失败为一，成功路径释放快照树及指标对象。
 */
int main(void)
{
    cc_cache_metrics_t *metrics = NULL;
    CHECK(ok(cc_cache_metrics_create(&metrics)));
    CHECK(ok(cc_cache_metrics_set_provider(metrics, "provider-a")));
    CHECK(ok(cc_cache_metrics_observe(metrics, CC_OBS_EVENT_LLM_USAGE,
        "{\"attributes\":{\"model\":\"model-1\",\"prompt_tokens\":100,\"cached_tokens_reported\":false}}")));
    CHECK(ok(cc_cache_metrics_observe(metrics, CC_OBS_EVENT_LLM_USAGE,
        "{\"attributes\":{\"model\":\"model-1\",\"prompt_tokens\":100,\"cached_tokens_reported\":true,\"cached_tokens\":25,\"cache_write_tokens_reported\":true,\"cache_write_tokens\":30}}")));
    CHECK(ok(cc_cache_metrics_observe(metrics, CC_OBS_EVENT_LLM_USAGE,
        "{\"attributes\":{\"model\":\"model-1\",\"prompt_tokens\":200,\"cached_tokens_reported\":true,\"cached_tokens\":0,\"cache_write_tokens_reported\":true,\"cache_write_tokens\":0}}")));
    CHECK(ok(cc_cache_metrics_observe(metrics, CC_OBS_EVENT_LLM_USAGE,
        "{\"attributes\":{\"model\":\"model-1\",\"cached_tokens_reported\":true,\"cached_tokens\":10,\"cache_write_tokens_reported\":true,\"cache_write_tokens\":-1}}")));
    CHECK(ok(cc_cache_metrics_observe(metrics, CC_OBS_EVENT_LLM_USAGE,
        "{\"attributes\":{\"model\":\"model-1\",\"prompt_tokens\":5,\"cached_tokens_reported\":true,\"cached_tokens\":10}}")));
    CHECK(ok(cc_cache_metrics_set_provider(metrics, "provider-b")));
    /* 后备来源变化后，迟到事件仍使用自己携带的原来源。 */
    CHECK(ok(cc_cache_metrics_observe(metrics, CC_OBS_EVENT_LLM_USAGE,
        "{\"attributes\":{\"provider\":\"provider-a\",\"model\":\"model-1\",\"prompt_tokens\":100,\"cached_tokens_reported\":true,\"cached_tokens\":50,\"cache_write_tokens_reported\":true,\"cache_write_tokens\":10}}")));
    CHECK(ok(cc_cache_metrics_observe(metrics, CC_OBS_EVENT_LLM_USAGE,
        "{\"attributes\":{\"model\":\"model-1\",\"cached_tokens_reported\":false}}")));
    CHECK(ok(cc_cache_metrics_observe(metrics, CC_OBS_EVENT_CONTEXT_SUMMARY_CACHE, "{\"status\":\"hit\"}")));
    CHECK(ok(cc_cache_metrics_observe(metrics, CC_OBS_EVENT_CONTEXT_SUMMARY_CACHE, "{\"status\":\"prefix_hit\"}")));
    CHECK(ok(cc_cache_metrics_observe(metrics, CC_OBS_EVENT_CONTEXT_SUMMARY_CACHE, "{\"status\":\"miss\"}")));
    CHECK(ok(cc_cache_metrics_observe(metrics, CC_OBS_EVENT_CONTEXT_SUMMARY_CACHE, "{\"status\":\"rejected\"}")));
    char *snapshot = NULL;
    CHECK(ok(cc_cache_metrics_snapshot(metrics, &snapshot)));
    cc_json_value_t *root = NULL;
    CHECK(ok(cc_json_parse(snapshot, &root)));
    free(snapshot);
    CHECK(strcmp(cc_json_string_value(cc_json_object_get(root, "delivery")), "best_effort") == 0);
    cc_json_value_t *models = cc_json_object_get(root, "models");
    CHECK(cc_json_array_size(models) == 2);
    cc_json_value_t *a = cc_json_array_get(models, 0), *b = cc_json_array_get(models, 1);
    CHECK(strcmp(cc_json_string_value(cc_json_object_get(a, "provider")), "provider-a") == 0);
    CHECK(number(a, "usage_events") == 6 && number(a, "cache_unreported") == 1);
    CHECK(number(a, "cache_unusable") == 2 && number(a, "cache_measured") == 3);
    CHECK(number(a, "prompt_tokens") == 400 && number(a, "cached_tokens") == 75);
    CHECK(number(a, "cache_write_reported") == 3 && number(a, "cache_write_unreported") == 2 &&
          number(a, "cache_write_unusable") == 1 && number(a, "cache_write_tokens") == 40 &&
          !cc_json_bool_value(cc_json_object_get(a, "cache_write_tokens_saturated")));
    CHECK(number(a, "cache_hit_basis_points") == 1875);
    CHECK(strcmp(cc_json_string_value(cc_json_object_get(b, "provider")), "provider-b") == 0);
    CHECK(number(b, "usage_events") == 1);
    CHECK(cc_json_is_null(cc_json_object_get(b, "cache_hit_basis_points")));
    cc_json_value_t *summary = cc_json_object_get(root, "summary_cache");
    CHECK(number(summary, "hit") == 1 && number(summary, "prefix_hit") == 1 &&
          number(summary, "miss") == 1 && number(summary, "rejected") == 1);
    cc_json_destroy(root);
    cc_cache_metrics_destroy(metrics);
    return 0;
}
