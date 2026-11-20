#include "cc/app/cc_cache_metrics.h"
#include "cc/core/cc_observability.h"
#include "cc/internal/cc_alloc.h"
#include "cc/internal/cc_json_syntax_check.h"
#include "cc/ports/cc_thread.h"
#include "cc/util/cc_json.h"
#include "cJSON.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_METRIC_MODEL_SLOTS 4U
#define CACHE_METRIC_NAME_BYTES 128U

typedef struct cache_model_metrics {
    int used;
    char provider[CACHE_METRIC_NAME_BYTES];
    char model[CACHE_METRIC_NAME_BYTES];
    uint64_t usage_events;
    uint64_t cache_unreported;
    uint64_t cache_unusable;
    uint64_t cache_measured;
    uint64_t prompt_tokens;
    uint64_t cached_tokens;
    int token_totals_saturated;
    uint64_t cache_write_reported;
    uint64_t cache_write_unreported;
    uint64_t cache_write_unusable;
    uint64_t cache_write_tokens;
    int cache_write_tokens_saturated;
} cache_model_metrics_t;

struct cc_cache_metrics {
    cc_mutex_t mutex;
    char provider[CACHE_METRIC_NAME_BYTES];
    cache_model_metrics_t models[CACHE_METRIC_MODEL_SLOTS];
    uint64_t overflow_models;
    uint64_t summary_hit;
    uint64_t summary_prefix_hit;
    uint64_t summary_miss;
    uint64_t summary_rejected;
    uint64_t summary_model_requests;
    uint64_t summary_usage_reported;
    uint64_t summary_usage_unreported;
    uint64_t memory_requests;
    uint64_t memory_usage_reported;
    uint64_t memory_usage_unreported;
};

/**
 * @brief 在无符号上界处饱和累加一个计数。
 * @param[in,out] target 已持指标锁或尚未发布的有效计数槽。
 * @param[in] amount 本次增量。
 * @post 计数增加至可表示值，溢出时为 UINT64_MAX，无动态资源。
 */
static void add_saturated(uint64_t *target, uint64_t amount)
{
    *target = *target > UINT64_MAX - amount ? UINT64_MAX : *target + amount;
}

/**
 * @brief 创建持有私有锁和四个模型槽的进程期观测指标。
 * @param[out] out_metrics 必填空拥有槽，入口清零，成功对象最终 destroy。
 * @return 成功为 CC_OK，参数、分配或锁创建失败返回拥有结果，调用者释放诊断。
 */
cc_result_t cc_cache_metrics_create(cc_cache_metrics_t **out_metrics)
{
    if (!out_metrics) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null cache metrics output");
    *out_metrics = NULL;
    cc_cache_metrics_t *metrics = calloc(1, sizeof(*metrics));
    if (!metrics) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate cache metrics");
    cc_result_t rc = cc_mutex_create(&metrics->mutex);
    if (rc.code != CC_OK) { free(metrics); return rc; }
    *out_metrics = metrics;
    return rc;
}

/**
 * @brief 消费指标对象及其私有锁。
 * @param[in,out] metrics 独占拥有对象，可为 NULL。
 * @pre 拥有者已关闭入口，全部 observe、set_provider、snapshot 及借用回调已经退出。
 * @post 锁和对象已释放，原对象及别名失效。
 */
void cc_cache_metrics_destroy(cc_cache_metrics_t *metrics)
{
    if (!metrics) return;
    cc_mutex_destroy(metrics->mutex);
    free(metrics);
}

/**
 * @brief 在锁内替换缺少事件级 provider 时使用的后备名称。
 * @param[in,out] metrics 借用存活对象。
 * @param[in] provider 调用期稳定的 NUL 结尾名称，须少于 128 字节。
 * @return 成功为 CC_OK，参数错误返回拥有结果，调用者释放诊断。
 * @note 名称在返回前复制，迟到事件仍优先使用自身 provider。锁的原生失败沿 void 端口契约处理。
 */
cc_result_t cc_cache_metrics_set_provider(cc_cache_metrics_t *metrics, const char *provider)
{
    if (!metrics || !provider || strlen(provider) >= CACHE_METRIC_NAME_BYTES)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid cache metrics provider");
    cc_mutex_lock(metrics->mutex);
    snprintf(metrics->provider, sizeof(metrics->provider), "%s", provider);
    cc_mutex_unlock(metrics->mutex);
    return cc_result_ok();
}

