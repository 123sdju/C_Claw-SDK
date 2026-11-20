



#ifndef CC_MEMORY_TOOL_FACTORY_H
#define CC_MEMORY_TOOL_FACTORY_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_memory_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建提供 store、recall、forget 和 list 操作的记忆工具。
 * @param[in] store 必需借用描述符，owner 保持其地址、self 和 vtable 覆盖工具全部调用期。
 * @param[out] out_tool 必需新建或已销毁槽，入口清零，成功取得独占 self 和静态方法表。
 * @return 参数错误或 OOM，成功后由创建方销毁或成功注册后交接 registry，拥有诊断由调用者释放。
 * @note 工具借用存储，实际记录可见性、写入约束和同步由注入后端决定，owner 排空调用后销毁。
 */
cc_result_t cc_memory_tool_create(cc_memory_store_t *store, cc_tool_t *out_tool);

/**
 * @brief 创建只发布并执行 recall/list 的固定只读记忆工具。
 * @param[in] store 必需存储描述符借用，保持地址和后端覆盖工具寿命。
 * @param[out] out_tool 必需新槽，先清零，成功对象由调用者销毁或交接 registry。
 * @return 参数错误、分配失败或成功拥有结果，调用者释放诊断，失败输出为空。
 * @note 调用时再次拒绝 store/forget，受管理记录筛选仍由后端策略负责。
 */
cc_result_t cc_memory_tool_create_readonly(cc_memory_store_t *store, cc_tool_t *out_tool);

/**
 * @brief 按精确后端名创建存储，指定后端失败直接返回。
 * @param[out] out_store 必需新描述符，入口清零，成功 self 由调用者最终 cc_memory_store_destroy。
 * @param[in] backend 必需借用名，支持 inmem、json_file、sqlite、noop 和 none。
 * @param[in] path 调用期可选路径，NULL 使用编译期默认路径，空串交具体后端解释。
 * @return 参数、未启用能力或后端拥有结果，noop/none 返回 PLATFORM，诊断由调用者释放。
 * @note 工厂只选择后端，挂载、业务恢复和成功描述符的并发生命周期由 owner 管理。
 */
cc_result_t cc_memory_store_factory_create(
    cc_memory_store_t *out_store,
    const char *backend,
    const char *path
);

#ifdef __cplusplus
}
#endif

#endif
