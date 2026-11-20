#include "cc/core/cc_message.h"

#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/internal/cc_json_fields.h"
#include "cc/internal/cc_json_syntax_check.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* 兼容旧版 SDK 写入的 UNKNOWN 占位前缀，仅检测首个 tool 文本项。 */
static const char *const legacy_unknown_prefix =
    "The tool outcome is unknown and its name is disabled until a user verifies it. ";

/** @brief 映射四种持久恢复状态。 @param[in] outcome 枚举值。 @return 静态名称借用，未知枚举为 NULL。 */
const char *cc_message_tool_outcome_string(cc_message_tool_outcome_t outcome)
{
    switch (outcome) {
    case CC_MESSAGE_TOOL_OUTCOME_RECORDED: return "recorded";
    case CC_MESSAGE_TOOL_OUTCOME_UNKNOWN: return "unknown";
    case CC_MESSAGE_TOOL_OUTCOME_RECONCILED_EXECUTED: return "reconciled_executed";
    case CC_MESSAGE_TOOL_OUTCOME_RECONCILED_NOT_EXECUTED: return "reconciled_not_executed";
    default: return NULL;
    }
}

/**
 * @brief 遍历连续恢复枚举并按字节匹配存储名称。
 * @param[in] text 必填调用期名称借用。
 * @param[out] outcome 必填槽，仅匹配成功时赋值。
 * @return NULL 参数为 INVALID_ARGUMENT，未知名称为 STORAGE，成功为 CC_OK，诊断交调用者。
 */
cc_result_t cc_message_tool_outcome_parse(const char *text, cc_message_tool_outcome_t *outcome)
{
    if (!text || !outcome) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid tool outcome");
    for (int i = CC_MESSAGE_TOOL_OUTCOME_RECORDED; i <= CC_MESSAGE_TOOL_OUTCOME_RECONCILED_NOT_EXECUTED; i++) {
        if (strcmp(text, cc_message_tool_outcome_string((cc_message_tool_outcome_t)i)) == 0) {
            *outcome = (cc_message_tool_outcome_t)i;
            return cc_result_ok();
        }
    }
    return cc_result_error(CC_ERR_STORAGE, "Unknown stored tool outcome");
}

/**
 * @brief 校验恢复枚举和关联角色/调用 ID 组合。
 * @param[in] message 可选稳定消息借用，NULL 为无效。
 * @return RECORDED 为一，UNKNOWN 须为 tool 且有 ID，核对状态须为 user 且有 ID，其他为零。
 */
int cc_message_tool_outcome_valid(const cc_message_t *message)
{
    if (!message || !cc_message_tool_outcome_string(message->tool_outcome)) return 0;
    if (message->tool_outcome == CC_MESSAGE_TOOL_OUTCOME_RECORDED) return 1;
    if (!message->tool_call_id || !message->tool_call_id[0]) return 0;
    if (message->tool_outcome == CC_MESSAGE_TOOL_OUTCOME_UNKNOWN) return message->role == CC_ROLE_TOOL;
    return message->role == CC_ROLE_USER;
}

/**
 * @brief 优先识别显式 UNKNOWN，再检查旧版占位文本。
 * @param[in] message 可选稳定借用，旧版匹配须有可读首项文本。
 * @return tool 角色的显式 UNKNOWN 或 RECORDED 首文本前缀匹配为一，其他为零。
 */
int cc_message_tool_outcome_is_unknown(const cc_message_t *message)
{
    if (!message || message->role != CC_ROLE_TOOL) return 0;
    if (message->tool_outcome == CC_MESSAGE_TOOL_OUTCOME_UNKNOWN) return 1;
    if (message->tool_outcome != CC_MESSAGE_TOOL_OUTCOME_RECORDED || message->content.count == 0 || !message->content.items ||
        message->content.items[0].kind != CC_MEDIA_TEXT || !message->content.items[0].text) return 0;
    return strncmp(message->content.items[0].text, legacy_unknown_prefix, strlen(legacy_unknown_prefix)) == 0;
}

/** @brief 映射四种消息角色到协议名称。 @param[in] role 枚举值。 @return 静态名称借用，未知值为 unknown。 */
const char *cc_message_role_string(cc_message_role_t role)
{
    switch (role) {
    case CC_ROLE_SYSTEM: return "system";
    case CC_ROLE_USER: return "user";
    case CC_ROLE_ASSISTANT: return "assistant";
    case CC_ROLE_TOOL: return "tool";
    default: return "unknown";
    }
}

