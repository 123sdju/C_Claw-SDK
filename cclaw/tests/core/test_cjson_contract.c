#if defined(CC_TEST_CJSON_THREADS) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "../../core/vendor/cjson/cJSON.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#endif

#if defined(CC_TEST_CJSON_THREADS)
#include <pthread.h>
#endif

static void *live_blocks[4096];
static size_t live_count;
static size_t allocation_attempts;
static size_t fail_at;

#if defined(CC_TEST_CJSON_FORMAT)
static int format_failure;

/** @brief 定向模拟数字格式化失败，其余调用转发标准 vsnprintf。
 * @param[out] buffer 调用者拥有的目标缓冲。
 * @param[in] size 缓冲容量，测试调用保证可由 int 表示。
 * @param[in] format 稳定格式文本，其余可变实参按该格式读取。
 * @param[in] ... 按 format 类型和顺序提供的格式化实参，借用期覆盖转发调用。
 * @return 正常返回标准长度，故障模式返回负数或超过容量的需求长度。
 */
int __wrap_snprintf(char *buffer, size_t size, const char *format, ...)
{
    cJSON_bool number_format = size == 26 &&
        (strcmp(format, "%1.15g") == 0 || strcmp(format, "%1.17g") == 0 ||
         strcmp(format, "%d") == 0 || strcmp(format, "null") == 0);
    if (number_format && format_failure < 0)
    {
        return -1;
    }
    if (number_format && format_failure > 0)
    {
        assert(size <= INT_MAX - 5U);
        if (size)
        {
            memset(buffer, 'X', size - 1);
            buffer[size - 1] = '\0';
        }
        return (int)size + 5;
    }
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return length;
}

/** @brief 检查数字格式化负值和容量溢出时完整撤销输出。
 * @post 故障模式恢复，打印缓冲和节点全部归还。
 */
static void test_format_failures(void)
{
    cJSON *number = cJSON_CreateNumber(1.25);
    assert(number);
    size_t baseline = live_count;
    format_failure = -1;
    assert(!cJSON_PrintUnformatted(number) && live_count == baseline);
    format_failure = 1;
    assert(!cJSON_PrintUnformatted(number) && live_count == baseline);
    format_failure = 0;
    char *text = cJSON_PrintUnformatted(number);
    assert(text && strcmp(text, "1.25") == 0);
    cJSON_free(text);
    cJSON_Delete(number);
    assert(live_count == 0);
}
#endif

/** @brief 为实际 vendor 提供可定位的分配故障与拥有计数。
 * @param[in] size 请求字节数，零返回独占哨兵，sanitizer 将其标为不可读写。
 * @return 独占块或 NULL，指定失败点不申请内存。
 */
static void *tracked_allocate(size_t size)
{
    allocation_attempts++;
    if (fail_at && allocation_attempts == fail_at)
    {
        return NULL;
    }
    void *block = malloc(size ? size : 8);
    if (block)
    {
        assert(live_count < sizeof(live_blocks) / sizeof(live_blocks[0]));
        live_blocks[live_count++] = block;
#if defined(__SANITIZE_ADDRESS__)
        if (size == 0)
        {
            __asan_poison_memory_region(block, 8);
        }
#endif
    }
    return block;
}

/** @brief 检查每块仅归还一次并释放实际分配。
 * @param[in] block 已记录独占块或 NULL。
 * @post 非空块从拥有集合移除，重复释放直接触发断言。
 */
static void tracked_deallocate(void *block)
{
    if (!block)
    {
        return;
    }
    size_t index = 0;
    while (index < live_count && live_blocks[index] != block)
    {
        index++;
    }
    assert(index < live_count);
    live_blocks[index] = live_blocks[--live_count];
#if defined(__SANITIZE_ADDRESS__)
    __asan_unpoison_memory_region(block, 1);
#endif
    free(block);
}

/** @brief 验证非法 Unicode 被拒绝且合法零码元保持原始 vendor 语义。
 * @post 全部成功树和失败部分节点已归还，错误位置只借用静态输入。
 */
