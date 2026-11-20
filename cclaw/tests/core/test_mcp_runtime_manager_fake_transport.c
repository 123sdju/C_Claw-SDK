#define _POSIX_C_SOURCE 200809L
#include "cc/app/cc_mcp_runtime_manager.h"
#include "cc/internal/cc_alloc.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_config.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#endif

/** @brief 由 manager 拥有的本地传输，记录握手、通知、发现、调用与重置次数。 */
typedef struct fake_transport {
    int initialize_count;
    int notify_count;
    int list_count;
    int call_count;
    int reset_count;
} fake_transport_t;

/** @brief 消费拥有结果并提取成功状态。
 * @param[in] result 按值交付的结果，消息拥有权随本次调用消费。
 * @return CC_OK 为一，其他为零。
 */
static int ok(cc_result_t result)
{
    int success = result.code == CC_OK;
    cc_result_free(&result);
    return success;
}

/**
 * @brief 为本地请求构造带结果的完整响应。
 * @param[in] id 借用请求中的精确小整数标识。
 * @param[in,out] result 消费拥有结果树，交由响应对象释放。
 * @return 需 free 的拥有正文，分配失败返回 NULL。
 */
static char *response_with_result(double id, cc_json_value_t *result)
{
    cc_json_value_t *root = cc_json_create_object();
    if (!root) {
        cc_json_destroy(result);
        return NULL;
    }
    cc_json_object_set(root, "jsonrpc", cc_json_create_string("2.0"));
    cc_json_object_set(root, "id", cc_json_create_number(id));
    cc_json_object_set(root, "result", result);
    char *json = cc_json_stringify_unformatted(root);
    cc_json_destroy(root);
    return json;
}

/** @brief 构造单个 echo 工具描述及对象参数 schema。
 * @return 拥有结果树，交 response_with_result 消费。
 */
static cc_json_value_t *tool_list_result(void)
{
    cc_json_value_t *result = cc_json_create_object();
    cc_json_value_t *tools = cc_json_create_array();
    cc_json_value_t *tool = cc_json_create_object();
    cc_json_object_set(tool, "name", cc_json_create_string("echo"));
    cc_json_object_set(tool, "description", cc_json_create_string("Echo through fake MCP"));
    cc_json_value_t *schema = cc_json_create_object();
    cc_json_object_set(schema, "type", cc_json_create_string("object"));
    cc_json_object_set(tool, "inputSchema", schema);
    cc_json_array_append(tools, tool);
    cc_json_object_set(result, "tools", tools);
    return result;
}

/**
 * @brief 同步提供三类本地响应，并验证发现工具前已完成 initialized 通知。
 * @param[in,out] self 借用有效 fake 状态，manager 覆盖调用寿命。
 * @param[in] request_json 本次借用的完整请求，解析树在返回前销毁。
 * @param[in] timeout_ms 本次正毫秒预算，响应立即完成。
 * @param[in] cancel_token 可选借用令牌，本场景不在传输内部取消。
 * @param[out] out_response_json 必填空槽，先置 NULL，成功移交需 free 的响应正文。
 * @return 解析或响应构造结果，返回消息由调用方释放。
 */
static cc_result_t fake_send_json(
    void *self,
    const char *request_json,
    int timeout_ms,
    cc_cancel_token_t *cancel_token,
    char **out_response_json
)
{
    (void)timeout_ms;
    (void)cancel_token;
    fake_transport_t *transport = (fake_transport_t *)self;
    *out_response_json = NULL;

    cc_json_value_t *request = NULL;
    cc_result_t rc = cc_json_parse(request_json, &request);
    if (rc.code != CC_OK) return rc;
    double id = cc_json_number_value(cc_json_object_get(request, "id"));
    const char *method = cc_json_string_value(cc_json_object_get(request, "method"));

    if (strcmp(method, "initialize") == 0) {
        transport->initialize_count++;
        cc_json_value_t *result = cc_json_create_object();
        cc_json_object_set(result, "protocolVersion", cc_json_create_string("2024-11-05"));
        cc_json_value_t *capabilities = cc_json_create_object();
        cc_json_object_set(capabilities, "tools", cc_json_create_object());
        cc_json_object_set(result, "capabilities", capabilities);
        cc_json_value_t *info = cc_json_create_object();
        cc_json_object_set(info, "name", cc_json_create_string("fake"));
        cc_json_object_set(info, "version", cc_json_create_string("1"));
        cc_json_object_set(result, "serverInfo", info);
        *out_response_json = response_with_result(id, result);
    } else if (strcmp(method, "tools/list") == 0) {
        if (transport->notify_count != transport->initialize_count) {
            cc_json_destroy(request);
            return cc_result_error(CC_ERR_INVALID_STATE, "Discovery preceded initialized notification");
        }
        transport->list_count++;
        *out_response_json = response_with_result(id, tool_list_result());
    } else if (strcmp(method, "tools/call") == 0) {
        transport->call_count++;
        cc_json_value_t *result = cc_json_create_object();
        cc_json_value_t *content = cc_json_create_array();
        cc_json_value_t *item = cc_json_create_object();
        cc_json_object_set(item, "type", cc_json_create_string("text"));
        cc_json_object_set(item, "text", cc_json_create_string("fake-ok"));
        cc_json_array_append(content, item);
        cc_json_object_set(result, "content", content);
        *out_response_json = response_with_result(id, result);
    }

    cc_json_destroy(request);
    if (!*out_response_json) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Fake MCP transport failed to build response");
    }
    return cc_result_ok();
}

