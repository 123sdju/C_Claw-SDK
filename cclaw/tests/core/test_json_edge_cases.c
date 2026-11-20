#include "cc/util/cc_json.h"
#include "cc/internal/cc_json_fields.h"
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 断言测试条件，失败时输出行号并从当前测试函数返回一。
 * @param[in] expression 求值一次的断言表达式。
 * @note 断言失败立即结束当前测试，后续局部清理语句不再执行。
 */
#define CHECK(expression) do { if (!(expression)) { fprintf(stderr, "check failed at %d\n", __LINE__); return 1; } } while (0)

/**
 * @brief 统一两个 JSON 解析入口的测试调用类型。
 * @param[in] text 借用的 NUL 结尾输入。
 * @param[out] out_value 解析结果槽，成功树由测试调用者销毁。
 * @return 被调用解析入口的结果，消息由测试调用者释放。
 */
typedef cc_result_t (*json_parser_fn)(const char *text, cc_json_value_t **out_value);

/**
 * @brief 验证解析失败会返回 JSON 错误并清空输出槽。
 * @param[in] parser 借用的有效解析函数。
 * @param[in] text 借用的非法 JSON 输入。
 * @return 断言通过为零，失败为一，成功路径释放操作结果消息。
 */
static int expect_parse_error(json_parser_fn parser, const char *text)
{
    cc_json_value_t *value = (cc_json_value_t *)(uintptr_t)1;
    cc_result_t rc = parser(text, &value);
    int rejected = rc.code == CC_ERR_JSON && value == NULL;
    cc_result_free(&rc);
    CHECK(rejected);
    return 0;
}

/**
 * @brief 核对两个解析入口的 Unicode 边界、字面反斜杠、输出状态及普通入口尾部兼容。
 * @return 全部断言通过为零，任一失败为一，成功路径销毁所有解析树及操作结果。
 */
static int test_parsing(void)
{
    const json_parser_fn parsers[] = {cc_json_parse, cc_json_parse_strict};
    const char *invalid[] = {
        "\"x\\uZZZZtail\"",
        "\"\\u12Q4\"",
        "{\"x\\uZZZZtail\":1}",
        "[\"valid\",{\"key\":\"bad\\u0x00\"}]",
        "\"\\u12\"",
        "\"\\uD800\""
    };
    const char *valid[] = {
        "\"\\u0041\\u03a9\\uD83D\\uDE00\"",
        "\"x\\\\uZZZZtail\"",
        "\"quote\\\" \\\\uZZZZ\""
    };
    const char *expected[] = {
        "A\xCE\xA9\xF0\x9F\x98\x80",
        "x\\uZZZZtail",
        "quote\" \\uZZZZ"
    };
    for (size_t parser = 0; parser < sizeof(parsers) / sizeof(parsers[0]); parser++) {
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
            CHECK(expect_parse_error(parsers[parser], invalid[i]) == 0);
        for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
            cc_json_value_t *value = NULL;
            cc_result_t rc = parsers[parser](valid[i], &value);
            CHECK(rc.code == CC_OK && value);
            cc_result_free(&rc);
            CHECK(cc_json_is_string(value));
            CHECK(strcmp(cc_json_string_value(value), expected[i]) == 0);
            cc_json_destroy(value);
        }
        cc_json_value_t *value = NULL;
        cc_result_t rc = parsers[parser]("\"\\u0000\"", &value);
        CHECK(rc.code == CC_OK && value);
        cc_result_free(&rc);
        CHECK(cc_json_is_string(value) && cc_json_string_value(value)[0] == '\0');
        cc_json_destroy(value);
    }

    cc_json_value_t *value = NULL;
    cc_result_t rc = cc_json_parse("true \"\\uZZZZ\"", &value);
    CHECK(rc.code == CC_OK && cc_json_bool_value(value));
    cc_result_free(&rc);
    cc_json_destroy(value);
    CHECK(expect_parse_error(cc_json_parse_strict, "true \"\\uZZZZ\"") == 0);
    CHECK(expect_parse_error(cc_json_parse, "{broken") == 0);
    CHECK(expect_parse_error(cc_json_parse_strict, "{broken") == 0);

    value = (cc_json_value_t *)(uintptr_t)1;
    rc = cc_json_parse(NULL, &value);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT && value == NULL);
    cc_result_free(&rc);
    value = (cc_json_value_t *)(uintptr_t)1;
    rc = cc_json_parse_strict(NULL, &value);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT && value == (cc_json_value_t *)(uintptr_t)1);
    cc_result_free(&rc);
    return 0;
}

