



#include "cc/app/cc_skill_catalog.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"

#include <limits.h>
#include <stdint.h>
#include <stdbool.h>
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

/** @brief 消费条目拥有的名称、来源和正文并归零。
 * @param[in,out] entry 可为 NULL 的独占条目，所有读者已退出。
 * @post 空条目可重复清理，条目结构本身由 catalog 保管。
 */
static void free_entry(cc_skill_entry_t *entry)
{
    if (!entry) return;
    free(entry->name);
    free(entry->source_path);
    free(entry->content);
    memset(entry, 0, sizeof(*entry));
}

/** @brief 创建空技能目录。
 * @param[out] out_catalog 必填空槽，先置 NULL，成功交付调用者 destroy 的对象。
 * @return CC_OK、INVALID_ARGUMENT 或 OUT_OF_MEMORY，结果诊断归调用者。
 */
cc_result_t cc_skill_catalog_create(cc_skill_catalog_t **out_catalog)
{
    if (!out_catalog) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null skill catalog output");
    }
    *out_catalog = NULL;
    cc_skill_catalog_t *catalog = calloc(1, sizeof(*catalog));
    if (!catalog) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create skill catalog");
    }
    *out_catalog = catalog;
    return cc_result_ok();
}

/** @brief 消费技能目录及全部条目。
 * @param[in,out] catalog 可为 NULL 的拥有对象，调用者变量不自动清空。
 * @pre 所有加载、列名和提示构建调用均已退出，对象无内部锁。
 */
void cc_skill_catalog_destroy(cc_skill_catalog_t *catalog)
{
    if (!catalog) return;
    for (size_t i = 0; i < catalog->count; i++) {
        free_entry(&catalog->entries[i]);
    }
    free(catalog->entries);
    free(catalog);
}

/** @brief 为新条目扩容并核对容量运算。
 * @param[in,out] catalog 必填独占目录。
 * @return 具有可写槽为 1，乘法容量不可表示或 realloc 失败为 0，失败保留旧数组。
 */
static int ensure_capacity(cc_skill_catalog_t *catalog)
{
    if (catalog->count < catalog->capacity) return 1;
    size_t limit = SIZE_MAX / sizeof(*catalog->entries);
    if (limit > (size_t)LONG_MAX) limit = (size_t)LONG_MAX;
    if (catalog->capacity > limit / 2U) return 0;
    size_t next_capacity = catalog->capacity ? catalog->capacity * 2U : 8U;
    if (next_capacity > limit) return 0;
    cc_skill_entry_t *next = realloc(catalog->entries, next_capacity * sizeof(*next));
    if (!next) return 0;
    memset(next + catalog->capacity, 0, (next_capacity - catalog->capacity) * sizeof(*next));
    catalog->entries = next;
    catalog->capacity = next_capacity;
    return 1;
}

/** @brief 按完整名称查找技能。
 * @param[in] catalog 必填调用期稳定目录。
 * @param[in] name 必填 NUL 名称借用，比较区分大小写。
 * @return 找到返回非负数组下标，否则为 -1，容量保证下标可由 long 表示。
 */
static long find_skill(cc_skill_catalog_t *catalog, const char *name)
{
    for (size_t i = 0; i < catalog->count; i++) {
        if (catalog->entries[i].name && strcmp(catalog->entries[i].name, name) == 0) {
            return (long)i;
        }
    }
    return -1;
}

/** @brief 按斜杠拼接两个路径片段。
 * @param[in] a 必填 NUL 父路径，可为空串。
 * @param[in] b 必填 NUL 后缀，不执行路径规范化或根约束。
 * @return 调用者 free 的拥有路径，NULL 输入、容量溢出或分配失败返回 NULL。
 */
static char *join_path2(const char *a, const char *b)
{
    if (!a || !b) return NULL;
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    int need_slash = alen > 0 && a[alen - 1] != '/';
    if (alen > SIZE_MAX - blen || alen + blen > SIZE_MAX - (size_t)need_slash - 1U) return NULL;
    char *out = malloc(alen + (need_slash ? 1 : 0) + blen + 1);
    if (!out) return NULL;
    memcpy(out, a, alen);
    size_t pos = alen;
    if (need_slash) out[pos++] = '/';
    memcpy(out + pos, b, blen);
    out[pos + blen] = '\0';
    return out;
}

