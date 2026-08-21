



#include "cc/app/cc_tool_registry_snapshot.h"
#include "cc/ports/cc_thread.h"

#include <stdlib.h>

/*
 * 工具注册表快照内部状态。
 *
 * generation 标识这批工具集合版本，ref_count 保护 reload 期间旧 run 继续使用旧 registry。
 * owns_registry 决定最后释放快照时是否销毁 registry。
 */
struct cc_tool_registry_snapshot {
    cc_tool_registry_t *registry;
    unsigned long generation;
    int owns_registry;
    size_t ref_count;
    cc_mutex_t mutex;
};

/*
 * 创建工具注册表快照。
 *
 * 初始引用计数为 1；调用方可以把 snapshot 发布给 runtime 读路径。mutex 创建失败会释放
 * 快照，但不会销毁外部传入的 registry。
 */
cc_result_t cc_tool_registry_snapshot_create(
    cc_tool_registry_t *registry,
    unsigned long generation,
    int owns_registry,
    cc_tool_registry_snapshot_t **out_snapshot
)
{
    if (!registry || !out_snapshot) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid tool registry snapshot");
    }
    cc_tool_registry_snapshot_t *snapshot = calloc(1, sizeof(cc_tool_registry_snapshot_t));
    if (!snapshot) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create registry snapshot");
    snapshot->registry = registry;
    snapshot->generation = generation;
    snapshot->owns_registry = owns_registry ? 1 : 0;
    snapshot->ref_count = 1;
    cc_result_t rc = cc_mutex_create(&snapshot->mutex);
    if (rc.code != CC_OK) {
        free(snapshot);
        return rc;
    }
    *out_snapshot = snapshot;
    return cc_result_ok();
}

/* 增加快照引用计数，保证调用方使用期间 registry 不会被 reload 释放。 */
cc_tool_registry_snapshot_t *cc_tool_registry_snapshot_acquire(
    cc_tool_registry_snapshot_t *snapshot
)
{
    if (!snapshot) return NULL;

    cc_mutex_lock(snapshot->mutex);
    snapshot->ref_count++;
    cc_mutex_unlock(snapshot->mutex);
    return snapshot;
}

/*
 * 释放快照引用。
 *
 * 最后一个引用负责可选销毁 registry、销毁 mutex 并释放 snapshot。引用计数操作在锁内
 * 完成，销毁动作在锁外完成，避免销毁 mutex 时仍持锁。
 */
void cc_tool_registry_snapshot_release(cc_tool_registry_snapshot_t *snapshot)
{
    if (!snapshot) return;
    int destroy = 0;
    cc_mutex_lock(snapshot->mutex);
    if (snapshot->ref_count > 0) snapshot->ref_count--;
    destroy = snapshot->ref_count == 0;
    cc_mutex_unlock(snapshot->mutex);
    if (!destroy) return;

    if (snapshot->owns_registry) {
        cc_tool_registry_destroy(snapshot->registry);
    }
    cc_mutex_destroy(snapshot->mutex);
    free(snapshot);
}

/* 返回快照中的 registry 借用指针。 */
cc_tool_registry_t *cc_tool_registry_snapshot_registry(
    cc_tool_registry_snapshot_t *snapshot
)
{
    return snapshot ? snapshot->registry : NULL;
}

/* 返回快照 generation，用于日志和 reload 一致性检查。 */
unsigned long cc_tool_registry_snapshot_generation(
    cc_tool_registry_snapshot_t *snapshot
)
{
    return snapshot ? snapshot->generation : 0;
}
