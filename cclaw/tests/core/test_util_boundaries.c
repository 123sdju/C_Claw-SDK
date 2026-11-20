#include "cc/util/cc_base64.h"
#include "cc/util/cc_network_policy.h"
#include "cc/util/cc_string_builder.h"
#include "cc/util/cc_token_counter.h"
#include "cc/ports/cc_logger.h"

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 私有副本只降低返回类型上界，以有限输入验证生产饱和逻辑。 */
enum { real_int_max = INT_MAX };
#undef INT_MAX
#define INT_MAX 7
#define cc_token_estimate test_small_token_estimate
#define cc_token_estimate_json_messages test_small_token_estimate_json_messages
#include "../../core/src/util/cc_token_counter.c"
#undef cc_token_estimate
#undef cc_token_estimate_json_messages
#undef INT_MAX
#define INT_MAX real_int_max

#if defined(CC_TEST_UTIL_FAILURES)
static void *move_buffer;
static size_t move_capacity;
static size_t fail_malloc_size;
static int fail_format_write;

/**
 * @brief 链接包装调用真实 realloc。
 * @param[in,out] pointer 旧分配，成功后原指针失效。
 * @param[in] size 新容量字节数。
 * @return 与原生 realloc 相同的拥有结果或 NULL。
 */
void *__real_realloc(void *pointer, size_t size);

/**
 * @brief 链接包装调用真实 malloc。
 * @param[in] size 请求容量字节数。
 * @return 原生拥有分配或 NULL。
 */
void *__real_malloc(size_t size);

/**
 * @brief 链接包装调用真实可变参数格式化。
 * @param[out] buffer 可选目标缓冲，零容量测量时可为 NULL。
 * @param[in] capacity 输出容量字节数。
 * @param[in] format 格式串借用。
 * @param[in] arguments 格式参数借用。
 * @return 原生所需长度或负错误。
 */
int __real_vsnprintf(char *buffer, size_t capacity, const char *format, va_list arguments);

/**
 * @brief 对指定 builder 分配强制搬迁，保证扩容后的旧借用失效。
 * @param[in,out] pointer 待扩容拥有缓冲，匹配 move_buffer 时由包装消费旧分配。
 * @param[in] size 新容量。
 * @return 新拥有缓冲或真实 realloc 结果，分配失败保留旧缓冲。
 */
void *__wrap_realloc(void *pointer, size_t size)
{
    if (pointer != move_buffer || !move_buffer) return __real_realloc(pointer, size);
    void *replacement = __real_malloc(size);
    if (!replacement) return NULL;
    memcpy(replacement, pointer, move_capacity);
    free(pointer);
    move_buffer = NULL;
    return replacement;
}

/**
 * @brief 单次拒绝指定长度的名称复制分配。
 * @param[in] size 请求容量。
 * @return 命中时为 NULL 并清除故障，其余为真实拥有分配。
 */
void *__wrap_malloc(size_t size)
{
    if (fail_malloc_size && size == fail_malloc_size) {
        fail_malloc_size = 0;
        return NULL;
    }
    return __real_malloc(size);
}

/**
 * @brief 在日志第二次格式化时返回负值，保持目标缓冲未写入。
 * @param[out] buffer 原生输出缓冲借用。
 * @param[in] capacity 原生目标容量。
 * @param[in] format 原生格式串借用。
 * @param[in] arguments 原生参数列表借用。
 * @return 故障命中时为 -1，其他交付真实格式化结果。
 */
int __wrap_vsnprintf(char *buffer, size_t capacity, const char *format, va_list arguments)
{
    if (fail_format_write && buffer && strcmp(format, "failure-payload %s") == 0) {
        fail_format_write = 0;
        return -1;
    }
    return __real_vsnprintf(buffer, capacity, format, arguments);
}
#endif

/**
 * @brief 核对 builder 整串与子串自追加，以及 take 后再次初始化。
 * @return 所有调用和正文一致时为零，失败为一，统一释放结果和缓冲。
 */
