



#include "cc/adapters/cc_json_memory_store.h"
#include "cc/app/cc_memory_plan.h"
#include "cc/app/cc_memory_snapshot.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/internal/cc_json_syntax_check.h"
#include "cc/ports/cc_thread.h"
#include "cc/ports/cc_filesystem.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef CC_JSON_MEMORY_MAX_FILE_BYTES
#define CC_JSON_MEMORY_MAX_FILE_BYTES 0
#endif

/** @brief 受管理记录的附加元数据，key 与 entries 中的主键对应，字符串由数组拥有。 */
typedef struct cc_json_memory_metadata {
    char *key;
    char *evidence;
    unsigned importance;
} cc_json_memory_metadata_t;

/**
 * @brief 整文件 JSON 存储私有状态，互斥锁保护记录、元数据、修订号及写入过程。
 *
 * 加载保留完整记录数组，写入同时持有旧数据、事务数据及序列化缓冲。
 * 文件字节上限约束磁盘内容，峰值 RAM 还包括 JSON 树、回读校验和查询副本。
 */
typedef struct {
    char *file_path;
    cc_memory_entry_t *entries;
    size_t count;
    size_t cap;
    cc_mutex_t mutex;
    cc_filesystem_t fs;
    int owns_fs; /**< 默认文件系统归本对象销毁，外部注入的描述只借用。 */
    int write_indeterminate; /**< 恢复旁路、外部修改或安装结果不确定时置位，阻断写入，允许读取内存。 */
    int managed_recall_only;
    size_t summary_per_session;
    size_t summary_max_entries;
    size_t summary_max_value_bytes;
    cc_json_memory_metadata_t *metadata;
    size_t metadata_count;
    uint64_t revision; /**< 进程内乐观并发版本，初值一，确认安装或恢复后递增。 */
    uint64_t disk_fingerprint;
    size_t disk_size;
    int disk_present;
} cc_json_memory_store_t;

/**
 * @brief 计算文件正文的 64 位滚动指纹，供写入前核对是否发生外部修改。
 * @param[in] text 有效的借用 NUL 结尾正文，仅计算 NUL 前的字节。
 * @return 按无符号 64 位回绕计算的指纹，与文件字节数一同使用。
 */
static uint64_t memory_fingerprint(const char *text)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/**
 * @brief 在当前受管理元数据中按 key 线性查找。
 * @pre 调用者持有存储锁，或对象尚未发布给其他使用者。
 * @param[in] s 有效存储，借用其元数据数组。
 * @param[in] key 有效的借用主键。
 * @return 匹配项下标，未找到时返回 s->metadata_count。
 */
static size_t metadata_index(const cc_json_memory_store_t *s, const char *key)
{
    for (size_t i = 0; i < s->metadata_count; i++)
        if (strcmp(s->metadata[i].key, key) == 0) return i;
    return s->metadata_count;
}

/**
 * @brief 释放元数据数组及已初始化项拥有的字符串。
 * @param[in,out] items 待释放数组，仅当 count 为零时允许为 NULL。
 * @param[in] count 待清理的有效项数。
 * @post 项字符串和数组均已释放，调用者保存的指针值需自行清理。
 */
static void metadata_free_array(cc_json_memory_metadata_t *items, size_t count)
{
    for (size_t i = 0; i < count; i++) { free(items[i].key); free(items[i].evidence); }
    free(items);
}

/**
 * @brief 复制一条受管理元数据并扩展数组，所有分配成功后才增加计数。
 * @pre 存储尚未发布，或调用者已持存储锁，调用者负责 key 唯一性。
 * @param[in,out] s 接收元数据副本的有效存储。
 * @param[in] key 有效的借用主键，函数复制。
 * @param[in] evidence 可为 NULL 的借用证据文本，存在时复制。
 * @param[in] importance 已校验的重要度整数，加载入口限制在 0 至 100。
 * @return 成功为 CC_OK，数组长度溢出为 CC_ERR_LIMIT_EXCEEDED，分配失败为 CC_ERR_OUT_OF_MEMORY，失败保留原数组。
 */
static cc_result_t metadata_append(cc_json_memory_store_t *s, const char *key,
                                    const char *evidence, unsigned importance)
{
    if (s->metadata_count == SIZE_MAX || s->metadata_count + 1U > SIZE_MAX / sizeof(*s->metadata))
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory metadata capacity overflow");
    cc_json_memory_metadata_t item = {0};
    item.key = cc_copy_string(key);
    item.evidence = evidence ? cc_copy_string(evidence) : NULL;
    item.importance = importance;
    if (!item.key || (evidence && !item.evidence)) {
        free(item.key); free(item.evidence);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy memory metadata");
    }
    cc_json_memory_metadata_t *next = realloc(s->metadata, (s->metadata_count + 1U) * sizeof(*next));
    if (!next) { free(item.key); free(item.evidence); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot grow memory metadata"); }
    s->metadata = next;
    s->metadata[s->metadata_count++] = item;
    return cc_result_ok();
}

/**
 * @brief 拼接路径与旁路后缀。
 * @param[in] path 有效的借用路径字符串。
 * @param[in] suffix 有效的借用后缀字符串，直接追加。
 * @return 拥有的新路径，由调用者 free，长度溢出或分配失败返回 NULL。
 */
static char *path_with_suffix(const char *path, const char *suffix)
{
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    if (path_len > SIZE_MAX - suffix_len - 1U) return NULL;
    char *result = malloc(path_len + suffix_len + 1U);
    if (!result) return NULL;
    memcpy(result, path, path_len);
    memcpy(result + path_len, suffix, suffix_len + 1U);
    return result;
}

/**
 * @brief 从路径的最后一个正斜杠或反斜杠提取父目录，保留根路径和盘符根目录。
 * @param[in] path 有效的借用路径字符串。
 * @return 拥有的父目录字符串，由调用者 free，无分隔符时为 "."，分配失败为 NULL。
 */
static char *parent_directory(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *separator = slash;
    if (!separator || (backslash && backslash > separator)) separator = backslash;
    if (!separator) return cc_copy_string(".");

    size_t length = (size_t)(separator - path);
    if (length == 0U) length = 1U;
    if (length == 2U && path[1] == ':') length = 3U;
    char *parent = malloc(length + 1U);
    if (!parent) return NULL;
    memcpy(parent, path, length);
    parent[length] = '\0';
    return parent;
}

/**
 * @brief 文件替换后按文件系统能力同步父目录。
 * @param[in] s 借用的有效存储，调用者在写入路径中持有存储锁。
 * @return 无目录同步能力或同步成功为 CC_OK，路径分配失败为 CC_ERR_OUT_OF_MEMORY，sync_dir 错误原样返回。
 */
static cc_result_t sync_parent_directory(cc_json_memory_store_t *s)
{
    if (!(s->fs.capabilities & CC_FS_CAP_DIR_SYNC) || !s->fs.vtable->sync_dir) {
        return cc_result_ok();
    }
    char *parent = parent_directory(s->file_path);
    if (!parent) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate parent path");
    cc_result_t rc = s->fs.vtable->sync_dir(s->fs.self, parent);
    free(parent);
    return rc;
}

/**
 * @brief 按倍增策略扩展记录槽，保留已存在记录及字符串所有权。
 * @pre 调用者持存储锁，或对象尚未发布。
 * @param[in,out] s 待扩容的有效存储，仅成功后更新 entries 和 cap。
 * @param[in] needed 所需总记录槽数，单位为条。
 * @return 容量足够或扩容成功为 CC_OK，容量溢出为 CC_ERR_LIMIT_EXCEEDED，realloc 失败为 CC_ERR_OUT_OF_MEMORY。
 */
static cc_result_t reserve_entries(cc_json_memory_store_t *s, size_t needed)
{
    if (needed <= s->cap) return cc_result_ok();
    size_t new_cap = s->cap ? s->cap : 64U;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2U) {
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory store capacity overflow");
        }
        new_cap *= 2U;
    }
    if (new_cap > SIZE_MAX / sizeof(*s->entries)) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory store allocation overflow");
    }
    cc_memory_entry_t *resized = realloc(s->entries, new_cap * sizeof(*s->entries));
    if (!resized) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow memory store");
    s->entries = resized;
    s->cap = new_cap;
    return cc_result_ok();
}

/**
 * @brief 深拷贝记录字符串并复制时间字段。
 * @param[in] source 有效借用记录，key/value 必须存在，category/session_id 可为 NULL。
 * @param[out] destination 无已有资源的输出结构，进入后清零，成功副本由调用者 cc_memory_entry_free。
 * @return 成功为 CC_OK，任一分配失败为 CC_ERR_OUT_OF_MEMORY，失败清理部分副本并将输出清零。
 */
static cc_result_t copy_entry(const cc_memory_entry_t *source, cc_memory_entry_t *destination)
{
    memset(destination, 0, sizeof(*destination));
    destination->key = cc_copy_string(source->key);
    destination->value = cc_copy_string(source->value);
    destination->category = source->category ? cc_copy_string(source->category) : NULL;
    destination->session_id = source->session_id ? cc_copy_string(source->session_id) : NULL;
    destination->created_at = source->created_at;
    destination->updated_at = source->updated_at;
    if (!destination->key || !destination->value ||
        (source->category && !destination->category) ||
        (source->session_id && !destination->session_id)) {
        cc_memory_entry_free(destination);
        memset(destination, 0, sizeof(*destination));
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy memory entry");
    }
    return cc_result_ok();
}

/**
 * @brief 复制新记录字段，将创建和更新时间均设为当前 time(NULL)。
 * @param[in] key 有效借用主键。
 * @param[in] value 有效借用正文，可为空字符串。
 * @param[in] category 可为 NULL 的借用分类。
 * @param[in] session_id 可为 NULL 的借用来源会话。
 * @param[out] entry 无已有资源的输出结构，由 copy_entry 先清零，成功副本交调用者清理。
 * @return 成功为 CC_OK，复制失败为 CC_ERR_OUT_OF_MEMORY 且输出为空结构。
 */