/**
 * @brief 读取可以精确转换为 u64 的非负整数属性。
 * @param[in] object 调用期借用的 JSON 对象，可为 NULL。
 * @param[in] name 借用的区分大小写字段名。
 * @param[out] out 有效槽，仅合法计数时写入。
 * @return 合法有限整数为一，缺失、类型、小数或范围错误为零，树保持只读。
 */
static int json_u64(cJSON *object, const char *name, uint64_t *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= 0.0) ||
        item->valuedouble >= (double)UINT64_MAX) return 0;
    uint64_t value = (uint64_t)item->valuedouble;
    if ((double)value != item->valuedouble) return 0;
    *out = value;
    return 1;
}

/**
 * @brief 在固定模型槽中查找或登记 provider 与 model 组合。
 * @param[in,out] metrics 已持私有锁的对象借用。
 * @param[in] provider 借用的稳定 NUL 结尾来源。
 * @param[in] model 借用的稳定 NUL 结尾模型名。
 * @return 命中或登记时返回锁期内有效的槽借用，名称过长或四槽已满返回 NULL。
 * @note 新槽复制名称，已有槽及其累计计数继续保留，无动态分配。
 */
static cache_model_metrics_t *find_model(cc_cache_metrics_t *metrics, const char *provider, const char *model)
{
    if (strlen(provider) >= CACHE_METRIC_NAME_BYTES || strlen(model) >= CACHE_METRIC_NAME_BYTES) return NULL;
    cache_model_metrics_t *empty = NULL;
    for (size_t i = 0; i < CACHE_METRIC_MODEL_SLOTS; i++) {
        cache_model_metrics_t *slot = &metrics->models[i];
        if (!slot->used) { if (!empty) empty = slot; continue; }
        if (strcmp(slot->provider, provider) == 0 && strcmp(slot->model, model) == 0)
            return slot;
    }
    if (!empty) return NULL;
    empty->used = 1;
    snprintf(empty->provider, sizeof(empty->provider), "%s", provider);
    snprintf(empty->model, sizeof(empty->model), "%s", model);
    return empty;
}

/**
 * @brief 从用量或摘要缓存事件更新按观察范围统计的饱和计数。
 * @param[in,out] metrics 调用期间存活的对象借用。
 * @param[in] event_type 调用期借用的事件名，其他事件成功跳过。
 * @param[in] event_json 调用期借用的完整对象文本，解码 NUL 或坏文本整条失败。
 * @return 成功为 CC_OK，参数或解析错误返回拥有结果，调用者释放诊断。
 * @note 解析树私有且在返回前销毁，锁保护全部共享字段，原生锁失败沿 void 端口契约处理。
 */