/** @brief 按字节解析角色并保留未知名称的 USER 缺省。 @param[in] role 可选调用期文本借用。 @return system/assistant/tool 精确匹配，NULL、user 和未知文本均为 USER。 */
cc_message_role_t cc_message_role_from_string(const char *role)
{
    if (!role) return CC_ROLE_USER;
    if (strcmp(role, "system") == 0) return CC_ROLE_SYSTEM;
    if (strcmp(role, "assistant") == 0) return CC_ROLE_ASSISTANT;
    if (strcmp(role, "tool") == 0) return CC_ROLE_TOOL;
    return CC_ROLE_USER;
}

/**
 * @brief 零分配消息并复制基础身份字段，统一失败清理。
 * @param[in] id 可选调用期消息 ID 借用。
 * @param[in] session_id 可选调用期会话 ID 借用。
 * @param[in] role 按值保存角色。
 * @param[in] tool_call_id 可选调用期关联 ID 借用。
 * @param[out] out_message 必填槽，先置 NULL，成功交付初始化的拥有堆对象。
 * @return 参数或 OOM，失败通过 destroy 归还全部已复制字段，诊断交调用者。
 */
static cc_result_t message_alloc_base(
    const char *id,
    const char *session_id,
    cc_message_role_t role,
    const char *tool_call_id,
    cc_message_t **out_message
)
{
    if (!out_message) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message output");
    }
    *out_message = NULL;
    cc_message_t *msg = calloc(1, sizeof(cc_message_t));
    if (!msg) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate message");
    msg->id = cc_copy_string(id);
    msg->session_id = cc_copy_string(session_id);
    msg->role = role;
    msg->tool_call_id = cc_copy_string(tool_call_id);
    cc_content_parts_init(&msg->content);
    cc_tool_call_list_init(&msg->tool_calls);
    if ((id && !msg->id) || (session_id && !msg->session_id) ||
        (tool_call_id && !msg->tool_call_id)) {
        cc_message_destroy(msg);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy message fields");
    }
    *out_message = msg;
    return cc_result_ok();
}

/**
 * @brief 创建基础消息，再复制可选文本为输入内容项。
 * @param[in] id 可选调用期消息 ID 借用。
 * @param[in] session_id 可选调用期会话 ID 借用。
 * @param[in] role 按值保存，不在此验证角色组合。
 * @param[in] text 可选调用期文本借用，NULL 保持零项，空串追加空文本。
 * @param[in] tool_call_id 可选调用期关联 ID 借用。
 * @param[out] out_message 必填空拥有槽，先置 NULL，全部成功后才发布。
 * @return 基础分配或文本追加结果，失败销毁局部消息，诊断交调用者。
 */
cc_result_t cc_message_create_text(
    const char *id,
    const char *session_id,
    cc_message_role_t role,
    const char *text,
    const char *tool_call_id,
    cc_message_t **out_message
)
{
    if (!out_message) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message output");
    *out_message = NULL;
    cc_message_t *msg = NULL;
    cc_result_t rc = message_alloc_base(id, session_id, role, tool_call_id, &msg);
    if (rc.code != CC_OK) return rc;
    if (text) {
        rc = cc_content_parts_append_text(&msg->content, text, CC_CONTENT_PART_INPUT);
        if (rc.code != CC_OK) {
            cc_message_destroy(msg);
            return rc;
        }
    }
    *out_message = msg;
    return cc_result_ok();
}

/**
 * @brief 创建基础消息后深拷贝可选内容列表。
 * @param[in] id 可选调用期消息 ID 借用。
 * @param[in] session_id 可选调用期会话 ID 借用。
 * @param[in] role 按值保存角色。
 * @param[in] parts 可选稳定列表借用，NULL 保持零项内容。
 * @param[in] tool_call_id 可选调用期关联 ID 借用。
 * @param[out] out_message 必填空拥有槽，先清空，全部复制成功后交接。
 * @return 基础字段或媒体复制结果，失败归还局部消息，源列表保持，诊断交调用者。
 */