static void test_unicode_and_parse_positions(void)
{
    const char *invalid[] = {
        "\"\\uZZZZ\"", "\"\\u12x4\"", "\"\\uD800\\uZZZZ\"",
        "\"\\uD800\"", "\"\\uDC00\"", "\"\\uD800\\u0041\""
    };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++)
    {
        const char *end = NULL;
        cJSON *tree = cJSON_ParseWithOpts(invalid[index], &end, 1);
        assert(!tree && end && cJSON_GetErrorPtr() == end);
        assert(live_count == 0);
    }
    cJSON *tree = cJSON_Parse("\"\\u0000x\"");
    assert(tree && tree->valuestring[0] == '\0' && tree->valuestring[1] == 'x');
    cJSON_Delete(tree);
    tree = cJSON_Parse("\"\\uD83D\\uDE00\"");
    assert(tree && strcmp(tree->valuestring, "\xF0\x9F\x98\x80") == 0);
    cJSON_Delete(tree);
    tree = cJSON_Parse("1 trailing");
    assert(tree && cJSON_GetNumberValue(tree) == 1 && !cJSON_GetErrorPtr());
    cJSON_Delete(tree);
    const char bom_number[] = {'\xEF', '\xBB', '\xBF', '0'};
    tree = cJSON_ParseWithLength(bom_number, sizeof(bom_number));
    assert(tree && tree->valuedouble == 0);
    cJSON_Delete(tree);
    assert(!cJSON_ParseWithLength(bom_number, 3));
    const char *end = "retained";
    const char *saved = end;
    assert(!cJSON_ParseWithOpts(NULL, &end, 1) && end == saved);
    assert(!cJSON_ParseWithLengthOpts(NULL, 0, &end, 1) && end == saved);
    assert(!cJSON_GetErrorPtr());
    assert(live_count == 0);
}

/** @brief 覆盖 NaN 缓存和字符串自身/内部子串更新。
 * @post 所有树与打印文本归还，失败扩容保持旧字符串及其地址。
 */
static void test_numbers_and_string_aliases(void)
{
    cJSON *number = cJSON_CreateNumber(NAN);
    assert(number && isnan(number->valuedouble) && number->valueint == 0);
    char *printed = cJSON_PrintUnformatted(number);
    assert(printed && strcmp(printed, "null") == 0);
    cJSON_free(printed);
    assert(isnan(cJSON_SetNumberHelper(number, NAN)) && number->valueint == 0);
    cJSON_SetNumberHelper(number, INFINITY);
    assert(number->valueint == INT_MAX);
    cJSON_SetNumberHelper(number, -INFINITY);
    assert(number->valueint == INT_MIN);
    cJSON_Delete(number);

    cJSON *string = cJSON_CreateString("prefix-tail");
    assert(string);
    char *original = string->valuestring;
    assert(cJSON_SetValuestring(string, original) == original);
    assert(cJSON_SetValuestring(string, original + 7) == original);
    assert(strcmp(string->valuestring, "tail") == 0);
    fail_at = allocation_attempts + 1;
    assert(!cJSON_SetValuestring(string, "a replacement longer than the existing buffer"));
    fail_at = 0;
    assert(string->valuestring == original && strcmp(original, "tail") == 0);
    cJSON_Delete(string);
    assert(live_count == 0);
}

/** @brief 核对错误父节点、已挂接替代项、自环和合法 detach 再接管。
 * @post 拒绝路径保留两棵原树及新节点，最终全部独占资源归还。
 */
static void test_membership_and_transfer(void)
{
    cJSON *left = cJSON_CreateArray();
    cJSON *right = cJSON_CreateArray();
    cJSON *first = cJSON_CreateNumber(1);
    cJSON *foreign = cJSON_CreateNumber(2);
    cJSON *replacement = cJSON_CreateNumber(3);
    assert(left && right && first && foreign && replacement);
    assert(cJSON_AddItemToArray(left, first));
    assert(cJSON_AddItemToArray(right, foreign));
    assert(!cJSON_DetachItemViaPointer(left, foreign));
    assert(!cJSON_ReplaceItemViaPointer(left, foreign, replacement));
    assert(!cJSON_ReplaceItemViaPointer(left, foreign, foreign));
    assert(left->child == first && right->child == foreign);
    assert(!replacement->next && !replacement->prev);
    assert(!cJSON_AddItemToArray(left, first));
    assert(!cJSON_AddItemToArray(left, foreign));
    assert(!cJSON_InsertItemInArray(left, 0, left));
    assert(!cJSON_ReplaceItemViaPointer(left, first, left));
    assert(!cJSON_ReplaceItemViaPointer(left, first, foreign));
    assert(cJSON_ReplaceItemViaPointer(left, first, first));
    assert(left->child == first && right->child == foreign);
    assert(cJSON_DetachItemViaPointer(right, foreign) == foreign);
    assert(!foreign->next && !foreign->prev);
    assert(cJSON_AddItemToArray(left, foreign));
    assert(cJSON_GetArraySize(left) == 2 && !right->child);
    assert(cJSON_ReplaceItemViaPointer(left, first, replacement));
    assert(left->child == replacement && replacement->next == foreign);
    cJSON_Delete(left);
    cJSON_Delete(right);
    assert(live_count == 0);
}

