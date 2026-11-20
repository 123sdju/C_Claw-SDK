

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

/**
 * @brief 管理传输实例、当前版本握手及工具发现的 MCP 组合对象。
 *
 * 本实现支持 2024-11-05 版本的工具调用路径。manager 拥有 server 与传输，registry
 * 拥有适配工具，工具借用 server。owner 在销毁前关闭入口并等调用退出，先清理 registry。
 */
typedef struct cc_mcp_runtime_manager cc_mcp_runtime_manager_t;

/** @brief 由工厂移交的传输实例与只读虚表，manager 成功接管后统一销毁。 */
typedef struct cc_mcp_transport {
    void *self;
    const struct cc_mcp_transport_vtable *vtable;
} cc_mcp_transport_t;

/** @brief 同步传输及通知接口，虚表与回调上下文覆盖实例整个使用期。 */
typedef struct cc_mcp_transport_vtable {


    /**
     * @brief 发送带 id 的完整请求并等待对应响应正文。
     * @param[in,out] self 借用存活传输实例。
     * @param[in] request_json 调用期间借用的完整 JSON-RPC 请求。
     * @param[in] timeout_ms 正的剩余传输预算，单位毫秒。
     * @param[in] cancel_token 可选借用取消令牌，覆盖本次同步调用。
     * @param[out] out_response_json 必填空输出，交付的非 NULL 正文均由 manager free，错误时也回收。
     * @return 传输结果，拥有诊断由 manager 消费，成功正文还需协议与 id 校验。
     * @note is_serial 返回零时允许同一实例并发进入，端口负责请求/响应归属和原始正文预算。
     *       初始化始终在 server 锁内发送，回调避免重入同一 server 的 manager 调用。
     */
    cc_result_t (*send_json)(
        void *self,
        const char *request_json,
        int timeout_ms,
        cc_cancel_token_t *cancel_token,
        char **out_response_json
    );

    /**
     * @brief 在全部在途请求退出后重置连接，使下一请求重新握手。
     * @param[in,out] self 借用的传输实例，调用期间由 server 锁串行化。
     * @return 实际重置结果，拥有诊断由 manager 消费，失败保留重置待办状态。
     * @note 闲置 TTL 或协议失败需要重置时使用，回调缺失会返回不支持诊断。
     */
    cc_result_t (*reset)(void *self);

    /**
     * @brief 声明实例是否要求请求串行化。
     * @param[in] self 调用期有效的实例借用。
     * @return 非零表示串行，零允许并发，缺失回调时 manager 采用串行。
     */
    int (*is_serial)(void *self);

    /**
     * @brief 释放工厂成功移交的传输资源。
     * @param[in,out] self 消费的实例，调用前所有请求、通知与 reset 已退出。
     * @post 实例及其拥有的连接资源释放，原借用结束。
     */
    void (*destroy)(void *self);

    /**
     * @brief 发送无 id 的 JSON-RPC 通知，完成本地传输交接后返回。
     * @param[in,out] self 调用期间有效的 transport 实例。
     * @param[in] notification_json 借用的完整通知文本，寿命覆盖本次调用。
     * @param[in] timeout_ms 本次传输剩余预算，单位毫秒且大于零。
     * @param[in] cancel_token 可选借用取消令牌，覆盖本次同步发送。
     * @return 传输结果及拥有诊断，由调用者释放。通知交接成功返回 CC_OK。
     * @note initialized 握手使用此入口，通知没有 JSON-RPC 响应。manager 在初始化锁内调用，
     *       回调应结束传输后返回，避免重入同一 server 的 manager 调用。
     */
    cc_result_t (*send_notification)(void *self, const char *notification_json,
                                     int timeout_ms, cc_cancel_token_t *cancel_token);
} cc_mcp_transport_vtable_t;