cc_result_t cc_message_create_parts(
    const char *id,
    const char *session_id,
    cc_message_role_t role,
    const cc_content_parts_t *parts,
    const char *tool_call_id,
    cc_message_t **out_message
)
{
    if (!out_message) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message output");
    *out_message = NULL;
    cc_message_t *msg = NULL;
    cc_result_t rc = message_alloc_base(id, session_id, role, tool_call_id, &msg);
    if (rc.code != CC_OK) return rc;
    if (parts) {
        rc = cc_content_parts_copy(parts, &msg->content);
        if (rc.code != CC_OK) {
            cc_message_destroy(msg);
            return rc;
        }
    }
    *out_message = msg;
    return cc_result_ok();
}

/**
 * @brief 依次归还身份、内容、调用与其余字符串，然后清零。
 * @param[in,out] message 可空初始化拥有记录，调用前排空所有借用。
 * @post 内部资源归还，结构体保留，可再次清理或初始化。
 */
void cc_message_cleanup(cc_message_t *message)
{
    if (!message) return;
    free(message->id);
    free(message->session_id);
    cc_content_parts_cleanup(&message->content);
    cc_tool_call_list_cleanup(&message->tool_calls);
    free(message->reasoning_content);
    free(message->tool_call_id);
    free(message->created_at);
    memset(message, 0, sizeof(*message));
}

/**
 * @brief 复用 cleanup 后释放消息堆对象。
 * @param[in,out] message 可空拥有堆地址，借用者已退出。
 * @post 非空对象与字段释放，所有原借用失效。
 */
void cc_message_destroy(cc_message_t *message)
{
    if (!message) return;
    cc_message_cleanup(message);
    free(message);
}

/**
 * @brief 复制字符串、恢复元数据和两个列表，自复制提前成功返回。
 * @param[in] src 必填稳定源借用，源拥有权保留。
 * @param[out] dst 必填新记录或已 cleanup 槽，与源相同则保持原值。
 * @return 参数失败保持目标，其他失败归还并清零局部目标，成功为独立副本，诊断交调用者。
 */
cc_result_t cc_message_copy(const cc_message_t *src, cc_message_t *dst)
{
    if (!src || !dst) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message copy argument");
    }
    if (src == dst) return cc_result_ok();
    memset(dst, 0, sizeof(*dst));
    dst->id = cc_copy_string(src->id);
    dst->session_id = cc_copy_string(src->session_id);
    dst->role = src->role;
    dst->tool_outcome = src->tool_outcome;
    dst->reasoning_content = cc_copy_string(src->reasoning_content);
    dst->tool_call_id = cc_copy_string(src->tool_call_id);
    dst->created_at = cc_copy_string(src->created_at);
    cc_result_t rc = cc_content_parts_copy(&src->content, &dst->content);
    if (rc.code == CC_OK) rc = cc_tool_call_list_copy(&src->tool_calls, &dst->tool_calls);
    if (rc.code != CC_OK ||
        (src->id && !dst->id) ||
        (src->session_id && !dst->session_id) ||
        (src->reasoning_content && !dst->reasoning_content) ||
        (src->tool_call_id && !dst->tool_call_id) ||
        (src->created_at && !dst->created_at)) {
        cc_message_cleanup(dst);
        if (rc.code != CC_OK) return rc;
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy message");
    }
    return cc_result_ok();
}

/**
 * @brief 按 kind 选择文本或资源深拷贝追加。
 * @param[in,out] message 必填独占消息，内容为有效可变拥有列表。
 * @param[in] part 必填源借用，可为自身列表现有项，原拥有权保留。
 * @return 参数或媒体追加结果，成功新增一项，失败不增加项数，诊断交调用者。
 */
cc_result_t cc_message_add_content_part(cc_message_t *message, const cc_content_part_t *part)
{
    if (!message || !part) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message content part");
    }
    if (part->kind == CC_MEDIA_TEXT) {
        return cc_content_parts_append_text(&message->content, part->text, part->direction);
    }
    return cc_content_parts_append_artifact(&message->content, &part->artifact, part->direction);
}

/**
 * @brief 复用调用列表追加，不在数据层限制消息角色或执行策略。
 * @param[in,out] message 必填独占消息，工具列表满足可变约定。
 * @param[in] call 必填调用期借用，可为同列表已有项。
 * @return 参数、容量或复制结果，输入保留原拥有权，诊断交调用者。
 */
cc_result_t cc_message_add_tool_call(cc_message_t *message, const cc_tool_call_t *call)
{
    if (!message) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message");
    return cc_tool_call_list_append(&message->tool_calls, call);
}

