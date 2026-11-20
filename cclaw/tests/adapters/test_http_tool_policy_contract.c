#include "cc/adapters/cc_default_policy_engine.h"
#include "cc/ports/cc_sandbox.h"
#include "cc/internal/cc_alloc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CC_TEST_HTTP_TOOL_CONTRACT
#include "cc/adapters/cc_builtin_tools.h"
#include "cc/util/cc_json.h"
#include "cJSON.h"
#endif

/** @brief 检查一次条件，失败打印定位并结束当前用例。 @param[in] condition 求值一次的条件。 */
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "HTTP/policy contract %d: %s\n", __LINE__, #condition); return 1; } } while (0)

#ifdef CC_TEST_HTTP_POLICY_OOM
/** @brief 调用真实分配器。 @param[in] size 字节数。 @return 拥有地址或 NULL。 */
void *__real_malloc(size_t size);
/** @brief 调用真实零分配器。 @param[in] count 元素数。 @param[in] size 单项字节。 @return 拥有地址或 NULL。 */
void *__real_calloc(size_t count, size_t size);
/** @brief 归还真实内存。 @param[in,out] pointer 拥有地址，可空。 @post 内存归还。 */
void __real_free(void *pointer);
static int fail_malloc_once;
static int fail_calloc_once;
static size_t fail_malloc_size;
static void *watched[5];
static unsigned released[5];

/** @brief 注入一次指定容量或任意 malloc 失败。 @param[in] size 请求字节数。 @return 拥有地址或 NULL，故障随后关闭。 */
void *__wrap_malloc(size_t size)
{
    if (fail_malloc_once && (!fail_malloc_size || size == fail_malloc_size)) {
        fail_malloc_once = 0;
        return NULL;
    }
    return __real_malloc(size);
}
/** @brief 注入一次 calloc 失败。 @param[in] count 元素数。 @param[in] size 单项字节。 @return 拥有地址或 NULL，故障随后关闭。 */
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_calloc_once) { fail_calloc_once = 0; return NULL; }
    return __real_calloc(count, size);
}
/** @brief 跟踪目标地址的唯一释放。 @param[in,out] pointer 拥有地址，可空。 @post 匹配计数增加并归还内存。 */
void __wrap_free(void *pointer)
{
    for (size_t i = 0; i < 5; i++) if (pointer && pointer == watched[i]) released[i]++;
    __real_free(pointer);
}
#endif

/** @brief 消费拥有诊断并比较结果码。 @param[in] result 接管结果。 @param[in] expected 预期码。 @return 匹配为一。 */
static int result_is(cc_result_t result, cc_error_code_t expected)
{
    int good = result.code == expected;
    cc_result_free(&result);
    return good;
}

