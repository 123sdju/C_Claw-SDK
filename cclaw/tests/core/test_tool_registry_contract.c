#include "cc/ports/cc_tool_registry.h"
#include "cc/util/cc_json.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "CHECK %d: %s\n", __LINE__, #condition); return 1; } } while (0)

typedef struct {
    const char *name;
    const char *description;
    const char *schema;
    int destroyed;
} tool_state_t;

#ifdef CC_TEST_TOOL_REGISTRY_OOM
/** @brief 调用真实分配器。 @param[in] size 请求字节数。 @return 拥有缓冲或 NULL。 */
void *__real_malloc(size_t size);
/** @brief 调用真实零分配器。 @param[in] count 元素数。 @param[in] size 元素字节数。 @return 拥有缓冲或 NULL。 */
void *__real_calloc(size_t count, size_t size);
/** @brief 归还真实分配。 @param[in,out] pointer 消费地址，可为 NULL。 @post 分配已归还。 */
void __real_free(void *pointer);
static int fail_malloc;
static int fail_calloc;

/** @brief 注入一次 malloc 失败。 @param[in] size 请求字节数。 @return 拥有缓冲或 NULL，触发后清除开关。 */
void *__wrap_malloc(size_t size)
{
    if (fail_malloc) { fail_malloc = 0; return NULL; }
    return __real_malloc(size);
}
/** @brief 注入一次 calloc 失败。 @param[in] count 元素数。 @param[in] size 元素字节数。 @return 拥有缓冲或 NULL，触发后清除开关。 */
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_calloc) { fail_calloc = 0; return NULL; }
    return __real_calloc(count, size);
}
/** @brief 转交真实释放。 @param[in,out] pointer 拥有地址，可为 NULL。 @post 地址归还。 */
void __wrap_free(void *pointer) { __real_free(pointer); }
#endif

static size_t json_allocations;
static size_t json_fail_at;
static size_t json_live;

/** @brief 按顺序拒绝一个 cJSON 分配点。 @param[in] size 请求字节数。 @return 拥有缓冲或 NULL，成功登记存活数。 */
static void *json_allocate(size_t size)
{
    json_allocations++;
    if (json_fail_at && json_allocations == json_fail_at) return NULL;
    void *pointer = malloc(size);
    if (pointer) json_live++;
    return pointer;
}
/** @brief 归还 cJSON 分配并维护计数。 @param[in,out] pointer 本钩子拥有地址，可为 NULL。 @post 非空分配归还且存活数减少。 */
static void json_release(void *pointer)
{
    if (pointer) json_live--;
    free(pointer);
}
/** @brief 返回 fixture 名称。 @param[in] self 存活状态借用。 @return 状态所借用名称，可为 NULL。 */
static const char *tool_name(void *self) { return ((tool_state_t *)self)->name; }
/** @brief 返回 fixture 说明。 @param[in] self 存活状态借用。 @return 状态所借用说明，可为 NULL。 */
static const char *tool_description(void *self) { return ((tool_state_t *)self)->description; }
/** @brief 返回可变覆盖 schema。 @param[in] self 存活状态借用。 @return 主测试保持有效的文本，可为 NULL。 */
static const char *tool_schema(void *self) { return ((tool_state_t *)self)->schema; }
/** @brief 标记逻辑资源销毁。 @param[in,out] self fixture 栈状态借用。 @post 销毁数加一，栈容器由测试保管。 */
static void tool_destroy(void *self) { ((tool_state_t *)self)->destroyed++; }
static const cc_tool_vtable_t tool_vtable = {
    tool_name, tool_description, tool_schema, NULL, tool_destroy
};