static cc_result_t create_entry(
    const char *key,
    const char *value,
    const char *category,
    const char *session_id,
    cc_memory_entry_t *entry)
{
    cc_memory_entry_t source = {
        .key = (char *)key,
        .value = (char *)value,
        .category = (char *)category,
        .session_id = (char *)session_id,
        .created_at = time(NULL),
    };
    source.updated_at = source.created_at;
    return copy_entry(&source, entry);
}

/**
 * @brief 检查 .tmp 与 .bak 是否有任一目录项存在，保留中断替换的恢复证据。
 * @param[in] s 借用的有效存储，调用者持锁或对象尚未发布。
 * @param[out] out_present 进入后置零，发现任意非缺失项后置一，后续检查失败时可能已置一。
 * @return 两项检查完成为 CC_OK，路径分配失败为 CC_ERR_OUT_OF_MEMORY，stat 错误原样返回。
 */
static cc_result_t memory_recovery_files_present(cc_json_memory_store_t *s, int *out_present)
{
    const char *suffixes[] = {".tmp", ".bak"};
    *out_present = 0;
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        char *candidate = path_with_suffix(s->file_path, suffixes[i]);
        if (!candidate) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot inspect memory recovery path");
        cc_filesystem_stat_t info = {.size = sizeof(info)};
        cc_result_t rc = s->fs.vtable->stat(s->fs.self, candidate, &info);
        free(candidate);
        if (rc.code != CC_OK) return rc;
        if (info.type != CC_FS_ENTRY_MISSING) *out_present = 1;
    }
    return cc_result_ok();
}

/**
 * @brief 写入前要求旁路文件均不存在，发现旁路时将内存写入阻断标志置位。
 * @pre 调用者持有存储锁。
 * @param[in,out] s 有效存储，发现旁路时将 write_indeterminate 置一。
 * @return 无旁路为 CC_OK，存在旁路为 CC_ERR_STORAGE，检查错误原样返回并终止本次写入。
 */
static cc_result_t memory_require_clean_recovery_files(cc_json_memory_store_t *s)
{
    int present = 0;
    cc_result_t rc = memory_recovery_files_present(s, &present);
    if (rc.code != CC_OK) return rc;
    if (present) {
        s->write_indeterminate = 1;
        return cc_result_error(CC_ERR_STORAGE, "Memory recovery files require review before writing");
    }
    return cc_result_ok();
}

/**
 * @brief 向 JSON 对象添加借用正文的字符串节点，避免序列化前复制各字段正文。
 * @pre 借用正文受存储锁保护，生命周期覆盖 JSON 树的序列化与销毁。
 * @param[in,out] object 有效的 JSON 对象，成功后接管新字符串节点。
 * @param[in] key 有效借用字段名，cJSON 添加对象项时复制名称。
 * @param[in] value 有效借用正文，引用节点自身释放时不释放正文。
 * @return 添加成功为一，节点创建或添加失败为零，失败清理本次节点。
 */
static int add_borrowed_string(cJSON *object, const char *key, const char *value)
{
    cJSON *string = cJSON_CreateStringReference(value);
    if (!string) return 0;
    if (cJSON_AddItemToObject(object, key, string)) return 1;
    cJSON_Delete(string);
    return 0;
}

/**
 * @brief 对各字符串转义后的字节数做文件上限预检，提前拒绝必然超限的内容。
 * @param[in] s 持锁的有效存储，借用所有记录及元数据字符串。
 * @return 未启用上限或字符串总预算可容纳为一，超限为零。JSON 标点和数值开销由完整序列化后再检查。
 */
static int memory_strings_fit_file_limit(const cc_json_memory_store_t *s)
{
    if (!CC_JSON_MEMORY_MAX_FILE_BYTES) return 1;
    size_t remaining = CC_JSON_MEMORY_MAX_FILE_BYTES;
    for (size_t i = 0; i < s->count; i++) {
        size_t metadata = metadata_index(s, s->entries[i].key);
        const char *values[] = {s->entries[i].key, s->entries[i].value,
            s->entries[i].category, s->entries[i].session_id,
            metadata < s->metadata_count ? s->metadata[metadata].evidence : NULL};
        for (size_t j = 0; j < sizeof(values) / sizeof(values[0]); j++) {
            const unsigned char *text = (const unsigned char *)values[j];
            if (!text) continue;
            for (; *text; text++) {
                size_t bytes = 1;
                if (*text == '"' || *text == '\\' || *text == '\b' || *text == '\f' ||
                    *text == '\n' || *text == '\r' || *text == '\t') bytes = 2;
                else if (*text < 0x20U) bytes = 6;
                if (bytes > remaining) return 0;
                remaining -= bytes;
            }
        }
    }
    return 1;
}

/**
 * @brief 将当前内存集合经临时文件同步、回读校验和原子替换保存为主文件。
 * @pre 调用者持有存储锁，并负责根据安装结果保留新集合或恢复旧集合。
 * @param[in,out] s 有效存储，成功安装后更新磁盘指纹，安装不确定、外部修改或残余旁路会阻断后续写入。
 * @param[out] out_installed 进入后置零，替换成功或错误后回读确认新正文可见时置一，目录同步失败时仍保留一。
 * @return 全流程成功为 CC_OK，分配失败为 CC_ERR_OUT_OF_MEMORY，文件超限为 CC_ERR_LIMIT_EXCEEDED，
 * 回读、外部修改或旁路状态异常为 CC_ERR_STORAGE，文件系统错误原样返回。
 * @note 替换前失败或能确认原文件未变时清理本次 .tmp，清理后仍有旁路或检查失败会阻断写入。
 * 替换结果不确定时保留恢复证据。函数不更新 revision，调用者在成功或确认安装后递增。
 */
static cc_result_t save_to_file(cc_json_memory_store_t *s, int *out_installed)
{
    *out_installed = 0;
    cc_result_t clean_rc = memory_require_clean_recovery_files(s);
    if (clean_rc.code != CC_OK) return clean_rc;
    if (!memory_strings_fit_file_limit(s))
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED,
            "Memory content exceeds configured file byte limit; export or reduce memories before retrying");
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create memory JSON array");
    for (size_t i = 0; i < s->count; i++) {
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            cJSON_Delete(arr);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create memory JSON object");
        }
        int good = add_borrowed_string(obj, "key", s->entries[i].key) &&
            add_borrowed_string(obj, "value", s->entries[i].value) &&
            (!s->entries[i].category || add_borrowed_string(obj, "category", s->entries[i].category)) &&
            (!s->entries[i].session_id || add_borrowed_string(obj, "session_id", s->entries[i].session_id)) &&
            cJSON_AddNumberToObject(obj, "created_at", (double)s->entries[i].created_at) &&
            cJSON_AddNumberToObject(obj, "updated_at", (double)s->entries[i].updated_at);
        size_t metadata = metadata_index(s, s->entries[i].key);
        if (good && metadata < s->metadata_count) {
            good = cJSON_AddBoolToObject(obj, "managed", 1) &&
                cJSON_AddNumberToObject(obj, "importance", (double)s->metadata[metadata].importance) &&
                (!s->metadata[metadata].evidence ||
                 add_borrowed_string(obj, "evidence", s->metadata[metadata].evidence));
        }
        if (!good || !cJSON_AddItemToArray(arr, obj)) {
            cJSON_Delete(obj);
            cJSON_Delete(arr);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to construct complete memory entry");
        }
    }
    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!json_str) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to serialize memory store");
    if (CC_JSON_MEMORY_MAX_FILE_BYTES && strlen(json_str) > CC_JSON_MEMORY_MAX_FILE_BYTES) {
        cJSON_free(json_str);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED,
            "Memory file exceeds configured byte limit; export or reduce memories before retrying");
    }
    char *tmp = path_with_suffix(s->file_path, ".tmp");
    if (!tmp) {
        cJSON_free(json_str);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate memory store temp path");
    }
    cc_result_t rc = s->fs.vtable->write_text(s->fs.self, tmp, json_str);
    if (rc.code == CC_OK && s->fs.vtable->sync_file) {
        rc = s->fs.vtable->sync_file(s->fs.self, tmp);
    }
    if (rc.code == CC_OK) {
        char *verify = NULL;
        rc = s->fs.vtable->read_text(s->fs.self, tmp, &verify);
        if (rc.code == CC_OK && (!verify || strcmp(verify, json_str) != 0))
            rc = cc_result_error(CC_ERR_STORAGE, "Memory temporary file verification failed");
        free(verify);
    }
    cc_filesystem_stat_t before = {.size = sizeof(before)};
    char *old_visible = NULL;
    if (rc.code == CC_OK) {
        rc = s->fs.vtable->stat(s->fs.self, s->file_path, &before);
        if (rc.code == CC_OK && before.type != CC_FS_ENTRY_FILE && before.type != CC_FS_ENTRY_MISSING)
            rc = cc_result_error(CC_ERR_STORAGE, "Memory destination is not a file");
        if (rc.code == CC_OK && before.type == CC_FS_ENTRY_FILE)
            rc = s->fs.vtable->read_text(s->fs.self, s->file_path, &old_visible);
        if (rc.code == CC_OK &&
            ((before.type == CC_FS_ENTRY_MISSING && s->disk_present) ||
             (before.type == CC_FS_ENTRY_FILE && (!s->disk_present || !old_visible ||
              before.byte_size != s->disk_size || strlen(old_visible) != s->disk_size ||
              memory_fingerprint(old_visible) != s->disk_fingerprint)))) {
            s->write_indeterminate = 1;
            rc = cc_result_error(CC_ERR_STORAGE, "Memory file changed outside this store; reopen before writing");
        }
    }
    int attempted_replace = 0;
    int known_unchanged = 0;
    if (rc.code == CC_OK) {
        attempted_replace = 1;
        rc = s->fs.vtable->atomic_replace(s->fs.self, tmp, s->file_path);
        if (rc.code == CC_OK) *out_installed = 1;
        else {
            cc_filesystem_stat_t after = {.size = sizeof(after)}, temp_info = {.size = sizeof(temp_info)};
            char *visible = NULL;
            cc_result_t inspect_rc = s->fs.vtable->stat(s->fs.self, s->file_path, &after);
            cc_result_t temp_rc = s->fs.vtable->stat(s->fs.self, tmp, &temp_info);
            if (inspect_rc.code == CC_OK && after.type == CC_FS_ENTRY_FILE)
                inspect_rc = s->fs.vtable->read_text(s->fs.self, s->file_path, &visible);
            if (inspect_rc.code == CC_OK && visible && strcmp(visible, json_str) == 0) {
                *out_installed = 1;
            } else if (inspect_rc.code == CC_OK && temp_rc.code == CC_OK &&
                       temp_info.type == CC_FS_ENTRY_FILE &&
                       ((before.type == CC_FS_ENTRY_MISSING && after.type == CC_FS_ENTRY_MISSING) ||
                        (before.type == CC_FS_ENTRY_FILE && after.type == CC_FS_ENTRY_FILE &&
                         old_visible && visible && strcmp(old_visible, visible) == 0))) {
                known_unchanged = 1;
            }
            cc_result_free(&inspect_rc);
            cc_result_free(&temp_rc);
            free(visible);
        }
    }
    if (rc.code == CC_OK) rc = sync_parent_directory(s);
    if (rc.code == CC_OK) {
        /* FAT 替换可能已安装主文件却遗留备份，保留已安装标志并要求核对旁路文件。 */
        rc = memory_require_clean_recovery_files(s);
        if (rc.code != CC_OK) s->write_indeterminate = 1;
    }
    if (*out_installed) {
        s->disk_present = 1;
        s->disk_size = strlen(json_str);
        s->disk_fingerprint = memory_fingerprint(json_str);
    }
    if (rc.code != CC_OK && attempted_replace && !known_unchanged) s->write_indeterminate = 1;
    if (rc.code != CC_OK && (!attempted_replace || known_unchanged)) {
        if (s->fs.vtable->remove) {
            cc_result_t cleanup = s->fs.vtable->remove(s->fs.self, tmp);
            cc_result_free(&cleanup);
        }
        int remaining = 0;
        cc_result_t inspect = memory_recovery_files_present(s, &remaining);
        if (inspect.code != CC_OK || remaining) s->write_indeterminate = 1;
        cc_result_free(&inspect);
    }
    free(tmp);
    free(old_visible);
    cJSON_free(json_str);
    return rc;
}

