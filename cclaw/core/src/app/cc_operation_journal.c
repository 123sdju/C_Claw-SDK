#include "cc/app/cc_operation_journal.h"
#include "cc/core/cc_id.h"
#include "cc/internal/cc_alloc.h"
#include "cc/internal/cc_json_syntax_check.h"
#include "cc/ports/cc_thread.h"
#include "cc/util/cc_json.h"
#include "cJSON.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 读取/编码单条记录最多 256 KiB，四个外部字段合计最多 64 KiB。 */
#define JOURNAL_MAX_RECORD_BYTES (256U * 1024U)
#define JOURNAL_MAX_FIELDS_BYTES (64U * 1024U)

struct cc_operation_journal {
    cc_filesystem_t *fs;            /* 借用至实例销毁，回调在日志锁内执行。 */
    char *directory;               /* 拥有的专用目录路径副本。 */
    size_t max_pending;            /* 普通新操作准入阈值，导入及显式只读入口可越过。 */
    cc_pending_operation_t *items; /* 拥有的记录数组及各记录字符串。 */
    size_t count;                  /* 全部执行中、UNKNOWN 及核对中的内存项数。 */
    size_t capacity;               /* items 的分配槽数，独立于准入阈值。 */
    cc_mutex_t mutex;              /* 串行保护记录状态及对应文件操作。 */
};

/**
 * @brief 释放操作 ID、会话及工具调用的拥有字段并清零记录。
 * @param[in,out] entry 有效拥有记录或零记录，允许 NULL。
 * @post 非空记录恢复全零，可再次清理。
 */
static void entry_cleanup(cc_pending_operation_t *entry)
{
    if (!entry) return;
    free(entry->operation_id);
    free(entry->session_id);
    cc_tool_call_cleanup(&entry->call);
    memset(entry, 0, sizeof(*entry));
}

/**
 * @brief 逐条清理前 count 项并释放操作数组。
 * @param[in,out] items 拥有数组，允许 NULL，非空时前 count 项须有效或零初始化。
 * @param[in] count 待释放的有效槽数，须在数组范围内。
 * @post 数组及其字符串已释放，调用者持有的指针失效。
 */
void cc_pending_operation_array_free(cc_pending_operation_t *items, size_t count)
{
    if (!items) return;
    for (size_t i = 0; i < count; i++) entry_cleanup(&items[i]);
    free(items);
}

/**
 * @brief 深拷贝持久化身份及工具调用，再复制两个进程状态标志。
 * @param[in] src 必填的有效源记录，同步借用。
 * @param[out] dst 必填且与 src 独立的空输出槽，先清零，成功取得拥有副本。
 * @return 成功为 CC_OK，分配或工具调用复制失败向上传递，失败清理本次副本。
 */
static cc_result_t entry_copy(const cc_pending_operation_t *src, cc_pending_operation_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->operation_id = cc_copy_string(src->operation_id);
    dst->session_id = cc_copy_string(src->session_id);
    if (!dst->operation_id || !dst->session_id) {
        entry_cleanup(dst);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy pending operation");
    }
    cc_result_t rc = cc_tool_call_copy(&src->call, &dst->call);
    if (rc.code != CC_OK) {
        entry_cleanup(dst);
        return rc;
    }
    dst->in_progress = src->in_progress;
    dst->reconciling = src->reconciling;
    return rc;
}

/**
 * @brief 向 CRC32 累加一个字段的正文及结尾 NUL 分隔字节。
 * @param[in] crc 上个字段留下的未最终异或状态。
 * @param[in] text 必填的同步借用字符串。
 * @return 用反射多项式 0xedb88320 更新后的状态，供下一字段继续累加。
 */