/** @brief 核对拒绝注册的拥有关系与输出快照。 @return 全部通过为零，断言失败为一。 */
static int test_registration(void)
{
    cc_tool_registry_t *registry = NULL;
    cc_result_t rc = cc_tool_registry_create(&registry);
    CHECK(rc.code == CC_OK && registry);
    cc_result_free(&rc);
    tool_state_t first = {"same", NULL, "{}", 0};
    tool_state_t duplicate = {"same", NULL, "{}", 0};
    cc_tool_t tool = {&first, &tool_vtable};
    rc = cc_tool_registry_add_with_capabilities(registry, tool, CC_TOOL_CAPABILITY_READ_ONLY);
    CHECK(rc.code == CC_OK);
    cc_result_free(&rc);
    rc = cc_tool_registry_add(registry, tool);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT && cc_tool_registry_count(registry) == 1);
    cc_result_free(&rc);
    tool.self = &duplicate;
    rc = cc_tool_registry_add(registry, tool);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT && !duplicate.destroyed);
    cc_result_free(&rc);
    tool_destroy(&duplicate);
    CHECK(duplicate.destroyed == 1);
    cc_tool_t found = {0};
    unsigned capabilities = 99;
    rc = cc_tool_registry_find_with_capabilities(registry, "same", &found, &capabilities);
    CHECK(rc.code == CC_OK && found.self == &first && capabilities == CC_TOOL_CAPABILITY_READ_ONLY);
    cc_result_free(&rc);
    rc = cc_tool_registry_find_with_capabilities(registry, "missing", &found, &capabilities);
    CHECK(rc.code == CC_ERR_NOT_FOUND && found.self == &first && capabilities == 0);
    cc_result_free(&rc);
    rc = cc_tool_registry_freeze(registry);
    CHECK(rc.code == CC_OK && cc_tool_registry_is_frozen(registry));
    cc_result_free(&rc);
    rc = cc_tool_registry_add(registry, (cc_tool_t){0});
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT && cc_tool_registry_count(registry) == 1);
    cc_result_free(&rc);
    cc_tool_registry_destroy(registry);
    CHECK(first.destroyed == 1 && duplicate.destroyed == 1);
    return 0;
}

/** @brief 核对固定容量、无名称条目和名称副本失败撤销。 @return 全部通过为零，断言失败为一。 */
static int test_capacity_and_names(void)
{
    cc_tool_registry_t *registry = NULL;
    cc_result_t rc = cc_tool_registry_create(&registry);
    CHECK(rc.code == CC_OK);
    cc_result_free(&rc);
    tool_state_t states[64] = {0};
    char names[64][16];
    for (size_t i = 0; i < 64; i++) {
        snprintf(names[i], sizeof(names[i]), "tool_%zu", i);
        states[i].name = names[i];
        rc = cc_tool_registry_add(registry, (cc_tool_t){&states[i], &tool_vtable});
        CHECK(rc.code == CC_OK);
        cc_result_free(&rc);
    }
    rc = cc_tool_registry_add(registry, (cc_tool_t){0});
    CHECK(rc.code == CC_ERR_OUT_OF_MEMORY && cc_tool_registry_count(registry) == 64);
    cc_result_free(&rc);
    char **copies = NULL;
    size_t count = 0;
#ifdef CC_TEST_TOOL_REGISTRY_OOM
    fail_calloc = 1;
    rc = cc_tool_registry_list_names(registry, &copies, &count);
    CHECK(rc.code == CC_ERR_OUT_OF_MEMORY && !copies && !count && !fail_calloc);
    cc_result_free(&rc);
    fail_malloc = 1;
    rc = cc_tool_registry_list_names(registry, &copies, &count);
    CHECK(rc.code == CC_ERR_OUT_OF_MEMORY && !copies && !count && !fail_malloc);
    cc_result_free(&rc);
#endif
    rc = cc_tool_registry_list_names(registry, &copies, &count);
    CHECK(rc.code == CC_OK && count == 64);
    cc_result_free(&rc);
    cc_tool_registry_destroy(registry);
    for (size_t i = 0; i < count; i++) {
        CHECK(strcmp(copies[i], names[i]) == 0 && states[i].destroyed == 1);
        free(copies[i]);
    }
    free(copies);
    rc = cc_tool_registry_create(&registry);
    CHECK(rc.code == CC_OK);
    cc_result_free(&rc);
    rc = cc_tool_registry_add_with_capabilities(registry, (cc_tool_t){0}, 2);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT && cc_tool_registry_count(registry) == 0);
    cc_result_free(&rc);
    rc = cc_tool_registry_add(registry, (cc_tool_t){0});
    CHECK(rc.code == CC_OK);
    cc_result_free(&rc);
    rc = cc_tool_registry_list_names(registry, &copies, &count);
    CHECK(rc.code == CC_OK && count == 1 && !copies[0]);
    cc_result_free(&rc);
    free(copies);
    cc_tool_registry_destroy(registry);
    return 0;
}