cc_result_t cc_cache_metrics_observe(cc_cache_metrics_t *metrics, const char *event_type,
                                     const char *event_json)
{
    if (!metrics || !event_type || !event_json)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid cache metrics event");
    int usage = strcmp(event_type, CC_OBS_EVENT_LLM_USAGE) == 0;
    int summary = strcmp(event_type, CC_OBS_EVENT_CONTEXT_SUMMARY_CACHE) == 0;
    if (!usage && !summary) return cc_result_ok();
    if (cc_storage_json_has_nul_escape(event_json))
        return cc_result_error(CC_ERR_JSON, "Cache metrics JSON contains a decoded NUL");
    cc_json_value_t *parsed = NULL;
    cc_result_t rc = cc_json_parse_strict(event_json, &parsed);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    cJSON *root = parsed;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return cc_result_error(CC_ERR_JSON, "Invalid cache metrics event JSON");
    }
    cc_mutex_lock(metrics->mutex);
    if (usage) {
        cJSON *attributes = cJSON_GetObjectItemCaseSensitive(root, "attributes");
        cJSON *model = cJSON_GetObjectItemCaseSensitive(attributes, "model");
        cJSON *event_provider = cJSON_GetObjectItemCaseSensitive(attributes, "provider");
        cJSON *reported = cJSON_GetObjectItemCaseSensitive(attributes, "cached_tokens_reported");
        cJSON *write_reported = cJSON_GetObjectItemCaseSensitive(attributes, "cache_write_tokens_reported");
        cJSON *purpose = cJSON_GetObjectItemCaseSensitive(attributes, "purpose");
        cJSON *usage_reported = cJSON_GetObjectItemCaseSensitive(attributes, "usage_reported");
        uint64_t prompt = 0, cached = 0;
        uint64_t cache_write = 0;
        int prompt_valid = json_u64(attributes, "prompt_tokens", &prompt) && prompt > 0;
        int cached_valid = cJSON_IsTrue(reported) && json_u64(attributes, "cached_tokens", &cached);
        if (cJSON_IsString(purpose) && purpose->valuestring &&
            strcmp(purpose->valuestring,"context_summary")==0) {
            add_saturated(&metrics->summary_model_requests,1);
            if (cJSON_IsTrue(usage_reported)) add_saturated(&metrics->summary_usage_reported,1);
            else add_saturated(&metrics->summary_usage_unreported,1);
        } else if (cJSON_IsString(purpose) && purpose->valuestring &&
                   strcmp(purpose->valuestring,"memory_extraction")==0) {
            add_saturated(&metrics->memory_requests,1);
            if (cJSON_IsTrue(usage_reported)) add_saturated(&metrics->memory_usage_reported,1);
            else add_saturated(&metrics->memory_usage_unreported,1);
        }
        const char *provider = cJSON_IsString(event_provider) && event_provider->valuestring &&
            event_provider->valuestring[0] ? event_provider->valuestring : metrics->provider;
        cache_model_metrics_t *slot = find_model(metrics, provider,
            cJSON_IsString(model) && model->valuestring ? model->valuestring : "");
        if (!slot) add_saturated(&metrics->overflow_models, 1);
        else {
            add_saturated(&slot->usage_events, 1);
            if (!cJSON_IsTrue(reported)) add_saturated(&slot->cache_unreported, 1);
            else if (!cached_valid || !prompt_valid || cached > prompt) {
                add_saturated(&slot->cache_unusable, 1);
            } else {
                add_saturated(&slot->cache_measured, 1);
                if (slot->prompt_tokens > UINT64_MAX - prompt ||
                    slot->cached_tokens > UINT64_MAX - cached) slot->token_totals_saturated = 1;
                add_saturated(&slot->prompt_tokens, prompt);
                add_saturated(&slot->cached_tokens, cached);
            }
            if (!cJSON_IsTrue(write_reported)) add_saturated(&slot->cache_write_unreported, 1);
            else if (!json_u64(attributes,"cache_write_tokens",&cache_write))
                add_saturated(&slot->cache_write_unusable, 1);
            else {
                add_saturated(&slot->cache_write_reported, 1);
                if (slot->cache_write_tokens>UINT64_MAX-cache_write) slot->cache_write_tokens_saturated=1;
                add_saturated(&slot->cache_write_tokens,cache_write);
            }
        }
    } else {
        cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
        const char *value = cJSON_IsString(status) ? status->valuestring : NULL;
        if (value && strcmp(value, "hit") == 0) add_saturated(&metrics->summary_hit, 1);
        else if (value && strcmp(value, "prefix_hit") == 0) add_saturated(&metrics->summary_prefix_hit, 1);
        else if (value && strcmp(value, "miss") == 0) add_saturated(&metrics->summary_miss, 1);
        else if (value && strcmp(value, "rejected") == 0) add_saturated(&metrics->summary_rejected, 1);
    }
    cc_mutex_unlock(metrics->mutex);
    cJSON_Delete(root);
    return cc_result_ok();
}

/**
 * @brief 在同一锁期内编码指标快照并交付独占 JSON。
 * @param[in,out] metrics 借用存活对象。
 * @param[out] out_json 必填空拥有槽，入口清零，成功字符串由调用者 free。
 * @return 成功为 CC_OK，参数或任一构造分配失败返回拥有结果，调用者释放诊断。
 * @note 计数按 JSON double 编码，大整数可能舍入，饱和标志按 u64 实际累加维护。
 * 命中率仅对有效且未饱和的观察样本计算，事件丢失和模型槽溢出在快照中分别描述。
 */
