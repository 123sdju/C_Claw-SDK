

#ifndef CC_TOOL_REGISTRY_SNAPSHOT_H
#define CC_TOOL_REGISTRY_SNAPSHOT_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 工具注册表快照。
 *
 * snapshot 用引用计数保护 registry generation，使 runtime reload 时旧 run 仍能继续使用
 * 旧工具集合，新 run 使用新 generation。它是无锁热更新常见的“不可变快照 + 引用计数”
 * 模式。
 */
typedef struct cc_tool_registry_snapshot cc_tool_registry_snapshot_t;

/*
 * 创建 registry 快照。
 *
 * owns_registry 为非 0 时，snapshot 最后一次 release 会销毁 registry；否则只借用 registry。
 * generation 用于观测和执行池判断当前工具集合版本。
 */
cc_result_t cc_tool_registry_snapshot_create(
    cc_tool_registry_t *registry,
    unsigned long generation,
    int owns_registry,
    cc_tool_registry_snapshot_t **out_snapshot
);

/* 增加引用计数并返回同一 snapshot；NULL 输入返回 NULL。 */
cc_tool_registry_snapshot_t *cc_tool_registry_snapshot_acquire(
    cc_tool_registry_snapshot_t *snapshot
);

/* 释放引用；最后一个引用负责销毁 snapshot 和可选 registry。 */
void cc_tool_registry_snapshot_release(cc_tool_registry_snapshot_t *snapshot);

/* 取得快照内 registry；调用方不能越过 snapshot 生命周期保存它。 */
cc_tool_registry_t *cc_tool_registry_snapshot_registry(
    cc_tool_registry_snapshot_t *snapshot
);

/* 返回快照 generation；NULL snapshot 返回 0。 */
unsigned long cc_tool_registry_snapshot_generation(
    cc_tool_registry_snapshot_t *snapshot
);

#ifdef __cplusplus
}
#endif

#endif
