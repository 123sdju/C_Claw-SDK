



#include "cc/app/cc_skill_catalog.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* 单个 skill 条目；catalog 拥有 name/source_path/content。 */
typedef struct cc_skill_entry {
    char *name;
    char *source_path;
    char *content;
} cc_skill_entry_t;

/* skill catalog 内部动态数组。 */
struct cc_skill_catalog {
    cc_skill_entry_t *entries;
    size_t count;
    size_t capacity;
};

/* 释放单个 skill 条目。 */
static void free_entry(cc_skill_entry_t *entry)
{
    if (!entry) return;
    free(entry->name);
    free(entry->source_path);
    free(entry->content);
    memset(entry, 0, sizeof(*entry));
}

/* 创建空 skill catalog。 */
cc_result_t cc_skill_catalog_create(cc_skill_catalog_t **out_catalog)
{
    if (!out_catalog) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null skill catalog output");
    }
    cc_skill_catalog_t *catalog = calloc(1, sizeof(*catalog));
    if (!catalog) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create skill catalog");
    }
    *out_catalog = catalog;
    return cc_result_ok();
}

/* 销毁 catalog 和所有已加载 skill。 */
void cc_skill_catalog_destroy(cc_skill_catalog_t *catalog)
{
    if (!catalog) return;
    for (size_t i = 0; i < catalog->count; i++) {
        free_entry(&catalog->entries[i]);
    }
    free(catalog->entries);
    free(catalog);
}

/* 确保 catalog 有可写槽位。 */
static int ensure_capacity(cc_skill_catalog_t *catalog)
{
    if (catalog->count < catalog->capacity) return 1;
    size_t next_capacity = catalog->capacity ? catalog->capacity * 2 : 8;
    cc_skill_entry_t *next = realloc(catalog->entries, next_capacity * sizeof(*next));
    if (!next) return 0;
    memset(next + catalog->capacity, 0, (next_capacity - catalog->capacity) * sizeof(*next));
    catalog->entries = next;
    catalog->capacity = next_capacity;
    return 1;
}

/* 按 skill 名称查找现有条目。 */
static long find_skill(cc_skill_catalog_t *catalog, const char *name)
{
    for (size_t i = 0; i < catalog->count; i++) {
        if (catalog->entries[i].name && strcmp(catalog->entries[i].name, name) == 0) {
            return (long)i;
        }
    }
    return -1;
}

/* 拼接两个 POSIX 风格路径片段；返回值由调用方 free()。 */
static char *join_path2(const char *a, const char *b)
{
    if (!a || !b) return NULL;
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    int need_slash = alen > 0 && a[alen - 1] != '/';
    char *out = malloc(alen + (need_slash ? 1 : 0) + blen + 1);
    if (!out) return NULL;
    memcpy(out, a, alen);
    size_t pos = alen;
    if (need_slash) out[pos++] = '/';
    memcpy(out + pos, b, blen);
    out[pos + blen] = '\0';
    return out;
}

/* 构造用户 home 下的默认 skill 目录。 */
static char *home_skill_dir(void)
{
    const char *home = getenv("HOME");
    if (!home || !home[0]) return NULL;
    return join_path2(home, ".cclaw/skills");
}

/*
 * 插入或更新 skill。
 *
 * 同名 skill 后加载会覆盖先加载内容，这让 workspace/plugin skill 可以按加载顺序替换
 * 默认 skill。name 首次插入后保持不变，内容和 source_path 可更新。
 */
static cc_result_t upsert_skill(
    cc_skill_catalog_t *catalog,
    const char *name,
    const char *source_path,
    const char *content
)
{
    if (!catalog || !name || !content) return cc_result_ok();
    long index = find_skill(catalog, name);
    cc_skill_entry_t *entry = NULL;
    if (index >= 0) {
        entry = &catalog->entries[index];
        free(entry->source_path);
        free(entry->content);
        entry->source_path = NULL;
        entry->content = NULL;
    } else {
        if (!ensure_capacity(catalog)) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow skill catalog");
        }
        entry = &catalog->entries[catalog->count++];
        entry->name = cc_copy_string(name);
        if (!entry->name) {
            catalog->count--;
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy skill name");
        }
    }

    entry->source_path = source_path ? cc_copy_string(source_path) : cc_copy_string("");
    entry->content = cc_copy_string(content);
    if (!entry->source_path || !entry->content) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy skill content");
    }
    return cc_result_ok();
}