/** @brief 检查默认规则、地址拒绝、原因分配和结果清理。 @return 全部通过为零，失败为一，正常路径销毁引擎。 */
static int test_policy_and_sandbox(void)
{
    cc_policy_engine_t policy = {0};
    CHECK(result_is(cc_policy_engine_create_default(1, &policy), CC_OK));
    cc_policy_decision_t decision = {0};
    const char *names[] = {"shell_run", "shell.run", "file_delete", "file.delete", "other"};
    for (size_t i = 0; i < 5; i++) {
        cc_tool_call_t call = {NULL, (char *)names[i], NULL};
        CHECK(result_is(policy.vtable->check_tool_call(policy.self, &call, NULL, &decision), CC_OK));
        CHECK(decision.allowed && decision.require_approval == (i < 4));
        CHECK((decision.reason != NULL) == (i < 4));
        cc_policy_decision_free(&decision);
        CHECK(!decision.allowed && !decision.require_approval && !decision.reason);
        cc_policy_decision_free(&decision);
    }
    CHECK(result_is(policy.vtable->check_tool_call(policy.self, NULL, NULL, &decision), CC_OK));
    CHECK(!decision.allowed && decision.reason);
    cc_policy_decision_free(&decision);
    CHECK(result_is(policy.vtable->check_tool_call(NULL, NULL, NULL, &decision), CC_ERR_INVALID_ARGUMENT));
    CHECK(!decision.allowed && !decision.reason);
    CHECK(result_is(policy.vtable->check_tool_call(policy.self, NULL, NULL, NULL), CC_ERR_INVALID_ARGUMENT));
#ifdef CC_TEST_HTTP_POLICY_OOM
    fail_malloc_once = 1;
    CHECK(result_is(policy.vtable->check_tool_call(policy.self, NULL, NULL, &decision), CC_ERR_OUT_OF_MEMORY));
    CHECK(!decision.allowed && !decision.require_approval && !decision.reason && !fail_malloc_once);
    cc_tool_call_t shell = {NULL, "shell.run", NULL};
    fail_malloc_once = 1;
    CHECK(result_is(policy.vtable->check_tool_call(policy.self, &shell, NULL, &decision), CC_ERR_OUT_OF_MEMORY));
    CHECK(!decision.allowed && !decision.require_approval && !decision.reason);
#endif
    policy.vtable->destroy(policy.self);
    CHECK(result_is(cc_policy_engine_create_default(0, &policy), CC_OK));
    cc_tool_call_t shell_without_approval = {NULL, "shell_run", NULL};
    CHECK(result_is(policy.vtable->check_tool_call(policy.self, &shell_without_approval, NULL, &decision), CC_OK));
    CHECK(decision.allowed && !decision.require_approval && !decision.reason);
    cc_policy_decision_free(&decision);
    policy.vtable->destroy(policy.self);
#ifdef CC_TEST_HTTP_POLICY_OOM
    policy = (cc_policy_engine_t){(void *)1, (const cc_policy_engine_vtable_t *)1};
    fail_calloc_once = 1;
    CHECK(result_is(cc_policy_engine_create_default(1, &policy), CC_ERR_OUT_OF_MEMORY));
    CHECK(!policy.self && !policy.vtable && !fail_calloc_once);
#endif
    cc_policy_decision_free(NULL);
    cc_sandbox_result_t sandbox_result = {0};
    sandbox_result.exit_code = 7;
    sandbox_result.timed_out = 1;
    sandbox_result.stdout_text = cc_copy_string("stdout");
    sandbox_result.stderr_text = cc_copy_string("stderr");
    CHECK(sandbox_result.stdout_text && sandbox_result.stderr_text);
    cc_sandbox_result_free(&sandbox_result);
    CHECK(!sandbox_result.exit_code && !sandbox_result.timed_out && !sandbox_result.stdout_text && !sandbox_result.stderr_text);
    cc_sandbox_result_free(&sandbox_result);
    cc_sandbox_result_free(NULL);
    return 0;
}

#ifdef CC_TEST_HTTP_TOOL_CONTRACT
typedef enum {
    RESPONSE_EXACT, RESPONSE_NUL, RESPONSE_ZERO, RESPONSE_MAX, RESPONSE_MISSING, RESPONSE_FAILURE
} response_kind_t;
typedef struct {
    int calls;
    response_kind_t kind;
    long status;
    long timeout_ms;
    int inject_body_copy_oom;
    size_t inject_result_size_oom;
    char method[32];
    char body[64];
} http_fixture_t;
static size_t json_allocations, json_fail_at, json_live;

/** @brief 对 cJSON 必需分配点逐次注入失败。 @param[in] size 请求字节数。 @return 拥有地址或 NULL，成功增加存活数。 */
static void *json_allocate(size_t size)
{
    json_allocations++;
    if (json_fail_at && json_allocations == json_fail_at) return NULL;
    void *pointer = malloc(size);
    if (pointer) json_live++;
    return pointer;
}
/** @brief 归还 cJSON 钩子分配。 @param[in,out] pointer 拥有地址，可空。 @post 非空地址归还且存活数减少。 */
static void json_release(void *pointer)
{
    if (pointer) json_live--;
    free(pointer);
}

/**
 * @brief 捕获真实工具请求并返回显式长度响应或带部分资源的失败。
 * @param[in,out] self 主测试保持有效的 fixture，记录方法、正文、预算和次数。
 * @param[in] request 同步借用请求，检查连接池、重试和网络复核回调。
 * @param[out] out_response 已清零槽，所有已发布字段均须由工具 response_free。
 * @return 普通为 CC_OK，失败模式返回拥有 NETWORK 诊断，成功也附带拥有诊断检查清理。
 */