static uint32_t crc_bytes(uint32_t crc, const char *text)
{
    /* 将 NUL 计入校验，保留字段拼接边界。 */
    const unsigned char *p = (const unsigned char *)text;
    do {
        crc ^= *p;
        for (unsigned i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
    } while (*p++);
    return crc;
}

/**
 * @brief 计算版本、操作 ID、会话和三个工具调用字符串的 CRC32。
 * @param[in] entry 必填的有效记录，各持久化字符串须非 NULL，同步借用。
 * @return 包含各字段 NUL 分隔并完成最终异或的校验值，进程标志独立于该校验。
 */
static uint32_t entry_crc(const cc_pending_operation_t *entry)
{
    uint32_t crc = 0xffffffffU;
    crc = crc_bytes(crc, "1");
    crc = crc_bytes(crc, entry->operation_id);
    crc = crc_bytes(crc, entry->session_id);
    crc = crc_bytes(crc, entry->call.id);
    crc = crc_bytes(crc, entry->call.name);
    crc = crc_bytes(crc, entry->call.arguments_json);
    return crc ^ 0xffffffffU;
}

/**
 * @brief 检查 op- 前缀及 36 字节小写 UUID 字符形状。
 * @param[in] id 同步借用的 ID，允许 NULL，版本位和变体位按普通十六进制字符检查。
 * @return 长度、前缀、连字符位置及字符均合法为一，其余为零。
 */
static int valid_operation_id(const char *id)
{
    if (!id || strlen(id) != 39U || strncmp(id, "op-", 3U) != 0) return 0;
    for (size_t i = 3; i < 39U; i++) {
        char c = id[i];
        if (i == 11U || i == 16U || i == 21U || i == 26U) {
            if (c != '-') return 0;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief 分配目录、斜线、操作 ID 和后缀拼成的完整路径。
 * @param[in] journal 借用的有效日志，其目录副本在实例寿命内稳定。
 * @param[in] id 同步借用的非 NULL 操作 ID。
 * @param[in] suffix 同步借用的后缀，本文件使用 .json 或 .tmp。
 * @return 拥有的堆路径，调用者 free，长度检查或分配失败返回 NULL。
 */
static char *entry_path(const cc_operation_journal_t *journal, const char *id, const char *suffix)
{
    size_t dir_len = strlen(journal->directory), id_len = strlen(id), suffix_len = strlen(suffix);
    if (dir_len > SIZE_MAX - id_len - suffix_len - 2U) return NULL;
    char *path = malloc(dir_len + id_len + suffix_len + 2U);
    if (!path) return NULL;
    snprintf(path, dir_len + id_len + suffix_len + 2U, "%s/%s%s",
        journal->directory, id, suffix);
    return path;
}

/**
 * @brief 将五个持久化字符串、版本一及 CRC32 编码为紧凑 JSON 对象。
 * @param[in] entry 必填的有效记录，同步借用，两个进程标志保留在 RAM。
 * @param[out] out_text 必填，先置 NULL，成功取得可由 free 释放的独立文本副本。
 * @return 成功为 CC_OK，编码/复制分配失败为 CC_ERR_OUT_OF_MEMORY，编码正文超过 256 KiB 为 CC_ERR_LIMIT_EXCEEDED。
 */
static cc_result_t serialize_entry(const cc_pending_operation_t *entry, char **out_text)
{
    *out_text = NULL;
    cJSON *obj = cJSON_CreateObject();
    char crc[9];
    snprintf(crc, sizeof(crc), "%08lx", (unsigned long)entry_crc(entry));
    if (!obj || !cJSON_AddNumberToObject(obj, "version", 1) ||
        !cJSON_AddStringToObject(obj, "operation_id", entry->operation_id) ||
        !cJSON_AddStringToObject(obj, "session_id", entry->session_id) ||
        !cJSON_AddStringToObject(obj, "tool_call_id", entry->call.id) ||
        !cJSON_AddStringToObject(obj, "tool_name", entry->call.name) ||
        !cJSON_AddStringToObject(obj, "arguments_json", entry->call.arguments_json) ||
        !cJSON_AddStringToObject(obj, "crc32", crc)) {
        cJSON_Delete(obj);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode pending operation");
    }
    char *printed = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!printed) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode pending operation");
    size_t length = strlen(printed);
    if (length > JOURNAL_MAX_RECORD_BYTES) {
        cJSON_free(printed);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Pending operation record is too large");
    }
    *out_text = cc_copy_string(printed);
    cJSON_free(printed);
    return *out_text ? cc_result_ok() :
        cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy pending operation record");
}

/**
 * @brief 按大小写敏感键取 JSON 字符串字段。
 * @param[in] obj 调用期间借用的 JSON 对象。
 * @param[in] key 同步借用的字段名。
 * @return 字符串字段的借用地址，有效期至 obj 删除，缺失或类型不符返回 NULL。
 */
static const char *json_string(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

/**
 * @brief 扫描原始文本，拒绝未被转义反斜线引出的 u0000 序列。
 *
 * 跳过已配对的转义字符，针对显式 u0000 防止身份及校验使用 NUL 前的截断内容。
 * @param[in] text 同步借用的原始 JSON 文本，允许 NULL。
 * @return 检出未被转义反斜线引出的 u0000 时为一，其余为零。
 */
static int journal_has_nul_escape(const char *text)
{
    for (const char *p = text; p && *p; p++) {
        if (*p != '\\' || !p[1]) continue;
        if (strncmp(p + 1, "u0000", 5U) == 0) return 1;
        p++;
    }
    return 0;
}

/**
 * @brief 检查七个已知顶层字段各自至多出现一次。
 * @param[in] object 借用的有效 JSON 对象，遍历子节点名，未知字段允许存在或重复。
 * @return 已知字段唯一且全部子节点有字段名为一，否则为零，必需字段存在性由解析后续检查。
 */
static int journal_fields_unique(const cJSON *object)
{
    static const char *const fields[] = {"version", "operation_id", "session_id", "tool_call_id",
                                         "tool_name", "arguments_json", "crc32"};
    unsigned seen = 0;
    for (const cJSON *item = object->child; item; item = item->next) {
        if (!item->string) return 0;
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
            if (strcmp(item->string, fields[i]) != 0) continue;
            unsigned bit = 1U << i;
            if (seen & bit) return 0;
            seen |= bit;
            break;
        }
    }
    return 1;
}

/**
 * @brief 使用 cJSON 要求完整尾部消费，验证单个对象的字段、长度及 CRC32 后取得拥有记录。
 * @param[in] text 同步借用的完整 JSON 文本，NULL 或可解码 NUL 被视为存储损坏。
 * @param[out] out 必填且无待释放内容，先清零，成功深拷贝五个字符串，两进程标志保持零。
 * @param[out] out_failure 可选分类，仅明确损坏时写 INTEGRITY，解析分配可能性保留调用前 OTHER。
 * @return 成功为 CC_OK，语法、版本、已知字段重复、身份、大小或校验异常为 CC_ERR_STORAGE，
 * 字符串分配失败为 CC_ERR_OUT_OF_MEMORY，失败输出保持全零。arguments_json 按原文本字符串保存。
 */
static cc_result_t parse_entry(const char *text, cc_pending_operation_t *out,
                               cc_operation_journal_failure_t *out_failure)
{
    memset(out, 0, sizeof(*out));
    if (!text || journal_has_nul_escape(text)) {
        if (out_failure) *out_failure = CC_OPERATION_JOURNAL_FAILURE_INTEGRITY;
        return cc_result_error(CC_ERR_STORAGE, "Pending operation contains an encoded NUL");
    }
    cc_json_value_t *parsed = NULL;
    cc_result_t parsed_rc = cc_json_parse_strict(text, &parsed);
    if (parsed_rc.code != CC_OK) {
        if (out_failure && cc_json_syntax_check(text) == CC_JSON_SYNTAX_INVALID)
            *out_failure = CC_OPERATION_JOURNAL_FAILURE_INTEGRITY;
        cc_result_free(&parsed_rc);
        return cc_result_error(CC_ERR_STORAGE, "Pending operation record cannot be parsed");
    }
    cc_result_free(&parsed_rc);
    cJSON *obj = (cJSON *)parsed;
    if (!cJSON_IsObject(obj) || !journal_fields_unique(obj)) {
        cJSON_Delete(obj);
        if (out_failure) *out_failure = CC_OPERATION_JOURNAL_FAILURE_INTEGRITY;
        return cc_result_error(CC_ERR_STORAGE, "Pending operation record is invalid");
    }
    cJSON *version = cJSON_GetObjectItemCaseSensitive(obj, "version");
    const char *id = json_string(obj, "operation_id");
    const char *session = json_string(obj, "session_id");
    const char *call_id = json_string(obj, "tool_call_id");
    const char *name = json_string(obj, "tool_name");
    const char *args = json_string(obj, "arguments_json");
    const char *checksum = json_string(obj, "crc32");
    if (!cJSON_IsNumber(version) || version->valuedouble != 1 ||
        !valid_operation_id(id) || !session || !session[0] || !call_id || !call_id[0] ||
        !name || !name[0] || !args || !checksum || strlen(checksum) != 8U ||
        strlen(session) + strlen(call_id) + strlen(name) + strlen(args) > JOURNAL_MAX_FIELDS_BYTES) {
        cJSON_Delete(obj);
        if (out_failure) *out_failure = CC_OPERATION_JOURNAL_FAILURE_INTEGRITY;
        return cc_result_error(CC_ERR_STORAGE, "Pending operation record has invalid fields");
    }
    out->operation_id = cc_copy_string(id);
    out->session_id = cc_copy_string(session);
    out->call.id = cc_copy_string(call_id);
    out->call.name = cc_copy_string(name);
    out->call.arguments_json = cc_copy_string(args);
    if (!out->operation_id || !out->session_id || !out->call.id ||
        !out->call.name || !out->call.arguments_json) {
        cJSON_Delete(obj);
        entry_cleanup(out);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot load pending operation");
    }
    char expected[9];
    snprintf(expected, sizeof(expected), "%08lx", (unsigned long)entry_crc(out));
    int matches = strcmp(checksum, expected) == 0;
    cJSON_Delete(obj);
    if (!matches) {
        entry_cleanup(out);
        if (out_failure) *out_failure = CC_OPERATION_JOURNAL_FAILURE_INTEGRITY;
        return cc_result_error(CC_ERR_STORAGE, "Pending operation checksum mismatch");
    }
    return cc_result_ok();
}

/**
 * @brief stat 限制普通文件类型与 256 KiB 上限，读取并核对字节长度后解析。
 * @pre 外层独占打开过程或持有日志锁，fs 和目录保持有效。
 * @param[in] journal 借用的日志及文件系统。
 * @param[in] path 同步借用的文件路径。
 * @param[out] out 必填且无待释放内容，仅进入 parse_entry 后清零，成功拥有记录供 entry_cleanup 清理。
 * @param[out] out_failure 可选，观察到文件/记录损坏时设 INTEGRITY，底层错误保持调用前分类。
 * @return 成功为 CC_OK，文件类型、大小、截断及记录损坏为 CC_ERR_STORAGE，底层读取或分配错误向上传递，读取阶段失败保持 out 原值。
 */
static cc_result_t read_entry(cc_operation_journal_t *journal, const char *path,
                              cc_pending_operation_t *out, cc_operation_journal_failure_t *out_failure)
{
    cc_filesystem_stat_t info = {0};
    info.size = sizeof(info);
    cc_result_t rc = journal->fs->vtable->stat(journal->fs->self, path, &info);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    if (info.type != CC_FS_ENTRY_FILE || info.byte_size > JOURNAL_MAX_RECORD_BYTES) {
        if (out_failure) *out_failure = CC_OPERATION_JOURNAL_FAILURE_INTEGRITY;
        return cc_result_error(CC_ERR_STORAGE, "Pending operation file is invalid or too large");
    }
    char *text = NULL;
    rc = journal->fs->vtable->read_text(journal->fs->self, path, &text);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    if (!text || strlen(text) != info.byte_size) {
        free(text);
        if (out_failure) *out_failure = CC_OPERATION_JOURNAL_FAILURE_INTEGRITY;
        return cc_result_error(CC_ERR_STORAGE, "Pending operation file was truncated");
    }
    rc = parse_entry(text, out, out_failure);
    free(text);
    return rc;
}

/**
 * @brief 在数组已满时从八槽起倍增，保证可追加一条内存记录。
 * @pre 外层独占打开过程或持有 journal->mutex。
 * @param[in,out] journal 拥有数组的日志，扩容只改变 items 和 capacity，既有记录保持有效。
 * @return 原容量足够或扩容成功为 CC_OK，容量溢出为 CC_ERR_LIMIT_EXCEEDED，分配失败为 CC_ERR_OUT_OF_MEMORY，失败保留原数组。
 */
static cc_result_t ensure_capacity(cc_operation_journal_t *journal)
{
    if (journal->count < journal->capacity) return cc_result_ok();
    size_t next = journal->capacity ? journal->capacity * 2U : 8U;
    if (next < journal->capacity || next > SIZE_MAX / sizeof(*journal->items))
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Pending operation capacity overflow");
    cc_pending_operation_t *items = realloc(journal->items, next * sizeof(*items));
    if (!items) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot grow pending operations");
    journal->items = items;
    journal->capacity = next;
    return cc_result_ok();
}

/**
 * @brief 文件系统声明目录同步能力时同步专用目录。
 * @pre 调用者持有 journal->mutex，open 已验证能力与回调匹配。
 * @param[in,out] journal 借用的日志及文件系统。
 * @return 支持目录同步时原样返回回调结果，否则为 CC_OK，实际掉电持久性取决于文件系统实现。
 */
static cc_result_t sync_directory(cc_operation_journal_t *journal)
{
    if (!(journal->fs->capabilities & CC_FS_CAP_DIR_SYNC)) return cc_result_ok();
    return journal->fs->vtable->sync_dir(journal->fs->self, journal->directory);
}

/**
 * @brief 逐字比较操作、会话、调用 ID、工具名称及参数文本。
 * @param[in] a 借用的有效记录，持久化字符串均非 NULL。
 * @param[in] b 借用的有效记录，持久化字符串均非 NULL。
 * @return 全部持久化字段相同为一，否则为零，进程标志独立于比较。
 */
static int same_persisted_operation(const cc_pending_operation_t *a, const cc_pending_operation_t *b)
{
    return strcmp(a->operation_id, b->operation_id) == 0 &&
           strcmp(a->session_id, b->session_id) == 0 &&
           strcmp(a->call.id, b->call.id) == 0 &&
           strcmp(a->call.name, b->call.name) == 0 &&
           strcmp(a->call.arguments_json, b->call.arguments_json) == 0;
}

/**
 * @brief 重复导入时重读磁盘记录，确认完整身份后同步文件及支持的目录。
 *
 * 较早安装可能返回错误却留下内存 UNKNOWN，仅凭内存记录仍需核对磁盘安装结果。
 * @pre 调用者持有日志锁，目录由同一所有者独占。
 * @param[in,out] journal 借用的日志及文件系统，保留现有内存状态。
 * @param[in] entry 借用的既有内存记录，用其 ID 构造正式路径并比较全部持久化字段。
 * @return 磁盘匹配且同步完成为 CC_OK，内容冲突为 CC_ERR_STORAGE，路径分配、文件读取及同步错误向上传递。
 */
static cc_result_t verify_existing_import(cc_operation_journal_t *journal,
                                           const cc_pending_operation_t *entry)
{
    char *path = entry_path(journal, entry->operation_id, ".json");
    if (!path) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate operation path");
    cc_pending_operation_t persisted = {0};
    cc_result_t rc = read_entry(journal, path, &persisted, NULL);
    if (rc.code == CC_OK && !same_persisted_operation(entry, &persisted))
        rc = cc_result_error(CC_ERR_STORAGE, "Pending operation conflicts with the stored record");
    if (rc.code == CC_OK) rc = journal->fs->vtable->sync_file(journal->fs->self, path);
    if (rc.code == CC_OK) rc = sync_directory(journal);
    entry_cleanup(&persisted);
    free(path);
    return rc;
}

/**
 * @brief 校验端口能力，创建锁并从目录加载全部可识别的正式日志文件。
 *
 * .tmp 和其他名称跳过，匹配 op-*.json 的候选必须通过名称、内容、CRC 和会话/调用 ID 唯一性检查。
 * 所有加载项的进程标志为零，超出准入阈值的历史项继续保留，任一读取错误清理整次装配。
 * @param[in] fs 借用至实例销毁的文件系统及 vtable，须提供声明的同步与替换能力。
 * @param[in] directory 已有专用目录的非空路径，同步借用并复制，目录由外层独占。
 * @param[in] max_pending 普通新操作的条数阈值，零使用默认 64 条。
 * @param[out] out_journal 必填，先置 NULL，成功接收拥有的日志。
 * @param[out] out_failure 可选，先置 OTHER，只有明确损坏时改为 INTEGRITY。
 * @return 成功为 CC_OK，输出非法为 CC_ERR_INVALID_ARGUMENT，能力/路径非法为 CC_ERR_UNSUPPORTED，
 * 记录损坏或重复为 CC_ERR_STORAGE，分配、目录读取和锁创建错误向上传递。
 */
cc_result_t cc_operation_journal_open_detailed(cc_filesystem_t *fs, const char *directory,
    size_t max_pending, cc_operation_journal_t **out_journal, cc_operation_journal_failure_t *out_failure)
{
    if (out_failure) *out_failure = CC_OPERATION_JOURNAL_FAILURE_OTHER;
    if (!out_journal) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null journal output");
    *out_journal = NULL;
    uint64_t required = CC_FS_CAP_ATOMIC_REPLACE | CC_FS_CAP_FILE_SYNC | CC_FS_CAP_STAT;
    if (!fs || fs->size < sizeof(*fs) || !directory || !directory[0] || !fs->vtable ||
        fs->vtable->size < offsetof(cc_filesystem_vtable_t, sync_dir) + sizeof(fs->vtable->sync_dir) ||
        (fs->capabilities & required) != required || !fs->vtable->read_text ||
        !fs->vtable->write_text || !fs->vtable->list_dir || !fs->vtable->remove ||
        !fs->vtable->stat || !fs->vtable->atomic_replace || !fs->vtable->sync_file ||
        ((fs->capabilities & CC_FS_CAP_DIR_SYNC) && !fs->vtable->sync_dir))
        return cc_result_error(CC_ERR_UNSUPPORTED, "Filesystem cannot safely store pending operations");
    cc_operation_journal_t *journal = calloc(1, sizeof(*journal));
    if (!journal) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate operation journal");
    journal->fs = fs;
    journal->directory = cc_copy_string(directory);
    journal->max_pending = max_pending ? max_pending : CC_OPERATION_JOURNAL_DEFAULT_LIMIT;
    if (!journal->directory) {
        free(journal);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate journal directory");
    }
    cc_result_t rc = cc_mutex_create(&journal->mutex);
    if (rc.code != CC_OK) {
        free(journal->directory);
        free(journal);
        return rc;
    }
    cc_result_free(&rc);
    char **names = NULL;
    size_t count = 0;
    rc = fs->vtable->list_dir(fs->self, directory, &names, &count);
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        for (size_t i = 0; i < count; i++) {
            const char *name = names[i];
            size_t len = strlen(name);
            if (strncmp(name, "op-", 3U) != 0 || len < 5U ||
                strcmp(name + len - 5U, ".json") != 0) continue;
            if (len != 44U) {
                if (out_failure) *out_failure = CC_OPERATION_JOURNAL_FAILURE_INTEGRITY;
                rc = cc_result_error(CC_ERR_STORAGE, "Invalid pending operation filename");
                break;
            }
            char id[40];
            memcpy(id, name, 39U);
            id[39] = '\0';
            if (!valid_operation_id(id)) {
                if (out_failure) *out_failure = CC_OPERATION_JOURNAL_FAILURE_INTEGRITY;
                rc = cc_result_error(CC_ERR_STORAGE, "Invalid pending operation filename");
                break;
            }
            char *path = entry_path(journal, id, ".json");
            if (!path) {
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate journal path");
                break;
            }
            cc_pending_operation_t entry;
            rc = read_entry(journal, path, &entry, out_failure);
            free(path);
            if (rc.code != CC_OK) break;
            cc_result_free(&rc);
            if (strcmp(entry.operation_id, id) != 0) {
                if (out_failure) *out_failure = CC_OPERATION_JOURNAL_FAILURE_INTEGRITY;
                entry_cleanup(&entry);
                rc = cc_result_error(CC_ERR_STORAGE, "Pending operation filename does not match record");
                break;
            }
            for (size_t j = 0; j < journal->count; j++) {
                if (strcmp(journal->items[j].session_id, entry.session_id) == 0 &&
                    strcmp(journal->items[j].call.id, entry.call.id) == 0) {
                    if (out_failure) *out_failure = CC_OPERATION_JOURNAL_FAILURE_INTEGRITY;
                    rc = cc_result_error(CC_ERR_STORAGE, "Pending operation identity is duplicated");
                    break;
                }
            }
            if (rc.code != CC_OK) { entry_cleanup(&entry); break; }
            rc = ensure_capacity(journal);
            if (rc.code != CC_OK) { entry_cleanup(&entry); break; }
            cc_result_free(&rc);
            journal->items[journal->count++] = entry;
        }
    }
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
    if (rc.code != CC_OK) {
        cc_operation_journal_destroy(journal);
        return rc;
    }
    *out_journal = journal;
    return cc_result_ok();
}

/**
 * @brief 保留原打开接口，通过详细入口加载日志并省略失败分类。
 * @param[in] fs 借用至日志销毁的文件系统及 vtable。
 * @param[in] directory 已有专用目录的非空路径，同步借用并复制。
 * @param[in] max_pending 普通新操作条数阈值，零使用默认 64 条。
 * @param[out] out_journal 必填，先置 NULL，成功取得拥有的实例。
 * @return 原样返回详细打开入口结果，原调用者保留既有错误处理方式。
 */
cc_result_t cc_operation_journal_open(cc_filesystem_t *fs, const char *directory,
                                     size_t max_pending, cc_operation_journal_t **out_journal)
{
    return cc_operation_journal_open_detailed(fs, directory, max_pending, out_journal, NULL);
}

/**
 * @brief 清理拥有记录、互斥锁及目录副本，最后释放实例。
 * @pre 外层已关闭入口并排空日志及文件系统回调，借用 fs 仍存活。
 * @param[in,out] journal 待销毁实例，允许 NULL，也支持打开失败时的部分装配。
 * @post 实例内存寿命结束，磁盘记录继续保留。
 */
void cc_operation_journal_destroy(cc_operation_journal_t *journal)
{
    if (!journal) return;
    cc_pending_operation_array_free(journal->items, journal->count);
    if (journal->mutex) cc_mutex_destroy(journal->mutex);
    free(journal->directory);
    free(journal);
}

/**
 * @brief 按操作 ID 逐字查找当前内存记录。
 * @pre 调用者持有日志锁。
 * @param[in] journal 借用的有效日志。
 * @param[in] operation_id 同步借用的非 NULL ID。
 * @return 找到时返回零基索引，否则返回 journal->count。
 */
static size_t find_entry(cc_operation_journal_t *journal, const char *operation_id)
{
    for (size_t i = 0; i < journal->count; i++)
        if (strcmp(journal->items[i].operation_id, operation_id) == 0) return i;
    return journal->count;
}

/**
 * @brief 在同一日志锁内执行新操作准入或中断导入，并持久化其身份及调用。
 *
 * 导入可越过同名 UNKNOWN 和条数检查，重复导入先验证已提交文件。普通入口检查同名 UNKNOWN，
 * 只有显式只读模式越过条数阈值。UUID 最多尝试八次避开 RAM 中的 ID。
 * 写临时文件后同步、回读精确比较，再原子替换并同步目录。替换已尝试的失败登记 UNKNOWN，
 * 成功才移交操作 ID。失败清理会尝试移除本次已写的临时文件，包含替换已经尝试的情况。
 * @pre 目录由外层唯一所有者保管，锁内端口回调遵守同一日志的重入约束。
 * @param[in,out] journal 借用的日志及文件系统。
 * @param[in] session_id 非空会话 ID，同步借用并复制。
 * @param[in] call 借用的调用，id/name 非空，arguments_json 非 NULL，原文本复制保存。
 * @param[out] out_operation_id 必填，只要指针存在就先置 NULL，成功输出拥有的 ID 供调用者 free。
 * @param[in] importing 非零使用导入模式，新记录 in_progress 为零，重复成功保留现有标志。
 * @param[in] read_only 非零允许普通新操作越过条数阈值，由调用者保证能力可信。
 * @return 成功为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，调用身份或同名阻断为 CC_ERR_INVALID_STATE，
 * 字段/条数/数组超限为 CC_ERR_LIMIT_EXCEEDED，UUID 碰撞重试耗尽为 CC_ERR_PLATFORM，其余分配及存储错误向上传递。
 */
static cc_result_t journal_begin(cc_operation_journal_t *journal, const char *session_id,
                                 const cc_tool_call_t *call, char **out_operation_id,
                                 int importing, int read_only)
{
    if (out_operation_id) *out_operation_id = NULL;
    if (!journal || !session_id || !session_id[0] || !call || !call->id || !call->id[0] ||
        !call->name || !call->name[0] || !call->arguments_json || !out_operation_id)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pending operation request");
    if (strlen(session_id) + strlen(call->id) + strlen(call->name) + strlen(call->arguments_json) >
        JOURNAL_MAX_FIELDS_BYTES)
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Pending operation fields are too large");
    cc_mutex_lock(journal->mutex);
    cc_result_t rc = cc_result_ok();
    for (size_t i = 0; i < journal->count; i++) {
        cc_pending_operation_t *entry = &journal->items[i];
        if (strcmp(entry->session_id, session_id) == 0 && strcmp(entry->call.id, call->id) == 0) {
            if (importing && strcmp(entry->call.name, call->name) == 0 &&
                strcmp(entry->call.arguments_json, call->arguments_json) == 0) {
                rc = verify_existing_import(journal, entry);
                if (rc.code == CC_OK) {
                    *out_operation_id = cc_copy_string(entry->operation_id);
                    if (!*out_operation_id) rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy operation id");
                }
                goto done;
            }
            rc = cc_result_error(CC_ERR_INVALID_STATE, "Tool call id already has an operation");
            goto done;
        }
        if (!importing && !entry->in_progress && strcmp(entry->call.name, call->name) == 0) {
            rc = cc_result_error(CC_ERR_INVALID_STATE, "Tool has an unresolved operation");
            goto done;
        }
    }
    if (!importing && !read_only && journal->count >= journal->max_pending) {
        rc = cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Pending operation limit reached; reconcile existing operations");
        goto done;
    }
    rc = ensure_capacity(journal);
    if (rc.code != CC_OK) goto done;
    cc_result_free(&rc);
    cc_pending_operation_t entry = {0};
    for (int attempt = 0; attempt < 8; attempt++) {
        rc = cc_id_generate_uuid_v4("op-", &entry.operation_id);
        if (rc.code != CC_OK) goto done;
        if (find_entry(journal, entry.operation_id) == journal->count) break;
        free(entry.operation_id);
        entry.operation_id = NULL;
    }
    if (!entry.operation_id) {
        rc = cc_result_error(CC_ERR_PLATFORM, "Cannot generate a unique operation id");
        goto done;
    }
    entry.session_id = cc_copy_string(session_id);
    rc = cc_tool_call_copy(call, &entry.call);
    if (!entry.session_id || rc.code != CC_OK) {
        if (rc.code == CC_OK) rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy operation session");
        entry_cleanup(&entry);
        goto done;
    }
    entry.in_progress = importing ? 0 : 1;
    char *operation_id = cc_copy_string(entry.operation_id);
    char *tmp = entry_path(journal, entry.operation_id, ".tmp");
    char *path = entry_path(journal, entry.operation_id, ".json");
    char *text = NULL;
    if (!operation_id || !tmp || !path) {
        rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate operation paths");
    } else {
        rc = serialize_entry(&entry, &text);
    }
    cc_filesystem_stat_t info = {.size = sizeof(info)};
    if (rc.code == CC_OK) rc = journal->fs->vtable->stat(journal->fs->self, path, &info);
    if (rc.code == CC_OK && info.type != CC_FS_ENTRY_MISSING)
        rc = cc_result_error(CC_ERR_STORAGE, "Pending operation path already exists");
    if (rc.code == CC_OK) rc = journal->fs->vtable->stat(journal->fs->self, tmp, &info);
    if (rc.code == CC_OK && info.type != CC_FS_ENTRY_MISSING)
        rc = cc_result_error(CC_ERR_STORAGE, "Pending operation temporary path already exists");
    int attempted_write = 0;
    int install_attempted = 0;
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        attempted_write = 1;
        rc = journal->fs->vtable->write_text(journal->fs->self, tmp, text);
    }
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = journal->fs->vtable->sync_file(journal->fs->self, tmp);
    }
    if (rc.code == CC_OK) {
        cc_pending_operation_t checked;
        cc_result_free(&rc);
            rc = read_entry(journal, tmp, &checked, NULL);
        if (rc.code == CC_OK) {
            if (!same_persisted_operation(&checked, &entry)) {
                cc_result_free(&rc);
                rc = cc_result_error(CC_ERR_STORAGE, "Pending operation verification failed");
            }
            entry_cleanup(&checked);
        }
    }
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        install_attempted = 1;
        rc = journal->fs->vtable->atomic_replace(journal->fs->self, tmp, path);
    }
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = sync_directory(journal);
    }
    if (rc.code == CC_OK) {
        journal->items[journal->count++] = entry;
        *out_operation_id = operation_id;
        operation_id = NULL;
    } else if (install_attempted) {
        /* 替换报错时正式文件可能已安装，保留 UNKNOWN 以阻断同名工具。 */
        entry.in_progress = 0;
        journal->items[journal->count++] = entry;
    } else {
        entry_cleanup(&entry);
    }
    if (rc.code != CC_OK && attempted_write) {
        cc_result_t cleanup = journal->fs->vtable->remove(journal->fs->self, tmp);
        cc_result_free(&cleanup);
    }
    free(operation_id);
    free(tmp);
    free(path);
    free(text);