/**
 * @brief 读取可选的非负整数时间戳并检查 time_t 可容纳的范围。
 * @param[in] object 借用的记录 JSON 对象。
 * @param[in] key 借用的时间戳字段名。
 * @param[out] out 进入后置零，字段缺失时保留零，合法字段转换为以秒计的 time_t。
 * @return 缺失或合法为 CC_OK，字段类型、范围或整数性非法为 CC_ERR_STORAGE。
 */
static cc_result_t memory_parse_timestamp(const cc_json_value_t *object, const char *key, time_t *out)
{
    *out = 0;
    cc_json_value_t *field = cc_json_object_get(object, key);
    if (!field) return cc_result_ok();
    if (!cc_json_is_number(field)) return cc_result_error(CC_ERR_STORAGE, "Memory timestamp is invalid");
    double value = cc_json_number_value(field);
    double maximum = sizeof(time_t) >= 8U ? 253402300799.0 : 2147483647.0;
    if (!(value >= 0 && value <= maximum) || value != (double)(uint64_t)value)
        return cc_result_error(CC_ERR_STORAGE, "Memory timestamp is out of range");
    *out = (time_t)(uint64_t)value;
    return cc_result_ok();
}

/**
 * @brief 完整消费主文件 JSON 后加载记录与受管理元数据，检查旁路、NUL 转义、重复主键及字段类型。
 * @pre s 为尚未发布的空存储，失败后调用者销毁已加载的部分内容。
 * @param[in,out] s 拥有加载副本的有效存储，合法主文件伴随旁路时仍可加载并设置写入阻断。
 * @return 主文件缺失且无旁路或完整加载为 CC_OK，损坏、NUL 转义、截断、重复主键及恢复状态异常为 CC_ERR_STORAGE，
 * 文件超限为 CC_ERR_LIMIT_EXCEEDED，资源、JSON 解析和文件系统错误原样返回。
 * @note 缺失或为零的 created_at 替换为当前时间，缺失或为零的 updated_at 使用 created_at。
 * 加载按整文件正文和 JSON 树分配内存，失败始终保留原磁盘文件。
 */
static cc_result_t load_from_file(cc_json_memory_store_t *s)
{
    cc_filesystem_stat_t info = {.size = sizeof(info)};
    cc_result_t rc = s->fs.vtable->stat(s->fs.self, s->file_path, &info);
    if (rc.code != CC_OK) return rc;
    int recovery_present = 0;
    rc = memory_recovery_files_present(s, &recovery_present);
    if (rc.code != CC_OK) return rc;
    if (info.type == CC_FS_ENTRY_MISSING) {
        if (recovery_present)
            return cc_result_error(CC_ERR_STORAGE, "Memory primary is missing and recovery files require review");
        return cc_result_ok();
    }
    if (info.type != CC_FS_ENTRY_FILE || info.byte_size > SIZE_MAX)
        return cc_result_error(CC_ERR_STORAGE, "Memory store path is invalid");
    if (CC_JSON_MEMORY_MAX_FILE_BYTES && info.byte_size > CC_JSON_MEMORY_MAX_FILE_BYTES)
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED,
            "Memory file exceeds configured byte limit; export or reduce the file before reloading");
    if (recovery_present) s->write_indeterminate = 1;
    char *content = NULL;
    rc = s->fs.vtable->read_text(s->fs.self, s->file_path, &content);
    if (rc.code != CC_OK) return rc;
    if (!content || strlen(content) != (size_t)info.byte_size) {
        free(content);
        return cc_result_error(CC_ERR_STORAGE, "Memory store is truncated or contains embedded NUL");
    }
    if (cc_storage_json_has_nul_escape(content)) {
        free(content);
        return cc_result_error(CC_ERR_STORAGE, "Memory store contains encoded NUL");
    }
    s->disk_present = 1;
    s->disk_size = (size_t)info.byte_size;
    s->disk_fingerprint = memory_fingerprint(content);
    cc_json_value_t *arr = NULL;
    rc = cc_json_parse_strict(content, &arr);
    free(content);
    if (rc.code != CC_OK) return rc;
    if (!cc_json_is_array(arr)) {
        cc_json_destroy(arr);
        return cc_result_error(CC_ERR_STORAGE, "Memory store must be a JSON array");
    }
    size_t len = (size_t)cc_json_array_size(arr);
    rc = reserve_entries(s, len);
    for (size_t i = 0; rc.code == CC_OK && i < len; i++) {
        cc_json_value_t *obj = cc_json_array_get(arr, i);
        cc_json_value_t *cat_value = cc_json_object_get(obj, "category");
        cc_json_value_t *sid_value = cc_json_object_get(obj, "session_id");
        cc_json_value_t *managed = cc_json_object_get(obj, "managed");
        cc_json_value_t *importance = cc_json_object_get(obj, "importance");
        cc_json_value_t *evidence_value = cc_json_object_get(obj, "evidence");
        const char *key = cc_json_string_value(cc_json_object_get(obj, "key"));
        const char *value = cc_json_string_value(cc_json_object_get(obj, "value"));
        const char *cat = cc_json_string_value(cat_value);
        const char *sid = cc_json_string_value(sid_value);
        if (!cc_json_is_object(obj) || !key || !key[0] || !value ||
            (cat_value && !cat) || (sid_value && !sid) ||
            (managed && !cc_json_is_bool(managed))) {
            rc = cc_result_error(CC_ERR_STORAGE, "Memory store contains an invalid entry");
            break;
        }
        for (size_t j = 0; j < s->count; j++) {
            if (strcmp(s->entries[j].key, key) == 0) {
                rc = cc_result_error(CC_ERR_STORAGE, "Memory store contains duplicate keys");
                break;
            }
        }
        if (rc.code != CC_OK) break;
        cc_memory_entry_t source = {
            .key = (char *)key, .value = (char *)value, .category = (char *)cat,
            .session_id = (char *)sid,
        };
        rc = memory_parse_timestamp(obj, "created_at", &source.created_at);
        if (rc.code == CC_OK) rc = memory_parse_timestamp(obj, "updated_at", &source.updated_at);
        if (rc.code != CC_OK) break;
        cc_memory_entry_t *entry = &s->entries[s->count];
        rc = copy_entry(&source, entry);
        if (rc.code != CC_OK) break;
        if (!entry->created_at) entry->created_at = time(NULL);
        if (!entry->updated_at) entry->updated_at = entry->created_at;
        s->count++;
        if (managed && cc_json_bool_value(managed)) {
            double score = cc_json_number_value(importance);
            const char *evidence = cc_json_string_value(evidence_value);
            if (!cc_json_is_number(importance) || !(score >= 0 && score <= 100) ||
                score != (double)(unsigned)score || (evidence_value && !evidence) ||
                !cat || !cat[0] || !value[0]) {
                rc = cc_result_error(CC_ERR_STORAGE, "Managed memory metadata is invalid");
                break;
            }
            rc = metadata_append(s, key, evidence, (unsigned)score);
        }
    }
    cc_json_destroy(arr);
    return rc;
}

/**
 * @brief 判断记录是否为未受管理的 context_summary 派生缓存。
 * @param[in] s 借用的有效存储，调用者持锁以稳定元数据数组。
 * @param[in] entry 借用的有效记录，key 必须存在，category 可为 NULL。
 * @return 分类为 context_summary 且无受管理元数据时为一，其他情况为零。
 */
static int is_derived_summary(const cc_json_memory_store_t *s, const cc_memory_entry_t *entry)
{
    return entry->category && strcmp(entry->category, "context_summary") == 0 &&
        metadata_index(s, entry->key) == s->metadata_count;
}

/**
 * @brief 比较摘要会话分组，两个 NULL 归为同组。
 * @param[in] a 第一个借用会话字符串，可为 NULL。
 * @param[in] b 第二个借用会话字符串，可为 NULL。
 * @return 均为 NULL 或字符串逐字节相同为一，其余为零，NULL 与空字符串分别成组。
 */
static int same_summary_session(const char *a, const char *b)
{
    return (!a && !b) || (a && b && strcmp(a, b) == 0);
}

