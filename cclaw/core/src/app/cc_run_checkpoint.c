#include "cc/app/cc_run_checkpoint.h"
#include "cc/internal/cc_alloc.h"
#include "cc/internal/cc_json_syntax_check.h"
#include "cc/ports/cc_thread.h"
#include "cc/util/cc_json.h"
#include "cJSON.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RUN_MAX_FILE_BYTES (512U * 1024U) /**< 完整编码记录的字节上限，不计 NUL。 */
#define RUN_MAX_INPUT_BYTES (64U * 1024U) /**< 原输入、推理和每条参数原文各自的字节上限。 */
#define RUN_MAX_CALLS 8U /**< 单个持久计划最多八项。 */

/** @brief 进程内活动声明，拥有运行和会话 ID，由存储链表保管。 */
typedef struct active_run_claim {
    char *run_id;
    char *session_id;
    struct active_run_claim *next;
} active_run_claim_t;

struct cc_run_checkpoint_store {
    cc_filesystem_t *fs; /**< 借用至实例销毁的文件系统句柄。 */
    char *directory; /**< 实例拥有的目录副本，目录本体由外层独占。 */
    cc_mutex_t mutex; /**< 串行文件操作与声明链表访问。 */
    active_run_claim_t *claims; /**< 实例拥有的 RAM 声明链表。 */
};

/**
 * @brief 释放拥有检查点的所有字段及工具调用列表，再清零结构。
 * @param[in,out] checkpoint 拥有结果，可为 NULL，字段须为可释放副本或已清零。
 * @post 非空结构全零，所有借出的字段指针失效。
 */
void cc_run_checkpoint_cleanup(cc_run_checkpoint_t *checkpoint)
{
    if (!checkpoint) return;
    free(checkpoint->run_id);
    free(checkpoint->session_id);
    free(checkpoint->user_message_id);
    free(checkpoint->user_input);
    free(checkpoint->reasoning_content);
    free(checkpoint->retry_after_call_id);
    cc_tool_call_list_cleanup(&checkpoint->planned_calls);
    memset(checkpoint, 0, sizeof(*checkpoint));
}

/**
 * @brief 核对 run- 前缀、40 字节总长度及小写 UUID 外形。
 * @param[in] id 借用运行 ID，可为 NULL。
 * @return 前缀、连字符位置及十六进制字符均匹配返回一，其余为零。
 */
