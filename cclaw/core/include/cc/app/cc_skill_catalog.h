

#ifndef CC_SKILL_CATALOG_H
#define CC_SKILL_CATALOG_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/util/cc_config.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * skill catalog 不透明句柄。
 *
 * catalog 保存从配置/文件系统加载的 skill 描述，并可生成 system prompt 片段。它只负责
 * SDK 内部提示词拼装，不提供外部 skill marketplace 或业务分发。
 */
typedef struct cc_skill_catalog cc_skill_catalog_t;

/** @brief 创建空技能目录。
 * @param[out] out_catalog 必填新槽，先置 NULL，成功对象由调用者 destroy。
 * @return CC_OK、INVALID_ARGUMENT 或 OUT_OF_MEMORY，结果诊断由调用者释放。
 */
cc_result_t cc_skill_catalog_create(cc_skill_catalog_t **out_catalog);

/** @brief 消费目录及其拥有的名称、来源和正文。
 * @param[in,out] catalog 可为 NULL 的拥有对象，调用者变量自行失效。
 * @pre 所有加载、列名和提示构建调用已退出，对象无内部锁。
 */
void cc_skill_catalog_destroy(cc_skill_catalog_t *catalog);

/** @brief 按配置顺序合并技能文件。
 * @param[in,out] catalog 必填独占目录，后加载同名覆盖，未涉及旧条目保留。
 * @param[in] filesystem 必填调用期借用端口，list/read 缺失时跳过，exists 可选。
 * @param[in] config 必填调用期稳定配置，依次加载 HOME、agent skills、插件、extra 和 workspace .agents/skills。
 * @return CC_OK 或参数/分配错误，普通缺失和读取失败跳过，失败保留先前已完成条目。
 * @pre 加载期间禁止并发读取或销毁目录，配置和文件来源须由调用者信任。
 * @note 使用普通文本读取，后端限定单文件大小，目录和提示总字节预算由上层承担。
 */
cc_result_t cc_skill_catalog_load_from_config(
    cc_skill_catalog_t *catalog,
    cc_filesystem_t *filesystem,
    const cc_config_t *config
);

/** @brief 构建技能系统指令片段。
 * @param[in] catalog 可为 NULL 的稳定目录，NULL/空目录成功交付空输出。
 * @param[in] allowlist 可为 NULL 的完整名称名单，NULL 或零项允许全部。
 * @param[out] out_prompt 必填新槽，先置 NULL，成功正文由调用者 free，全过滤时保留 NULL。
 * @return CC_OK 或参数、容量、分配错误，失败清理部分构建器。
 * @pre 调用期禁止目录修改或销毁，注入方负责内容信任与上下文预算。
 */
cc_result_t cc_skill_catalog_build_prompt(
    cc_skill_catalog_t *catalog,
    const cc_config_string_list_t *allowlist,
    char **out_prompt
);

/** @brief 复制目录名称快照。
 * @param[in] catalog 可为 NULL 的稳定目录，NULL/空目录成功交空结果。
 * @param[out] out_names 必填空槽，成功数组及逐项字符串均由调用者 free。
 * @param[out] out_count 必填条数槽，基本参数有效后与 names 一同初始化为空。
 * @return CC_OK 或参数/分配错误，失败回收部分副本。
 * @pre 禁止与加载、销毁并发，输出独立于目录后续寿命。
 */
cc_result_t cc_skill_catalog_list_names(
    cc_skill_catalog_t *catalog,
    char ***out_names,
    size_t *out_count
);

#ifdef __cplusplus
}
#endif

#endif