/**
 * @brief 在缓存限制内暂存摘要插入或裁剪，并通过一次整文件替换提交。
 * @pre 调用者持存储锁，三个摘要限制均已配置为正数。
 * @param[in,out] s 有效存储，成功或确认安装后采用新数组并递增 revision，其他失败恢复旧数组。
 * @param[in] key 要新增或替换的借用主键，NULL 表示仅裁剪已有摘要。
 * @param[in] value key 非 NULL 时为有效的借用摘要正文，仅裁剪时忽略。
 * @param[in] session_id 可为 NULL 的借用会话，只有新增或替换时使用，同 key 的会话必须一致。
 * @return 成功或裁剪无需变更为 CC_OK，同 key 属于其他记录或会话为 CC_ERR_PERMISSION_DENIED，
 * 容量、正文预算或版本溢出为 CC_ERR_LIMIT_EXCEEDED，分配失败为 CC_ERR_OUT_OF_MEMORY，写入阻断为 CC_ERR_STORAGE，保存错误原样返回。
 * @note 从数组尾部向前选择满足会话条数、总条数及正文总字节限制的派生摘要，其他记录全部保留。
 * 暂存数组借用旧字符串，只有新摘要单独分配，确定安装后才释放被删除字符串。
 */
static cc_result_t bounded_summaries_locked(cc_json_memory_store_t *s,
    const char *key, const char *value, const char *session_id)
{
    int adding = key != NULL;
    if (adding && strlen(value) > s->summary_max_value_bytes)
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Context summary exceeds cache byte limit");
    if (!adding && !s->count) return cc_result_ok();
    if (s->count == SIZE_MAX || s->count + 1U > SIZE_MAX / sizeof(cc_memory_entry_t))
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Context summary capacity overflow");

    size_t capacity = s->count + 1U;
    if (capacity < 64U) capacity = 64U;
    cc_memory_entry_t *next = calloc(capacity, sizeof(*next));
    cc_memory_entry_t *removed = calloc(s->count + 1U, sizeof(*removed));
    unsigned char *keep = calloc(s->count + 1U, sizeof(*keep));
    size_t kept_capacity = s->count + (adding ? 1U : 0U);
    if (kept_capacity > s->summary_max_entries) kept_capacity = s->summary_max_entries;
    size_t *kept = calloc(kept_capacity ? kept_capacity : 1U, sizeof(*kept));
    if (!next || !removed || !keep || !kept) {
        free(next); free(removed); free(keep); free(kept);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate context summary transaction");
    }

    size_t count = 0, removed_count = 0;
    time_t old_created_at = 0;
    int replaced = 0;
    for (size_t i = 0; i < s->count; i++) {
        if (adding && strcmp(s->entries[i].key, key) == 0) {
            if (!is_derived_summary(s, &s->entries[i]) ||
                !same_summary_session(s->entries[i].session_id, session_id)) {
                free(next); free(removed); free(keep); free(kept);
                return cc_result_error(CC_ERR_PERMISSION_DENIED, "Context summary key is already in use");
            }
            old_created_at = s->entries[i].created_at;
            replaced = 1;
            removed[removed_count++] = s->entries[i];
        } else {
            next[count++] = s->entries[i];
        }
    }
    cc_memory_entry_t inserted = {0};
    if (adding) {
        cc_result_t rc = create_entry(key, value, "context_summary", session_id, &inserted);
        if (rc.code != CC_OK) { free(next); free(removed); free(keep); free(kept); return rc; }
        cc_result_free(&rc);
        if (replaced) inserted.created_at = old_created_at;
        next[count++] = inserted;
    }

    size_t retained_count = 0, retained_bytes = 0;
    for (size_t cursor = count; cursor > 0; cursor--) {
        size_t i = cursor - 1U;
        if (!is_derived_summary(s, &next[i])) { keep[i] = 1; continue; }
        size_t session_count = 0;
        for (size_t j = 0; j < retained_count; j++)
            if (same_summary_session(next[i].session_id, next[kept[j]].session_id)) session_count++;
        size_t bytes = strlen(next[i].value);
        if (session_count < s->summary_per_session && retained_count < s->summary_max_entries &&
            bytes <= s->summary_max_value_bytes - retained_bytes) {
            keep[i] = 1;
            kept[retained_count++] = i;
            retained_bytes += bytes;
        }
    }
    size_t final_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (keep[i]) next[final_count++] = next[i];
        else removed[removed_count++] = next[i];
    }
    free(keep); free(kept);
    if (!adding && !removed_count) { free(next); free(removed); return cc_result_ok(); }
    if (s->write_indeterminate || s->revision == UINT64_MAX) {
        if (adding) cc_memory_entry_free(&inserted);
        free(next); free(removed);
        return s->write_indeterminate ?
            cc_result_error(CC_ERR_STORAGE, "Memory storage needs recovery before further writes") :
            cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory revision overflow");
    }

    cc_memory_entry_t *old = s->entries;
    size_t old_count = s->count, old_capacity = s->cap;
    s->entries = next; s->count = final_count; s->cap = capacity;
    int installed = 0;
    cc_result_t rc = save_to_file(s, &installed);
    if (rc.code == CC_OK || installed) {
        s->revision++;
        for (size_t i = 0; i < removed_count; i++) cc_memory_entry_free(&removed[i]);
        free(old);
    } else {
        s->entries = old; s->count = old_count; s->cap = old_capacity;
        if (adding) cc_memory_entry_free(&inserted);
        free(next);
    }
    free(removed);
    return rc;
}

/**
 * @brief 在存储锁内新增或更新普通记录，启用摘要限制后将 context_summary 交给有界缓存事务。
 * @param[in,out] self 有效的私有存储，由公共包装校验入口，操作期间借用。
 * @param[in] key 非空的借用主键，已有受管理主键拒绝由此入口更新。
 * @param[in] value 有效借用正文，复制后写入，允许空字符串。
 * @param[in] category 可为 NULL 的借用分类，更新时替换旧分类。
 * @param[in] session_id 可为 NULL 的借用来源会话，普通记录更新时保留初次创建的会话。
 * @return 完整保存为 CC_OK，NULL 或空主键为 CC_ERR_INVALID_ARGUMENT，受管理主键为 CC_ERR_PERMISSION_DENIED，写入阻断为 CC_ERR_STORAGE，
 * 容量或修订号溢出为 CC_ERR_LIMIT_EXCEEDED，分配失败为 CC_ERR_OUT_OF_MEMORY，摘要事务及保存错误原样返回。
 * @note 确认安装时保留新内存数据并递增 revision，即使返回同步错误。未确认安装时恢复旧内存数据。
 */
static cc_result_t json_set(void *self, const char *key, const char *value,
                             const char *category, const char *session_id)
{
    if (!key || !key[0])
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Memory key must not be empty");
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    cc_mutex_lock(s->mutex);
    if (s->write_indeterminate) {
        cc_mutex_unlock(s->mutex);
        return cc_result_error(CC_ERR_STORAGE, "Memory storage needs recovery before further writes");
    }
    if (s->revision == UINT64_MAX) {
        cc_mutex_unlock(s->mutex);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory revision overflow");
    }

    if (s->summary_per_session && category && strcmp(category, "context_summary") == 0) {
        cc_result_t rc = bounded_summaries_locked(s, key, value, session_id);
        cc_mutex_unlock(s->mutex);
        return rc;
    }

    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].key, key) == 0) {
            if (metadata_index(s, key) < s->metadata_count) {
                cc_mutex_unlock(s->mutex);
                return cc_result_error(CC_ERR_PERMISSION_DENIED, "Use managed memory editing for this entry");
            }
            char *new_value = cc_copy_string(value);
            char *new_category = category ? cc_copy_string(category) : NULL;
            if (!new_value || (category && !new_category)) {
                free(new_value);
                free(new_category);
                cc_mutex_unlock(s->mutex);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to update memory entry");
            }
            char *old_value = s->entries[i].value;
            char *old_category = s->entries[i].category;
            time_t old_updated_at = s->entries[i].updated_at;
            s->entries[i].value = new_value;
            s->entries[i].category = new_category;
            s->entries[i].updated_at = time(NULL);
            int installed = 0;
            cc_result_t save_rc = save_to_file(s, &installed);
            if (save_rc.code == CC_OK || installed) {
                s->revision++;
                free(old_value);
                free(old_category);
            } else {
                s->entries[i].value = old_value;
                s->entries[i].category = old_category;
                s->entries[i].updated_at = old_updated_at;
                free(new_value);
                free(new_category);
            }
            cc_mutex_unlock(s->mutex);
            return save_rc;
        }
    }

    cc_result_t reserve_rc = reserve_entries(s, s->count + 1U);
    if (reserve_rc.code != CC_OK) {
        cc_mutex_unlock(s->mutex);
        return reserve_rc;
    }
    cc_result_free(&reserve_rc);

    cc_memory_entry_t *e = &s->entries[s->count];
    cc_result_t create_rc = create_entry(key, value, category, session_id, e);
    if (create_rc.code != CC_OK) {
        cc_mutex_unlock(s->mutex);
        return create_rc;
    }
    cc_result_free(&create_rc);
    s->count++;

    int installed = 0;
    cc_result_t save_rc = save_to_file(s, &installed);
    if (save_rc.code != CC_OK && !installed) {
        s->count--;
        cc_memory_entry_free(e);
        memset(e, 0, sizeof(*e));
    }
    if (save_rc.code == CC_OK || installed) s->revision++;
    cc_mutex_unlock(s->mutex);
    return save_rc;
}

/**
 * @brief 在存储锁内按精确 key 复制记录，允许读取普通记录、受管理记录及摘要。
 * @param[in] self 有效的借用私有存储，公共包装保证参数存在。
 * @param[in] key 有效的借用主键，按字节比较。
 * @param[out] out_entry 无已有资源的输出结构，找到记录后由 copy_entry 清零，成功副本由调用者 cc_memory_entry_free。
 * @return 找到并复制为 CC_OK，未找到为 CC_ERR_NOT_FOUND 且输出保持原值，复制失败为 CC_ERR_OUT_OF_MEMORY 且输出清零。
 */
