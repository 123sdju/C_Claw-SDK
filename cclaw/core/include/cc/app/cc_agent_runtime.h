



#ifndef CC_AGENT_RUNTIME_H
#define CC_AGENT_RUNTIME_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_llm_provider.h"
#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_policy_engine.h"
#include "cc/ports/cc_sandbox.h"
#include "cc/ports/cc_memory_store.h"
#include "cc/ports/cc_event_bus.h"
#include "cc/ports/cc_logger.h"
#include "cc/ports/cc_thread.h"
#include "cc/core/cc_stream_chunk.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/app/cc_run_checkpoint.h"
#include "cc/app/cc_run_history.h"
#include "cc/util/cc_config.h"
#include <stddef.h>

/** @brief 创建时复制的运行资源限制，正数预算生效，各零值语义见字段说明。 */
typedef struct cc_runtime_limits {
    size_t size; /**< 结构尺寸标识，调用者提供当前基础布局，runtime 按值复制。 */
    int run_timeout_ms; /**< 每次执行调用的毫秒超时，非正数表示无限期，恢复调用重新计算截止时间。 */
    int tool_timeout_ms; /**< 单工具毫秒预算，转交执行器按其规则处理。 */
    int provider_timeout_ms; /**< 每次 provider 毫秒预算，与运行截止时间取较早者，非正数沿 provider 默认。 */
    size_t max_input_bytes; /**< 原输入正文上限，不含 NUL，零表示不限。 */
    size_t max_output_bytes; /**< 最终答复正文上限，并用作同步 provider 响应预算，零表示不限。 */
    size_t max_tool_result_bytes; /**< 工具结果正文预算，交执行器应用。 */
    size_t max_stream_bytes; /**< 本次运行累计片段 content 字节预算，含思考和工具增量，零表示不限。 */
    int max_steps; /**< 正数时优先于旧 config.max_steps，最终缺省至少一步。 */
    int max_concurrency; /**< 正数运行准入上限，非正数采用一，生命周期屏障由外层提供。 */
} cc_runtime_limits_t;

/**
 * @brief runtime 创建配置，标量按值复制，字符串和 MIME 列表深拷贝。
 *
 * 调用者提供完整基础布局，创建后可清理原配置，runtime 副本随后保持固定。
 */
typedef struct cc_agent_runtime_config {
    size_t size; /**< 配置结构尺寸标识，创建入口读取完整基础配置。 */
    int max_steps; /**< 兼容的模型步骤上限，limits.max_steps 正数优先，最终缺省一步。 */

    char *system_prompt; /**< 可选系统提示，创建时复制。 */

    char *workspace_dir; /**< 可选工作区路径，复制后用于会话缺省路径和工具上下文。 */

    char *model; /**< 可选模型名称，创建时复制并传入请求。 */
    char *model_extra_body_json; /**< 可选 provider 扩展 JSON 正文，创建时复制，具体合并规则由 provider 解释。 */
    int max_tokens; /**< 模型输出 token 配置，原值传入请求。 */
    double temperature; /**< 模型采样温度，原值传入请求。 */
    int context_window_tokens; /**< 上下文 token 预算，由 context builder 划分。 */

    double context_compress_threshold; /**< 触发历史压缩的预算比例，解释沿 context builder。 */

    int context_keep_recent; /**< 压缩时保留近期原文条数，非正数由 builder 采用默认值。 */

    int summary_max_tokens; /**< 独立摘要请求的输出 token 配置。 */

    double summary_temperature; /**< 独立摘要请求的采样温度。 */

    cc_multimodal_config_t multimodal; /**< 多模态开关及预算，MIME 前缀列表也深拷贝。 */
    int active_memory_enabled; /**< 可选活动记忆钩子的实例开关，还受编译开关控制。 */
    int active_memory_write_summary; /**< 完整答复成功后允许活动问答摘要写入。 */
    int active_memory_max_value_chars; /**< 活动摘要实际按字节截断并退到 UTF-8 字首，非正数采用 1600 字节。 */
    char *active_memory_category; /**< 可选活动摘要分类，创建时复制，NULL 使用 active_summary。 */
    cc_runtime_limits_t limits; /**< 创建时按值复制的统一预算。 */
} cc_agent_runtime_config_t;