/**
 * @brief 接收无 id 的 initialized 通知并记录握手阶段。
 * @param[in,out] self 借用有效传输状态。
 * @param[in] notification_json 同步借用完整通知，解析后销毁树。
 * @param[in] timeout_ms 正毫秒剩余预算，本 fake 不等待。
 * @param[in] cancel_token 可选借用令牌，本 fake 不保留。
 * @return 通知形状及顺序正确返回 CC_OK，否则返回实际解析或状态错误。
 */
static cc_result_t fake_notify(void *self, const char *notification_json, int timeout_ms,
                               cc_cancel_token_t *cancel_token)
{
    (void)timeout_ms;
    (void)cancel_token;
    fake_transport_t *transport = self;
    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse_strict(notification_json, &root);
    if (rc.code != CC_OK) return rc;
    const char *method = cc_json_string_value(cc_json_object_get(root, "method"));
    int valid = method && strcmp(method, "notifications/initialized") == 0 &&
        !cc_json_object_get(root, "id") && transport->notify_count + 1 == transport->initialize_count;
    cc_json_destroy(root);
    if (!valid) return cc_result_error(CC_ERR_INVALID_STATE, "Invalid initialized notification");
    transport->notify_count++;
    return cc_result_ok();
}

/** @brief 记录连接重置，已有测试状态和计数继续保留。
 * @param[in,out] self 有效传输状态。
 * @return 始终 CC_OK。
 */
static cc_result_t fake_reset(void *self)
{
    fake_transport_t *transport = (fake_transport_t *)self;
    transport->reset_count++;
    return cc_result_ok();
}

/** @brief 声明传输支持并行请求。
 * @param[in] self 本例忽略的借用状态。
 * @return 零。
 */
static int fake_is_serial(void *self)
{
    (void)self;
    return 0;
}

/** @brief 释放 manager 独占的 fake 状态。
 * @param[in,out] self 消费拥有实例。
 * @post 实例失效，测试不再读取其计数。
 */
static void fake_destroy(void *self)
{
    free(self);
}

/* 由 manager 独占的测试传输回调表，包含握手后的初始化通知入口。 */
static const cc_mcp_transport_vtable_t fake_vtable = {
    fake_send_json,
    fake_reset,
    fake_is_serial,
    fake_destroy,
    fake_notify
};

#ifdef CC_MCP_TEST_WRAP_CLOCK
static uint64_t fake_clock_ms = 100;
/** @brief 为本目标提供可控单调时钟。
 * @return 当前测试毫秒值，由主测试顺序推进。
 */
uint64_t __wrap_cc_platform_monotonic_ms(void) { return fake_clock_ms; }
#endif

/** @brief 推进超过一毫秒空闲期限，原生无包装平台短睡眠。
 * @post 下一请求观察到空闲到期。
 */
static void wait_for_idle_expiry(void)
{
#ifdef CC_MCP_TEST_WRAP_CLOCK
    fake_clock_ms += 5;
#elif defined(_WIN32)
    Sleep(5);
#else
    const struct timespec delay = {0, 5000000L};
    nanosleep(&delay, NULL);
#endif
}

/** @brief 创建 manager 拥有的本地传输并保留测试借用指针。
 * @param[in] server_config 本 fixture 忽略的配置借用。
 * @param[out] out_transport 必填无已有资源槽，成功交付实例与静态虚表。
 * @param[out] user_data 有效 fake_transport_t 指针输出槽，借用实例至 manager 销毁。
 * @return CC_OK 或 OUT_OF_MEMORY。
 */
static cc_result_t fake_factory(
    const cc_config_mcp_server_t *server_config,
    cc_mcp_transport_t *out_transport,
    void *user_data
)
{
    (void)server_config;
    fake_transport_t **out_seen = (fake_transport_t **)user_data;
    fake_transport_t *transport = calloc(1, sizeof(*transport));
    if (!transport) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "fake transport alloc failed");
    *out_seen = transport;
    out_transport->self = transport;
    out_transport->vtable = &fake_vtable;
    return cc_result_ok();
}

/** @brief 固定返回工厂错误以验证单 server 诊断。
 * @param[in] server_config 忽略的借用配置。
 * @param[out] out_transport 未写入的输出槽。
 * @param[in] user_data 忽略的可选上下文。
 * @return 拥有 factory boom 消息的 PLATFORM 错误，由调用者释放。
 */