static cc_result_t json_get(void *self, const char *key, cc_memory_entry_t *out_entry)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].key, key) == 0) {
            cc_result_t copy_rc = copy_entry(&s->entries[i], out_entry);
            cc_mutex_unlock(s->mutex);
            return copy_rc;
        }
    }

    cc_mutex_unlock(s->mutex);
    return cc_result_errf(CC_ERR_NOT_FOUND, "Memory key not found: %s", key);
}

/**
 * @brief 按字节扫描子串，仅对 ASCII 大小写折叠。
 * @param[in] haystack 借用的待检索字符串，可为 NULL。
 * @param[in] needle 借用的检索词，可为 NULL 或空字符串。
 * @return 找到非空检索词为一，空检索词、NULL 参数或无匹配为零。
 */
static int match_query(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !needle[0]) return 0;
    for (const unsigned char *start = (const unsigned char *)haystack;
         *start; start++) {
        size_t i = 0;
        while (needle[i] && start[i]) {
            unsigned char a = start[i];
            unsigned char b = (unsigned char)needle[i];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
            if (a != b) break;
            i++;
        }
        if (!needle[i]) return 1;
    }
    return 0;
}

/**
 * @brief 在锁内按数组正序扫描 key、value、category 的子串匹配。
 * @param[in] self 有效的借用私有存储，公共包装保证参数存在。
 * @param[in] query 有效的借用检索词，空字符串无匹配。
 * @param[in] limit 结果条数上限，小于等于零表示不限。
 * @param[out] out_entries 成功后取得拥有的深拷贝数组，使用 cc_memory_entry_free_array 释放，失败保持原值。
 * @param[out] out_count 仅成功时填写条数，零条时仍可能返回已分配数组。
 * @return 成功为 CC_OK，分配或复制失败为 CC_ERR_OUT_OF_MEMORY，结果容量溢出为 CC_ERR_LIMIT_EXCEEDED。
 * @note managed_recall_only 启用后跳过所有未受管理记录，筛选与复制均在存储锁内进行。
 */
static cc_result_t json_search(void *self, const char *query, int limit,
                                cc_memory_entry_t **out_entries, size_t *out_count)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    size_t cap = 16;
    cc_memory_entry_t *results = malloc(cap * sizeof(cc_memory_entry_t));
    if (!results) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }

    size_t count = 0;
    for (size_t i = 0; i < s->count && (limit <= 0 || count < (size_t)limit); i++) {
        cc_memory_entry_t *e = &s->entries[i];
        if (s->managed_recall_only && metadata_index(s,e->key)==s->metadata_count) continue;
        if (match_query(e->key, query) || match_query(e->value, query) || match_query(e->category, query)) {
            if (count >= cap) {
                if (cap > SIZE_MAX / 2U || cap * 2U > SIZE_MAX / sizeof(*results)) {
                    cc_memory_entry_free_array(results, count);
                    cc_mutex_unlock(s->mutex);
                    return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory search result overflow");
                }
                size_t next_cap = cap * 2U;
                cc_memory_entry_t *resized = realloc(results, next_cap * sizeof(*results));
                if (!resized) {
                    cc_memory_entry_free_array(results, count);
                    cc_mutex_unlock(s->mutex);
                    return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow search results");
                }
                results = resized;
                cap = next_cap;
            }
            cc_result_t copy_rc = copy_entry(e, &results[count]);
            if (copy_rc.code != CC_OK) {
                cc_memory_entry_free_array(results, count);
                cc_mutex_unlock(s->mutex);
                return copy_rc;
            }
            cc_result_free(&copy_rc);
            count++;
        }
    }

    cc_mutex_unlock(s->mutex);
    *out_entries = results;
    *out_count = count;
    return cc_result_ok();
}

/**
 * @brief 核对结构化查询的分类、排除分类、会话与正文条件。
 * @param[in] entry 借用的候选记录，可为 NULL。
 * @param[in] query 借用的查询，可为 NULL，空分类或会话不限定相应字段，空检索词匹配所有剩余候选。
 * @return 所有条件满足为一，NULL 参数或任一条件不匹配为零。
 */
static int json_query_matches(
    const cc_memory_entry_t *entry,
    const cc_memory_query_t *query)
{
    if (!entry || !query) return 0;
    if (query->category && query->category[0] &&
        (!entry->category || strcmp(entry->category, query->category) != 0)) {
        return 0;
    }
    if (cc_memory_query_excludes_category(query, entry->category)) return 0;
    if (query->session_id && query->session_id[0] &&
        (!entry->session_id || strcmp(entry->session_id, query->session_id) != 0)) {
        return 0;
    }
    if (!query->query || !query->query[0]) return 1;
    return match_query(entry->key, query->query) ||
           match_query(entry->value, query->query) ||
           match_query(entry->category, query->query);
}

/**
 * @brief 根据最先匹配的字段赋予固定相关度分值。
 * @param[in] entry 有效的借用候选记录。
 * @param[in] query 借用检索词，可为 NULL 或空字符串。
 * @return 空词或 key 匹配为 1.0，value 匹配为 0.8，category 匹配为 0.6，均不匹配为 0.0。
 */
static double json_query_score(
    const cc_memory_entry_t *entry,
    const char *query)
{
    if (!query || !query[0]) return 1.0;
    if (match_query(entry->key, query)) return 1.0;
    if (match_query(entry->value, query)) return 0.8;
    if (match_query(entry->category, query)) return 0.6;
    return 0.0;
}

/**
 * @brief 在锁内按数组倒序筛选记录，再截取 limit 条并复制为带分值结果。
 * @param[in] self 有效的借用私有存储，公共入口负责对象生命周期。
 * @param[in] query 必需的借用查询，limit 小于等于零表示不限条数。
 * @param[out] out_results 必需输出，三个显式参数合法后置 NULL，成功数组由调用者 cc_memory_search_result_free_array 释放。
 * @param[out] out_count 必需输出，参数合法后置零，成功时填写条数，零条仍可能取得已分配数组。
 * @return 成功为 CC_OK，NULL 查询或输出为 CC_ERR_INVALID_ARGUMENT，分配失败为 CC_ERR_OUT_OF_MEMORY，容量溢出为 CC_ERR_LIMIT_EXCEEDED。
 * @note 先应用分类和会话过滤，再截断条数。分值只随结果返回，遍历顺序按数组位置决定。
 * 启用受管理过滤后，只有显式 category=context_summary 查询允许召回未受管理摘要。
 */
static cc_result_t json_query(
    void *self,
    const cc_memory_query_t *query,
    cc_memory_search_result_t **out_results,
    size_t *out_count)
{
    if (!query || !out_results || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid JSON memory query");
    }
    *out_results = NULL;
    *out_count = 0;
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    cc_mutex_lock(s->mutex);
    size_t capacity = query->limit > 0 && query->limit < 16 ?
        (size_t)query->limit : 16U;
    if (capacity == 0) capacity = 1;
    cc_memory_search_result_t *results = calloc(capacity, sizeof(*results));
    if (!results) {
        cc_mutex_unlock(s->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate memory query results");
    }
    size_t count = 0;
    int summary_query=query->category && strcmp(query->category,"context_summary")==0;
    /* 从数组尾部向前扫描，新追加项优先，普通记录原位更新不会调整其位置。 */
    for (size_t cursor = s->count; cursor > 0 &&
         (query->limit <= 0 || count < (size_t)query->limit); cursor--) {
        size_t i = cursor - 1U;
        if (s->managed_recall_only && !summary_query &&
            metadata_index(s,s->entries[i].key)==s->metadata_count) continue;
        if (!json_query_matches(&s->entries[i], query)) continue;
        if (count == capacity) {
            if (capacity > SIZE_MAX / 2U ||
                capacity * 2U > SIZE_MAX / sizeof(*results)) {
                cc_memory_search_result_free_array(results, count);
                cc_mutex_unlock(s->mutex);
                return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory query result overflow");
            }
            size_t next_capacity = capacity * 2U;
            cc_memory_search_result_t *resized =
                realloc(results, next_capacity * sizeof(*results));
            if (!resized) {
                cc_memory_search_result_free_array(results, count);
                cc_mutex_unlock(s->mutex);
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow memory query results");
            }
            memset(resized + capacity, 0,
                   (next_capacity - capacity) * sizeof(*resized));
            results = resized;
            capacity = next_capacity;
        }
        cc_result_t rc = copy_entry(&s->entries[i], &results[count].entry);
        if (rc.code != CC_OK) {
            cc_memory_search_result_free_array(results, count);
            cc_mutex_unlock(s->mutex);
            return rc;
        }
        results[count].size = sizeof(results[count]);
        results[count].score = json_query_score(&s->entries[i], query->query);
        count++;
    }
    cc_mutex_unlock(s->mutex);
    *out_results = results;
    *out_count = count;
    return cc_result_ok();
}

/**
 * @brief 在锁内按数组正序列出分类匹配的记录，受管理过滤启用时只返回受管理项。
 * @param[in] self 有效的借用私有存储，公共包装保证对象及输出有效。
 * @param[in] category 借用分类，NULL 或空字符串表示所有分类。
 * @param[in] limit 结果条数上限，小于等于零表示不限。
 * @param[out] out_entries 仅成功时写入拥有的深拷贝数组，调用者 cc_memory_entry_free_array，零条仍可能有数组。
 * @param[out] out_count 仅成功时写入结果条数，失败保持原值。
 * @return 成功为 CC_OK，分配或复制失败为 CC_ERR_OUT_OF_MEMORY，容量溢出为 CC_ERR_LIMIT_EXCEEDED。
 */
static cc_result_t json_list(void *self, const char *category, int limit,
                              cc_memory_entry_t **out_entries, size_t *out_count)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    cc_mutex_lock(s->mutex);

    size_t cap = 16;
    cc_memory_entry_t *results = malloc(cap * sizeof(cc_memory_entry_t));
    if (!results) { cc_mutex_unlock(s->mutex); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM"); }

    size_t count = 0;
    for (size_t i = 0; i < s->count && (limit <= 0 || count < (size_t)limit); i++) {
        cc_memory_entry_t *e = &s->entries[i];
        if (s->managed_recall_only && metadata_index(s,e->key)==s->metadata_count) continue;
        if (!category || !category[0] || (e->category && strcmp(e->category, category) == 0)) {
            if (count >= cap) {
                if (cap > SIZE_MAX / 2U || cap * 2U > SIZE_MAX / sizeof(*results)) {
                    cc_memory_entry_free_array(results, count);
                    cc_mutex_unlock(s->mutex);
                    return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory list result overflow");
                }
                size_t next_cap = cap * 2U;
                cc_memory_entry_t *resized = realloc(results, next_cap * sizeof(*results));
                if (!resized) {
                    cc_memory_entry_free_array(results, count);
                    cc_mutex_unlock(s->mutex);
                    return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow list results");
                }
                results = resized;
                cap = next_cap;
            }
            cc_result_t copy_rc = copy_entry(e, &results[count]);
            if (copy_rc.code != CC_OK) {
                cc_memory_entry_free_array(results, count);
                cc_mutex_unlock(s->mutex);
                return copy_rc;
            }
            cc_result_free(&copy_rc);
            count++;
        }
    }

    cc_mutex_unlock(s->mutex);
    *out_entries = results;
    *out_count = count;
    return cc_result_ok();
}

/**
 * @brief 在锁内删除一个普通记录并提交整文件，受管理记录须通过专用编辑入口处理。
 * @param[in,out] self 有效的借用私有存储，确认安装后递增修订号并释放被删项。
 * @param[in] key 有效的借用主键，由公共包装保证存在。
 * @return 成功为 CC_OK，主键缺失为 CC_ERR_NOT_FOUND，受管理主键为 CC_ERR_PERMISSION_DENIED，
 * 写入阻断为 CC_ERR_STORAGE，修订号溢出为 CC_ERR_LIMIT_EXCEEDED，保存错误原样返回。
 * @note 先移出目标并压缩数组，未确认安装时恢复原位置和计数，已安装后同步失败仍保留删除结果。
 */
static cc_result_t json_delete_entry(void *self, const char *key)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    cc_mutex_lock(s->mutex);
    if (s->write_indeterminate) {
        cc_mutex_unlock(s->mutex);
        return cc_result_error(CC_ERR_STORAGE, "Memory storage needs recovery before further writes");
    }
    if (s->revision == UINT64_MAX) {
        cc_mutex_unlock(s->mutex);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory revision overflow");
    }

    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].key, key) == 0) {
            if (metadata_index(s, key) < s->metadata_count) {
                cc_mutex_unlock(s->mutex);
                return cc_result_error(CC_ERR_PERMISSION_DENIED, "Use managed memory editing for this entry");
            }
            cc_memory_entry_t removed = s->entries[i];
            if (i < s->count - 1)
                memmove(&s->entries[i], &s->entries[i + 1], (s->count - i - 1) * sizeof(cc_memory_entry_t));
            s->count--;
            int installed = 0;
            cc_result_t save_rc = save_to_file(s, &installed);
            if (save_rc.code == CC_OK || installed) {
                s->revision++;
                cc_memory_entry_free(&removed);
            } else {
                if (i < s->count) {
                    memmove(&s->entries[i + 1], &s->entries[i], (s->count - i) * sizeof(cc_memory_entry_t));
                }
                s->entries[i] = removed;
                s->count++;
            }
            cc_mutex_unlock(s->mutex);
            return save_rc;
        }
    }

    cc_mutex_unlock(s->mutex);
    return cc_result_error(CC_ERR_NOT_FOUND, "Memory key not found");
}