/** @brief 覆盖 replacement 键借用、自同对象及复制失败事务边界。
 * @post 成功原成员已释放，OOM 或缺键时原树和 replacement 原键保持。
 */
static void test_object_replacement_aliases(void)
{
    cJSON *object = cJSON_CreateObject();
    cJSON *holder = cJSON_CreateObject();
    cJSON *old = cJSON_AddNumberToObject(object, "key", 1);
    cJSON *replacement = cJSON_AddNumberToObject(holder, "key", 2);
    assert(object && holder && old && replacement);
    assert(cJSON_DetachItemViaPointer(holder, replacement) == replacement);
    char *original_key = replacement->string;
    fail_at = allocation_attempts + 1;
    assert(!cJSON_ReplaceItemInObject(object, original_key, replacement));
    fail_at = 0;
    assert(object->child == old && replacement->string == original_key);
    assert(strcmp(original_key, "key") == 0);
    assert(!cJSON_ReplaceItemInObject(object, "absent", replacement));
    assert(replacement->string == original_key);
    assert(cJSON_ReplaceItemInObject(object, replacement->string, replacement));
    assert(object->child == replacement && replacement->valuedouble == 2);
    assert(cJSON_ReplaceItemInObjectCaseSensitive(object, replacement->string, replacement));
    assert(strcmp(replacement->string, "key") == 0);
    cJSON_Delete(holder);
    cJSON_Delete(object);
    assert(live_count == 0);
}

/** @brief 覆盖引用包装在键复制 OOM 时撤销且不归还借用内容。
 * @post 各失败点不泄漏包装，引用成功后删除包装仍保留原文本。
 */
static void test_reference_failures(void)
{
    cJSON *source = cJSON_CreateString("borrowed");
    cJSON *object = cJSON_CreateObject();
    assert(source && object);
    size_t baseline = live_count;
    for (size_t failure = 1; failure <= 2; failure++)
    {
        fail_at = allocation_attempts + failure;
        assert(!cJSON_AddItemReferenceToObject(object, "key", source));
        fail_at = 0;
        assert(live_count == baseline && !object->child);
        assert(strcmp(source->valuestring, "borrowed") == 0);
    }
    assert(cJSON_AddItemReferenceToObject(object, "key", source));
    cJSON_Delete(object);
    assert(strcmp(source->valuestring, "borrowed") == 0);
    cJSON_Delete(source);
    assert(live_count == 0);
}

/** @brief 验证空/非空引用容器只读，同时保留包装自身的正常拥有移动。
 * @post 借用链未被结构操作改写，clone/print 可用，所有拥有包装与原树归还。
 */
