#include "cc/core/cc_message.h"
#include "cc/core/cc_tool_call.h"
#include "cc/internal/cc_alloc.h"
#include "../../core/vendor/cjson/cJSON.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief 单线程 JSON 故障记录，所有钩子在本进程串行安装和恢复。 */
static void *json_allocations[1024];
static size_t json_attempts, json_live, json_fail_at = SIZE_MAX;

#ifdef CC_TEST_MESSAGE_TOOL_OOM
static size_t fail_malloc_bytes, watch_malloc_bytes;
static int fail_malloc_once, fail_calloc_once, inside_json_allocator;
static void *move_pointer, *watched_pointer;
static size_t move_old_bytes, watched_frees;
static int force_move_once;

/** @brief 绕过包装的真实分配。 @param[in] bytes 字节数。 @return 标准 malloc 结果。 */
void *__real_malloc(size_t bytes);
/** @brief 绕过包装的真实零分配。 @param[in] count 项数。 @param[in] bytes 单项字节数。 @return 标准 calloc 结果。 */
void *__real_calloc(size_t count, size_t bytes);
/** @brief 绕过包装的真实扩容。 @param[in,out] pointer 旧拥有地址，可为 NULL。 @param[in] bytes 新容量。 @return 标准 realloc 结果。 */
void *__real_realloc(void *pointer, size_t bytes);
/** @brief 绕过包装释放拥有地址。 @param[in,out] pointer 可空消费地址。 @post 标准 free 已完成。 */
void __real_free(void *pointer);

/**
 * @brief 在 JSON 分配之外拒绝指定副本，并可登记一次精确地址。
 * @param[in] bytes 本次分配字节数。
 * @return 主动故障为 NULL，其他为真实分配结果。
 */
void *__wrap_malloc(size_t bytes)
{
    if (!inside_json_allocator && fail_malloc_once && bytes == fail_malloc_bytes) {
        fail_malloc_once = 0;
        return NULL;
    }
    void *pointer = __real_malloc(bytes);
    if (pointer && !inside_json_allocator && watch_malloc_bytes == bytes) {
        watched_pointer = pointer;
        watch_malloc_bytes = 0;
    }
    return pointer;
}

/**
 * @brief 拒绝一次构造器零分配。
 * @param[in] count 元素数。
 * @param[in] bytes 单项字节数。
 * @return 故障时 NULL，其他交真实 calloc。
 */
void *__wrap_calloc(size_t count, size_t bytes)
{
    if (fail_calloc_once) { fail_calloc_once = 0; return NULL; }
    return __real_calloc(count, bytes);
}

/**
 * @brief 对指定列表扩容强制移动，确保旧 items 在源项复制前失效。
 * @param[in,out] pointer 标准拥有旧地址，可为 NULL。
 * @param[in] bytes 新字节容量，主动移动场景不小于旧容量。
 * @return 新地址或分配失败 NULL，失败保留旧地址，其他分配委托真实 realloc。
 */
void *__wrap_realloc(void *pointer, size_t bytes)
{
    if (!force_move_once || pointer != move_pointer) return __real_realloc(pointer, bytes);
    assert(bytes >= move_old_bytes);
    force_move_once = 0;
    void *next = __real_malloc(bytes);
    if (!next) return NULL;
    memcpy(next, pointer, move_old_bytes);
    __real_free(pointer);
    move_pointer = NULL;
    return next;
}

/**
 * @brief 记录目标拥有字段恰好释放一次。
 * @param[in,out] pointer 可空消费地址。
 * @post 匹配记录先清空再真实 free，防止地址复用误计。
 */
void __wrap_free(void *pointer)
{
    if (pointer && pointer == watched_pointer) { watched_frees++; watched_pointer = NULL; }
    __real_free(pointer);
}
#endif

/**
 * @brief 逐个拒绝 cJSON 真实分配并登记活跃节点或字符串。
 * @param[in] bytes 分配字节数。
 * @return 指定故障为 NULL，其他为标准拥有地址，登记容量不足由断言终止。
 */
