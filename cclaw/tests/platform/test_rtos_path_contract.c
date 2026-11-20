#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "cc/ports/cc_path.h"
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* 同一物理源私有包含两平台实现，各用正常与极小容量，保持真实算法和调用入口。 */
static void *g_owned[64];
static size_t g_owned_count;
static int g_alloc_calls;
static int g_fail_alloc;
static const char *g_length_input;
static size_t g_length_value;

/**
 * @brief 对私有生产分配注入失败并登记拥有权。
 * @param[in] size 分配字节数。
 * @return 指定失败点返回 NULL，其余返回调用方拥有的存储。
 */
static void *model_malloc(size_t size)
{
    if (++g_alloc_calls == g_fail_alloc) return NULL;
    void *result = malloc(size);
    if (result) {
        assert(g_owned_count < sizeof(g_owned) / sizeof(g_owned[0]));
        g_owned[g_owned_count++] = result;
    }
    return result;
}

/**
 * @brief 释放本翻译单元登记的生产分配。
 * @param[in] value 独占分配，可为 NULL。
 * @post 非空分配从登记表移除并归还系统。
 */
static void model_free(void *value)
{
    if (!value) return;
    size_t index = 0;
    while (index < g_owned_count && g_owned[index] != value) index++;
    assert(index < g_owned_count);
    g_owned[index] = g_owned[--g_owned_count];
    free(value);
}

/**
 * @brief 为算术边界提供合成长度，其余输入使用真实 strlen。
 * @param[in] value 借用的 NUL 结尾字符串。
 * @return 指定字符串返回合成值，其余返回真实字节长度。
 */
static size_t model_strlen(const char *value)
{
    return value == g_length_input ? g_length_value : strlen(value);
}

#define malloc model_malloc
#define free model_free
#define strlen model_strlen
#define ESP_PLATFORM 1
#undef PATH_MAX
#define PATH_MAX 32
#define normalize_absolute_path esp_normalize
#define cc_path_join esp_join
#define cc_path_canonical esp_canonical
#define cc_path_is_within esp_within
#define cc_path_dirname esp_dirname
#define cc_path_exists esp_exists
#include "../../platforms/esp32/src/cc_esp32_path.c"
#undef normalize_absolute_path
#undef cc_path_join
#undef cc_path_canonical
#undef cc_path_is_within
#undef cc_path_dirname
#undef cc_path_exists

#define CC_FREERTOS_PATH_MAX 32
#define freertos_copy_string freertos_copy
#define cc_path_join freertos_join
#define cc_path_canonical freertos_canonical
#define cc_path_is_within freertos_within
#define cc_path_dirname freertos_dirname
#define cc_path_exists freertos_exists
#include "../../platforms/freertos/src/cc_freertos_path.c"
#undef freertos_copy_string
#undef cc_path_join
#undef cc_path_canonical
#undef cc_path_is_within
#undef cc_path_dirname
#undef cc_path_exists

#undef PATH_MAX
#define PATH_MAX 3
#define normalize_absolute_path tiny_esp_normalize
#define cc_path_join tiny_esp_join
#define cc_path_canonical tiny_esp_canonical
#define cc_path_is_within tiny_esp_within
#define cc_path_dirname tiny_esp_dirname
#define cc_path_exists tiny_esp_exists
#include "../../platforms/esp32/src/cc_esp32_path.c"
#undef normalize_absolute_path
#undef cc_path_join
#undef cc_path_canonical
#undef cc_path_is_within
#undef cc_path_dirname
#undef cc_path_exists

#undef CC_FREERTOS_PATH_MAX
#define CC_FREERTOS_PATH_MAX 3
#define freertos_copy_string tiny_freertos_copy
#define cc_path_join tiny_freertos_join
#define cc_path_canonical tiny_freertos_canonical
#define cc_path_is_within tiny_freertos_within
#define cc_path_dirname tiny_freertos_dirname
#define cc_path_exists tiny_freertos_exists
#include "../../platforms/freertos/src/cc_freertos_path.c"
#undef freertos_copy_string
#undef cc_path_join
#undef cc_path_canonical
#undef cc_path_is_within
#undef cc_path_dirname
#undef cc_path_exists
#undef CC_FREERTOS_PATH_MAX
#undef ESP_PLATFORM
#undef malloc
#undef free
#undef strlen

/**
 * @brief 统一表达有一个路径输入并交付拥有字符串的测试入口。
 * @param[in] value 调用期间借用的路径。
 * @return 交付可由 model_free 释放的字符串或 NULL。
 */
typedef char *(*path_unary_fn)(const char *value);
/**
 * @brief 统一表达两输入路径拼接入口。
 * @param[in] base 借用基路径。
 * @param[in] child 借用子路径。
 * @return 拥有字符串或 NULL。
 */
typedef char *(*path_join_fn)(const char *base, const char *child);
/**
 * @brief 统一表达路径归属判断入口。
 * @param[in] base 借用基路径。
 * @param[in] path 借用目标路径。
 * @return 归属判断值。
 */
typedef int (*path_within_fn)(const char *base, const char *path);
/**
 * @brief 统一表达存在性判断入口。
 * @param[in] path 借用查询路径。
 * @return 存在性判断值。
 */
typedef int (*path_exists_fn)(const char *path);

typedef struct path_api {
    path_unary_fn canonical; /**< 生产规范化入口。 */
    path_join_fn join;       /**< 生产拼接入口。 */
    path_within_fn within;   /**< 生产归属入口。 */
    path_unary_fn dirname;   /**< 生产父目录入口。 */
    path_exists_fn exists;   /**< 生产存在性入口。 */
    int absolute;           /**< 规范化相对输入时是否补根斜杠。 */
} path_api_t;

