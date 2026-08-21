

#ifndef CC_TOOL_CALL_H
#define CC_TOOL_CALL_H

#include "cc/core/cc_media.h"
#include "cc/core/cc_result.h"
#include <stddef.h>

/*
 * LLM 请求执行工具时产生的一次调用描述。
 *
 * id/name/arguments_json 都由结构拥有。arguments_json 保持 JSON 字符串形式，是为了
 * 不把核心 SDK 绑定到某个 schema AST；真正的参数校验由 tool executor 在执行前做。
 */
typedef struct cc_tool_call {
    char *id;
    char *name;
    char *arguments_json;
} cc_tool_call_t;

/* tool call 动态数组；message 和 LLM response 都通过它承载多个并行调用。 */
typedef struct cc_tool_call_list {
    cc_tool_call_t *items;
    size_t count;
    size_t capacity;
} cc_tool_call_list_t;

/*
 * 工具执行结果。
 *
 * ok 为 0 表示可恢复的工具级失败，不等同于 SDK 崩溃；text/error/metadata 由
 * result 拥有，artifacts 支持工具返回文件或媒体。线程安全由调用方保证。
 */
typedef struct cc_tool_result {
    int ok;
    char *text;
    char *error;
    char *metadata;
    cc_media_artifact_list_t artifacts;
} cc_tool_result_t;

/*
 * provider 返回的一次 LLM 响应。
 *
 * text/content/artifacts/tool_calls/reasoning_content 都由 response 拥有，调用方必须
 * cc_llm_response_free()。该结构是 provider port 与 runtime 之间的 DTO，不做内部
 * 加锁；provider 填充完成后再交给 runtime 消费。
 */
typedef struct cc_llm_response {
    int has_text;
    char *text;
    cc_content_parts_t content;
    cc_media_artifact_list_t artifacts;
    cc_tool_call_list_t tool_calls;
    int finished;
    char *reasoning_content;
} cc_llm_response_t;

/* 创建堆上 tool call；成功后调用方用 cc_tool_call_destroy() 释放。 */
cc_result_t cc_tool_call_create(
    const char *id,
    const char *name,
    const char *arguments_json,
    cc_tool_call_t **out_call
);

/* 清理栈上或数组内 tool call；不释放 call 指针本身。 */
void cc_tool_call_cleanup(cc_tool_call_t *call);

/* 销毁堆上 tool call；允许 NULL。 */
void cc_tool_call_destroy(cc_tool_call_t *call);

/* 深拷贝 tool call；失败时清理 dst。 */
cc_result_t cc_tool_call_copy(const cc_tool_call_t *src, cc_tool_call_t *dst);

/* 初始化 tool call list。 */
void cc_tool_call_list_init(cc_tool_call_list_t *list);

/* 清理 tool call list 内所有元素和数组。 */
void cc_tool_call_list_cleanup(cc_tool_call_list_t *list);

/* 追加 tool call 的深拷贝。 */
cc_result_t cc_tool_call_list_append(
    cc_tool_call_list_t *list,
    const cc_tool_call_t *call
);

/* 用字段值快速追加 tool call；arguments_json 为 NULL 时使用 "{}"。 */
cc_result_t cc_tool_call_list_append_values(
    cc_tool_call_list_t *list,
    const char *id,
    const char *name,
    const char *arguments_json
);

/* 深拷贝 tool call list；失败时清理 dst。 */
cc_result_t cc_tool_call_list_copy(
    const cc_tool_call_list_t *src,
    cc_tool_call_list_t *dst
);

/* 序列化为 provider 兼容的 function tool_calls JSON；返回字符串由调用方 free()。 */
cc_result_t cc_tool_call_list_to_json(
    const cc_tool_call_list_t *list,
    char **out_json
);

/* 从 provider function tool_calls JSON 解析列表；失败时 out_list 可安全 cleanup。 */
cc_result_t cc_tool_call_list_from_json(
    const char *json,
    cc_tool_call_list_t *out_list
);

/* 创建堆上工具结果；成功后调用方用 cc_tool_result_destroy() 释放。 */
cc_result_t cc_tool_result_create(
    int ok,
    const char *text,
    const char *error,
    const char *metadata,
    cc_tool_result_t **out_result
);

/* 清理栈上或嵌入式 tool result；不释放 result 指针。 */
void cc_tool_result_cleanup(cc_tool_result_t *result);

/* 销毁堆上 tool result；允许 NULL。 */
void cc_tool_result_destroy(cc_tool_result_t *result);

/* 向工具结果追加 artifact 深拷贝。 */
cc_result_t cc_tool_result_add_artifact(
    cc_tool_result_t *result,
    const cc_media_artifact_t *artifact
);

/* 替换工具结果 artifact 列表；先拷贝成功再替换旧值。 */
cc_result_t cc_tool_result_set_artifacts(
    cc_tool_result_t *result,
    const cc_media_artifact_list_t *artifacts
);

/* 转移工具结果中的 artifacts 所有权，调用方负责 cleanup 返回列表。 */
cc_media_artifact_list_t cc_tool_result_take_artifacts(cc_tool_result_t *result);

/* 初始化 LLM response；必须在 provider 填充前调用。 */
cc_result_t cc_llm_response_init(cc_llm_response_t *response);

/* 释放 LLM response 内部资源；不释放 response 指针。 */
void cc_llm_response_free(cc_llm_response_t *response);

/* 设置响应文本并标记 has_text；text 会被深拷贝。 */
cc_result_t cc_llm_response_set_text(cc_llm_response_t *response, const char *text);

/* 向响应追加 tool call，供 runtime 后续进入 tool executor。 */
cc_result_t cc_llm_response_add_tool_call(
    cc_llm_response_t *response,
    const char *id,
    const char *name,
    const char *arguments_json
);

/* 向响应追加 artifact，适配支持多模态输出的 provider。 */
cc_result_t cc_llm_response_add_artifact(
    cc_llm_response_t *response,
    const cc_media_artifact_t *artifact
);

#endif