/*
 * 从一个目录加载 skills。
 *
 * 目录下每个子目录的 SKILL.md 被视为一个 skill，子目录名作为 skill 名。缺失目录、
 * 不支持 list/read 或单个文件读取失败都按可恢复 no-op 处理。
 */
static cc_result_t load_skill_dir(
    cc_skill_catalog_t *catalog,
    cc_filesystem_t *fs,
    const char *dir
)
{
    if (!catalog || !fs || !fs->vtable || !dir || !dir[0]) return cc_result_ok();
    if (!fs->vtable->list_dir || !fs->vtable->read_text) return cc_result_ok();

    int exists = 0;
    cc_result_t rc = fs->vtable->exists ?
        fs->vtable->exists(fs->self, dir, &exists) : cc_result_ok();
    if (rc.code != CC_OK || !exists) {
        cc_result_free(&rc);
        return cc_result_ok();
    }
    cc_result_free(&rc);

    char **items = NULL;
    size_t count = 0;
    rc = fs->vtable->list_dir(fs->self, dir, &items, &count);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return cc_result_ok();
    }

    for (size_t i = 0; i < count; i++) {
        char *skill_dir = join_path2(dir, items[i]);
        char *skill_md = skill_dir ? join_path2(skill_dir, "SKILL.md") : NULL;
        char *content = NULL;
        if (skill_md) {
            cc_result_t read_rc = fs->vtable->read_text(fs->self, skill_md, &content);
            if (read_rc.code == CC_OK && content && content[0]) {
                cc_result_t add_rc = upsert_skill(catalog, items[i], skill_md, content);
                if (add_rc.code != CC_OK) {
                    for (size_t j = 0; j < count; j++) free(items[j]);
                    free(items);
                    free(skill_dir);
                    free(skill_md);
                    free(content);
                    return add_rc;
                }
            }
            cc_result_free(&read_rc);
        }
        free(content);
        free(skill_dir);
        free(skill_md);
        free(items[i]);
    }
    free(items);
    return cc_result_ok();
}

/* 加载 plugin 配置中声明的 skill_dirs。 */
static cc_result_t load_plugin_skill_dirs(
    cc_skill_catalog_t *catalog,
    cc_filesystem_t *fs,
    const cc_config_t *config
)
{
    if (!config) return cc_result_ok();
    for (size_t i = 0; i < config->plugins.entry_count; i++) {
        const cc_config_plugin_entry_t *entry = &config->plugins.entries[i];
        for (size_t j = 0; j < entry->skill_dirs.count; j++) {
            cc_result_t rc = load_skill_dir(catalog, fs, entry->skill_dirs.items[j]);
            if (rc.code != CC_OK) return rc;
        }
    }
    return cc_result_ok();
}

/*
 * 按配置加载所有 skill 来源。
 *
 * 加载顺序包括用户 home、agent 默认目录、plugin skill_dirs、extra_dirs 和 workspace
 * .agents/skills。后加载同名 skill 会覆盖先前内容。
 */
cc_result_t cc_skill_catalog_load_from_config(
    cc_skill_catalog_t *catalog,
    cc_filesystem_t *filesystem,
    const cc_config_t *config
)
{
    if (!catalog || !filesystem || !config) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid skill catalog load request");
    }



    char *home_dir = home_skill_dir();
    cc_result_t rc = load_skill_dir(catalog, filesystem, home_dir);
    free(home_dir);
    if (rc.code != CC_OK) return rc;

    if (config->agents.defaults.agent_dir) {
        char *personal = join_path2(config->agents.defaults.agent_dir, "skills");
        rc = load_skill_dir(catalog, filesystem, personal);
        free(personal);
        if (rc.code != CC_OK) return rc;
    }

    rc = load_plugin_skill_dirs(catalog, filesystem, config);
    if (rc.code != CC_OK) return rc;

    for (size_t i = 0; i < config->skills.extra_dirs.count; i++) {
        rc = load_skill_dir(catalog, filesystem, config->skills.extra_dirs.items[i]);
        if (rc.code != CC_OK) return rc;
    }

    if (config->workspace_path) {
        char *agents = join_path2(config->workspace_path, ".agents/skills");
        rc = load_skill_dir(catalog, filesystem, agents);
        free(agents);
        if (rc.code != CC_OK) return rc;
    }
    return cc_result_ok();
}