/** @brief 拥有配置、热缓存和实例锁的 runtime 句柄，注入依赖继续借用。 */
typedef struct cc_agent_runtime cc_agent_runtime_t;
typedef struct cc_operation_journal cc_operation_journal_t;

/**
 * @brief runtime 的借用依赖集合，端口描述按值保存，底层对象始终由原拥有者管理。
 *
 * 基础布局须完整，末尾 journal/history 字段按 size 可见范围读取，依赖覆盖 runtime 全部调用及回调。
 */
typedef struct cc_agent_runtime_deps {
    size_t size; /**< 当前依赖结构可见字节数，用于读取可选尾字段。 */

    cc_llm_provider_t llm; /**< 借用 provider self/vtable，调用者协调共享并发和最终销毁。 */

    cc_tool_registry_t *tool_registry; /**< 借用的模型工具注册表，其冻结及工具寿命沿注册表契约。 */

    cc_session_store_t store; /**< 借用会话端口，附加 journal 时成功 append_records 须持久化。 */

    cc_policy_engine_t policy; /**< 借用工具策略端口。 */

    cc_sandbox_t sandbox; /**< 借用沙箱端口。 */

    cc_event_bus_t *event_bus; /**< 可选借用观测总线，订阅者寿命沿总线契约管理。 */

    cc_logger_t *logger; /**< 可选借用日志对象。 */

    cc_memory_store_t *memory_store; /**< 可选借用长期记忆及派生摘要存储。 */

    cc_tool_executor_pool_t *tool_pool; /**< 可选借用工具池，工作任务及关闭屏障由原拥有者协调。 */

    cc_tool_approval_fn approve_tool_call; /**< 可选借用审批函数，执行器持锁取得函数与上下文配对快照后解锁调用。 */

    void *approval_user_data; /**< 借用审批上下文，覆盖所有已取得该快照的执行调用。 */
    /* 可选借用的设备账本，跨 generation 共享，size 覆盖此字段时读取。 */
    cc_operation_journal_t *operation_journal;
    /* 非零且缺少账本时形成统一工具派发屏障。 */
    int require_operation_journal;
    /* 可选借用的运行恢复依赖，配置 checkpoint 时须有完整 history_find 及已附加、必需的账本。 */
    cc_run_checkpoint_store_t *run_checkpoints;
    cc_run_history_find_fn run_history_find;
    /* 可选的完整且有界内存历史遍历，缺少时核对路径要求 store 支持 limit<0 的完整加载。 */
    cc_run_history_visit_fn run_history_visit;
} cc_agent_runtime_deps_t;

/** @brief runtime 创建选项，基础配置须完整，provider 身份尾字段按 size 读取。 */
typedef struct cc_agent_runtime_options {
    size_t size; /**< 可见字节数，用于兼容可选身份尾字段。 */

    cc_agent_runtime_config_t config; /**< 基础配置在创建时复制，原字符串只借用至 create 返回。 */

    int thinking_mode; /**< 初始思考模式，后续通过专用 setter 更新。 */
    /* 可选 generation 观测身份，size 覆盖时深拷贝。 */
    const char *provider_name;
    /* 可选的稳定非秘密端点或租户身份，仅保存指纹。缺少时使用 generation 内热缓存，
     * 持久摘要复用要求此命名空间。后端变化使旧摘要失效时，拥有者应更新该身份。 */
    const char *summary_cache_namespace;
} cc_agent_runtime_options_t;

/** @brief 单次同步调用选项，token 在 provider、工具和步骤检查点同步借用。 */
typedef struct cc_agent_runtime_run_options {
    size_t size; /**< 调用者提供完整结构，手动工具 with_options 入口检查此尺寸。 */
    cc_cancel_token_t *cancel_token; /**< 可选借用 token，拥有者覆盖本次调用及回调。 */
} cc_agent_runtime_run_options_t;

/**
 * @brief 同步接收本轮实时片段，跨回调保留正文时由应用复制。
 * @param[in] chunk SDK 拥有的借用片段及字段，仅在本次回调期间有效。
 * @param[in,out] user_data 应用拥有的借用上下文，生命周期覆盖整个运行调用，跨并发运行的共享由应用同步。
 * @return CC_OK 继续当前片段链，其他错误经转发链返回 provider 或执行入口，provider 应传播该错误。
 * @note 回调结束后方可归还其所借用的资源，runtime 销毁须等待运行及回调退出。
 */
