#include "cc/app/cc_sse_parser.h"
#include "cc/internal/cc_alloc.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct event_list {
    char *items[8];
    int count;
} event_list_t;

#if defined(CC_TEST_SSE_OOM)
static int allocation_failure = -1;
static int allocation_calls;
static char *owned_success_message;
static int success_messages_released;
/**
 * @brief 调用真实零填充分配。
 * @param[in] count 元素数量。
 * @param[in] size 元素大小。
 * @return 真实分配拥有指针或 NULL。
 */
void *__real_calloc(size_t count, size_t size);
/**
 * @brief 调用真实缓冲扩容。
 * @param[in] pointer 原分配或 NULL。
 * @param[in] size 新容量。
 * @return 新分配或 NULL，失败保留原分配。
 */
void *__real_realloc(void *pointer, size_t size);
/**
 * @brief 调用真实释放入口。
 * @param[in] pointer 可由 free 消费的拥有指针或 NULL。
 */
void __real_free(void *pointer);
/**
 * @brief 统计回调成功结果的诊断释放并交给真实分配器。
 * @param[in] pointer 正常拥有分配或 NULL。
 */
void __wrap_free(void *pointer)
{
    if (pointer && pointer == owned_success_message) {
        owned_success_message = NULL;
        success_messages_released++;
    }
    __real_free(pointer);
}
/**
 * @brief 按指定序号模拟零填充分配失败。
 * @param[in] count 元素数量。
 * @param[in] size 元素大小。
 * @return 命中故障时 NULL，否则返回真实分配。
 */
void *__wrap_calloc(size_t count, size_t size)
{
    if (allocation_failure >= 0 && allocation_calls++ == allocation_failure) return NULL;
    return __real_calloc(count, size);
}
/**
 * @brief 按指定序号模拟扩容失败并保留原分配。
 * @param[in] pointer 原分配或 NULL。
 * @param[in] size 新容量。
 * @return 命中故障时 NULL，否则返回真实扩容结果。
 */
void *__wrap_realloc(void *pointer, size_t size)
{
    if (allocation_failure >= 0 && allocation_calls++ == allocation_failure) return NULL;
    return __real_realloc(pointer, size);
}
#endif

/**
 * @brief 深复制回调文本到测试拥有的列表。
 * @param[in] data 仅回调期间有效的事件文本。
 * @param[in,out] user_data 指向最多容纳八条事件的列表。
 * @return 成功、列表容量错误或复制错误，副本由测试清理。
 */
static cc_result_t collect_event(const char *data, void *user_data)
{
    event_list_t *events = (event_list_t *)user_data;
    if (events->count >= 8) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Too many events");
    events->items[events->count] = cc_copy_string(data ? data : "");
    if (!events->items[events->count]) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "copy failed");
    events->count++;
    return cc_result_ok();
}

/**
 * @brief 断言错误码并消费结果诊断。
 * @param[in] result 按值转入唯一释放责任的结果。
 * @param[in] code 预期错误码。
 */
static void expect_code(cc_result_t result, cc_error_code_t code)
{
    assert(result.code == code);
    cc_result_free(&result);
}

/**
 * @brief 释放事件副本并重置测试列表。
 * @param[in,out] events 已初始化的事件列表。
 */
static void clear_events(event_list_t *events)
{
    for (int i = 0; i < events->count; ++i) free(events->items[i]);
    memset(events, 0, sizeof(*events));
}

/**
 * @brief 检查注释、跨块 JSON、多行 data 与完成标记的既有行为。
 */
