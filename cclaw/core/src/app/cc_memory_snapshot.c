#include "cc/app/cc_memory_snapshot.h"
#include "cc/core/cc_id.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_thread.h"
#include "cc/util/cc_json.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cc_memory_snapshot_store {
    cc_filesystem_t *fs;  /* 借用，覆盖实例整个寿命。 */
    char *directory;     /* 拥有的快照目录路径副本。 */
    char *active_path;   /* 拥有的主文件路径副本。 */
    cc_mutex_t mutex;    /* 本实例文件操作的互斥锁，主文件写入由外层协调。 */
};

/**
 * @brief 按反射多项式 0xedb88320 计算文本内容的 CRC32。
 * @param[in] text 必填的 NUL 结尾字符串，同步借用，结尾 NUL 不计入校验。
 * @return 对全部正文计算并取最终异或的 32 位校验值。
 */
static uint32_t snapshot_crc32(const char *text)
{
    uint32_t crc = 0xffffffffU;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        crc ^= *p;
        for (unsigned i = 0; i < 8U; i++)
            crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
    }
    return crc ^ 0xffffffffU;
}

/**
 * @brief 检查原因前缀、小写 UUID 形状及八位小写十六进制校验后缀。
 * @param[in] id 同步借用的 ID，允许 NULL，UUID 的版本位和变体位仅按十六进制字符检查。
 * @param[out] out_reason 可选输出，仅 ID 格式全部通过后填写原因，失败保持原值。
 * @return 格式合法为一，其余为零，内容校验由 checksum_matches 执行。
 */
