#include "cc/app/cc_plugin_protocol.h"
#include "cc/util/cc_json.h"
#include "../../core/vendor/cjson/cJSON.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 单次求值检查测试条件，失败报告源码位置并返回一。
 * @param[in] expression 要验证的条件表达式。
 */
#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "plugin protocol check failed at line %d\n", __LINE__); return 1; \
} } while (0)

/** @brief cJSON 串行故障注入记录，所有分配均来自标准 malloc。 */
static void *json_allocations[256];
static size_t json_allocation_count;
static size_t json_fail_at = SIZE_MAX;
static size_t json_live_allocations;

/**
 * @brief 为 cJSON 分配记录拥有地址，并使指定的一次分配失败。
 * @param[in] bytes 分配字节数。
 * @return 正常返回 malloc 地址，指定故障或记录容量用尽返回 NULL。
 */
static void *json_test_allocate(size_t bytes)
{
    if (json_allocation_count++ == json_fail_at) return NULL;
    void *allocation = malloc(bytes);
    if (!allocation) return NULL;
    for (size_t i = 0; i < sizeof(json_allocations) / sizeof(json_allocations[0]); i++) {
        if (!json_allocations[i]) {
            json_allocations[i] = allocation;
            json_live_allocations++;
            return allocation;
        }
    }
    free(allocation);
    return NULL;
}

/**
 * @brief 释放一个已登记的 cJSON 内部节点或打印分配。
 * @param[in,out] allocation 消费登记地址，NULL 时跳过。
 * @post 记录移除并 free，出现未知地址通过断言终止。
 */
static void json_test_free(void *allocation)
{
    if (!allocation) return;
    for (size_t i = 0; i < sizeof(json_allocations) / sizeof(json_allocations[0]); i++) {
        if (json_allocations[i] == allocation) {
            json_allocations[i] = NULL;
            json_live_allocations--;
            free(allocation);
            return;
        }
    }
    assert(!"cJSON released an untracked allocation");
}

/**
 * @brief 用正常解析器核对构造结果包含完整的固定 ID 信封及预期参数类型。
 * @param[in] request 借用构造器成功输出，检查期间保持有效。
 * @param[in] method 借用预期方法名。
 * @param[in] array_params 非零要求数组，否则要求对象。
 * @return 全部检查通过返回零，失败返回一，成功清理解析树和结果诊断。
 */
static int check_request(const char *request, const char *method, int array_params)
{
    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse_strict(request, &root);
    CHECK(rc.code == CC_OK && root && cc_json_is_object(root));
    cc_result_free(&rc);
    CHECK(cc_json_object_size(root) == 4 && !strchr(request, '\n'));
    const char *version = cc_json_string_value(cc_json_object_get_case_sensitive(root, "jsonrpc"));
    const char *id = cc_json_string_value(cc_json_object_get_case_sensitive(root, "id"));
    const char *actual_method = cc_json_string_value(cc_json_object_get_case_sensitive(root, "method"));
    CHECK(version && id && actual_method && strcmp(version, "2.0") == 0 &&
          strcmp(id, "1") == 0 && strcmp(actual_method, method) == 0);
    cc_json_value_t *params = cc_json_object_get_case_sensitive(root, "params");
    CHECK(array_params ? cc_json_is_array(params) : cc_json_is_object(params));
    cc_json_destroy(root);
    return 0;
}

/**
 * @brief 检查对象、数组、缺省参数，以及格式、类型和解码 NUL 的明确拒绝。
 * @return 通过返回零，失败返回一，所有成功请求在返回前释放。
 */
static int test_request_parameters(void)
{
    const char *valid[] = {"{\"city\":\"Beijing\"}", "[1,{\"city\":\"北京\"}]", NULL,
                           "{\"text\":\"literal\\\\u0000\"}"};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        char *request = cc_plugin_protocol_build_request("weather.query", valid[i]);
        CHECK(request && check_request(request, "weather.query", i == 1) == 0);
        free(request);
    }
    char *request = cc_plugin_protocol_build_request(NULL, NULL);
    CHECK(request && check_request(request, "", 0) == 0);
    free(request);
    const char *invalid[] = {"", "{", "{} trailing", "[] {}", "1", "true", "null", "\"text\"",
                             "{\"x\":\"bad\\u12x4\"}", "{\"x\":\"cut\\u0000tail\"}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        CHECK(!cc_plugin_protocol_build_request("weather.query", invalid[i]));
    return 0;
}

/**
 * @brief 检查响应固定 ID、唯一信封、业务节点输出和非法输出槽。
 * @return 通过返回零，失败返回一，成功路径释放全部字符串与拥有诊断。
 */