static void test_provider_payloads(void)
{
    cc_sse_parser_t *parser = NULL;
    expect_code(cc_sse_parser_create(&parser), CC_OK);

    event_list_t events;
    memset(&events, 0, sizeof(events));

    const char *pieces[] = { ":heartbeat\n", "data: {\"id\":1,", "\"result\":", "{} }\n\n",
        "data: first\n", "data: second\n\n", "data: [DONE]\n\n" };
    for (size_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); ++i) {
        expect_code(cc_sse_parser_feed(parser, pieces[i], strlen(pieces[i]), collect_event, &events), CC_OK);
    }
    expect_code(cc_sse_parser_finish(parser, collect_event, &events), CC_OK);

    int ok = events.count == 3 &&
        strcmp(events.items[0], "{\"id\":1,\"result\":{} }") == 0 &&
        strcmp(events.items[1], "first\nsecond") == 0 &&
        strcmp(events.items[2], "[DONE]") == 0;

    assert(ok);
    clear_events(&events);
    cc_sse_parser_destroy(parser);
}

/**
 * @brief 检查 BOM、空 data 以及首空格规则组成的事件序列。
 * @param[in] events 完成输入后的五条测试事件。
 */
static void assert_line_events(const event_list_t *events)
{
    assert(events->count == 5);
    assert(strcmp(events->items[0], "one") == 0);
    assert(strcmp(events->items[1], "") == 0);
    assert(strcmp(events->items[2], "\nlast\n") == 0);
    assert(strcmp(events->items[3], " leading") == 0);
    assert(strcmp(events->items[4], "tail\xe4\xb8\xad\xe6\x96\x87") == 0);
}

/**
 * @brief 穷举所有二分切点和逐字节输入，核对三种行终止符与尾部扩展。
 */
static void test_line_boundaries(void)
{
    const char stream[] = "\xef\xbb\xbf" "data: one\r\n\r\n"
        ":comment\rdata\r\rdata:\ndata: last\ndata:\n\n"
        "event: ignored\ndata:  leading\n\nData: ignored\ndata: tail\xe4\xb8\xad\xe6\x96\x87";
    size_t length = sizeof(stream) - 1;
    for (size_t split = 0; split <= length; ++split) {
        cc_sse_parser_t *parser = NULL;
        event_list_t events = {0};
        expect_code(cc_sse_parser_create(&parser), CC_OK);
        expect_code(cc_sse_parser_feed(parser, stream, split, collect_event, &events), CC_OK);
        expect_code(cc_sse_parser_feed(parser, stream + split, length - split, collect_event, &events), CC_OK);
        expect_code(cc_sse_parser_finish(parser, collect_event, &events), CC_OK);
        expect_code(cc_sse_parser_finish(parser, collect_event, &events), CC_OK);
        assert_line_events(&events);
        clear_events(&events);
        cc_sse_parser_destroy(parser);
    }
    cc_sse_parser_t *parser = NULL;
    event_list_t events = {0};
    expect_code(cc_sse_parser_create(&parser), CC_OK);
    for (size_t i = 0; i < length; ++i) {
        expect_code(cc_sse_parser_feed(parser, stream + i, 1, collect_event, &events), CC_OK);
    }
    expect_code(cc_sse_parser_finish(parser, collect_event, &events), CC_OK);
    assert_line_events(&events);
    clear_events(&events);
    cc_sse_parser_destroy(parser);
}

/**
 * @brief 记录一次事件并要求解析器停止当前块。
 * @param[in] data 借用事件文本，应为 stop。
 * @param[in,out] user_data 回调计数器。
 * @return 拥有诊断的取消结果，由解析调用者清理。
 */
static cc_result_t reject_event(const char *data, void *user_data)
{
    assert(strcmp(data, "stop") == 0);
    (*(int *)user_data)++;
    return cc_result_error(CC_ERR_CANCELLED, "test callback stopped stream");
}

/**
 * @brief 验证 NUL 块在消费前拒绝、回调错误消费事件以及参数边界。
 */
