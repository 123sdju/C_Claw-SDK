



#ifndef CC_TOOL_EXECUTOR_H
#define CC_TOOL_EXECUTOR_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_tool_call.h"
#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_cancel_token.h"

typedef struct cc_tool_executor_options {
    /** 可选的借用 token，寿命覆盖整个同步执行，executor 在准入边界检查，工具在自身调用中检查。 */
    cc_cancel_token_t *cancel_token;
    /** 单调时钟的绝对毫秒截止时间，零使用 runtime/lane 的工具预算；已进入工具后只能等待其返回再核对截止。 */
    uint64_t deadline_ms;
} cc_tool_executor_options_t;

/**
 * @brief 同步执行无操作账本的工具调用，组合实现查找、schema、策略、审批、lane 和结果预算。
 *
 * 启用或要求操作账本时通过 Agent runtime 的组合步骤执行，当前直接入口返回 UNSUPPORTED。
 * schema 回调缺失或返回 NULL、空串、精确的 {} 时跳过参数校验。其余情况经
 * cc_json_parse_strict 消费完整 schema 和参数正文，空参数按 {} 校验，工具接收原参数正文。
 * 取消/截止只在检查边界核对，工具自身负责执行中的取消和传入 timeout_ms 预算，返回后的
 * 正文、产物限制或截止检查可能发生在动作已经产生副作用之后。
 * @pre 调用者维持 runtime、registry/工具及其他依赖的寿命，并协调共享工具及外部回调的重入。
 * @param[in,out] runtime 借用的运行时，registry 须存在，运行期间配置和依赖保持有效。
 * @param[in] session_id 同步借用的非 NULL 会话标识，用于策略、工具上下文和观测归因。
 * @param[in] call 同步借用的调用，name 须非空，arguments_json 须非 NULL，字符串和工具 self 覆盖本次调用。
 * @param[in] options 可选的同步借用选项，token 寿命由外层覆盖，deadline_ms 为单调绝对毫秒值。
 * @param[out] out_result 必填且无待释放内容，执行基础参数通过后清零；入口拒绝时保持原值，开始执行后无论返回码均用 cc_tool_result_cleanup 归还拥有字段。
 * @return 已启用/要求账本为 CC_ERR_UNSUPPORTED，参数、取消、部分超时、资源及执行约束错误为非 OK；
 * 多数工具拒绝或业务失败为 CC_OK 且 out_result->ok 为零，调用者同时读取两层结果并核对可能的副作用。
 * @note 策略、审批、元数据、工具及同步观测回调发生在调用线程。审批函数与上下文成对快照后解锁调用，
 * 所借用的旧上下文由其拥有者保管至已取得快照的调用结束。部分错误消息复制失败可保留 NULL error。
 * lane 按完整工具身份动态构造，plugin/MCP 使用完整编码来源前缀，普通工具使用 tool.全名。
 * 同步策略和工具上下文仅在本次执行期借用 lane，构造失败返回资源错误，入口取消/截止检查先行。
 */
cc_result_t cc_tool_executor_execute_with_options(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_call_t *call,
    const cc_tool_executor_options_t *options,
    cc_tool_result_t *out_result
);


/**
 * @brief 以 NULL options 调用直接执行入口，沿用 runtime/lane 的工具预算。
 * @param[in,out] runtime 借用的存活运行时及其执行依赖。
 * @param[in] session_id 同步借用的非 NULL 会话标识。
 * @param[in] call 同步借用的调用及字符串，name 非空，arguments_json 非 NULL。
 * @param[out] out_result 必填且无待释放内容，初始化和清理契约与 execute_with_options 相同。
 * @return 原样返回 execute_with_options 结果，CC_OK 时继续检查业务 ok 标志。
 */
cc_result_t cc_tool_executor_execute(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_call_t *call,
    cc_tool_result_t *out_result
);

#endif
