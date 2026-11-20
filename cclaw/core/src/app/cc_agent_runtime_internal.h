

#ifndef CC_AGENT_RUNTIME_INTERNAL_H
#define CC_AGENT_RUNTIME_INTERNAL_H

#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_operation_journal.h"
#include "cc/app/cc_tool_executor.h"
#include "cc/util/cc_json.h"
#include <stddef.h>
#include <stdint.h>

#define CC_CONTEXT_SUMMARY_CACHE_SLOTS 4

/** @brief runtime generation 拥有的单槽摘要热缓存，访问及替换受 runtime mutex 保护。 */
typedef struct cc_context_summary_cache_entry {
    char *session_id; /**< 拥有的来源会话 ID。 */
    char *summary; /**< 拥有的摘要正文。 */
    uint64_t history_fingerprint; /**< 对应被摘要历史前缀的指纹。 */
    size_t summarized_count; /**< 已摘要的历史消息条数。 */
    uint64_t last_used; /**< 本 generation 内用于淘汰选择的最近使用序号。 */
} cc_context_summary_cache_entry_t;

/**
 * @brief 单个 runtime generation 的配置、依赖借用视图及可变运行状态。
 *
 * mutex 保护准入计数、thinking 标志、审批配对及摘要缓存。依赖对象寿命和 generation 租约由外层保证，
 * 销毁前须排空全部运行、回调、锁等待者与直接借用。
 */
struct cc_agent_runtime {
    cc_agent_runtime_config_t config; /**< 创建时复制的标量与拥有的字符串、MIME 列表，随后保持固定。 */
    char *provider_name; /**< 拥有的观测身份副本，延后事件仍归因于此 generation。 */
    uint64_t summary_cache_namespace_hash; /**< 非秘密端点命名空间的指纹。 */
    int summary_cache_namespace_available; /**< 已配置持久摘要命名空间。 */
    cc_llm_provider_t llm; /**< 按值保存的借用 provider self/vtable。 */
    cc_tool_registry_t *tool_registry; /**< 借用的工具注册表，工具状态沿注册表冻结契约。 */
    cc_session_store_t store; /**< 按值保存的借用会话存储端口。 */
    cc_policy_engine_t policy; /**< 按值保存的借用策略端口。 */
    cc_sandbox_t sandbox; /**< 按值保存的借用沙箱端口。 */
    cc_event_bus_t *event_bus; /**< 借用的观测事件总线。 */
    cc_logger_t *logger; /**< 借用的日志对象。 */
    cc_memory_store_t *memory_store; /**< 借用的长期记忆及派生摘要存储。 */
    cc_tool_executor_pool_t *tool_pool; /**< 借用的工具池，关闭和工作任务寿命由外层负责。 */
    cc_operation_journal_t *operation_journal; /**< 跨 generation 借用的设备账本。 */
    int require_operation_journal; /**< 必需账本缺失时禁止工具派发。 */
    cc_run_checkpoint_store_t *run_checkpoints; /**< 可选借用的运行检查点存储及会话 claim 管理器。 */
    cc_run_history_find_fn run_history_find; /**< 借用的完整历史定位能力。 */
    cc_run_history_visit_fn run_history_visit; /**< 可选的完整、有界内存历史遍历能力。 */
    cc_runtime_services_t services; /**< 给工具上下文的借用服务，审批函数及 userdata 在同锁下成对更新和读取。 */
    int thinking_mode; /**< 锁内读写的请求思考标志。 */
    int active_runs; /**< 运行、手动工具及核对入口已占用的准入名额，外层另行提供销毁屏障。 */
    cc_context_summary_cache_entry_t summary_cache[CC_CONTEXT_SUMMARY_CACHE_SLOTS]; /**< generation 自有的四槽摘要缓存。 */
    uint64_t summary_cache_clock; /**< 摘要缓存最近使用序号，访问在实例锁内。 */
    cc_mutex_t mutex; /**< 保护可变运行状态，外层在销毁前排空全部使用者。 */
};

/**
 * @brief 判断必需恢复账本缺失是否构成工具派发屏障。
 * @param[in] runtime 可选借用实例，相关依赖在创建后保持固定。
 * @return 有效实例要求账本且尚未附加时为一，其他情况为零。
 */
static inline int cc_agent_runtime_recovery_blocks_tools(const cc_agent_runtime_t *runtime)
{
    return runtime && runtime->require_operation_journal && !runtime->operation_journal;
}

typedef struct cc_tool_dispatch_checkpoint {
    char *operation_id; /**< 执行器交付的拥有账本 ID，调用者 free 并据此结束或保留 UNKNOWN。 */
    int outcome_unknown; /**< 已派发动作的结果仍待外部核对。 */
    int not_dispatched; /**< 执行器已确认本次动作尚未开始。 */
} cc_tool_dispatch_checkpoint_t;

/**
 * @brief 发布一次模型用量观测，运行和原对话标识取空。
 * @param[in] runtime 借用实例及事件总线，可为 NULL。
 * @param[in] session_id 可选借用会话标识。
 * @param[in] step 模型步骤序号，辅助调用可用负一。
 * @param[in] model 可选借用模型名。
 * @param[in] purpose 可选借用用途，区分 agent、摘要和独立提取。
 * @param[in] usage 可选借用用量视图，NULL 以未报告表达，缓存计数沿独立有效位。
 * @post 临时 JSON 和发布结果已清理，观测失败由内部消费。
 */