static int valid_run_id(const char *id)
{
    if (!id || strlen(id) != 40U || strncmp(id, "run-", 4U) != 0) return 0;
    for (size_t i = 4; i < 40U; i++) {
        char c = id[i];
        if (i == 12U || i == 17U || i == 22U || i == 27U) {
            if (c != '-') return 0;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

/**
 * @brief 校验运行 ID 后追加 -user，生成稳定原输入消息 ID。
 * @param[in] run_id 借用的合法 40 字节运行 ID。
 * @param[out] out_id 必填，先置 NULL，成功拥有 45 字节正文及 NUL，由调用者 free。
 * @return 参数或外形错误返回 CC_ERR_INVALID_ARGUMENT，分配失败返回 CC_ERR_OUT_OF_MEMORY，成功返回 CC_OK。
 */
cc_result_t cc_run_checkpoint_derive_user_message_id(const char *run_id, char **out_id)
{
    if (!out_id) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid run input id output");
    *out_id=NULL;
    if (!valid_run_id(run_id)) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid run id");
    char *id=malloc(46U);
    if (!id) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot allocate run input id");
    snprintf(id,46U,"%s-user",run_id);
    *out_id=id; return cc_result_ok();
}

/**
 * @brief 对 NUL 之前的原文字节计算 CRC32。
 * @param[in] text 非空借用 C 字符串，按原字节顺序计算。
 * @return 初值和末次异或均为全一的 CRC32，使用反射多项式 0xedb88320。
 */
static uint32_t crc32_text(const char *text)
{
    uint32_t crc = 0xffffffffU;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        crc ^= *p;
        for (unsigned i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
    }
    return crc ^ 0xffffffffU;
}

/**
 * @brief 复用 SDK 完整尾部与 Unicode 转义检查，保留本模块解析指针契约。
 * @param[in] text 借用的 NUL 结尾 JSON，解析期间有效。
 * @return 成功取得拥有树，由调用者 cJSON_Delete，语法或分配失败返回 NULL。
 * @note 此 helper 沿用原模块的解析失败折叠方式，返回 NULL 本身不能区分损坏与资源不足。
 */
static cJSON *parse_complete_json(const char *text)
{
    cc_json_value_t *value = NULL;
    cc_result_t rc = cc_json_parse_strict(text, &value);
    cc_result_free(&rc);
    return (cJSON *)value;
}

/**
 * @brief 核对字段预算、阶段与游标组合，以及计划调用的唯一身份和参数对象格式。
 * @param[in] checkpoint 借用输入，可为 NULL，调用期间所有列表和字符串保持稳定。
 * @return 字段、MODEL/TOOLS 组合及完整参数 JSON 对象均合法返回一，错误或参数解析资源不足返回零。
 * 会话、消息、调用和重试 ID 最多 512 字节，原输入、推理和单项参数最多 64 KiB。
 * TOOLS 至少一项且最多八项，重试等待须对应未完成游标并区别于当前替代调用 ID。
 */
static int valid_checkpoint(const cc_run_checkpoint_t *checkpoint)
{
    if (!checkpoint || !valid_run_id(checkpoint->run_id) || !checkpoint->session_id ||
        !checkpoint->session_id[0] || strlen(checkpoint->session_id) > 512U ||
        !checkpoint->user_message_id || !checkpoint->user_message_id[0] ||
        strlen(checkpoint->user_message_id) > 512U || !checkpoint->user_input ||
        !checkpoint->user_input[0] || strlen(checkpoint->user_input) > RUN_MAX_INPUT_BYTES ||
        (checkpoint->reasoning_content && strlen(checkpoint->reasoning_content) > RUN_MAX_INPUT_BYTES) ||
        checkpoint->step == 0 || checkpoint->planned_calls.count > RUN_MAX_CALLS ||
        checkpoint->next_call > checkpoint->planned_calls.count ||
        (checkpoint->retry_after_call_id && (!checkpoint->retry_after_call_id[0] ||
         strlen(checkpoint->retry_after_call_id) > 512U))) return 0;
    if (checkpoint->phase == CC_RUN_CHECKPOINT_MODEL) {
        if (checkpoint->planned_calls.count || checkpoint->next_call || checkpoint->retry_after_call_id) return 0;
    } else if (checkpoint->phase == CC_RUN_CHECKPOINT_TOOLS) {
        if (!checkpoint->planned_calls.count || !checkpoint->planned_calls.items) return 0;
        if (checkpoint->retry_after_call_id && checkpoint->next_call >= checkpoint->planned_calls.count) return 0;
    } else return 0;
    for (size_t i = 0; i < checkpoint->planned_calls.count; i++) {
        const cc_tool_call_t *call = &checkpoint->planned_calls.items[i];
        if (!call->id || !call->id[0] || strlen(call->id) > 512U ||
            !call->name || !call->name[0] || strlen(call->name) > 512U ||
            !call->arguments_json || strlen(call->arguments_json) > RUN_MAX_INPUT_BYTES) return 0;
        cJSON *arguments = parse_complete_json(call->arguments_json);
        int arguments_valid = cJSON_IsObject(arguments);
        cJSON_Delete(arguments);
        if (!arguments_valid) return 0;
        for (size_t j = 0; j < i; j++)
            if (strcmp(call->id, checkpoint->planned_calls.items[j].id) == 0) return 0;
    }
    if (checkpoint->retry_after_call_id &&
        strcmp(checkpoint->retry_after_call_id,
               checkpoint->planned_calls.items[checkpoint->next_call].id) == 0) return 0;
    return 1;
}

/**
 * @brief 拼接目录、运行 ID 和指定后缀，分配完整路径副本。
 * @param[in] store 非空存储，借用其稳定目录。
 * @param[in] id 非空借用运行 ID，由调用方先验证外形。
 * @param[in] suffix 非空借用后缀，本模块使用 .json、.json.bak 和 .tmp。
 * @return 成功取得拥有路径，由调用者 free，长度溢出或分配失败返回 NULL。
 */
static char *checkpoint_path(cc_run_checkpoint_store_t *store, const char *id, const char *suffix)
{
    size_t directory_len = strlen(store->directory), id_len = strlen(id), suffix_len = strlen(suffix);
    if (directory_len > SIZE_MAX - id_len - suffix_len - 2U) return NULL;
    char *path = malloc(directory_len + id_len + suffix_len + 2U);
    if (path) snprintf(path, directory_len + id_len + suffix_len + 2U, "%s/%s%s",
                       store->directory, id, suffix);
    return path;
}

/**
 * @brief 编码计划 payload，按其紧凑 JSON 原文计算 CRC，再封装版本一记录。
 * @param[in] checkpoint 借用输入，先核对全部字段及计划约束。
 * @param[out] out_text 必填，先置 NULL，成功拥有完整记录副本，调用者 free。
 * @return 字段或参数验证失败返回 CC_ERR_INVALID_ARGUMENT，分配失败返回 CC_ERR_OUT_OF_MEMORY，
 * 编码后超过 512 KiB 返回 CC_ERR_LIMIT_EXCEEDED，成功返回 CC_OK。
 * 中间 cJSON 树及打印缓冲均在本函数清理，进程内来源标志保持在 RAM。
 */
static cc_result_t encode_checkpoint(const cc_run_checkpoint_t *checkpoint, char **out_text)
{
    *out_text = NULL;
    if (!valid_checkpoint(checkpoint))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run checkpoint fields");
    cJSON *payload = cJSON_CreateObject();
    cJSON *calls = cJSON_CreateArray();
    if (!payload || !calls) {
        cJSON_Delete(payload); cJSON_Delete(calls);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate run checkpoint");
    }
    int good = cJSON_AddStringToObject(payload, "run_id", checkpoint->run_id) &&
        cJSON_AddStringToObject(payload, "session_id", checkpoint->session_id) &&
        cJSON_AddStringToObject(payload, "user_message_id", checkpoint->user_message_id) &&
        cJSON_AddStringToObject(payload, "user_input", checkpoint->user_input) &&
        cJSON_AddStringToObject(payload, "reasoning_content",
                                checkpoint->reasoning_content ? checkpoint->reasoning_content : "") &&
        cJSON_AddStringToObject(payload, "retry_after_call_id",
                                checkpoint->retry_after_call_id ? checkpoint->retry_after_call_id : "") &&
        cJSON_AddNumberToObject(payload, "step", (double)checkpoint->step) &&
        cJSON_AddNumberToObject(payload, "phase", (double)checkpoint->phase) &&
        cJSON_AddNumberToObject(payload, "next_call", (double)checkpoint->next_call);
    for (size_t i = 0; good && i < checkpoint->planned_calls.count; i++) {
        const cc_tool_call_t *call = &checkpoint->planned_calls.items[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) { good = 0; break; }
        good = cJSON_AddStringToObject(item, "id", call->id) &&
            cJSON_AddStringToObject(item, "name", call->name) &&
            cJSON_AddStringToObject(item, "arguments_json", call->arguments_json);
        if (good) good = cJSON_AddItemToArray(calls, item);
        if (!good) cJSON_Delete(item);
    }
    if (good) { good = cJSON_AddItemToObject(payload, "calls", calls); if (good) calls = NULL; }
    cJSON_Delete(calls);
    if (!good) { cJSON_Delete(payload); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot build run checkpoint"); }
    char *payload_text = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (!payload_text) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode run checkpoint");
    char checksum[9];
    snprintf(checksum, sizeof(checksum), "%08lx", (unsigned long)crc32_text(payload_text));
    cJSON *envelope = cJSON_CreateObject();
    good = envelope && cJSON_AddNumberToObject(envelope, "version", 1) &&
        cJSON_AddStringToObject(envelope, "payload", payload_text) &&
        cJSON_AddStringToObject(envelope, "crc32", checksum);
    cJSON_free(payload_text);
    if (!good) { cJSON_Delete(envelope); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode run envelope"); }
    char *encoded = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    if (!encoded) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode run envelope");
    if (strlen(encoded) > RUN_MAX_FILE_BYTES) {
        cJSON_free(encoded);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Run checkpoint is too large");
    }
    *out_text = cc_copy_string(encoded);
    cJSON_free(encoded);
    return *out_text ? cc_result_ok() : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy run checkpoint");
}

/**
 * @brief 按区分大小写的字段名读取 JSON 字符串。
 * @param[in] object 借用对象，可为 NULL，查找沿用 cJSON 首个匹配字段行为。
 * @param[in] name 非空借用字段名。
 * @return 成功借出树内字符串，树销毁后失效，缺失或类型不符返回 NULL。
 */
static const char *string_field(cJSON *object, const char *name)
{
    cJSON *field = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(field) ? field->valuestring : NULL;
}

/**
 * @brief 核对版本、payload 原文 CRC 和计划约束，深拷贝可恢复状态。
 *
 * envelope 与解码后的 payload 分别拒绝会产生 NUL 的转义，再做 SDK 完整尾部和 Unicode 形式检查。
 * 参数字符串继续保留原 JSON 文本，内部合法 NUL 转义按原文保存。
 * @param[in] text 非空借用记录正文，调用期间稳定。
 * @param[out] out 必填独立空输出，入口清零，成功拥有字段及计划列表，失败清理部分副本并保持全零。
 * @return 记录、转义、CRC、类型或状态组合错误返回 CC_ERR_STORAGE，字段复制失败返回 CC_ERR_OUT_OF_MEMORY。
 * cJSON 解析分配失败沿用存储错误映射，返回码本身不能证明已确认完整性损坏。
 * 缺失 retry_after_call_id 保持旧记录兼容，字段存在时须为字符串，空字符串解码为 NULL。
 */
static cc_result_t decode_checkpoint(const char *text, cc_run_checkpoint_t *out)
{
    memset(out, 0, sizeof(*out));
    if (cc_storage_json_has_nul_escape(text))
        return cc_result_error(CC_ERR_STORAGE, "Run checkpoint envelope contains an encoded NUL");
    cJSON *envelope = parse_complete_json(text);
    cJSON *version = cJSON_GetObjectItemCaseSensitive(envelope, "version");
    const char *payload_text = string_field(envelope, "payload");
    const char *checksum = string_field(envelope, "crc32");
    char expected[9];
    if (!cJSON_IsObject(envelope) || !cJSON_IsNumber(version) || version->valuedouble != 1 ||
        !payload_text || !checksum || strlen(checksum) != 8U) {
        cJSON_Delete(envelope);
        return cc_result_error(CC_ERR_STORAGE, "Run checkpoint envelope is invalid");
    }
    snprintf(expected, sizeof(expected), "%08lx", (unsigned long)crc32_text(payload_text));
    if (strcmp(expected, checksum) != 0) {
        cJSON_Delete(envelope);
        return cc_result_error(CC_ERR_STORAGE, "Run checkpoint checksum mismatch");
    }
    if (cc_storage_json_has_nul_escape(payload_text)) {
        cJSON_Delete(envelope);
        return cc_result_error(CC_ERR_STORAGE, "Run checkpoint payload contains an encoded NUL");
    }
    cJSON *payload = parse_complete_json(payload_text);
    cJSON *step = cJSON_GetObjectItemCaseSensitive(payload, "step");
    cJSON *phase = cJSON_GetObjectItemCaseSensitive(payload, "phase");
    cJSON *next = cJSON_GetObjectItemCaseSensitive(payload, "next_call");
    cJSON *calls = cJSON_GetObjectItemCaseSensitive(payload, "calls");
    const char *run_id = string_field(payload, "run_id");
    const char *session = string_field(payload, "session_id");
    const char *user_id = string_field(payload, "user_message_id");
    const char *input = string_field(payload, "user_input");
    const char *reasoning = string_field(payload, "reasoning_content");
    cJSON *retry_field = cJSON_GetObjectItemCaseSensitive(payload, "retry_after_call_id");
    const char *retry_after = string_field(payload, "retry_after_call_id");
    if (!cJSON_IsObject(payload) || !run_id || !session || !user_id || !input || !reasoning ||
        !cJSON_IsNumber(step) || step->valuedouble < 1 || step->valuedouble > UINT_MAX ||
        step->valuedouble != (double)(unsigned)step->valuedouble ||
        !cJSON_IsNumber(phase) || (phase->valuedouble != CC_RUN_CHECKPOINT_MODEL &&
                                   phase->valuedouble != CC_RUN_CHECKPOINT_TOOLS) ||
        !cJSON_IsNumber(next) || next->valuedouble < 0 || next->valuedouble > RUN_MAX_CALLS ||
        next->valuedouble != (double)(unsigned)next->valuedouble ||
        !cJSON_IsArray(calls) || cJSON_GetArraySize(calls) > (int)RUN_MAX_CALLS ||
        (retry_field && !cJSON_IsString(retry_field))) {
        cJSON_Delete(payload); cJSON_Delete(envelope);
        return cc_result_error(CC_ERR_STORAGE, "Run checkpoint fields are invalid");
    }
    out->run_id = cc_copy_string(run_id);
    out->session_id = cc_copy_string(session);
    out->user_message_id = cc_copy_string(user_id);
    out->user_input = cc_copy_string(input);
    out->reasoning_content = cc_copy_string(reasoning);
    out->retry_after_call_id = retry_after && retry_after[0] ? cc_copy_string(retry_after) : NULL;
    out->step = (unsigned)step->valuedouble;
    out->phase = (cc_run_checkpoint_phase_t)(int)phase->valuedouble;
    out->next_call = (size_t)next->valuedouble;
    cc_result_t rc = cc_result_ok();
    if (!out->run_id || !out->session_id || !out->user_message_id ||
        !out->user_input || !out->reasoning_content ||
        (retry_after && retry_after[0] && !out->retry_after_call_id)) {
        rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy run checkpoint fields");
    }
    for (int i = 0; rc.code == CC_OK && i < cJSON_GetArraySize(calls); i++) {
        cJSON *item = cJSON_GetArrayItem(calls, i);
        const char *id = string_field(item, "id");
        const char *name = string_field(item, "name");
        const char *args = string_field(item, "arguments_json");
        if (!cJSON_IsObject(item) || !id || !name || !args) {
            rc = cc_result_error(CC_ERR_STORAGE, "Run checkpoint tool call is invalid");
        } else {
            rc = cc_tool_call_list_append_values(&out->planned_calls, id, name, args);
        }
    }
    if (rc.code == CC_OK && !valid_checkpoint(out))
        rc = cc_result_error(CC_ERR_STORAGE, "Run checkpoint invariants are invalid");
    cJSON_Delete(payload);
    cJSON_Delete(envelope);
    if (rc.code != CC_OK) cc_run_checkpoint_cleanup(out);
    return rc;
}

/**
 * @brief stat 限制文件类型与大小，读取后核对实际字节数并解码。
 * @pre 调用者持有 store->mutex 或独占实例及目录。
 * @param[in] store 非空存储，借用其文件系统。
 * @param[in] path 非空借用文件路径。
 * @param[out] out 必填独立空输出，进入解码后清零，成功取得拥有状态。
 * @return 缺失返回 CC_ERR_NOT_FOUND，文件类型、大小、截断或解码错误返回 CC_ERR_STORAGE，
 * 其他 stat、读取及字段复制错误向上传递，读取文本在返回前释放。
 */
static cc_result_t read_checkpoint(cc_run_checkpoint_store_t *store, const char *path,
                                   cc_run_checkpoint_t *out)
{
    cc_filesystem_stat_t info = {.size = sizeof(info)};
    cc_result_t rc = store->fs->vtable->stat(store->fs->self, path, &info);
    if (rc.code != CC_OK) return rc;
    if (info.type == CC_FS_ENTRY_MISSING) return cc_result_error(CC_ERR_NOT_FOUND, "Run checkpoint not found");
    if (info.type != CC_FS_ENTRY_FILE || info.byte_size > RUN_MAX_FILE_BYTES)
        return cc_result_error(CC_ERR_STORAGE, "Run checkpoint file is invalid or too large");
    char *text = NULL;
    rc = store->fs->vtable->read_text(store->fs->self, path, &text);
    if (rc.code != CC_OK) return rc;
    if (!text || strlen(text) != info.byte_size) {
        free(text);
        return cc_result_error(CC_ERR_STORAGE, "Run checkpoint file is truncated");
    }
    rc = decode_checkpoint(text, out);
    free(text);
    return rc;
}

/**
 * @brief 文件系统声明目录同步能力时同步检查点目录。
 * @param[in] store 非空存储，打开时已核对条件能力与入口一致。
 * @return 有能力时返回后端同步结果，其余返回 CC_OK。
 */
static cc_result_t sync_dir(cc_run_checkpoint_store_t *store)
{
    return (store->fs->capabilities & CC_FS_CAP_DIR_SYNC) ?
        store->fs->vtable->sync_dir(store->fs->self, store->directory) : cc_result_ok();
}

/**
 * @brief 核对目录及文件系统持久化能力，复制路径并创建存储互斥量。
 * @param[in] fs 非空文件系统借用，结构与虚表须覆盖所用字段，存活至 store 销毁。
 * @param[in] directory 非空既有目录，路径复制，目录由外层独占。
 * @param[out] out_store 必填，入口置 NULL，成功取得拥有实例。
 * @return 输出非法返回 CC_ERR_INVALID_ARGUMENT，能力或目录参数不符返回 CC_ERR_UNSUPPORTED，
 * 目录类型不符返回 CC_ERR_STORAGE，stat、路径复制和互斥量错误向上传递，成功返回 CC_OK。
 * 打开阶段只检查目录，文件内容在后续 load/list 访问。
 */
cc_result_t cc_run_checkpoint_store_open(cc_filesystem_t *fs, const char *directory,
                                         cc_run_checkpoint_store_t **out_store)
{
    if (!out_store) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null run store output");
    *out_store = NULL;
    uint64_t required = CC_FS_CAP_ATOMIC_REPLACE | CC_FS_CAP_FILE_SYNC | CC_FS_CAP_STAT;
    if (!fs || fs->size < sizeof(*fs) || !fs->vtable ||
        fs->vtable->size < offsetof(cc_filesystem_vtable_t, sync_dir) + sizeof(fs->vtable->sync_dir) ||
        (fs->capabilities & required) != required || !directory || !directory[0] ||
        !fs->vtable->stat || !fs->vtable->list_dir || !fs->vtable->read_text ||
        !fs->vtable->write_text || !fs->vtable->remove || !fs->vtable->sync_file ||
        !fs->vtable->atomic_replace || ((fs->capabilities & CC_FS_CAP_DIR_SYNC) && !fs->vtable->sync_dir))
        return cc_result_error(CC_ERR_UNSUPPORTED, "Filesystem cannot store run checkpoints");
    cc_filesystem_stat_t info = {.size = sizeof(info)};
    cc_result_t rc = fs->vtable->stat(fs->self, directory, &info);
    if (rc.code != CC_OK) return rc;
    if (info.type != CC_FS_ENTRY_DIRECTORY)
        return cc_result_error(CC_ERR_STORAGE, "Run checkpoint directory is unavailable");
    cc_run_checkpoint_store_t *store = calloc(1, sizeof(*store));
    if (!store) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate run checkpoint store");
    store->fs = fs;
    store->directory = cc_copy_string(directory);
    if (!store->directory) { free(store); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy run directory"); }
    rc = cc_mutex_create(&store->mutex);
    if (rc.code != CC_OK) { free(store->directory); free(store); return rc; }
    *out_store = store;
    return cc_result_ok();
}

/**
 * @brief 清理拥有的声明链表、互斥量和目录路径，释放存储容器。
 * @pre 外层关闭入口并排空调用及回调，全部运行声明已释放，借用 fs 继续有效。
 * @param[in,out] store 待销毁对象，可为 NULL。
 * @post 实例及内部借用失效，磁盘检查点与文件系统句柄继续由原 owner 管理。
 */
void cc_run_checkpoint_store_destroy(cc_run_checkpoint_store_t *store)
{
    if (!store) return;
    while (store->claims) {
        active_run_claim_t *next = store->claims->next;
        free(store->claims->run_id);
        free(store->claims->session_id);
        free(store->claims);
        store->claims = next;
    }
    cc_mutex_destroy(store->mutex);
    free(store->directory);
    free(store);
}

/**
 * @brief 先准备拥有的声明副本，再持锁核对运行 ID 和会话的排他性。
 * @param[in,out] store 非空声明所属存储，成功时链表接管新声明。
 * @param[in] run_id 合法运行 ID，调用期间借用并复制。
 * @param[in] session_id 非空且最多 512 字节的会话 ID，调用期间借用并复制。
 * @return 参数错误返回 CC_ERR_INVALID_ARGUMENT，副本分配失败返回 CC_ERR_OUT_OF_MEMORY，
 * 任一运行 ID 或会话冲突返回 CC_ERR_INVALID_STATE，成功返回 CC_OK。
 * 声明只存在于本进程，磁盘检查点可以稍后创建，停止运行后由调用者 release。
 */
cc_result_t cc_run_checkpoint_claim(cc_run_checkpoint_store_t *store,
                                     const char *run_id, const char *session_id)
{
    if (!store || !valid_run_id(run_id) || !session_id || !session_id[0] || strlen(session_id) > 512U)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run claim");
    active_run_claim_t *claim = calloc(1, sizeof(*claim));
    if (claim) {
        claim->run_id = cc_copy_string(run_id);
        claim->session_id = cc_copy_string(session_id);
    }
    if (!claim || !claim->run_id || !claim->session_id) {
        if (claim) { free(claim->run_id); free(claim->session_id); free(claim); }
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate run claim");
    }
    cc_mutex_lock(store->mutex);
    for (active_run_claim_t *p = store->claims; p; p = p->next) {
        if (strcmp(p->run_id, run_id) == 0 || strcmp(p->session_id, session_id) == 0) {
            cc_mutex_unlock(store->mutex);
            free(claim->run_id); free(claim->session_id); free(claim);
            return cc_result_error(CC_ERR_INVALID_STATE, "A run is already active in this session");
        }
    }
    claim->next = store->claims;
    store->claims = claim;
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

/**
 * @brief 锁内摘除匹配的进程内声明，锁外释放其字符串和节点。
 * @param[in,out] store 非空声明所属存储。
 * @param[in] run_id 合法借用运行 ID，外层保证相关运行已停止受保护访问。
 * @return 参数非法返回 CC_ERR_INVALID_ARGUMENT，声明缺失返回 CC_ERR_NOT_FOUND，释放成功返回 CC_OK。
 */
cc_result_t cc_run_checkpoint_release(cc_run_checkpoint_store_t *store, const char *run_id)
{
    if (!store || !valid_run_id(run_id))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run claim release");
    cc_mutex_lock(store->mutex);
    active_run_claim_t **link = &store->claims;
    while (*link && strcmp((*link)->run_id, run_id) != 0) link = &(*link)->next;
    active_run_claim_t *claim = *link;
    if (claim) *link = claim->next;
    cc_mutex_unlock(store->mutex);
    if (!claim) return cc_result_error(CC_ERR_NOT_FOUND, "Run claim not found");
    free(claim->run_id); free(claim->session_id); free(claim);
    return cc_result_ok();
}

/**
 * @brief 锁外编码借用输入，锁内完成临时写入、文件同步、原文回读、替换和目录同步。
 * @pre 调用者通过运行声明或独占初始化协调目标，输入字段在编码和路径构造期间稳定。
 * @param[in,out] store 非空存储，同一实例的文件事务由 mutex 串行。
 * @param[in] checkpoint 合法借用状态，存储复制编码文本，输入所有权保持原样。
 * @return 返回参数、字段验证、编码预算、分配或文件系统结果，成功返回 CC_OK。
 * 错误后调用者停止推进，替换可能已经完成，失败清理仅尝试删除本次已尝试写入的临时路径，忽略其清理错误。
 * 编码文本与路径始终在本函数释放，活动声明由调用者单独管理。
 */
cc_result_t cc_run_checkpoint_save(cc_run_checkpoint_store_t *store,
                                    const cc_run_checkpoint_t *checkpoint)
{
    if (!store || !valid_checkpoint(checkpoint))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run checkpoint save");
    char *encoded = NULL;
    cc_result_t rc = encode_checkpoint(checkpoint, &encoded);
    if (rc.code != CC_OK) return rc;
    char *temporary = checkpoint_path(store, checkpoint->run_id, ".tmp");
    char *path = checkpoint_path(store, checkpoint->run_id, ".json");
    if (!temporary || !path) {
        free(encoded); free(temporary); free(path);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate run checkpoint path");
    }
    cc_mutex_lock(store->mutex);
    rc = store->fs->vtable->write_text(store->fs->self, temporary, encoded);
    if (rc.code == CC_OK) rc = store->fs->vtable->sync_file(store->fs->self, temporary);
    if (rc.code == CC_OK) {
        char *check = NULL;
        rc = store->fs->vtable->read_text(store->fs->self, temporary, &check);
        if (rc.code == CC_OK && (!check || strcmp(check, encoded) != 0)) {
            cc_result_free(&rc);
            rc = cc_result_error(CC_ERR_STORAGE, "Run checkpoint verification failed");
        }
        free(check);
    }
    if (rc.code == CC_OK) rc = store->fs->vtable->atomic_replace(store->fs->self, temporary, path);
    if (rc.code == CC_OK) rc = sync_dir(store);
    if (rc.code != CC_OK) {
        cc_result_t cleanup = store->fs->vtable->remove(store->fs->self, temporary);
        cc_result_free(&cleanup);
    }
    cc_mutex_unlock(store->mutex);
    free(encoded); free(temporary); free(path);
    return rc;
}

/**
 * @brief 依次选择正式、备份和临时来源，仅前一候选缺失时继续尝试。
 * @pre 调用者持有 store->mutex，输出已清零且没有待释放字段。
 * @param[in] store 非空存储，目录与文件系统在调用期间有效。
 * @param[in] run_id 合法借用 ID，用于构造路径和核对内容身份。
 * @param[out] out_checkpoint 必填独立空输出，成功拥有记录并设置实际来源标志。
 * @return 所有候选缺失返回 CC_ERR_NOT_FOUND，内容 ID 与文件名不符返回 CC_ERR_STORAGE，其他读取错误原样返回。
 * 已存在候选损坏或读取失败时保留错误，来源选择过程保持磁盘原样。
 */
static cc_result_t load_unlocked(cc_run_checkpoint_store_t *store, const char *run_id,
                                 cc_run_checkpoint_t *out_checkpoint)
{
    char *path = checkpoint_path(store, run_id, ".json");
    char *backup = checkpoint_path(store, run_id, ".json.bak");
    if (!path || !backup) { free(path); free(backup); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate run path"); }
    cc_result_t rc = read_checkpoint(store, path, out_checkpoint);
    int used_backup = 0;
    int used_temporary = 0;
    if (rc.code == CC_ERR_NOT_FOUND) {
        cc_result_free(&rc);
        rc = read_checkpoint(store, backup, out_checkpoint);
        used_backup = rc.code == CC_OK;
    }
    if (rc.code == CC_ERR_NOT_FOUND) {
        char *temporary = checkpoint_path(store, run_id, ".tmp");
        cc_result_free(&rc);
        rc = temporary ? read_checkpoint(store, temporary, out_checkpoint) :
            cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate temporary run path");
        used_temporary = rc.code == CC_OK;
        free(temporary);
    }
    free(path); free(backup);
    if (rc.code == CC_OK && strcmp(out_checkpoint->run_id, run_id) != 0) {
        cc_run_checkpoint_cleanup(out_checkpoint);
        return cc_result_error(CC_ERR_STORAGE, "Run checkpoint id does not match its filename");
    }
    if (rc.code == CC_OK) out_checkpoint->recovered_from_backup = used_backup;
    if (rc.code == CC_OK) out_checkpoint->recovered_from_temporary = used_temporary;
    return rc;
}

/**
 * @brief 清零输出后持锁载入指定运行及其实际恢复来源。
 * @param[in] store 非空借用存储。
 * @param[in] run_id 合法借用运行 ID。
 * @param[out] out_checkpoint 必填独立空输出，参数有效后清零，成功拥有所有字段，由调用者 cleanup。
 * @return 参数错误返回 CC_ERR_INVALID_ARGUMENT 并保持输出原值，其他返回候选读取及解码结果。
 * 读取与解码失败保持空输出，已存在但无效的主文件会阻止回退。
 */
cc_result_t cc_run_checkpoint_load(cc_run_checkpoint_store_t *store, const char *run_id,
                                    cc_run_checkpoint_t *out_checkpoint)
{
    if (!store || !valid_run_id(run_id) || !out_checkpoint)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run checkpoint lookup");
    memset(out_checkpoint, 0, sizeof(*out_checkpoint));
    cc_mutex_lock(store->mutex);
    cc_result_t rc = load_unlocked(store, run_id, out_checkpoint);
    cc_mutex_unlock(store->mutex);
    return rc;
}

/**
 * @brief 在用户已确认的前提下核对孤立临时记录，拒绝活动会话后同步安装。
 * @pre 用户已核对临时输入，外层保证目录及文件系统在本次调用期间有效。
 * @param[in,out] store 非空存储，全过程持 mutex，临时解码副本由本函数清理。
 * @param[in] run_id 合法借用运行 ID。
 * @return 非临时来源或相同运行/会话已有声明返回 CC_ERR_INVALID_STATE，
 * 其他返回读取、路径分配或持久化结果，成功返回 CC_OK。
 * 替换已发生但目录同步失败时也会报错，重试前应重新读取实际来源。
 */
cc_result_t cc_run_checkpoint_confirm_temporary(cc_run_checkpoint_store_t *store,
                                                const char *run_id)
{
    if (!store || !valid_run_id(run_id))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid temporary run confirmation");
    cc_mutex_lock(store->mutex);
    cc_run_checkpoint_t checkpoint = {0};
    cc_result_t rc = load_unlocked(store, run_id, &checkpoint);
    if (rc.code == CC_OK && !checkpoint.recovered_from_temporary)
        rc = cc_result_error(CC_ERR_INVALID_STATE, "Run does not have an orphan temporary checkpoint");
    for (active_run_claim_t *claim = store->claims; rc.code == CC_OK && claim; claim = claim->next) {
        if (strcmp(claim->run_id, run_id) == 0 || strcmp(claim->session_id, checkpoint.session_id) == 0)
            rc = cc_result_error(CC_ERR_INVALID_STATE, "Run session is active");
    }
    char *temporary = NULL, *path = NULL;
    if (rc.code == CC_OK) {
        temporary = checkpoint_path(store, run_id, ".tmp");
        path = checkpoint_path(store, run_id, ".json");
        if (!temporary || !path)
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate temporary run paths");
    }
    if (rc.code == CC_OK) rc = store->fs->vtable->sync_file(store->fs->self, temporary);
    if (rc.code == CC_OK) rc = store->fs->vtable->atomic_replace(store->fs->self, temporary, path);
    if (rc.code == CC_OK) rc = sync_dir(store);
    free(temporary);
    free(path);
    cc_run_checkpoint_cleanup(&checkpoint);
    cc_mutex_unlock(store->mutex);
    return rc;
}

/**
 * @brief 为 qsort 按运行 ID 的原文字节顺序排序。
 * @param[in] a 非空字符串指针元素的借用地址。
 * @param[in] b 非空字符串指针元素的借用地址。
 * @return 原样返回 strcmp 的负、零或正比较值。
 */
static int compare_ids(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/**
 * @brief 持锁枚举三类文件后缀，对运行 ID 去重并完整读取核对，最后排序交付副本。
 * @param[in] store 非空借用存储，目录与文件系统由外层独占。
 * @param[out] out_run_ids 必填独立输出，参数有效后置 NULL，成功拥有逐项 ID 副本和数组，调用者逐项 free 后 free。
 * @param[out] out_count 必填输出，参数有效后置零，成功填写 ID 数量，零条时数组也可能非空。
 * @return 成功返回 CC_OK，候选名称或内容无效返回 CC_ERR_STORAGE，其他目录、读取或分配错误向上传递。
 * 失败清理本次已复制结果，文件本体保留，最终答案与未完成语义由上层继续核对。
 */
cc_result_t cc_run_checkpoint_list(cc_run_checkpoint_store_t *store,
                                   char ***out_run_ids, size_t *out_count)
{
    if (!store || !out_run_ids || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run checkpoint list");
    *out_run_ids = NULL; *out_count = 0;
    cc_mutex_lock(store->mutex);
    char **names = NULL, **ids = NULL;
    size_t names_count = 0, count = 0;
    cc_result_t rc = store->fs->vtable->list_dir(store->fs->self, store->directory, &names, &names_count);
    if (rc.code == CC_OK && names_count) {
        ids = calloc(names_count, sizeof(*ids));
        if (!ids) rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate run list");
    }
    for (size_t i = 0; rc.code == CC_OK && i < names_count; i++) {
        size_t length = strlen(names[i]);
        if (strncmp(names[i], "run-", 4U) != 0) continue;
        int candidate = (length >= 5U && strcmp(names[i] + length - 5U, ".json") == 0) ||
                        (length >= 9U && strcmp(names[i] + length - 9U, ".json.bak") == 0) ||
                        (length >= 4U && strcmp(names[i] + length - 4U, ".tmp") == 0);
        if (!candidate) continue;
        if (length != 44U && length != 45U && length != 49U) { rc = cc_result_error(CC_ERR_STORAGE, "Invalid run checkpoint filename"); break; }
        char id[41];
        memcpy(id, names[i], 40U); id[40] = '\0';
        if (!valid_run_id(id)) { rc = cc_result_error(CC_ERR_STORAGE, "Invalid run checkpoint filename"); break; }
        int found = 0;
        for (size_t j = 0; j < count; j++) if (strcmp(ids[j], id) == 0) { found = 1; break; }
        if (found) continue;
        cc_run_checkpoint_t check = {0};
        rc = load_unlocked(store, id, &check);
        cc_run_checkpoint_cleanup(&check);
        if (rc.code != CC_OK) break;
        ids[count] = cc_copy_string(id);
        if (!ids[count]) { rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy run id"); break; }
        count++;
    }
    for (size_t i = 0; i < names_count; i++) free(names[i]);
    free(names);
    cc_mutex_unlock(store->mutex);
    if (rc.code != CC_OK) {
        for (size_t i = 0; i < count; i++) free(ids[i]);
        free(ids);
        return rc;
    }
    if (count > 1) qsort(ids, count, sizeof(*ids), compare_ids);
    *out_run_ids = ids;
    *out_count = count;
    return cc_result_ok();
}

/**
 * @brief 核对路径类型后删除存在的普通文件，缺失路径按成功处理。
 * @pre 调用者持有 store->mutex。
 * @param[in] store 非空存储，借用其文件系统。
 * @param[in] path 非空借用路径。
 * @return 缺失返回 stat 的成功结果，类型不符返回 CC_ERR_STORAGE，其他返回 stat 或删除结果。
 */
static cc_result_t remove_if_present(cc_run_checkpoint_store_t *store, const char *path)
{
    cc_filesystem_stat_t info = {.size = sizeof(info)};
    cc_result_t rc = store->fs->vtable->stat(store->fs->self, path, &info);
    if (rc.code != CC_OK || info.type == CC_FS_ENTRY_MISSING) return rc;
    if (info.type != CC_FS_ENTRY_FILE) return cc_result_error(CC_ERR_STORAGE, "Run checkpoint path is not a file");
    return store->fs->vtable->remove(store->fs->self, path);
}

/**
 * @brief 先删除临时和备份并同步目录，再删除正式记录并再次同步。
 * @pre 最终答案已可靠保存或用户决定放弃，调用者通过声明或独占初始化协调目标运行。
 * @param[in,out] store 非空存储，同一实例删除过程持 mutex。
 * @param[in] run_id 合法借用运行 ID。
 * @return 成功返回 CC_OK，参数或路径分配错误返回对应错误，文件系统失败立即停止并返回。
 * 失败可能已删除部分文件，路径副本始终释放，进程内声明继续由调用者单独 release。
 */
cc_result_t cc_run_checkpoint_remove(cc_run_checkpoint_store_t *store, const char *run_id)
{
    if (!store || !valid_run_id(run_id)) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run checkpoint removal");
    char *path = checkpoint_path(store, run_id, ".json");
    char *backup = checkpoint_path(store, run_id, ".json.bak");
    char *temporary = checkpoint_path(store, run_id, ".tmp");
    if (!path || !backup || !temporary) { free(path); free(backup); free(temporary); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate run path"); }
    cc_mutex_lock(store->mutex);
    cc_result_t rc = remove_if_present(store, temporary);
    if (rc.code == CC_OK) rc = remove_if_present(store, backup);
    if (rc.code == CC_OK) rc = sync_dir(store);
    if (rc.code == CC_OK) rc = remove_if_present(store, path);
    if (rc.code == CC_OK) rc = sync_dir(store);
    cc_mutex_unlock(store->mutex);
    free(path); free(backup); free(temporary);
    return rc;
}