static int test_builder_alias(void)
{
    char text[201];
    memset(text, 'x', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    cc_string_builder_t builder = {0};
    cc_result_t rc = cc_string_builder_init(&builder);
    int failed = rc.code != CC_OK;
    cc_result_free(&rc);
    if (failed) return failed;
    rc = cc_string_builder_append(&builder, text);
    failed |= rc.code != CC_OK;
    cc_result_free(&rc);
    if (failed) goto cleanup;
#if defined(CC_TEST_UTIL_FAILURES)
    move_buffer = builder.data;
    move_capacity = builder.capacity;
#endif
    rc = cc_string_builder_append(&builder, cc_string_builder_cstr(&builder));
    failed |= rc.code != CC_OK || builder.length != 400;
    cc_result_free(&rc);
    if (failed) goto cleanup;
#if defined(CC_TEST_UTIL_FAILURES)
    failed |= move_buffer != NULL;
    move_buffer = builder.data;
    move_capacity = builder.capacity;
#endif
    rc = cc_string_builder_append(&builder, cc_string_builder_cstr(&builder) + 100);
    failed |= rc.code != CC_OK || builder.length != 700;
    cc_result_free(&rc);
    if (failed) goto cleanup;
#if defined(CC_TEST_UTIL_FAILURES)
    failed |= move_buffer != NULL;
    move_buffer = NULL;
#endif
    for (size_t i = 0; i < builder.length; i++) failed |= builder.data[i] != 'x';
    char *owned = cc_string_builder_take(&builder);
    failed |= !owned || builder.data != NULL || builder.capacity != 0;
    free(owned);
    rc = cc_string_builder_init(&builder);
    failed |= rc.code != CC_OK;
    cc_result_free(&rc);
    rc = cc_string_builder_appendf(&builder, "%s:%d", "ok", 42);
    failed |= rc.code != CC_OK || strcmp(cc_string_builder_cstr(&builder), "ok:42") != 0;
    cc_result_free(&rc);
    cc_string_builder_clear(&builder);
    failed |= builder.length != 0 || cc_string_builder_error(&builder) != CC_OK;
cleanup:
#if defined(CC_TEST_UTIL_FAILURES)
    move_buffer = NULL;
#endif
    cc_string_builder_deinit(&builder);
    return failed;
}

/**
 * @brief 核对 Base64 空输入、标准向量、宽松字母表和格式失败输出。
 * @return 所有向量通过为零，失败为一，每次回收拥有结果及输出。
 */
static int test_base64_edges(void)
{
    const char *plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char *encoded[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    int failed = 0;
    for (size_t i = 0; i < sizeof(plain) / sizeof(plain[0]); i++) {
        char *text = NULL;
        cc_result_t rc = cc_base64_encode((const unsigned char *)plain[i], strlen(plain[i]), &text);
        failed |= rc.code != CC_OK || !text || strcmp(text, encoded[i]) != 0;
        cc_result_free(&rc);
        unsigned char *data = NULL;
        size_t length = 0;
        rc = cc_base64_decode(encoded[i], &data, &length);
        failed |= rc.code != CC_OK || !data || length != strlen(plain[i]);
        if (data && length == strlen(plain[i])) failed |= memcmp(data, plain[i], length) != 0;
        cc_result_free(&rc);
        free(data);
        free(text);
    }
    const char *invalid[] = {"A", "====", "Zg=A", "Zg==AAAA", "***="};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        unsigned char *data = (unsigned char *)(uintptr_t)1;
        size_t length = 99;
        cc_result_t rc = cc_base64_decode(invalid[i], &data, &length);
        failed |= rc.code != CC_ERR_INVALID_ARGUMENT || data != NULL || length != 0;
        cc_result_free(&rc);
    }
    unsigned char *data = NULL;
    size_t length = 0;
    cc_result_t rc = cc_base64_decode(" -_8=\r\n", &data, &length);
    failed |= rc.code != CC_OK || !data || length != 2;
    if (data && length == 2) failed |= data[0] != 0xfb || data[1] != 0xff;
    cc_result_free(&rc);
    free(data);
    return failed;
}

/**
 * @brief 核对 token 估计的现有分段规则及有限输入下的饱和副本。
 * @return 估计和饱和结果均符合预期为零，其他为一。
 */
static int test_token_edges(void)
{
    int failed = cc_token_estimate(NULL) != 0 || cc_token_estimate("") != 0;
    failed |= cc_token_estimate("abcde") != 2;
    failed |= cc_token_estimate("a\xe4\xb8\xad\xe6\x96\x87") != 3;
    failed |= cc_token_estimate_json_messages("abcd") != 1;
    failed |= test_small_token_estimate("abcdefghijklmnopqrstuvwxyz012345") != 7;
    failed |= test_small_token_estimate_json_messages("abcdefghijklmno\xe4\xb8\xad\xe6\x96\x87\xe4\xb8\xad\xe6\x96\x87") != 7;
    return failed;
}

/**
 * @brief 核对 IPv4 映射和兼容 IPv6 的尾部分类，以及缺数组的列表拒绝。
 * @return 私网与公网分类及参数边界均正确为零，其他为一。
 */
static int test_address_edges(void)
{
    cc_network_policy_t policy = {0};
    const char *private_addresses[] = {
        "::ffff:127.0.0.1", "::ffff:7f00:1", "0:0:0:0:0:ffff:10.0.0.1",
        "::127.0.0.1", "::ffff:192.168.1.1", "fe80::1", "fc00::1"
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(private_addresses) / sizeof(private_addresses[0]); i++)
        failed |= cc_network_policy_address_allowed(&policy, NULL, private_addresses[i]) != 0;
    failed |= cc_network_policy_address_allowed(&policy, NULL, "::ffff:8.8.8.8") != 1;
    failed |= cc_network_policy_address_allowed(&policy, NULL, "2001:4860:4860::8888") != 1;
    policy.allow_private_networks = 1;
    failed |= cc_network_policy_address_allowed(&policy, NULL, "::ffff:127.0.0.1") != 1;
    policy.allowlist.count = 1;
    failed |= cc_network_policy_url_allowed(&policy, "https://example.test/") != 0;
    return failed;
}

static char captured_line[256];
static int captured_count;

/**
 * @brief 在单线程日志场景复制完整行以核对格式化失败降级。
 * @param[in,out] user_data 未使用的可选上下文。
 * @param[in] level 本条级别，测试不使用。
 * @param[in] logger_name 本条名称借用，测试不使用。
 * @param[in] message 本次完整行借用，复制进固定缓冲。
 * @post captured_count 递增，captured_line 保存 NUL 结尾的有界副本。
 */
static void capture_log(void *user_data, cc_log_level_t level, const char *logger_name, const char *message)
{
    (void)user_data;
    (void)level;
    (void)logger_name;
    snprintf(captured_line, sizeof(captured_line), "%s", message);
    captured_count++;
}

/**
 * @brief 核对 logger 名称分配错误、NULL 格式串及第二次格式化失败处理。
 * @return 全部断言通过为零，失败为一，返回前清除 sink 并销毁 logger。
 */
static int test_logger_edges(void)
{
    int failed = 0;
    cc_logger_t *logger = NULL;
#if defined(CC_TEST_UTIL_FAILURES)
    const char *name = "unique-logger-name-for-allocation-failure";
    fail_malloc_size = strlen(name) + 1;
    cc_result_t failed_rc = cc_logger_create(name, CC_LOG_INFO, &logger);
    failed |= failed_rc.code != CC_ERR_OUT_OF_MEMORY || logger != NULL || fail_malloc_size != 0;
    cc_result_free(&failed_rc);
    cc_logger_destroy(logger);
    logger = NULL;
#endif
    cc_result_t rc = cc_logger_create("edges", CC_LOG_INFO, &logger);
    failed |= rc.code != CC_OK || !logger;
    cc_result_free(&rc);
    if (!logger) return 1;
    cc_logger_set_sink(capture_log, NULL);
    captured_count = 0;
    cc_logger_log(logger, CC_LOG_INFO, NULL);
    failed |= captured_count != 0;
    cc_logger_log(logger, CC_LOG_INFO, "token=%s", "private-log-value");
    failed |= captured_count != 1 || strstr(captured_line, "private-log-value") != NULL;
#if defined(CC_TEST_UTIL_FAILURES)
    fail_format_write = 1;
    cc_logger_log(logger, CC_LOG_INFO, "failure-payload %s", "unused-value");
    failed |= captured_count != 2 || fail_format_write != 0 || !strstr(captured_line, "[FORMAT_FAILED]");
#endif
    cc_logger_set_sink(NULL, NULL);
    cc_logger_destroy(logger);
    return failed;
}

/**
 * @brief 顺序执行工具层字符串、编码、预算、地址和日志边界回归。
 * @return 全部场景通过为零，任一失败为一，各场景自行收尾拥有资源。
 */
int main(void)
{
    int failed = test_builder_alias();
    failed |= test_base64_edges();
    failed |= test_token_edges();
    failed |= test_address_edges();
    failed |= test_logger_edges();
    return failed;
}