typedef cc_result_t (*cc_agent_runtime_stream_callback_fn)(
    const cc_stream_chunk_t *chunk,
    void *user_data
);

/** @brief 单次流式调用选项，provider 与应用回调均在调用返回前完成。 */
typedef struct cc_agent_runtime_stream_options {
    size_t size; /**< 结构尺寸标识，调用者提供包含全部基础字段的对象。 */
    /* 可选借用 token，覆盖运行、工具及片段回调。 */
    cc_cancel_token_t *cancel_token;
    /* 可选实时应用回调，观测事件另外发布到 event_bus。 */
    cc_agent_runtime_stream_callback_fn on_chunk;
    void *user_data; /**< 应用拥有的借用上下文，覆盖全部本次回调。 */
} cc_agent_runtime_stream_options_t;

/** @brief 外部已核实的工具副作用结论，解决入口据此保存证据，后续运行自行推进。 */
typedef enum cc_tool_recovery_disposition {
    /* 已确认动作完成，调用者同时提供 observed_result。 */
    CC_TOOL_RECOVERY_EXECUTED = 1,
    /* 已确认动作未执行，保存可恢复失败并允许后续运行规划新调用。 */
    CC_TOOL_RECOVERY_NOT_EXECUTED = 2
} cc_tool_recovery_disposition_t;

/**
 * @brief 校验可查询的 provider 能力，复制配置并绑定借用依赖后创建 runtime。
 * @param[in] deps 必需借用依赖，基础布局完整，可选 journal/history 尾字段按 size 读取。
 * @param[in] options 必需借用选项，配置字符串和 MIME 列表深拷贝，provider 身份可选尾字段按 size 读取。
 * @param[out] out_runtime 必需的无已有资源输出槽，仅成功赋拥有实例，失败保持原值，由调用者 destroy。
 * @return 成功为 CC_OK，参数及恢复依赖组合非法为 CC_ERR_INVALID_ARGUMENT，能力不足为 CC_ERR_UNSUPPORTED，分配、锁及能力查询错误原样返回。
 * @note 注入端口及外部对象持续借用，所有依赖和回调上下文覆盖使用期。配置 checkpoint 须有完整 history_find 及已附加、必需的 journal。
 * max_concurrency 只管理运行准入，调用者另行协调同会话写入、共享 provider 及销毁屏障。
 */
cc_result_t cc_agent_runtime_create(
    const cc_agent_runtime_deps_t *deps,
    const cc_agent_runtime_options_t *options,
    cc_agent_runtime_t **out_runtime
);

/**
 * @brief 销毁 runtime 拥有的配置、身份、热缓存及锁。
 * @pre 外层停止新入口并排空全部运行、provider/工具/应用回调、直接借用及锁等待者。
 * @param[in,out] runtime 待销毁实例，可为 NULL。
 * @post 实例寿命结束，注入的 provider、store、registry 等依赖继续由原拥有者管理，原指针由调用者清空。
 */
void cc_agent_runtime_destroy(cc_agent_runtime_t *runtime);

/**
 * @brief 在实例锁内更新后续请求使用的思考模式。
 * @param[in,out] runtime 借用实例，NULL 时跳过。
 * @param[in] enabled 非零归一为一，零关闭标志。
 * @post 有效实例的标志已更新，实际模型行为沿 provider 能力和实现。
 */
void cc_agent_runtime_set_thinking_mode(cc_agent_runtime_t *runtime, int enabled);

/**
 * @brief 在实例锁内读取当前思考模式标志。
 * @param[in] runtime 借用实例，可为 NULL。
 * @return 当前保存值，NULL 返回零，创建初值沿 options，setter 将更新值归一为零或一。
 */
int cc_agent_runtime_get_thinking_mode(cc_agent_runtime_t *runtime);

/**
 * @brief 在实例锁内配对更新后续审批使用的函数和上下文。
 * @param[in,out] runtime 借用实例，NULL 时跳过。
 * @param[in] approve_tool_call 可选借用审批函数，NULL 清除当前配置。
 * @param[in] user_data 借用上下文，寿命须覆盖已取得该配对快照的所有执行调用，旧上下文在借用结束后由原拥有者释放。
 * @post 后续审批可读取新配对，已取得旧快照的调用继续使用旧函数和上下文，替换只保存借用关系。
 */