/**
 * @brief 把完整副本准备到局部列表，成功后替换旧调用列表。
 * @param[in,out] message 必填独占消息，外层排除并发读写。
 * @param[in] tool_calls 必填稳定源列表借用，允许为自身列表。
 * @return 参数或复制错误保持旧列表，成功清理旧值并接管副本，诊断交调用者。
 */
cc_result_t cc_message_set_tool_calls(cc_message_t *message, const cc_tool_call_list_t *tool_calls)
{
    if (!message || !tool_calls) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message tool calls");
    }
    cc_tool_call_list_t copy;
    cc_result_t rc = cc_tool_call_list_copy(tool_calls, &copy);
    if (rc.code != CC_OK) return rc;
    cc_tool_call_list_cleanup(&message->tool_calls);
    message->tool_calls = copy;
    return cc_result_ok();
}

/**
 * @brief 先复制借用文本，再释放并替换旧推理字段。
 * @param[in,out] message 必填独占消息。
 * @param[in] reasoning_content 可选调用期借用，可指向旧字段内部，NULL 清空。
 * @return 参数或 OOM 保持旧字段，成功交接副本，诊断交调用者。
 */
cc_result_t cc_message_set_reasoning_content(cc_message_t *message, const char *reasoning_content)
{
    if (!message) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message");
    char *copy = cc_copy_string(reasoning_content);
    if (reasoning_content && !copy) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy reasoning content");
    }
    free(message->reasoning_content);
    message->reasoning_content = copy;
    return cc_result_ok();
}

/**
 * @brief 将稳定内容列表交给媒体摘要入口。
 * @param[in] message 必填调用期消息借用。
 * @param[out] out_summary 必填空文本槽，基础参数失败保持原值，其他沿媒体初始化约定。
 * @return 参数或媒体摘要结果，成功文本交调用者 free，诊断交调用者。
 */
cc_result_t cc_message_get_text_summary(const cc_message_t *message, char **out_summary)
{
    if (!message || !out_summary) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message summary argument");
    }
    return cc_content_parts_text_summary(&message->content, out_summary);
}

/**
 * @brief 按消息形态写入 content，任一节点创建或挂接失败即返回错误。
 * @param[in,out] jm 借用的有效 JSON 对象，调用者串行构造并负责销毁。
 * @param[in] message 调用期间借用的稳定消息，其内容数组已通过外层校验。
 * @return 成功为 CC_OK，否则返回拥有错误消息的结果，由调用者释放。
 * @note 纯工具调用输出 null，空内容输出空字符串，多片段复用媒体序列化。
 */
static cc_result_t add_content_to_json(cc_json_value_t *jm, const cc_message_t *message)
{
    if (message->tool_calls.count > 0 && message->content.count == 0) {
        return cc_json_object_set_owned(jm, "content", cc_json_create_null())
            ? cc_result_ok() : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to set message content");
    }
    if (message->content.count == 0) {
        return cc_json_object_set_owned(jm, "content", cc_json_create_string(""))
            ? cc_result_ok() : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to set message content");
    }
    if (message->content.count == 1 &&
        message->content.items[0].kind == CC_MEDIA_TEXT) {
        return cc_json_object_set_owned(jm, "content",
            cc_json_create_string(message->content.items[0].text ?
                message->content.items[0].text : ""))
            ? cc_result_ok() : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to set message content");
    }
    char *parts_json = NULL;
    cc_result_t rc = cc_content_parts_to_json(&message->content, &parts_json);
    if (rc.code != CC_OK) return rc;
    cc_json_value_t *parts = NULL;
    rc = cc_json_parse(parts_json, &parts);
    free(parts_json);
    if (rc.code != CC_OK) return rc;
    return cc_json_object_set_owned(jm, "content", parts)
        ? cc_result_ok() : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to set message content");
}

/**
 * @brief 完整序列化消息的模型协议字段，构造失败时销毁全部临时节点。
 * @param[in] message 调用期间借用的稳定消息，非空列表须具有有效 items。
 * @param[in] include_reasoning_content 非零时输出非空推理正文。
 * @param[out] out_json 输出独占 NUL 结尾字符串，调用者 free，失败为 NULL。
 * @return 成功为 CC_OK，参数、列表尺寸、JSON 或内存错误的诊断由调用者释放。
 * @note 只输出模型字段，身份、时间和恢复状态跳过，同一消息可并发只读，写入者须外层同步。
 */
