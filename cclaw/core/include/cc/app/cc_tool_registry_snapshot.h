

#ifndef CC_TOOL_REGISTRY_SNAPSHOT_H
#define CC_TOOL_REGISTRY_SNAPSHOT_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 带版本标识和引用计数的工具注册表包装。
 *
 * 对象创建后 registry 指针、generation 和拥有标志保持稳定，引用计数由内部 mutex 保护。
 * 每个使用者持有一份引用，registry 借用及工具调用须在该引用的有效期内结束。
 * 发布、替换快照及从共享位置取得首份引用由外层同步，最后 release 前须排除新的无引用访问。
 * 注册表集合的冻结及业务修改遵守 registry 本身的接口，创建快照仅保存其指针。
 * 当前 core 构建提供这一接口，实际运行时的 generation 装配与寿命由各调用链单独管理。
 */
typedef struct cc_tool_registry_snapshot cc_tool_registry_snapshot_t;

/**
 * @brief 创建初始引用数为一的注册表快照。
 * @param[in,out] registry 必填的有效注册表，owns_registry 非零且创建成功时转交快照拥有，失败仍归调用者。
 * @param[in] generation 调用方指定的版本标识，原样保存，允许零。
 * @param[in] owns_registry 非零时最后 release 销毁注册表，为零时外层保证注册表覆盖全部快照引用寿命。
 * @param[out] out_snapshot 必填，成功接收拥有初始引用的快照，失败保持原槽值，原槽须无待释放引用。
 * @return 成功为 CC_OK，空注册表/输出为 CC_ERR_INVALID_ARGUMENT，分配失败为 CC_ERR_OUT_OF_MEMORY，mutex 创建错误原样返回。
 */
cc_result_t cc_tool_registry_snapshot_create(
    cc_tool_registry_t *registry,
    unsigned long generation,
    int owns_registry,
    cc_tool_registry_snapshot_t **out_snapshot
);

/**
 * @brief 在计数锁内增加一份快照引用并返回同一对象。
 * @pre 调用者已持有效引用，或通过外层发布锁保证对象在本次 acquire 结束前存活。
 * @param[in,out] snapshot 借用的存活快照，允许 NULL，计数增长须在 size_t 表示范围内。
 * @return 非空输入返回同一地址，调用者负责配对一次 release，NULL 输入返回 NULL。
 */
cc_tool_registry_snapshot_t *cc_tool_registry_snapshot_acquire(
    cc_tool_registry_snapshot_t *snapshot
);

/**
 * @brief 归还一份引用，最后一次归还在计数锁外销毁拥有的注册表及快照。
 * @param[in,out] snapshot 调用者持有的一份引用，允许 NULL，每份引用配对释放一次。
 * @pre 此引用覆盖的 registry 和工具借用已退出，最后释放时外层已排除未持引用的访问者。
 * @post 当前引用已消费，其他有效引用可继续使用；最后释放使快照及其拥有的注册表失效。
 */
void cc_tool_registry_snapshot_release(cc_tool_registry_snapshot_t *snapshot);

/**
 * @brief 读取快照中固定的注册表指针。
 * @param[in] snapshot 调用者持有有效引用的快照，允许 NULL。
 * @return 注册表借用指针，借用至当前引用 release 前结束，NULL 快照返回 NULL。
 */
cc_tool_registry_t *cc_tool_registry_snapshot_registry(
    cc_tool_registry_snapshot_t *snapshot
);

/**
 * @brief 读取创建时保存的版本标识。
 * @param[in] snapshot 调用者持有有效引用的快照，允许 NULL。
 * @return 创建时的 generation，NULL 快照返回零，零也可为有效实例的版本值。
 */
unsigned long cc_tool_registry_snapshot_generation(
    cc_tool_registry_snapshot_t *snapshot
);

#ifdef __cplusplus
}
#endif

#endif