static cc_result_t perform(void *self, const cc_http_request_t *request, cc_http_response_t *out_response)
{
    http_fixture_t *fixture = self;
    fixture->calls++;
    fixture->timeout_ms = request->timeout_ms;
    snprintf(fixture->method, sizeof(fixture->method), "%s", request->method);
    snprintf(fixture->body, sizeof(fixture->body), "%s", request->body ? request->body : "");
    if (request->retry_count >= 0 || strcmp(request->connection_pool, "default") ||
        request->max_response_bytes != 16U * 1024U || request->max_redirects != 5 ||
        !request->validate_url || !request->validate_address ||
        !request->validate_url("https://api.example.com/redirect", request->validate_url_user_data) ||
        request->validate_url("https://other.example.org/", request->validate_url_user_data) ||
        !request->validate_address("api.example.com", "8.8.8.8", request->validate_address_user_data)) abort();
    out_response->status_code = fixture->status ? fixture->status : 200;
    if (fixture->kind == RESPONSE_MISSING) {
        out_response->body_size = 2;
    } else {
        size_t allocation_size = fixture->kind == RESPONSE_ZERO || fixture->kind == RESPONSE_MAX ? 1 : 6;
        out_response->body = malloc(allocation_size);
        if (!out_response->body) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Fixture body allocation failed");
        memset(out_response->body, 'x', allocation_size);
        out_response->body_size = fixture->kind == RESPONSE_ZERO ? 0 :
            fixture->kind == RESPONSE_MAX ? SIZE_MAX : allocation_size;
        if (fixture->kind == RESPONSE_NUL) out_response->body[2] = '\0';
    }
    if (fixture->kind == RESPONSE_FAILURE) {
        out_response->headers = calloc(1, sizeof(*out_response->headers));
        if (!out_response->headers) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Fixture headers allocation failed");
        out_response->header_count = 1;
        out_response->headers[0].name = cc_copy_string("X-Fixture");
        out_response->headers[0].value = cc_copy_string("value");
    }
    cc_result_t result = cc_result_error(fixture->kind == RESPONSE_FAILURE ? CC_ERR_NETWORK : CC_OK, "Owned HTTP diagnostic");
#ifdef CC_TEST_HTTP_POLICY_OOM
    if (fixture->kind == RESPONSE_FAILURE) {
        watched[0] = out_response->body;
        watched[1] = out_response->headers;
        watched[2] = (void *)out_response->headers[0].name;
        watched[3] = (void *)out_response->headers[0].value;
        watched[4] = result.message;
    }
    if (fixture->inject_body_copy_oom) {
        fail_malloc_size = 7;
        fail_malloc_once = 1;
    }
    if (fixture->inject_result_size_oom) {
        fail_malloc_size = fixture->inject_result_size_oom;
        fail_malloc_once = 1;
    }
#endif
    return result;
}
static const cc_http_client_vtable_t http_vtable = {.perform = perform};

/** @brief 对两层成功结果检查状态和完整正文。 @param[in] text 稳定工具 JSON 借用。 @param[in] expected_body 预期字符串借用。 @return 符合预期为零，失败为一，解析树和诊断已归还。 */
static int check_body(const char *text, const char *expected_body)
{
    cc_json_value_t *json = NULL;
    CHECK(result_is(cc_json_parse_strict(text, &json), CC_OK));
    const char *body = cc_json_string_value(cc_json_object_get(json, "body"));
    CHECK(body && strcmp(body, expected_body) == 0 && cc_json_is_number(cc_json_object_get(json, "status")));
    cc_json_destroy(json);
    return 0;
}

/** @brief 检查严格参数不派发、超时上界及 HTTP 缺省。 @param[in] tool 存活工具借用。 @param[in,out] fixture 主测试状态。 @return 全部通过为零，失败为一，每次清理工具结果。 */
static int test_parameters(cc_tool_t tool, http_fixture_t *fixture)
{
    const char *invalid[] = {
        NULL, "{", "[]", "{\"url\":\"https://api.example.com\"} tail",
        "{\"url\":\"https://api.example.com\\u0000.evil\"}",
        "{\"url\":\"https://api.example.com\",\"URL\":\"https://other.example.org\"}",
        "{\"url\":\"https://api.example.com\",\"method\":null}",
        "{\"url\":\"https://api.example.com\",\"body\":1}",
        "{\"url\":\"https://api.example.com\",\"timeout_ms\":0.5}",
        "{\"url\":\"https://api.example.com\",\"timeout_ms\":1e100}",
        "{\"url\":\"https://api.example.com\",\"timeout_ms\":\"10\"}"
    };
    cc_tool_result_t result;
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        int before = fixture->calls;
        CHECK(result_is(tool.vtable->call(tool.self, invalid[i], NULL, &result), CC_OK));
        CHECK(!result.ok && result.error && fixture->calls == before);
        cc_tool_result_cleanup(&result);
    }
    cc_tool_context_t context = {0};
    context.timeout_ms = 10;
    CHECK(result_is(tool.vtable->call(tool.self,
        "{\"URL\":\"https://api.example.com\",\"BODY\":\"payload\",\"timeout_ms\":1000}", &context, &result), CC_OK));
    CHECK(result.ok && fixture->timeout_ms == 10 && !strcmp(fixture->method, "POST") && !strcmp(fixture->body, "payload"));
    CHECK(check_body(result.text, "xxxxxx") == 0);
    cc_tool_result_cleanup(&result);
    CHECK(result_is(tool.vtable->call(tool.self,
        "{\"url\":\"https://api.example.com\",\"timeout_ms\":5}", &context, &result), CC_OK));
    CHECK(result.ok && fixture->timeout_ms == 5 && !strcmp(fixture->method, "GET"));
    cc_tool_result_cleanup(&result);
    CHECK(result_is(tool.vtable->call(tool.self,
        "{\"url\":\"https://api.example.com\",\"timeout_ms\":-1}", NULL, &result), CC_OK));
    CHECK(result.ok && fixture->timeout_ms == 30000);
    cc_tool_result_cleanup(&result);
    return 0;
}