static void *json_test_allocate(size_t bytes)
{
    if (json_attempts++ == json_fail_at) return NULL;
#ifdef CC_TEST_MESSAGE_TOOL_OOM
    inside_json_allocator = 1;
#endif
    void *pointer = malloc(bytes);
#ifdef CC_TEST_MESSAGE_TOOL_OOM
    inside_json_allocator = 0;
#endif
    if (!pointer) return NULL;
    for (size_t i = 0; i < sizeof(json_allocations) / sizeof(json_allocations[0]); i++) {
        if (!json_allocations[i]) { json_allocations[i] = pointer; json_live++; return pointer; }
    }
    assert(!"JSON allocation tracker is full");
    free(pointer);
    return NULL;
}

/**
 * @brief 归还本测试钩子分配的地址。
 * @param[in,out] pointer 可空消费地址，非空须在登记表中。
 * @post 记录移除并 free，未知地址由断言终止。
 */
static void json_test_free(void *pointer)
{
    if (!pointer) return;
    for (size_t i = 0; i < sizeof(json_allocations) / sizeof(json_allocations[0]); i++) {
        if (json_allocations[i] == pointer) { json_allocations[i] = NULL; json_live--; free(pointer); return; }
    }
    assert(!"JSON released an unknown allocation");
}

/**
 * @brief 按值消费成功结果，失败时报告合成诊断并终止。
 * @param[in,out] result 待消费结果，原调用者不再释放该副本的字段。
 * @post 已确认 CC_OK，拥有消息归还。
 */
static void require_ok(cc_result_t result)
{
    if (result.code != CC_OK) fprintf(stderr, "fixture error %d %s\n", (int)result.code, result.message ? result.message : "");
    assert(result.code == CC_OK);
    cc_result_free(&result);
}

/**
 * @brief 消费预期错误并核对定位信息。
 * @param[in,out] result 待消费拥有结果。
 * @param[in] code 预期状态码。
 * @param[in] location 可选应出现的字段或项索引文本借用。
 * @post 状态和可选定位断言通过，结果归还。
 */
static void require_error(cc_result_t result, cc_error_code_t code, const char *location)
{
    assert(result.code == code);
    if (location) assert(result.message && strstr(result.message, location));
    cc_result_free(&result);
}

/**
 * @brief 检查构造 NULL 输出、NULL/空文本区别、状态标签和自复制。
 * @post 创建的消息和调用全部销毁，自复制保留原拥有地址和内容。
 */
static void test_creation_and_self_copy(void)
{
    require_error(cc_message_create_text("m", "s", CC_ROLE_USER, "text", NULL, NULL), CC_ERR_INVALID_ARGUMENT, NULL);
    require_error(cc_message_create_parts("m", "s", CC_ROLE_USER, NULL, NULL, NULL), CC_ERR_INVALID_ARGUMENT, NULL);
    cc_message_t *message = NULL;
    require_ok(cc_message_create_text("m", "s", CC_ROLE_USER, NULL, NULL, &message));
    assert(message->content.count == 0);
    char *id = message->id;
    require_ok(cc_message_copy(message, message));
    assert(message->id == id && strcmp(message->id, "m") == 0);
    require_ok(cc_message_set_reasoning_content(message, "reasoning"));
    require_ok(cc_message_set_reasoning_content(message, message->reasoning_content + 2));
    assert(strcmp(message->reasoning_content, "asoning") == 0);
    require_ok(cc_message_set_reasoning_content(message, NULL));
    assert(!message->reasoning_content);
    cc_message_destroy(message);
    require_ok(cc_message_create_text(NULL, NULL, CC_ROLE_TOOL, "", "call", &message));
    assert(message->content.count == 1 && strcmp(message->content.items[0].text, "") == 0);
    message->tool_outcome = CC_MESSAGE_TOOL_OUTCOME_UNKNOWN;
    assert(cc_message_tool_outcome_valid(message) && cc_message_tool_outcome_is_unknown(message));
    cc_message_destroy(message);
    cc_message_t malformed = {.role=CC_ROLE_TOOL,.content={.count=1}};
    assert(!cc_message_tool_outcome_is_unknown(&malformed));
    cc_message_tool_outcome_t outcome = CC_MESSAGE_TOOL_OUTCOME_UNKNOWN;
    require_error(cc_message_tool_outcome_parse("bad", &outcome), CC_ERR_STORAGE, NULL);
    assert(outcome == CC_MESSAGE_TOOL_OUTCOME_UNKNOWN && !cc_message_tool_outcome_string((cc_message_tool_outcome_t)99));
    require_ok(cc_message_tool_outcome_parse("reconciled_executed", &outcome));
    assert(outcome == CC_MESSAGE_TOOL_OUTCOME_RECONCILED_EXECUTED && cc_message_role_from_string("future") == CC_ROLE_USER);
    cc_tool_call_t *call = NULL;
    require_ok(cc_tool_call_create(NULL, NULL, NULL, &call));
    assert(!call->id && !call->name && strcmp(call->arguments_json, "{}") == 0);
    char *arguments = call->arguments_json;
    require_ok(cc_tool_call_copy(call, call));
    assert(call->arguments_json == arguments);
    cc_tool_call_destroy(call);
    require_ok(cc_tool_call_create("id", "name", "", &call));
    assert(strcmp(call->arguments_json, "") == 0);
    cc_tool_call_destroy(call);
}