void cc_agent_runtime_publish_llm_usage(cc_agent_runtime_t *runtime, const char *session_id,
    int step, const char *model, const char *purpose, const cc_llm_usage_t *usage);
/**
 * @brief 附运行及原对话身份发布一次模型用量观测。
 * @param[in] runtime 借用实例及事件总线，可为 NULL。
 * @param[in] session_id 可选借用会话标识。
 * @param[in] run_id 可选借用运行标识。
 * @param[in] origin_turn_id 可选借用原对话标识。
 * @param[in] step 模型步骤序号。
 * @param[in] model 可选借用模型名。
 * @param[in] purpose 可选借用用途。
 * @param[in] usage 可选借用用量，字段是否报告及有效由对应位决定。
 * @post 已尝试发布并清理临时资源，借用输入由原拥有者管理。
 */
void cc_agent_runtime_publish_llm_usage_with_identity(cc_agent_runtime_t *runtime,
    const char *session_id, const char *run_id, const char *origin_turn_id,
    int step, const char *model, const char *purpose, const cc_llm_usage_t *usage);

/**
 * @brief 为一次运行读取历史和记忆，应用预算并构建拥有的请求消息。
 * @param[in,out] runtime 借用的有效 generation，构造可能生成摘要并更新热缓存与派生存储。
 * @param[in] session_id 必需借用会话标识，同会话写入由 claim 或外层串行化。
 * @param[in] run_id 可选借用运行 ID，仅用于观测关联。
 * @param[in] system_prompt 可选借用系统提示，具体构造规则沿 builder。
 * @param[in] cancel_token 可选借用 token，覆盖构造及同步摘要调用。
 * @param[out] out_messages 必需无已有资源输出，初始化沿 builder，成功数组由调用者逐项 cleanup 后 free。
 * @param[out] out_count 必需输出，成功填写消息条数，失败清理本次部分结果。
 * @return builder 的参数、预算、取消、资源、媒体及依赖错误原样返回。
 */
cc_result_t cc_context_builder_build_messages_for_run(cc_agent_runtime_t *runtime,
    const char *session_id, const char *run_id, const char *system_prompt,
    cc_cancel_token_t *cancel_token, cc_message_t **out_messages, size_t *out_count);

/**
 * @brief 统一检查参数、策略和恢复账本后执行工具，交付结果及派发证据。
 * @param[in] runtime 借用实例，依赖和 generation 租约覆盖同步调用及工具工作任务。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] call 必需借用调用，参数为完整 JSON 对象。
 * @param[in] options 可选借用执行器选项，取消及截止时间沿执行器契约。
 * @param[out] out_result 必需空资源输出，基础参数合法后清零，错误返回也由调用者 cleanup。
 * @param[out] out_checkpoint 必需空资源输出，基础参数合法后清零并置 not_dispatched，拥有 operation_id 由调用者 free 并完成账本收尾。
 * @param[in] trusted_tool 可选借用应用显式实现，名称须匹配，寿命覆盖调用。
 * @param[in] run_id 可选借用观测运行 ID。
 * @param[in] step 观测步骤序号。
 * @return 工具业务拒绝可用 CC_OK 加失败结果表达，无法可靠取得结果或持久证据时返回对应执行器错误。
 */
cc_result_t cc_tool_executor_execute_guarded(
    cc_agent_runtime_t *runtime, const char *session_id, const cc_tool_call_t *call,
    const cc_tool_executor_options_t *options, cc_tool_result_t *out_result,
    cc_tool_dispatch_checkpoint_t *out_checkpoint, const cc_tool_t *trusted_tool,
    const char *run_id, int step);


/**
 * @brief 保存意图、经守卫执行注册表工具，再提交工具结果和相关会话记录。
 * @param[in] runtime 借用实例及依赖，外层协调同会话写入与寿命。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] call 必需借用工具调用。
 * @param[in] reasoning_content 可选借用思考正文，复制进意图。
 * @param[in] cancel_token 可选借用取消 token。
 * @param[in] deadline_ms 绝对单调毫秒截止时间，零表示无限期。
 * @return 工具、资源、历史及账本结果原样返回，错误时仍可能已提交部分持久证据。
 */
cc_result_t cc_agent_runtime_execute_tool_step(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_call_t *call,
    const char *reasoning_content,
    cc_cancel_token_t *cancel_token,
    uint64_t deadline_ms
);


/**
 * @brief 生成随机消息 ID 并保存完整 assistant 正文及可选思考。
 * @pre 调用者已确认该文本达到完整最终答复的提交条件。
 * @param[in] runtime 借用实例及有效的会话批次追加端口。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] text 可选借用正文，NULL 采用空字符串。
 * @param[in] reasoning_content 可选借用思考正文。
 * @return UUID、消息构造、复制及追加结果原样返回，本次拥有 ID 和消息始终清理。
 */
cc_result_t cc_agent_runtime_store_assistant_text(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *text,
    const char *reasoning_content
);

#endif
