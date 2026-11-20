



#ifndef CC_CONFIG_H
#define CC_CONFIG_H

#include "cc/core/cc_result.h"
#include <stddef.h>

/** @brief 配置拥有的字符串列表，数组与字符串由 cc_config_destroy 统一释放。 */
typedef struct cc_config_string_list {
    char **items; /**< 拥有的字符串指针数组，空列表可为 NULL。 */
    size_t count; /**< 已初始化且可独立释放的字符串槽数。 */
} cc_config_string_list_t;

/** @brief 配置拥有的 agent 覆盖项，builder 复制需要长期保存的字段。 */
typedef struct cc_config_agent_profile {
    char *id; /**< 拥有的 agent 标识，可由 defaults 缺省名提供。 */
    char *model; /**< 可选拥有模型覆盖名。 */
    char *workspace; /**< 可选拥有工作目录覆盖路径。 */
    char *agent_dir; /**< 可选拥有 agent 资源目录。 */
    char *system_prompt_file; /**< 可选拥有提示词文件路径。 */
    cc_config_string_list_t skills; /**< 拥有的技能选择列表。 */
} cc_config_agent_profile_t;

/** @brief agent 缺省项及命名 profile 集合。 */
typedef struct cc_config_agents {
    cc_config_agent_profile_t defaults; /**< 拥有的默认 profile 字段。 */
    cc_config_agent_profile_t *profiles; /**< 拥有的覆盖项数组。 */
    size_t profile_count; /**< 已登记并可清理的 profile 槽数。 */
} cc_config_agents_t;

/** @brief 队列 lane、会话并发与等待预算，具体调度语义由队列装配解释。 */
typedef struct cc_config_queue {
    int main_concurrency; /**< 主 lane 并发，validate 要求正数。 */
    int subagent_concurrency; /**< 子 agent lane 并发，validate 要求正数。 */
    int plugin_concurrency; /**< 插件 lane 并发，validate 要求正数。 */
    int mcp_concurrency; /**< MCP lane 并发，validate 要求正数。 */
    int per_session_concurrency; /**< 同会话并发配置，归一规则沿队列。 */
    char *mode; /**< 拥有的调度模式名称。 */
    int debounce_ms; /**< 去抖毫秒，validate 要求非负。 */
    int max_pending_per_session; /**< 每会话等待预算，validate 要求非负。 */
    size_t max_total_tasks; /**< 总任务条数预算，零值沿队列缺省规则。 */
    unsigned completed_ttl_ms; /**< 完成项保留毫秒，零值沿队列规则。 */
} cc_config_queue_t;

/** @brief 按完整 lane 匹配的并发与超时覆盖，标量零值由装配或执行器解释。 */
typedef struct cc_config_tool_policy {
    char *name; /**< 拥有的完整 lane 名称，例如 tool.file_read，当前按显式 lane 匹配，validate 要求非空。 */
    int concurrency; /**< 并发配置，validate 要求非负。 */
    int timeout_ms; /**< 超时毫秒，validate 要求非负。 */
} cc_config_tool_policy_t;

/** @brief 配置拥有的工具启用、网络允许列表及逐工具策略。 */
typedef struct cc_config_tools {
    cc_config_string_list_t enabled; /**< 拥有的启用名称列表，另复制旧 enabled_tools 缓存。 */
    cc_config_string_list_t network_allowlist; /**< 拥有的网络允许项，匹配规则由工具适配器解释。 */
    int default_timeout_ms; /**< 默认工具超时毫秒，validate 要求非负。 */
    cc_config_tool_policy_t *policies; /**< 拥有的逐工具策略数组。 */
    size_t policy_count; /**< 已登记策略槽数。 */
} cc_config_tools_t;

/** @brief 配置拥有的插件工具声明，参数节点序列化后与原 JSON 树独立。 */
typedef struct cc_config_plugin_tool {
    char *name; /**< 拥有的工具名称。 */
    char *description; /**< 拥有的工具说明。 */
    char *parameters_json; /**< 拥有的参数 schema JSON 正文，缺失时采用空对象 schema。 */
} cc_config_plugin_tool_t;