cc_result_t cc_message_to_json(
    const cc_message_t *message,
    int include_reasoning_content,
    char **out_json
)
{
    if (!out_json) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message JSON argument");
    }
    *out_json = NULL;
    if (!message || (message->content.count && !message->content.items) ||
        (message->tool_calls.count && !message->tool_calls.items)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid message JSON argument");
    }
    cc_json_value_t *jm = cc_json_create_object();
    if (!jm) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate message JSON");
    if (!cc_json_object_set_owned(jm, "role", cc_json_create_string(cc_message_role_string(message->role)))) {
        cc_json_destroy(jm);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to set message role");
    }
    cc_result_t rc = add_content_to_json(jm, message);
    if (rc.code != CC_OK) {
        cc_json_destroy(jm);
        return rc;
    }
    if (message->tool_calls.count > 0) {
        char *tool_calls_json = NULL;
        rc = cc_tool_call_list_to_json(&message->tool_calls, &tool_calls_json);
        if (rc.code != CC_OK) {
            cc_json_destroy(jm);
            return rc;
        }
        cc_json_value_t *tool_calls = NULL;
        rc = cc_json_parse(tool_calls_json, &tool_calls);
        free(tool_calls_json);
        if (rc.code != CC_OK) {
            cc_json_destroy(jm);
            return rc;
        }
        if (!cc_json_object_set_owned(jm, "tool_calls", tool_calls)) {
            cc_json_destroy(jm);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to set message tool calls");
        }
    }
    if (include_reasoning_content &&
        message->reasoning_content && message->reasoning_content[0]) {
        if (!cc_json_object_set_owned(jm, "reasoning_content",
            cc_json_create_string(message->reasoning_content))) {
            cc_json_destroy(jm);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to set message reasoning");
        }
    }
    if (message->role == CC_ROLE_TOOL && message->tool_call_id) {
        if (!cc_json_object_set_owned(jm, "tool_call_id", cc_json_create_string(message->tool_call_id))) {
            cc_json_destroy(jm);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to set message tool identity");
        }
    }
    *out_json = cc_json_stringify_unformatted(jm);
    cc_json_destroy(jm);
    return *out_json ? cc_result_ok()
                     : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to stringify message");
}

/**
 * @brief 按顺序序列化全部消息，任一消息失败时撤销整份输出。
 * @param[in] messages 调用期间借用的稳定消息数组，count 为零时可为 NULL。
 * @param[in] count 可访问消息数量，每条消息遵守单消息序列化前置条件。
 * @param[in] include_reasoning_content 是否包含非空推理正文，逐条传递。
 * @param[out] out_json 输出独占字符串，调用者 free，失败为 NULL。
 * @return 成功为 CC_OK，参数、数组/列表尺寸、JSON 或内存错误诊断由调用者释放。
 * @note 无共享写入状态，源数组的并发修改须由调用者排除。
 */
cc_result_t cc_messages_to_json(
    const cc_message_t *messages,
    size_t count,
    int include_reasoning_content,
    char **out_json
)
{
    if (!out_json) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null messages JSON output");
    *out_json = NULL;
    if (count && !messages) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null messages array");
    if (count > SIZE_MAX / sizeof(cc_message_t))
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Message array count overflow");
    cc_json_value_t *arr = cc_json_create_array();
    if (!arr) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate messages JSON");
    for (size_t i = 0; i < count; i++) {
        char *msg_json = NULL;
        cc_result_t rc = cc_message_to_json(&messages[i], include_reasoning_content, &msg_json);
        if (rc.code != CC_OK) {
            cc_json_destroy(arr);
            return rc;
        }
        cc_json_value_t *obj = NULL;
        rc = cc_json_parse(msg_json, &obj);
        free(msg_json);
        if (rc.code != CC_OK) {
            cc_json_destroy(arr);
            return rc;
        }
        if (!cc_json_array_append_owned(arr, obj)) {
            cc_json_destroy(arr);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to append message JSON");
        }
    }
    *out_json = cc_json_stringify_unformatted(arr);
    cc_json_destroy(arr);
    return *out_json ? cc_result_ok()
                     : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to stringify messages");
}

/**
 * @brief 完整解析对象并验证五种已知字段，失败统一清理目标。
 * @param[in] json 必填调用期文本借用，拒绝尾部及解码 NUL。
 * @param[out] out_message 必填新记录或已 cleanup 槽，先清零再接管独立字段。
 * @return 参数、JSON/字段或复制错误，失败输出为空，诊断交调用者。
 * @note 字段名忽略大小写，重复已知字段拒绝，未知键跳过。
 * 缺失/未知字符串角色按 USER，缺失/null 内容为旧空文本，缺失/null 工具数组为空。
 * 可选推理和关联 ID 的 null 按缺失处理，其他错类型返回 JSON，字段副本分配失败返回 OOM。
 */
