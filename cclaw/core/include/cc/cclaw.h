#ifndef CC_CCLAW_H
#define CC_CCLAW_H

/*
 * C-Claw SDK 的聚合 public header。
 *
 * 下游应用优先 include <cc/cclaw.h>，而不是依赖 src/ 或 *_internal.h。该头只聚合
 * 核心 SDK 合约：数据模型、runtime、ports、必要 adapters 和 util，不包含 examples、
 * CLI、Web、UART 或业务 gateway。嵌入式项目可以在 profile 层裁剪具体库目标，但
 * public API 边界以这里列出的头文件为准。
 */

/* 版本、错误模型和核心数据结构。 */
#include "cc/core/cc_version.h"
#include "cc/core/cc_result.h"
#include "cc/core/cc_types.h"
#include "cc/core/cc_media.h"
#include "cc/core/cc_memory_entry.h"
#include "cc/core/cc_message.h"
#include "cc/core/cc_observability.h"
#include "cc/core/cc_session.h"
#include "cc/core/cc_stream_chunk.h"
#include "cc/core/cc_tool_call.h"

/* 应用层 runtime 组件：负责调度、上下文构建、session 和工具执行。 */
#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/app/cc_context_builder.h"
#include "cc/app/cc_memory_context.h"
#include "cc/app/cc_app_features.h"
#include "cc/app/cc_runtime_builder.h"
#include "cc/app/cc_runtime_features.h"
#include "cc/app/cc_session_manager.h"
#include "cc/app/cc_tool_executor.h"

/* 平台与能力 ports：通过 struct + vtable 做依赖注入，便于嵌入式移植。 */
#include "cc/ports/cc_event_bus.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_http_client.h"
#include "cc/ports/cc_llm_provider.h"
#include "cc/ports/cc_logger.h"
#include "cc/ports/cc_memory_store.h"
#include "cc/ports/cc_path.h"
#include "cc/ports/cc_platform.h"
#include "cc/ports/cc_policy_engine.h"
#include "cc/ports/cc_sandbox.h"
#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_tool_registry.h"

/* SDK 自带的可选 adapter 声明；业务 gateway 和产品工具不放在这里。 */
#include "cc/adapters/cc_builtin_tools.h"
#include "cc/adapters/cc_default_policy_engine.h"
#include "cc/adapters/cc_http_llm_provider.h"
#include "cc/adapters/cc_llm_providers.h"

/* 通用 util：JSON、配置、内存、网络策略、脱敏和 token 估算。 */
#include "cc/util/cc_config.h"
#include "cc/util/cc_json.h"
#include "cc/util/cc_network_policy.h"
#include "cc/util/cc_redaction.h"
#include "cc/util/cc_string_builder.h"
#include "cc/util/cc_token_counter.h"

#endif