/* 判断 skill 是否在 allowlist 中；空 allowlist 表示全部允许。 */
static int allow_skill(const cc_config_string_list_t *allowlist, const char *name)
{
    if (!allowlist || allowlist->count == 0) return 1;
    for (size_t i = 0; i < allowlist->count; i++) {
        if (allowlist->items[i] && name && strcmp(allowlist->items[i], name) == 0) return 1;
    }
    return 0;
}

/*
 * 构建 skill prompt 片段。
 *
 * 只把 allowlist 允许的 skill 写入 prompt；没有可用 skill 时返回 NULL prompt，避免向
 * system prompt 注入空标题。
 */
cc_result_t cc_skill_catalog_build_prompt(
    cc_skill_catalog_t *catalog,
    const cc_config_string_list_t *allowlist,
    char **out_prompt
)
{
    if (!out_prompt) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null skill prompt output");
    }
    *out_prompt = NULL;
    if (!catalog || catalog->count == 0) return cc_result_ok();

    cc_string_builder_t sb;
    cc_result_t rc = cc_string_builder_init(&sb);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    cc_string_builder_append(&sb, "\n\n[Skills]\n");
    cc_string_builder_append(&sb,
        "The following SKILL.md files are available for this agent. "
        "Use them as task-specific operating instructions when relevant.\n");

    for (size_t i = 0; i < catalog->count; i++) {
        cc_skill_entry_t *entry = &catalog->entries[i];
        if (!allow_skill(allowlist, entry->name)) continue;
        cc_string_builder_appendf(&sb, "\n## %s\n", entry->name ? entry->name : "skill");
        cc_string_builder_append(&sb, entry->content ? entry->content : "");
        size_t content_len = entry->content ? strlen(entry->content) : 0U;
        if (content_len > 0U && entry->content[content_len - 1U] != '\n') {
            cc_string_builder_append(&sb, "\n");
        }
    }

    cc_error_code_t builder_error = cc_string_builder_error(&sb);
    if (builder_error != CC_OK) {
        cc_string_builder_deinit(&sb);
        return cc_result_error(builder_error, "Failed to build skill prompt");
    }
    const char *text = cc_string_builder_cstr(&sb);
    if (!text || strlen(text) <= strlen("\n\n[Skills]\n")) {
        cc_string_builder_deinit(&sb);
        return cc_result_ok();
    }
    *out_prompt = cc_string_builder_take(&sb);
    return *out_prompt ? cc_result_ok() :
        cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build skill prompt");
}

/* 列出 catalog 中的 skill 名称，返回数组和字符串由调用方释放。 */
cc_result_t cc_skill_catalog_list_names(
    cc_skill_catalog_t *catalog,
    char ***out_names,
    size_t *out_count
)
{
    if (!out_names || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid skill list output");
    }
    *out_names = NULL;
    *out_count = 0;
    if (!catalog || catalog->count == 0) return cc_result_ok();
    char **names = calloc(catalog->count, sizeof(char *));
    if (!names) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate skill name list");
    for (size_t i = 0; i < catalog->count; i++) {
        names[i] = cc_copy_string(catalog->entries[i].name ? catalog->entries[i].name : "");
        if (!names[i]) {
            for (size_t j = 0; j < i; j++) free(names[j]);
            free(names);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy skill name");
        }
    }
    *out_names = names;
    *out_count = catalog->count;
    return cc_result_ok();
}