static void test_readonly_reference_containers(void)
{
    for (unsigned populated = 0; populated < 2; populated++)
    {
        cJSON *source = cJSON_CreateArray();
        cJSON *object = cJSON_CreateObject();
        assert(source && object);
        if (populated)
        {
            assert(cJSON_AddItemToArray(source, cJSON_CreateNumber(1)));
            assert(cJSON_AddNumberToObject(object, "key", 2));
        }
        cJSON *reference = cJSON_CreateArrayReference(source->child);
        cJSON *object_reference = cJSON_CreateObjectReference(object->child);
        cJSON *candidate = cJSON_CreateNumber(3);
        assert(reference && object_reference && candidate);
        cJSON *array_first = source->child;
        cJSON *object_first = object->child;
        assert(cJSON_GetArraySize(reference) == (int)populated);
        assert(cJSON_GetArrayItem(reference, 0) == array_first);
        assert(cJSON_GetObjectItem(object_reference, "key") == object_first);
        assert(!cJSON_AddItemToArray(reference, candidate));
        assert(!cJSON_AddItemToObject(object_reference, "new", candidate));
        assert(!cJSON_AddItemToObjectCS(object_reference, "new", candidate));
        assert(!cJSON_AddItemReferenceToArray(reference, candidate));
        assert(!cJSON_AddItemReferenceToObject(object_reference, "new", candidate));
        assert(!cJSON_InsertItemInArray(reference, 0, candidate));
        assert(!cJSON_DetachItemViaPointer(reference, array_first));
        assert(!cJSON_DetachItemFromArray(reference, 0));
        assert(!cJSON_DetachItemFromObject(object_reference, "key"));
        assert(!cJSON_DetachItemFromObjectCaseSensitive(object_reference, "key"));
        assert(!cJSON_ReplaceItemViaPointer(reference, array_first, candidate));
        assert(!cJSON_ReplaceItemViaPointer(reference, array_first, array_first));
        assert(!cJSON_ReplaceItemInArray(reference, 0, candidate));
        assert(!cJSON_ReplaceItemInObject(object_reference, "key", candidate));
        assert(!cJSON_ReplaceItemInObjectCaseSensitive(object_reference, "key", candidate));
        cJSON_DeleteItemFromArray(reference, 0);
        cJSON_DeleteItemFromObject(object_reference, "key");
        cJSON_DeleteItemFromObjectCaseSensitive(object_reference, "key");
        size_t baseline = live_count;
        assert(!cJSON_AddNullToObject(object_reference, "new"));
        assert(live_count == baseline);
        assert(source->child == array_first && reference->child == array_first);
        assert(object->child == object_first && object_reference->child == object_first);
        assert(!candidate->prev && !candidate->next && !candidate->string);
        cJSON *copy = cJSON_Duplicate(reference, 1);
        assert(copy && !(copy->type & cJSON_IsReference));
        assert(cJSON_AddItemToArray(copy, cJSON_CreateNumber(4)));
        char *printed = cJSON_PrintUnformatted(reference);
        assert(printed && strcmp(printed, populated ? "[1]" : "[]") == 0);
        cJSON_free(printed);
        cJSON_Delete(copy);
        cJSON *owner = cJSON_CreateArray();
        assert(owner && cJSON_AddItemToArray(owner, reference));
        assert(cJSON_DetachItemViaPointer(owner, reference) == reference);
        assert(cJSON_AddItemToArray(owner, reference));
        assert(cJSON_ReplaceItemViaPointer(owner, reference, candidate));
        cJSON_Delete(owner);
        cJSON_Delete(object_reference);
        assert(source->child == array_first && object->child == object_first);
        cJSON_Delete(source);
        cJSON_Delete(object);
        assert(live_count == 0);
    }
}

/** @brief 覆盖零初始打印容量与合法末尾反斜杠的就地压缩。
 * @post 拥有打印结果和树已归还，零容量调用者缓冲保持哨兵。
 */
static void test_print_and_minify(void)
{
    cJSON *tree = cJSON_Parse("{\"a\":1,\"b\":[true,\"text\"]}");
    assert(tree);
    char *printed = cJSON_PrintBuffered(tree, 0, 0);
    assert(printed && strcmp(printed, "{\"a\":1,\"b\":[true,\"text\"]}") == 0);
    cJSON_free(printed);
    char sentinel = 'Q';
    assert(!cJSON_PrintPreallocated(tree, &sentinel, 0, 0) && sentinel == 'Q');
    cJSON_Delete(tree);
    char text[] = " { \"a\" : \"\\\\\", \"b\" : 1 } ";
    cJSON_Minify(text);
    assert(strcmp(text, "{\"a\":\"\\\\\",\"b\":1}") == 0);
    tree = cJSON_ParseWithOpts(text, NULL, 1);
    assert(tree && strcmp(cJSON_GetObjectItem(tree, "a")->valuestring, "\\") == 0);
    cJSON_Delete(tree);
    assert(live_count == 0);
}

/** @brief 对实际解析、打印和深复制逐个 JSON 分配位置注入失败。
 * @post 失败无部分拥有输出，成功输出归还后 live_count 回到操作前基线。
 */
