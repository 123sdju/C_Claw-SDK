

#ifndef CC_MESSAGE_H
#define CC_MESSAGE_H

#include "cc/core/cc_media.h"
#include "cc/core/cc_result.h"
#include "cc/core/cc_tool_call.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief SDK 历史与模型协议共用的消息角色。 */
typedef enum cc_message_role {
    CC_ROLE_SYSTEM, /**< 系统指令。 */
    CC_ROLE_USER, /**< 用户输入或恢复核对说明。 */
    CC_ROLE_ASSISTANT, /**< 模型文本及工具调用。 */
    CC_ROLE_TOOL /**< 与调用 ID 关联的工具结果。 */
} cc_message_role_t;

/** @brief 本地持久恢复状态，消息协议序列化不输出此字段。 */
typedef enum cc_message_tool_outcome {
    CC_MESSAGE_TOOL_OUTCOME_RECORDED = 0, /**< 普通已记录消息的零值状态。 */
    CC_MESSAGE_TOOL_OUTCOME_UNKNOWN, /**< tool 角色的待核对结果。 */
    CC_MESSAGE_TOOL_OUTCOME_RECONCILED_EXECUTED, /**< user 角色确认原调用已经执行。 */
    CC_MESSAGE_TOOL_OUTCOME_RECONCILED_NOT_EXECUTED /**< user 角色确认原调用未执行。 */
} cc_message_tool_outcome_t;

/**
 * @brief 拥有字符串和嵌套列表的单条消息，没有内部锁。
 * @note 栈对象或数组元素用 cleanup，create 返回的堆对象用 destroy。
 * 多线程只读可共享稳定借用，修改、清理和销毁由外层排除并发访问。
 */
typedef struct cc_message {
    char *id; /**< 可选拥有消息身份，模型协议序列化跳过。 */
    char *session_id; /**< 可选拥有会话身份，模型协议序列化跳过。 */
    cc_message_role_t role; /**< 角色值，创建和复制按值保留。 */
    cc_content_parts_t content; /**< 拥有内容列表及各项。 */
    cc_tool_call_list_t tool_calls; /**< 拥有工具调用列表及字符串。 */
    char *reasoning_content; /**< 可选拥有推理文本，序列化可选择隐藏。 */
    char *tool_call_id; /**< 可选拥有源调用 ID，也可关联 user 角色的资源观察。 */
    char *created_at; /**< 可选拥有时间文本，本模块不解析其格式。 */
    cc_message_tool_outcome_t tool_outcome; /**< 本地恢复状态，不推断工具业务成功与否。 */
} cc_message_t;

/**
 * @brief 判断 tool 消息是否为显式 UNKNOWN 或旧版固定前缀占位。
 * @param[in] message 可选稳定借用，NULL、非 tool 或缺少可读旧文本返回零。
 * @return 显式 UNKNOWN 或 RECORDED 首文本匹配旧前缀为一，其他为零。
 */
int cc_message_tool_outcome_is_unknown(const cc_message_t *message);
/** @brief 取得恢复状态名称。 @param[in] outcome 状态值。 @return 静态借用名称，未知枚举为 NULL。 */
const char *cc_message_tool_outcome_string(cc_message_tool_outcome_t outcome);
/**
 * @brief 按精确文本解析四种本地恢复状态。
 * @param[in] text 必填调用期文本借用。
 * @param[out] outcome 必填值槽，仅成功时写入，失败保留原值。
 * @return 成功为 CC_OK，NULL 为 INVALID_ARGUMENT，未知名称为 STORAGE，诊断由调用者释放。
 */
cc_result_t cc_message_tool_outcome_parse(const char *text, cc_message_tool_outcome_t *outcome);
/**
 * @brief 检查恢复状态与角色及关联调用 ID 的组合。
 * @param[in] message 可选稳定借用，NULL 返回零。
 * @return RECORDED 接纳任意角色，UNKNOWN 须为 tool 且有非空调用 ID，两种核对状态须为 user 且有 ID，其他为零。
 */
int cc_message_tool_outcome_valid(const cc_message_t *message);