done:
    cc_mutex_unlock(journal->mutex);
    return rc;
}

/**
 * @brief 通过普通准入模式登记工具执行意图，检查同名 UNKNOWN 及条数阈值。
 * @param[in,out] journal 借用的日志及文件系统，检查和持久化由内部锁串行保护。
 * @param[in] session_id 同步借用并复制的非空会话 ID。
 * @param[in] call 同步借用的调用，id/name 非空，arguments_json 非 NULL。
 * @param[out] out_operation_id 必填，存在时先置 NULL，成功取得拥有的 ID 供调用者 free。
 * @return 返回 journal_begin 的准入及写入结果，替换尝试后失败可保留 UNKNOWN 且输出为空。
 */
cc_result_t cc_operation_journal_begin(cc_operation_journal_t *journal, const char *session_id,
                                       const cc_tool_call_t *call, char **out_operation_id)
{
    return journal_begin(journal, session_id, call, out_operation_id, 0, 0);
}

/**
 * @brief 通过显式只读模式登记操作，越过条数阈值但继续检查调用身份与同名 UNKNOWN。
 * @pre 外层已核实工具只读能力，未声明 READ_ONLY 的工具执行器调用普通 begin。
 * @param[in,out] journal 借用的日志及文件系统。
 * @param[in] session_id 同步借用并复制的非空会话 ID。
 * @param[in] call 同步借用的调用，id/name 非空，arguments_json 非 NULL。
 * @param[out] out_operation_id 必填，存在时先置 NULL，成功取得拥有的 ID 供调用者 free。
 * @return 返回 journal_begin 的身份校验和持久化结果，失败现场及 UNKNOWN 语义与普通入口相同。
 */