/** @brief 核对元数据转义、schema 完整性及逐 JSON 分配失败。 @return 全部通过为零，失败为一，钩子在每次序列化结束后恢复。 */
static int test_schema(void)
{
    cc_tool_registry_t *registry = NULL;
    cc_result_t rc = cc_tool_registry_create(&registry);
    CHECK(rc.code == CC_OK);
    cc_result_free(&rc);
    tool_state_t state = {"name\"\\\n", "description\"\\\n", "{\"type\":\"object\"}", 0};
    rc = cc_tool_registry_add(registry, (cc_tool_t){&state, &tool_vtable});
    CHECK(rc.code == CC_OK);
    cc_result_free(&rc);
    char *text = NULL;
    rc = cc_tool_registry_build_schema_json(registry, &text);
    CHECK(rc.code == CC_OK && text);
    cc_result_free(&rc);
    cc_json_value_t *array = NULL;
    rc = cc_json_parse_strict(text, &array);
    CHECK(rc.code == CC_OK && cc_json_array_size(array) == 1);
    cc_result_free(&rc);
    cc_json_value_t *function = cc_json_object_get(cc_json_array_get(array, 0), "function");
    CHECK(strcmp(cc_json_string_value(cc_json_object_get(function, "name")), state.name) == 0);
    CHECK(strcmp(cc_json_string_value(cc_json_object_get(function, "description")), state.description) == 0);
    free(text);
    cc_json_destroy(array);
    const char *invalid[] = {"{", "{} tail", "[]", "{\"x\":\"\\u0000\"}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        state.schema = invalid[i];
        text = (char *)1;
        rc = cc_tool_registry_build_schema_json(registry, &text);
        CHECK(rc.code == CC_ERR_JSON && !text);
        cc_result_free(&rc);
    }
    state.schema = "{\"type\":\"object\"}";
    cJSON_Hooks hooks = {json_allocate, json_release};
    json_allocations = 0;
    cJSON_InitHooks(&hooks);
    rc = cc_tool_registry_build_schema_json(registry, &text);
    cJSON_InitHooks(NULL);
    CHECK(rc.code == CC_OK && text && json_live == 0);
    cc_result_free(&rc);
    free(text);
    size_t points = json_allocations;
    for (size_t i = 1; i <= points; i++) {
        json_allocations = 0;
        json_fail_at = i;
        cJSON_InitHooks(&hooks);
        rc = cc_tool_registry_build_schema_json(registry, &text);
        cJSON_InitHooks(NULL);
        CHECK(rc.code != CC_OK && !text && json_live == 0);
        cc_result_free(&rc);
    }
    json_fail_at = 0;
    cc_tool_registry_destroy(registry);
    CHECK(state.destroyed == 1);
    return 0;
}

/** @brief 顺序运行注册、容量和 schema 回归。 @return 全部通过为零，任一断言失败为一。 */
int main(void)
{
    CHECK(test_registration() == 0);
    CHECK(test_capacity_and_names() == 0);
    CHECK(test_schema() == 0);
#ifdef CC_TEST_TOOL_REGISTRY_OOM
    cc_tool_registry_t *registry = (cc_tool_registry_t *)1;
    fail_calloc = 1;
    cc_result_t rc = cc_tool_registry_create(&registry);
    CHECK(rc.code == CC_ERR_OUT_OF_MEMORY && !registry && !fail_calloc);
    cc_result_free(&rc);
#endif
    return 0;
}
