

#ifndef CC_RUNTIME_BUILDER_H
#define CC_RUNTIME_BUILDER_H

#include "cc/app/cc_agent_manager.h"
#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_runtime_features.h"
#include "cc/util/cc_config.h"
#include "cc/ports/cc_logger.h"

#include <stdint.h>


/*
 * runtime builder 不透明句柄。
 *
 * builder 负责把 config + feature set 装配成 runtime、agent manager、logger、store、
 * tools 和扩展状态。它是应用启动阶段的组合根，不是业务 gateway。
 */
typedef struct cc_runtime_builder cc_runtime_builder_t;
typedef struct cc_runtime_generation cc_runtime_generation_t;

/*
 * runtime reload 报告。
 *
 * generation 用于说明热重载前后版本；各 *_reloaded 标志描述哪些子系统被替换；
 * rolled_back/failed_component/message 描述失败回滚。diagnostics 保存可继续运行但需要
 * 用户注意的 provider/tool/plugin/MCP 诊断。
 */
typedef struct cc_runtime_reload_report {
    unsigned long old_generation;
    unsigned long new_generation;
    int tools_reloaded;
    int plugins_reloaded;
    int mcp_reloaded;
    int skills_reloaded;
    int tool_pool_reloaded;
    int rolled_back;
    char failed_component[32];
    char message[160];
    cc_runtime_diagnostics_t diagnostics;
} cc_runtime_reload_report_t;

/*
 * 创建 runtime builder。
 *
 * config/features 只在调用期间借用；builder 会创建并持有 runtime 所需组件。失败时不会
 * 返回半初始化 builder，诊断可通过错误结果或后续设计暴露。
 */
cc_result_t cc_runtime_builder_create(
    const cc_config_t *config,
    const cc_runtime_feature_set_t *features,
    cc_runtime_builder_t **out_builder
);

/* 返回 builder 持有的 runtime；指针生命周期不超过 builder。 */
cc_result_t cc_runtime_builder_acquire_generation(
    cc_runtime_builder_t *builder,
    cc_runtime_generation_t **out_generation
);
void cc_runtime_generation_release(cc_runtime_generation_t *generation);
cc_agent_runtime_t *cc_runtime_generation_runtime(cc_runtime_generation_t *generation);
unsigned long cc_runtime_generation_id(const cc_runtime_generation_t *generation);

/* 返回 builder 持有的 agent manager；指针生命周期不超过 builder。 */
cc_agent_manager_t *cc_runtime_builder_agent_manager(cc_runtime_builder_t *builder);

/* 返回最近一次构建/加载诊断集合；调用方不能修改或释放。 */
const cc_runtime_diagnostics_t *cc_runtime_builder_diagnostics(cc_runtime_builder_t *builder);

/* 使用新配置热重载；不需要报告时可调用该简化入口。 */
cc_result_t cc_runtime_builder_reload(
    cc_runtime_builder_t *builder,
    const cc_config_t *config
);

/* 使用新配置热重载并返回详细报告；out_report 由调用方提供。 */
cc_result_t cc_runtime_builder_reload_with_report(
    cc_runtime_builder_t *builder,
    const cc_config_t *config,
    cc_runtime_reload_report_t *out_report
);

/* 请求 builder 进入 shutdown；长任务应通过 cancel/run queue 在边界处停止。 */
void cc_runtime_builder_request_shutdown(cc_runtime_builder_t *builder);

/* 返回 builder 使用的 logger；调用方不能销毁。 */
cc_logger_t *cc_runtime_builder_logger(cc_runtime_builder_t *builder);

/* Reset only this builder's HTTP connection pools. */
cc_result_t cc_runtime_builder_reset_http_connections(cc_runtime_builder_t *builder);

/*
 * 在有限时间内等待 generation 引用归零并销毁 builder。
 * 超时返回 CC_ERR_TIMEOUT，builder 保持已分配且处于 shutdown 状态，调用方可稍后重试销毁。
 */
cc_result_t cc_runtime_builder_destroy(cc_runtime_builder_t *builder, uint32_t timeout_ms);

#endif
