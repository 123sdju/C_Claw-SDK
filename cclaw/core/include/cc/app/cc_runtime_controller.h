#ifndef CC_RUNTIME_CONTROLLER_H
#define CC_RUNTIME_CONTROLLER_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_session.h"
#include "cc/core/cc_stream_chunk.h"
#include "cc/ports/cc_tool.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 借用 builder、串行接纳运行并协调当前会话的控制器。
 *
 * 状态由内部互斥量保护，一次接纳一个文本、恢复或手动工具运行。
 * owner 在销毁前先关闭外层入口，并保证每个已进入 API 的调用持有有效句柄。
 * shutdown 负责等待已接纳运行及会话变更，历史读取、状态查询等其他调用由 owner 排空。
 * 销毁成功后所有句柄和借用均失效。
 */
typedef struct cc_runtime_controller cc_runtime_controller_t;
typedef struct cc_runtime_builder cc_runtime_builder_t;
typedef uint64_t cc_runtime_controller_run_id_t; /**< 本次接纳运行的数值 ID，零表示空闲，与检查点字符串 ID 分开管理。 */
/**
 * @brief 比较控制器借用的 builder 身份。
 * @param[in] controller 待核对对象，可为 NULL，调用期间须存活。
 * @param[in] builder 预期 builder，可为 NULL，调用期间须存活。
 * @return 两个指针均非空且装配身份相同时返回非零，否则为零。该查询直接读取固定借用关系。
 */
int cc_runtime_controller_uses_builder(const cc_runtime_controller_t *controller,
                                       const cc_runtime_builder_t *builder);

typedef enum cc_runtime_controller_lifecycle {
    CC_RUNTIME_CONTROLLER_STARTING = 0, /**< 正在构造，尚未交出对象。 */
    CC_RUNTIME_CONTROLLER_RUNNING, /**< 接受空闲时的新运行及会话变更。 */
    CC_RUNTIME_CONTROLLER_DRAINING, /**< 停止接纳，已发起取消，等待已接纳工作。 */
    CC_RUNTIME_CONTROLLER_STOPPED, /**< 已排空已接纳工作，可销毁。 */
    CC_RUNTIME_CONTROLLER_FAILED, /**< 创建失败清理期间的内部状态。 */
} cc_runtime_controller_lifecycle_t;

typedef struct cc_runtime_controller_config {
    size_t size; /**< 结构体大小，创建时据此判断 fresh_session_on_start 尾字段是否可读。 */
    const char *session_base; /**< 会话 ID 前缀，创建时 NULL 或空前缀生成 default，配置时 NULL 保留原值，长前缀缩短以保留完整后缀。 */
    const char *workspace_dir; /**< 复制的工作区路径，创建时 NULL 复制为空串，配置时 NULL 保留原值。 */
    int drop_late_events; /**< 非零时丢弃运行 ID 或运行时代际已不匹配的流事件，取消后的事件始终受取消检查。 */
    int fresh_session_on_start; /**< 创建时选择 UUID 会话并延迟持久化，apply_config 保留创建时选定的模式。 */
} cc_runtime_controller_config_t;

typedef struct cc_runtime_controller_status {
    size_t size; /**< 输出结构体大小。 */
    cc_runtime_controller_lifecycle_t lifecycle; /**< 锁内取得的生命周期状态。 */
    cc_runtime_controller_run_id_t active_run_id; /**< 已接纳运行 ID，零表示空闲。 */
    unsigned long runtime_generation; /**< 最近一次成功取得运行时代际的 ID，空闲时也保留。 */
    unsigned long session_generation; /**< 本对象内的会话选择版本，新建、恢复或身份配置变化时递增。 */
    char session_id[96]; /**< 当前会话 ID 的独立 NUL 结尾副本。 */
    int session_is_persisted; /**< 控制器记录的会话已创建标志，反映 session_needs_create 的反值。 */
} cc_runtime_controller_status_t;

typedef struct cc_runtime_controller_stream_event {
    size_t size; /**< 事件结构体大小。 */
    const cc_stream_chunk_t *chunk; /**< 本次回调期间借用的原始流片段。 */
    const char *agent_id; /**< 当前控制器事件保持为 NULL。 */
    const char *session_id; /**< 本次运行复制的会话 ID，回调期间借用。 */
    cc_runtime_controller_run_id_t run_id; /**< 控制器本次接纳运行的数值 ID。 */
    unsigned long runtime_generation; /**< 本次运行持有的运行时代际 ID。 */
} cc_runtime_controller_stream_event_t;