/**
 * @brief 验证强制移动时的自追加、字段借用追加和列表自复制。
 * @post 列表所有项独立拥有，失败不改变 count，完整清理数组和字符串。
 */
static void test_list_alias_and_shape(void)
{
    cc_tool_call_list_t list = {0};
    require_ok(cc_tool_call_list_append_values(&list, "alpha-id", "alpha-tool", "{\"x\":1}"));
    require_ok(cc_tool_call_list_append_values(&list, "beta-id", "beta-tool", NULL));
    assert(list.count == list.capacity);
#ifdef CC_TEST_MESSAGE_TOOL_OOM
    move_pointer = list.items; move_old_bytes = list.capacity * sizeof(*list.items); force_move_once = 1;
#endif
    require_ok(cc_tool_call_list_append(&list, &list.items[0]));
    assert(list.count == 3 && strcmp(list.items[2].id, "alpha-id") == 0 && list.items[2].id != list.items[0].id);
#ifdef CC_TEST_MESSAGE_TOOL_OOM
    assert(!force_move_once);
#endif
    require_ok(cc_tool_call_list_append_values(&list, list.items[1].id, list.items[1].name, list.items[1].arguments_json));
    assert(list.count == 4 && strcmp(list.items[3].name, "beta-tool") == 0);
    cc_tool_call_t *items = list.items;
    require_ok(cc_tool_call_list_copy(&list, &list));
    assert(list.items == items && list.count == 4);
#ifdef CC_TEST_MESSAGE_TOOL_OOM
    size_t count = list.count;
    move_pointer = list.items; move_old_bytes = list.capacity * sizeof(*list.items); force_move_once = 1;
    fail_malloc_bytes = strlen(list.items[0].name) + 1; fail_malloc_once = 1;
    require_error(cc_tool_call_list_append(&list, &list.items[0]), CC_ERR_OUT_OF_MEMORY, NULL);
    assert(!force_move_once && !fail_malloc_once && list.count == count && strcmp(list.items[0].name, "alpha-tool") == 0);
#endif
    cc_tool_call_list_cleanup(&list);
    cc_tool_call_t call = {.id="view-id",.name="view-tool",.arguments_json="{}"};
    cc_tool_call_list_t view = {.items=&call,.count=1};
    require_error(cc_tool_call_list_append(&view, &call), CC_ERR_INVALID_ARGUMENT, NULL);
    require_ok(cc_tool_call_list_copy(&view, &list));
    assert(list.count == 1 && list.items[0].id != call.id && strcmp(list.items[0].id, call.id) == 0);
    cc_tool_call_list_cleanup(&list);
    char *json = NULL;
    require_ok(cc_tool_call_list_to_json(&view, &json)); free(json);
    view.count = SIZE_MAX / sizeof(call); view.capacity = view.count;
    require_error(cc_tool_call_list_append(&view, &call), CC_ERR_LIMIT_EXCEEDED, NULL);
    view.count = SIZE_MAX; view.capacity = 0;
    json = (char *)(uintptr_t)1;
    require_error(cc_tool_call_list_to_json(&view, &json), CC_ERR_LIMIT_EXCEEDED, NULL);
    assert(!json);
    require_error(cc_tool_call_list_copy(&view, &list), CC_ERR_LIMIT_EXCEEDED, NULL);
    assert(!list.items && !list.count);
    cc_tool_call_list_t missing = {.count=1,.capacity=1};
    require_error(cc_tool_call_list_append(&missing, &call), CC_ERR_INVALID_ARGUMENT, NULL);
    cc_tool_call_list_cleanup(&missing);
    assert(!missing.count && !missing.items);
    cc_message_t message = {0};
    require_error(cc_messages_to_json(&message, SIZE_MAX, 0, &json), CC_ERR_LIMIT_EXCEEDED, NULL);
    assert(!json);
}