/**
 * @brief 核对固定字节位置诊断不复制合成凭据或大尾部，并保持首值/严格入口边界。
 * @return 全部通过为零，失败为一，成功路径归还树、拥有诊断及大输入分配。
 */
static int test_parse_diagnostics(void)
{
    const json_parser_fn parsers[] = {cc_json_parse, cc_json_parse_strict};
    const char *invalid[] = {
        "{\"api_key\":SYNTHETIC_TOKEN}",
        "{\"name\":\"\xE4\xB8\xAD\",\"api_key\":SYNTHETIC_TOKEN}"
    };
    for (size_t parser = 0; parser < sizeof(parsers) / sizeof(parsers[0]); parser++) {
        for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
            const char *token = strstr(invalid[index], "SYNTHETIC_TOKEN");
            CHECK(token);
            char location[64];
            snprintf(location, sizeof(location), "byte %zu", (size_t)(token - invalid[index]));
            cc_json_value_t *value = (cc_json_value_t *)(uintptr_t)1;
            cc_result_t rc = parsers[parser](invalid[index], &value);
            CHECK(rc.code == CC_ERR_JSON && value == NULL && rc.message);
            CHECK(strstr(rc.message, location) && !strstr(rc.message, "SYNTHETIC_TOKEN"));
            CHECK(!strstr(rc.message, "api_key") && strlen(rc.message) < 96);
            cc_result_free(&rc);
        }
    }

    const size_t tail_bytes = 1024U * 1024U;
    const char *prefix = "{\"api_key\":SYNTHETIC_TOKEN";
    size_t prefix_bytes = strlen(prefix);
    char *large = malloc(prefix_bytes + tail_bytes + 2);
    CHECK(large);
    memcpy(large, prefix, prefix_bytes);
    memset(large + prefix_bytes, 'X', tail_bytes);
    large[prefix_bytes + tail_bytes] = '}';
    large[prefix_bytes + tail_bytes + 1] = '\0';
    for (size_t parser = 0; parser < sizeof(parsers) / sizeof(parsers[0]); parser++) {
        cc_json_value_t *value = (cc_json_value_t *)(uintptr_t)1;
        cc_result_t rc = parsers[parser](large, &value);
        CHECK(rc.code == CC_ERR_JSON && value == NULL && rc.message);
        CHECK(strstr(rc.message, "byte 11") && strlen(rc.message) < 96);
        CHECK(!strstr(rc.message, "SYNTHETIC_TOKEN") && !strchr(rc.message, 'X'));
        cc_result_free(&rc);
    }
    free(large);

    cc_json_value_t *value = NULL;
    cc_result_t rc = cc_json_parse("true SYNTHETIC_TOKEN", &value);
    CHECK(rc.code == CC_OK && cc_json_bool_value(value));
    cc_result_free(&rc);
    cc_json_destroy(value);
    value = (cc_json_value_t *)(uintptr_t)1;
    rc = cc_json_parse_strict("true SYNTHETIC_TOKEN", &value);
    CHECK(rc.code == CC_ERR_JSON && !value && rc.message && strstr(rc.message, "byte 5"));
    CHECK(!strstr(rc.message, "SYNTHETIC_TOKEN"));
    cc_result_free(&rc);
    rc = cc_json_parse_strict("true", &value);
    CHECK(rc.code == CC_OK && cc_json_bool_value(value));
    cc_result_free(&rc);
    cc_json_destroy(value);
    rc = cc_json_parse("true", NULL);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT);
    cc_result_free(&rc);
    rc = cc_json_parse_strict("true", NULL);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT);
    cc_result_free(&rc);
    return 0;
}