void cc_agent_runtime_set_tool_approval(
    cc_agent_runtime_t *runtime,
    cc_tool_approval_fn approve_tool_call,
    void *user_data
);

/**
 * @brief 取得注入的事件总线借用指针。
 * @param[in] runtime 借用实例，可为 NULL。
 * @return event_bus，NULL 实例或未配置返回 NULL，总线寿命和销毁权限由原拥有者协调。
 */
cc_event_bus_t *cc_agent_runtime_event_bus(cc_agent_runtime_t *runtime);

/** @brief 辅助模型用量观测的用途分类。 */
typedef enum cc_auxiliary_usage_purpose {
    CC_AUXILIARY_USAGE_MEMORY_EXTRACTION = 1 /**< 独立记忆提取请求。 */
} cc_auxiliary_usage_purpose_t;
/**
 * @brief 发布一次辅助模型调用的最佳努力用量观测。
 * @param[in] runtime 借用实例，可为 NULL。
 * @param[in] purpose 当前支持 MEMORY_EXTRACTION，其他值跳过。
 * @param[in] model 可选借用模型名，NULL 或空时采用实例配置。
 * @param[in] session_id 可选借用会话标识。
 * @param[in] usage 可选借用用量，NULL 表示本次未报告，缓存字段按独立 reported/valid 位表达零值与缺失。
 * @post 已尝试发布，本次临时资源及结果由内部清理，真实费用由服务商记录及有效用量解释。
 */
void cc_agent_runtime_report_auxiliary_usage(cc_agent_runtime_t *runtime,
    cc_auxiliary_usage_purpose_t purpose, const char *model, const char *session_id,
    const cc_llm_usage_t *usage);
/**
 * @brief 将辅助用量同时归因到待处理记忆项的原对话身份。
 * @param[in] runtime 借用实例，可为 NULL。
 * @param[in] purpose 当前支持 MEMORY_EXTRACTION，其他值跳过。
 * @param[in] model 可选借用模型名，空时采用配置。
 * @param[in] session_id 可选借用会话标识。
 * @param[in] turn_id 可选借用原对话标识，允许与 checkpoint/run ID 独立。
 * @param[in] usage 可选借用有效用量视图，NULL 按未报告表达。
 * @post 以辅助用途、step=-1 及可选原对话身份尝试发布，输入保持借用。
 */
void cc_agent_runtime_report_auxiliary_usage_for_turn(cc_agent_runtime_t *runtime,
    cc_auxiliary_usage_purpose_t purpose, const char *model, const char *session_id,
    const char *turn_id, const cc_llm_usage_t *usage);

/**
 * @brief 取得注入的工具注册表借用指针。
 * @param[in] runtime 借用实例，可为 NULL。
 * @return tool_registry，缺失返回 NULL，表及工具寿命、冻结状态和销毁沿原拥有者与 registry 契约。
 */
cc_tool_registry_t *cc_agent_runtime_tool_registry(cc_agent_runtime_t *runtime);

/**
 * @brief 取得注入的工具执行池借用指针。
 * @param[in] runtime 借用实例，可为 NULL。
 * @return tool_pool，缺失返回 NULL，使用和最终关闭由原拥有者协调。
 */
cc_tool_executor_pool_t *cc_agent_runtime_tool_pool(cc_agent_runtime_t *runtime);

/**
 * @brief 取得 runtime 内嵌的会话存储端口描述地址。
 * @param[in] runtime 借用实例，可为 NULL。
 * @return 借用的内嵌端口，NULL 实例返回 NULL，地址寿命止于 runtime 销毁，底层 self/vtable 归注入依赖拥有者。
 */
cc_session_store_t *cc_agent_runtime_session_store(cc_agent_runtime_t *runtime);
/**
 * @brief 按原字符串核对 generation 创建时保存的 workspace。
 * @param[in] runtime 借用实例，可为 NULL，配置路径保持固定。
 * @param[in] workspace 借用候选路径，可为 NULL。
 * @return 两路径均存在且逐字节相等为一，其他情况为零。
 */
int cc_agent_runtime_workspace_matches(const cc_agent_runtime_t *runtime, const char *workspace);