/**
 * @brief 在锁内暂存指定分类之外的记录，再以一次整文件替换提交删除。
 * @param[in,out] self 有效的借用私有存储，安装确认前保留旧数组及字符串。
 * @param[in] category 有效的借用分类，按字节匹配，公共包装允许空字符串。
 * @return 成功为 CC_OK，分类中含任一受管理项为 CC_ERR_PERMISSION_DENIED，写入阻断为 CC_ERR_STORAGE，
 * 数组或修订号溢出为 CC_ERR_LIMIT_EXCEEDED，分配失败为 CC_ERR_OUT_OF_MEMORY，保存错误原样返回。
 * @note 确认安装后释放被删记录并递增修订号，未确认安装时恢复旧数组。无匹配记录时仍尝试保存文件。
 */
static cc_result_t json_delete_by_category(void *self, const char *category)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    cc_mutex_lock(s->mutex);
    if (s->write_indeterminate) {
        cc_mutex_unlock(s->mutex);
        return cc_result_error(CC_ERR_STORAGE, "Memory storage needs recovery before further writes");
    }
    if (s->revision == UINT64_MAX) {
        cc_mutex_unlock(s->mutex);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory revision overflow");
    }
    for (size_t i = 0; i < s->count; i++) {
        if (s->entries[i].category && strcmp(s->entries[i].category, category) == 0 &&
            metadata_index(s, s->entries[i].key) < s->metadata_count) {
            cc_mutex_unlock(s->mutex);
            return cc_result_error(CC_ERR_PERMISSION_DENIED, "Use managed memory editing for this category");
        }
    }

    if (s->cap > SIZE_MAX / sizeof(*s->entries)) {
        cc_mutex_unlock(s->mutex);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory store allocation overflow");
    }
    cc_memory_entry_t *survivors = malloc(s->cap * sizeof(*survivors));
    if (!survivors) {
        cc_mutex_unlock(s->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to stage category deletion");
    }

    size_t survivor_count = 0;
    for (size_t i = 0; i < s->count; i++) {
        if (!(s->entries[i].category && strcmp(s->entries[i].category, category) == 0)) {
            survivors[survivor_count++] = s->entries[i];
        }
    }
    cc_memory_entry_t *old_entries = s->entries;
    size_t old_count = s->count;
    s->entries = survivors;
    s->count = survivor_count;
    int installed = 0;
    cc_result_t save_rc = save_to_file(s, &installed);
    if (save_rc.code == CC_OK || installed) {
        s->revision++;
        for (size_t i = 0; i < old_count; i++) {
            if (old_entries[i].category && strcmp(old_entries[i].category, category) == 0) {
                cc_memory_entry_free(&old_entries[i]);
            }
        }
        free(old_entries);
    } else {
        free(survivors);
        s->entries = old_entries;
        s->count = old_count;
    }

    cc_mutex_unlock(s->mutex);
    return save_rc;
}

/**
 * @brief 释放私有存储及其拥有的全部记录、元数据、路径和锁。
 * @pre 调用者已关闭入口并排空所有并发访问，或对象尚未发布。
 * @param[in,out] self 待销毁私有实例，可为 NULL。
 * @post 实例寿命结束，自有默认文件系统也已销毁，外部注入的文件系统仍由调用者管理。
 */
static void json_destroy(void *self)
{
    cc_json_memory_store_t *s = (cc_json_memory_store_t *)self;
    if (!s) return;
    for (size_t i = 0; i < s->count; i++) cc_memory_entry_free(&s->entries[i]);
    free(s->entries);
    metadata_free_array(s->metadata, s->metadata_count);
    free(s->file_path);
    cc_mutex_destroy(s->mutex);
    if (s->owns_fs && s->fs.vtable && s->fs.vtable->destroy) s->fs.vtable->destroy(s->fs.self);
    free(s);
}

/* JSON 文件适配器提供原生结构化查询，公共包装通过此表转发调用。 */
static cc_memory_store_vtable_t json_vtable = {
    .set = json_set,
    .get = json_get,
    .search = json_search,
    .list = json_list,
    .delete_entry = json_delete_entry,
    .delete_by_category = json_delete_by_category,
    .destroy = json_destroy,
    .query = json_query,
};

/**
 * @brief 在锁内将受管理元数据映射为记录，再构造拥有的集合副本。
 * @param[in] store 本适配器创建的有效存储，阻断写入时仍可读取内存。
 * @param[out] out_state 无已有资源的输出结构，参数合法后清零，成功副本由调用者 cc_memory_plan_cleanup。
 * @param[out] out_revision 参数合法后置零，仅成功时填写同一次锁快照中的修订号。
 * @return 成功为 CC_OK，参数不符为 CC_ERR_INVALID_ARGUMENT，元数据缺少对应项为 CC_ERR_STORAGE，
 * 暂存分配失败为 CC_ERR_OUT_OF_MEMORY，方案构造错误原样返回。
 */
cc_result_t cc_json_memory_managed_list(
    cc_memory_store_t *store, cc_memory_plan_t *out_state, uint64_t *out_revision)
{
    if (!store || store->vtable != &json_vtable || !store->self || !out_state || !out_revision)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid managed memory list");
    memset(out_state, 0, sizeof(*out_state));
    *out_revision = 0;
    cc_json_memory_store_t *s = store->self;
    cc_mutex_lock(s->mutex);
    size_t count = s->metadata_count;
    cc_memory_plan_record_t *borrowed = count ? calloc(count, sizeof(*borrowed)) : NULL;
    cc_result_t rc = count && !borrowed ? cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot list managed memories") : cc_result_ok();
    for (size_t i = 0; rc.code == CC_OK && i < count; i++) {
        size_t index = 0;
        while (index < s->count && strcmp(s->entries[index].key, s->metadata[i].key) != 0) index++;
        if (index == s->count) { rc = cc_result_error(CC_ERR_STORAGE, "Managed memory metadata has no entry"); break; }
        borrowed[i].size = sizeof(borrowed[i]);
        borrowed[i].id = s->entries[index].key;
        borrowed[i].text = s->entries[index].value;
        borrowed[i].category = s->entries[index].category;
        borrowed[i].evidence = s->metadata[i].evidence;
        borrowed[i].source_session = s->entries[index].session_id;
        borrowed[i].importance = s->metadata[i].importance;
        borrowed[i].updated_at = (int64_t)s->entries[index].updated_at;
    }
    if (rc.code == CC_OK)
        rc = cc_memory_plan_build(borrowed, count, NULL, 0, count ? count : 1U, NULL, NULL, out_state);
    if (rc.code == CC_OK) *out_revision = s->revision;
    free(borrowed);
    cc_mutex_unlock(s->mutex);
    return rc;
}