/**
 * @brief 创建拥有身份副本和可选输入文本项的堆消息。
 * @param[in] id 可选调用期消息 ID 借用，非 NULL 时复制。
 * @param[in] session_id 可选调用期会话 ID 借用，非 NULL 时复制。
 * @param[in] role 角色值，按值保存，本入口不验证业务角色组合。
 * @param[in] text 可选调用期文本，NULL 生成零项内容，空串生成一项空文本。
 * @param[in] tool_call_id 可选调用期关联 ID，非 NULL 时复制。
 * @param[out] out_message 必填空拥有槽，存在时先置 NULL，成功交付须 destroy 的消息。
 * @return CC_OK、参数或分配错误，失败归还本次消息，诊断由调用者释放。
 */
cc_result_t cc_message_create_text(
    const char *id,
    const char *session_id,
    cc_message_role_t role,
    const char *text,
    const char *tool_call_id,
    cc_message_t **out_message
);

/**
 * @brief 深拷贝可选内容列表并创建拥有的堆消息。
 * @param[in] id 可选调用期消息 ID 借用。
 * @param[in] session_id 可选调用期会话 ID 借用。
 * @param[in] role 按值保存角色。
 * @param[in] parts 可选稳定列表借用，NULL 采用零项内容，非空项须可读。
 * @param[in] tool_call_id 可选调用期关联 ID 借用。
 * @param[out] out_message 必填空拥有槽，存在时先置 NULL，成功交付独立消息。
 * @return 基础字段或内容复制结果，失败保持空输出且归还全部部分字段，诊断由调用者释放。
 */
cc_result_t cc_message_create_parts(
    const char *id,
    const char *session_id,
    cc_message_role_t role,
    const cc_content_parts_t *parts,
    const char *tool_call_id,
    cc_message_t **out_message
);

/** @brief 消费堆上消息及全部拥有字段。 @param[in,out] message 可空拥有地址，调用前借用者已退出。 @post 非空消息被释放，原地址与字段借用失效。 */
void cc_message_destroy(cc_message_t *message);

/**
 * @brief 消费拥有字段并清零消息记录。
 * @param[in,out] message 可空初始化对象，嵌套列表须遵守拥有容器约定，调用前排空借用。
 * @post 字符串、内容和工具列表已归还，结构体仍归调用者，可再次初始化或清理。
 */
void cc_message_cleanup(cc_message_t *message);

/**
 * @brief 深拷贝消息全部字段，自复制保持原对象。
 * @param[in] src 必填稳定源借用，列表按只读项数访问。
 * @param[out] dst 必填新对象或已 cleanup 槽，与 src 相同则成功跳过，其他情况先清零。
 * @return 参数错误保持可访问目标原值，复制失败归还部分目标并清零，成功取得独立拥有副本，诊断由调用者释放。
 */
cc_result_t cc_message_copy(const cc_message_t *src, cc_message_t *dst);

/**
 * @brief 深拷贝追加一个文本或资源内容项。
 * @param[in,out] message 必填独占消息，其内容为有效可变拥有列表。
 * @param[in] part 必填调用期源借用，可来自同一列表的现有项。
 * @return 参数或媒体追加结果，源拥有权保留，失败不增加项数，诊断由调用者释放。
 */
cc_result_t cc_message_add_content_part(
    cc_message_t *message,
    const cc_content_part_t *part
);

/**
 * @brief 向消息工具列表追加独立调用副本。
 * @param[in,out] message 必填独占消息，列表遵守可变容器约定，本入口不限制角色。
 * @param[in] call 必填调用期借用，可指向同列表现有项，内层字符串保留原拥有权。
 * @return 参数、容量或复制结果，成功项数加一，失败不增加项数，诊断由调用者释放。
 */
cc_result_t cc_message_add_tool_call(
    cc_message_t *message,
    const cc_tool_call_t *call
);

/**
 * @brief 先深拷贝完整列表，再替换消息中的旧工具调用。
 * @param[in,out] message 必填独占拥有消息。
 * @param[in] tool_calls 必填稳定源借用，允许为 message 自身工具列表。
 * @return 参数或复制错误保持旧列表，成功归还旧列表并接管副本，诊断由调用者释放。
 */
