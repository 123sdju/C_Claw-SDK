#include "cc/core/cc_message.h"

#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>

/*
 * 将内部 role 枚举转换成 provider 协议需要的字符串。
 *
 * 这里集中维护映射关系，避免 runtime、存储层和各个 provider adapter
 * 重复写 switch。未知值保留为 "unknown"，便于日志暴露坏状态而不是崩溃。
 */
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

/*
 * 将外部 JSON/protocol 字符串解析为内部 role。
 *
 * 默认回退到 user 是为了兼容缺省或未知输入；安全边界不依赖这个函数，
 * 因此它选择宽松解析，真正的协议校验应在更高层完成。
 */
cc_message_role_t cc_message_role_from_string(const char *role)
{
    if (!role) return CC_ROLE_USER;
    if (strcmp(role, "system") == 0) return CC_ROLE_SYSTEM;
    if (strcmp(role, "assistant") == 0) return CC_ROLE_ASSISTANT;
    if (strcmp(role, "tool") == 0) return CC_ROLE_TOOL;
    return CC_ROLE_USER;
}

/*
 * 创建 message 的公共初始化骨架。
 *
 * create_text/create_parts 都要分配同一批基础字段，所以抽成这个私有 helper。
 * 它负责把 out_message 先置空、深拷贝字符串字段并初始化嵌套列表；任何字段
 * 拷贝失败都会走 cc_message_destroy()，保证调用方不会看到半初始化对象。
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

/*
 * 创建只有文本 content 的消息。
 *
 * 该函数把“业务上看起来是一段文本”的输入统一转成 content parts 模型，
 * 让后续 runtime/provider 无需区分文本消息和多模态消息。失败路径释放已经
 * 分配的 message，符合嵌入式 C 中常见的“单出口所有权清晰”原则。
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

/*
 * 创建直接携带 content parts 的消息。
 *
 * parts 通过 cc_content_parts_copy() 深拷贝，这样调用方可以在函数返回后立即
 * 释放自己的临时 parts。SDK 内部所有 message 都拥有独立内容，避免异步 run
 * 或 session 落库时出现悬垂指针。
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

/*
 * 清理 message 内部资源但不释放结构体本身。
 *
 * 这个函数服务于栈对象、数组元素和深拷贝失败回滚。最后 memset 为零可以让
 * 后续 cleanup/destroy 在 NULL 字段上安全返回，也能减少错误路径中的二次释放风险。
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

/*
 * 销毁堆分配 message。
 *
 * create_* 返回的是堆对象，所以这里先复用 cleanup 释放字段，再释放结构体。
 * 允许 NULL 让调用方在复杂失败路径里直接调用，不需要额外分支。
 */
void cc_message_destroy(cc_message_t *message)
{
    if (!message) return;
    cc_message_cleanup(message);
    free(message);
}

/*
 * 深拷贝完整 message。
 *
 * runtime、session manager 和测试经常需要在不同生命周期之间传递消息；深拷贝
 * 可以把所有权边界讲清楚。函数先清零 dst，再逐字段复制；任何一步失败都会清理
 * dst，调用方只需要检查返回码，不需要理解半成品结构。
 */
cc_result_t cc_message_copy(const cc_message_t *src, cc_message_t *dst)
{
    if (!src || !dst) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message copy argument");
    }
    memset(dst, 0, sizeof(*dst));
    dst->id = cc_copy_string(src->id);
    dst->session_id = cc_copy_string(src->session_id);
    dst->role = src->role;
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

/*
 * 追加一个 content part。
 *
 * 文本和 artifact 使用不同的底层追加函数，因为文本需要复制字符串，artifact
 * 需要复制二进制/引用元数据。message 本身不加锁，调用方要保证同一对象写入串行化。
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

/*
 * 追加一次工具调用。
 *
 * assistant 消息可能包含多个 tool call，这里只负责数据层追加，不做 provider
 * 协议或 approval 判断；那些策略由 runtime/tool executor 处理。
 */
cc_result_t cc_message_add_tool_call(cc_message_t *message, const cc_tool_call_t *call)
{
    if (!message) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message");
    return cc_tool_call_list_append(&message->tool_calls, call);
}

/*
 * 原子语义地替换 tool call 列表。
 *
 * 先复制新列表，成功后再清理旧列表；这样 OOM 或 JSON 解析失败时不会破坏
 * message 里原本可用的 tool call 数据。
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

/*
 * 设置 assistant reasoning content。
 *
 * reasoning 可能来自支持思维输出的 provider，也可能需要在序列化时隐藏。这里
 * 只管理内存所有权：新字符串复制成功后再替换旧值，传 NULL 等价于清空。
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

/*
 * 生成文本摘要。
 *
 * 多模态 content 在日志、memory 或不支持多模态的 provider 中常需要退化成文本；
 * 具体拼接规则由 media 模块维护，message 模块只暴露统一入口。
 */
cc_result_t cc_message_get_text_summary(const cc_message_t *message, char **out_summary)
{
    if (!message || !out_summary) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message summary argument");
    }
    return cc_content_parts_text_summary(&message->content, out_summary);
}