cc_result_t cc_operation_journal_begin_readonly(cc_operation_journal_t *journal, const char *session_id,
                                                const cc_tool_call_t *call, char **out_operation_id)
{
    return journal_begin(journal, session_id, call, out_operation_id, 0, 1);
}

/**
 * @brief 导入待核对操作，重复身份先校验磁盘并同步，新增记录初始化为 UNKNOWN。
 * @param[in,out] journal 借用的日志及文件系统，导入越过同名阻断和条数阈值。
 * @param[in] session_id 同步借用并复制的非空会话 ID。
 * @param[in] call 同步借用的调用，id/name 非空，arguments_json 非 NULL。
 * @param[out] out_operation_id 必填，存在时先置 NULL，成功输出新建或既有记录的 ID 副本，调用者 free。
 * @return 返回 journal_begin 的导入、身份或存储结果，重复成功保留既有进程标志。
 */
cc_result_t cc_operation_journal_import_unknown(cc_operation_journal_t *journal,
                                                const char *session_id,
                                                const cc_tool_call_t *call,
                                                char **out_operation_id)
{
    return journal_begin(journal, session_id, call, out_operation_id, 1, 0);
}

/**
 * @brief 锁内清除 in_progress，保留人工核对标志和持久化记录。
 * @param[in,out] journal 借用的日志。
 * @param[in] operation_id 同步借用的非 NULL 操作 ID。
 * @return 成功为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，内存 ID 缺失为 CC_ERR_NOT_FOUND。
 */