static const path_api_t g_apis[] = {
    {esp_canonical, esp_join, esp_within, esp_dirname, esp_exists, 1},
    {freertos_canonical, freertos_join, freertos_within, freertos_dirname, freertos_exists, 0},
    {tiny_esp_canonical, tiny_esp_join, tiny_esp_within, tiny_esp_dirname, tiny_esp_exists, 1},
    {tiny_freertos_canonical, tiny_freertos_join, tiny_freertos_within, tiny_freertos_dirname, tiny_freertos_exists, 0}
};

/**
 * @brief 比较拥有路径并归还分配。
 * @param[in] actual 交接拥有权的输出，必须非 NULL。
 * @param[in] expected 只读期望字符串。
 * @post 两者相等且实际分配已经释放。
 */
static void expect_path(char *actual, const char *expected)
{
    assert(actual && !strcmp(actual, expected));
    model_free(actual);
}

/**
 * @brief 验证正常容量中完整输入拒绝、紧容量成功及组件表满时不丢名字。
 * @param[in] api 正常 32 字节容量的入口集合。
 * @post 所有输出回收，分配表恢复为空。
 */
static void test_capacity(const path_api_t *api)
{
    char text[64];
    memset(text, 'a', sizeof(text));
    text[63] = '\0';
    assert(!api->canonical(text));
    text[0] = '/';
    assert(!api->canonical(text));
    text[31] = '\0';
    expect_path(api->canonical(text), text);
    memset(text, 'a', sizeof(text));
    text[31] = '\0';
    if (api->absolute) assert(!api->canonical(text));
    else expect_path(api->canonical(text), text);
    text[30] = '\0';
    if (api->absolute) {
        char expected[32]; expected[0] = '/'; memcpy(expected + 1, text, 31);
        expect_path(api->canonical(text), expected);
    } else expect_path(api->canonical(text), text);
    char components[32];
    for (size_t i = 0; i < 31; i++) components[i] = i % 2 ? '/' : 'a';
    components[31] = '\0';
    if (api->absolute) assert(!api->canonical(components));
    else expect_path(api->canonical(components), components);
    assert(g_owned_count == 0);
}

/**
 * @brief 验证四种生产包含的紧容量、旧拼接语义和根边界。
 * @param[in] api 本次变体的只读入口集合。
 * @post 所有返回字符串及临时分配已回收。
 */
static void test_common(const path_api_t *api)
{
    assert(!api->canonical(NULL));
    expect_path(api->canonical("/a"), "/a");
    expect_path(api->canonical("a"), api->absolute ? "/a" : "a");
    expect_path(api->canonical(""), api->absolute ? "/" : ".");
    expect_path(api->join("root", "/leaf"), "root//leaf");
    expect_path(api->join("", "leaf"), "/leaf");
    expect_path(api->join(NULL, "leaf"), "leaf");
    assert(!api->join(NULL, NULL));
    expect_path(api->dirname("/a/"), "/a");
    expect_path(api->dirname("leaf"), ".");
    assert(api->within("/", "/a"));
    assert(!api->within("/a", "/ab"));
    assert(!api->within(NULL, "/a"));
    assert(g_owned_count == 0);
}

/**
 * @brief 覆盖合成长度溢出及各入口分配失败，不让合成长度进入真实大对象访问。
 * @param[in] api 本次变体的入口集合。
 * @post 故障项归零，所有部分规范化分配归还。
 */
static void test_failures(const path_api_t *api)
{
    char input[] = "a";
    g_length_input = input;
    g_length_value = SIZE_MAX - 2;
    assert(!api->join(input, "bc"));
    assert(!api->join(input, "bcd"));
    assert(!api->canonical(input));
    g_length_input = NULL;
    g_alloc_calls = 0; g_fail_alloc = 1;
    assert(!api->canonical("a"));
    g_alloc_calls = 0; g_fail_alloc = 1;
    assert(!api->join("a", "b"));
    g_alloc_calls = 0; g_fail_alloc = 1;
    assert(!api->dirname("a"));
    if (api->absolute) {
        g_alloc_calls = 0; g_fail_alloc = 1;
        assert(!api->within("/a", "/a"));
    }
    g_fail_alloc = 0;
    assert(g_owned_count == 0);
}

/**
 * @brief 通过真实文件确认 ESP access 转发与 FreeRTOS 缺省存在查询差异。
 * @post 临时文件及描述符已回收。
 */
static void test_exists_contract(void)
{
    char path[] = "/tmp/cclaw_rtos_path_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(g_apis[0].exists(path));
    assert(!g_apis[1].exists(path));
    assert(close(fd) == 0 && unlink(path) == 0);
}

/**
 * @brief 执行实际生产包含的 RTOS 路径容量、算术和资源回归。
 * @return 全部断言成立返回 0，失败由 assert 终止。
 * @note PATH_MAX=3 让真实组件表被有效输入填满，未伪造 strtok 流；Linux access 不能证明目标 VFS 行为。
 */
int main(void)
{
    for (size_t i = 0; i < sizeof(g_apis) / sizeof(g_apis[0]); i++) {
        test_common(&g_apis[i]);
        test_failures(&g_apis[i]);
        if (i < 2) test_capacity(&g_apis[i]);
    }
    test_exists_contract();
    assert(g_owned_count == 0);
    return 0;
}
