

#include "cc/util/cc_config.h"
#include "cc/ports/cc_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 浮点配置比较 helper，避免二进制浮点精度导致测试抖动。 */
static int double_eq(double a, double b)
{
    double d = a - b;
    if (d < 0.0) d = -d;
    return d < 0.000001;
}

/* 设置测试环境变量，兼容 Windows 和 POSIX。 */
static void set_test_env(const char *key, const char *value)
{
#if defined(_WIN32)
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

/* 清理测试环境变量，避免影响后续测试进程。 */
static void unset_test_env(const char *key)
{
#if defined(_WIN32)
    _putenv_s(key, "");
#else
    unsetenv(key);
#endif
}


/*
 * 验证配置缺失时默认值仍完整可用。
 *
 * 覆盖缺失文件降级默认配置、UI/模型字段覆盖、thinking mode 对 stream 的影响，以及
 * api_key_env 优先于文件明文 api_key 的安全契约。
 */
int main(void)
{
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

#if defined(CC_STORAGE_JSON_FILE) && CC_STORAGE_JSON_FILE
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