/**
 * @brief 验证替换别名、按值移动资源及整体响应文本的独立字段语义。
 * @post 所有源、目标、移出列表和响应完整归还，移动资源只由接收者清理。
 */
static void test_replacement_and_move(void)
{
    cc_message_t *message = NULL;
    require_ok(cc_message_create_text("id", "session", CC_ROLE_ASSISTANT, "text", NULL, &message));
    require_ok(cc_message_add_tool_call(message, &(cc_tool_call_t){.id="call",.name="tool",.arguments_json="{}"}));
    require_ok(cc_message_set_tool_calls(message, &message->tool_calls));
    assert(message->tool_calls.count == 1 && strcmp(message->tool_calls.items[0].name, "tool") == 0);
    cc_message_t copy;
    require_ok(cc_message_copy(message, &copy));
    assert(copy.id != message->id && copy.content.items != message->content.items && copy.tool_calls.items != message->tool_calls.items);
    cc_message_cleanup(&copy); cc_message_destroy(message);
    cc_tool_result_t *result = NULL;
    require_ok(cc_tool_result_create(-2, "result", "error", "metadata", &result));
    assert(result->ok == -2);
    cc_media_artifact_t artifact = {.id="artifact-owned",.kind=CC_MEDIA_FILE,.path="fixture.txt"};
    require_ok(cc_tool_result_add_artifact(result, &artifact));
    require_ok(cc_tool_result_set_artifacts(result, &result->artifacts));
#ifdef CC_TEST_MESSAGE_TOOL_OOM
    watched_pointer = result->artifacts.items[0].id; watched_frees = 0;
#endif
    cc_media_artifact_list_t taken = cc_tool_result_take_artifacts(result);
    assert(!result->artifacts.items && !result->artifacts.count && taken.count == 1);
    cc_tool_result_destroy(result);
    assert(strcmp(taken.items[0].id, "artifact-owned") == 0);
#ifdef CC_TEST_MESSAGE_TOOL_OOM
    assert(watched_frees == 0);
#endif
    cc_media_artifact_list_cleanup(&taken);
#ifdef CC_TEST_MESSAGE_TOOL_OOM
    assert(watched_frees == 1 && !watched_pointer);
#endif
    taken = cc_tool_result_take_artifacts(NULL); assert(!taken.items && !taken.count);
    cc_llm_response_t response;
    require_ok(cc_llm_response_init(&response));
    require_ok(cc_llm_response_set_text(&response, NULL));
    assert(response.has_text && response.text && !*response.text && !response.content.count);
    require_ok(cc_llm_response_set_text(&response, "before"));
    require_ok(cc_llm_response_set_text(&response, response.text + 2));
    assert(strcmp(response.text, "fore") == 0);
    require_ok(cc_llm_response_add_tool_call(&response, response.text, "tool", NULL));
    require_ok(cc_llm_response_set_text(&response, "after"));
    assert(strcmp(response.tool_calls.items[0].id, "fore") == 0);
    require_ok(cc_llm_response_add_artifact(&response, &artifact));
    cc_llm_response_free(&response);
}

/**
 * @brief 检查严格消息和调用列表的合法缺省、大小写及原始参数文本。
 * @post 每份成功解析输出均清理，未知模型字段没有注入本地身份或恢复状态。
 */