/**
 * @brief 在运行调用路径中同步接收流事件，调用时未持有控制器互斥量。
 * @param[in] event 回调期间借用的事件及片段，需保留时由接收方复制。
 * @param[in,out] user_data 提交者提供的借用上下文，可为 NULL。
 * @post 接收方已处理事件。回调可查询状态或发起取消，等待本次运行退出的 shutdown/destroy 须由回调外协调。
 */
typedef void (*cc_runtime_controller_stream_fn)(
    const cc_runtime_controller_stream_event_t *event,
    void *user_data
);

/**
 * @brief 创建控制器并取得初始运行时代际，按会话模式建立或延迟建立会话。
 * @param[in] builder 非空借用 builder，寿命须覆盖控制器及全部调用。
 * @param[in] config 可选配置，字符串复制。NULL 使用 default 前缀、空工作区、丢弃迟到事件及普通会话模式。
 * 普通模式立即创建前缀加版本号的会话，UUID 模式保留未持久化的新会话 ID。
 * @param[out] out_controller 必填拥有对象输出，builder 与输出均有效时先置 NULL。
 * @return CC_OK 表示创建成功，对象使用 destroy 释放，其他返回参数、分配、同步原语、ID、代际取得或会话创建错误。
 * 创建错误清理本对象资源，已发生的底层会话写入按存储实际结果核对。
 */
cc_result_t cc_runtime_controller_create(
    cc_runtime_builder_t *builder,
    const cc_runtime_controller_config_t *config,
    cc_runtime_controller_t **out_controller
);

/**
 * @brief 同步向当前会话提交文本，并按运行时能力选择流式或普通执行。
 * @param[in,out] controller 非空控制器，须处于 RUNNING 且没有已接纳运行或会话变更。
 * @param[in] user_input 非空指针的用户输入，调用期间借用，正文限制由 runtime 核对。
 * @param[in] on_stream 可选同步流事件回调，普通执行路径沿用 runtime 的非流式行为。
 * @param[in,out] user_data 回调上下文，借用至本次提交返回，可为 NULL。
 * @param[out] out_response 必填独立输出，基础参数通过后置 NULL，非空结果归调用者 free。
 * 错误时也可能返回拥有的诊断文字，输出不得覆盖尚未释放的旧结果。
 * @return 返回运行结果，繁忙返回 CC_ERR_QUEUE_FULL，停止接纳返回 CC_ERR_INVALID_STATE。
 * 失败时原输入、最终答复或工具操作可能已持久化，须结合检查点与账本判断。
 */
cc_result_t cc_runtime_controller_submit_text(
    cc_runtime_controller_t *controller,
    const char *user_input,
    cc_runtime_controller_stream_fn on_stream,
    void *user_data,
    char **out_response
);

/**
 * @brief 当前会话仍与预备会话一致时，同步启动指定检查点 ID 的新运行。
 * @param[in,out] controller 非空、空闲且运行中的控制器。
 * @param[in] expected_session_id 非空预备会话 ID，在接纳运行的锁内与当前选择比较，调用期间借用。
 * @param[in] run_id 调用者生成的非空 run-UUID 字符串，runtime 核对并用于检查点，调用期间借用。
 * @param[in] user_input 非空输入指针，调用期间借用。
 * @param[in] on_stream 可选同步流事件回调。
 * @param[in,out] user_data 借用回调上下文，可为 NULL。
 * @param[out] out_response 必填独立输出，运行准备阶段置 NULL，非空字符串即使出错也由调用者 free。
 * @return 返回接纳、会话创建或检查点运行结果，选择变化返回 CC_ERR_INVALID_STATE，失败可能已有持久化进展。
 */
cc_result_t cc_runtime_controller_submit_text_with_run_id(
    cc_runtime_controller_t *controller, const char *expected_session_id,
    const char *run_id, const char *user_input, cc_runtime_controller_stream_fn on_stream,
    void *user_data, char **out_response);

/**
 * @brief 同步恢复属于当前所选历史会话的中断运行。
 * @param[in,out] controller 非空且空闲的运行中控制器，当前会话须已建立。
 * @param[in] run_id 非空检查点运行 ID，调用期间借用，载入后核对所属会话。
 * @param[in] on_stream 可选同步流事件回调。
 * @param[in,out] user_data 借用回调上下文，可为 NULL。
 * @param[out] out_response 必填独立输出，基础参数通过后置 NULL，非空结果即使出错也由调用者 free。
 * @return 返回接纳、检查点读取或恢复结果。新会话尚未创建时返回状态错误，检查点属于其他会话时返回参数错误。
 * 错误可能伴随历史、检查点或工具账本的部分推进。
 */
