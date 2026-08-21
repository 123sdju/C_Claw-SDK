#ifndef CC_OBSERVABILITY_H
#define CC_OBSERVABILITY_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_event_bus.h"

#include <stddef.h>

/* 观测事件 schema 的版本号。下游日志、指标和调试 UI 可以用它做兼容判断。 */
#define CC_OBSERVABILITY_SCHEMA_VERSION 1

/* Runtime 主流程事件。 */
#define CC_OBS_EVENT_RUN_FINISHED "run.finished"

/* LLM 请求/响应事件。这里使用新的 start/finish 命名，避免继续依赖旧事件名。 */
#define CC_OBS_EVENT_LLM_REQUEST_START "llm.request.start"
#define CC_OBS_EVENT_LLM_REQUEST_CONFIG "llm.request.config"
#define CC_OBS_EVENT_LLM_RESPONSE_FINISH "llm.response.finish"
#define CC_OBS_EVENT_LLM_HTTP_ATTEMPT "llm.http.attempt"
#define CC_OBS_EVENT_LLM_CONTEXT_BUILD "llm.context.build"
#define CC_OBS_EVENT_LLM_TOOLS_SCHEMA_BUILD "llm.tools.schema_build"
#define CC_OBS_EVENT_LLM_PROVIDER_SUBMIT "llm.provider.submit"
#define CC_OBS_EVENT_LLM_FIRST_BODY "llm.first_body"
#define CC_OBS_EVENT_LLM_STREAM_FINISH "llm.stream.finish"

/* 工具执行和审批事件。 */
#define CC_OBS_EVENT_TOOL_START "tool.start"
#define CC_OBS_EVENT_TOOL_FINISH "tool.finish"
#define CC_OBS_EVENT_TOOL_ERROR "tool.error"
#define CC_OBS_EVENT_APPROVAL_REQUIRED "approval.required"
#define CC_OBS_EVENT_APPROVAL_APPROVED "approval.approved"
#define CC_OBS_EVENT_APPROVAL_DENIED "approval.denied"

/* Memory 检索/写入事件，具体 adapter 可按需发布。 */
#define CC_OBS_EVENT_MEMORY_QUERY "memory.query"
#define CC_OBS_EVENT_MEMORY_WRITE "memory.write"

/* Stream 事件仍使用 stream.* family，但 payload 统一为 observability schema JSON。 */
#define CC_OBS_EVENT_STREAM_TEXT "stream.text"
#define CC_OBS_EVENT_STREAM_THINKING "stream.thinking"
#define CC_OBS_EVENT_STREAM_TOOL_START "stream.tool.start"
#define CC_OBS_EVENT_STREAM_TOOL_DELTA "stream.tool.delta"
#define CC_OBS_EVENT_STREAM_TOOL_END "stream.tool.end"
#define CC_OBS_EVENT_STREAM_FINISHED "stream.finished"
#define CC_OBS_EVENT_STREAM_ARTIFACT "stream.artifact"
#define CC_OBS_EVENT_STREAM_PROVIDER_WARNING "stream.provider.warning"
#define CC_OBS_EVENT_STREAM_ERROR "stream.error"

/* 通用错误事件，供无法归入具体 family 的错误使用。 */
#define CC_OBS_EVENT_ERROR_RUNTIME "error.runtime"

/*
 * 统一观测事件描述。
 *
 * 所有字符串都是借用指针，调用方只需保证它们在函数调用期间有效。
 * attributes_json 必须是 JSON object 文本；解析成功后会挂到 payload.attributes。
 * error 为可选结构化错误，发布层会提取 code/message/detail 并统一脱敏。
 */
typedef struct cc_observability_event {
    size_t size;
    const char *event;
    const char *session_id;
    const char *run_id;
    int step;
    const char *status;
    const cc_result_t *error;
    const char *message;
    const char *attributes_json;
} cc_observability_event_t;

/*
 * 构造统一 schema JSON。
 *
 * 返回值由调用方 free()；attributes_json 必须是 JSON object，否则返回 NULL，让调用方
 * 明确知道事件 schema 不合法。该函数不发布事件，适合单元测试和 adapter 在发送前
 * 检查 payload。
 */
char *cc_observability_event_json(const cc_observability_event_t *event);

/*
 * 通过 event bus 发布统一观测事件。
 *
 * event bus 底层仍会做 redaction；业务层不应再直接拼 JSON 调底层发布函数。失败时
 * 返回 CC_ERR_JSON 或 event bus 的错误码，调用方可以把它当作观测失败而不是业务失败。
 */
cc_result_t cc_observability_publish(
    cc_event_bus_t *bus,
    const cc_observability_event_t *event
);

#endif