/**
 * @brief 检查 provider 是否提供原生 chat_stream 函数。
 * @param[in] runtime 借用实例，可为 NULL。
 * @return 原生流入口存在时为非零，其他情况为零，具体调用结果仍由 provider 返回。
 */
int cc_agent_runtime_supports_stream(cc_agent_runtime_t *runtime);

/**
 * @brief 通过可选会话存储入口创建会话及其 workspace 关联。
 * @param[in] runtime 必需借用实例。
 * @param[in] session_id 必需借用会话标识，具体格式和重复规则由 store 校验。
 * @param[in] workspace_dir 可选借用路径，NULL 回退 runtime 配置路径。
 * @return 参数非法为 CC_ERR_INVALID_ARGUMENT，无 create_session 入口时为 CC_OK，其他后端结果原样返回。
 */
cc_result_t cc_agent_runtime_create_session(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *workspace_dir
);

/**
 * @brief 使用缺省同步选项处理用户输入，工具和模型共用统一步骤引擎。
 * @param[in,out] runtime 必需借用实例及依赖，外层保持 generation 租约与对象寿命。
 * @param[in] session_id 必需借用的已有会话标识。
 * @param[in] user_input 必需借用原输入，启用 checkpoint 时进一步要求非空。
 * @param[out] out_response 必需的无已有资源输出槽，基本参数合法后置空，成功答复或失败诊断都可能拥有，调用者均须 free。
 * @return 运行、准入、存储、模型、工具及资源错误沿带选项入口返回。达到步骤上限时为 CC_OK 并交付停止提示。
 */
cc_result_t cc_agent_runtime_handle_message(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    char **out_response
);

/**
 * @brief 带可选取消 token 执行同步消息处理。
 * @param[in,out] runtime 必需借用实例，负责本次运行准入名额的配对管理。
 * @param[in] session_id 必需借用会话标识，缺少 checkpoint 时同会话写入由外层串行化。
 * @param[in] user_input 必需借用原输入，正文预算在写历史前检查。
 * @param[in] options 可选借用完整结构，token 及其 source 覆盖本次运行和回调。
 * @param[out] out_response 基本参数合法后置空，非 NULL 正文始终归调用者，后续 claim 释放失败也须清理。
 * @return 准入满为 CC_ERR_QUEUE_FULL，取消/超时/预算各返回对应错误，其余检查点、模型、工具、存储和分配错误原样返回。
 * @note 失败可以已保存原输入或工具证据，恢复按 checkpoint、账本及历史核对实际推进位置。
 */
cc_result_t cc_agent_runtime_handle_message_with_options(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    const cc_agent_runtime_run_options_t *options,
    char **out_response
);

/**
 * @brief 同步运行模型循环并将临时片段送给可选应用回调。
 * @param[in,out] runtime 必需借用实例，生命周期覆盖 provider、工具及全部应用回调。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] user_input 必需借用原输入。
 * @param[in] options 可选借用完整流选项，回调上下文及 token 覆盖调用期。
 * @param[out] out_response 基础参数合法后置空，可能交付拥有的成功答复或失败诊断，所有返回路径均由调用者 free。
 * @return 同步运行的准入、资源、取消、超时、模型、工具或持久化结果原样返回。
 * @note 原生流逐片段调用应用，同步 chat 在完整响应返回后适配同一片段状态机。
 * USAGE 在内部合并，其余片段可转发。provider 返回后才执行工具，final 通过预算与取消检查后落库，原输入和工具证据具有独立提交边界。
 */
cc_result_t cc_agent_runtime_handle_message_stream_cb(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    const cc_agent_runtime_stream_options_t *options,
    char **out_response
);

/**
 * @brief 使用调用者预先生成的 run-UUID 启动可恢复运行。
 * @param[in,out] runtime 必需借用实例，要求 checkpoint 与完整历史依赖。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] user_input 必需借用非空输入。
 * @param[in] run_id 必需借用 run-UUID，要求未用于检查点、同会话输入或已完成 final。
 * @param[in] options 可选借用流选项。
 * @param[out] out_response 非 NULL 时先置空，任何拥有正文由调用者 free。
 * @return 空或非法 ID 为 CC_ERR_INVALID_ARGUMENT，能力缺失或重复身份为 CC_ERR_INVALID_STATE，其余运行结果原样返回。
 */