static void test_failure_boundaries(void)
{
    cc_sse_parser_t *parser = NULL;
    event_list_t events = {0};
    expect_code(cc_sse_parser_create(NULL), CC_ERR_INVALID_ARGUMENT);
    expect_code(cc_sse_parser_create(&parser), CC_OK);
    expect_code(cc_sse_parser_feed(NULL, "", 0, NULL, NULL), CC_ERR_INVALID_ARGUMENT);
    expect_code(cc_sse_parser_feed(parser, NULL, 1, NULL, NULL), CC_ERR_INVALID_ARGUMENT);
    expect_code(cc_sse_parser_feed(parser, NULL, 0, NULL, NULL), CC_OK);
    const char damaged[] = "data: no-dispatch\n\n\0data: hidden\n\n";
    expect_code(cc_sse_parser_feed(parser, damaged, sizeof(damaged) - 1, collect_event, &events), CC_ERR_INVALID_ARGUMENT);
    assert(events.count == 0);
    expect_code(cc_sse_parser_finish(parser, collect_event, &events), CC_OK);
    assert(events.count == 0);
    cc_sse_parser_destroy(parser);
    int calls = 0;
    const char stop[] = "data: stop\n\ndata: later\n\n";
    expect_code(cc_sse_parser_create(&parser), CC_OK);
    expect_code(cc_sse_parser_feed(parser, stop, sizeof(stop) - 1, reject_event, &calls), CC_ERR_CANCELLED);
    assert(calls == 1);
    /* 只验证失败事件的消费状态，生产拥有者应在错误后销毁解析器。 */
    expect_code(cc_sse_parser_finish(parser, collect_event, &events), CC_OK);
    assert(events.count == 0);
    cc_sse_parser_destroy(parser);
    expect_code(cc_sse_parser_create(&parser), CC_OK);
    expect_code(cc_sse_parser_feed(parser, "data:\n\n", 7, NULL, NULL), CC_ERR_INVALID_ARGUMENT);
    cc_sse_parser_destroy(parser);
    expect_code(cc_sse_parser_finish(NULL, NULL, NULL), CC_ERR_INVALID_ARGUMENT);
    cc_sse_parser_destroy(NULL);
}

/**
 * @brief 验证 16 KiB 行上限和含多行连接符的 64 KiB 事件上限。
 */
static void test_capacity_limits(void)
{
    cc_sse_parser_t *parser = NULL;
    event_list_t events = {0};
    char *line = malloc(16385);
    assert(line);
    memset(line, 'x', 16385);
    expect_code(cc_sse_parser_create(&parser), CC_OK);
    expect_code(cc_sse_parser_feed(parser, line, 16384, NULL, NULL), CC_OK);
    expect_code(cc_sse_parser_feed(parser, line, 1, NULL, NULL), CC_ERR_LIMIT_EXCEEDED);
    cc_sse_parser_destroy(parser);
    free(line);
    char data_line[8198];
    memcpy(data_line, "data: ", 6);
    memset(data_line + 6, 'x', 8191);
    data_line[8197] = '\n';
    expect_code(cc_sse_parser_create(&parser), CC_OK);
    for (int i = 0; i < 8; ++i) {
        expect_code(cc_sse_parser_feed(parser, data_line, sizeof(data_line), collect_event, &events), CC_OK);
    }
    expect_code(cc_sse_parser_feed(parser, "data:\n\n", 7, collect_event, &events), CC_OK);
    assert(events.count == 1 && strlen(events.items[0]) == 65536);
    clear_events(&events);
    cc_sse_parser_destroy(parser);
    expect_code(cc_sse_parser_create(&parser), CC_OK);
    for (int i = 0; i < 8; ++i) {
        expect_code(cc_sse_parser_feed(parser, data_line, sizeof(data_line), collect_event, &events), CC_OK);
    }
    expect_code(cc_sse_parser_feed(parser, "data:\n", 6, collect_event, &events), CC_OK);
    expect_code(cc_sse_parser_feed(parser, "data:\n", 6, collect_event, &events), CC_ERR_LIMIT_EXCEEDED);
    assert(events.count == 0);
    cc_sse_parser_destroy(parser);
}