/**
 * @brief 在锁内读取缓存的写入阻断状态，本次调用只访问内存。
 * @param[in] store 本适配器创建的有效存储。
 * @param[out] out_ready 合法参数下在锁内填写，未阻断为一，已阻断为零，参数错误时保持原值。
 * @return 查询成功为 CC_OK，NULL 参数或其他适配器为 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t cc_json_memory_write_ready(cc_memory_store_t *store, int *out_ready)
{
    if (!store || store->vtable != &json_vtable || !store->self || !out_ready)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid managed memory status request");
    cc_json_memory_store_t *s = store->self;
    cc_mutex_lock(s->mutex);
    *out_ready = !s->write_indeterminate;
    cc_mutex_unlock(s->mutex);
    return cc_result_ok();
}

/**
 * @brief 在锁内将创建时保存的路径与给定字符串逐字节比较。
 * @param[in] store 待核对存储，可为 NULL，必须是本适配器才可能匹配。
 * @param[in] path 借用的候选路径，可为 NULL。
 * @return 适配器、实例及路径全部匹配为一，其他情况为零，直接使用 strcmp 的路径语义。
 */
int cc_json_memory_store_matches_path(cc_memory_store_t *store, const char *path)
{
    if (!store || store->vtable!=&json_vtable || !store->self || !path) return 0;
    cc_json_memory_store_t *s=store->self;
    cc_mutex_lock(s->mutex);
    int matches=strcmp(s->file_path,path)==0;
    cc_mutex_unlock(s->mutex); return matches;
}

/**
 * @brief 验证目标集合，在存储锁内创建或复用快照并提交受管理记忆替换。
 * @param[in,out] store 本适配器实例，未受管理记录先深拷贝保留，已有受管理记录由目标集合完整替换。
 * @param[in] expected_revision 当前进程修订号，锁内要求相等以拒绝陈旧方案。
 * @param[in] plan 借用的目标集合，先通过 cc_memory_plan_build 校验并复制，允许空集合。
 * @param[in,out] snapshots 可为 NULL 的借用快照存储，与非零 reason 同时提供，须绑定当前主文件路径。
 * @param[in] reason 零跳过快照，或使用 REWRITE、RESET 两个快照原因。
 * @param[in] prior_snapshot_id 可选借用的快照 ID，须提供 snapshots，前缀与原因一致，复制后调用 confirm。
 * @param[out] out_snapshot_id 必需的无已有资源输出槽，基础参数合法后置 NULL，成功获得的拥有 ID 在后续失败时仍交调用者 free。
 * @param[out] out_installed 必需输出，基础参数合法后置零，save_to_file 确认新文件可见时为一。
 * @return 完整提交为 CC_OK，非法方案、路径、时间或 ID 为 CC_ERR_INVALID_ARGUMENT，版本冲突为 CC_ERR_INVALID_STATE，
 * 写入阻断为 CC_ERR_STORAGE，容量或版本溢出为 CC_ERR_LIMIT_EXCEEDED，分配失败为 CC_ERR_OUT_OF_MEMORY，快照及保存错误原样返回。
 * @note 确认安装时采用新数组并递增 revision，后续错误仍保留该结果。未确认安装则恢复旧数组，
 * 结果未知时由保存路径保留 write_indeterminate。快照可能早于后续方案冲突检查创建，失败后仍须处理返回的 ID。
 */
cc_result_t cc_json_memory_managed_apply(
    cc_memory_store_t *store, uint64_t expected_revision, const cc_memory_plan_t *plan,
    cc_memory_snapshot_store_t *snapshots, cc_memory_snapshot_reason_t reason,
    const char *prior_snapshot_id, char **out_snapshot_id, int *out_installed)
{
    if (!store || store->vtable != &json_vtable || !store->self || !plan ||
        (!plan->records && plan->count) || !out_snapshot_id || !out_installed ||
        (reason != 0 && reason != CC_MEMORY_SNAPSHOT_REWRITE && reason != CC_MEMORY_SNAPSHOT_RESET) ||
        ((reason != 0) != (snapshots != NULL)) || (prior_snapshot_id && !snapshots))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid managed memory transaction");
    *out_snapshot_id = NULL; *out_installed = 0;
    cc_memory_plan_t validated = {0};
    cc_result_t rc = cc_memory_plan_build(plan->records, plan->count, NULL, 0,
                                           plan->count ? plan->count : 1U, NULL, NULL, &validated);
    if (rc.code != CC_OK) return rc;
    cc_json_memory_store_t *s = store->self;
    cc_mutex_lock(s->mutex);
    if (s->write_indeterminate)
        rc = cc_result_error(CC_ERR_STORAGE, "Memory storage needs recovery before further writes");
    else if (s->revision != expected_revision)
        rc = cc_result_error(CC_ERR_INVALID_STATE, "Memory changed while the update was being planned");
    else if (s->revision == UINT64_MAX)
        rc = cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Memory revision overflow");
    else if (snapshots && !cc_memory_snapshot_source_matches(snapshots, s->file_path))
        rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Snapshot store targets another memory file");
    if (rc.code == CC_OK) rc = memory_require_clean_recovery_files(s);

    if (rc.code == CC_OK && snapshots) {
        if (prior_snapshot_id) {
            if ((reason == CC_MEMORY_SNAPSHOT_REWRITE && strncmp(prior_snapshot_id, "rewrite-", 8) != 0) ||
                (reason == CC_MEMORY_SNAPSHOT_RESET && strncmp(prior_snapshot_id, "reset-", 6) != 0))
                rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Snapshot reason does not match transaction");
            else {
                *out_snapshot_id = cc_copy_string(prior_snapshot_id);
                if (!*out_snapshot_id) rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy snapshot id");
                else rc = cc_memory_snapshot_confirm(snapshots, prior_snapshot_id);
            }
        } else {
            cc_filesystem_stat_t info = {.size = sizeof(info)};
            rc = s->fs.vtable->stat(s->fs.self, s->file_path, &info);
            if (rc.code == CC_OK && info.type == CC_FS_ENTRY_MISSING && s->count != 0)
                rc = cc_result_error(CC_ERR_STORAGE, "Memory primary disappeared before snapshot");
            else if (rc.code == CC_OK && info.type != CC_FS_ENTRY_MISSING)
                rc = cc_memory_snapshot_create(snapshots, reason, out_snapshot_id);
        }
    }

    size_t unmanaged = 0;
    for (size_t i = 0; i < s->count; i++)
        if (metadata_index(s, s->entries[i].key) == s->metadata_count) unmanaged++;
    if (rc.code == CC_OK && (unmanaged > SIZE_MAX - validated.count ||
        unmanaged + validated.count > SIZE_MAX / sizeof(cc_memory_entry_t)))
        rc = cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Managed memory transaction is too large");
    size_t total = rc.code == CC_OK ? unmanaged + validated.count : 0;
    size_t capacity = total < 64U ? 64U : total;
    cc_memory_entry_t *next_entries = NULL;
    cc_json_memory_metadata_t *next_metadata = NULL;
    if (rc.code == CC_OK) {
        next_entries = calloc(capacity, sizeof(*next_entries));
        next_metadata = validated.count ? calloc(validated.count, sizeof(*next_metadata)) : NULL;
        if (!next_entries || (validated.count && !next_metadata))
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot stage managed memory transaction");
    }
    size_t entry_count = 0, metadata_count = 0;
    for (size_t i = 0; rc.code == CC_OK && i < s->count; i++) {
        if (metadata_index(s, s->entries[i].key) < s->metadata_count) continue;
        rc = copy_entry(&s->entries[i], &next_entries[entry_count]);
        if (rc.code == CC_OK) entry_count++;
    }
    for (size_t i = 0; rc.code == CC_OK && i < validated.count; i++) {
        const cc_memory_plan_record_t *record = &validated.records[i];
        if (strcmp(record->category, "context_summary") == 0) {
            rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Context summary is not a managed long-term memory");
            break;
        }
        for (size_t j = 0; j < unmanaged; j++) {
            if (strcmp(next_entries[j].key, record->id) == 0) {
                rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Managed memory id conflicts with existing data");
                break;
            }
        }
        if (rc.code != CC_OK) break;
        int64_t max_time = sizeof(time_t) >= 8U ? 253402300799LL : 2147483647LL;
        if (record->updated_at > max_time) {
            rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Managed memory timestamp is out of range");
            break;
        }
        time_t created = time(NULL);
        for (size_t j = 0; j < s->count; j++) {
            if (strcmp(s->entries[j].key, record->id) == 0 &&
                metadata_index(s, record->id) < s->metadata_count) { created = s->entries[j].created_at; break; }
        }
        cc_memory_entry_t source = {.key = record->id, .value = record->text,
            .category = record->category, .session_id = record->source_session,
            .created_at = created, .updated_at = (time_t)record->updated_at};
        rc = copy_entry(&source, &next_entries[entry_count]);
        if (rc.code != CC_OK) break;
        entry_count++;
        next_metadata[metadata_count].key = cc_copy_string(record->id);
        next_metadata[metadata_count].evidence = record->evidence ? cc_copy_string(record->evidence) : NULL;
        next_metadata[metadata_count].importance = record->importance;
        metadata_count++;
        if (!next_metadata[metadata_count - 1U].key ||
            (record->evidence && !next_metadata[metadata_count - 1U].evidence))
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot stage managed memory metadata");
    }
    if (rc.code == CC_OK) {
        cc_memory_entry_t *old_entries = s->entries;
        size_t old_count = s->count, old_capacity = s->cap;
        cc_json_memory_metadata_t *old_metadata = s->metadata;
        size_t old_metadata_count = s->metadata_count;
        s->entries = next_entries; s->count = entry_count; s->cap = capacity;
        s->metadata = next_metadata; s->metadata_count = metadata_count;
        int installed = 0;
        rc = save_to_file(s, &installed);
        *out_installed = installed;
        if (rc.code == CC_OK || installed) {
            s->revision++;
            cc_memory_entry_free_array(old_entries, old_count);
            metadata_free_array(old_metadata, old_metadata_count);
            next_entries = NULL; next_metadata = NULL;
        } else {
            s->entries = old_entries; s->count = old_count; s->cap = old_capacity;
            s->metadata = old_metadata; s->metadata_count = old_metadata_count;
        }
    }
    if (next_entries) cc_memory_entry_free_array(next_entries, entry_count);
    if (next_metadata) metadata_free_array(next_metadata, metadata_count);
    cc_mutex_unlock(s->mutex);
    cc_memory_plan_cleanup(&validated);
    return rc;
}