static int valid_id(const char *id, cc_memory_snapshot_reason_t *out_reason)
{
    if (!id) return 0;
    size_t prefix;
    cc_memory_snapshot_reason_t reason;
    if (strncmp(id, "rewrite-", 8) == 0) { prefix = 8; reason = CC_MEMORY_SNAPSHOT_REWRITE; }
    else if (strncmp(id, "reset-", 6) == 0) { prefix = 6; reason = CC_MEMORY_SNAPSHOT_RESET; }
    else return 0;
    if (strlen(id) != prefix + 45U || id[prefix + 36U] != '-') return 0;
    for (size_t i = 0; i < 36U; i++) {
        char c = id[prefix + i];
        if (i == 8U || i == 13U || i == 18U || i == 23U) { if (c != '-') return 0; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    for (size_t i = prefix + 37U; i < prefix + 45U; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return 0;
    if (out_reason) *out_reason = reason;
    return 1;
}

/**
 * @brief 将文本 CRC32 格式化为八位小写十六进制并比较 ID 后缀。
 * @pre id 已由 valid_id 校验通过，text 为有效字符串。
 * @param[in] id 同步借用的快照 ID。
 * @param[in] text 同步借用的完整文件文本。
 * @return 后缀与内容 CRC32 一致为一，否则为零。
 */
static int checksum_matches(const char *id, const char *text)
{
    char expected[9];
    snprintf(expected, sizeof(expected), "%08lx", (unsigned long)snapshot_crc32(text));
    return strcmp(id + strlen(id) - 8U, expected) == 0;
}

/**
 * @brief 分配并拼接目录、斜线、ID 和文件后缀。
 * @param[in] store 借用的有效存储，其目录字符串在实例寿命内稳定。
 * @param[in] id 同步借用的非空指针字符串，公开入口先验证 ID 格式。
 * @param[in] suffix 同步借用的后缀，本文件传入 .json 或 .tmp。
 * @return 拥有的路径字符串，调用者 free，长度检查或分配失败返回 NULL。
 */
static char *path_for(cc_memory_snapshot_store_t *store, const char *id, const char *suffix)
{
    size_t a = strlen(store->directory), b = strlen(id), c = strlen(suffix);
    if (a > SIZE_MAX - b - c - 2U) return NULL;
    char *path = malloc(a + b + c + 2U);
    if (path) snprintf(path, a + b + c + 2U, "%s/%s%s", store->directory, id, suffix);
    return path;
}

/**
 * @brief 按能力标志同步快照目录，缺少目录同步能力时直接成功。
 * @pre 调用者持有 store->mutex，open 已核对能力与 vtable 一致。
 * @param[in,out] store 借用的存储及文件系统。
 * @return 支持目录同步时原样返回 sync_dir 结果，其余返回 CC_OK。
 */
static cc_result_t sync_directory(cc_memory_snapshot_store_t *store)
{
    return (store->fs->capabilities & CC_FS_CAP_DIR_SYNC) ?
        store->fs->vtable->sync_dir(store->fs->self, store->directory) : cc_result_ok();
}

/**
 * @brief 先 stat 普通文件，再读全文并校验字节长度、解析尾部消费及数组顶层。
 *
 * cc_json_parse_strict 使用 cJSON 的接纳规则，业务记录格式由上层记忆存储另行验证。
 * @pre 调用者持有 store->mutex，并协调主文件的外部写入。
 * @param[in] store 借用的存储及有效文件系统。
 * @param[in] path 同步借用的完整文件路径。
 * @param[out] out_json 必填，先置 NULL，成功取得拥有的原文本，用 free 释放。
 * @param[out] out_modified 可选输出，成功填写 stat 的修改毫秒时间，失败保持原值。
 * @return 缺失为 CC_ERR_NOT_FOUND，类型、长度或非数组为 CC_ERR_STORAGE，解析与文件系统错误原样返回。
 */
static cc_result_t read_memory_file(cc_memory_snapshot_store_t *store, const char *path,
                                    char **out_json, uint64_t *out_modified)
{
    *out_json = NULL;
    cc_filesystem_stat_t stat = {.size = sizeof(stat)};
    cc_result_t rc = store->fs->vtable->stat(store->fs->self, path, &stat);
    if (rc.code != CC_OK) return rc;
    if (stat.type == CC_FS_ENTRY_MISSING) return cc_result_error(CC_ERR_NOT_FOUND, "Memory version not found");
    if (stat.type != CC_FS_ENTRY_FILE || stat.byte_size > SIZE_MAX)
        return cc_result_error(CC_ERR_STORAGE, "Memory version file is invalid");
    char *text = NULL;
    rc = store->fs->vtable->read_text(store->fs->self, path, &text);
    if (rc.code != CC_OK) return rc;
    if (!text || strlen(text) != (size_t)stat.byte_size) {
        free(text);
        return cc_result_error(CC_ERR_STORAGE, "Memory version is truncated");
    }
    cc_json_value_t *json = NULL;
    rc = cc_json_parse_strict(text, &json);
    if (rc.code == CC_OK && !cc_json_is_array(json))
        rc = cc_result_error(CC_ERR_STORAGE, "Memory version is not an array");
    cc_json_destroy(json);
    if (rc.code != CC_OK) { free(text); return rc; }
    if (out_modified) *out_modified = stat.modified_ms;
    *out_json = text;
    return cc_result_ok();
}

/**
 * @brief 检查文件系统表和能力、确认目录存在，再复制路径并创建互斥锁。
 * @param[in] fs 借用至销毁的文件系统及 vtable，须支持必需操作和所声明的可选目录同步。
 * @param[in] directory 已存在的非空目录路径，同步借用并复制。
 * @param[in] active_path 非空主文件路径，同步借用并复制，主文件在 create/confirm 时读取。
 * @param[out] out_store 必填，先置 NULL，成功取得拥有实例，失败清理本次分配。
 * @return 输出非法为 CC_ERR_INVALID_ARGUMENT，能力/路径非法为 CC_ERR_UNSUPPORTED，目录类型错误为 CC_ERR_STORAGE，
 * 分配失败为 CC_ERR_OUT_OF_MEMORY，stat 和锁创建错误原样返回，成功为 CC_OK。
 */
cc_result_t cc_memory_snapshot_store_open(cc_filesystem_t *fs, const char *directory,
                                          const char *active_path,
                                          cc_memory_snapshot_store_t **out_store)
{
    if (!out_store) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null memory snapshot output");
    *out_store = NULL;
    uint64_t caps = CC_FS_CAP_STAT | CC_FS_CAP_ATOMIC_REPLACE | CC_FS_CAP_FILE_SYNC;
    if (!fs || fs->size < sizeof(*fs) || !fs->vtable ||
        fs->vtable->size < offsetof(cc_filesystem_vtable_t, sync_dir) + sizeof(fs->vtable->sync_dir) ||
        (fs->capabilities & caps) != caps || !directory || !directory[0] || !active_path || !active_path[0] ||
        !fs->vtable->stat || !fs->vtable->list_dir || !fs->vtable->read_text || !fs->vtable->write_text ||
        !fs->vtable->remove || !fs->vtable->sync_file || !fs->vtable->atomic_replace ||
        ((fs->capabilities & CC_FS_CAP_DIR_SYNC) && !fs->vtable->sync_dir))
        return cc_result_error(CC_ERR_UNSUPPORTED, "Filesystem cannot store memory snapshots");
    cc_filesystem_stat_t info = {.size = sizeof(info)};
    cc_result_t rc = fs->vtable->stat(fs->self, directory, &info);
    if (rc.code != CC_OK) return rc;
    if (info.type != CC_FS_ENTRY_DIRECTORY)
        return cc_result_error(CC_ERR_STORAGE, "Memory snapshot directory is unavailable");
    cc_memory_snapshot_store_t *store = calloc(1, sizeof(*store));
    if (!store) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate memory snapshot store");
    store->fs = fs;
    store->directory = cc_copy_string(directory);
    store->active_path = cc_copy_string(active_path);
    if (!store->directory || !store->active_path) {
        free(store->directory); free(store->active_path); free(store);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy memory snapshot paths");
    }
    rc = cc_mutex_create(&store->mutex);
    if (rc.code != CC_OK) { free(store->directory); free(store->active_path); free(store); return rc; }
    *out_store = store;
    return cc_result_ok();
}

/**
 * @brief 销毁锁并释放路径副本及实例，快照文件继续保留。
 * @pre 外层已关闭入口并排空调用，借用 fs 仍存活。
 * @param[in,out] store 待释放实例，允许 NULL。
 * @post 非空实例寿命结束，其全部借用指针失效。
 */
void cc_memory_snapshot_store_destroy(cc_memory_snapshot_store_t *store)
{
    if (!store) return;
    cc_mutex_destroy(store->mutex);
    free(store->directory); free(store->active_path); free(store);
}

/**
 * @brief 对比不可变的绑定路径，供记忆提交核对快照来源。
 * @param[in] store 借用的存储，允许 NULL。
 * @param[in] active_path 同步借用的路径字符串，允许 NULL。
 * @return 参数有效且路径字节相同为一，否则为零。
 */
int cc_memory_snapshot_source_matches(cc_memory_snapshot_store_t *store, const char *active_path)
{
    return store && active_path && strcmp(store->active_path, active_path) == 0;
}

/**
 * @brief 锁内读取主文件，以 UUID 和内容 CRC32 命名，经临时写入、同步及回读后原子安装快照。
 *
 * 本次开始写临时文件后，替换前的错误会尝试删除该临时文件。既有候选路径冲突保留现场。
 * 已尝试替换时保留文件和 ID，供调用者确认安装结果，即使替换或目录同步返回错误。
 * @pre 外层持续持有主文件写入所有权至后续对应记忆提交完成。
 * @param[in,out] store 借用的存储及文件系统，文件阶段持实例锁执行。
 * @param[in] reason 必须为 REWRITE 或 RESET，决定 ID 前缀。
 * @param[out] out_id 必填，参数有效后先置 NULL，成功或已尝试替换时输出拥有的 ID，调用者 free。
 * @return 成功为 CC_OK，非法参数为 CC_ERR_INVALID_ARGUMENT，主文件缺失为 CC_ERR_NOT_FOUND，
 * 分配、UUID、格式、路径冲突、读取、写入、校验、替换及同步错误向上传递。
 */
cc_result_t cc_memory_snapshot_create(cc_memory_snapshot_store_t *store,
                                       cc_memory_snapshot_reason_t reason, char **out_id)
{
    if (!store || !out_id || (reason != CC_MEMORY_SNAPSHOT_REWRITE && reason != CC_MEMORY_SNAPSHOT_RESET))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory snapshot request");
    *out_id = NULL;
    char *base = NULL;
    cc_result_t rc = cc_id_generate_uuid_v4(reason == CC_MEMORY_SNAPSHOT_REWRITE ? "rewrite-" : "reset-", &base);
    if (rc.code != CC_OK) return rc;
    cc_mutex_lock(store->mutex);
    char *text = NULL;
    rc = read_memory_file(store, store->active_path, &text, NULL);
    char *id = NULL, *path = NULL, *temporary = NULL;
    if (rc.code == CC_OK) {
        size_t length = strlen(base);
        id = malloc(length + 10U);
        if (!id) rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate snapshot id");
        else {
            snprintf(id, length + 10U, "%s-%08lx", base, (unsigned long)snapshot_crc32(text));
            path = path_for(store, id, ".json");
            temporary = path_for(store, id, ".tmp");
            if (!path || !temporary) rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate snapshot path");
        }
    }
    cc_filesystem_stat_t info = {.size = sizeof(info)};
    if (rc.code == CC_OK) rc = store->fs->vtable->stat(store->fs->self, path, &info);
    if (rc.code == CC_OK && info.type != CC_FS_ENTRY_MISSING)
        rc = cc_result_error(CC_ERR_STORAGE, "Memory snapshot id already exists");
    if (rc.code == CC_OK) rc = store->fs->vtable->stat(store->fs->self, temporary, &info);
    if (rc.code == CC_OK && info.type != CC_FS_ENTRY_MISSING)
        rc = cc_result_error(CC_ERR_STORAGE, "Memory snapshot temporary path already exists");
    int attempted_write = 0;
    if (rc.code == CC_OK) {
        attempted_write = 1;
        rc = store->fs->vtable->write_text(store->fs->self, temporary, text);
    }
    if (rc.code == CC_OK) rc = store->fs->vtable->sync_file(store->fs->self, temporary);
    if (rc.code == CC_OK) {
        char *check = NULL;
        rc = store->fs->vtable->read_text(store->fs->self, temporary, &check);
        if (rc.code == CC_OK && (!check || strcmp(check, text) != 0))
            rc = cc_result_error(CC_ERR_STORAGE, "Memory snapshot verification failed");
        free(check);
    }
    int attempted_replace = 0;
    if (rc.code == CC_OK) {
        attempted_replace = 1;
        rc = store->fs->vtable->atomic_replace(store->fs->self, temporary, path);
    }
    if (rc.code == CC_OK) rc = sync_directory(store);
    if (rc.code != CC_OK && attempted_write && !attempted_replace) {
        cc_result_t cleanup = store->fs->vtable->remove(store->fs->self, temporary);
        cc_result_free(&cleanup);
    }
    cc_mutex_unlock(store->mutex);
    free(base); free(text); free(path); free(temporary);
    if (rc.code == CC_OK || attempted_replace) *out_id = id;
    else free(id);
    return rc;
}

/**
 * @brief 锁内校验快照及 CRC32，与当前主文件逐字比较后同步目录。
 * @pre 外层协调主文件写入，并将确认至对应记忆提交作为同一写入所有权区间。
 * @param[in,out] store 借用的存储及文件系统。
 * @param[in] id 同步借用的完整快照 ID，先检查格式再构造路径。
 * @return 相同且同步成功为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，文件缺失为 CC_ERR_NOT_FOUND，
 * 主文件已变化为 CC_ERR_INVALID_STATE，分配、读取、校验及同步错误向上传递。
 */
cc_result_t cc_memory_snapshot_confirm(cc_memory_snapshot_store_t *store, const char *id)
{
    if (!store || !valid_id(id, NULL))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory snapshot confirmation");
    char *path = path_for(store, id, ".json"), *json = NULL;
    if (!path) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate snapshot path");
    cc_mutex_lock(store->mutex);
    cc_result_t rc = read_memory_file(store, path, &json, NULL);
    if (rc.code == CC_OK && !checksum_matches(id, json))
        rc = cc_result_error(CC_ERR_STORAGE, "Memory snapshot checksum mismatch");
    char *active = NULL;
    if (rc.code == CC_OK) rc = read_memory_file(store, store->active_path, &active, NULL);
    if (rc.code == CC_OK && strcmp(json, active) != 0)
        rc = cc_result_error(CC_ERR_INVALID_STATE, "Active memory changed after snapshot");
    if (rc.code == CC_OK) rc = sync_directory(store);
    cc_mutex_unlock(store->mutex);
    free(path); free(json); free(active);
    return rc;
}

/**
 * @brief 释放已初始化的 ID 字符串和列表数组。
 * @param[in,out] items 拥有的列表，count 为零时允许 NULL。
 * @param[in] count 已初始化且拥有 ID 的元素数，须在数组范围内。
 * @post ID 与数组已释放，调用者持有的指针失效。
 */
void cc_memory_snapshot_info_free_array(cc_memory_snapshot_info_t *items, size_t count)
{
    for (size_t i = 0; i < count; i++) free(items[i].id);
    free(items);
}

/**
 * @brief 为 qsort 按修改时间降序、同时间 ID 字节序升序比较两项。
 * @param[in] left 借用的有效 cc_memory_snapshot_info_t 元素指针。
 * @param[in] right 借用的有效 cc_memory_snapshot_info_t 元素指针。
 * @return left 排在前时为负、相同为零、排在后时为正。
 */
static int compare_snapshots(const void *left, const void *right)
{
    const cc_memory_snapshot_info_t *a = left, *b = right;
    if (a->modified_ms != b->modified_ms) return a->modified_ms > b->modified_ms ? -1 : 1;
    return strcmp(a->id, b->id);
}

/**
 * @brief 锁内筛选并验证历史目录的快照，解锁后排序并移交拥有列表。
 *
 * 只处理原因前缀和 .json 后缀匹配的名称，匹配候选损坏则清理全部部分结果并报错。
 * @param[in] store 借用的有效存储，内部锁覆盖遍历和文件校验。
 * @param[out] out_items 必填且无待释放内容，参数有效后置 NULL，成功取得列表，用 cc_memory_snapshot_info_free_array 清理。
 * @param[out] out_count 必填，参数有效后置零，成功返回已验证条数，零条时列表仍可能已分配。
 * @return 成功为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，分配、目录列举、名称、文件及校验错误向上传递。
 */
cc_result_t cc_memory_snapshot_list(cc_memory_snapshot_store_t *store,
                                     cc_memory_snapshot_info_t **out_items, size_t *out_count)
{
    if (!store || !out_items || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory snapshot list");
    *out_items = NULL; *out_count = 0;
    cc_mutex_lock(store->mutex);
    char **names = NULL;
    size_t count = 0, used = 0;
    cc_result_t rc = store->fs->vtable->list_dir(store->fs->self, store->directory, &names, &count);
    cc_memory_snapshot_info_t *items = NULL;
    if (rc.code == CC_OK && count) {
        items = calloc(count, sizeof(*items));
        if (!items) rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate snapshot list");
    }
    for (size_t i = 0; rc.code == CC_OK && i < count; i++) {
        size_t length = strlen(names[i]);
        if (length < 6U || strcmp(names[i] + length - 5U, ".json") != 0 ||
            (strncmp(names[i], "rewrite-", 8) != 0 && strncmp(names[i], "reset-", 6) != 0)) continue;
        char *id = cc_copy_string(names[i]);
        if (!id) { rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy snapshot id"); break; }
        id[length - 5U] = '\0';
        cc_memory_snapshot_reason_t reason;
        if (!valid_id(id, &reason)) { free(id); rc = cc_result_error(CC_ERR_STORAGE, "Invalid memory snapshot filename"); break; }
        char *path = path_for(store, id, ".json"), *json = NULL;
        if (!path) { free(id); rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate snapshot path"); break; }
        uint64_t modified = 0;
        rc = read_memory_file(store, path, &json, &modified);
        if (rc.code == CC_OK && !checksum_matches(id, json))
            rc = cc_result_error(CC_ERR_STORAGE, "Memory snapshot checksum mismatch");
        free(path); free(json);
        if (rc.code != CC_OK) { free(id); break; }
        items[used].id = id; items[used].reason = reason; items[used].modified_ms = modified; used++;
    }
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
    cc_mutex_unlock(store->mutex);
    if (rc.code != CC_OK) { cc_memory_snapshot_info_free_array(items, used); return rc; }
    if (used > 1U) qsort(items, used, sizeof(*items), compare_snapshots);
    *out_items = items; *out_count = used;
    return cc_result_ok();
}

/**
 * @brief 根据合法 ID 构造路径，锁内读取 JSON 数组并核对 CRC32。
 * @param[in] store 借用的存储，内部锁覆盖读取和内容校验。
 * @param[in] id 同步借用的快照 ID，须通过完整格式检查。
 * @param[out] out_json 必填，参数有效后先置 NULL，成功输出原文本供调用者 free，校验失败释放并置 NULL。
 * @return 成功为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，文件缺失为 CC_ERR_NOT_FOUND，分配、读取或校验错误向上传递。
 */
cc_result_t cc_memory_snapshot_read(cc_memory_snapshot_store_t *store, const char *id, char **out_json)
{
    if (!store || !valid_id(id, NULL) || !out_json)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory snapshot read");
    *out_json = NULL;
    char *path = path_for(store, id, ".json");
    if (!path) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate snapshot path");
    cc_mutex_lock(store->mutex);
    cc_result_t rc = read_memory_file(store, path, out_json, NULL);
    if (rc.code == CC_OK && !checksum_matches(id, *out_json)) {
        free(*out_json); *out_json = NULL;
        rc = cc_result_error(CC_ERR_STORAGE, "Memory snapshot checksum mismatch");
    }
    cc_mutex_unlock(store->mutex);
    free(path);
    return rc;
}

/**
 * @brief 锁内确认目标为普通文件后删除，再按能力同步目录。
 * @param[in,out] store 借用的存储及文件系统。
 * @param[in] id 同步借用的合法 ID，目标内容可损坏，删除入口按路径和文件类型检查。
 * @return 成功为 CC_OK，非法参数为 CC_ERR_INVALID_ARGUMENT，缺失为 CC_ERR_NOT_FOUND，类型错误为 CC_ERR_STORAGE，
 * 分配、stat、删除及目录同步错误向上传递，同步失败时文件可能已删除。
 */
cc_result_t cc_memory_snapshot_delete(cc_memory_snapshot_store_t *store, const char *id)
{
    if (!store || !valid_id(id, NULL)) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory snapshot deletion");
    char *path = path_for(store, id, ".json");
    if (!path) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate snapshot path");
    cc_mutex_lock(store->mutex);
    cc_filesystem_stat_t info = {.size = sizeof(info)};
    cc_result_t rc = store->fs->vtable->stat(store->fs->self, path, &info);
    if (rc.code == CC_OK && info.type == CC_FS_ENTRY_MISSING)
        rc = cc_result_error(CC_ERR_NOT_FOUND, "Memory snapshot not found");
    else if (rc.code == CC_OK && info.type != CC_FS_ENTRY_FILE)
        rc = cc_result_error(CC_ERR_STORAGE, "Memory snapshot path is not a file");
    if (rc.code == CC_OK) rc = store->fs->vtable->remove(store->fs->self, path);
    if (rc.code == CC_OK) rc = sync_directory(store);
    cc_mutex_unlock(store->mutex);
    free(path);
    return rc;
}