cc_result_t cc_message_from_json(const char *json, cc_message_t *out_message)
{
    if (!out_message) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message JSON parse argument");
    }
    memset(out_message, 0, sizeof(*out_message));
    if (!json) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message JSON text");
    if (cc_storage_json_has_nul_escape(json))
        return cc_result_error(CC_ERR_JSON, "Message JSON contains a decoded NUL");
    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse_strict(json, &root);
    if (rc.code != CC_OK) return rc;
    if (!cc_json_is_object(root)) {
        rc = cc_result_error(CC_ERR_JSON, "Message JSON must be an object");
        goto done;
    }
    static const char *const fields[] = {"role", "content", "tool_calls", "reasoning_content", "tool_call_id"};
    const char *duplicate = cc_json_duplicate_known_field(root, fields, sizeof(fields) / sizeof(fields[0]));
    if (duplicate) {
        rc = cc_result_errf(CC_ERR_JSON, "Message repeats field %s", duplicate);
        goto done;
    }
    cc_json_value_t *role = cc_json_object_get(root, "role");
    cc_json_value_t *content = cc_json_object_get(root, "content");
    cc_json_value_t *tool_calls = cc_json_object_get(root, "tool_calls");
    cc_json_value_t *reasoning = cc_json_object_get(root, "reasoning_content");
    cc_json_value_t *tool_call_id = cc_json_object_get(root, "tool_call_id");
    if (role && !cc_json_is_string(role)) {
        rc = cc_result_error(CC_ERR_JSON, "Message field role must be a string");
        goto done;
    }
    if (content && !cc_json_is_null(content) && !cc_json_is_string(content) && !cc_json_is_array(content)) {
        rc = cc_result_error(CC_ERR_JSON, "Message field content must be a string, array or null");
        goto done;
    }
    if (tool_calls && !cc_json_is_null(tool_calls) && !cc_json_is_array(tool_calls)) {
        rc = cc_result_error(CC_ERR_JSON, "Message field tool_calls must be an array or null");
        goto done;
    }
    if (reasoning && !cc_json_is_null(reasoning) && !cc_json_is_string(reasoning)) {
        rc = cc_result_error(CC_ERR_JSON, "Message field reasoning_content must be a string or null");
        goto done;
    }
    if (tool_call_id && !cc_json_is_null(tool_call_id) && !cc_json_is_string(tool_call_id)) {
        rc = cc_result_error(CC_ERR_JSON, "Message field tool_call_id must be a string or null");
        goto done;
    }
    out_message->role = cc_message_role_from_string(
        cc_json_string_value(role));
    cc_content_parts_init(&out_message->content);
    cc_tool_call_list_init(&out_message->tool_calls);
    if (content && cc_json_is_array(content)) {
        char *parts_json = cc_json_stringify_unformatted(content);
        if (!parts_json) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to stringify content");
            goto done;
        }
        rc = cc_content_parts_from_json(parts_json, &out_message->content);
        free(parts_json);
    } else {
        const char *text = cc_json_string_value(content);
        rc = cc_content_parts_append_text(&out_message->content, text ? text : "", CC_CONTENT_PART_INPUT);
    }
    if (rc.code == CC_OK) {
        if (tool_calls && cc_json_is_array(tool_calls)) {
            char *tc_json = cc_json_stringify_unformatted(tool_calls);
            if (!tc_json) {
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to stringify tool calls");
            } else {
                rc = cc_tool_call_list_from_json(tc_json, &out_message->tool_calls);
                free(tc_json);
            }
        }
    }
    if (rc.code == CC_OK) {
        out_message->reasoning_content =
            cc_copy_string(cc_json_string_value(reasoning));
        out_message->tool_call_id =
            cc_copy_string(cc_json_string_value(tool_call_id));
        if ((cc_json_is_string(reasoning) && !out_message->reasoning_content) ||
            (cc_json_is_string(tool_call_id) && !out_message->tool_call_id)) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy message JSON fields");
        }
    }
done:
    cc_json_destroy(root);
    if (rc.code != CC_OK) cc_message_cleanup(out_message);
    return rc;
}