/** @brief 配置拥有的插件进程、并发与工具声明，实际装载能力沿 feature 集合。 */
typedef struct cc_config_plugin_entry {
    char *id; /**< 拥有的 entries 对象键，validate 要求非空。 */
    int enabled; /**< 是否启用，启用项须有 command。 */
    char *command; /**< 可选拥有进程命令。 */
    char **args; /**< 拥有的字符串参数数组，按 arg_count 遍历。 */
    size_t arg_count; /**< 有效参数数，数组不承诺额外 NULL 哨兵。 */
    int workers; /**< 工作进程配置，validate 要求正数。 */
    int timeout_ms; /**< 调用超时毫秒，validate 要求非负。 */
    int max_in_flight; /**< 最大在途调用数，validate 要求正数。 */
    int restart_on_crash; /**< 是否允许异常退出后重启。 */
    cc_config_plugin_tool_t *tools; /**< 拥有的工具声明数组。 */
    size_t tool_count; /**< 已登记且可清理的声明槽数。 */
    cc_config_string_list_t skill_dirs; /**< 拥有的附带技能目录。 */
} cc_config_plugin_entry_t;

/** @brief 插件集合及交给上层解释的热重载设置。 */
typedef struct cc_config_plugins {
    int hot_reload; /**< 热重载开关。 */
    int reload_debounce_ms; /**< 热重载去抖毫秒。 */
    cc_config_plugin_entry_t *entries; /**< 拥有的插件数组。 */
    size_t entry_count; /**< 已登记且可清理的插件槽数。 */
} cc_config_plugins_t;

/** @brief 技能目录加载和上层监听配置。 */
typedef struct cc_config_skills {
    int watch; /**< 交给上层的目录监听开关。 */
    int watch_debounce_ms; /**< 监听去抖毫秒，validate 要求非负。 */
    cc_config_string_list_t extra_dirs; /**< 拥有的额外技能目录，由 catalog 读取。 */
} cc_config_skills_t;

/** @brief 配置拥有的 MCP server 描述，传输实际支持范围沿适配器。 */
typedef struct cc_config_mcp_server {
    char *name; /**< 拥有的 servers 对象键。 */
    char *command; /**< stdio 需要的拥有进程命令。 */
    char **args; /**< 拥有的参数数组，无额外终止槽承诺。 */
    size_t arg_count; /**< 有效参数数。 */
    char *cwd; /**< 可选拥有工作目录。 */
    char *url; /**< 网络传输需要的拥有地址，校验仅检查非空。 */
    char *transport; /**< 拥有的传输名，NULL 按 stdio。 */
    int connection_timeout_ms; /**< 连接超时毫秒，validate 要求非负。 */
} cc_config_mcp_server_t;

/** @brief MCP 启用、空闲期限及 server 集合。 */
typedef struct cc_config_mcp {
    int enabled; /**< 是否加载 MCP。 */
    int session_idle_ttl_ms; /**< 会话空闲期限毫秒，validate 要求非负。 */
    cc_config_mcp_server_t *servers; /**< 拥有的 server 数组。 */
    size_t server_count; /**< 已登记且可清理的 server 槽数。 */
} cc_config_mcp_t;

/** @brief 多模态输入或输出能力标志，validate 核对编译能力。 */
typedef struct cc_config_modality_flags {
    int image; /**< 图像开关。 */
    int audio; /**< 音频开关。 */
    int video; /**< 视频开关。 */
    int file; /**< 文件开关。 */
} cc_config_modality_flags_t;

/** @brief 转为媒体端口限制的预算，零值具体含义沿使用方。 */
typedef struct cc_config_multimodal_limits {
    size_t max_artifacts; /**< 产物个数预算。 */
    size_t max_artifact_bytes; /**< 单产物字节预算。 */
    size_t max_base64_bytes; /**< 内联 base64 字节预算。 */
    int allow_inline_base64; /**< 是否允许内联 base64，validate 核对编译支持。 */
    cc_config_string_list_t allowed_mime_prefixes; /**< 拥有的 MIME 前缀列表。 */
} cc_config_multimodal_limits_t;

/** @brief 输入、输出和媒体预算配置。 */
typedef struct cc_multimodal_config {
    cc_config_modality_flags_t input; /**< 输入能力标志。 */
    cc_config_modality_flags_t output; /**< 输出能力标志。 */
    cc_config_multimodal_limits_t limits; /**< 媒体预算与拥有的 MIME 列表。 */
} cc_multimodal_config_t;