/**
 * @brief 验证 NaN、正负无穷大与有限数的读取、整数缓存和两种序列化行为。
 * @return 全部断言通过为零，任一失败为一，成功路径释放节点、打印缓冲及回读结果。
 */
static int test_numbers(void)
{
    const double numbers[] = {NAN, INFINITY, -INFINITY, 42.75, 0.0, -42.75, 1e100, -1e100};
    const int integers[] = {0, INT_MAX, INT_MIN, 42, 0, -42, INT_MAX, INT_MIN};
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        cc_json_value_t *value = cc_json_create_number(numbers[i]);
        CHECK(value && cc_json_is_number(value));
        double observed = cc_json_number_value(value);
        CHECK(isnan(numbers[i]) ? isnan(observed) : observed == numbers[i]);
        CHECK(cc_json_int_value(value) == integers[i]);
        char *formatted = cc_json_stringify(value);
        char *compact = cc_json_stringify_unformatted(value);
        CHECK(formatted && compact);
        if (isnan(numbers[i]) || isinf(numbers[i])) {
            CHECK(strcmp(formatted, "null") == 0 && strcmp(compact, "null") == 0);
        } else {
            cc_json_value_t *roundtrip = NULL;
            cc_result_t rc = cc_json_parse_strict(formatted, &roundtrip);
            CHECK(rc.code == CC_OK && cc_json_is_number(roundtrip));
            cc_result_free(&rc);
            CHECK(cc_json_number_value(roundtrip) == numbers[i]);
            cc_json_destroy(roundtrip);
            roundtrip = NULL;
            rc = cc_json_parse_strict(compact, &roundtrip);
            CHECK(rc.code == CC_OK && cc_json_is_number(roundtrip));
            cc_result_free(&rc);
            CHECK(cc_json_number_value(roundtrip) == numbers[i]);
            cc_json_destroy(roundtrip);
        }
        free(formatted);
        free(compact);
        cc_json_destroy(value);
    }
    return 0;
}

/**
 * @brief 核对字段大小写、拥有节点交接和失败消费。
 * @return 全部断言通过为零，失败为一，成功路径销毁对象及全部已交接节点。
 */
static int test_object_fields(void)
{
    cc_json_value_t *object = cc_json_create_object();
    CHECK(object);
    cc_json_value_t *lower = cc_json_create_string("lower");
    cc_json_value_t *upper = cc_json_create_string("upper");
    CHECK(lower && upper);
    CHECK(cc_json_object_set_owned(object, "name", lower));
    CHECK(cc_json_object_set_owned(object, "Name", upper));
    CHECK(cc_json_object_get_case_sensitive(object, "name") == lower);
    CHECK(cc_json_object_get_case_sensitive(object, "Name") == upper);
    CHECK(cc_json_object_get_case_sensitive(object, "NAME") == NULL);
    CHECK(cc_json_object_get(object, "Name") == lower);
    const char *old_key = cc_json_object_key_at(object, 0);
    CHECK(cc_json_object_set_owned(object, old_key, cc_json_create_string("replacement")));
    CHECK(strcmp(cc_json_string_value(cc_json_object_get_case_sensitive(object, "name")), "replacement") == 0);
    CHECK(cc_json_object_get_case_sensitive(NULL, "name") == NULL);
    CHECK(cc_json_object_get_case_sensitive(object, NULL) == NULL);
    CHECK(!cc_json_object_set_owned(NULL, "unused", cc_json_create_string("discard")));
    CHECK(!cc_json_object_set_owned(object, NULL, cc_json_create_string("discard")));
    CHECK(!cc_json_object_set_owned(object, "unused", NULL));
    CHECK(cc_json_object_size(object) == 2);
    cc_json_destroy(object);
    return 0;
}