static void test_json_defaults(void)
{
    cc_message_t message;
    require_ok(cc_message_from_json("{}", &message));
    assert(message.role == CC_ROLE_USER && message.content.count == 1 && !*message.content.items[0].text);
    cc_message_cleanup(&message);
    require_ok(cc_message_from_json("{\"ROLE\":\"future\",\"CONTENT\":null,\"TOOL_CALLS\":null,\"REASONING_CONTENT\":null,\"TOOL_CALL_ID\":null,\"id\":7}", &message));
    assert(message.role == CC_ROLE_USER && !message.reasoning_content && !message.tool_call_id && !message.id);
    cc_message_cleanup(&message);
    require_ok(cc_message_from_json("{\"role\":\"assistant\",\"content\":\"literal\\\\u0000\",\"reasoning_content\":\"\",\"tool_call_id\":\"\"}", &message));
    assert(strcmp(message.content.items[0].text, "literal\\u0000") == 0 && message.reasoning_content && !*message.reasoning_content);
    cc_message_cleanup(&message);
    cc_tool_call_list_t calls;
    require_ok(cc_tool_call_list_from_json(NULL, &calls)); assert(!calls.count);
    require_ok(cc_tool_call_list_from_json("", &calls)); assert(!calls.count);
    require_ok(cc_tool_call_list_from_json("[{\"ID\":null,\"TYPE\":\"future\",\"FUNCTION\":{\"NAME\":null,\"ARGUMENTS\":null}}]", &calls));
    assert(calls.count == 1 && !calls.items[0].id && !calls.items[0].name && strcmp(calls.items[0].arguments_json, "{}") == 0);
    cc_tool_call_list_cleanup(&calls);
    require_ok(cc_tool_call_list_from_json("[{\"function\":{\"arguments\":\"raw parameter text\"}}]", &calls));
    assert(strcmp(calls.items[0].arguments_json, "raw parameter text") == 0);
    cc_tool_call_list_cleanup(&calls);
}

/**
 * @brief 检查尾部、解码 NUL、错类型、已知重复及后续坏项导致整份撤销。
 * @post 全部失败输出清零，错误包含可识别字段或项位置。
 */
static void test_json_rejection(void)
{
    const char *messages[] = {"{} trailing", "null", "{\"role\":null}", "{\"content\":7}",
        "{\"tool_calls\":{}}", "{\"reasoning_content\":4}", "{\"tool_call_id\":false}",
        "{\"role\":\"user\",\"ROLE\":\"tool\"}", "{\"content\":\"cut\\u0000tail\"}",
        "{\"content\":\"allocated\",\"tool_calls\":[{\"function\":{}},7]}"};
    const char *locations[] = {NULL,NULL,"role","content","tool_calls","reasoning_content","tool_call_id","role","NUL","item 1"};
    for (size_t i = 0; i < sizeof(messages) / sizeof(messages[0]); i++) {
        cc_message_t message;
        require_error(cc_message_from_json(messages[i], &message), CC_ERR_JSON, locations[i]);
        assert(!message.content.items && !message.tool_calls.items && !message.reasoning_content && !message.tool_call_id);
    }
    const char *calls[] = {"[] tail", "{}", "[7]", "[{}]", "[{\"function\":[]}]", "[{\"id\":7,\"function\":{}}]",
        "[{\"type\":null,\"function\":{}}]", "[{\"function\":{\"name\":false}}]", "[{\"function\":{\"arguments\":{}}}]",
        "[{\"function\":{\"name\":\"one\",\"NAME\":\"two\"}}]", "[{\"function\":{\"name\":\"cut\\u0000tail\"}}]",
        "[{\"function\":{}},{\"function\":{\"arguments\":5}}]"};
    for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        cc_tool_call_list_t list;
        require_error(cc_tool_call_list_from_json(calls[i], &list), CC_ERR_JSON, NULL);
        assert(!list.items && !list.count && !list.capacity);
    }
    cc_message_t message;
    require_error(cc_message_from_json(NULL, &message), CC_ERR_INVALID_ARGUMENT, NULL);
    assert(!message.content.items);
}

/**
 * @brief 逐个拒绝两类解析的 JSON 分配，确认部分列表和消息全部撤销。
 * @post cJSON 钩子恢复默认，活跃记录归零，成功输出及每次诊断归还。
 */