cc_result_t cc_agent_runtime_handle_message_with_run_id(
    cc_agent_runtime_t *runtime, const char *session_id, const char *user_input,
    const char *run_id, const cc_agent_runtime_stream_options_t *options, char **out_response);

/**
 * @brief claim 后重新读取持久检查点，以保留输入和已确认结果继续运行。
 * @param[in,out] runtime 必需借用实例，要求 checkpoint 和完整 history_find。
 * @param[in] run_id 必需借用运行 ID。
 * @param[in] options 可选借用流选项，覆盖本次恢复及回调。
 * @param[out] out_response 基础参数合法后置空，返回的任何拥有正文由调用者 free。
 * @return 参数或能力缺失为 CC_ERR_INVALID_ARGUMENT，检查点身份变化为 CC_ERR_STORAGE，未知工具待核对为 CC_ERR_INVALID_STATE，其余结果沿运行和依赖接口。
 * @note 已存在的原输入与稳定 final 按历史核对复用，未知副作用先由外部核对，恢复调用重新计算本次时间预算。
 */
cc_result_t cc_agent_runtime_resume_run(
    cc_agent_runtime_t *runtime, const char *run_id,
    const cc_agent_runtime_stream_options_t *options, char **out_response);

/**
 * @brief 列举检查点存储中的持久运行 ID。
 * @param[in] runtime 借用实例，须配置检查点存储。
 * @param[out] out_run_ids 必需空资源输出，初始化沿底层 list，成功后各 ID 和数组由调用者逐项 free。
 * @param[out] out_count 必需输出，底层 list 填写条数。
 * @return 参数或能力缺失为 CC_ERR_INVALID_ARGUMENT，其余列表结果原样返回。
 */
cc_result_t cc_agent_runtime_list_interrupted_runs(
    cc_agent_runtime_t *runtime, char ***out_run_ids, size_t *out_count);
/**
 * @brief 读取持久运行检查点供调用者查看。
 * @param[in] runtime 借用实例，须配置检查点存储。
 * @param[in] run_id 必需借用运行 ID。
 * @param[out] out_checkpoint 必需无已有资源结构，初始化沿底层 load，取得的字段由调用者 cc_run_checkpoint_cleanup。
 * @return 参数或能力缺失为 CC_ERR_INVALID_ARGUMENT，其余加载及校验结果原样返回。
 */
cc_result_t cc_agent_runtime_load_interrupted_run(
    cc_agent_runtime_t *runtime, const char *run_id, cc_run_checkpoint_t *out_checkpoint);

/**
 * @brief claim 目标会话并核对后删除运行检查点。
 * @param[in] runtime 借用实例，须配置检查点存储。
 * @param[in] run_id 必需借用运行 ID。
 * @return 参数或能力缺失为 CC_ERR_INVALID_ARGUMENT，其余加载、claim、删除及释放结果原样返回。
 * @note 设备账本中的未知工具由独立核对入口继续管理，删除检查点自身也可能遇到部分持久化结果。
 */
cc_result_t cc_agent_runtime_abandon_run(cc_agent_runtime_t *runtime, const char *run_id);

/**
 * @brief 通过模型注册表显式执行手动工具，应用策略、账本和会话记录。
 * @param[in,out] runtime 必需借用实例，调用占用运行名额及可选会话 claim。
 * @param[in] session_id 必需借用的已有会话标识。
 * @param[in] call 必需借用调用描述，有账本时每次尝试须用全新调用 ID，历史读取失败时停止派发。
 * @param[out] out_result 必需的无已有资源结构，基本参数合法后清零，任何拥有字段由调用者 cc_tool_result_cleanup，错误时同样清理。
 * @return 准入、参数、策略、资源、执行和持久化结果原样返回，工具业务失败也可能以 CC_OK 和 out_result.ok=0 表达。
 * @note 有 checkpoint 时 claim 串行同会话运行，其他场景由调用者串行化同会话写入。错误可能发生在工具副作用或部分记录已提交之后。
 */
cc_result_t cc_agent_runtime_execute_manual_tool(
    cc_agent_runtime_t *runtime, const char *session_id, const cc_tool_call_t *call,
    cc_tool_result_t *out_result);