cc_result_t cc_message_set_tool_calls(
    cc_message_t *message,
    const cc_tool_call_list_t *tool_calls
);

/**
 * @brief 先复制再替换推理文本。
 * @param[in,out] message 必填独占消息，本入口不限制角色。
 * @param[in] reasoning_content 可选调用期文本借用，可指向旧字段或其内部，NULL 表示清空。
 * @return 参数或 OOM 保持旧字段，成功接管新副本，诊断由调用者释放。
 */
cc_result_t cc_message_set_reasoning_content(
    cc_message_t *message,
    const char *reasoning_content
);

/**
 * @brief 复用媒体模块生成内容的文本摘要。
 * @param[in] message 必填稳定消息借用。
 * @param[out] out_summary 必填空字符串槽，基础参数失败保持原值，后续沿媒体输出初始化约定。
 * @return 参数或媒体摘要结果，成功字符串由调用者 free，诊断由调用者释放。
 */
cc_result_t cc_message_get_text_summary(
    const cc_message_t *message,
    char **out_summary
);

/**
 * @brief 完整序列化模型协议字段，任何字段构造失败时撤销输出。
 * @param[in] message 借用的稳定消息，非空内容和工具列表须有有效 items。
 * @param[in] include_reasoning_content 非零时包含非空推理正文。
 * @param[out] out_json 输出独占 NUL 结尾字符串，调用者 free，失败为 NULL。
 * @return 成功为 CC_OK，参数、列表尺寸、JSON 或分配错误的诊断由调用者释放。
 * @note 源消息在调用期间保持稳定，可并发只读，修改须外层同步。
 */
cc_result_t cc_message_to_json(
    const cc_message_t *message,
    int include_reasoning_content,
    char **out_json
);

/**
 * @brief 按顺序完整序列化消息数组，任一条失败时撤销整份输出。
 * @param[in] messages 借用的稳定数组，count 为零时可为 NULL。
 * @param[in] count 可访问的消息数量，每条消息遵守单消息序列化条件。
 * @param[in] include_reasoning_content 是否包含非空推理正文。
 * @param[out] out_json 输出独占字符串，调用者 free，失败为 NULL。
 * @return 成功为 CC_OK，参数、数组/列表尺寸、JSON 或分配错误的诊断由调用者释放。
 * @note 不修改源数组，并发写入须由调用者排除。
 */
cc_result_t cc_messages_to_json(
    const cc_message_t *messages,
    size_t count,
    int include_reasoning_content,
    char **out_json
);

/**
 * @brief 完整解析模型消息子集，任一已知字段错误时撤销整条输出。
 * @param[in] json 必填调用期文本借用，顶层须为对象，拒绝尾部内容及解码 NUL。
 * @param[out] out_message 必填新对象或已 cleanup 槽，存在时先清零，成功接管独立字段。
 * @return CC_OK、参数、JSON/字段或分配错误，失败清理全部输出，诊断由调用者释放。
 * @note 字段名沿既有忽略大小写匹配，重复已知字段拒绝，未知字段跳过。
 * role 缺失或未知字符串按 USER，content 缺失/null 生成空文本，tool_calls 缺失/null 为空列表。
 * content 接纳字符串/数组/null，tool_calls 接纳数组/null，已提供 role 须为字符串。
 * reasoning_content/tool_call_id 缺失/null 为 NULL，其他值须为字符串，空串也会独立复制。
 * 仅解析 role/content/tool_calls/reasoning_content/tool_call_id，id/session_id/created_at/恢复状态留给存储层。
 */
cc_result_t cc_message_from_json(
    const char *json,
    cc_message_t *out_message
);

/** @brief 取得角色协议名。 @param[in] role 枚举值。 @return 静态 system/user/assistant/tool 名称，未知为 unknown。 */
const char *cc_message_role_string(cc_message_role_t role);

/** @brief 宽松映射角色名称。 @param[in] role 可选调用期字符串借用，按字节匹配。 @return system/assistant/tool 精确映射，NULL、user 或未知名称均为 USER。 */
cc_message_role_t cc_message_role_from_string(const char *role);

#ifdef __cplusplus
}
#endif

#endif