static void test_json_allocation_failures(void)
{
    const char *text[] = {"{\"content\":[{\"type\":\"text\",\"text\":\"part\"}],\"tool_calls\":[{\"function\":{\"name\":\"tool\"}}],\"reasoning_content\":\"reasoning\"}",
        "[{\"id\":\"one\",\"function\":{\"name\":\"tool\",\"arguments\":\"{}\"}},{\"function\":{\"name\":\"two\"}}]"};
    cJSON_Hooks hooks = {json_test_allocate,json_test_free};
    for (int kind = 0; kind < 2; kind++) {
        cc_message_t message; cc_tool_call_list_t list;
        json_attempts = 0; json_fail_at = SIZE_MAX; cJSON_InitHooks(&hooks);
        cc_result_t result = kind ? cc_tool_call_list_from_json(text[kind], &list) : cc_message_from_json(text[kind], &message);
        size_t allocations = json_attempts; cJSON_InitHooks(NULL);
        require_ok(result); assert(!json_live && allocations);
        if (kind) cc_tool_call_list_cleanup(&list); else cc_message_cleanup(&message);
        for (size_t failure = 0; failure < allocations; failure++) {
            json_attempts = 0; json_fail_at = failure; cJSON_InitHooks(&hooks);
            result = kind ? cc_tool_call_list_from_json(text[kind], &list) : cc_message_from_json(text[kind], &message);
            cJSON_InitHooks(NULL);
            assert(result.code != CC_OK && !json_live); cc_result_free(&result);
            if (kind) assert(!list.items && !list.count); else assert(!message.content.items && !message.tool_calls.items);
        }
    }
    json_fail_at = SIZE_MAX;
}

/**
 * @brief 拒绝非 JSON 的构造或字段副本，验证输出清零和旧值保留。
 * @post Linux 故障全部消费，部分列表首项精确释放，其他平台继续执行可移植场景。
 */
static void test_owned_copy_failures(void)
{
#ifdef CC_TEST_MESSAGE_TOOL_OOM
    cc_message_t *message = (cc_message_t *)(uintptr_t)1;
    fail_calloc_once = 1;
    require_error(cc_message_create_text("m", "s", CC_ROLE_USER, "text", NULL, &message), CC_ERR_OUT_OF_MEMORY, NULL);
    assert(!message && !fail_calloc_once);
    require_ok(cc_message_create_text("m", "s", CC_ROLE_USER, "text", NULL, &message));
    require_ok(cc_message_set_reasoning_content(message, "old"));
    fail_malloc_bytes = strlen("replacement reasoning") + 1; fail_malloc_once = 1;
    require_error(cc_message_set_reasoning_content(message, "replacement reasoning"), CC_ERR_OUT_OF_MEMORY, NULL);
    assert(!fail_malloc_once && strcmp(message->reasoning_content, "old") == 0);
    cc_message_destroy(message);
    cc_llm_response_t response;
    require_ok(cc_llm_response_init(&response)); require_ok(cc_llm_response_set_text(&response, "old"));
    fail_malloc_bytes = strlen("replacement text") + 1; fail_malloc_once = 1;
    require_error(cc_llm_response_set_text(&response, "replacement text"), CC_ERR_OUT_OF_MEMORY, NULL);
    assert(!fail_malloc_once && response.has_text && strcmp(response.text, "old") == 0);
    cc_llm_response_free(&response);
    cJSON_Hooks hooks = {json_test_allocate,json_test_free};
    json_fail_at = SIZE_MAX;
    const char *json = "[{\"id\":\"first-owned-identifier\",\"function\":{\"name\":\"first\"}},{\"function\":{\"name\":\"second-copy-failure-name\"}}]";
    watch_malloc_bytes = strlen("first-owned-identifier") + 1; watched_frees = 0;
    fail_malloc_bytes = strlen("second-copy-failure-name") + 1; fail_malloc_once = 1;
    cc_tool_call_list_t list; cJSON_InitHooks(&hooks);
    cc_result_t result = cc_tool_call_list_from_json(json, &list); cJSON_InitHooks(NULL);
    require_error(result, CC_ERR_OUT_OF_MEMORY, NULL);
    assert(!fail_malloc_once && !watch_malloc_bytes && !watched_pointer && watched_frees == 1 && !json_live && !list.items);
#endif
}

/** @brief 顺序验证消息与调用的创建、别名、容量、严格解析和资源归还。 @return 全部断言通过为零，失败由断言终止。 */
int main(void)
{
    test_creation_and_self_copy();
    test_list_alias_and_shape();
    test_replacement_and_move();
    test_json_defaults();
    test_json_rejection();
    test_json_allocation_failures();
    test_owned_copy_failures();
    return 0;
}