cc_result_t cc_runtime_controller_resume_run(
    cc_runtime_controller_t *controller, const char *run_id,
    cc_runtime_controller_stream_fn on_stream, void *user_data, char **out_response);
/**
 * @brief 将用户手动工具调用作为当前会话的一次串行运行同步执行。
 * @param[in,out] controller 非空、空闲且运行中的控制器，创建取消源并持有运行时代际。
 * @param[in] call 借用工具调用及字符串，须满足 runtime 的名称、参数及唯一调用 ID 契约。
 * @param[in] trusted_tool 可选借用实现，NULL 查注册表，非空时须附有操作账本并保持实现存活至返回。
 * @param[out] out_result 必填独立输出，基础参数通过后清零，拥有字段由调用者 cc_tool_result_cleanup，出错时也须清理。
 * @return 返回接纳或工具执行路径结果。CC_OK 时仍须检查 out_result->ok，错误时可能已产生副作用或账本写入。
 * 取消通过当前控制器运行 ID 发起，实际响应时机由工具执行路径决定。
 */
cc_result_t cc_runtime_controller_execute_manual_tool(cc_runtime_controller_t *controller,
    const cc_tool_call_t *call, const cc_tool_t *trusted_tool, cc_tool_result_t *out_result);

/**
 * @brief 在锁内向指定的当前运行发出取消信号。
 * @param[in,out] controller 非空控制器。
 * @param[in] run_id 非零控制器数值运行 ID，须与当前已接纳运行一致。
 * @return 匹配并发出信号返回 CC_OK，ID 已结束或不匹配返回 CC_ERR_NOT_FOUND，参数无效返回参数错误。
 * 已发出信号后由 runtime 或工具协作退出，已确认成功的结果保持成功。
 */
cc_result_t cc_runtime_controller_cancel(
    cc_runtime_controller_t *controller,
    cc_runtime_controller_run_id_t run_id
);

/**
 * @brief 停止接纳新工作，取消当前运行并等待已接纳运行及会话变更。
 * @pre owner 已关闭外层入口，调用期间保持控制器有效，并在回调之外执行等待。
 * @param[in,out] controller 非空控制器，进入 DRAINING，排空成功后进入 STOPPED。
 * @param[in] timeout_ms 非负等待预算，单位毫秒，零表示只检查当前排空状态。
 * @return 已停止或排空成功返回 CC_OK，超时返回 CC_ERR_TIMEOUT，保留 DRAINING 对象供重试。
 * 此入口保留对象资源，其他查询与历史读取调用的排空由外层 owner 协调。
 */
cc_result_t cc_runtime_controller_shutdown(
    cc_runtime_controller_t *controller,
    int timeout_ms
);

/**
 * @brief 先执行 shutdown，成功后释放控制器及其拥有的字符串、条件变量和互斥量。
 * @pre owner 已关闭外层入口并排空其他 API 访问及内部会话 ID 借用。
 * 已接纳运行与会话变更由 shutdown 等待，builder 在整个清理期间存活。
 * @param[in,out] controller 待销毁对象，可为 NULL，成功后句柄失效。
 * @param[in] timeout_ms 非负等待预算，单位毫秒，传给 shutdown。
 * @return NULL 返回 CC_OK。shutdown 失败保留对象供重试，成功释放后禁止再次访问。
 */
cc_result_t cc_runtime_controller_destroy(
    cc_runtime_controller_t *controller,
    int timeout_ms
);

/**
 * @brief 空闲时切换到新会话 ID，标记为待创建会话。
 * @param[in,out] controller 非空、空闲且运行中的对象，会话版本递增，ID 按创建时选定模式生成。
 * @return 成功返回 CC_OK，繁忙或关闭返回状态错误，UUID 分配失败保留原选择。
 * 新会话在实际输入或 ensure_session 时持久化，既有历史继续由存储管理。
 */
cc_result_t cc_runtime_controller_start_new_session(cc_runtime_controller_t *controller);
/**
 * @brief 为已接受的输入持久化当前待创建会话。
 * @param[in,out] controller 非空、空闲且运行中的对象，创建期间登记会话变更阻止新运行。
 * @param[in] expected_session_id 非空预期会话 ID，调用期间借用，须与当前选择匹配。
 * @return 已创建或本次创建成功返回 CC_OK，选择变化或繁忙返回状态错误，其他返回底层错误。
 * 本调用只建立会话，空会话列表或预览流程由历史读取接口处理。
 */
