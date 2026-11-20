



#include "cc/app/cc_tool_registry_snapshot.h"
#include "cc/ports/cc_thread.h"

#include <stdlib.h>

/** 注册表指针和版本在创建后固定，引用计数锁仅保护引用增减。 */
struct cc_tool_registry_snapshot {
    cc_tool_registry_t *registry; /**< 按 owns_registry 拥有或持续借用的注册表。 */
    unsigned long generation;    /**< 调用者指定的不可变版本值。 */
    int owns_registry;           /**< 规范化的一/零拥有标志。 */
    size_t ref_count;            /**< 受 mutex 保护的有效引用数，创建后为一。 */
    cc_mutex_t mutex;            /**< 对象拥有的引用计数互斥锁。 */
};

/**
 * @brief 分配快照、固定注册表身份与版本，并创建保护初始单引用的 mutex。
 * @param[in,out] registry 必填注册表，成功且 owns_registry 非零时转交拥有，失败保持外层拥有关系。
 * @param[in] generation 原样保存的版本标识，允许零。
 * @param[in] owns_registry 非零规范化为一，决定最后 release 是否销毁 registry。
 * @param[out] out_snapshot 必填，仅成功时填写初始引用，失败保持原值，须无待释放旧引用。
 * @return 成功为 CC_OK，空参数为 CC_ERR_INVALID_ARGUMENT，分配失败为 CC_ERR_OUT_OF_MEMORY，mutex 创建错误原样返回并释放快照外壳。
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

/**
 * @brief 锁内递增 ref_count，交付需要配对释放的新引用。
 * @pre 外层有效引用或发布锁保证 snapshot 在加锁前已存活，计数加一在 size_t 范围内。
 * @param[in,out] snapshot 存活的快照，允许 NULL。
 * @return 返回相同快照地址，NULL 输入返回 NULL，外层协调 acquire 与最后释放之间的发布顺序。
 */
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

/**
 * @brief 锁内归还引用并判断归零，锁外销毁拥有的 registry、mutex 和快照外壳。
 * @pre 调用者归还一份有效引用，其 registry/工具使用已退出，外层排除无引用访问及重复释放。
 * @param[in,out] snapshot 待消费的引用，允许 NULL。
 * @post 本次引用消费完毕；归零时借用指针失效，注册表销毁可能调用工具 destroy，执行时已释放计数锁。
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

/**
 * @brief 直接读取创建后固定的 registry 指针。
 * @param[in] snapshot 调用者持有有效引用的快照，允许 NULL。
 * @return registry 借用指针，须在当前引用归还前结束使用，NULL 输入返回 NULL。
 */
cc_tool_registry_t *cc_tool_registry_snapshot_registry(
    cc_tool_registry_snapshot_t *snapshot
)
{
    return snapshot ? snapshot->registry : NULL;
}

/**
 * @brief 直接读取创建后固定的 generation 值。
 * @param[in] snapshot 调用者持有有效引用的快照，允许 NULL。
 * @return 保存的版本值，NULL 输入返回零，查询无需修改快照状态。
 */
unsigned long cc_tool_registry_snapshot_generation(
    cc_tool_registry_snapshot_t *snapshot
)
{
    return snapshot ? snapshot->generation : 0;
}