/**
 * @brief 执行应用提供的同步工具实现，沿用 schema、策略、账本、同名阻断和会话提交链。
 * @param[in,out] runtime 借用实例，要求已附加设备 journal。
 * @param[in] session_id 必需借用的已有会话标识。
 * @param[in] call 必需借用调用描述，名称须与显式实现一致。
 * @param[in] trusted_tool 必需借用实现，覆盖调用期，注册与销毁权限仍归应用。
 * @param[out] out_result 非 NULL 时先清零，取得的拥有字段由调用者在所有返回路径 cleanup。
 * @return 空实现为 CC_ERR_INVALID_ARGUMENT，缺少实例或账本为 CC_ERR_INVALID_STATE，其余结果沿统一手动执行链。
 * @note 此入口供受信任应用调用，实现仅在本次执行借用，模型工具 schema 由注册表独立决定。
 */
cc_result_t cc_agent_runtime_execute_manual_tool_with_impl(
    cc_agent_runtime_t *runtime, const char *session_id, const cc_tool_call_t *call,
    const cc_tool_t *trusted_tool, cc_tool_result_t *out_result);
/**
 * @brief 将可选取消 token 传入手动工具执行链。
 * @param[in,out] runtime 必需借用实例。
 * @param[in] session_id 必需借用已有会话标识。
 * @param[in] call 必需借用调用描述。
 * @param[in] trusted_tool 可选借用显式实现，NULL 使用注册表，非 NULL 要求 journal。
 * @param[in] options 可选借用选项，非 NULL 时 size 须覆盖当前结构，token 覆盖同步调用及工作任务借用期。
 * @param[out] out_result 非 NULL 时先清零，拥有字段在所有返回路径由调用者 cleanup。
 * @return 选项非法为 CC_ERR_INVALID_ARGUMENT，显式实现缺少账本为 CC_ERR_INVALID_STATE，其他结果沿统一手动执行链。
 */
cc_result_t cc_agent_runtime_execute_manual_tool_with_options(
    cc_agent_runtime_t *runtime, const char *session_id, const cc_tool_call_t *call,
    const cc_tool_t *trusted_tool, const cc_agent_runtime_run_options_t *options,
    cc_tool_result_t *out_result);

/**
 * @brief 优先从设备账本、再从历史扫描取得待外部核对的工具调用副本。
 * @param[in,out] runtime 借用实例，调用期占用并归还运行名额。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] tool_call_id 可选借用调用 ID，NULL 按各来源的待处理顺序选择。
 * @param[out] out_call 必需无已有资源结构，参数合法后清零，成功副本由调用者 cc_tool_call_cleanup，失败内部清理。
 * @return 命中为 CC_OK，未找到为 CC_ERR_NOT_FOUND，参数、准入及依赖错误原样返回。
 * @note 兼容历史路径请求最多 500 条消息，其覆盖范围沿后端加载规则，设备完整恢复还依赖账本和完整 history 能力。
 */
cc_result_t cc_agent_runtime_get_pending_tool_call(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *tool_call_id,
    cc_tool_call_t *out_call
);

/**
 * @brief 保存外部已确认的工具副作用结论，推进关联检查点并结束可确认账本项。
 * @pre 调用者先核实设备或外部状态，结果仍未知时继续保留待核对状态。
 * @param[in,out] runtime 必需借用实例，配对管理准入及核对期间的 claim。
 * @param[in] session_id 必需借用会话标识。
 * @param[in] tool_call_id 必需且非空的借用调用 ID。
 * @param[in] disposition 只能为 EXECUTED 或 NOT_EXECUTED，由调用者的实际核对决定。
 * @param[in] observed_result EXECUTED 时必需借用结果，NOT_EXECUTED 时忽略，拥有字段仍归调用者。
 * @return 非法结论或缺少必需参数为 CC_ERR_INVALID_ARGUMENT，目标缺失为 CC_ERR_NOT_FOUND，已持久决定冲突为 CC_ERR_INVALID_STATE，其他准入、资源和持久化错误原样返回。
 * @note 本调用保存决定和产物，后续模型或恢复运行决定下一步动作。持久化失败可保留已提交证据，重试会先比对旧决定。
 */
cc_result_t cc_agent_runtime_resolve_pending_tool(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *tool_call_id,
    cc_tool_recovery_disposition_t disposition,
    const cc_tool_result_t *observed_result
);

#endif