cc_result_t cc_operation_journal_mark_unknown(cc_operation_journal_t *journal, const char *operation_id)
{
    if (!journal || !operation_id) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid operation id");
    cc_mutex_lock(journal->mutex);
    size_t index = find_entry(journal, operation_id);
    if (index == journal->count) {
        cc_mutex_unlock(journal->mutex);
        return cc_result_error(CC_ERR_NOT_FOUND, "Pending operation not found");
    }
    journal->items[index].in_progress = 0;
    cc_mutex_unlock(journal->mutex);
    return cc_result_ok();
}

/**
 * @brief 锁内拒绝执行中或已核对占用的记录，为 UNKNOWN 设置人工核对占用。
 * @param[in,out] journal 借用的日志，本次占用只保存在内存。
 * @param[in] operation_id 同步借用的非 NULL 操作 ID。
 * @return 成功为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，缺失为 CC_ERR_NOT_FOUND，状态冲突为 CC_ERR_INVALID_STATE。
 */
cc_result_t cc_operation_journal_claim_reconciliation(
    cc_operation_journal_t *journal, const char *operation_id)
{
    if (!journal || !operation_id) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid operation id");
    cc_mutex_lock(journal->mutex);
    size_t index = find_entry(journal, operation_id);
    if (index == journal->count) {
        cc_mutex_unlock(journal->mutex);
        return cc_result_error(CC_ERR_NOT_FOUND, "Pending operation not found");
    }
    if (journal->items[index].in_progress || journal->items[index].reconciling) {
        cc_mutex_unlock(journal->mutex);
        return cc_result_error(CC_ERR_INVALID_STATE, "Operation is running or already being reconciled");
    }
    journal->items[index].reconciling = 1;
    cc_mutex_unlock(journal->mutex);
    return cc_result_ok();
}

