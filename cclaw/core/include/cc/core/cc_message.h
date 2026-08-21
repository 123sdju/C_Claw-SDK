

#ifndef CC_MESSAGE_H
#define CC_MESSAGE_H

#include "cc/core/cc_media.h"
#include "cc/core/cc_result.h"
#include "cc/core/cc_tool_call.h"
#include <stddef.h>

/*
 * Agent 对话消息的角色枚举。
 *
 * 这些值对应主流 LLM chat 协议中的 role 字段，也是 session store、
 * context builder 和 provider adapter 之间共享的稳定语义。嵌入式面试里
 * 可以把它解释为“协议层状态的类型安全表达”：外部 JSON 使用字符串，
 * SDK 内部使用 enum，避免业务代码到处比较裸字符串。
 */
typedef enum cc_message_role {
    CC_ROLE_SYSTEM,
    CC_ROLE_USER,
    CC_ROLE_ASSISTANT,
    CC_ROLE_TOOL
} cc_message_role_t;

/*
 * 一条对话消息的核心数据模型。
 *
 * 所有 char * 字段和嵌套列表都由该结构拥有；调用方通过
 * cc_message_cleanup() 释放栈上对象，通过 cc_message_destroy() 释放
 * cc_message_create_*() 创建的堆对象。结构本身不做内部加锁，因此同一条
 * message 不能被多个线程同时写；跨线程传递时应先深拷贝或由上层队列保证
 * 所有权转移。后续扩展字段应保持“清理函数统一释放”的规则。
 */
typedef struct cc_message {
    char *id;
    char *session_id;
    cc_message_role_t role;
    cc_content_parts_t content;
    cc_tool_call_list_t tool_calls;
    char *reasoning_content;
    char *tool_call_id;
    char *created_at;
} cc_message_t;

/*
 * 创建纯文本消息。
 *
 * 函数会深拷贝 id/session_id/text/tool_call_id，并把文本转换为 content
 * parts 中的一个输入文本片段。成功后 *out_message 的所有权交给调用方；
 * 失败时不会泄漏半初始化对象，错误通过 cc_result_t 返回。
 */
cc_result_t cc_message_create_text(
    const char *id,
    const char *session_id,
    cc_message_role_t role,
    const char *text,
    const char *tool_call_id,
    cc_message_t **out_message
);

/*
 * 创建多模态 content parts 消息。
 *
 * parts 会被深拷贝，调用方仍然拥有原始 parts。这个接口用于 assistant/user
 * 同时携带文本、图片、artifact 引用等内容的场景；失败时 *out_message 保持
 * NULL，便于嵌入式代码用单一路径处理 OOM。
 */
cc_result_t cc_message_create_parts(
    const char *id,
    const char *session_id,
    cc_message_role_t role,
    const cc_content_parts_t *parts,
    const char *tool_call_id,
    cc_message_t **out_message
);

/* 释放堆上消息；允许传入 NULL，适合作为错误路径的统一清理函数。 */
void cc_message_destroy(cc_message_t *message);

/*
 * 清理栈上或数组内的消息对象。
 *
 * 只释放字段和嵌套资源，不释放 message 指针本身；清理后结构被清零，调用方
 * 可以安全地重新初始化。不要对同一对象重复 cleanup 后再访问旧字段。
 */
void cc_message_cleanup(cc_message_t *message);

/*
 * 深拷贝消息。
 *
 * dst 会先被清零，再获得 src 的独立副本；如果中途失败，函数会清理 dst，
 * 避免调用方处理半拷贝状态。调用前 dst 不应持有需要保留的资源。
 */
cc_result_t cc_message_copy(const cc_message_t *src, cc_message_t *dst);

/* 向消息追加一个 content part；part 内容会被底层列表深拷贝。 */
cc_result_t cc_message_add_content_part(
    cc_message_t *message,
    const cc_content_part_t *part
);

/* 向 assistant 消息追加一次 tool call；call 会被深拷贝到消息内部列表。 */
cc_result_t cc_message_add_tool_call(
    cc_message_t *message,
    const cc_tool_call_t *call
);

/*
 * 替换消息中的 tool call 列表。
 *
 * 先拷贝新列表，拷贝成功后再释放旧列表，保证失败时原消息状态不被破坏。
 */
cc_result_t cc_message_set_tool_calls(
    cc_message_t *message,
    const cc_tool_call_list_t *tool_calls
);

/* 设置 assistant 推理内容；传入 NULL 表示清空，字符串会被深拷贝。 */
cc_result_t cc_message_set_reasoning_content(
    cc_message_t *message,
    const char *reasoning_content
);

/*
 * 提取消息文本摘要。
 *
 * 多模态消息会只汇总文本片段，返回的 out_summary 由调用方 free()。该接口用于
 * 日志、memory 摘要和不支持多模态 provider 的降级路径。
 */
cc_result_t cc_message_get_text_summary(
    const cc_message_t *message,
    char **out_summary
);

/*
 * 序列化单条消息为 provider 友好的 JSON。
 *
 * include_reasoning_content 控制是否输出 reasoning_content，避免默认把模型思考
 * 内容写入日志或发给不需要该字段的 provider。返回的 JSON 字符串由调用方 free()。
 */
cc_result_t cc_message_to_json(
    const cc_message_t *message,
    int include_reasoning_content,
    char **out_json
);

/* 序列化消息数组；数组元素不会被修改，返回 JSON 字符串由调用方 free()。 */
cc_result_t cc_messages_to_json(
    const cc_message_t *messages,
    size_t count,
    int include_reasoning_content,
    char **out_json
);

/*
 * 从 JSON 解析消息到调用方提供的对象。
 *
 * out_message 会被初始化为可 cleanup 的深拷贝状态；失败时函数会清理已分配字段。
 * 当前解析的是 SDK/provider 交互使用的 message 子集，不承诺保留未知字段。
 */
cc_result_t cc_message_from_json(
    const char *json,
    cc_message_t *out_message
);

/* role enum 到协议字符串的稳定映射；未知值返回 "unknown" 供日志诊断。 */
const char *cc_message_role_string(cc_message_role_t role);

/* 协议字符串到 role enum 的宽松解析；NULL 或未知字符串按 user 处理。 */
cc_message_role_t cc_message_role_from_string(const char *role);

#endif
