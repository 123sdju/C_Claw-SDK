

#ifndef CC_MCP_RUNTIME_MANAGER_H
#define CC_MCP_RUNTIME_MANAGER_H

#include "cc/app/cc_cancel_token.h"
#include "cc/app/cc_runtime_features.h"
#include "cc/core/cc_result.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/util/cc_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MCP runtime manager 不透明句柄。
 *
 * manager 负责根据配置连接 MCP server、发现工具并注册到 tool registry。SDK 只提供协议
 * 管理层，不内置具体业务 server。
 */
typedef struct cc_mcp_runtime_manager cc_mcp_runtime_manager_t;

/* MCP transport 接口对象：self + vtable，便于进程管道、stdio、测试 fake 等实现。 */
typedef struct cc_mcp_transport {
    void *self;
    const struct cc_mcp_transport_vtable *vtable;
} cc_mcp_transport_t;

/* MCP transport vtable。 */
typedef struct cc_mcp_transport_vtable {


    /*
     * 发送 JSON-RPC 请求并等待响应。
     *
     * request_json 借用；out_response_json 成功后由调用方 free()。timeout/cancel_token
     * 用于避免 server 卡死阻塞 runtime。
     */
    cc_result_t (*send_json)(
        void *self,
        const char *request_json,
        int timeout_ms,
        cc_cancel_token_t *cancel_token,
        char **out_response_json
    );

    /* 重置连接状态；用于协议错误或 reload 后重新同步。 */
    cc_result_t (*reset)(void *self);

    /* 返回 transport 是否只能串行请求；stdio 类 transport 通常需要串行化。 */
    int (*is_serial)(void *self);

    /* 销毁 transport self。 */
    void (*destroy)(void *self);
} cc_mcp_transport_vtable_t;

/*
 * MCP transport factory。
 *
 * server_config 只在调用期间借用；out_transport 成功后由 manager 管理。user_data 由调用方
 * 注入，用于测试 fake 或平台特定创建参数。
 */
typedef cc_result_t (*cc_mcp_transport_factory_fn)(
    const cc_config_mcp_server_t *server_config,
    cc_mcp_transport_t *out_transport,
    void *user_data
);

/* 创建 MCP runtime manager；factory 用于后续 load_tools 时创建 server transport。 */
cc_result_t cc_mcp_runtime_manager_create(
    cc_mcp_transport_factory_fn factory,
    void *factory_user_data,
    cc_mcp_runtime_manager_t **out_manager
);

/* 销毁 manager 和所有已加载 transport/tool state。 */
void cc_mcp_runtime_manager_destroy(cc_mcp_runtime_manager_t *manager);

/* 根据 config 加载 MCP tools 并注册到 registry；diagnostics 记录可恢复加载问题。 */
cc_result_t cc_mcp_runtime_manager_load_tools(
    cc_mcp_runtime_manager_t *manager,
    const cc_config_t *config,
    cc_tool_registry_t *registry,
    cc_runtime_diagnostics_t *diagnostics
);


/* 校验 JSON-RPC response id 是否匹配 request id；out_matches 写 0/1。 */
cc_result_t cc_mcp_jsonrpc_response_matches_request(
    const char *request_json,
    const char *response_json,
    int *out_matches
);

#ifdef __cplusplus
}
#endif

#endif