/** @brief 从进程 HOME 构造默认技能路径。
 * @return HOME 存在时返回调用者 free 的路径，HOME 缺失/空或分配失败返回 NULL。
 * @pre 调用期间环境变量保持稳定，缺失与分配失败由外层结合 HOME 区分。
 */
static char *home_skill_dir(void)
{
    const char *home = getenv("HOME");
    if (!home || !home[0]) return NULL;
    return join_path2(home, ".cclaw/skills");
}

/**
 * @brief 复制完整条目后原子替换同名技能。
 * @param[in,out] catalog 必填独占目录，NULL 按跳过。
 * @param[in] name 必填名称借用，NULL 按跳过。
 * @param[in] source_path 可为 NULL 的来源路径，NULL 保存空串。
 * @param[in] content 必填正文借用，NULL 按跳过。
 * @return CC_OK 或 OUT_OF_MEMORY，失败保留原条目及 count。
 * @post 成功条目完全拥有副本，后加载同名覆盖早先内容。
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
    cc_skill_entry_t replacement = {0};
    replacement.name = cc_copy_string(name);
    replacement.source_path = cc_copy_string(source_path ? source_path : "");
    replacement.content = cc_copy_string(content);
    if (!replacement.name || !replacement.source_path || !replacement.content) {
        free_entry(&replacement);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy skill content");
    }
    if (index < 0 && !ensure_capacity(catalog)) {
        free_entry(&replacement);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow skill catalog");
    }
    cc_skill_entry_t *entry = index >= 0 ? &catalog->entries[index] : &catalog->entries[catalog->count++];
    free_entry(entry);
    *entry = replacement;
    return cc_result_ok();
}

/**
 * @brief 列出一级子项并读取各自 SKILL.md。
 * @param[in,out] catalog 可为空，非空时由当前调用独占。
 * @param[in] fs 可为空的调用期端口借用，list/read 缺失时跳过，exists 可选。
 * @param[in] dir 可为空或空串的目录借用，此时跳过。
 * @return 缺失目录和普通读取失败跳过，分配/容量失败返回 OUT_OF_MEMORY。
 * @post 已成功加载的条目保留，临时路径、正文及整份列名结果均回收。
 * @note 使用普通 read_text，目录与文件为调用者信任的提示来源，读取预算由后端决定。
 */
static cc_result_t load_skill_dir(
    cc_skill_catalog_t *catalog,
    cc_filesystem_t *fs,
    const char *dir
)
{
    if (!catalog || !fs || !fs->vtable || !dir || !dir[0]) return cc_result_ok();
    if (!fs->vtable->list_dir || !fs->vtable->read_text) return cc_result_ok();

    int exists = 1;
    cc_result_t rc = fs->vtable->exists ?
        fs->vtable->exists(fs->self, dir, &exists) : cc_result_ok();
    if (rc.code != CC_OK || !exists) {
        if (rc.code == CC_ERR_OUT_OF_MEMORY) return rc;
        cc_result_free(&rc);
        return cc_result_ok();
    }
    cc_result_free(&rc);

    char **items = NULL;
    size_t count = 0;
    rc = fs->vtable->list_dir(fs->self, dir, &items, &count);
    if (rc.code != CC_OK) {
        if (rc.code == CC_ERR_OUT_OF_MEMORY) return rc;
        cc_result_free(&rc);
        return cc_result_ok();
    }

    for (size_t i = 0; i < count; i++) {
        char *skill_dir = join_path2(dir, items[i]);
        char *skill_md = skill_dir ? join_path2(skill_dir, "SKILL.md") : NULL;
        char *content = NULL;
        if (!skill_dir || !skill_md) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate skill path");
        } else {
            cc_result_t read_rc = fs->vtable->read_text(fs->self, skill_md, &content);
            if (read_rc.code == CC_OK && content && content[0]) {
                rc = upsert_skill(catalog, items[i], skill_md, content);
            } else if (read_rc.code == CC_ERR_OUT_OF_MEMORY) {
                rc = read_rc;
                read_rc = cc_result_ok();
            }
            cc_result_free(&read_rc);
        }
        free(content);
        free(skill_dir);
        free(skill_md);
        if (rc.code != CC_OK) break;
    }
    for (size_t i = 0; i < count; i++) free(items[i]);
    free(items);
    return rc;
}