/** @brief 检查长度正文、部分响应和副作用后分配失败。 @param[in] tool 存活工具借用。 @param[in,out] fixture 主测试状态。 @return 全部通过为零，失败为一，所有结果已清理。 */
static int test_responses(cc_tool_t tool, http_fixture_t *fixture)
{
    cc_tool_result_t result;
    const char *args = "{\"url\":\"https://api.example.com\"}";
    fixture->kind = RESPONSE_ZERO;
    CHECK(result_is(tool.vtable->call(tool.self, args, NULL, &result), CC_OK));
    CHECK(result.ok && check_body(result.text, "") == 0);
    cc_tool_result_cleanup(&result);
    const response_kind_t invalid[] = {RESPONSE_NUL, RESPONSE_MAX, RESPONSE_MISSING};
    for (size_t i = 0; i < 3; i++) {
        fixture->kind = invalid[i];
        CHECK(result_is(tool.vtable->call(tool.self, args, NULL, &result), CC_OK));
        CHECK(!result.ok && result.error && !result.text);
        cc_tool_result_cleanup(&result);
    }
    fixture->kind = RESPONSE_FAILURE;
    CHECK(result_is(tool.vtable->call(tool.self, args, NULL, &result), CC_OK));
    CHECK(!result.ok && result.error);
#ifdef CC_TEST_HTTP_POLICY_OOM
    for (size_t i = 0; i < 5; i++) {
        CHECK(watched[i] && released[i] == 1);
        watched[i] = NULL;
        released[i] = 0;
    }
#endif
    cc_tool_result_cleanup(&result);
    fixture->kind = RESPONSE_EXACT;
    fixture->status = 503;
    CHECK(result_is(tool.vtable->call(tool.self, args, NULL, &result), CC_OK));
    CHECK(!result.ok && result.error && check_body(result.text, "xxxxxx") == 0);
    cc_tool_result_cleanup(&result);
    fixture->status = 200;
#ifdef CC_TEST_HTTP_POLICY_OOM
    fixture->inject_body_copy_oom = 1;
    CHECK(result_is(tool.vtable->call(tool.self, args, NULL, &result), CC_ERR_OUT_OF_MEMORY));
    CHECK(!result.ok && !result.text && !result.error && !fail_malloc_once);
    fixture->inject_body_copy_oom = 0;
    fail_malloc_size = 0;
    fixture->inject_result_size_oom = strlen("{\"status\":200,\"body\":\"xxxxxx\"}") + 1;
    CHECK(result_is(tool.vtable->call(tool.self, args, NULL, &result), CC_ERR_OUT_OF_MEMORY));
    CHECK(!result.ok && !result.text && !result.error && !fail_malloc_once);
    fixture->status = 503;
    fixture->inject_result_size_oom = strlen("HTTP status outside 2xx") + 1;
    CHECK(result_is(tool.vtable->call(tool.self, args, NULL, &result), CC_ERR_OUT_OF_MEMORY));
    CHECK(!result.ok && !result.text && !result.error && !fail_malloc_once);
    fixture->status = 200;
    fixture->inject_result_size_oom = 0;
    fail_malloc_size = 0;
#endif
    return 0;
}