/**
 * @brief 锁内清除 reconciling，保留 in_progress 及记录内容。
 * @param[in,out] journal 借用的日志，调用者负责申领顺序，可重复释放仍存在的记录。
 * @param[in] operation_id 同步借用的非 NULL 操作 ID。
 * @return 成功为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，缺失为 CC_ERR_NOT_FOUND。
 */
cc_result_t cc_operation_journal_release_reconciliation(
    cc_operation_journal_t *journal, const char *operation_id)
{
    if (!journal || !operation_id) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid operation id");
    cc_mutex_lock(journal->mutex);
    size_t index = find_entry(journal, operation_id);
    if (index == journal->count) {
        cc_mutex_unlock(journal->mutex);
        return cc_result_error(CC_ERR_NOT_FOUND, "Pending operation not found");
    }
    journal->items[index].reconciling = 0;
    cc_mutex_unlock(journal->mutex);
    return cc_result_ok();
}

/**
 * @brief 锁内删除正式文件并同步目录，成功后清理内存项并用末项填补空位。
 * @pre 外层已持久化结果或完成用户核对，并协调执行/申领生命周期。
 * @param[in,out] journal 借用的日志及文件系统，删除/同步失败保留记录并清零 in_progress，路径分配失败保持原状态。
 * @param[in] operation_id 同步借用的非 NULL 操作 ID。
 * @return 成功为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，内存缺失为 CC_ERR_NOT_FOUND，
 * 路径分配、删除或同步错误向上传递，删除已完成但同步失败时可再次 finish 核对缺失并重试同步。
 */