static int test_response_envelopes(void)
{
    const char *valid[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":{\"ok\":true}}",
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":null}",
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"error\":{\"message\":\"bad\"}}",
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"error\":\"legacy business node\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":\"literal\\\\u0000\"}"
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        char *result = NULL, *error = NULL;
        cc_result_t rc = cc_plugin_protocol_parse_response(valid[i], &result, &error);
        CHECK(rc.code == CC_OK && ((result != NULL) != (error != NULL)));
        CHECK((i == 2 || i == 3) ? error != NULL : result != NULL);
        cc_result_free(&rc);
        cc_plugin_protocol_free_response(result, error);
    }
    const char *invalid[] = {
        "[]", "{", "{}", "{\"jsonrpc\":\"1.0\",\"id\":\"1\",\"result\":0}",
        "{\"JSONRPC\":\"2.0\",\"id\":\"1\",\"result\":0}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":0}",
        "{\"jsonrpc\":\"2.0\",\"id\":\"2\",\"result\":0}",
        "{\"jsonrpc\":\"2.0\",\"id\":null,\"result\":0}",
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":0,\"error\":{}}",
        "{\"jsonrpc\":\"2.0\",\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":0}",
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"id\":\"1\",\"result\":0}",
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":0,\"result\":1}",
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"error\":{},\"error\":{}}",
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":0} trailing",
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":\"bad\\u12x4\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":\"cut\\u0000tail\"}"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        char *result = (char *)(uintptr_t)1, *error = (char *)(uintptr_t)1;
        cc_result_t rc = cc_plugin_protocol_parse_response(invalid[i], &result, &error);
        CHECK(rc.code != CC_OK && !result && !error);
        cc_result_free(&rc);
    }
    char *shared = (char *)(uintptr_t)1;
    cc_result_t rc = cc_plugin_protocol_parse_response(valid[0], &shared, &shared);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT && !shared);
    cc_result_free(&rc);
    char *result = (char *)(uintptr_t)1, *error = (char *)(uintptr_t)1;
    rc = cc_plugin_protocol_parse_response(NULL, &result, &error);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT && !result && !error);
    cc_result_free(&rc);
    rc = cc_plugin_protocol_parse_response(valid[0], NULL, &error);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT && !error);
    cc_result_free(&rc);
    cc_plugin_protocol_free_response(NULL, NULL);
    return 0;
}

/**
 * @brief 逐个拒绝构造与解包的 cJSON 分配，核对原子输出和全部节点清理。
 * @return 通过返回零，失败返回一；每次调用后先恢复全局 hooks，再执行断言。
 * @note 本程序串行运行，钩子仅记录 cJSON 内部分配；SDK 独立复制的导出字符串仍用标准 free。
 */
static int test_json_allocation_failures(void)
{
    cJSON_Hooks hooks = {json_test_allocate, json_test_free};
    json_allocation_count = 0;
    json_fail_at = SIZE_MAX;
    cJSON_InitHooks(&hooks);
    char *request = cc_plugin_protocol_build_request("weather.query", "{\"city\":\"Beijing\"}");
    size_t request_allocations = json_allocation_count;
    cJSON_InitHooks(NULL);
    CHECK(request && check_request(request, "weather.query", 0) == 0);
    free(request);
    CHECK(!json_live_allocations);
    for (size_t i = 0; i < request_allocations; i++) {
        json_allocation_count = 0;
        json_fail_at = i;
        cJSON_InitHooks(&hooks);
        request = cc_plugin_protocol_build_request("weather.query", "{\"city\":\"Beijing\"}");
        cJSON_InitHooks(NULL);
        CHECK(!request && !json_live_allocations);
    }
    const char *responses[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":{\"ok\":true}}",
        "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"error\":{\"message\":\"bad\"}}"
    };
    for (size_t response = 0; response < sizeof(responses) / sizeof(responses[0]); response++) {
        char *result = NULL, *error = NULL;
        json_allocation_count = 0;
        json_fail_at = SIZE_MAX;
        cJSON_InitHooks(&hooks);
        cc_result_t rc = cc_plugin_protocol_parse_response(responses[response], &result, &error);
        size_t response_allocations = json_allocation_count;
        cJSON_InitHooks(NULL);
        CHECK(rc.code == CC_OK && ((result != NULL) != (error != NULL)));
        cc_result_free(&rc);
        cc_plugin_protocol_free_response(result, error);
        CHECK(!json_live_allocations);
        for (size_t i = 0; i < response_allocations; i++) {
            result = error = NULL;
            json_allocation_count = 0;
            json_fail_at = i;
            cJSON_InitHooks(&hooks);
            rc = cc_plugin_protocol_parse_response(responses[response], &result, &error);
            cJSON_InitHooks(NULL);
            CHECK(rc.code != CC_OK && !result && !error && !json_live_allocations);
            cc_result_free(&rc);
        }
    }
    json_fail_at = SIZE_MAX;
    return 0;
}

/**
 * @brief 运行插件信封的严格参数、固定 ID、业务输出和逐分配失败回归。
 * @return 全部检查通过返回零，否则返回一。
 */
int main(void)
{
    CHECK(test_request_parameters() == 0);
    CHECK(test_response_envelopes() == 0);
    CHECK(test_json_allocation_failures() == 0);
    return 0;
}