cc_result_t cc_cache_metrics_snapshot(cc_cache_metrics_t *metrics, char **out_json)
{
    if (out_json) *out_json = NULL;
    if (!metrics || !out_json) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid cache snapshot request");
    cJSON *root = cJSON_CreateObject();
    cJSON *models = cJSON_CreateArray();
    cJSON *summary = cJSON_CreateObject();
    cJSON *memory = cJSON_CreateObject();
    if (!root || !models || !summary || !memory) {
        cJSON_Delete(root); cJSON_Delete(models); cJSON_Delete(summary); cJSON_Delete(memory);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate cache snapshot");
    }
    int good = 1;
    cc_mutex_lock(metrics->mutex);
    good = cJSON_AddStringToObject(root, "scope", "process_observed_events") != NULL &&
        cJSON_AddStringToObject(root, "delivery", "best_effort") != NULL &&
        cJSON_AddNumberToObject(root, "model_events_omitted", (double)metrics->overflow_models) != NULL &&
        cJSON_AddNumberToObject(summary, "hit", (double)metrics->summary_hit) != NULL &&
        cJSON_AddNumberToObject(summary, "prefix_hit", (double)metrics->summary_prefix_hit) != NULL &&
        cJSON_AddNumberToObject(summary, "miss", (double)metrics->summary_miss) != NULL &&
        cJSON_AddNumberToObject(summary, "rejected", (double)metrics->summary_rejected) != NULL &&
        cJSON_AddNumberToObject(summary, "model_requests", (double)metrics->summary_model_requests) != NULL &&
        cJSON_AddNumberToObject(summary, "usage_reported", (double)metrics->summary_usage_reported) != NULL &&
        cJSON_AddNumberToObject(summary, "usage_unreported", (double)metrics->summary_usage_unreported) != NULL &&
        cJSON_AddNumberToObject(memory, "extraction_requests", (double)metrics->memory_requests) != NULL &&
        cJSON_AddNumberToObject(memory, "extraction_usage_reported", (double)metrics->memory_usage_reported) != NULL &&
        cJSON_AddNumberToObject(memory, "extraction_usage_unreported", (double)metrics->memory_usage_unreported) != NULL;
    for (size_t i = 0; good && i < CACHE_METRIC_MODEL_SLOTS; i++) {
        cache_model_metrics_t *slot = &metrics->models[i];
        if (!slot->used) continue;
        cJSON *entry = cJSON_CreateObject();
        if (!entry) { good = 0; break; }
        good = cJSON_AddStringToObject(entry, "provider", slot->provider) != NULL &&
            cJSON_AddStringToObject(entry, "model", slot->model) != NULL &&
            cJSON_AddNumberToObject(entry, "usage_events", (double)slot->usage_events) != NULL &&
            cJSON_AddNumberToObject(entry, "cache_unreported", (double)slot->cache_unreported) != NULL &&
            cJSON_AddNumberToObject(entry, "cache_unusable", (double)slot->cache_unusable) != NULL &&
            cJSON_AddNumberToObject(entry, "cache_measured", (double)slot->cache_measured) != NULL &&
            cJSON_AddNumberToObject(entry, "prompt_tokens", (double)slot->prompt_tokens) != NULL &&
            cJSON_AddNumberToObject(entry, "cached_tokens", (double)slot->cached_tokens) != NULL &&
            cJSON_AddBoolToObject(entry, "token_totals_saturated", slot->token_totals_saturated) != NULL &&
            cJSON_AddNumberToObject(entry, "cache_write_reported", (double)slot->cache_write_reported) != NULL &&
            cJSON_AddNumberToObject(entry, "cache_write_unreported", (double)slot->cache_write_unreported) != NULL &&
            cJSON_AddNumberToObject(entry, "cache_write_unusable", (double)slot->cache_write_unusable) != NULL &&
            cJSON_AddNumberToObject(entry, "cache_write_tokens", (double)slot->cache_write_tokens) != NULL &&
            cJSON_AddBoolToObject(entry, "cache_write_tokens_saturated", slot->cache_write_tokens_saturated) != NULL;
        if (good && slot->prompt_tokens > 0 && !slot->token_totals_saturated) {
            double basis_points = (double)slot->cached_tokens * 10000.0 / (double)slot->prompt_tokens;
            if (basis_points > 10000.0) basis_points = 10000.0;
            good = cJSON_AddNumberToObject(entry, "cache_hit_basis_points", (double)(unsigned)basis_points) != NULL;
        } else if (good) {
            good = cJSON_AddNullToObject(entry, "cache_hit_basis_points") != NULL;
        }
        if (good) good = cJSON_AddItemToArray(models, entry);
        if (!good) cJSON_Delete(entry);
    }
    cc_mutex_unlock(metrics->mutex);
    if (good) { good = cJSON_AddItemToObject(root, "models", models); if (good) models = NULL; }
    if (good) { good = cJSON_AddItemToObject(root, "summary_cache", summary); if (good) summary = NULL; }
    if (good) { good = cJSON_AddItemToObject(root, "memory_model", memory); if (good) memory = NULL; }
    cJSON_Delete(models);
    cJSON_Delete(summary);
    cJSON_Delete(memory);
    if (!good) { cJSON_Delete(root); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot build cache snapshot"); }
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode cache snapshot");
    *out_json = cc_copy_string(printed);
    cJSON_free(printed);
    return *out_json ? cc_result_ok() : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy cache snapshot");
}
