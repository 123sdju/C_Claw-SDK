

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

/* 创建空 skill catalog；成功后调用方 destroy。 */
cc_result_t cc_skill_catalog_create(cc_skill_catalog_t **out_catalog);

/* 销毁 catalog 和已加载 skill 元数据。 */
void cc_skill_catalog_destroy(cc_skill_catalog_t *catalog);

/* 根据 config 和 filesystem 加载 skill 描述；filesystem 只在调用期间借用。 */
cc_result_t cc_skill_catalog_load_from_config(
    cc_skill_catalog_t *catalog,
    cc_filesystem_t *filesystem,
    const cc_config_t *config
);

/* 构建可注入 system prompt 的 skill 文本；allowlist 为 NULL 时由实现决定默认策略。 */
cc_result_t cc_skill_catalog_build_prompt(
    cc_skill_catalog_t *catalog,
    const cc_config_string_list_t *allowlist,
    char **out_prompt
);

/* 列出已加载 skill 名称；返回数组和字符串由调用方释放。 */
cc_result_t cc_skill_catalog_list_names(
    cc_skill_catalog_t *catalog,
    char ***out_names,
    size_t *out_count
);

#ifdef __cplusplus
}
#endif

#endif