/**
 * @brief SDK 主配置及其全部拥有的字符串、数组与嵌套字段。
 * 该结构由 cc_config_load* 填充，所有字符串、数组和嵌套对象由 config 拥有，并通过
 * cc_config_destroy() 统一释放。runtime builder 读取它来创建 provider、storage、
 * tools、queue、plugin、MCP、multimodal 和 active memory。产品 I/O、网络、Web、
 * 显示、OTA 等配置属于应用层，不进入 SDK runtime config。
 * load 失败仍可能保留拥有的默认或部分配置，接收者须 destroy。读者使用期由外层协调，
 * load 和 destroy 不提供并发锁。手工构造时各字符串及数组须独立拥有且计数一致。
 */
typedef struct cc_config {
    char *provider; /**< 拥有的 provider 名称，默认按编译选择。 */

    char *model; /**< 拥有的模型名称。 */

    char *model_extra_body_json; /**< 可选拥有的 provider 扩展对象 JSON。 */

    char *base_url; /**< 拥有的 provider 基础 URL。 */

    char *api_key; /**< 可选拥有的密钥正文，环境覆盖后可再由引用解析替换。 */

    char *api_key_ref; /**< 可选拥有的密钥存储引用。 */

    int tls_verify; /**< TLS 证书校验开关，缺省启用。 */

    int http_connect_timeout_ms; /**< HTTP 建连超时毫秒。 */

    int http_first_byte_timeout_ms; /**< HTTP 首字节超时毫秒。 */

    int http_stream_idle_timeout_ms; /**< HTTP 流空闲超时毫秒。 */

    int http_retry_count; /**< HTTP 重试次数配置，解释沿客户端。 */

    int http_log_level; /**< HTTP 日志级别，名称映射 trace=0 到 fatal=5，整数值沿客户端。 */

    int http_trace_persist; /**< HTTP trace 持久化开关。 */

    char *storage_type; /**< 拥有的会话后端名，具体可用性沿 feature 工厂。 */

    char *data_dir; /**< 拥有的数据根目录。 */

    char *storage_path; /**< 拥有的会话文件或目录路径。 */

    size_t session_segment_bytes; /**< 会话分段字节预算，validate 要求正数。 */

    char *session_media_dir; /**< 拥有的媒体目录配置，validate 要求非空。 */

    char *workspace_path; /**< 拥有的默认工作区路径。 */

    char *sandbox_type; /**< 拥有的沙箱后端名。 */

    int shell_requires_approval; /**< shell 工具是否需要审批，默认启用。 */

    int sandbox_timeout_ms; /**< 沙箱超时毫秒，解释沿后端。 */

    int max_steps; /**< 模型步骤预算，缺省 MCU 为四、其他平台为十，零值沿 runtime。 */

    /* 运行预算由 builder 转交 runtime，零值分别沿使用方的缺省或不限规则。 */
    int run_timeout_ms; /**< 单次运行超时毫秒，validate 要求非负。 */
    int provider_timeout_ms; /**< 每步 provider 超时毫秒，validate 要求非负。 */
    int tool_timeout_ms; /**< 单工具超时毫秒，validate 要求非负。 */
    int max_input_bytes; /**< 原输入字节预算，validate 要求非负。 */
    int max_output_bytes; /**< 最终正文及同步响应字节预算。 */
    int max_stream_bytes; /**< 运行累计片段内容字节预算。 */
    int max_tool_result_bytes; /**< 工具结果字节预算。 */
    int max_concurrency; /**< runtime 运行准入并发配置。 */

    int thinking_mode; /**< 模型思考开关，文件加载完成后非零强制 stream_mode 开启。 */

    int max_tokens; /**< 模型输出 token 配置。 */

    double temperature; /**< 模型采样温度，具体范围沿 provider。 */

    int stream_mode; /**< 配置层流式开关，实际入口和能力由调用方决定。 */

    int debug_mode; /**< 调试开关，cli 配置在 system 配置后覆盖。 */

    char *memory_backend; /**< 拥有的记忆后端名称。 */

    char *memory_path; /**< 拥有的记忆存储路径。 */

    int active_memory_enabled; /**< 可选活动记忆开关，仍受编译能力约束。 */

    int active_memory_write_summary; /**< 完整答复后是否尽力写入活动摘要。 */
    int active_memory_max_value_chars; /**< 活动摘要实际上按字节截断，非正数的使用方缺省为 1600。 */
    char *active_memory_category; /**< 拥有的活动摘要分类名。 */
    char *system_prompt; /**< 可选拥有显式提示词，非空时优先于文件来源。 */

    char *soul_file; /**< 拥有的 soul 提示词来源路径。 */

    char *user_file; /**< 拥有的 user 提示词来源路径。 */

    char **enabled_tools; /**< tools.enabled 的独立拥有兼容副本。 */

    size_t enabled_tools_count; /**< 兼容工具副本中的已复制项数。 */
    char **plugin_commands; /**< 兼容旧调用者的拥有命令数组，当前 JSON loader 未填充。 */

    size_t plugin_count; /**< 兼容命令数组的可释放项数。 */
    int context_window_tokens; /**< 上下文 token 窗口，validate 要求正数。 */

    double context_compress_threshold; /**< 有限压缩比例，文件接受 80 等百分数写法并除以百，validate 检查零到一。 */

    int context_keep_recent; /**< 压缩保留近期消息数，validate 要求非负。 */

    int summary_max_tokens; /**< 辅助摘要输出 token 数，validate 要求正数。 */

    double summary_temperature; /**< 辅助摘要温度，validate 检查有限且非负。 */



    cc_config_agents_t agents; /**< 拥有的 agent 默认和覆盖配置。 */
    cc_config_queue_t queue; /**< 队列预算与拥有的模式名。 */
    cc_config_tools_t tools; /**< 拥有的工具列表与策略。 */
    cc_config_plugins_t plugins; /**< 拥有的插件配置集合。 */
    cc_config_skills_t skills; /**< 技能监听与拥有的目录列表。 */
    cc_config_mcp_t mcp; /**< 拥有的 MCP server 集合。 */
    cc_multimodal_config_t multimodal; /**< 媒体能力、预算和拥有的 MIME 列表。 */
} cc_config_t;