static cc_result_t failing_factory(
    const cc_config_mcp_server_t *server_config,
    cc_mcp_transport_t *out_transport,
    void *user_data
)
{
    (void)server_config;
    (void)out_transport;
    (void)user_data;
    return cc_result_error(CC_ERR_PLATFORM, "factory boom");
}

/**
 * @brief 核对初始化通知发现顺序、调用、前置取消、空闲重连与单 server 工厂诊断。
 * @return 全部检查通过为零，失败为一。
 * @note 先销毁 registry 工具，再销毁 manager 传输，最后销毁配置；不使用真实网络。
 */
int main(void)
{
    cc_config_t config;
    if (!ok(cc_config_load_default(&config))) return 1;
    config.mcp.enabled = 1;
    config.mcp.session_idle_ttl_ms = 1;
    config.mcp.server_count = 1;
    config.mcp.servers = calloc(1, sizeof(cc_config_mcp_server_t));
    if (!config.mcp.servers) return 1;
    config.mcp.servers[0].name = cc_copy_string("fake");
    config.mcp.servers[0].transport = cc_copy_string("fake");

    cc_tool_registry_t *registry = NULL;
    cc_mcp_runtime_manager_t *manager = NULL;
    fake_transport_t *transport = NULL;
    cc_runtime_diagnostics_t diagnostics;
    cc_runtime_diagnostics_reset(&diagnostics);
    int passed = ok(cc_tool_registry_create(&registry)) &&
        ok(cc_mcp_runtime_manager_create(fake_factory, &transport, &manager)) &&
        ok(cc_mcp_runtime_manager_load_tools(manager, &config, registry, &diagnostics)) &&
        transport && transport->initialize_count == 1 && transport->notify_count == 1 && transport->list_count == 1;

    cc_tool_t tool;
    if (passed) passed = ok(cc_tool_registry_find(registry, "mcp.fake.echo", &tool));
    if (passed) {
        cc_tool_result_t result;
        cc_tool_context_t ctx;
        memset(&result, 0, sizeof(result));
        memset(&ctx, 0, sizeof(ctx));
        passed = ok(tool.vtable->call(tool.self, "{\"text\":\"hello\"}", &ctx, &result)) &&
            result.ok &&
            result.text &&
            strstr(result.text, "fake-ok") != NULL &&
            transport->call_count == 1;
        cc_tool_result_cleanup(&result);
    }

    if (passed) {
        cc_cancel_source_t *source = NULL;
        cc_tool_result_t result;
        cc_tool_context_t ctx;
        memset(&result, 0, sizeof(result));
        memset(&ctx, 0, sizeof(ctx));
        passed = ok(cc_cancel_source_create(&source));
        if (passed) {
            int calls_before_cancel = transport->call_count;
            cc_cancel_source_cancel(source);
            ctx.cancel_token = cc_cancel_source_token(source);
            passed = ok(tool.vtable->call(tool.self, "{\"text\":\"cancel\"}", &ctx, &result)) &&
                !result.ok &&
                result.error &&
                strstr(result.error, "cancel") != NULL &&
                transport->call_count == calls_before_cancel;
        }
        cc_tool_result_cleanup(&result);
        cc_cancel_source_destroy(source);
    }

    if (passed) {
        cc_tool_result_t result;
        cc_tool_context_t ctx;
        memset(&result, 0, sizeof(result));
        memset(&ctx, 0, sizeof(ctx));
        int resets_before = transport->reset_count;
        int initializes_before = transport->initialize_count;
        int notifications_before = transport->notify_count;
        wait_for_idle_expiry();
        passed = ok(tool.vtable->call(tool.self, "{\"text\":\"after-ttl\"}", &ctx, &result)) &&
            result.ok &&
            transport->reset_count == resets_before + 1 &&
            transport->initialize_count == initializes_before + 1 &&
            transport->notify_count == notifications_before + 1 &&
            transport->call_count == 2;
        cc_tool_result_cleanup(&result);
    }

    if (passed) {
        cc_tool_registry_t *diag_registry = NULL;
        cc_mcp_runtime_manager_t *diag_manager = NULL;
        cc_runtime_diagnostics_t diag;
        cc_runtime_diagnostics_reset(&diag);
        passed = ok(cc_tool_registry_create(&diag_registry)) &&
            ok(cc_mcp_runtime_manager_create(failing_factory, NULL, &diag_manager)) &&
            ok(cc_mcp_runtime_manager_load_tools(diag_manager, &config, diag_registry, &diag)) &&
            diag.count == 1 &&
            strcmp(diag.items[0].kind, "mcp") == 0 &&
            strstr(diag.items[0].message, "factory boom") != NULL;
        cc_tool_registry_destroy(diag_registry);
        cc_mcp_runtime_manager_destroy(diag_manager);
    }

    cc_tool_registry_destroy(registry);
    cc_mcp_runtime_manager_destroy(manager);
    cc_config_destroy(&config);
    return passed ? 0 : 1;
}