/** @brief 按插件条目及目录顺序追加技能。
 * @param[in,out] catalog 必填独占目录。
 * @param[in] fs 调用期文件系统借用。
 * @param[in] config 可为 NULL 的稳定配置，NULL 跳过。
 * @return 首个加载错误或 CC_OK，前序成功条目保留。
 */
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

/**
 * @brief 按 HOME、agent、插件、extra、workspace 顺序装入技能。
 * @param[in,out] catalog 必填独占目录，旧条目继续保留并允许同名覆盖。
 * @param[in] filesystem 必填调用期文件系统借用。
 * @param[in] config 必填稳定配置，目录字符串与数组在调用内借用。
 * @return CC_OK、INVALID_ARGUMENT 或加载分配错误，先前成功条目继续有效。
 * @note 正文作为任务指令注入，调用方负责信任配置及文件来源，catalog 本身无字节预算或沙箱。
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
    const char *home = getenv("HOME");
    if (home && home[0] && !home_dir)
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate home skill path");
    cc_result_t rc = load_skill_dir(catalog, filesystem, home_dir);
    free(home_dir);
    if (rc.code != CC_OK) return rc;

    if (config->agents.defaults.agent_dir) {
        char *personal = join_path2(config->agents.defaults.agent_dir, "skills");
        if (!personal) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate agent skill path");
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
        if (!agents) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate workspace skill path");
        rc = load_skill_dir(catalog, filesystem, agents);
        free(agents);
        if (rc.code != CC_OK) return rc;
    }
    return cc_result_ok();
}

/** @brief 判断完整技能名是否通过允许列表。
 * @param[in] allowlist 可为 NULL，NULL 或零项允许全部，非空数组须有效。
 * @param[in] name 名称借用，NULL 仅在允许全部时通过。
 * @return 允许为 1，否则为 0，比较区分大小写。
 */
static int allow_skill(const cc_config_string_list_t *allowlist, const char *name)
{
    if (!allowlist || allowlist->count == 0) return 1;
    for (size_t i = 0; i < allowlist->count; i++) {
        if (allowlist->items[i] && name && strcmp(allowlist->items[i], name) == 0) return 1;
    }
    return 0;
}

/**
 * @brief 为允许的技能构建系统提示片段。
 * @param[in] catalog 可为 NULL 的调用期稳定目录，NULL/空目录成功返回空输出。
 * @param[in] allowlist 可为 NULL 的稳定名单，NULL/零项允许全部。
 * @param[out] out_prompt 必填空槽，先置 NULL，成功字符串由调用者 free。
 * @return CC_OK 或参数/构建器容量/分配错误，全被过滤返回 NULL。
 * @pre 目录禁止并发加载或销毁，调用者将内容当作可信指令并承担总提示预算。
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
    bool included = false;
    for (size_t i = 0; i < catalog->count; i++) {
        cc_skill_entry_t *entry = &catalog->entries[i];
        if (!allow_skill(allowlist, entry->name)) continue;
        if (!included) {
            cc_string_builder_append(&sb, "\n\n[Skills]\n");
            cc_string_builder_append(&sb,
                "The following SKILL.md files are available for this agent. "
                "Use them as task-specific operating instructions when relevant.\n");
            included = true;
        }
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
    if (!included) {
        cc_string_builder_deinit(&sb);
        return cc_result_ok();
    }
    *out_prompt = cc_string_builder_take(&sb);
    return *out_prompt ? cc_result_ok() :
        cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build skill prompt");
}

/** @brief 按首次插入顺序复制全部技能名称。
 * @param[in] catalog 可为 NULL 的稳定目录，NULL/空目录交付空结果。
 * @param[out] out_names 必填空槽，参数有效后先置 NULL，成功逐项及数组均由调用者 free。
 * @param[out] out_count 必填计数槽，参数有效后先置零，单位为名称条数。
 * @return CC_OK 或参数、分配错误，失败回收全部部分副本。
 * @pre 目录禁止并发加载或销毁。
 */
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