#if defined(CC_TEST_SSE_OOM)
/**
 * @brief 为同步事件返回带拥有诊断的成功结果。
 * @param[in] data 本次同步借用事件，应为 owned。
 * @param[in,out] user_data 忽略的上下文，可为 NULL。
 * @return 拥有诊断的 CC_OK，交解析器消费或转交 finish 的调用者。
 */
static cc_result_t own_success_event(const char *data, void *user_data)
{
    (void)user_data;
    assert(strcmp(data, "owned") == 0);
    assert(owned_success_message == NULL);
    cc_result_t result = cc_result_error(CC_OK, "owned callback diagnostic");
    assert(result.message);
    owned_success_message = result.message;
    return result;
}

/**
 * @brief 核对 feed 消费中间成功诊断和 finish 交接最终诊断的释放责任。
 */
static void test_success_result_ownership(void)
{
    cc_sse_parser_t *parser = NULL;
    success_messages_released = 0;
    expect_code(cc_sse_parser_create(&parser), CC_OK);
    const char stream[] = "data: owned\n\ndata: owned\n\ndata: owned";
    expect_code(cc_sse_parser_feed(parser, stream, sizeof(stream) - 1, own_success_event, NULL), CC_OK);
    assert(success_messages_released == 2 && !owned_success_message);
    cc_result_t result = cc_sse_parser_finish(parser, own_success_event, NULL);
    assert(result.message == owned_success_message && success_messages_released == 2);
    expect_code(result, CC_OK);
    assert(success_messages_released == 3 && !owned_success_message);
    cc_sse_parser_destroy(parser);
}

/**
 * @brief 对构造及各缓冲扩容依次注入失败并核对输出和事件提交。
 */
static void test_allocation_failures(void)
{
    cc_sse_parser_t *parser = (cc_sse_parser_t *)1;
    allocation_calls = 0;
    allocation_failure = 0;
    cc_result_t result = cc_sse_parser_create(&parser);
    allocation_failure = -1;
    expect_code(result, CC_ERR_OUT_OF_MEMORY);
    assert(parser == NULL);
    int reached_success = 0;
    char stream[528];
    memcpy(stream, "data: ", 6);
    memset(stream + 6, 'a', 256);
    stream[262] = '\n';
    memcpy(stream + 263, "data: ", 6);
    memset(stream + 269, 'b', 256);
    stream[525] = '\n';
    stream[526] = '\n';
    stream[527] = '\0';
    for (int failure = 0; failure < 8; ++failure) {
        event_list_t events = {0};
        expect_code(cc_sse_parser_create(&parser), CC_OK);
        allocation_calls = 0;
        allocation_failure = failure;
        result = cc_sse_parser_feed(parser, stream, sizeof(stream) - 1, collect_event, &events);
        allocation_failure = -1;
        if (result.code == CC_OK) {
            expect_code(result, CC_OK);
            assert(events.count == 1 && strlen(events.items[0]) == 513);
            assert(events.items[0][0] == 'a' && events.items[0][256] == '\n' &&
                   events.items[0][512] == 'b');
            reached_success = 1;
        } else {
            expect_code(result, CC_ERR_OUT_OF_MEMORY);
            assert(events.count == 0);
        }
        clear_events(&events);
        cc_sse_parser_destroy(parser);
        if (reached_success) break;
    }
    assert(reached_success);
}
#endif

/**
 * @brief 执行协议切块、失败和容量回归，所有拥有资源在每个成功场景后清理。
 * @return 全部断言通过返回零。
 */
int main(void)
{
    test_provider_payloads();
    test_line_boundaries();
    test_failure_boundaries();
    test_capacity_limits();
#if defined(CC_TEST_SSE_OOM)
    test_allocation_failures();
    test_success_result_ownership();
#endif
    return 0;
}