/**
 * @brief 核对数组追加的节点交接、无效目标消费及顺序。
 * @return 全部通过为零，断言失败为一，成功路径销毁全部树。
 */
static int test_array_ownership(void)
{
    cc_json_value_t *array = cc_json_create_array();
    cc_json_value_t *object = cc_json_create_object();
    cc_json_value_t *first = cc_json_create_string("first");
    CHECK(array && object && first);
    CHECK(cc_json_array_append_owned(array, first));
    CHECK(cc_json_array_get(array, 0) == first);
    CHECK(cc_json_array_append_owned(array, cc_json_create_number(2)));
    CHECK(cc_json_int_value(cc_json_array_get(array, 1)) == 2);
    CHECK(!cc_json_array_append_owned(NULL, cc_json_create_string("discard")));
    CHECK(!cc_json_array_append_owned(object, cc_json_create_string("discard")));
    CHECK(!cc_json_array_append_owned(array, NULL));
    CHECK(cc_json_array_size(array) == 2 && cc_json_object_size(object) == 0);
    cc_json_destroy(array);
    cc_json_destroy(object);
    return 0;
}

/**
 * @brief 核对已知字段的大小写重复检测及借用树稳定性。
 * @return 全部断言通过为零，失败为一，成功路径销毁树与序列化副本。
 */
static int test_duplicate_known_fields(void)
{
    const char *fields[] = {"role", "content"};
    const char *texts[] = {
        "{\"ROLE\":\"user\",\"role\":\"assistant\"}",
        "{\"content\":\"a\",\"c\\u006fntent\":\"b\"}",
        "{\"unknown\":1,\"unknown\":2,\"ROLE\":\"user\"}",
        "{\"content\":\"a\",\"CONTENT\":\"b\",\"role\":\"user\",\"Role\":\"assistant\"}"
    };
    const char *expected[] = {fields[0], fields[1], NULL, fields[0]};
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        cc_json_value_t *object = NULL;
        cc_result_t rc = cc_json_parse_strict(texts[i], &object);
        CHECK(rc.code == CC_OK && object);
        cc_result_free(&rc);
        char *before = cc_json_stringify_unformatted(object);
        CHECK(before);
        CHECK(cc_json_duplicate_known_field(object, fields, 2) == expected[i]);
        char *after = cc_json_stringify_unformatted(object);
        CHECK(after && strcmp(before, after) == 0);
        free(before);
        free(after);
        cc_json_destroy(object);
    }

    cc_json_value_t *large = cc_json_create_object();
    CHECK(large);
    for (size_t i = 0; i < 100; i++) {
        char key[32];
        snprintf(key, sizeof(key), "unknown_%zu", i);
        CHECK(cc_json_object_set_owned(large, key, cc_json_create_number((double)i)));
    }
    CHECK(cc_json_duplicate_known_field(large, fields, 2) == NULL);
    CHECK(cc_json_object_set_owned(large, "role", cc_json_create_string("user")));
    CHECK(cc_json_object_set_owned(large, "ROLE", cc_json_create_string("assistant")));
    CHECK(cc_json_duplicate_known_field(large, fields, 2) == fields[0]);
    CHECK(cc_json_duplicate_known_field(NULL, fields, 2) == NULL);
    CHECK(cc_json_duplicate_known_field(large, NULL, 0) == NULL);
    cc_json_destroy(large);
    return 0;
}

/**
 * @brief 依次运行 JSON Unicode、诊断边界、数字、字段、重复键和数组拥有回归。
 * @return 全部通过为零，子测试任一断言失败为一。
 */
int main(void)
{
    CHECK(test_parsing() == 0);
    CHECK(test_parse_diagnostics() == 0);
    CHECK(test_numbers() == 0);
    CHECK(test_object_fields() == 0);
    CHECK(test_array_ownership() == 0);
    CHECK(test_duplicate_known_fields() == 0);
    return 0;
}