/*
 * 把 message.content 写入 JSON 对象。
 *
 * 这里保留 chat 协议的几个细节：纯 tool_calls 且无文本时 content 为 null；
 * 无内容时输出空字符串；单一文本时输出字符串；多片段时输出数组。这个 helper
 * 把 provider adapter 需要兼容的协议差异集中在一处。
 */
static cc_result_t add_content_to_json(cc_json_value_t *jm, const cc_message_t *message)
{
    if (message->tool_calls.count > 0 && message->content.count == 0) {
        cc_json_object_set(jm, "content", cc_json_create_null());
        return cc_result_ok();
    }
    if (message->content.count == 0) {
        cc_json_object_set(jm, "content", cc_json_create_string(""));
        return cc_result_ok();
    }
    if (message->content.count == 1 &&
        message->content.items[0].kind == CC_MEDIA_TEXT) {
        cc_json_object_set(jm, "content",
            cc_json_create_string(message->content.items[0].text ?
                message->content.items[0].text : ""));
        return cc_result_ok();
    }
    char *parts_json = NULL;
    cc_result_t rc = cc_content_parts_to_json(&message->content, &parts_json);
    if (rc.code != CC_OK) return rc;
    cc_json_value_t *parts = NULL;
    rc = cc_json_parse(parts_json, &parts);
    free(parts_json);
    if (rc.code != CC_OK) return rc;
    cc_json_object_set(jm, "content", parts);
    return cc_result_ok();
}

/*
 * 序列化单条消息。
 *
 * 函数先构造 JSON AST，再 stringify，避免手工拼接造成转义错误。reasoning_content
 * 是否输出由调用方决定；tool 角色会额外输出 tool_call_id，保证 provider 能把
 * tool result 关联回原始调用。
 */
cc_result_t cc_message_to_json(
    const cc_message_t *message,
    int include_reasoning_content,
    char **out_json
)
{
    if (!message || !out_json) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message JSON argument");
    }
    *out_json = NULL;
    cc_json_value_t *jm = cc_json_create_object();
    if (!jm) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate message JSON");
    cc_json_object_set(jm, "role", cc_json_create_string(cc_message_role_string(message->role)));
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
        cc_json_object_set(jm, "tool_calls", tool_calls);
    }
    if (include_reasoning_content &&
        message->reasoning_content && message->reasoning_content[0]) {
        cc_json_object_set(jm, "reasoning_content",
            cc_json_create_string(message->reasoning_content));
    }
    if (message->role == CC_ROLE_TOOL && message->tool_call_id) {
        cc_json_object_set(jm, "tool_call_id",
            cc_json_create_string(message->tool_call_id));
    }
    *out_json = cc_json_stringify_unformatted(jm);
    cc_json_destroy(jm);
    return *out_json ? cc_result_ok()
                     : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to stringify message");
}

/*
 * 序列化消息数组。
 *
 * 每个元素复用 cc_message_to_json()，再解析回 JSON AST 放入数组。这样虽然多一次
 * stringify/parse，但复用单条消息的协议规则，减少多处实现不一致的发布风险。
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
        cc_json_array_append(arr, obj);
    }
    *out_json = cc_json_stringify_unformatted(arr);
    cc_json_destroy(arr);
    return *out_json ? cc_result_ok()
                     : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to stringify messages");
}

/*
 * 从 JSON 解析消息。
 *
 * 解析流程先初始化 out_message 的嵌套列表，再按 content 的 JSON 类型决定文本
 * 或多片段解析方式。任何后续步骤失败都会 cleanup，保证调用方不会拿到需要猜测
 * 状态的对象；未知字段当前不保留，SDK 只承诺核心协议字段。
 */
cc_result_t cc_message_from_json(const char *json, cc_message_t *out_message)
{
    if (!json || !out_message) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message JSON parse argument");
    }
    memset(out_message, 0, sizeof(*out_message));
    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse(json, &root);
    if (rc.code != CC_OK) return rc;
    out_message->role = cc_message_role_from_string(
        cc_json_string_value(cc_json_object_get(root, "role")));
    cc_content_parts_init(&out_message->content);
    cc_tool_call_list_init(&out_message->tool_calls);
    cc_json_value_t *content = cc_json_object_get(root, "content");
    if (content && cc_json_is_array(content)) {
        char *parts_json = cc_json_stringify_unformatted(content);
        if (!parts_json) {
            cc_json_destroy(root);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to stringify content");
        }
        rc = cc_content_parts_from_json(parts_json, &out_message->content);
        free(parts_json);
    } else {
        const char *text = cc_json_string_value(content);
        rc = cc_content_parts_append_text(&out_message->content, text ? text : "", CC_CONTENT_PART_INPUT);
    }
    if (rc.code == CC_OK) {
        cc_json_value_t *tool_calls = cc_json_object_get(root, "tool_calls");
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
            cc_copy_string(cc_json_string_value(cc_json_object_get(root, "reasoning_content")));
        out_message->tool_call_id =
            cc_copy_string(cc_json_string_value(cc_json_object_get(root, "tool_call_id")));
        if ((cc_json_object_get(root, "reasoning_content") && !out_message->reasoning_content) ||
            (cc_json_object_get(root, "tool_call_id") && !out_message->tool_call_id)) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy message JSON fields");
        }
    }
    cc_json_destroy(root);
    if (rc.code != CC_OK) cc_message_cleanup(out_message);
    return rc;
}