cc_result_t cc_runtime_controller_ensure_session(cc_runtime_controller_t *controller,
                                                  const char *expected_session_id);

/**
 * @brief 读取会话列表确认目标存在后，将其设为当前会话。
 * @param[in,out] controller 非空、空闲且运行中的对象，列表读取期间登记会话变更。
 * @param[in] session_id 非空会话 ID，最多 95 字节，调用期间借用，入口先复制。
 * @return 成功切换返回 CC_OK，目标缺失返回 CC_ERR_NOT_FOUND，繁忙返回状态错误。
 * 读取期间进入关闭状态则返回 CC_ERR_CANCELLED 并保留原选择，读取错误向上传递。
 */
cc_result_t cc_runtime_controller_resume_session(
    cc_runtime_controller_t *controller, const char *session_id);

/**
 * @brief 借用当前运行时代际的存储后端读取会话列表。
 * @param[in] controller 非空对象，读取期间由 owner 保证寿命，shutdown 对此读取的排空由 owner 协调。
 * @param[out] out_sessions 必填独立输出，基础参数通过后置 NULL，返回拥有数组由 cc_session_free_array 释放。
 * @param[out] out_count 必填独立输出，基础参数通过后置零，后端填写条目数。
 * @return 返回代际取得或后端读取结果，缺少 list_sessions 返回 CC_ERR_UNSUPPORTED，错误时也须清理实际返回的数组。
 */
cc_result_t cc_runtime_controller_list_sessions(
    cc_runtime_controller_t *controller, cc_session_t **out_sessions, size_t *out_count);
/**
 * @brief 借用当前运行时代际的存储后端读取指定会话消息。
 * @param[in] controller 非空对象，读取期间由 owner 保证寿命及与销毁的协调。
 * @param[in] session_id 非空目标会话 ID，调用期间借用。
 * @param[in] limit 最近消息数限制，负数表示全部，数值原样传给存储后端。
 * @param[out] out_messages 必填独立输出，基础参数通过后置 NULL，返回拥有数组须逐项 cc_message_cleanup 后 free。
 * @param[out] out_count 必填独立输出，基础参数通过后置零，后端填写消息数。
 * @return 返回代际取得或后端读取结果，缺少 load_messages 返回 CC_ERR_UNSUPPORTED，错误时也须清理实际返回的数组。
 */
cc_result_t cc_runtime_controller_load_messages(
    cc_runtime_controller_t *controller, const char *session_id, int limit,
    cc_message_t **out_messages, size_t *out_count);

/**
 * @brief 通过兼容入口调用 start_new_session 切换新会话。
 * @param[in,out] controller 待切换对象，要求与 start_new_session 相同。
 * @return 原样返回新会话切换结果，共享视图由应用层维护。
 */
cc_result_t cc_runtime_controller_clear_session(cc_runtime_controller_t *controller);

/**
 * @brief 空闲时更新控制器配置，身份变化时切换到待创建的新会话。
 * @param[in,out] controller 非空、空闲且运行中的对象。
 * @param[in] config 非空配置，session_base/workspace_dir 为 NULL 时保留旧值，非空字符串先复制再替换。
 * drop_late_events 总会更新，fresh_session_on_start 沿用创建时模式。
 * @return 成功返回 CC_OK，繁忙或关闭返回状态错误，分配或新 ID 生成失败保留原配置与会话选择。
 */
cc_result_t cc_runtime_controller_apply_config(
    cc_runtime_controller_t *controller,
    const cc_runtime_controller_config_t *config
);

/**
 * @brief 在互斥量保护下复制控制器状态。
 * @param[in] controller 待查询对象，可为 NULL，非空时由调用者保证寿命。
 * @param[out] out_status 可选输出，非空时先清零并填写 size，再复制状态。
 * @post 输出非空时取得独立快照，controller 为 NULL 时保留全零状态及当前结构大小。
 */
void cc_runtime_controller_get_status(
    cc_runtime_controller_t *controller,
    cc_runtime_controller_status_t *out_status
);

/**
 * @brief 直接借出对象内部的当前会话 ID。
 * @pre 外层协调会话修改与借用读取，此入口直接访问缓冲区，跨线程快照使用 get_status。
 * @param[in] controller 待查询对象，可为 NULL。
 * @return 内部借用字符串，下一次会话修改或对象销毁时失效，NULL 对象返回 NULL。
 */
const char *cc_runtime_controller_session_id(cc_runtime_controller_t *controller);

#ifdef __cplusplus
}
#endif

#endif