static void test_allocation_failures(void)
{
    const char *json = "{\"key\":[1,\"long text\",{\"nested\":true}]}";
    cJSON *source = cJSON_Parse(json);
    assert(source);
    size_t baseline = live_count;
    for (unsigned operation = 0; operation < 3; operation++)
    {
        size_t first = allocation_attempts;
        cJSON *tree = NULL;
        char *text = NULL;
        if (operation == 0) tree = cJSON_Parse(json);
        if (operation == 1) text = cJSON_Print(source);
        if (operation == 2) tree = cJSON_Duplicate(source, 1);
        size_t attempts = allocation_attempts - first;
        assert(tree || text);
        cJSON_Delete(tree);
        cJSON_free(text);
        assert(live_count == baseline);
        for (size_t failure = 1; failure <= attempts; failure++)
        {
            tree = NULL;
            text = NULL;
            fail_at = allocation_attempts + failure;
            if (operation == 0) tree = cJSON_Parse(json);
            if (operation == 1) text = cJSON_Print(source);
            if (operation == 2) tree = cJSON_Duplicate(source, 1);
            fail_at = 0;
            assert(!tree && !text && live_count == baseline);
        }
    }
    cJSON_Delete(source);
    assert(live_count == 0);
}

#if defined(CC_TEST_CJSON_THREADS)
typedef struct
{
    const char *input; /* 静态错误文本，覆盖两线程检查期。 */
    pthread_barrier_t *barrier; /* 主线程拥有的两方屏障。 */
} parse_thread_context_t;

/** @brief 在屏障两侧核对每线程失败位置、成功清槽及静态版本文本。
 * @param[in] argument 主线程借用上下文，覆盖 pthread_join。
 * @return NULL，树在工作线程内归还，不交付拥有结果。
 */
static void *parse_thread(void *argument)
{
    parse_thread_context_t *context = argument;
    const char *end = NULL;
    assert(!cJSON_ParseWithOpts(context->input, &end, 1) && end);
    pthread_barrier_wait(context->barrier);
    assert(cJSON_GetErrorPtr() == end);
    cJSON *tree = cJSON_Parse("true");
    assert(tree);
    cJSON_Delete(tree);
    pthread_barrier_wait(context->barrier);
    assert(!cJSON_GetErrorPtr());
    for (size_t index = 0; index < 1000; index++)
    {
        assert(strcmp(cJSON_Version(), "1.7.19") == 0);
    }
    return NULL;
}

/** @brief 用真实 pthread 屏障验证错误槽互不覆盖。
 * @post 所有线程已 join，输入/屏障仍有效，主线程错误位置未被工作线程改变。
 */
static void test_thread_local_errors(void)
{
    const char *main_input = "bad-main";
    const char *main_end = NULL;
    assert(!cJSON_ParseWithOpts(main_input, &main_end, 1) && main_end);
    pthread_barrier_t barrier;
    assert(pthread_barrier_init(&barrier, NULL, 2) == 0);
    parse_thread_context_t contexts[] = {{"[", &barrier}, {"{bad-worker", &barrier}};
    pthread_t threads[2];
    assert(pthread_create(&threads[0], NULL, parse_thread, &contexts[0]) == 0);
    assert(pthread_create(&threads[1], NULL, parse_thread, &contexts[1]) == 0);
    assert(pthread_join(threads[0], NULL) == 0);
    assert(pthread_join(threads[1], NULL) == 0);
    assert(cJSON_GetErrorPtr() == main_end);
    assert(pthread_barrier_destroy(&barrier) == 0);
}
#endif

/** @brief 串行安装计数钩子完成契约回归，再恢复标准钩子执行线程用例。
 * @return 全部断言满足返回零，钩子切换时保证无存活自定义分配。
 */
int main(void)
{
    cJSON_Hooks hooks = {tracked_allocate, tracked_deallocate};
    cJSON_InitHooks(&hooks);
    test_unicode_and_parse_positions();
    test_numbers_and_string_aliases();
    test_membership_and_transfer();
    test_object_replacement_aliases();
    test_reference_failures();
    test_readonly_reference_containers();
    test_print_and_minify();
    test_allocation_failures();
#if defined(CC_TEST_CJSON_FORMAT)
    test_format_failures();
#endif
    assert(live_count == 0);
    cJSON_InitHooks(NULL);
#if defined(CC_TEST_CJSON_THREADS)
    test_thread_local_errors();
#endif
    puts("cJSON ownership, errors, aliases and allocation failures: PASS");
    return 0;
}
