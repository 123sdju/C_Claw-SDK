



#ifndef CC_TOOL_H
#define CC_TOOL_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_tool_call.h"

/* event bus/logger/memory store 等服务在此只做前置声明，避免工具接口反向依赖实现。 */
typedef struct cc_event_bus cc_event_bus_t;

typedef struct cc_logger cc_logger_t;

typedef struct cc_memory_store cc_memory_store_t;


typedef struct cc_tool_executor_pool cc_tool_executor_pool_t;


typedef struct cc_cancel_token cc_cancel_token_t;

/*
 * 工具审批回调。
 *
 * 返回非 0 表示批准，0 表示拒绝。tool_name/arguments_json/reason 都只在调用期间借用；
 * user_data 由注册方拥有。无审批 handler 时，高风险工具应默认 deny，这个策略由
 * tool executor/policy engine 执行。
 */
typedef int (*cc_tool_approval_fn)(
    const char *tool_name,
    const char *arguments_json,
    const char *reason,
    void *user_data
);

/*
 * runtime 注入给工具的服务集合。
 *
 * 这是依赖注入结构：工具不直接链接全局单例，而是通过 services 使用 logger、memory、
 * event bus、tool pool 和 approval handler。字段由 runtime 拥有，工具不能保存超过
 * call 生命周期，除非明确复制所需数据。
 */
typedef struct cc_runtime_services {

    cc_event_bus_t *event_bus;

    cc_logger_t *logger;

    cc_memory_store_t *memory_store;

    cc_tool_executor_pool_t *tool_pool;

    cc_tool_approval_fn approve_tool_call;

    void *approval_user_data;
} cc_runtime_services_t;

/*
 * 单次工具调用上下文。
 *
 * 字符串字段均为借用指针；workspace_dir 是文件类工具的安全根目录，cancel_token 用于
 * 长耗时工具响应取消，timeout_ms 是工具层预算，lane/generation 用于执行池和 registry
 * snapshot 保持一致。工具实现不应修改该结构。
 */
typedef struct cc_tool_context {
    const char *session_id;
    const char *workspace_dir;
    const char *user_id;
    const cc_runtime_services_t *services;
    cc_cancel_token_t *cancel_token;
    int timeout_ms;
    const char *lane_name;
    unsigned long generation;
} cc_tool_context_t;


/* 工具 vtable 前置声明。 */
typedef struct cc_tool_vtable cc_tool_vtable_t;

/* 工具接口对象前置声明。 */
typedef struct cc_tool cc_tool_t;

/*
 * 工具接口对象。
 *
 * self 指向具体工具实现，vtable 提供多态函数。registry 接收该结构后通常拥有 self；
 * 查找返回的是浅拷贝，调用方不能直接释放 self。
 */
struct cc_tool {
    void *self;

    const cc_tool_vtable_t *vtable;
};


/*
 * 工具 vtable。
 *
 * 这是 C 语言模拟接口/虚函数表的核心。name/description/schema_json 返回的字符串由
 * 工具实现拥有，至少在工具生命周期内有效；call 把结果写入调用方提供的 out_result；
 * destroy 释放 self。
 */
struct cc_tool_vtable {


    /* 返回稳定工具名，用于 LLM tool call 和 registry 查找。 */
    const char *(*name)(void *self);



    /* 返回给模型和调试 UI 使用的简短描述；可返回 NULL。 */
    const char *(*description)(void *self);



    /* 返回 JSON Schema 参数对象字符串；用于 provider tool schema 和执行前校验。 */
    const char *(*schema_json)(void *self);



    /*
     * 执行工具。
     *
     * args_json 是已通过最小 schema 校验的参数字符串；out_result 由调用方提供，工具
     * 填充其字段并把资源所有权交给调用方 cleanup。工具级失败应返回 ok=0 的 result，
     * SDK/系统级失败才返回非 OK cc_result_t。
     */
    cc_result_t (*call)(
        void *self,
        const char *args_json,
        const cc_tool_context_t *ctx,
        cc_tool_result_t *out_result
    );



    /* 销毁工具实现 self；registry destroy 时调用。 */
    void (*destroy)(void *self);
};

#endif
