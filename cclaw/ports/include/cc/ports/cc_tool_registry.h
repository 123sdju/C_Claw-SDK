



#ifndef CC_TOOL_REGISTRY_H
#define CC_TOOL_REGISTRY_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_tool.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 拥有成功注册的工具状态、至多六十四项的互斥注册表。
 * @note 名称、描述、schema 和 destroy 回调在表锁内执行，须同步返回且不得重入本表。
 * find 得到浅借用，owner 关闭入口并排空 API 和工具调用后才 destroy。
 */
typedef struct cc_tool_registry cc_tool_registry_t;

/** 可信注册方声明该实例的每次调用无外部副作用，未标记工具按可能有副作用处理。 */
#define CC_TOOL_CAPABILITY_READ_ONLY 1U

/**
 * @brief 创建带 mutex 的空注册表。
 * @param[out] out_registry 必填空槽，先置 NULL，成功取得须 destroy 的实例。
 * @return 参数、分配或锁创建结果，失败无新实例，拥有诊断由调用者释放。
 */
cc_result_t cc_tool_registry_create(cc_tool_registry_t **out_registry);

/**
 * @brief 消费注册表和其中非空且提供 destroy 的工具状态。
 * @param[in,out] registry 独占实例，可为 NULL。
 * @pre owner 已关闭入口并排空 API、find 浅借用和实际工具调用，工具 destroy 不得重入本表。
 * @post 实例、锁和工具资源释放，旧句柄及别名失效，外部借用服务仍归其 owner。
 */
void cc_tool_registry_destroy(cc_tool_registry_t *registry);

/**
 * @brief 以零能力标记注册一个工具，成功才交接 self。
 * @param[in,out] registry 必填存活注册表。
 * @param[in] tool 拥有描述符按值传入，vtable 及元数据文本在实例寿命内稳定。
 * @return NULL、冻结、同非空 self 或同名为 INVALID_ARGUMENT，满表为 OOM，成功为 CC_OK。
 * @note 失败不改变原拥有关系，不同新实例由 caller 释放，重复 self 的原实例仍归已有注册表。
 * 成功后 caller 仅保留浅别名，名称按字节比较，诊断交调用者。
 */
cc_result_t cc_tool_registry_add(
    cc_tool_registry_t *registry,
    cc_tool_t tool
);

/**
 * @brief 按同一准入和拥有规则注册工具及可信能力。
 * @param[in,out] registry 必填存活注册表。
 * @param[in] tool 仅成功时交接状态，失败集合及拥有权保持。
 * @param[in] capabilities 允许 READ_ONLY 或零，其他位拒绝。
 * @return add 的结果类别，未知能力为 INVALID_ARGUMENT，诊断由调用者释放。
 */
cc_result_t cc_tool_registry_add_with_capabilities(
    cc_tool_registry_t *registry, cc_tool_t tool, unsigned capabilities);

/** @brief 永久关闭注册准入，可重复冻结。 @param[in,out] registry 必填存活实例。 @return NULL 为 INVALID_ARGUMENT，其他为 CC_OK，诊断交调用者。 */
cc_result_t cc_tool_registry_freeze(cc_tool_registry_t *registry);

/** @brief 读取冻结标志快照。 @param[in] registry 可空存活实例。 @return NULL 为零，其他为当前标志，后续操作仍检查其自身结果。 */
int cc_tool_registry_is_frozen(cc_tool_registry_t *registry);

/**
 * @brief 按精确名称取得注册工具的浅借用。
 * @param[in] registry 必填存活实例。
 * @param[in] name 必填调用期稳定名称。
 * @param[out] out_tool 必填描述符槽，仅成功覆盖，失败保留原值。
 * @return 参数、NOT_FOUND 或 CC_OK，诊断交调用者，借用使用期由外层保持 registry 存活。
 */
cc_result_t cc_tool_registry_find(
    cc_tool_registry_t *registry,
    const char *name,
    cc_tool_t *out_tool
);

/**
 * @brief 在同一锁内读取匹配工具与能力。
 * @param[in] registry 必填存活实例。
 * @param[in] name 必填调用期稳定名称。
 * @param[out] out_tool 必填浅借用槽，失败保留原值。
 * @param[out] out_capabilities 可选值槽，存在时先置零，成功取得匹配条目能力。
 * @return 参数、NOT_FOUND 或 CC_OK，诊断交调用者，输出 self 继续由 registry 拥有。
 */
cc_result_t cc_tool_registry_find_with_capabilities(
    cc_tool_registry_t *registry, const char *name, cc_tool_t *out_tool, unsigned *out_capabilities);

/**
 * @brief 从提供 schema 的条目构造完整 function 数组 JSON。
 * @param[in] registry 必填存活实例，元数据回调在锁内执行。
 * @param[out] out_tools_json 必填空槽，基础参数齐全后先置 NULL，成功文本由调用者 free。
 * @return 参数、schema JSON 或 OOM 错误，失败无部分输出，诊断由调用者释放。
 * @note NULL schema 跳过，缺名称使用 unknown，schema 须完整对象且无解码 NUL，字符串元数据正确转义。
 */
cc_result_t cc_tool_registry_build_schema_json(
    cc_tool_registry_t *registry,
    char **out_tools_json
);

/** @brief 快照全部注册条目数。 @param[in] registry 可空存活实例。 @return NULL 为零，其他为当前数量，包含无名称或无 schema 条目。 */
size_t cc_tool_registry_count(cc_tool_registry_t *registry);

/**
 * @brief 按注册顺序建立独立的名称数组副本。
 * @param[in] registry 必填存活实例，名称回调在锁内执行。
 * @param[out] out_names 必填空槽，基础参数齐全后先置 NULL，数组及非空元素分别由调用者 free。
 * @param[out] out_count 必填值槽，基础参数齐全后先置零，成功写注册条目数。
 * @return 参数、OOM 或 CC_OK，失败清理全部部分副本并保留空输出，诊断交调用者。
 * @note 无名称回调或返回 NULL 的条目对应 NULL 元素，零条目返回 NULL 数组和零。
 */
cc_result_t cc_tool_registry_list_names(
    cc_tool_registry_t *registry,
    char ***out_names,
    size_t *out_count
);

#ifdef __cplusplus
}
#endif

#endif