/**
 * @brief 构造默认值后按文件覆盖配置并执行已实现的语义校验。
 * @param[in] path 可选借用路径，NULL 仅构造默认值，非 NULL 使用默认文件系统。
 * @param[out] out_config 必需且无已有资源，成功及部分失败输出均由调用者 destroy。
 * @return 成功为 CC_OK，空输出为 CC_ERR_INVALID_ARGUMENT，读取、JSON、密钥引用、分配及校验错误按实现返回。
 * @note 普通 JSON 入口保留首值后的尾部兼容。文件或解析失败保留已构造默认值，后续失败可保留部分覆盖结果。无内部锁，拥有者协调并发使用。
 */
cc_result_t cc_config_load(const char *path, cc_config_t *out_config);

/**
 * @brief 按编译配置直接构造独立拥有的默认值。
 * @param[out] out_config 必需且无已有资源，进入后清零，成功由调用者 destroy。
 * @return 成功为 CC_OK，NULL 为 CC_ERR_INVALID_ARGUMENT，必需字符串分配失败为 CC_ERR_OUT_OF_MEMORY 并清零输出。
 * @note 本入口不读取默认文件或环境，也不调用完整 validate。
 */
cc_result_t cc_config_load_default(cc_config_t *out_config);

/**
 * @brief 检查已实现的运行、插件、MCP、存储及媒体编译能力约束。
 * @param[in] config 借用有效配置，数组计数与指针一致，校验期间保持稳定。
 * @return 通过为 CC_OK，NULL 或已检查参数非法为 CC_ERR_INVALID_ARGUMENT，媒体能力未编译为 CC_ERR_UNSUPPORTED。
 * @note 不修改配置，未检查的字段沿下游工厂或端口约束，错误消息由调用者释放。
 */
cc_result_t cc_config_validate(const cc_config_t *config);

/**
 * @brief 释放全部拥有字段并清零，支持部分加载结果及重复清理。
 * @pre 所有读者和加载调用已结束，手工配置的字段独立拥有且计数有效。
 * @param[in,out] config 可选配置指针，NULL 跳过。
 * @post 非 NULL 结构归零，旧字段借用失效。
 */
void cc_config_destroy(cc_config_t *config);

/**
 * @brief 复制显式提示词，或拼接 soul/user 文件，空内容回退内置提示词。
 * @param[in] config 必需借用配置，覆盖本次同步读取期。
 * @param[out] out_prompt 必需无已有资源输出，基础参数合法后置 NULL，成功正文由调用者 free。
 * @return 成功为 CC_OK，空参数为 CC_ERR_INVALID_ARGUMENT，复制或 builder 失败返回对应资源错误。
 * @note 文件读取错误允许继续其他来源，拼接失败向上传递，配置字段保持原值。
 */
cc_result_t cc_config_build_system_prompt(const cc_config_t *config, char **out_prompt);

#endif
