#include "cc/app/cc_skill_catalog.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_config.h"
#include "cc/internal/cc_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 字符串包含断言 helper。 */
static int has_text(const char *haystack, const char *needle)
{
    return haystack && needle && strstr(haystack, needle) != NULL;
}

/*
 * 验证 skill catalog 从配置目录加载并按 allowlist 构造 prompt。
 *
 * 创建 alpha/beta 两个 SKILL.md，但 defaults.skills 只允许 alpha，最终 prompt 不能包含 beta。
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
    cc_skill_catalog_destroy(catalog);
    cc_config_destroy(&config);
    if (fs.vtable && fs.vtable->destroy) fs.vtable->destroy(fs.self);
    return ok ? 0 : 1;
}
