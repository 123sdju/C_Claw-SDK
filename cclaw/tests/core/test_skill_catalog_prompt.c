#include "cc/app/cc_skill_catalog.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/util/cc_config.h"
#include "cc/internal/cc_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct read_fault {
    cc_filesystem_t *underlying;
    size_t reads;
} read_fault_t;

/** @brief 在第二次读取注入 OOM，覆盖已处理列名的错误清理。
 * @param[in,out] self 必填独占故障上下文，底层文件系统覆盖调用。
 * @param[in] path 必填同步路径借用。
 * @param[out] out_text 必填空槽，成功正文归加载器 free，注入失败写 NULL。
 * @return 第二次读取为 OUT_OF_MEMORY，其余透传底层，串行调用。
 */
static cc_result_t read_with_fault(void *self, const char *path, char **out_text)
{
    read_fault_t *fault = self;
    *out_text = NULL;
    if (++fault->reads == 2U)
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Injected skill read allocation failure");
    return fault->underlying->vtable->read_text(fault->underlying->self, path, out_text);
}

/** @brief 转发故障端口的目录存在检查。
 * @param[in] self 必填上下文借用。
 * @param[in] path 必填同步路径借用。
 * @param[out] out_exists 必填存在标志槽，依底层约定。
 * @return 底层结果，诊断归加载器释放，串行调用。
 */
static cc_result_t exists_with_fault(void *self, const char *path, int *out_exists)
{
    read_fault_t *fault = self;
    return fault->underlying->vtable->exists(fault->underlying->self, path, out_exists);
}

/** @brief 转发目录快照枚举。
 * @param[in] self 必填上下文借用。
 * @param[in] path 必填同步路径借用。
 * @param[out] out_items 必填空槽，成功数组及逐项名称由加载器 free。
 * @param[out] out_count 必填条数槽，依底层约定。
 * @return 底层结果，诊断归加载器释放，串行调用。
 */
static cc_result_t list_with_fault(void *self, const char *path, char ***out_items, size_t *out_count)
{
    read_fault_t *fault = self;
    return fault->underlying->vtable->list_dir(fault->underlying->self, path, out_items, out_count);
}

/** @brief 检查正文包含片段。
 * @param[in] haystack 可为 NULL 的正文借用。
 * @param[in] needle 可为 NULL 的片段借用。
 * @return 两指针有效且包含片段为真，无分配及共享状态。
 */
static int has_text(const char *haystack, const char *needle)
{
    return haystack && needle && strstr(haystack, needle) != NULL;
}

/**
 * @brief 验证加载、覆盖、允许名单及空过滤结果。
 * @return 全部检查通过为零，失败为一。
 * @note 测试串行持有文件系统和配置，成功路径回收目录及拥有输出，早退交进程回收。
 */
int main(void)
{
    cc_filesystem_t fs = {0};
    cc_config_t config;
    memset(&config, 0, sizeof(config));
    if (cc_filesystem_get_default(&fs).code != CC_OK) return 1;
    if (cc_config_load_default(&config).code != CC_OK) return 1;

    const char *base = "/tmp/cclaw_skill_catalog_test";
    const char *skills = "/tmp/cclaw_skill_catalog_test/skills";
    const char *alpha = "/tmp/cclaw_skill_catalog_test/skills/alpha";
    const char *beta = "/tmp/cclaw_skill_catalog_test/skills/beta";
    fs.vtable->make_dir(fs.self, alpha);
    fs.vtable->make_dir(fs.self, beta);
    fs.vtable->write_text(fs.self, "/tmp/cclaw_skill_catalog_test/skills/alpha/SKILL.md",
        "# Alpha\nUse alpha instructions.");
    fs.vtable->write_text(fs.self, "/tmp/cclaw_skill_catalog_test/skills/beta/SKILL.md",
        "# Beta\nUse beta instructions.");

    free(config.workspace_path);
    config.workspace_path = cc_copy_string(base);
    config.skills.extra_dirs.items = calloc(1, sizeof(char *));
    config.skills.extra_dirs.items[0] = cc_copy_string(skills);
    config.skills.extra_dirs.count = 1;
    config.agents.defaults.skills.items = calloc(1, sizeof(char *));
    config.agents.defaults.skills.items[0] = cc_copy_string("alpha");
    config.agents.defaults.skills.count = 1;

    cc_skill_catalog_t *catalog = NULL;
    if (cc_skill_catalog_create(&catalog).code != CC_OK) return 1;
    if (cc_skill_catalog_load_from_config(catalog, &fs, &config).code != CC_OK) return 1;

    char *prompt = NULL;
    if (cc_skill_catalog_build_prompt(catalog, &config.agents.defaults.skills, &prompt).code != CC_OK) return 1;
    int ok = has_text(prompt, "Alpha") && has_text(prompt, "Use alpha") && !has_text(prompt, "Use beta");

    free(prompt);
    prompt = NULL;
    free(config.agents.defaults.skills.items[0]);
    config.agents.defaults.skills.items[0] = cc_copy_string("missing-skill");
    if (cc_skill_catalog_build_prompt(catalog, &config.agents.defaults.skills, &prompt).code != CC_OK) return 1;
    ok = ok && prompt == NULL;
    free(prompt);
    prompt = NULL;
    fs.vtable->write_text(fs.self, "/tmp/cclaw_skill_catalog_test/skills/alpha/SKILL.md", "Updated alpha");
    if (cc_skill_catalog_load_from_config(catalog, &fs, &config).code != CC_OK) return 1;
    if (cc_skill_catalog_build_prompt(catalog, NULL, &prompt).code != CC_OK) return 1;
    ok = ok && has_text(prompt, "Updated alpha") && !has_text(prompt, "Use alpha") && has_text(prompt, "Use beta");
    free(prompt);
    read_fault_t fault = {.underlying = &fs};
    cc_filesystem_vtable_t fault_vtable = {
        .read_text = read_with_fault,
        .exists = exists_with_fault,
        .list_dir = list_with_fault
    };
    cc_filesystem_t fault_fs = {.self = &fault, .vtable = &fault_vtable};
    cc_result_t failed_load = cc_skill_catalog_load_from_config(catalog, &fault_fs, &config);
    ok = ok && failed_load.code == CC_ERR_OUT_OF_MEMORY && fault.reads == 2U;
    cc_result_free(&failed_load);
    cc_skill_catalog_destroy(catalog);
    cc_config_destroy(&config);
    if (fs.vtable && fs.vtable->destroy) fs.vtable->destroy(fs.self);
    return ok ? 0 : 1;
}