/**
 * @brief 检查文件系统能力、分配未发布实例并加载已有主文件。
 * @param[out] out_store 无已有资源的输出句柄，out_store 与 file_path 非 NULL 后先清零，仅成功时发布 self/vtable。
 * @param[in] file_path 有效的借用主文件路径，复制保存。
 * @param[in] filesystem 可为 NULL，NULL 时取得并拥有默认文件系统，否则复制描述且借用其 self/vtable。
 * @return 成功为 CC_OK，必需参数或借用描述尺寸非法为 CC_ERR_INVALID_ARGUMENT，能力不足为 CC_ERR_UNSUPPORTED，
 * 分配失败为 CC_ERR_OUT_OF_MEMORY，文件系统获取、锁创建和加载错误原样返回，失败释放本次已分配资源。
 */
static cc_result_t memory_store_create(
    cc_memory_store_t *out_store, const char *file_path, const cc_filesystem_t *filesystem)
{
    if (!out_store || !file_path)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid json file memory store arguments");
    memset(out_store, 0, sizeof(*out_store));

    cc_json_memory_store_t *s = calloc(1, sizeof(cc_json_memory_store_t));
    if (!s) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate json memory store");

    if (filesystem) {
        if (filesystem->size < sizeof(*filesystem)) { free(s); return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid borrowed filesystem"); }
        s->fs = *filesystem;
    } else {
        cc_result_t fs_rc = cc_filesystem_get_default(&s->fs);
        if (fs_rc.code != CC_OK) { free(s); return fs_rc; }
        s->owns_fs = 1;
    }
    uint64_t required_caps = CC_FS_CAP_ATOMIC_REPLACE | CC_FS_CAP_FILE_SYNC | CC_FS_CAP_STAT;
    if (!s->fs.vtable ||
        s->fs.vtable->size < offsetof(cc_filesystem_vtable_t, sync_dir) + sizeof(s->fs.vtable->sync_dir) ||
        s->fs.version < 3U ||
        (s->fs.capabilities & required_caps) != required_caps ||
        !s->fs.vtable->read_text || !s->fs.vtable->write_text ||
        !s->fs.vtable->atomic_replace || !s->fs.vtable->sync_file || !s->fs.vtable->stat ||
        ((s->fs.capabilities & CC_FS_CAP_DIR_SYNC) && !s->fs.vtable->sync_dir)) {
        if (s->owns_fs && s->fs.vtable && s->fs.vtable->destroy) s->fs.vtable->destroy(s->fs.self);
        free(s);
        return cc_result_error(CC_ERR_UNSUPPORTED,
            "JSON memory store requires atomic replace and file sync");
    }
    s->file_path = cc_copy_string(file_path);
    if (!s->file_path) {
        if (s->owns_fs && s->fs.vtable && s->fs.vtable->destroy) s->fs.vtable->destroy(s->fs.self);
        free(s);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy memory store path");
    }
    s->cap = 64;
    s->revision = 1;
    s->entries = calloc(s->cap, sizeof(cc_memory_entry_t));
    if (!s->entries) {
        free(s->file_path);
        if (s->owns_fs && s->fs.vtable && s->fs.vtable->destroy) s->fs.vtable->destroy(s->fs.self);
        free(s);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "OOM");
    }

    cc_result_t mutex_rc = cc_mutex_create(&s->mutex);
    if (mutex_rc.code != CC_OK) {
        free(s->entries);
        free(s->file_path);
        if (s->owns_fs && s->fs.vtable && s->fs.vtable->destroy) s->fs.vtable->destroy(s->fs.self);
        free(s);
        return mutex_rc;
    }
    cc_result_free(&mutex_rc);
    cc_result_t load_rc = load_from_file(s);
    if (load_rc.code != CC_OK) {
        json_destroy(s);
        return load_rc;
    }

    out_store->self = s;
    out_store->vtable = &json_vtable;
    return cc_result_ok();
}

/**
 * @brief 通过内部创建入口使用默认文件系统打开存储。
 * @param[out] out_store 无已有资源的输出句柄，初始化时机与所有权遵循 memory_store_create。
 * @param[in] file_path 调用期间借用并复制的主文件路径。
 * @return memory_store_create 的创建或加载结果原样返回。
 */
cc_result_t cc_memory_store_create_json_file(cc_memory_store_t *out_store, const char *file_path)
{
    return memory_store_create(out_store, file_path, NULL);
}

/**
 * @brief 拒绝空文件系统后通过内部入口创建借用文件系统的存储。
 * @param[out] out_store 无已有资源的输出句柄，filesystem 为 NULL 时保持原值，其他初始化规则见 memory_store_create。
 * @param[in] file_path 调用期间借用并复制的主文件路径。
 * @param[in] filesystem 必需的借用描述，self/vtable 生命周期须覆盖存储。
 * @return NULL filesystem 为 CC_ERR_INVALID_ARGUMENT，其他创建或加载结果原样返回。
 */
cc_result_t cc_memory_store_create_json_file_with_filesystem(
    cc_memory_store_t *out_store, const char *file_path, const cc_filesystem_t *filesystem)
{
    if (!filesystem) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null borrowed filesystem");
    return memory_store_create(out_store, file_path, filesystem);
}

/**
 * @brief 在锁内开启受管理召回过滤标志，供 search、list 和普通 query 使用。
 * @param[in,out] store 本适配器实例，配置仅保存在内存中。
 * @return 成功为 CC_OK，NULL 参数、空实例或适配器不符为 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t cc_json_memory_restrict_recall_to_managed(cc_memory_store_t *store)
{
    if (!store || store->vtable!=&json_vtable || !store->self)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid managed memory recall store");
    cc_json_memory_store_t *s=store->self;
    cc_mutex_lock(s->mutex);
    s->managed_recall_only=1;
    cc_mutex_unlock(s->mutex);
    return cc_result_ok();
}

/**
 * @brief 在锁内安装三项摘要限制，再调用有界摘要事务裁剪已有缓存。
 * @pre 外层已排空运行中的生成与直接存储使用者，配置期间保持屏障。
 * @param[in,out] store 本适配器实例，事务失败也保留本次设置的三项限制。
 * @param[in] per_session 正数，每个会话的派生摘要条数上限。
 * @param[in] max_entries 正数，全部会话派生摘要条数上限。
 * @param[in] max_value_bytes 正数，全部保留派生摘要正文的 UTF-8 字节总上限，不含 NUL。
 * @return 非法参数为 CC_ERR_INVALID_ARGUMENT，其他结果由 bounded_summaries_locked 原样返回。
 * 无需裁剪时可直接返回 CC_OK，即使存储已有写入阻断。
 */
cc_result_t cc_json_memory_set_summary_cache_limits(
    cc_memory_store_t *store, size_t per_session, size_t max_entries, size_t max_value_bytes)
{
    if (!store || store->vtable != &json_vtable || !store->self ||
        !per_session || !max_entries || !max_value_bytes)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid context summary cache limits");
    cc_json_memory_store_t *s = store->self;
    cc_mutex_lock(s->mutex);
    s->summary_per_session = per_session;
    s->summary_max_entries = max_entries;
    s->summary_max_value_bytes = max_value_bytes;
    cc_result_t rc = bounded_summaries_locked(s, NULL, NULL, NULL);
    cc_mutex_unlock(s->mutex);
    return rc;
}

/**
 * @brief 为写入阻断实例重新打开主文件，成功后在原外部句柄中替换私有对象。
 * @pre 外层排空所有读写及回调使用者，函数返回前禁止重新进入该存储。
 * @param[in,out] store 本适配器有效句柄，成功采用新 self、迁移文件系统所有权与配置，再销毁旧 self。
 * @return 成功为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，无需恢复为 CC_ERR_INVALID_STATE，
 * 修订号溢出为 CC_ERR_LIMIT_EXCEEDED，主文件缺失或旁路仍存在为 CC_ERR_STORAGE，检查及重新加载错误原样返回。
 * @note 任一失败保留旧句柄和写入阻断状态，成功后 revision 在旧值上加一。
 */
cc_result_t cc_json_memory_recover_in_place(cc_memory_store_t *store)
{
    if (!store || store->vtable!=&json_vtable || !store->self)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory store recovery");
    cc_json_memory_store_t *old=store->self;
    /* 外层提供排他屏障，此锁仅获取一致的恢复前提，随后释放锁以构建新实例。 */
    cc_mutex_lock(old->mutex);
    int uncertain=old->write_indeterminate;
    uint64_t revision=old->revision;
    cc_mutex_unlock(old->mutex);
    if (!uncertain) return cc_result_error(CC_ERR_INVALID_STATE,"Memory store does not require recovery");
    if (revision==UINT64_MAX) return cc_result_error(CC_ERR_LIMIT_EXCEEDED,"Memory revision overflow");
    cc_filesystem_stat_t info={.size=sizeof(info)};
    cc_result_t rc=old->fs.vtable->stat(old->fs.self,old->file_path,&info);
    if (rc.code!=CC_OK) return rc;
    if (info.type!=CC_FS_ENTRY_FILE)
        return cc_result_error(CC_ERR_STORAGE,"Installed memory file is unavailable during recovery");
    cc_memory_store_t reopened={0};
    rc=memory_store_create(&reopened,old->file_path,&old->fs);
    if (rc.code!=CC_OK) return rc;
    cc_json_memory_store_t *fresh=reopened.self;
    if (fresh->write_indeterminate) {
        cc_memory_store_destroy(&reopened);
        return cc_result_error(CC_ERR_STORAGE,"Memory recovery files still require review");
    }
    fresh->revision=revision+1U;
    fresh->managed_recall_only=old->managed_recall_only;
    fresh->summary_per_session=old->summary_per_session;
    fresh->summary_max_entries=old->summary_max_entries;
    fresh->summary_max_value_bytes=old->summary_max_value_bytes;
    fresh->owns_fs=old->owns_fs; old->owns_fs=0;
    *store=reopened;
    json_destroy(old);
    return cc_result_ok();
}