/** @brief 逐参数解析与结果 JSON 分配故障检查完整失败。 @param[in] tool 存活工具借用。 @return 全部通过为零，失败为一，每轮恢复 cJSON 全局钩子。 */
static int test_json_failures(cc_tool_t tool)
{
    cJSON_Hooks hooks = {json_allocate, json_release};
    const char *args = "{\"url\":\"https://api.example.com\"}";
    cc_tool_result_t result;
    json_allocations = 0;
    cJSON_InitHooks(&hooks);
    cc_result_t rc = tool.vtable->call(tool.self, args, NULL, &result);
    cJSON_InitHooks(NULL);
    CHECK(result_is(rc, CC_OK) && result.ok && !json_live);
    cc_tool_result_cleanup(&result);
    size_t points = json_allocations;
    for (size_t i = 1; i <= points; i++) {
        json_allocations = 0;
        json_fail_at = i;
        cJSON_InitHooks(&hooks);
        rc = tool.vtable->call(tool.self, args, NULL, &result);
        cJSON_InitHooks(NULL);
        CHECK(!result.ok && !result.text && !json_live);
        CHECK(rc.code == CC_ERR_OUT_OF_MEMORY || (rc.code == CC_OK && result.error));
        cc_result_free(&rc);
        cc_tool_result_cleanup(&result);
    }
    json_fail_at = 0;
    return 0;
}

/** @brief 检查工厂早期输出、策略快照与全部 HTTP 调用组。 @return 全部通过为零，失败为一，最终销毁工具。 */
static int test_http_tool(void)
{
    http_fixture_t fixture = {0};
    cc_http_client_t http = {.self = &fixture, .vtable = &http_vtable,
        .capabilities = CC_HTTP_CAP_RESOLVED_ADDRESS_VALIDATION};
    char *items[] = {"api.example.com"};
    cc_network_policy_t policy = {0};
    policy.allowlist = (cc_config_string_list_t){items, 1};
    cc_tool_t tool = {(void *)1, (const cc_tool_vtable_t *)1};
    cc_network_policy_t invalid = policy;
    invalid.allowlist.items = NULL;
    CHECK(result_is(cc_http_request_tool_create_with_policy(&http, &invalid, &tool), CC_ERR_INVALID_ARGUMENT));
    CHECK(tool.self == (void *)1 && tool.vtable == (const cc_tool_vtable_t *)1);
    invalid.allowlist.items = items;
    invalid.allowlist.count = SIZE_MAX;
    CHECK(result_is(cc_http_request_tool_create_with_policy(&http, &invalid, &tool), CC_ERR_LIMIT_EXCEEDED));
#ifdef CC_TEST_HTTP_POLICY_OOM
    fail_calloc_once = 1;
    CHECK(result_is(cc_http_request_tool_create_with_policy(&http, &policy, &tool), CC_ERR_OUT_OF_MEMORY));
    CHECK(!tool.self && !tool.vtable && !fail_calloc_once);
    fail_malloc_once = 1;
    CHECK(result_is(cc_http_request_tool_create_with_policy(&http, &policy, &tool), CC_ERR_OUT_OF_MEMORY));
    CHECK(!tool.self && !tool.vtable && !fail_malloc_once);
#endif
    CHECK(result_is(cc_http_request_tool_create_with_policy(&http, &policy, &tool), CC_OK));
    items[0] = "other.example.org";
    CHECK(test_parameters(tool, &fixture) == 0);
    CHECK(test_responses(tool, &fixture) == 0);
    CHECK(test_json_failures(tool) == 0);
    cc_tool_result_t result;
    CHECK(result_is(tool.vtable->call(NULL, "{}", NULL, &result), CC_ERR_INVALID_ARGUMENT));
    CHECK(result_is(tool.vtable->call(tool.self, "{}", NULL, NULL), CC_ERR_INVALID_ARGUMENT));
#ifdef CC_TEST_HTTP_POLICY_OOM
    fail_malloc_once = 1;
    CHECK(result_is(tool.vtable->call(tool.self, NULL, NULL, &result), CC_ERR_OUT_OF_MEMORY));
    CHECK(!result.text && !result.error && !fail_malloc_once);
#endif
    tool.vtable->destroy(tool.self);
    return 0;
}
#endif

/** @brief 运行无条件策略/清理契约和启用时的 HTTP 工具契约。 @return 全部通过为零，任一断言失败为一。 */
int main(void)
{
    CHECK(test_policy_and_sandbox() == 0);
#ifdef CC_TEST_HTTP_TOOL_CONTRACT
    CHECK(test_http_tool() == 0);
#endif
    return 0;
}