cc_result_t cc_operation_journal_finish(cc_operation_journal_t *journal, const char *operation_id)
{
    if (!journal || !operation_id) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid operation id");
    cc_mutex_lock(journal->mutex);
    size_t index = find_entry(journal, operation_id);
    if (index == journal->count) {
        cc_mutex_unlock(journal->mutex);
        return cc_result_error(CC_ERR_NOT_FOUND, "Pending operation not found");
    }
    char *path = entry_path(journal, operation_id, ".json");
    if (!path) {
        cc_mutex_unlock(journal->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate operation path");
    }
    cc_result_t rc = journal->fs->vtable->remove(journal->fs->self, path);
    if (rc.code != CC_OK) {
        /* 前次删除可能已完成而目录同步失败，路径已缺失时继续重试目录同步。 */
        cc_filesystem_stat_t info = {0};
        info.size = sizeof(info);
        cc_result_t stat_rc = journal->fs->vtable->stat(journal->fs->self, path, &info);
        if (stat_rc.code == CC_OK && info.type == CC_FS_ENTRY_MISSING) {
            cc_result_free(&rc);
            rc = cc_result_ok();
        }
        cc_result_free(&stat_rc);
    }
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = sync_directory(journal);
    }
    free(path);
    if (rc.code == CC_OK) {
        entry_cleanup(&journal->items[index]);
        journal->count--;
        if (index != journal->count) journal->items[index] = journal->items[journal->count];
    } else {
        journal->items[index].in_progress = 0;
    }
    cc_mutex_unlock(journal->mutex);
    return rc;
}

