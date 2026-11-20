



#ifndef CC_TOOL_H
#define CC_TOOL_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_tool_call.h"

#ifdef __cplusplus
extern "C" {
#endif

/* event bus/logger/memory store 等服务在此只做前置声明，避免工具接口反向依赖实现。 */
typedef struct cc_event_bus cc_event_bus_t;

typedef struct cc_logger cc_logger_t;

typedef struct cc_memory_store cc_memory_store_t;


typedef struct cc_tool_executor_pool cc_tool_executor_pool_t;


typedef struct cc_cancel_token cc_cancel_token_t;

/**
 * @brief 同步向注册方询问本次调用是否批准。
 * @param[in] tool_name 调用期间借用的工具名称。
 * @param[in] arguments_json 调用期间借用的参数正文。
 * @param[in] reason 调用期间借用的审批原因。
 * @param[in,out] user_data 注册方拥有的上下文，寿命覆盖已接纳回调，并发由注册方协调。
 * @return 非零批准，零拒绝，不交接参数拥有权。
 * @note 回调在 executor 审批阶段同步调用，缺少 handler 的拒绝规则由 policy/executor 实施。
 */
typedef int (*cc_tool_approval_fn)(
    const char *tool_name,
    const char *arguments_json,
    const char *reason,
    void *user_data
);

/*
 * runtime 注入的服务借用集合，字段由相应 owner 保持至工具调用退出。
 * 工具仅在 call 期间使用这些地址，后续保留数据须建立独立副本或取得对应接口租约。
 */
typedef struct cc_runtime_services {

    cc_event_bus_t *event_bus; /**< 可选事件总线借用。 */

    cc_logger_t *logger; /**< 可选日志实例借用。 */

    cc_memory_store_t *memory_store; /**< 可选记忆端口借用。 */

    cc_tool_executor_pool_t *tool_pool; /**< 可选执行池借用。 */

    cc_tool_approval_fn approve_tool_call; /**< 可选同步审批回调。 */

    void *approval_user_data; /**< 与审批回调配对的注册方上下文。 */
} cc_runtime_services_t;

/*
 * 单次调用的只读借用上下文，由 caller 保持结构及各指向对象覆盖同步 call。
 * 不同工具按自身实现响应预算和取消，executor 在调用前后另行核对。
 */
typedef struct cc_tool_context {
    const char *session_id; /**< 可选会话身份借用。 */
    const char *workspace_dir; /**< 文件类工具使用的根目录借用。 */
    const char *user_id; /**< 可选用户身份借用。 */
    const cc_runtime_services_t *services; /**< 可选服务集合借用。 */
    cc_cancel_token_t *cancel_token; /**< 可选协作取消令牌借用。 */
    int timeout_ms; /**< 工具预算毫秒，具体未设置值和执行方式沿调用者与工具契约。 */
    const char *lane_name; /**< 与本次策略及执行池对应的 lane 借用。 */
    unsigned long generation; /**< 本次执行所处代号。 */
    /* 输出文本预算字节数，不含 NUL，零表示未指定。 */
    size_t max_result_bytes;
    /* 有账本时为持久操作 ID，否则为 provider/runtime 调用 ID，可传给支持幂等键的后端。 */
    const char *idempotency_key;
} cc_tool_context_t;


/* 工具 vtable 前置声明。 */
typedef struct cc_tool_vtable cc_tool_vtable_t;

/* 工具接口对象前置声明。 */
typedef struct cc_tool cc_tool_t;

/*
 * 工具描述符，创建成功后由 creator 拥有 self，注册成功才交给 registry。
 * 查找返回浅借用，使用方保持 registry 与 vtable 存活，释放责任仍在注册表 owner。
 */
struct cc_tool {
    void *self; /**< 具体工具状态，静态无状态工具允许 NULL。 */

    const cc_tool_vtable_t *vtable; /**< 覆盖工具寿命的稳定方法表借用。 */
};


/*
 * 元数据回调返回工具寿命内稳定的只读文本，registry 可在自身锁内调用这些方法。
 * 工具的实际调用并发由 executor/pool 和实现自身约定，方法表不自动提供对象锁。
 */
struct cc_tool_vtable {


    /** @brief 提供稳定工具名。 @param[in] self 存活状态借用，是否可空沿具体工具。 @return 工具寿命内有效的名称借用，注册表接纳 NULL 名称但无法按其查找。 */
    const char *(*name)(void *self);



    /** @brief 提供模型说明。 @param[in] self 存活状态借用。 @return 稳定描述借用，可为 NULL，调用者不释放。 */
    const char *(*description)(void *self);



    /** @brief 提供参数 JSON Schema 对象文本。 @param[in] self 存活状态借用。 @return 工具寿命内稳定借用，可为 NULL 表示无 schema，参与导出时须为完整对象。 */
    const char *(*schema_json)(void *self);



    /**
     * @brief 同步执行一次工具调用并发布拥有结果字段。
     * @param[in,out] self 调用期间有效的工具状态借用，是否可空和并发能力沿实现。
     * @param[in] args_json 调用期借用参数，executor 在存在 schema 时做子集校验，直接调用者按具体工具要求提供。
     * @param[in] ctx 调用期上下文借用，是否允许 NULL 沿具体工具，工具不修改或长期保留。
     * @param[out] out_result 必填新对象或已 cleanup 槽，所有已发布字段均由 caller cleanup，包括非 OK 返回时的部分字段。
     * @return 业务失败通常以 CC_OK 和 ok=0 反馈，SDK/系统失败返回非 OK，拥有诊断由 caller cc_result_free。
     * @note 失败可能已产生外部副作用，执行及取消的最终状态需按具体工具和操作账本核对。
     */
    cc_result_t (*call)(
        void *self,
        const char *args_json,
        const cc_tool_context_t *ctx,
        cc_tool_result_t *out_result
    );



    /** @brief 消费工具拥有状态。 @param[in,out] self 具体实现拥有的状态，NULL 支持沿实现。 @pre owner 关闭入口并等待已接纳调用退出。 @post 原状态及借用失效，借用的外部服务沿其 owner 管理。 */
    void (*destroy)(void *self);
};

#ifdef __cplusplus
}
#endif

#endif
