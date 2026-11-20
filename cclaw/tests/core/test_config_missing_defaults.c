

#include "cc/util/cc_config.h"
#include "cc/ports/cc_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/**
 * @brief 按固定绝对误差比较测试配置浮点数。
 * @param[in] a 待检查值。
 * @param[in] b 预期值。
 * @return 绝对差小于百万分之一为一，其余为零。
 */
static int double_eq(double a, double b)
{
    double d = a - b;
    if (d < 0.0) d = -d;
    return d < 0.000001;
}

/**
 * @brief 设置本测试使用的环境变量。
 * @param[in] key 借用的 NUL 结尾变量名。
 * @param[in] value 借用的变量值。
 * @post 已调用平台设置入口，后续加载断言检查实际结果。
 */
static void set_test_env(const char *key, const char *value)
{
#if defined(_WIN32)
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

/**
 * @brief 清理本测试设置的环境变量。
 * @param[in] key 借用的变量名。
 * @post POSIX 删除变量，Windows 写为空值，设置结果由平台负责。
 */
static void unset_test_env(const char *key)
{
#if defined(_WIN32)
    _putenv_s(key, "");
#else
    unsetenv(key);
#endif
}


/**
 * @brief 写入一个配置夹具并核对公共加载结果，始终清理接收的配置。
 * @param[in] json 借用的完整配置正文。
 * @param[in] expected 期望结果码。
 * @return 写入及加载结果匹配为零，其余为一，临时文件和错误消息在函数内清理。
 */
static int expect_load_result(const char *json, cc_error_code_t expected)
{
    const char *path = "__c_claw_config_size_test__.json";
    FILE *file = fopen(path, "wb");
    if (!file) return 1;
    int written = fputs(json, file) >= 0;
    int closed = fclose(file) == 0;
    cc_config_t config = {0};
    cc_result_t rc = cc_config_load(path, &config);
    remove(path);
    int failed = !written || !closed || rc.code != expected;
    cc_result_free(&rc);
    cc_config_destroy(&config);
    return failed;
}

/**
 * @brief 检查公共空输出拒绝、无符号转换边界及 system 缺省步骤保留。
 * @return 全部通过为零，任一失败为一，成功路径清理所有配置和文件。
 */
static int test_configuration_boundaries(void)
{
    cc_result_t rc = cc_config_load_default(NULL);
    int failed = rc.code != CC_ERR_INVALID_ARGUMENT;
    cc_result_free(&rc);
    rc = cc_config_load(NULL, NULL);
    failed |= rc.code != CC_ERR_INVALID_ARGUMENT;
    cc_result_free(&rc);
    if (failed) return 1;

    const char *invalid[] = {
        "{\"queue\":{\"maxTotalTasks\":18446744073709551616}}",
        "{\"queue\":{\"max_total_tasks\":1e309}}",
        "{\"queue\":{\"completedTtlMs\":1e309}}",
        "{\"queue\":{\"completed_ttl_ms\":-1}}",
        "{\"system\":{\"summary_temperature\":1e309}}",
        "{\"system\":{\"context_compress_threshold\":1e309}}",
        "{\"storage\":{\"session_segment_bytes\":1e309}}",
        "{\"storage\":{\"sessionSegmentBytes\":-1}}",
        "{\"multimodal\":{\"limits\":{\"maxArtifacts\":-1}}}",
        "{\"multimodal\":{\"limits\":{\"max_artifact_bytes\":1e309}}}",
        "{\"multimodal\":{\"limits\":{\"maxBase64Bytes\":18446744073709551616}}}"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        if (expect_load_result(invalid[i], CC_ERR_INVALID_ARGUMENT)) return 1;
    }

    cc_config_t defaults = {0};
    rc = cc_config_load_default(&defaults);
    if (rc.code != CC_OK) { cc_result_free(&rc); return 1; }
    cc_result_free(&rc);
    int default_steps = defaults.max_steps;
    defaults.context_compress_threshold = NAN;
    rc = cc_config_validate(&defaults);
    failed = rc.code != CC_ERR_INVALID_ARGUMENT;
    cc_result_free(&rc);
    defaults.context_compress_threshold = 0.8;
    defaults.summary_temperature = NAN;
    rc = cc_config_validate(&defaults);
    failed |= rc.code != CC_ERR_INVALID_ARGUMENT;
    cc_result_free(&rc);
    cc_config_destroy(&defaults);
    if (failed) return 1;
    const char *path = "__c_claw_config_fraction_test__.json";
    FILE *file = fopen(path, "wb");
    if (!file) return 1;
    fputs("{\"system\":{\"debug\":true},\"storage\":{\"session_segment_bytes\":1.9},"
          "\"queue\":{\"maxTotalTasks\":3.9,\"completedTtlMs\":6.9},"
          "\"multimodal\":{\"limits\":{\"maxArtifacts\":4.9,"
          "\"maxArtifactBytes\":5.9,\"maxBase64Bytes\":7.9}}}", file);
    fclose(file);
    cc_config_t config = {0};
    rc = cc_config_load(path, &config);
    remove(path);
    failed = rc.code != CC_OK || config.max_steps != default_steps ||
        config.session_segment_bytes != 1 || config.queue.max_total_tasks != 3 ||
        config.queue.completed_ttl_ms != 6 || config.multimodal.limits.max_artifacts != 4 ||
        config.multimodal.limits.max_artifact_bytes != 5 ||
        config.multimodal.limits.max_base64_bytes != 7;
    cc_result_free(&rc);
    cc_config_destroy(&config);
    return failed;
}

/**
 * @brief 核对边界、缺失文件后的拥有默认值、字段覆盖和环境密钥优先级。
 * @return 全部通过为零，任一失败为一，加载错误也清理拥有的配置。
 */
int main(void)
{
    if (test_configuration_boundaries()) return 1;
    cc_config_t config;
    cc_result_t rc = cc_config_load("__c_claw_missing_config__.json", &config);

    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        cc_config_destroy(&config);
        return 1;
    }
    cc_result_free(&rc);

    if (!config.provider || !config.model || !config.storage_type ||
        !config.data_dir || !config.storage_path || !config.workspace_path ||
        !config.sandbox_type || !config.memory_backend ||
        !config.memory_path) {
        cc_config_destroy(&config);
        return 1;
    }

    if (strlen(config.provider) == 0 || strlen(config.storage_type) == 0) {
        cc_config_destroy(&config);
        return 1;
    }

#if defined(CC_STORAGE_SQLITE) && CC_STORAGE_SQLITE
    if (strcmp(config.storage_type, "sqlite") != 0 ||
        !config.storage_path || strstr(config.storage_path, "c-claw.db") == NULL) {
        cc_config_destroy(&config);
        return 1;
    }
#elif defined(CC_STORAGE_JSON_SEGMENTED) && CC_STORAGE_JSON_SEGMENTED
    if (strcmp(config.storage_type, "json_segmented") != 0 ||
        !config.storage_path ||
        strstr(config.storage_path, "sessions") == NULL ||
        config.session_segment_bytes != 262144u ||
        !config.session_media_dir ||
        strcmp(config.session_media_dir, "media") != 0) {
        cc_config_destroy(&config);
        return 1;
    }
#endif

    if (config.stream_mode != 0 || config.debug_mode != 0) {
        cc_config_destroy(&config);
        return 1;
    }
    if (config.max_tokens != 4096 || !double_eq(config.temperature, 0.7) ||
        config.summary_max_tokens != 1024 ||
        !double_eq(config.summary_temperature, 0.3)) {
        cc_config_destroy(&config);
        return 1;
    }

#if CC_PLATFORM != CC_PLATFORM_ESP32
    const char root_workspace_path[] = "." "/" "workspace";
    const char root_data_dir[] = "." "/" "data";

    if (strcmp(config.workspace_path, root_workspace_path) == 0 ||
        strcmp(config.data_dir, root_data_dir) == 0 ||
        strstr(config.data_dir, "/runtime/") == NULL) {
        cc_config_destroy(&config);
        return 1;
    }
#endif

    cc_config_destroy(&config);

    const char *path = "__c_claw_config_ui_test__.json";
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    fputs("{\"model\":{\"stream_mode\":true,\"max_tokens\":2048,\"temperature\":0.2},"
          "\"system\":{\"context_compress_threshold\":0.6,"
          "\"summary_max_tokens\":512,\"summary_temperature\":0.1},"
          "\"cli\":{\"debug_mode\":true}}", f);
    fclose(f);

    memset(&config, 0, sizeof(config));
    rc = cc_config_load(path, &config);
    remove(path);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        cc_config_destroy(&config);
        return 1;
    }
    cc_result_free(&rc);

    if (config.stream_mode != 1 || config.debug_mode != 1) {
        cc_config_destroy(&config);
        return 1;
    }
    if (config.max_tokens != 2048 || !double_eq(config.temperature, 0.2) ||
        !double_eq(config.context_compress_threshold, 0.6) ||
        config.summary_max_tokens != 512 ||
        !double_eq(config.summary_temperature, 0.1)) {
        cc_config_destroy(&config);
        return 1;
    }
    cc_config_destroy(&config);

    f = fopen(path, "wb");
    if (!f) return 1;
    fputs("{\"model\":{\"thinking_mode\":1,\"stream_mode\":0},\"cli\":{\"debug_mode\":false}}", f);
    fclose(f);

    memset(&config, 0, sizeof(config));
    rc = cc_config_load(path, &config);
    remove(path);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        cc_config_destroy(&config);
        return 1;
    }
    cc_result_free(&rc);

    if (config.thinking_mode != 1 || config.stream_mode != 1) {
        cc_config_destroy(&config);
        return 1;
    }
    cc_config_destroy(&config);

    set_test_env("CCLAW_TEST_API_KEY", "env-secret");
    f = fopen(path, "wb");
    if (!f) return 1;
    fputs("{\"model\":{\"api_key\":\"file-secret\",\"api_key_env\":\"CCLAW_TEST_API_KEY\"}}", f);
    fclose(f);

    memset(&config, 0, sizeof(config));
    rc = cc_config_load(path, &config);
    remove(path);
    unset_test_env("CCLAW_TEST_API_KEY");
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        cc_config_destroy(&config);
        return 1;
    }
    cc_result_free(&rc);

    if (!config.api_key || strcmp(config.api_key, "env-secret") != 0) {
        cc_config_destroy(&config);
        return 1;
    }
    cc_config_destroy(&config);

    f = fopen(path, "wb");
    if (!f) return 1;
    fputs("{\"storage\":{\"root\":\"/tmp/cclaw\",\"session_backend\":\"json_segmented\","
          "\"session_segment_bytes\":131072,\"session_media_dir\":\"mediax\"}}", f);
    fclose(f);

    memset(&config, 0, sizeof(config));
    rc = cc_config_load(path, &config);
    remove(path);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        cc_config_destroy(&config);
        return 1;
    }
    cc_result_free(&rc);
    if (!config.data_dir || strcmp(config.data_dir, "/tmp/cclaw") != 0 ||
        !config.storage_path || strcmp(config.storage_path, "/tmp/cclaw/sessions") != 0 ||
        config.session_segment_bytes != 131072u ||
        !config.session_media_dir || strcmp(config.session_media_dir, "mediax") != 0) {
        cc_config_destroy(&config);
        return 1;
    }
    cc_config_destroy(&config);
    return 0;
}