/**
 * @brief 锁内跨会话查找同名且 in_progress 为零的记录。
 * @param[in] journal 借用的日志，人工核对占用也属于阻断状态。
 * @param[in] tool_name 同步借用的非 NULL 名称，逐字匹配。
 * @param[out] out_blocked 必填，参数有效后置零，存在匹配项时置一。
 * @return 参数有效为 CC_OK，其余为 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t cc_operation_journal_is_blocked(cc_operation_journal_t *journal,
                                            const char *tool_name, int *out_blocked)
{
    if (!journal || !tool_name || !out_blocked)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid tool status request");
    *out_blocked = 0;
    cc_mutex_lock(journal->mutex);
    for (size_t i = 0; i < journal->count; i++) {
        if (!journal->items[i].in_progress && strcmp(journal->items[i].call.name, tool_name) == 0) {
            *out_blocked = 1;
            break;
        }
    }
    cc_mutex_unlock(journal->mutex);
    return cc_result_ok();
}

/**
 * @brief 锁内查找并最多复制一条 UNKNOWN，包括人工核对中的记录。
 * @param[in] journal 借用的日志，当前数组首个匹配项胜出。
 * @param[in] session_id 同步借用的非 NULL 会话 ID。
 * @param[in] tool_call_id 可选调用 ID，NULL 匹配任意调用。
 * @param[out] out_call 可选的空输出结构，参数有效后清零，成功副本用 cc_tool_call_cleanup 清理。
 * @param[out] out_operation_id 可选输出，参数有效后置 NULL，成功取得拥有 ID 供调用者 free。
 * @param[out] out_found 必填，参数有效后置零，全部所需副本成功后置一。
 * @return 匹配或未匹配均为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，复制错误向上传递并清理部分副本。
 */
cc_result_t cc_operation_journal_find_unknown(
    cc_operation_journal_t *journal, const char *session_id, const char *tool_call_id,
    cc_tool_call_t *out_call, char **out_operation_id, int *out_found)
{
    if (!journal || !session_id || !out_found)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pending operation lookup");
    *out_found = 0;
    if (out_operation_id) *out_operation_id = NULL;
    if (out_call) memset(out_call, 0, sizeof(*out_call));
    cc_mutex_lock(journal->mutex);
    cc_result_t rc = cc_result_ok();
    for (size_t i = 0; i < journal->count; i++) {
        cc_pending_operation_t *entry = &journal->items[i];
        if (entry->in_progress || strcmp(entry->session_id, session_id) != 0 ||
            (tool_call_id && strcmp(entry->call.id, tool_call_id) != 0)) continue;
        if (out_call) {
            rc = cc_tool_call_copy(&entry->call, out_call);
            if (rc.code != CC_OK) break;
        }
        if (out_operation_id) {
            *out_operation_id = cc_copy_string(entry->operation_id);
            if (!*out_operation_id) {
                if (out_call) cc_tool_call_cleanup(out_call);
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy operation id");
                break;
            }
        }
        *out_found = 1;
        break;
    }
    cc_mutex_unlock(journal->mutex);
    return rc;
}

/**
 * @brief 锁内检查会话/调用 ID，并将同 ID 不同名称或参数视为身份冲突。
 * @param[in] journal 借用的日志，所有进程状态均参与匹配。
 * @param[in] session_id 同步借用的非 NULL 会话 ID。
 * @param[in] call 借用的调用，id/name/arguments_json 均须非 NULL。
 * @param[out] out_found 必填，参数有效后置零，身份和内容完整匹配时置一。
 * @return 不存在或完整匹配为 CC_OK，内容冲突为 CC_ERR_STORAGE，参数非法为 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t cc_operation_journal_contains(cc_operation_journal_t *journal,
                                          const char *session_id, const cc_tool_call_t *call,
                                          int *out_found)
{
    if (!journal || !session_id || !call || !call->id || !call->name ||
        !call->arguments_json || !out_found)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid operation lookup");
    *out_found = 0;
    cc_mutex_lock(journal->mutex);
    cc_result_t rc = cc_result_ok();
    for (size_t i = 0; i < journal->count; i++) {
        cc_pending_operation_t *entry = &journal->items[i];
        if (strcmp(entry->session_id, session_id) == 0 &&
            strcmp(entry->call.id, call->id) == 0) {
            if (strcmp(entry->call.name, call->name) != 0 ||
                strcmp(entry->call.arguments_json, call->arguments_json) != 0)
                rc = cc_result_error(CC_ERR_STORAGE, "Pending operation conflicts with tool call identity");
            else *out_found = 1;
            break;
        }
    }
    cc_mutex_unlock(journal->mutex);
    return rc;
}

/**
 * @brief 锁内深拷贝当前全部记录及执行/核对标志，移交独立数组。
 * @param[in] journal 借用的日志，输出保持当前内存顺序。
 * @param[out] out_items 必填且无待释放内容，参数有效后置 NULL，成功数组用 cc_pending_operation_array_free 清理。
 * @param[out] out_count 必填，参数有效后置零，成功输出条数，空日志保持 NULL 和零。
 * @return 成功为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，分配及记录复制错误向上传递并清理部分列表。
 */
cc_result_t cc_operation_journal_list(cc_operation_journal_t *journal,
                                     cc_pending_operation_t **out_items, size_t *out_count)
{
    if (!journal || !out_items || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid operation list request");
    *out_items = NULL;
    *out_count = 0;
    cc_mutex_lock(journal->mutex);
    if (!journal->count) { cc_mutex_unlock(journal->mutex); return cc_result_ok(); }
    cc_pending_operation_t *items = calloc(journal->count, sizeof(*items));
    if (!items) {
        cc_mutex_unlock(journal->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate operation list");
    }
    cc_result_t rc = cc_result_ok();
    size_t copied = 0;
    for (; copied < journal->count; copied++) {
        rc = entry_copy(&journal->items[copied], &items[copied]);
        if (rc.code != CC_OK) break;
        cc_result_free(&rc);
    }
    if (rc.code == CC_OK) {
        *out_items = items;
        *out_count = journal->count;
    } else {
        cc_pending_operation_array_free(items, copied);
    }
    cc_mutex_unlock(journal->mutex);
    return rc;
}