/**
 * @brief 按 server 配置创建拥有的 MCP 传输。
 * @param[in] server_config 调用期间借用配置，工厂复制实例需长期持有的内容。
 * @param[out] out_transport 必填空资源槽，成功移交 self 和存活期有效虚表。
 * @param[in,out] user_data 可选借用工厂上下文，由调用者覆盖加载调用期。
 * @return 工厂结果，CC_OK 才移交实例，失败由工厂清理自身部分资源并交付拥有诊断。
 * @note 成功交付空虚表表示该配置未提供传输。握手需要 send_json 与 send_notification，
 *       连接重置需求还使用 reset，manager 会把缺失能力写入 server 诊断。
 */
typedef cc_result_t (*cc_mcp_transport_factory_fn)(
    const cc_config_mcp_server_t *server_config,
    cc_mcp_transport_t *out_transport,
    void *user_data
);

/**
 * @brief 创建保存传输工厂及 server 拥有表的管理器。
 * @param[in] factory 必填工厂，后续加载时同步调用。
 * @param[in] factory_user_data 可选工厂上下文借用，覆盖各次加载调用。
 * @param[out] out_manager 必填，成功交付拥有句柄，失败保持原值。
 * @return CC_OK、INVALID_ARGUMENT 或 OUT_OF_MEMORY，诊断由调用者释放。
 */
cc_result_t cc_mcp_runtime_manager_create(
    cc_mcp_transport_factory_fn factory,
    void *factory_user_data,
    cc_mcp_runtime_manager_t **out_manager
);

/**
 * @brief 释放全部 server、传输与管理器。
 * @param[in,out] manager 消费独占管理器，允许 NULL。
 * @pre owner 关闭入口并等待所有使用者退出，先销毁借用 server 的 registry 工具。
 * @post 非空管理器的全部拥有资源释放，原句柄失效。
 */
void cc_mcp_runtime_manager_destroy(cc_mcp_runtime_manager_t *manager);

/**
 * @brief 连接配置 server、发现远端工具并将编码后的公开名称注册到 registry。
 * @param[in,out] manager 借用管理器，NULL 时跳过，拥有成功接入的 server/transport，保持到工具使用结束。
 * @param[in] config 调用期间借用配置，NULL 或 MCP 关闭时跳过，server 名称由运行态复制。
 * @param[in,out] registry 借用未冻结注册表，NULL 时跳过，成功接管工具，调用者先销毁工具再销毁 manager。
 * @param[in,out] diagnostics 可选诊断集合，单 server 创建、初始化或列举失败可追加诊断后继续。
 * @return CC_OK、配置数组缺失或 server 表资源错误，部分 server/工具可能已接入，调用者按拥有顺序收尾。
 * @pre owner 独占 manager 加载及 registry 装配，完成后再接纳工具调用。
 * @note 公开名为 mcp.编码server.编码tool，组件内点号编码为 %2E、百分号编码为 %25，
 * 普通组件保持原名。远端 tools/call 始终使用原始工具名。
 */
cc_result_t cc_mcp_runtime_manager_load_tools(
    cc_mcp_runtime_manager_t *manager,
    const cc_config_t *config,
    cc_tool_registry_t *registry,
    cc_runtime_diagnostics_t *diagnostics
);


/**
 * @brief 完整解析两个 JSON-RPC 对象，并按 id 类型和值匹配归属。
 * @param[in] request_json 调用期间借用的完整请求 JSON。
 * @param[in] response_json 调用期间借用的完整响应 JSON。
 * @param[out] out_matches 必填，参数有效后先写零，两个 id 均存在且相等时写一。
 * @return CC_OK 或参数、JSON、id 副本分配失败结果，拥有诊断由调用者释放。
 * @note 字段名区分大小写，拒绝 NUL 转义和重复控制字段，缺 id 时成功返回不匹配。
 */
cc_result_t cc_mcp_jsonrpc_response_matches_request(
    const char *request_json,
    const char *response_json,
    int *out_matches
);

#ifdef __cplusplus
}
#endif

#endif
