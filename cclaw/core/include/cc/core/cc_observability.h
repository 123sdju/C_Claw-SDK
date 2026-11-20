#ifndef CC_OBSERVABILITY_H
#define CC_OBSERVABILITY_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_event_bus.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 观测事件 schema 的版本号。下游日志、指标和调试 UI 可以用它做兼容判断。 */
#define CC_OBSERVABILITY_SCHEMA_VERSION 1

/* 运行时主流程事件。 */
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
#define CC_OBS_EVENT_LLM_USAGE "llm.usage"
#define CC_OBS_EVENT_CONTEXT_SUMMARY_CACHE "llm.context.summary_cache"

/* 工具执行和审批事件。 */
#define CC_OBS_EVENT_TOOL_START "tool.start"
#define CC_OBS_EVENT_TOOL_FINISH "tool.finish"
#define CC_OBS_EVENT_TOOL_ERROR "tool.error"
#define CC_OBS_EVENT_TOOL_INTENT "tool.intent"
#define CC_OBS_EVENT_TOOL_RECOVERY_REQUIRED "tool.recovery.required"
#define CC_OBS_EVENT_TOOL_RECOVERY_RESOLVED "tool.recovery.resolved"
#define CC_OBS_EVENT_APPROVAL_REQUIRED "approval.required"
#define CC_OBS_EVENT_APPROVAL_APPROVED "approval.approved"
#define CC_OBS_EVENT_APPROVAL_DENIED "approval.denied"

/* 记忆检索及写入事件，由具体适配器按实际操作发布。 */
#define CC_OBS_EVENT_MEMORY_QUERY "memory.query"
#define CC_OBS_EVENT_MEMORY_WRITE "memory.write"

/* 流式事件沿 stream.* 事件族命名，载荷使用统一观测结构。 */
#define CC_OBS_EVENT_STREAM_TEXT "stream.text"
#define CC_OBS_EVENT_STREAM_THINKING "stream.thinking"
#define CC_OBS_EVENT_STREAM_TOOL_START "stream.tool.start"
#define CC_OBS_EVENT_STREAM_TOOL_DELTA "stream.tool.delta"
#define CC_OBS_EVENT_STREAM_TOOL_END "stream.tool.end"
#define CC_OBS_EVENT_STREAM_FINISHED "stream.finished"
#define CC_OBS_EVENT_STREAM_ARTIFACT "stream.artifact"
#define CC_OBS_EVENT_STREAM_PROVIDER_WARNING "stream.provider.warning"
#define CC_OBS_EVENT_STREAM_ERROR "stream.error"

/* 无法归入具体事件族时使用的通用错误事件。 */
#define CC_OBS_EVENT_ERROR_RUNTIME "error.runtime"

/*
 * 统一观测事件描述。
 *
 * 所有字符串都是借用指针，调用方只需保证它们在函数调用期间有效。
 * attributes_json 为可选完整 JSON 对象文本，成功解析后作为 attributes 子树。
 * error 为可选结构化错误，构造层复制 code/message/detail，总线发布层处理脱敏。
 * size 为当前布局信息，现有实现按完整布局访问，调用方保持全部借用至调用返回。
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

/**
 * @brief 为借用事件构造完整的统一结构 JSON。
 * @param[in] event 调用期间稳定的完整描述及嵌套借用，事件名称须非 NULL。
 * @return 成功返回调用者 free 的独占字符串，参数、属性格式/类型或任一分配失败返回 NULL。
 * @note NULL 可选字段按各字段缺省规则省略或写空串，构造失败撤销临时树。
 * 返回载荷保留输入正文，经过事件总线发布时执行统一脱敏。
 */
char *cc_observability_event_json(const cc_observability_event_t *event);

/**
 * @brief 构造完整观测载荷并通过总线发布。
 * @param[in,out] bus 存活总线的借用，拥有者协调发布、回调与关闭寿命。
 * @param[in] event 调用期间稳定的完整事件及其嵌套借用。
 * @return 参数错误为 INVALID_ARGUMENT，构造失败为 JSON，其余为总线拥有结果，调用者释放诊断。
 * @note 总线按自身契约复制和脱敏，调用方按业务需要处理观测失败。
 */
cc_result_t cc_observability_publish(
    cc_event_bus_t *bus,
    const cc_observability_event_t *event
);

#ifdef __cplusplus
}
#endif

#endif
