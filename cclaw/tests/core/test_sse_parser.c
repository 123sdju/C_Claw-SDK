#include "cc/app/cc_sse_parser.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>
#include <string.h>

typedef struct event_list {
    char *items[8];
    int count;
} event_list_t;

/* 收集 parser 输出的 data payload，验证跨 chunk 拼接结果。 */
static cc_result_t collect_event(const char *data, void *user_data)
{
    event_list_t *events = (event_list_t *)user_data;
    if (events->count >= 8) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Too many events");
    events->items[events->count] = cc_copy_string(data ? data : "");
    if (!events->items[events->count]) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "copy failed");
    events->count++;
    return cc_result_ok();
}

/*
 * 验证 SSE parser 的核心契约。
 *
 * 覆盖注释行忽略、单个 data 被多次 feed 切开、多行 data 用换行合并、[DONE] 事件以及
 * finish 时尾部处理。
 */
int main(void)
{
    cc_sse_parser_t *parser = NULL;
    if (cc_sse_parser_create(&parser).code != CC_OK) return 1;

    event_list_t events;
    memset(&events, 0, sizeof(events));

    if (cc_sse_parser_feed(parser, ":heartbeat\n", strlen(":heartbeat\n"), collect_event, &events).code != CC_OK) return 1;
    if (cc_sse_parser_feed(parser, "data: {\"id\":1,", strlen("data: {\"id\":1,"), collect_event, &events).code != CC_OK) return 1;
    if (cc_sse_parser_feed(parser, "\"result\":", strlen("\"result\":"), collect_event, &events).code != CC_OK) return 1;
    if (cc_sse_parser_feed(parser, "{} }\n\n", strlen("{} }\n\n"), collect_event, &events).code != CC_OK) return 1;
    if (cc_sse_parser_feed(parser, "data: first\n", strlen("data: first\n"), collect_event, &events).code != CC_OK) return 1;
    if (cc_sse_parser_feed(parser, "data: second\n\n", strlen("data: second\n\n"), collect_event, &events).code != CC_OK) return 1;
    if (cc_sse_parser_feed(parser, "data: [DONE]\n\n", strlen("data: [DONE]\n\n"), collect_event, &events).code != CC_OK) return 1;
    if (cc_sse_parser_finish(parser, collect_event, &events).code != CC_OK) return 1;

    int ok = events.count == 3 &&
        strcmp(events.items[0], "{\"id\":1,\"result\":{} }") == 0 &&
        strcmp(events.items[1], "first\nsecond") == 0 &&
        strcmp(events.items[2], "[DONE]") == 0;

    for (int i = 0; i < events.count; i++) free(events.items[i]);
    cc_sse_parser_destroy(parser);
    return ok ? 0 : 1;
}
