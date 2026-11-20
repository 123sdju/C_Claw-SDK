#include "cc/ports/cc_session_store.h"
#include "cc/adapters/cc_json_segmented_history.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_thread.h"
#include "cc/ports/cc_platform.h"
#include "cc/core/cc_id.h"
#include "cc/util/cc_base64.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"
#include "cc_segmented_cursor.h"
#include "cc_segmented_integrity.h"
#include "cc_json_record_validation.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CC_SEGMENTED_DEFAULT_BYTES (256u * 1024u)
#define CC_SEGMENTED_DEFAULT_MEDIA_DIR "media"
#define CC_SEGMENTED_MAX_LINE_READ (8u * 1024u * 1024u)

typedef struct cc_json_segmented_store {
    char *root_path;                 /**< 后端拥有的根路径副本。 */
    char *media_dir;                 /**< 后端拥有的安全相对媒体目录。 */
    size_t segment_bytes;            /**< 软分段阈值，单批次可超过该值。 */
    size_t max_base64_bytes;         /**< 普通回填检查原始字节，上下文另检查编码字节，零无限制。 */
    cc_filesystem_t fs;              /**< 独占默认文件系统端口。 */
    cc_mutex_t mutex;                /**< 串行化实例读写及游标注册。 */
    cc_segmented_cursor_t *cursors;  /**< 注册游标链借用，游标由调用者销毁。 */
} cc_json_segmented_store_t;

typedef struct string_vec {
    char **items;       /**< 拥有数组及 count 个独占字符串。 */
    size_t count;       /**< 已初始化条数。 */
    size_t capacity;    /**< 已分配指针槽数。 */
} string_vec_t;

typedef struct message_vec {
    cc_message_t *items; /**< 拥有数组及 count 个消息的全部字段。 */
    size_t count;       /**< 已完成消息数。 */
    size_t capacity;    /**< 已分配消息槽数。 */
    int metadata_only;  /**< 非零时解码不回填媒体字节。 */
} message_vec_t;

struct cc_segmented_cursor {
    cc_json_segmented_store_t *store; /* 借用后端，状态受其互斥量保护。 */
    char *session_id;                 /**< 游标拥有的会话名副本。 */
    char *session_dir;                /**< 游标拥有的目录路径。 */
    int segment;                     /**< 当前分段号，零表示遍历完成。 */
    string_vec_t lines;               /**< 拥有当前分段全部行副本。 */
    size_t next_line;                 /**< 下一待解析行的反向位置。 */
    message_vec_t batch;              /**< 拥有当前行解码出的消息。 */
    size_t next_message;              /**< batch 下一待移动消息位置。 */
    int invalid;                     /**< 同会话写入或读取错误后的失效锁存。 */
    struct cc_segmented_cursor *next; /**< 后端注册链上的下一游标借用。 */
};

/** @brief 标记同会话的所有游标失效。
 * @param[in,out] store 必填实例借用，调用者已持 mutex。
 * @param[in] session_id 必填稳定会话名借用，不转移资源。
 * @post 后续 next 返回错误，其他会话游标保持有效。
 */
static void invalidate_cursors_locked(cc_json_segmented_store_t *store, const char *session_id)
{
    for (cc_segmented_cursor_t *cursor = store->cursors; cursor; cursor = cursor->next)
        if (strcmp(cursor->session_id, session_id) == 0) cursor->invalid = 1;
}

/** @brief 消费字符串向量并归零。
 * @param[in,out] vec 可为 NULL 的独占向量，结构仍归调用者。
 * @post 已清空向量可重复清理，调用前读者均退出。
 */
static void string_vec_cleanup(string_vec_t *vec)
{
    if (!vec) return;
    for (size_t i = 0; i < vec->count; i++) free(vec->items[i]);
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

/** @brief 成功时接管独占字符串并追加到向量。
 * @param[in,out] vec 必填独占向量，失败保留原数组。
 * @param[in,out] text 必填拥有字符串，成功转移，失败仍归调用者 free。
 * @return CC_OK 或参数、容量、分配错误，结果诊断归调用者。
 */
static cc_result_t string_vec_append_take(string_vec_t *vec, char *text)
{
    if (!vec || !text) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid string vec append");
    if (vec->count == vec->capacity) {
        if (vec->capacity > SIZE_MAX / sizeof(*vec->items) / 2U)
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "String vector capacity overflow");
        size_t next_cap = vec->capacity ? vec->capacity * 2 : 8;
        char **next = realloc(vec->items, next_cap * sizeof(char *));
        if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow string vector");
        vec->items = next;
        vec->capacity = next_cap;
    }
    vec->items[vec->count++] = text;
    return cc_result_ok();
}

/** @brief 消费消息向量的全部拥有字段并归零。
 * @param[in,out] vec 可为 NULL 的独占向量，结构本身继续归调用者。
 * @pre 所有读者已退出，空向量允许重复清理。
 */
static void message_vec_cleanup(message_vec_t *vec)
{
    if (!vec) return;
    for (size_t i = 0; i < vec->count; i++) cc_message_cleanup(&vec->items[i]);
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

/** @brief 成功时移动消息进向量并清空原消息。
 * @param[in,out] vec 必填独占向量，失败保留原数组。
 * @param[in,out] msg 必填拥有消息，成功清零，失败所有权保留。
 * @return CC_OK 或参数、容量、分配错误，诊断由调用者释放。
 */
static cc_result_t message_vec_append_take(message_vec_t *vec, cc_message_t *msg)
{
    if (!vec || !msg) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid message vec append");
    if (vec->count == vec->capacity) {
        if (vec->capacity > SIZE_MAX / sizeof(*vec->items) / 2U)
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Message vector capacity overflow");
        size_t next_cap = vec->capacity ? vec->capacity * 2 : 8;
        cc_message_t *next = realloc(vec->items, next_cap * sizeof(cc_message_t));
        if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow message vector");
        memset(next + vec->capacity, 0, (next_cap - vec->capacity) * sizeof(cc_message_t));
        vec->items = next;
        vec->capacity = next_cap;
    }
    vec->items[vec->count++] = *msg;
    memset(msg, 0, sizeof(*msg));
    return cc_result_ok();
}

/** @brief 用当前历史格式的 FNV 类常量计算确定性校验值。
 * @param[in] data 借用至少 len 字节，len 为零可为 NULL。
 * @param[in] len 字节数。
 * @return 64 位无符号校验值，无分配及共享状态。
 */
static unsigned long long fnv1a_bytes(const unsigned char *data, size_t len)
{
    unsigned long long hash = 1469598103934665603ull;
    for (size_t i = 0; i < len; i++) {
        hash ^= (unsigned long long)data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

/** @brief 计算 NUL 文本的格式兼容校验值。
 * @param[in] text 可为 NULL 的稳定文本借用，NULL 按空串。
 * @return 确定性校验值，无资源交接。
 */
static unsigned long long fnv1a_text(const char *text)
{
    return fnv1a_bytes((const unsigned char *)(text ? text : ""), text ? strlen(text) : 0);
}

/** @brief 分配完整格式化字符串。
 * @param[in] fmt 必填 printf 格式借用。
 * @param[in] ... 与格式匹配的值及调用期借用参数。
 * @return 调用者 free 的字符串，格式化或分配失败返回 NULL，无共享状态。
 */
static char *dup_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return NULL;
    }
    char *buf = malloc((size_t)needed + 1);
    if (!buf) {
        va_end(args);
        return NULL;
    }
    vsnprintf(buf, (size_t)needed + 1, fmt, args);
    va_end(args);
    return buf;
}

/** @brief 查询路径是否为目录并消费查询诊断。
 * @param[in] store 可为 NULL 的调用期后端借用。
 * @param[in] path 可为 NULL 的同步路径借用。
 * @return 确认目录为一，参数缺失或任意查询失败为零，访问由外层协调。
 */
static int path_is_dir(cc_json_segmented_store_t *store, const char *path)
{
    if (!store || !path || !store->fs.vtable || !store->fs.vtable->stat) return 0;
    cc_filesystem_stat_t st = {.size = sizeof(st)};
    cc_result_t rc = store->fs.vtable->stat(store->fs.self, path, &st);
    int matched = rc.code == CC_OK && st.type == CC_FS_ENTRY_DIRECTORY;
    cc_result_free(&rc);
    return matched;
}

/** @brief 确证媒体为文件并查询可表示大小。
 * @param[in] store 必填提供 stat 的后端借用，调用者已持实例锁。
 * @param[in] path 必填同步路径借用。
 * @param[out] out_size 必填槽，先置零，仅成功值可用于读取预算。
 * @return 成功或查询、缺失、类型、容量错误，调用者消费诊断并选择可选媒体兼容路径。
 */
static cc_result_t media_file_size(cc_json_segmented_store_t *store, const char *path, size_t *out_size)
{
    *out_size = 0;
    cc_filesystem_stat_t st = {.size = sizeof(st)};
    cc_result_t rc = store->fs.vtable->stat(store->fs.self, path, &st);
    if (rc.code != CC_OK) return rc;
    if (st.type != CC_FS_ENTRY_FILE)
        return cc_result_error(CC_ERR_NOT_FOUND, "Stored media is unavailable");
    if (st.byte_size > SIZE_MAX)
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Stored media size exceeds addressable memory");
    *out_size = (size_t)st.byte_size;
    return rc;
}

/** @brief 用斜杠拼接路径，保留已有斜杠或反斜杠。
 * @param[in] a 可为 NULL 或空的父路径借用。
 * @param[in] b 可为 NULL 或空的后缀借用。
 * @return 调用者 free 的路径副本，分配/格式化失败返回 NULL，不规范化路径。
 */
static char *join_path2(const char *a, const char *b)
{
    if (!a || !*a) return b ? cc_copy_string(b) : cc_copy_string("");
    if (!b || !*b) return cc_copy_string(a);
    size_t alen = strlen(a);
    int has_sep = a[alen - 1] == '/' || a[alen - 1] == '\\';
    return dup_printf("%s%s%s", a, has_sep ? "" : "/", b);
}

/** @brief 顺序拼接三个路径组件。
 * @param[in] a 可为 NULL 的首段借用。
 * @param[in] b 可为 NULL 的中段借用。
 * @param[in] c 可为 NULL 的末段借用。
 * @return 调用者 free 的完整路径，失败 NULL，临时路径自行回收。
 */
static char *join_path3(const char *a, const char *b, const char *c)
{
    char *ab = join_path2(a, b);
    if (!ab) return NULL;
    char *abc = join_path2(ab, c);
    free(ab);
    return abc;
}

/** @brief 已有目录直接成功，否则调用后端 make_dir。
 * @param[in] store 必填调用期后端借用。
 * @param[in] path 必填非空路径借用。
 * @return 参数错误或建目录结果，失败可能已创建前缀，递归能力依后端。
 */
static cc_result_t ensure_dir(cc_json_segmented_store_t *store, const char *path)
{
    if (!store || !path || !*path || !store->fs.vtable || !store->fs.vtable->make_dir) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid directory request");
    }
    if (path_is_dir(store, path)) return cc_result_ok();
    return store->fs.vtable->make_dir(store->fs.self, path);
}

/** @brief 委托后端目录准备，不在本层自行递归。
 * @param[in] store 必填后端借用。
 * @param[in] path 必填非空路径借用。
 * @return ensure_dir 原结果，目录创建深度沿 make_dir 契约，外层协调并发。
 */
static cc_result_t ensure_dir_recursive(cc_json_segmented_store_t *store, const char *path)
{
    return ensure_dir(store, path);
}

/** @brief 替换文件名特殊字节并附加当前格式的低 32 位哈希。
 * @param[in] text 可为 NULL 或空，按 empty 处理的稳定文本借用。
 * @return 调用者 free 的组件名，容量或分配失败 NULL，isalnum 沿进程 locale。
 */
static char *safe_component(const char *text)
{
    const char *safe = (text && *text) ? text : "empty";
    size_t len = strlen(safe);
    if (len > SIZE_MAX - 18U) return NULL;
    unsigned long long hash = fnv1a_text(safe);
    char *out = malloc(len + 18);
    if (!out) return NULL;
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)safe[i];
        out[pos++] = (isalnum(ch) || ch == '-' || ch == '_' || ch == '.') ? (char)ch : '_';
    }
    if (pos == 0) out[pos++] = 'x';
    snprintf(out + pos, 18, "-%08llx", hash & 0xffffffffull);
    return out;
}

/** @brief 按会话名确定根下的安全目录路径。
 * @param[in] store 必填稳定后端借用。
 * @param[in] session_id 调用期会话名借用，空值沿 safe_component 规则。
 * @return 调用者 free 的路径，失败 NULL，临时组件自行回收。
 */
static char *session_dir_path(cc_json_segmented_store_t *store, const char *session_id)
{
    char *safe = safe_component(session_id);
    if (!safe) return NULL;
    char *path = join_path2(store->root_path, safe);
    free(safe);
    return path;
}

/** @brief 拼接会话清单路径。
 * @param[in] session_dir 会话目录借用。
 * @return 调用者 free 的路径，失败 NULL。
 */
static char *manifest_path_for_session_dir(const char *session_dir)
{
    return join_path2(session_dir, "manifest.json");
}

/** @brief 分配 events 编号文件名。
 * @param[in] index 正数分段号，非正数按一处理。
 * @return 调用者 free 的文件名，失败 NULL，六位为最小宽度。
 */
static char *segment_name(int index)
{
    return dup_printf("events-%06d.jsonl", index > 0 ? index : 1);
}

/** @brief 分配指定分段的完整路径。
 * @param[in] session_dir 调用期目录借用。
 * @param[in] index 分段编号，沿 segment_name 规则。
 * @return 调用者 free 的路径，失败 NULL，临时文件名自行回收。
 */
static char *segment_path_for_session_dir(const char *session_dir, int index)
{
    char *name = segment_name(index);
    if (!name) return NULL;
    char *path = join_path2(session_dir, name);
    free(name);
    return path;
}

/** @brief 准备四种媒体各三种来源及 misc 目录。
 * @param[in] store 必填后端借用，调用者独占目录修改或持实例锁。
 * @param[in] session_dir 必填会话目录借用。
 * @return 首个路径分配或创建错误，失败保留已建目录，临时路径全部释放。
 */
static cc_result_t ensure_media_dirs(cc_json_segmented_store_t *store, const char *session_dir)
{
    static const char *kinds[] = {"img", "audio", "video", "file"};
    static const char *sources[] = {"user", "assistant", "tool"};
    char *media = join_path2(session_dir, store->media_dir);
    if (!media) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build media path");
    cc_result_t rc = ensure_dir_recursive(store, media);
    for (size_t i = 0; rc.code == CC_OK && i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        char *kind_dir = join_path2(media, kinds[i]);
        if (!kind_dir) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build media kind path");
            break;
        }
        rc = ensure_dir_recursive(store, kind_dir);
        for (size_t j = 0; rc.code == CC_OK && j < sizeof(sources) / sizeof(sources[0]); j++) {
            char *source_dir = join_path2(kind_dir, sources[j]);
            if (!source_dir) {
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build media source path");
                break;
            }
            rc = ensure_dir_recursive(store, source_dir);
            free(source_dir);
        }
        free(kind_dir);
    }
    if (rc.code == CC_OK) {
        char *misc = join_path2(media, "misc");
        rc = misc ? ensure_dir_recursive(store, misc)
                  : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build media misc path");
        free(misc);
    }
    free(media);
    return rc;
}

/** @brief 拒绝清单旁已有 tmp/bak 副本，保留人工核对证据。
 * @param[in] store 提供 stat 的稳定后端借用，调用者持实例锁或尚未发布实例。
 * @param[in] path 必填清单路径借用。
 * @return 无旁路文件为成功，其他为完整性、查询或分配错误，临时路径自行释放。
 */
static cc_result_t check_manifest_sidecars(cc_json_segmented_store_t *store, const char *path)
{
    const char *const suffixes[] = {".tmp", ".bak"};
    for (size_t i = 0; i < 2; i++) {
        char *candidate = dup_printf("%s%s", path, suffixes[i]);
        if (!candidate) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot inspect manifest recovery file");
        cc_filesystem_stat_t info = {.size = sizeof(info)};
        cc_result_t rc = store->fs.vtable->stat(store->fs.self, candidate, &info);
        free(candidate);
        if (rc.code != CC_OK) return rc;
        if (info.type != CC_FS_ENTRY_MISSING)
            return cc_segmented_integrity_error("Manifest recovery file requires inspection");
    }
    return cc_result_ok();
}

/** @brief 临时写入并同步后原子安装文本，保留失败副本。
 * @param[in] store 调用者已持锁的后端借用，同路径外部写者须另行排除。
 * @param[in] path 必填目标路径借用。
 * @param[in] text 可为 NULL 的正文借用，NULL 写空串。
 * @return 参数/端口、旁路文件、分配、写入、同步或替换结果，错误可能已安装目标。
 * @note 临时路径自行释放，目录同步 UNSUPPORTED 可继续，掉电保证沿后端与设备。
 */
static cc_result_t write_text_file_atomic(
    cc_json_segmented_store_t *store,
    const char *path,
    const char *text
)
{
    if (!store || !path || !store->fs.vtable || !store->fs.vtable->write_text ||
        !store->fs.vtable->atomic_replace || !store->fs.vtable->stat) {
        return cc_result_error(CC_ERR_UNSUPPORTED, "Filesystem cannot durably replace JSON files");
    }
    cc_result_t rc = check_manifest_sidecars(store, path);
    if (rc.code != CC_OK) return rc;
    char *tmp = dup_printf("%s.tmp", path);
    if (!tmp) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build temporary path");
    rc = store->fs.vtable->write_text(store->fs.self, tmp, text ? text : "");
    if (rc.code == CC_OK && store->fs.vtable->sync_file) {
        rc = store->fs.vtable->sync_file(store->fs.self, tmp);
    }
    if (rc.code == CC_OK) {
        rc = store->fs.vtable->atomic_replace(store->fs.self, tmp, path);
    }
    if (rc.code == CC_OK && store->fs.vtable->sync_dir) {
            char *parent = cc_copy_string(path);
        if (!parent) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy manifest parent path");
        } else {
            char *slash = strrchr(parent, '/');
            char *backslash = strrchr(parent, '\\');
            char *cut = slash;
            if (!cut || (backslash && backslash > cut)) cut = backslash;
            if (cut) *cut = '\0';
            cc_result_t sync_rc = store->fs.vtable->sync_dir(store->fs.self,
                cut && parent[0] ? parent : ".");
            if (sync_rc.code != CC_OK && sync_rc.code != CC_ERR_UNSUPPORTED) rc = sync_rc;
            else cc_result_free(&sync_rc);
            free(parent);
        }
    }
    free(tmp);
    return rc;
}

/** @brief 严格读取并验证清单身份、分段数及元数据类型。
 * @param[in] store 已持锁或独占的后端借用。
 * @param[in] path 必填清单路径借用，读取前后检查旁路文件和元数据。
 * @param[out] out_value 必填空槽，合法入口置 NULL，成功树由调用者 destroy。
 * @return 查询/读取、文件变化、结构完整性或解析分配错误，失败清理候选树和文本。
 */
static cc_result_t parse_json_file_strict(
    cc_json_segmented_store_t *store, const char *path, cc_json_value_t **out_value)
{
    if (!store || !path || !out_value || !store->fs.vtable || !store->fs.vtable->read_text ||
        !store->fs.vtable->stat)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid JSON file read");
    *out_value = NULL;
    cc_result_t rc = check_manifest_sidecars(store, path);
    if (rc.code != CC_OK) return rc;
    char *text = NULL;
    cc_filesystem_stat_t before = {.size = sizeof(before)}, after = {.size = sizeof(after)};
    rc = store->fs.vtable->stat(store->fs.self, path, &before);
    if (rc.code == CC_OK && before.type != CC_FS_ENTRY_FILE)
        rc = cc_result_error(CC_ERR_STORAGE, "Session manifest is missing or invalid");
    if (rc.code == CC_OK) rc = store->fs.vtable->read_text(store->fs.self, path, &text);
    if (rc.code == CC_OK) rc = store->fs.vtable->stat(store->fs.self, path, &after);
    if (rc.code == CC_OK && (!text || after.type != CC_FS_ENTRY_FILE ||
        before.byte_size != after.byte_size || before.modified_ms != after.modified_ms ||
        (uint64_t)strlen(text) != after.byte_size))
        rc = cc_result_error(CC_ERR_STORAGE, "Session manifest changed or contains NUL");
    if (rc.code == CC_OK && cc_storage_json_has_nul_escape(text))
        rc = cc_segmented_integrity_error("Session manifest contains encoded NUL");
    if (rc.code == CC_OK) rc = check_manifest_sidecars(store, path);
    if (rc.code == CC_OK) {
        rc = cc_json_parse_strict(text, out_value);
        rc = cc_segmented_classify_json_failure(rc, text);
    }
    static const char *const fields[] = {"id", "workspace_dir", "status", "segment_bytes",
                                         "media_dir", "current_segment", "segments", "name", "model",
                                         "created_at", "updated_at"};
    if (rc.code == CC_OK && !cc_storage_json_fields_unique(*out_value, fields, 11)) {
        cc_json_destroy(*out_value);
        *out_value = NULL;
        rc = cc_segmented_integrity_error("Session manifest fields are invalid or duplicated");
    }
    if (rc.code == CC_OK) {
        const char *id = cc_json_string_value(cc_json_object_get(*out_value, "id"));
        cc_json_value_t *current = cc_json_object_get(*out_value, "current_segment");
        cc_json_value_t *segments = cc_json_object_get(*out_value, "segments");
        double number = cc_json_number_value(current);
        if (!id || !id[0] || !cc_json_is_number(current) || number < 1 || number > INT_MAX ||
            number != (double)(int)number ||
            (segments && (!cc_json_is_array(segments) || cc_json_array_size(segments) != (int)number))) {
            cc_json_destroy(*out_value);
            *out_value = NULL;
            rc = cc_segmented_integrity_error("Session manifest structure is invalid");
        }
    }
    if (rc.code == CC_OK) {
        static const char *const optional_text[] = {"workspace_dir", "status", "name", "model",
                                                    "created_at", "updated_at"};
        for (size_t i = 0; i < sizeof(optional_text) / sizeof(optional_text[0]); i++) {
            cc_json_value_t *value = cc_json_object_get(*out_value, optional_text[i]);
            if (value && !cc_json_is_string(value)) {
                cc_json_destroy(*out_value);
                *out_value = NULL;
                rc = cc_segmented_integrity_error("Session manifest metadata is invalid");
                break;
            }
        }
    }
    free(text);
    return rc;
}

/** @brief 保留已有键的大小写拼写并安装独占值。
 * @param[in,out] root 必填独占 JSON 对象借用。
 * @param[in] key 必填稳定键名借用，查找兼容既有不区分大小写规则。
 * @param[in,out] value 消费独占未挂接节点，允许 NULL 表示分配失败。
 * @return 成功或 OOM，失败释放未挂接值，调用者消费诊断。
 */
static cc_result_t manifest_set(cc_json_value_t *root, const char *key, cc_json_value_t *value)
{
    if (!value) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate manifest field");
    const char *actual_key = key;
    int count = cc_json_object_size(root);
    for (int i = 0; i < count; i++) {
        const char *candidate = cc_json_object_key_at(root, i);
        if (candidate && cc_storage_json_field_equal(candidate, key)) { actual_key = candidate; break; }
    }
    cc_json_object_set(root, actual_key, value);
    if (cc_json_object_get(root, key) != value) {
        cc_json_destroy(value);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot set manifest field");
    }
    return cc_result_ok();
}

/** @brief 新建清单或保留原元数据及扩展字段后更新分段表。
 * @param[in] store 已持锁的后端借用。
 * @param[in] session_dir 必填会话目录借用。
 * @param[in] session_id 必填会话标识借用，更新时须匹配已有身份。
 * @param[in] workspace_dir 非 NULL 表示新建清单，NULL 表示读取后更新。
 * @param[in] current_segment 正数当前分段号，生成一至该编号的文件名数组。
 * @return 分配、身份、严格读取或安装错误，保存失败可能已经替换磁盘。
 * @note 所有候选树/文本/路径内部回收，更新只修改分段配置和清单字段。
 */
static cc_result_t save_manifest(
    cc_json_segmented_store_t *store,
    const char *session_dir,
    const char *session_id,
    const char *workspace_dir,
    int current_segment
)
{
    char *manifest_path = manifest_path_for_session_dir(session_dir);
    if (!manifest_path) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build manifest path");

    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_result_ok();
    if (!workspace_dir) {
        rc = parse_json_file_strict(store, manifest_path, &root);
        if (rc.code != CC_OK) {
            free(manifest_path);
            return rc;
        }
        const char *existing_id = cc_json_string_value(cc_json_object_get(root, "id"));
        if (!existing_id || !session_id || strcmp(existing_id, session_id) != 0) {
            cc_json_destroy(root);
            free(manifest_path);
            return cc_result_error(CC_ERR_STORAGE, "Session manifest identity changed");
        }
    } else {
        root = cc_json_create_object();
        if (!root) { free(manifest_path); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate manifest JSON"); }
        rc = manifest_set(root, "id", cc_json_create_string(session_id ? session_id : ""));
        if (rc.code == CC_OK) rc = manifest_set(root, "workspace_dir", cc_json_create_string(workspace_dir));
        if (rc.code == CC_OK) rc = manifest_set(root, "status", cc_json_create_string("active"));
    }
    if (rc.code == CC_OK) rc = manifest_set(root, "segment_bytes", cc_json_create_number((double)store->segment_bytes));
    if (rc.code == CC_OK) rc = manifest_set(root, "media_dir", cc_json_create_string(store->media_dir));
    if (rc.code == CC_OK) rc = manifest_set(root, "current_segment", cc_json_create_number((double)current_segment));
    cc_json_value_t *segments = rc.code == CC_OK ? cc_json_create_array() : NULL;
    if (rc.code == CC_OK && !segments) rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate manifest segments");
    for (int i = 0; rc.code == CC_OK && i < current_segment; i++) {
        char *name = segment_name(i + 1);
        cc_json_value_t *entry = name ? cc_json_create_string(name) : NULL;
        free(name);
        if (!entry) { rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate manifest segment name"); break; }
        cc_json_array_append(segments, entry);
        if (cc_json_array_size(segments) != i + 1 || cc_json_array_get(segments, i) != entry) {
            if (cc_json_array_get(segments, i) != entry) cc_json_destroy(entry);
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot append manifest segment name");
        }
    }
    if (rc.code == CC_OK) { rc = manifest_set(root, "segments", segments); segments = NULL; }
    cc_json_destroy(segments);
    char *json = rc.code == CC_OK ? cc_json_stringify(root) : NULL;
    cc_json_destroy(root);
    if (rc.code == CC_OK)
        rc = json ? write_text_file_atomic(store, manifest_path, json)
                  : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to stringify manifest");
    free(json);
    free(manifest_path);
    return rc;
}

/** @brief 严格读取当前分段并核对会话身份。
 * @param[in] store 已持锁的后端借用。
 * @param[in] session_dir 必填目录借用。
 * @param[in] session_id 必填会话名借用。
 * @param[out] out_current 必填槽，仅成功写正数编号。
 * @return 路径、读取或完整性错误，临时树及路径自行回收。
 */
static cc_result_t load_current_segment(cc_json_segmented_store_t *store,
                                        const char *session_dir, const char *session_id,
                                        int *out_current)
{
    char *manifest = manifest_path_for_session_dir(session_dir);
    if (!manifest) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate manifest path");
    cc_json_value_t *root = NULL;
    cc_result_t rc = parse_json_file_strict(store, manifest, &root);
    free(manifest);
    if (rc.code != CC_OK) return rc;
    const char *id = cc_json_string_value(cc_json_object_get(root, "id"));
    cc_json_value_t *value = cc_json_object_get(root, "current_segment");
    double parsed = cc_json_number_value(value);
    if (!cc_json_is_object(root) || !id || strcmp(id, session_id) != 0 ||
        !cc_json_is_number(value) || parsed < 1 || parsed > INT_MAX || parsed != (double)(int)parsed) {
        cc_json_destroy(root);
        return cc_segmented_integrity_error("Session manifest is invalid");
    }
    *out_current = (int)parsed;
    cc_json_destroy(root);
    return cc_result_ok();
}

/** @brief 核对规范分段文件集合，只允许额外存在空的紧邻下一段。
 * @param[in] store 已持锁且提供 list/stat 的后端借用。
 * @param[in] session_dir 必填目录借用。
 * @param[in] current 已由清单确认的正数编号。
 * @return 成功或端口、列举/查询、分配、缺段、歧义完整性错误，内部释放列名。
 */
static cc_result_t check_segment_directory(cc_json_segmented_store_t *store,
                                           const char *session_dir, int current)
{
    if (!store->fs.vtable->list_dir)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Cannot inspect session segment directory");
    char **items = NULL;
    size_t count = 0, seen = 0;
    cc_result_t rc = store->fs.vtable->list_dir(store->fs.self, session_dir, &items, &count);
    for (size_t i = 0; rc.code == CC_OK && i < count; i++) {
        const char *name = items[i];
        if (!name || strlen(name) < 7U) continue;
        int prefix = 1;
        const char *tag = "events-";
        for (size_t j = 0; j < 7U; j++)
            if (tolower((unsigned char)name[j]) != tag[j]) { prefix = 0; break; }
        if (!prefix) continue;
        const char *p = name + 7;
        unsigned long index = 0;
        if (*p < '0' || *p > '9') {
            rc = cc_segmented_integrity_error("Unrecognized session segment file");
            break;
        }
        while (*p >= '0' && *p <= '9') {
            unsigned digit = (unsigned)(*p++ - '0');
            if (index > ((unsigned long)INT_MAX - digit) / 10UL) {
                rc = cc_segmented_integrity_error("Session segment number is invalid");
                break;
            }
            index = index * 10UL + digit;
        }
        if (rc.code != CC_OK) break;
        char *expected = index ? segment_name((int)index) : NULL;
        if (!expected && index) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot validate session segment name");
            break;
        }
        int valid = expected && strcmp(name, expected) == 0;
        free(expected);
        if (!valid || strcmp(p, ".jsonl") != 0) {
            rc = cc_segmented_integrity_error("Unrecognized session segment file");
            break;
        }
        if (index <= (unsigned long)current) { seen++; continue; }
        if (current == INT_MAX || index != (unsigned long)current + 1UL) {
            rc = cc_segmented_integrity_error("Unlisted session segment requires inspection");
            break;
        }
        char *path = join_path2(session_dir, name);
        if (!path) { rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot inspect session segment"); break; }
        cc_filesystem_stat_t info = {.size = sizeof(info)};
        rc = store->fs.vtable->stat(store->fs.self, path, &info);
        free(path);
        if (rc.code == CC_OK && (info.type != CC_FS_ENTRY_FILE || info.byte_size != 0))
            rc = cc_segmented_integrity_error("Unlisted session segment requires inspection");
    }
    for (size_t i = 0; i < count; i++) free(items[i]);
    free(items);
    if (rc.code == CC_OK && seen != (size_t)current)
        rc = cc_result_error(CC_ERR_STORAGE, "Session history segment is missing");
    return rc;
}

/** @brief 验证既有会话或准备全新目录、首段及清单。
 * @param[in] store 调用者已持实例锁的后端借用。
 * @param[in] session_dir 必填目标目录借用。
 * @param[in] session_id 必填会话名借用。
 * @param[in] workspace_dir 可为 NULL 的新会话工作目录，NULL 保存空串。
 * @return 首个路径、存储、分配或同步错误，失败可能留新目录/空分段供核对。
 * @note 既有目录必须有完整清单，禁止用新清单覆盖无法验证的历史。
 */
static cc_result_t prepare_session_locked(cc_json_segmented_store_t *store,
                                          const char *session_dir,
                                          const char *session_id,
                                          const char *workspace_dir)
{
    cc_filesystem_stat_t info = {.size = sizeof(info)};
    cc_result_t rc = store->fs.vtable->stat(store->fs.self, session_dir, &info);
    if (rc.code != CC_OK) return rc;
    if (info.type == CC_FS_ENTRY_DIRECTORY) {
        int current = 0;
        rc = load_current_segment(store, session_dir, session_id, &current);
        if (rc.code == CC_OK) rc = check_segment_directory(store, session_dir, current);
        char *segment = rc.code == CC_OK ? segment_path_for_session_dir(session_dir, current) : NULL;
        if (rc.code == CC_OK && !segment)
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build segment path");
        if (rc.code == CC_OK) {
            cc_filesystem_stat_t seg_info = {.size = sizeof(seg_info)};
            rc = store->fs.vtable->stat(store->fs.self, segment, &seg_info);
            if (rc.code == CC_OK && seg_info.type != CC_FS_ENTRY_FILE)
                rc = cc_result_error(CC_ERR_STORAGE, "Current session segment is missing");
        }
        free(segment);
        if (rc.code == CC_OK) rc = ensure_media_dirs(store, session_dir);
        return rc;
    }
    if (info.type != CC_FS_ENTRY_MISSING)
        return cc_result_error(CC_ERR_STORAGE, "Session path is not a directory");
    rc = ensure_dir_recursive(store, session_dir);
    if (rc.code == CC_OK) rc = ensure_media_dirs(store, session_dir);
    char *segment = rc.code == CC_OK ? segment_path_for_session_dir(session_dir, 1) : NULL;
    if (rc.code == CC_OK && !segment)
        rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build segment path");
    if (rc.code == CC_OK) {
        rc = store->fs.vtable->append_bytes(store->fs.self, segment, NULL, 0);
        if (rc.code == CC_OK) rc = store->fs.vtable->sync_file(store->fs.self, segment);
    }
    if (rc.code == CC_OK && store->fs.vtable->sync_dir) {
        cc_result_t sync_rc = store->fs.vtable->sync_dir(store->fs.self, session_dir);
        if (sync_rc.code != CC_OK && sync_rc.code != CC_ERR_UNSUPPORTED) rc = sync_rc;
        else cc_result_free(&sync_rc);
    }
    if (rc.code == CC_OK)
        rc = save_manifest(store, session_dir, session_id, workspace_dir ? workspace_dir : "", 1);
    free(segment);
    return rc;
}

/** @brief 显式准备新会话或验证同名既有会话。
 * @param[in,out] self 必填存活实例借用，外层排除并发销毁。
 * @param[in] session_id 必填非空会话名借用。
 * @param[in] workspace_dir 可为 NULL 的工作目录借用，新建按空串。
 * @return 参数、目录准备或清单结果，失败可能已有持久前缀。
 * @note 根目录准备后持锁，先使同会话游标失效，再准备会话。
 */
static cc_result_t segmented_create_session(
    void *self,
    const char *session_id,
    const char *workspace_dir
)
{
    cc_json_segmented_store_t *store = (cc_json_segmented_store_t *)self;
    if (!store || !session_id || !*session_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid segmented session create request");
    }
    cc_result_t rc = ensure_dir_recursive(store, store->root_path);
    if (rc.code != CC_OK) return rc;

    char *session_dir = session_dir_path(store, session_id);
    if (!session_dir) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build session directory");
    cc_mutex_lock(store->mutex);
    invalidate_cursors_locked(store, session_id);
    rc = prepare_session_locked(store, session_dir, session_id, workspace_dir);
    cc_mutex_unlock(store->mutex);
    free(session_dir);
    return rc;
}

/** @brief 选择媒体来源目录。
 * @param[in] role 消息角色，其他值沿 user 目录。
 * @return 静态 assistant/tool/user 文本借用，无资源交接。
 */
static const char *role_source(cc_message_role_t role)
{
    if (role == CC_ROLE_ASSISTANT) return "assistant";
    if (role == CC_ROLE_TOOL) return "tool";
    return "user";
}

/** @brief 优先按 MIME、随后按 kind 选择媒体分类。
 * @param[in] artifact 可为 NULL 的调用期制品借用。
 * @return 静态 img/audio/video/file/misc 借用，无分配。
 */
static const char *media_kind_dir(const cc_media_artifact_t *artifact)
{
    if (!artifact) return "misc";
    if (artifact->mime) {
        if (strncmp(artifact->mime, "image/", 6) == 0) return "img";
        if (strncmp(artifact->mime, "audio/", 6) == 0) return "audio";
        if (strncmp(artifact->mime, "video/", 6) == 0) return "video";
        if (strncmp(artifact->mime, "application/", 12) == 0) return "file";
    }
    if (artifact->kind == CC_MEDIA_IMAGE) return "img";
    if (artifact->kind == CC_MEDIA_AUDIO) return "audio";
    if (artifact->kind == CC_MEDIA_VIDEO) return "video";
    if (artifact->kind == CC_MEDIA_FILE) return "file";
    return "misc";
}

/** @brief 按已知 MIME 映射扩展名。
 * @param[in] artifact 可为 NULL 的调用期制品借用。
 * @return 静态含点扩展名借用，未知为 .bin，无分配。
 */
static const char *extension_for_artifact(const cc_media_artifact_t *artifact)
{
    const char *mime = artifact ? artifact->mime : NULL;
    if (mime) {
        if (strcmp(mime, "image/jpeg") == 0 || strcmp(mime, "image/jpg") == 0) return ".jpg";
        if (strcmp(mime, "image/png") == 0) return ".png";
        if (strcmp(mime, "image/webp") == 0) return ".webp";
        if (strcmp(mime, "audio/wav") == 0 || strcmp(mime, "audio/x-wav") == 0) return ".wav";
        if (strcmp(mime, "audio/mpeg") == 0 || strcmp(mime, "audio/mp3") == 0) return ".mp3";
        if (strcmp(mime, "video/mp4") == 0) return ".mp4";
        if (strcmp(mime, "application/json") == 0) return ".json";
        if (strcmp(mime, "text/plain") == 0) return ".txt";
    }
    return ".bin";
}

/** @brief 检查路径前缀和目录分隔边界。
 * @param[in] path 可为 NULL 的路径借用。
 * @param[in] prefix 可为 NULL 的前缀借用。
 * @return 完整前缀后为 NUL、斜杠或反斜杠时为一，否则零，不规范化路径。
 */
static int path_starts_with(const char *path, const char *prefix)
{
    if (!path || !prefix) return 0;
    size_t len = strlen(prefix);
    return strncmp(path, prefix, len) == 0 &&
           (path[len] == '\0' || path[len] == '/' || path[len] == '\\');
}

/** @brief 验证媒体相对路径的组件语法。
 * @param[in] path 可为 NULL 的路径借用。
 * @return 非空且无绝对根、空组件、点组件、反斜杠、冒号及控制字节为一。
 * @note 不访问磁盘，链接及外部目录改装由文件系统和装配方约束。
 */
static int media_relative_path_safe(const char *path)
{
    if (!path || !path[0] || path[0] == '/') return 0;
    const char *segment = path;
    for (const char *p = path;; p++) {
        unsigned char c = (unsigned char)*p;
        if (c && (c < 0x20U || c == '\\' || c == ':')) return 0;
        if (c == '/' || c == '\0') {
            size_t n = (size_t)(p - segment);
            if (!n || (n == 1 && segment[0] == '.') ||
                (n == 2 && segment[0] == '.' && segment[1] == '.')) return 0;
            if (!c) break;
            segment = p + 1;
        }
    }
    return 1;
}

/** @brief 比较已存媒体完整字节，并检查读取前后元数据稳定性。
 * @param[in] store 已持锁的后端借用，提供 stat/read_bytes。
 * @param[in] path 必填媒体路径借用。
 * @param[in] data 借用 len 字节，len 为零可为 NULL。
 * @param[in] len 预期字节数，长度不符时跳过整文件读取。
 * @param[out] out_matches 必填槽，先置零，成功按完整字节比较写入。
 * @param[out] out_stat 可为 NULL，成功交读取后的元数据副本。
 * @return 查询/读取、变化或存储错误，临时正文内部释放，诊断归调用者。
 */
static cc_result_t inspect_media_bytes(
    cc_json_segmented_store_t *store, const char *path, const unsigned char *data, size_t len,
    int *out_matches, cc_filesystem_stat_t *out_stat)
{
    *out_matches = 0;
    cc_filesystem_stat_t before = {.size = sizeof(before)}, after = {.size = sizeof(after)};
    cc_result_t rc = store->fs.vtable->stat(store->fs.self, path, &before);
    if (rc.code != CC_OK) return rc;
    if (before.type != CC_FS_ENTRY_FILE)
        return cc_result_error(CC_ERR_STORAGE, "Media file is missing or invalid");
    void *actual = NULL;
    size_t actual_len = 0;
    /* 长度不同可直接判定不匹配，仍复核元数据，避免加载任意大的遗留文件。 */
    if (before.byte_size == (uint64_t)len)
        rc = store->fs.vtable->read_bytes(store->fs.self, path, &actual, &actual_len);
    if (rc.code == CC_OK)
        rc = store->fs.vtable->stat(store->fs.self, path, &after);
    if (rc.code == CC_OK && (after.type != CC_FS_ENTRY_FILE ||
        before.byte_size != after.byte_size || before.modified_ms != after.modified_ms ||
        (before.byte_size == (uint64_t)len && (actual_len != len || (len && !actual)))))
        rc = cc_result_error(CC_ERR_STORAGE, "Media file changed during inspection");
    if (rc.code == CC_OK) {
        *out_matches = before.byte_size == (uint64_t)len &&
            (!len || memcmp(actual, data, len) == 0);
        if (out_stat) *out_stat = after;
    }
    free(actual);
    return rc;
}

/** @brief 要求已存媒体与预期正文完全一致。
 * @param[in] store 已持锁后端借用。
 * @param[in] path 必填路径借用。
 * @param[in] data 借用 len 字节，零长度可为 NULL。
 * @param[in] len 预期字节数。
 * @return 完整相符成功，其他为读取/变化/不匹配错误，无拥有输出。
 */
static cc_result_t verify_media_bytes(
    cc_json_segmented_store_t *store, const char *path, const unsigned char *data, size_t len)
{
    int matches = 0;
    cc_result_t rc = inspect_media_bytes(store, path, data, len, &matches, NULL);
    if (rc.code == CC_OK && !matches)
        return cc_result_error(CC_ERR_STORAGE, "Existing media file does not match expected content");
    return rc;
}

/** @brief 请求媒体父目录同步。
 * @param[in] store 已持锁后端借用，sync_dir 可缺失。
 * @param[in] path 必填完整路径借用。
 * @return 缺能力或 UNSUPPORTED 成功，其他透传或 OOM，内部释放路径。
 */
static cc_result_t sync_media_parent(cc_json_segmented_store_t *store, const char *path)
{
    if (!store->fs.vtable->sync_dir) return cc_result_ok();
    char *parent = cc_copy_string(path);
    if (!parent) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate media parent path");
    char *slash = strrchr(parent, '/');
    char *backslash = strrchr(parent, '\\');
    char *cut = slash;
    if (!cut || (backslash && backslash > cut)) cut = backslash;
    if (cut) *cut = '\0';
    cc_result_t rc = store->fs.vtable->sync_dir(store->fs.self, cut && parent[0] ? parent : ".");
    free(parent);
    if (rc.code == CC_ERR_UNSUPPORTED) { cc_result_free(&rc); return cc_result_ok(); }
    return rc;
}

/** @brief 最多八次生成不存在的媒体暂存文件名。
 * @param[in] store 已持锁后端借用，外部写者已排除。
 * @param[in] temporary 必填原暂存路径借用，新文件位于同目录。
 * @param[out] out_path 必填新槽，先置 NULL，成功路径由调用者 free。
 * @return 成功或生成 ID、分配、查询、重试耗尽错误，临时资源内部回收。
 */
static cc_result_t alternate_media_temporary(
    cc_json_segmented_store_t *store, const char *temporary, char **out_path)
{
    *out_path = NULL;
    char *parent = cc_copy_string(temporary);
    cc_result_t rc = parent ? cc_result_ok() :
        cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate media staging directory");
    if (rc.code == CC_OK) {
        char *slash = strrchr(parent, '/');
        char *backslash = strrchr(parent, '\\');
        char *cut = slash;
        if (!cut || (backslash && backslash > cut)) cut = backslash;
        if (cut) *cut = '\0'; else { free(parent); parent = cc_copy_string("."); }
        if (!parent) rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate media staging directory");
    }
    int selected = 0;
    for (int attempt = 0; rc.code == CC_OK && attempt < 8 && !selected; attempt++) {
        char *id = NULL;
        rc = cc_id_generate_uuid_v4("media-attempt-", &id);
        if (rc.code != CC_OK) break;
        char *name = dup_printf("%s.tmp", id);
        free(id);
        char *candidate = name ? join_path2(parent, name) : NULL;
        free(name);
        if (!candidate) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate media staging path");
            break;
        }
        cc_filesystem_stat_t info = {.size = sizeof(info)};
        rc = store->fs.vtable->stat(store->fs.self, candidate, &info);
        if (rc.code == CC_OK && info.type == CC_FS_ENTRY_MISSING) {
            *out_path = candidate;
            selected = 1;
        } else free(candidate);
    }
    if (rc.code == CC_OK && !selected)
        rc = cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Cannot allocate a unique media staging name");
    free(parent);
    return rc;
}

/** @brief 验证已有媒体或经暂存同步安装新媒体，保留不同的遗留副本。
 * @param[in] store 已持锁的后端借用，调用方另排除同目录跨实例/进程写入。
 * @param[in] path 必填目标路径借用。
 * @param[in] data 借用 len 字节，零长度允许 NULL。
 * @param[in] len 完整正文长度。
 * @return 端口、路径、分配、查询/读写、同步或替换错误，失败可能已有暂存/安装结果。
 * @note 已有目标须完整相符，bak 存在拒绝，部分 tmp 原位保留并另选暂存，临时路径内部释放。
 */
static cc_result_t write_bytes_if_missing(
    cc_json_segmented_store_t *store,
    const char *path,
    const unsigned char *data,
    size_t len
)
{
    if (!store || !path || (len && !data) || !store->fs.vtable ||
        !store->fs.vtable->stat || !store->fs.vtable->read_bytes ||
        !store->fs.vtable->write_bytes || !store->fs.vtable->sync_file ||
        !store->fs.vtable->atomic_replace || !store->fs.vtable->remove)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Filesystem cannot safely install media bytes");

    char *temporary = dup_printf("%s.tmp", path);
    char *backup = dup_printf("%s.bak", path);
    char *alternate = NULL;
    const char *working = temporary;
    int preserve_temporary = 0;
    if (!temporary || !backup) {
        free(temporary); free(backup);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate media recovery paths");
    }
    cc_filesystem_stat_t destination = {.size = sizeof(destination)};
    cc_filesystem_stat_t staged = {.size = sizeof(staged)};
    cc_filesystem_stat_t saved = {.size = sizeof(saved)};
    cc_result_t rc = store->fs.vtable->stat(store->fs.self, backup, &saved);
    if (rc.code == CC_OK && saved.type != CC_FS_ENTRY_MISSING)
        rc = cc_result_error(CC_ERR_STORAGE, "Media backup requires inspection");
    if (rc.code == CC_OK) rc = store->fs.vtable->stat(store->fs.self, path, &destination);
    if (rc.code == CC_OK) rc = store->fs.vtable->stat(store->fs.self, temporary, &staged);
    if (rc.code == CC_OK && destination.type != CC_FS_ENTRY_MISSING &&
        destination.type != CC_FS_ENTRY_FILE)
        rc = cc_result_error(CC_ERR_STORAGE, "Media destination is not a file");
    if (rc.code == CC_OK && staged.type != CC_FS_ENTRY_MISSING && staged.type != CC_FS_ENTRY_FILE)
        rc = cc_result_error(CC_ERR_STORAGE, "Media temporary path is not a file");
    if (rc.code == CC_OK && destination.type == CC_FS_ENTRY_FILE)
        rc = verify_media_bytes(store, path, data, len);
    if (rc.code == CC_OK && staged.type == CC_FS_ENTRY_FILE) {
        int matches = 0;
        cc_filesystem_stat_t observed = {.size = sizeof(observed)};
        rc = inspect_media_bytes(store, temporary, data, len, &matches, &observed);
        if (rc.code == CC_OK && !matches) {
            /* 新建字节前先保留并同步旧暂存副本。 */
            cc_filesystem_stat_t current = {.size = sizeof(current)};
            rc = store->fs.vtable->sync_file(store->fs.self, temporary);
            if (rc.code == CC_OK) rc = store->fs.vtable->stat(store->fs.self, temporary, &current);
            if (rc.code == CC_OK && (current.type != CC_FS_ENTRY_FILE ||
                current.byte_size != observed.byte_size || current.modified_ms != observed.modified_ms))
                rc = cc_result_error(CC_ERR_STORAGE, "Media temporary changed before retry");
            if (rc.code == CC_OK) rc = sync_media_parent(store, temporary);
            if (rc.code == CC_OK && destination.type == CC_FS_ENTRY_MISSING) {
                rc = alternate_media_temporary(store, temporary, &alternate);
                if (rc.code == CC_OK) { working = alternate; staged.type = CC_FS_ENTRY_MISSING; }
            } else if (rc.code == CC_OK) preserve_temporary = 1;
        }
    }

    if (rc.code == CC_OK && destination.type == CC_FS_ENTRY_MISSING) {
        if (staged.type == CC_FS_ENTRY_MISSING)
            rc = store->fs.vtable->write_bytes(store->fs.self, working, data, len);
        if (rc.code == CC_OK) rc = store->fs.vtable->sync_file(store->fs.self, working);
        if (rc.code == CC_OK) rc = verify_media_bytes(store, working, data, len);
        /* 调用者独占目录，安装前仍复核，检测到竞争目标时先验证其完整字节。 */
        if (rc.code == CC_OK) rc = store->fs.vtable->stat(store->fs.self, backup, &saved);
        if (rc.code == CC_OK && saved.type != CC_FS_ENTRY_MISSING)
            rc = cc_result_error(CC_ERR_STORAGE, "Media backup appeared during installation");
        if (rc.code == CC_OK) rc = store->fs.vtable->stat(store->fs.self, path, &destination);
        if (rc.code == CC_OK && destination.type == CC_FS_ENTRY_FILE)
            rc = verify_media_bytes(store, path, data, len);
        else if (rc.code == CC_OK && destination.type == CC_FS_ENTRY_MISSING)
            rc = store->fs.vtable->atomic_replace(store->fs.self, working, path);
        else if (rc.code == CC_OK)
            rc = cc_result_error(CC_ERR_STORAGE, "Media destination changed during installation");
    }
    if (rc.code == CC_OK) rc = verify_media_bytes(store, path, data, len);
    if (rc.code == CC_OK) rc = store->fs.vtable->sync_file(store->fs.self, path);
    if (rc.code == CC_OK) rc = sync_media_parent(store, path);
    if (rc.code == CC_OK && !preserve_temporary) {
        staged.size = sizeof(staged);
        rc = store->fs.vtable->stat(store->fs.self, working, &staged);
        if (rc.code == CC_OK && staged.type == CC_FS_ENTRY_FILE) {
            rc = verify_media_bytes(store, working, data, len);
            if (rc.code == CC_OK) rc = store->fs.vtable->remove(store->fs.self, working);
            if (rc.code == CC_OK) rc = sync_media_parent(store, path);
        } else if (rc.code == CC_OK && staged.type != CC_FS_ENTRY_MISSING) {
            rc = cc_result_error(CC_ERR_STORAGE, "Media temporary path changed during installation");
        }
    }
    free(alternate); free(temporary); free(backup);
    return rc;
}

/** @brief 读取完整媒体字节并可选计算格式兼容哈希。
 * @param[in] store 必填调用期后端借用。
 * @param[in] path 路径借用，校验交后端。
 * @param[out] out_data 必填空槽，合法输出入口置 NULL，成功交调用者 free 的缓冲。
 * @param[out] out_len 必填字节数槽，合法输出入口置零。
 * @param[out] out_hash 可为 NULL，仅成功时写完整字节哈希。
 * @return 参数、端口或读取结果，调用者释放诊断，完整读取预算由外层控制。
 */
static cc_result_t read_file_bytes(
    cc_json_segmented_store_t *store,
    const char *path,
    unsigned char **out_data,
    size_t *out_len,
    unsigned long long *out_hash
)
{
    if (!out_data || !out_len) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid file read output");
    *out_data = NULL;
    *out_len = 0;
    if (!store || !store->fs.vtable || !store->fs.vtable->read_bytes) {
        return cc_result_error(CC_ERR_UNSUPPORTED, "Filesystem cannot read media bytes");
    }
    void *raw = NULL;
    size_t n = 0;
    cc_result_t rc = store->fs.vtable->read_bytes(store->fs.self, path, &raw, &n);
    if (rc.code != CC_OK) return rc;
    unsigned char *buf = (unsigned char *)raw;
    if (out_hash) *out_hash = fnv1a_bytes(buf, n);
    *out_data = buf;
    *out_len = n;
    return cc_result_ok();
}

/** @brief 按媒体分类及来源拼接会话内相对路径。
 * @param[in] store 必填稳定后端借用。
 * @param[in] kind_dir 必填静态分类借用，misc 不分来源。
 * @param[in] source 可为 NULL 的来源借用，NULL 按 user。
 * @param[in] file_name 文件名借用。
 * @return 调用者 free 的路径，分配失败 NULL，临时路径内部释放。
 */
static char *media_relative_path(
    cc_json_segmented_store_t *store,
    const char *kind_dir,
    const char *source,
    const char *file_name
)
{
    if (strcmp(kind_dir, "misc") == 0) {
        return join_path3(store->media_dir, "misc", file_name);
    }
    char *kind_source = join_path3(store->media_dir, kind_dir, source ? source : "user");
    if (!kind_source) return NULL;
    char *path = join_path2(kind_source, file_name);
    free(kind_source);
    return path;
}

/** @brief 将独占制品正文保存到会话媒体目录，成功后改存相对路径。
 * @param[in] store 可为 NULL 的后端借用，NULL 跳过，非空时调用者持实例锁。
 * @param[in] session_dir 可为 NULL 的目录借用，NULL 跳过。
 * @param[in] source 可为 NULL 的来源借用，沿来源目录规则。
 * @param[in] record_id 可为 NULL 的记录 ID，NULL 使用制品 ID。
 * @param[in] part_index 零基索引，仅参与文件名。
 * @param[in,out] artifact 可为 NULL 的独占制品，成功消费原 path/base64 并安装新路径。
 * @return 文本/空制品直接成功，其他为路径权限、解码、分配或媒体安装结果。
 * @note 仅修改消息/制品的副本，失败可能已保存媒体文件，原输入由外层保管。
 */
static cc_result_t externalize_artifact(
    cc_json_segmented_store_t *store,
    const char *session_dir,
    const char *source,
    const char *record_id,
    size_t part_index,
    cc_media_artifact_t *artifact
)
{
    if (!store || !session_dir || !artifact || artifact->kind == CC_MEDIA_TEXT) {
        return cc_result_ok();
    }
    if (artifact->path && path_starts_with(artifact->path, store->media_dir)) {
        if (!media_relative_path_safe(artifact->path))
            return cc_result_error(CC_ERR_PERMISSION_DENIED, "Unsafe stored media path");
        if (artifact->data_base64) {
            unsigned char *data = NULL;
            size_t data_len = 0;
            cc_result_t rc = cc_base64_decode(artifact->data_base64, &data, &data_len);
            char *abs = rc.code == CC_OK ? join_path2(session_dir, artifact->path) : NULL;
            if (rc.code == CC_OK && !abs)
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate stored media path");
            if (rc.code == CC_OK) rc = write_bytes_if_missing(store, abs, data, data_len);
            free(abs);
            free(data);
            if (rc.code != CC_OK) return rc;
            free(artifact->data_base64);
            artifact->data_base64 = NULL;
            artifact->bytes = data_len;
        }
        return cc_result_ok();
    }
    if (!artifact->data_base64 && !artifact->path) {
        return cc_result_ok();
    }

    unsigned char *data = NULL;
    size_t data_len = 0;
    unsigned long long hash = 0;
    cc_result_t rc = cc_result_ok();
    /* 无路径的空 base64 表示零字节文件，同时有外部路径时沿既有路径读取行为。 */
    if (artifact->data_base64 && (artifact->data_base64[0] || !artifact->path || !artifact->path[0])) {
        rc = cc_base64_decode(artifact->data_base64, &data, &data_len);
        if (rc.code != CC_OK) return rc;
        hash = fnv1a_bytes(data, data_len);
    } else if (artifact->path && artifact->path[0]) {
        rc = read_file_bytes(store, artifact->path, &data, &data_len, &hash);
        if (rc.code != CC_OK) {
            if (rc.code == CC_ERR_OUT_OF_MEMORY || rc.code == CC_ERR_LIMIT_EXCEEDED) return rc;
            cc_result_free(&rc);
            return cc_result_ok();
        }
    }
    if (!data) {
        return cc_result_ok();
    }

    const char *kind_dir = media_kind_dir(artifact);
    const char *ext = extension_for_artifact(artifact);
    const char *label = (source && strcmp(source, "tool") == 0) ? "artifact" : "part";
    char *safe_record = safe_component(record_id ? record_id : artifact->id);
    if (!safe_record) {
        free(data);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build media file name");
    }
    char *file_name = dup_printf(
        "%s-%s-%04u-%08llx%s",
        safe_record,
        label,
        (unsigned)(part_index + 1),
        hash & 0xffffffffull,
        ext);
    free(safe_record);
    if (!file_name) {
        free(data);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate media file name");
    }

    char *rel = media_relative_path(store, kind_dir, source, file_name);
    char *abs = rel ? join_path2(session_dir, rel) : NULL;
    free(file_name);
    if (!rel || !abs) {
        free(rel);
        free(abs);
        free(data);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build media path");
    }
    rc = write_bytes_if_missing(store, abs, data, data_len);
    if (rc.code == CC_OK) {
        free(artifact->path);
        artifact->path = rel;
        rel = NULL;
        free(artifact->data_base64);
        artifact->data_base64 = NULL;
        artifact->bytes = data_len;
    }
    free(rel);
    free(abs);
    free(data);
    return rc;
}

/** @brief 将会话内媒体读回并编码到独占制品。
 * @param[in] store 可为 NULL 的后端借用，非空时调用者持实例锁。
 * @param[in] session_dir 可为 NULL 的会话目录借用。
 * @param[in,out] artifact 可为 NULL 的独占制品，已含正文时保持，成功接管新 base64。
 * @return 不适用、预读超单件原始字节限制或普通媒体查询/读取失败时成功跳过。
 * @note OOM/容量失败向上传播，读取后增长超限返回 LIMIT。临时字节内部回收，成功编码更新缺失的 bytes。
 */
static cc_result_t hydrate_artifact(
    cc_json_segmented_store_t *store,
    const char *session_dir,
    cc_media_artifact_t *artifact
)
{
    if (!store || !session_dir || !artifact || !artifact->path ||
        artifact->data_base64 || !path_starts_with(artifact->path, store->media_dir)) {
        return cc_result_ok();
    }
    if (!media_relative_path_safe(artifact->path))
        return cc_result_error(CC_ERR_PERMISSION_DENIED, "Unsafe stored media path");
    char *abs = join_path2(session_dir, artifact->path);
    if (!abs) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build media hydrate path");
    size_t size = 0;
    cc_result_t rc = media_file_size(store, abs, &size);
    if (rc.code != CC_OK) {
        free(abs);
        if (rc.code == CC_ERR_OUT_OF_MEMORY || rc.code == CC_ERR_LIMIT_EXCEEDED) return rc;
        /* 缺失或普通查询失败保留元数据，禁止按零大小继续读取。 */
        cc_result_free(&rc);
        return cc_result_ok();
    }
    if (store->max_base64_bytes > 0 && size > store->max_base64_bytes) {
        free(abs);
        return cc_result_ok();
    }
    unsigned char *data = NULL;
    size_t len = 0;
    rc = read_file_bytes(store, abs, &data, &len, NULL);
    free(abs);
    if (rc.code != CC_OK) {
        if (rc.code == CC_ERR_OUT_OF_MEMORY || rc.code == CC_ERR_LIMIT_EXCEEDED) return rc;
        cc_result_free(&rc);
        return cc_result_ok();
    }
    if (store->max_base64_bytes && len > store->max_base64_bytes) {
        free(data);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Stored media grew beyond the inline limit");
    }
    char *base64 = NULL;
    rc = cc_base64_encode(data, len, &base64);
    free(data);
    if (rc.code != CC_OK) return rc;
    artifact->data_base64 = base64;
    if (artifact->bytes == 0) artifact->bytes = len;
    return cc_result_ok();
}

/** @brief 顺序回填消息的全部非文本部分。
 * @param[in] store 调用者已持锁的后端借用。
 * @param[in] session_dir 必填目录借用。
 * @param[in,out] message 必填独占有效消息，失败保留此前已回填部分。
 * @return 首个制品回填错误或成功，拥有结果由调用者消费。
 */
static cc_result_t hydrate_message(
    cc_json_segmented_store_t *store,
    const char *session_dir,
    cc_message_t *message
)
{
    for (size_t i = 0; i < message->content.count; i++) {
        cc_content_part_t *part = &message->content.items[i];
        if (part->kind == CC_MEDIA_TEXT) continue;
        cc_result_t rc = hydrate_artifact(store, session_dir, &part->artifact);
        if (rc.code != CC_OK) return rc;
    }
    return cc_result_ok();
}

/** @brief 非空字符串复制到对象并检查节点挂接。
 * @param[in,out] obj 必填独占 JSON 对象借用。
 * @param[in] key 必填键名借用。
 * @param[in] value 可为 NULL 的文本借用，NULL 跳过。
 * @return 成功或跳过为一，分配/挂接失败为零，候选节点内部消费。
 */
static int json_set_string_if_present(cc_json_value_t *obj, const char *key, const char *value)
{
    return !value || cc_json_object_set_owned(obj, key, cc_json_create_string(value));
}

/** @brief 完整复制消息身份、正文、调用及可选元数据到候选对象。
 * @param[in] message 必填稳定有效消息借用，工具状态须合法。
 * @param[out] out_obj 必填空槽，先置 NULL，成功树由调用者 destroy 或挂接。
 * @return 参数状态、序列化或分配错误，失败清理全部候选和临时 JSON 文本。
 */
static cc_result_t message_to_record_object(const cc_message_t *message, cc_json_value_t **out_obj)
{
    *out_obj = NULL;
    if (!cc_message_tool_outcome_valid(message))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid message tool outcome");
    cc_json_value_t *msg = cc_json_create_object();
    if (!msg) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate message record");
    if (!json_set_string_if_present(msg, "id", message->id ? message->id : "") ||
        !json_set_string_if_present(msg, "session_id", message->session_id ? message->session_id : "") ||
        !json_set_string_if_present(msg, "role", cc_message_role_string(message->role))) {
        cc_json_destroy(msg);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode message identity");
    }
    char *content_parts = NULL;
    cc_result_t rc = cc_content_parts_to_json(&message->content, &content_parts);
    if (rc.code != CC_OK) {
        cc_json_destroy(msg);
        return rc;
    }
    int installed = json_set_string_if_present(msg, "content_parts", content_parts ? content_parts : "[]");
    free(content_parts);
    if (!installed) { cc_json_destroy(msg); return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode message content"); }
    char *tool_calls = NULL;
    rc = cc_tool_call_list_to_json(&message->tool_calls, &tool_calls);
    if (rc.code != CC_OK) {
        cc_json_destroy(msg);
        return rc;
    }
    installed = json_set_string_if_present(msg, "tool_calls", tool_calls ? tool_calls : "[]");
    free(tool_calls);
    if (!installed ||
        !json_set_string_if_present(msg, "reasoning_content", message->reasoning_content) ||
        !json_set_string_if_present(msg, "tool_call_id", message->tool_call_id) ||
        !json_set_string_if_present(msg, "created_at", message->created_at) ||
        (message->tool_outcome != CC_MESSAGE_TOOL_OUTCOME_RECORDED &&
            !json_set_string_if_present(msg, "tool_outcome", cc_message_tool_outcome_string(message->tool_outcome)))) {
        cc_json_destroy(msg);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode message metadata");
    }
    *out_obj = msg;
    return cc_result_ok();
}

/** @brief 验证记录并深复制成拥有全部字段的消息。
 * @param[in] msg 已解析的稳定节点借用。
 * @param[in] session_id 必填预期会话名借用，须匹配记录身份。
 * @param[out] out 必填无拥有字段的槽，先清零，成功由调用者 cleanup。
 * @return 结构/数组/工具状态或 OOM，失败回收部分输出，调用者持存储锁。
 */
static cc_result_t message_from_record_object(cc_json_value_t *msg, const char *session_id,
                                              cc_message_t *out)
{
    memset(out, 0, sizeof(*out));
    static const char *const fields[] = {"id", "session_id", "role", "content_parts", "tool_calls",
                                         "reasoning_content", "tool_call_id", "created_at", "tool_outcome"};
    if (!cc_storage_json_fields_unique(msg, fields, 9))
        return cc_result_error(CC_ERR_STORAGE, "Stored message object is invalid");
    const char *id = cc_json_string_value(cc_json_object_get(msg, "id"));
    const char *sid = cc_json_string_value(cc_json_object_get(msg, "session_id"));
    const char *role = cc_json_string_value(cc_json_object_get(msg, "role"));
    const char *parts_json = cc_json_string_value(cc_json_object_get(msg, "content_parts"));
    const char *tc_json = cc_json_string_value(cc_json_object_get(msg, "tool_calls"));
    if (!id || !id[0] || !sid || strcmp(sid, session_id) != 0 || !role ||
        (strcmp(role, "system") != 0 && strcmp(role, "user") != 0 &&
         strcmp(role, "assistant") != 0 && strcmp(role, "tool") != 0))
        return cc_result_error(CC_ERR_STORAGE, "Stored message identity or role is invalid");
    const char *const optional[] = {"reasoning_content", "tool_call_id", "created_at"};
    for (size_t i = 0; i < 3; i++) {
        cc_json_value_t *value = cc_json_object_get(msg, optional[i]);
        if (value && !cc_json_is_string(value))
            return cc_result_error(CC_ERR_STORAGE, "Stored message field is not a string");
    }
    cc_result_t rc = cc_storage_json_validate_array(parts_json, 0);
    if (rc.code == CC_OK) rc = cc_storage_json_validate_array(tc_json, 1);
    if (rc.code == CC_ERR_JSON) {
        rc = cc_segmented_classify_json_failure(rc,
            cc_json_syntax_check(parts_json) == CC_JSON_SYNTAX_INVALID ? parts_json : tc_json);
    }
    if (rc.code != CC_OK) return rc;
    out->id = id ? cc_copy_string(id) : NULL;
    out->session_id = sid ? cc_copy_string(sid) : NULL;
    out->role = cc_message_role_from_string(role);
    cc_content_parts_init(&out->content);
    cc_tool_call_list_init(&out->tool_calls);
    rc = cc_content_parts_from_json(parts_json, &out->content);
    if (rc.code == CC_OK) {
        rc = cc_tool_call_list_from_json(tc_json, &out->tool_calls);
    }
    if (rc.code == CC_OK) {
        const char *reasoning = cc_json_string_value(cc_json_object_get(msg, "reasoning_content"));
        const char *tool_call_id = cc_json_string_value(cc_json_object_get(msg, "tool_call_id"));
        const char *created_at = cc_json_string_value(cc_json_object_get(msg, "created_at"));
        out->reasoning_content = reasoning ? cc_copy_string(reasoning) : NULL;
        out->tool_call_id = tool_call_id ? cc_copy_string(tool_call_id) : NULL;
        out->created_at = created_at ? cc_copy_string(created_at) : NULL;
        if ((reasoning && !out->reasoning_content) ||
            (tool_call_id && !out->tool_call_id) ||
            (created_at && !out->created_at) ||
            (id && !out->id) || (sid && !out->session_id)) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy message record");
        }
    }
    if (rc.code == CC_OK) {
        cc_json_value_t *outcome = cc_json_object_get(msg, "tool_outcome");
        if (outcome && !cc_json_is_string(outcome))
            rc = cc_result_error(CC_ERR_STORAGE, "Stored tool outcome is not a string");
        else if (outcome) rc = cc_message_tool_outcome_parse(cc_json_string_value(outcome), &out->tool_outcome);
        if (rc.code == CC_OK && !cc_message_tool_outcome_valid(out))
            rc = cc_result_error(CC_ERR_STORAGE, "Stored tool outcome has invalid message fields");
    }
    if (rc.code != CC_OK) cc_message_cleanup(out);
    return rc;
}

/** @brief 将完整批次封装成单行追加，必要时先建立下一段并更新清单。
 * @param[in] store 已持锁的后端借用，外部写者须另排除。
 * @param[in] session_dir 必填已准备目录借用。
 * @param[in] session_id 必填会话名借用。
 * @param[in] record 必填稳定批次对象借用，所有权保留。
 * @return 序列化、长度、路径、清单或 I/O 错误，失败可能已有新清单或持久追加前缀。
 * @note 软分段阈值允许单批超阈值，临时行及 framed 字节内部释放。
 */
static cc_result_t append_record_line(
    cc_json_segmented_store_t *store,
    const char *session_dir,
    const char *session_id,
    cc_json_value_t *record
)
{
    char *line = cc_json_stringify_unformatted(record);
    if (!line) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to serialize record");
    size_t payload_len = strlen(line);
    if (payload_len > CC_SEGMENTED_MAX_LINE_READ) {
        free(line);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "JSONL record exceeds read limit");
    }
    if (payload_len == SIZE_MAX) {
        free(line);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Segment record length overflow");
    }
    size_t line_len = payload_len + 1;
    int current = 0;
    cc_result_t current_rc = load_current_segment(store, session_dir, session_id, &current);
    if (current_rc.code != CC_OK) { free(line); return current_rc; }
    cc_result_free(&current_rc);
    char *seg_path = segment_path_for_session_dir(session_dir, current);
    if (!seg_path) {
        free(line);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build segment path");
    }
    cc_filesystem_stat_t seg_info = {.size = sizeof(seg_info)};
    cc_result_t rc = store->fs.vtable->stat(store->fs.self, seg_path, &seg_info);
    if (rc.code == CC_OK && (seg_info.type != CC_FS_ENTRY_FILE || seg_info.byte_size > SIZE_MAX))
        rc = cc_result_error(CC_ERR_STORAGE, "Current session segment is missing or too large");
    size_t current_size = (size_t)seg_info.byte_size;
    if (rc.code == CC_OK && current_size) {
        unsigned char last = 0;
        size_t got = 0;
        const cc_filesystem_vtable_t *fs = store->fs.vtable;
        if (store->fs.version >= 4U && (store->fs.capabilities & CC_FS_CAP_READ_TAIL) &&
            fs->size >= offsetof(cc_filesystem_vtable_t, read_tail) + sizeof(fs->read_tail) &&
            fs->read_tail) {
            rc = fs->read_tail(store->fs.self, seg_path, &last, 1, &got);
        } else {
            void *data = NULL;
            size_t size = 0;
            rc = fs->read_bytes(store->fs.self, seg_path, &data, &size);
            if (rc.code == CC_OK && size) { last = ((unsigned char *)data)[size - 1U]; got = 1; }
            free(data);
        }
        if (rc.code == CC_OK && (got != 1 || last != '\n'))
            rc = cc_result_error(CC_ERR_STORAGE, "Session history has an incomplete trailing record");
    }
    if (rc.code != CC_OK) { free(line); free(seg_path); return rc; }
    if (store->segment_bytes > 0 && current_size > 0 &&
        (line_len > SIZE_MAX - current_size || current_size + line_len > store->segment_bytes)) {
        if (current == INT_MAX) {
            free(line);
            free(seg_path);
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Too many session history segments");
        }
        free(seg_path);
        current++;
        seg_path = segment_path_for_session_dir(session_dir, current);
        if (!seg_path) {
            free(line);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build next segment path");
        }
        cc_filesystem_stat_t next_info = {.size = sizeof(next_info)};
        rc = store->fs.vtable->stat(store->fs.self, seg_path, &next_info);
        if (rc.code == CC_OK && next_info.type != CC_FS_ENTRY_MISSING &&
            (next_info.type != CC_FS_ENTRY_FILE || next_info.byte_size != 0))
            rc = cc_result_error(CC_ERR_STORAGE, "Unexpected uncommitted session segment");
        if (rc.code == CC_OK && next_info.type == CC_FS_ENTRY_MISSING)
            rc = store->fs.vtable->append_bytes(store->fs.self, seg_path, NULL, 0);
        if (rc.code == CC_OK) rc = store->fs.vtable->sync_file(store->fs.self, seg_path);
        if (rc.code == CC_OK && store->fs.vtable->sync_dir) {
            cc_result_t sync_rc = store->fs.vtable->sync_dir(store->fs.self, session_dir);
            if (sync_rc.code != CC_OK && sync_rc.code != CC_ERR_UNSUPPORTED) rc = sync_rc;
            else cc_result_free(&sync_rc);
        }
        if (rc.code == CC_OK) rc = save_manifest(store, session_dir, session_id, NULL, current);
        if (rc.code != CC_OK) {
            free(line);
            free(seg_path);
            return rc;
        }
    }
    if (!store->fs.vtable || !store->fs.vtable->append_bytes) {
        free(line);
        free(seg_path);
        return cc_result_error(CC_ERR_UNSUPPORTED, "Filesystem cannot append segment records");
    }
    char *framed = malloc(line_len);
    if (!framed) {
        free(line);
        free(seg_path);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to frame segment record");
    }
    memcpy(framed, line, payload_len);
    framed[payload_len] = '\n';
    rc = store->fs.vtable->append_bytes(store->fs.self, seg_path, framed, line_len);
    if (rc.code == CC_OK && store->fs.vtable->sync_file) {
        rc = store->fs.vtable->sync_file(store->fs.self, seg_path);
    }
    free(framed);
    free(line);
    free(seg_path);
    return rc;
}

/** @brief 读取整段并分配各行副本，要求完整换行及合法行长度。
 * @param[in] store 已持锁且提供 read_bytes 的后端借用。
 * @param[in] path 必填分段路径借用。
 * @param[in,out] out_lines 必填独占向量，追加成功行，错误时也由调用者 cleanup。
 * @return 参数、读取、容量、分配或空行/NUL/尾行完整性错误，整段临时缓冲内部释放。
 */
static cc_result_t load_segment_lines(
    cc_json_segmented_store_t *store,
    const char *path,
    string_vec_t *out_lines
)
{
    if (!store || !path || !out_lines || !store->fs.vtable ||
        !store->fs.vtable->read_bytes) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid segment read request");
    }
    void *raw = NULL;
    size_t size = 0;
    cc_result_t rc = store->fs.vtable->read_bytes(store->fs.self, path, &raw, &size);
    if (rc.code != CC_OK) return rc;
    if (size > CC_SEGMENTED_MAX_LINE_READ && memchr(raw, '\n', size) == NULL) {
        free(raw);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "JSONL record exceeds read limit");
    }
    const char *bytes = (const char *)raw;
    if (size > 0 && bytes[size - 1U] != '\n') {
        free(raw);
        return cc_segmented_integrity_error("Session history has an incomplete trailing record");
    }
    size_t start = 0;
    for (size_t i = 0; rc.code == CC_OK && i <= size; i++) {
        if (i < size && bytes[i] != '\n') continue;
        size_t len = i - start;
        if (len > 0 && bytes[start + len - 1] == '\r') len--;
        if (i < size && len == 0) {
            rc = cc_segmented_integrity_error("Session history contains an empty record");
            break;
        }
        if (len > CC_SEGMENTED_MAX_LINE_READ) {
            rc = cc_result_error(CC_ERR_LIMIT_EXCEEDED, "JSONL record exceeds read limit");
            break;
        }
        if (len > 0) {
            if (memchr(bytes + start, '\0', len)) {
                rc = cc_segmented_integrity_error("Session history record contains NUL");
                break;
            }
            char *line = malloc(len + 1);
            if (!line) {
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate segment line");
                break;
            }
            memcpy(line, bytes + start, len);
            line[len] = '\0';
            rc = string_vec_append_take(out_lines, line);
            if (rc.code != CC_OK) free(line);
        }
        start = i + 1;
    }
    free(raw);
    return rc;
}

/** @brief 验证单条审计或将消息解码并追加到反向向量。
 * @param[in] store 已持锁后端借用。
 * @param[in] session_dir 必填目录借用。
 * @param[in] session_id 必填预期会话名借用。
 * @param[in] record 已解析稳定记录借用。
 * @param[in,out] newest 必填独占消息向量，metadata_only 控制是否回填媒体。
 * @return 完整性、解码、回填或分配结果，审计只验证不追加消息。
 */
static cc_result_t append_message_record_object(
    cc_json_segmented_store_t *store,
    const char *session_dir,
    const char *session_id,
    cc_json_value_t *record,
    message_vec_t *newest
)
{
    static const char *const fields[] = {"type", "message", "tool_call", "tool_result",
                                         "schema_version", "length", "checksum", "records_json"};
    if (!cc_storage_json_fields_unique(record, fields, 8) || cc_json_object_get(record, "schema_version") ||
        cc_json_object_get(record, "length") || cc_json_object_get(record, "checksum") ||
        cc_json_object_get(record, "records_json"))
        return cc_segmented_integrity_error("Session record is not a valid object");
    const char *type = cc_json_string_value(cc_json_object_get(record, "type"));
    if (type && (strcmp(type, "tool_call") == 0 || strcmp(type, "tool_result") == 0)) {
        cc_json_value_t *audit = cc_json_object_get(record, type);
        static const char *const audit_fields[] = {
            "id", "session_id", "name", "arguments_json", "status", "tool_call_id",
            "ok", "text", "error", "metadata", "artifacts"
        };
        const char *sid = cc_json_string_value(cc_json_object_get(audit, "session_id"));
        const char *id = cc_json_string_value(cc_json_object_get(audit, "id"));
        const char *other = strcmp(type, "tool_call") == 0 ? "tool_result" : "tool_call";
        if (!cc_storage_json_fields_unique(audit, audit_fields, 11) || !sid || strcmp(sid, session_id) != 0 ||
            !id || !id[0] ||
            cc_json_object_get(record, "message") || cc_json_object_get(record, other))
            return cc_segmented_integrity_error("Stored tool audit record is invalid");
        if (strcmp(type, "tool_call") == 0) {
            const char *name = cc_json_string_value(cc_json_object_get(audit, "name"));
            const char *status = cc_json_string_value(cc_json_object_get(audit, "status"));
            if (!name || !name[0] || !cc_json_is_string(cc_json_object_get(audit, "arguments_json")) ||
                !status || strcmp(status, "completed") != 0)
                return cc_segmented_integrity_error("Stored tool call audit is invalid");
        } else {
            const char *call_id = cc_json_string_value(cc_json_object_get(audit, "tool_call_id"));
            const char *artifacts_json = cc_json_string_value(cc_json_object_get(audit, "artifacts"));
            if (!call_id || strcmp(call_id, id) != 0 || !cc_json_is_bool(cc_json_object_get(audit, "ok")) ||
                !cc_json_is_string(cc_json_object_get(audit, "text")) ||
                !cc_json_is_string(cc_json_object_get(audit, "error")) ||
                !cc_json_is_string(cc_json_object_get(audit, "metadata")) ||
                !artifacts_json || cc_storage_json_has_nul_escape(artifacts_json))
                return cc_segmented_integrity_error("Stored tool result audit is invalid");
            cc_json_value_t *artifacts = NULL;
            cc_result_t rc = cc_json_parse_strict(artifacts_json, &artifacts);
            rc = cc_segmented_classify_json_failure(rc, artifacts_json);
            if (rc.code != CC_OK) return rc;
            if (!cc_json_is_array(artifacts)) {
                cc_json_destroy(artifacts);
                return cc_segmented_integrity_error("Stored tool result artifacts are invalid");
            }
            cc_json_destroy(artifacts);
        }
        return cc_result_ok();
    }
    if (!type || strcmp(type, "message") != 0 || cc_json_object_get(record, "tool_call") ||
        cc_json_object_get(record, "tool_result"))
        return cc_segmented_integrity_error("Session record has an unknown type");
    cc_message_t message;
    memset(&message, 0, sizeof(message));
    cc_result_t rc = message_from_record_object(cc_json_object_get(record, "message"), session_id, &message);
    if (rc.code == CC_ERR_STORAGE) {
        /* 内存解码器的 STORAGE 表示字段损坏，资源和 JSON 失败保持原分类。 */
        cc_result_free(&rc);
        rc = cc_segmented_integrity_error("Stored message fields are invalid");
    }
    if (rc.code == CC_OK && !newest->metadata_only) rc = hydrate_message(store, session_dir, &message);
    if (rc.code == CC_OK) rc = message_vec_append_take(newest, &message);
    cc_message_cleanup(&message);
    return rc;
}

/** @brief 验证单行封装及校验值，按批次反序追加所需消息。
 * @param[in] store 已持锁后端借用。
 * @param[in] session_dir 必填目录借用。
 * @param[in] session_id 必填预期会话名借用。
 * @param[in] line 必填完整 NUL 行借用。
 * @param[in] max_loaded 向量总条数上限，零表示全部。
 * @param[in,out] newest 必填独占向量，失败保留部分消息供上层 cleanup。
 * @return 解析/分配、完整性或消息追加结果，候选树内部释放。
 */
static cc_result_t parse_message_records(
    cc_json_segmented_store_t *store,
    const char *session_dir,
    const char *session_id,
    const char *line,
    size_t max_loaded,
    message_vec_t *newest
)
{
    if (cc_storage_json_has_nul_escape(line))
        return cc_segmented_integrity_error("Session record contains an encoded NUL");
    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse_strict(line, &root);
    rc = cc_segmented_classify_json_failure(rc, line);
    if (rc.code != CC_OK) return rc;
    static const char *const fields[] = {"type", "schema_version", "records_json", "checksum", "length"};
    if (!cc_storage_json_fields_unique(root, fields, 5)) {
        cc_json_destroy(root);
        return cc_segmented_integrity_error("Session record envelope is invalid");
    }
    const char *type = cc_json_string_value(cc_json_object_get(root, "type"));
    if (!type || strcmp(type, "batch") != 0) {
        rc = append_message_record_object(store, session_dir, session_id, root, newest);
        cc_json_destroy(root);
        return rc;
    }
    if (cc_json_object_get(root, "message") || cc_json_object_get(root, "tool_call") ||
        cc_json_object_get(root, "tool_result")) {
        cc_json_destroy(root);
        return cc_segmented_integrity_error("Segment batch contains conflicting records");
    }

    cc_json_value_t *schema = cc_json_object_get(root, "schema_version");
    const char *records_json = cc_json_string_value(cc_json_object_get(root, "records_json"));
    const char *checksum = cc_json_string_value(cc_json_object_get(root, "checksum"));
    cc_json_value_t *length = cc_json_object_get(root, "length");
    double length_value = cc_json_number_value(length);
    size_t records_len = records_json ? strlen(records_json) : 0;
    unsigned long long hash = fnv1a_bytes((const unsigned char *)(records_json ? records_json : ""),
                                          records_len);
    char expected[17];
    snprintf(expected, sizeof(expected), "%016llx", hash);
    if (!cc_json_is_number(schema) || cc_json_number_value(schema) != 2 ||
        !cc_json_is_number(length) || !records_json || cc_storage_json_has_nul_escape(records_json) || !checksum ||
        length_value != (double)records_len || strcmp(checksum, expected) != 0) {
        cc_json_destroy(root);
        return cc_segmented_integrity_error("Segment batch checksum or schema is invalid");
    }

    cc_json_value_t *records = NULL;
    rc = cc_json_parse_strict(records_json, &records);
    rc = cc_segmented_classify_json_failure(rc, records_json);
    if (rc.code != CC_OK || !cc_json_is_array(records)) {
        cc_json_destroy(records);
        cc_json_destroy(root);
        if (rc.code == CC_OK) return cc_segmented_integrity_error("Segment batch records are invalid");
        return rc;
    }
    for (int i = cc_json_array_size(records) - 1;
         rc.code == CC_OK && i >= 0 && (max_loaded == 0 || newest->count < max_loaded);
         i--) {
        rc = append_message_record_object(store, session_dir, session_id,
                                          cc_json_array_get(records, i), newest);
    }
    cc_json_destroy(records);
    cc_json_destroy(root);
    return rc;
}

/** @brief 加载当前位置分段的行快照并定位末行。
 * @param[in,out] cursor 必填独占游标，后端仍存活且其锁已持有，旧 lines 已清理。
 * @return 查询/读取/分配或完整性错误，部分行由游标统一销毁。
 * @note 当前段在 open 时快照，较旧段在到达时读取，物理位置独立于可能重复的消息 ID。
 */
static cc_result_t cursor_load_segment_locked(cc_segmented_cursor_t *cursor)
{
    char *path = segment_path_for_session_dir(cursor->session_dir, cursor->segment);
    if (!path) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate segment path");
    cc_filesystem_stat_t info = {.size = sizeof(info)};
    cc_result_t rc = cursor->store->fs.vtable->stat(cursor->store->fs.self, path, &info);
    if (rc.code == CC_OK && info.type != CC_FS_ENTRY_FILE)
        rc = cc_result_error(CC_ERR_STORAGE, "Session history segment is missing");
    if (rc.code == CC_OK) rc = load_segment_lines(cursor->store, path, &cursor->lines);
    free(path);
    if (rc.code == CC_OK) cursor->next_line = cursor->lines.count;
    return rc;
}

/** @brief 创建反向物理游标并快照最新段。
 * @param[in] handle 必填分段端口借用，覆盖游标寿命，外层排除并发销毁。
 * @param[in] session_id 必填非空会话名借用，游标保存副本。
 * @param[out] out_cursor 必填新槽，先置 NULL，成功拥有游标由调用者 destroy。
 * @return 参数、分配、清单、目录或分段读取错误，失败回收候选，注册和读取持实例锁。
 */
cc_result_t cc_segmented_cursor_open(cc_session_store_t *handle, const char *session_id,
                                     cc_segmented_cursor_t **out_cursor)
{
    if (!out_cursor) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid recovery cursor output");
    *out_cursor = NULL;
    if (!cc_json_segmented_store_is_instance(handle) || !session_id || !session_id[0])
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid recovery cursor request");
    cc_json_segmented_store_t *store = handle->self;
    cc_segmented_cursor_t *cursor = calloc(1, sizeof(*cursor));
    if (!cursor) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate recovery cursor");
    cursor->store = store;
    cursor->session_id = cc_copy_string(session_id);
    cursor->session_dir = session_dir_path(store, session_id);
    if (!cursor->session_id || !cursor->session_dir) {
        cc_segmented_cursor_destroy(cursor);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate recovery cursor path");
    }
    cc_mutex_lock(store->mutex);
    cc_result_t rc = load_current_segment(store, cursor->session_dir, session_id, &cursor->segment);
    if (rc.code == CC_OK) rc = check_segment_directory(store, cursor->session_dir, cursor->segment);
    if (rc.code == CC_OK) rc = cursor_load_segment_locked(cursor);
    if (rc.code == CC_OK) {
        cursor->next = store->cursors;
        store->cursors = cursor;
    }
    cc_mutex_unlock(store->mutex);
    if (rc.code != CC_OK) { cc_segmented_cursor_destroy(cursor); return rc; }
    *out_cursor = cursor;
    return cc_result_ok();
}

/** @brief 取出反向消息页并推进游标。
 * @param[in,out] cursor 必填独占调用游标，next/destroy 由调用者串行化。
 * @param[in] limit 条数上限 1 至 256。
 * @param[out] out_messages 必填新槽，合法入口置 NULL，成功逐项 cleanup 后 free。
 * @param[out] out_count 必填条数槽，合法入口置零，成功零条表示结束。
 * @return 参数、失效、读取/解析或分配结果，读取失败标记永久失效并清理本页。
 * @note 持后端锁，页内由新到旧，后端须覆盖调用。
 */
cc_result_t cc_segmented_cursor_next(cc_segmented_cursor_t *cursor, size_t limit,
                                     cc_message_t **out_messages, size_t *out_count)
{
    if (!cursor || !limit || limit > 256U || !out_messages || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid recovery cursor page request");
    *out_messages = NULL;
    *out_count = 0;
    cc_json_segmented_store_t *store = cursor->store;
    if (!store) return cc_result_error(CC_ERR_INVALID_STATE, "Recovery cursor store is unavailable");
    cc_mutex_lock(store->mutex);
    cc_result_t rc = cursor->invalid ?
        cc_result_error(CC_ERR_STORAGE, "Session changed while traversing recovery history") : cc_result_ok();
    message_vec_t page = {0};
    while (rc.code == CC_OK && page.count < limit && cursor->segment > 0) {
        if (cursor->next_message < cursor->batch.count) {
            rc = message_vec_append_take(&page, &cursor->batch.items[cursor->next_message++]);
            continue;
        }
        message_vec_cleanup(&cursor->batch);
        cursor->next_message = 0;
        if (cursor->next_line > 0) {
            rc = parse_message_records(store, cursor->session_dir, cursor->session_id,
                cursor->lines.items[--cursor->next_line], 0, &cursor->batch);
            continue;
        }
        string_vec_cleanup(&cursor->lines);
        cursor->segment--;
        if (cursor->segment > 0) rc = cursor_load_segment_locked(cursor);
    }
    if (rc.code != CC_OK) cursor->invalid = 1;
    cc_mutex_unlock(store->mutex);
    if (rc.code != CC_OK) { message_vec_cleanup(&page); return rc; }
    *out_messages = page.items;
    *out_count = page.count;
    return cc_result_ok();
}

/** @brief 注销并消费游标及尚未交付的快照。
 * @param[in,out] cursor 可为 NULL 的拥有对象，调用者变量自行失效。
 * @pre next 已退出，存活后端的注销操作持实例锁，已脱离后端的游标可直接清理。
 */
void cc_segmented_cursor_destroy(cc_segmented_cursor_t *cursor)
{
    if (!cursor) return;
    cc_json_segmented_store_t *store = cursor->store;
    if (store) {
        cc_mutex_lock(store->mutex);
        cc_segmented_cursor_t **link = &store->cursors;
        while (*link && *link != cursor) link = &(*link)->next;
        if (*link) *link = cursor->next;
        cc_mutex_unlock(store->mutex);
    }
    string_vec_cleanup(&cursor->lines);
    message_vec_cleanup(&cursor->batch);
    free(cursor->session_id);
    free(cursor->session_dir);
    free(cursor);
}

/** @brief 从最新段逆向收集消息，最终交付正序独立数组。
 * @param[in] self 必填存活实例借用，外层排除销毁。
 * @param[in] session_id 必填会话名借用，目录身份须匹配。
 * @param[in] limit 正数取最新条数，非正数取全部。
 * @param[out] out_messages 必填空槽，输出合法后置 NULL，成功逐项 cleanup 后 free。
 * @param[out] out_count 必填条数槽，输出合法后置零。
 * @param[in] metadata_only 非零跳过媒体回填，消息元数据仍完整复制。
 * @return 参数、清单/分段/消息、媒体或分配错误，失败清理全部部分输出。
 * @note 全程持实例锁，内存同时包含分段行副本、临时解析树和结果向量。
 */
static cc_result_t segmented_load_messages_impl(
    void *self,
    const char *session_id,
    int limit,
    cc_message_t **out_messages,
    size_t *out_count,
    int metadata_only
)
{
    cc_json_segmented_store_t *store = (cc_json_segmented_store_t *)self;
    if (!out_messages || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid load messages output");
    }
    *out_messages = NULL;
    *out_count = 0;
    if (!store || !session_id) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid load messages request");

    cc_mutex_lock(store->mutex);
    char *session_dir = session_dir_path(store, session_id);
    if (!session_dir) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build session directory");
    }
    int current = 0;
    cc_result_t rc = load_current_segment(store, session_dir, session_id, &current);
    if (rc.code == CC_OK) rc = check_segment_directory(store, session_dir, current);
    if (rc.code != CC_OK) {
        free(session_dir);
        cc_mutex_unlock(store->mutex);
        return rc;
    }
    int max_loaded = limit > 0 ? limit : 0;
    message_vec_t newest;
    memset(&newest, 0, sizeof(newest));
    newest.metadata_only = metadata_only;
    rc = cc_result_ok();
    for (int seg = current; rc.code == CC_OK && seg >= 1; seg--) {
        char *path = segment_path_for_session_dir(session_dir, seg);
        if (!path) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build segment path");
            break;
        }
        string_vec_t lines;
        memset(&lines, 0, sizeof(lines));
        cc_filesystem_stat_t info = {.size = sizeof(info)};
        rc = store->fs.vtable->stat(store->fs.self, path, &info);
        if (rc.code == CC_OK && info.type != CC_FS_ENTRY_FILE) {
            cc_result_free(&rc);
            rc = cc_result_error(CC_ERR_STORAGE, "Session history segment is missing");
        }
        if (rc.code == CC_OK) rc = load_segment_lines(store, path, &lines);
        free(path);
        for (size_t idx = lines.count; rc.code == CC_OK && idx > 0; idx--) {
            if (max_loaded > 0 && newest.count >= (size_t)max_loaded) break;
            rc = parse_message_records(store, session_dir, session_id, lines.items[idx - 1],
                                       max_loaded > 0 ? (size_t)max_loaded : 0,
                                       &newest);
        }
        string_vec_cleanup(&lines);
        if (max_loaded > 0 && newest.count >= (size_t)max_loaded) break;
    }
    free(session_dir);
    cc_mutex_unlock(store->mutex);
    if (rc.code != CC_OK) {
        message_vec_cleanup(&newest);
        return rc;
    }
    for (size_t i = 0; i < newest.count / 2; i++) {
        cc_message_t tmp = newest.items[i];
        newest.items[i] = newest.items[newest.count - 1 - i];
        newest.items[newest.count - 1 - i] = tmp;
    }
    *out_messages = newest.items;
    *out_count = newest.count;
    return cc_result_ok();
}

/** @brief 通过普通路径读取最新消息并回填可用媒体。
 * @param[in] self 存活后端借用。
 * @param[in] session_id 会话名借用。
 * @param[in] limit 正数限制条数，非正数全部。
 * @param[out] out_messages 必填新槽，成功逐项 cleanup 后 free。
 * @param[out] out_count 必填条数槽。
 * @return 公共实现原结果，锁及失败清理沿公共实现。
 */
static cc_result_t segmented_load_messages(
    void *self, const char *session_id, int limit, cc_message_t **out_messages, size_t *out_count)
{
    return segmented_load_messages_impl(self, session_id, limit, out_messages, out_count, 0);
}

/** @brief 读取最新消息元数据，延后媒体字节回填。
 * @param[in] self 存活后端借用。
 * @param[in] session_id 会话名借用。
 * @param[in] limit 正数限制条数，非正数全部。
 * @param[out] out_messages 必填新槽，成功逐项 cleanup 后 free。
 * @param[out] out_count 必填条数槽。
 * @return 公共实现原结果，锁及失败清理沿公共实现。
 */
static cc_result_t segmented_load_message_metadata(
    void *self, const char *session_id, int limit, cc_message_t **out_messages, size_t *out_count)
{
    return segmented_load_messages_impl(self, session_id, limit, out_messages, out_count, 1);
}

/** @brief 消费会话字段并清零。
 * @param[in,out] session 可为 NULL 的独占结构，结构本身仍归调用者。
 * @pre 全部读者已退出，空结构允许重复清理。
 */
static void session_cleanup_fields(cc_session_t *session)
{
    if (!session) return;
    free(session->id);
    free(session->name);
    free(session->workspace_dir);
    free(session->model);
    free(session->created_at);
    free(session->updated_at);
    memset(session, 0, sizeof(*session));
}

/** @brief 将清单状态转换为会话枚举。
 * @param[in] status 可为 NULL 的状态文本借用。
 * @return completed/error 对应状态，其他兼容 ACTIVE，无分配。
 */
static cc_session_status_t session_status_from_manifest(const char *status)
{
    if (status && strcmp(status, "completed") == 0) return CC_SESSION_COMPLETED;
    if (status && strcmp(status, "error") == 0) return CC_SESSION_ERROR;
    return CC_SESSION_ACTIVE;
}

/** @brief 严格读取清单、核对路径身份并复制会话到列表。
 * @param[in] store 调用者已持锁的后端借用。
 * @param[in] manifest_path 必填清单路径借用。
 * @param[in,out] sessions 必填拥有数组槽，成功可扩容，失败仍由上层清理。
 * @param[in,out] count 必填已完成条数槽，仅完整复制后增加。
 * @param[in,out] capacity 必填数组容量槽，扩容成功即更新。
 * @return 严格读取、路径/重复身份、容量或分配错误，失败清理当前未计数项。
 */
static cc_result_t append_session_from_manifest(
    cc_json_segmented_store_t *store,
    const char *manifest_path,
    cc_session_t **sessions,
    size_t *count,
    size_t *capacity
)
{
    cc_json_value_t *root = NULL;
    cc_result_t rc = parse_json_file_strict(store, manifest_path, &root);
    if (rc.code != CC_OK) return rc;
    const char *manifest_id = cc_json_string_value(cc_json_object_get(root, "id"));
    if (!cc_json_is_object(root) || !manifest_id || !manifest_id[0]) {
        cc_json_destroy(root);
        return cc_segmented_integrity_error("Session manifest has no valid id");
    }
    char *expected_dir = session_dir_path(store, manifest_id);
    char *expected_manifest = expected_dir ? manifest_path_for_session_dir(expected_dir) : NULL;
    free(expected_dir);
    if (!expected_manifest) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot validate session manifest path");
    }
    int matches_path = strcmp(expected_manifest, manifest_path) == 0;
    free(expected_manifest);
    if (!matches_path) {
        cc_json_destroy(root);
        return cc_segmented_integrity_error("Session manifest id does not match its directory");
    }
    for (size_t i=0; i<*count; i++) {
        if ((*sessions)[i].id && strcmp((*sessions)[i].id, manifest_id) == 0) {
            cc_json_destroy(root);
            return cc_segmented_integrity_error("Duplicate session manifest id");
        }
    }
    if (*count == *capacity) {
        if (*capacity > SIZE_MAX / sizeof(**sessions) / 2U) {
            cc_json_destroy(root);
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Session list capacity overflow");
        }
        size_t next_cap = *capacity ? *capacity * 2 : 8;
        cc_session_t *next = realloc(*sessions, next_cap * sizeof(cc_session_t));
        if (!next) {
            cc_json_destroy(root);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow session list");
        }
        memset(next + *capacity, 0, (next_cap - *capacity) * sizeof(cc_session_t));
        *sessions = next;
        *capacity = next_cap;
    }
    cc_session_t *out = &(*sessions)[*count];
    const char *id = cc_json_string_value(cc_json_object_get(root, "id"));
    const char *ws = cc_json_string_value(cc_json_object_get(root, "workspace_dir"));
    const char *status = cc_json_string_value(cc_json_object_get(root, "status"));
    const char *name = cc_json_string_value(cc_json_object_get(root, "name"));
    const char *model = cc_json_string_value(cc_json_object_get(root, "model"));
    const char *created = cc_json_string_value(cc_json_object_get(root, "created_at"));
    const char *updated = cc_json_string_value(cc_json_object_get(root, "updated_at"));
    out->id = id ? cc_copy_string(id) : cc_copy_string("");
    out->workspace_dir = ws ? cc_copy_string(ws) : cc_copy_string("");
    out->name = cc_copy_string(name ? name : (id ? id : ""));
    out->model = model ? cc_copy_string(model) : NULL;
    out->created_at = created ? cc_copy_string(created) : NULL;
    out->updated_at = updated ? cc_copy_string(updated) : NULL;
    out->status = session_status_from_manifest(status);
    if (!out->id || !out->workspace_dir || !out->name || (model && !out->model) ||
        (created && !out->created_at) || (updated && !out->updated_at)) {
        cc_json_destroy(root);
        session_cleanup_fields(out);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy session manifest");
    }
    cc_json_destroy(root);
    (*count)++;
    return cc_result_ok();
}

/** @brief 列举根下各会话目录的清单路径。
 * @param[in] store 已持锁且提供 list/stat 的后端借用。
 * @param[in] root_path 必填根路径借用。
 * @param[in,out] paths 必填独占向量，接管成功路径，失败部分项由上层 cleanup。
 * @return 参数、枚举/查询、缺清单或分配错误，列名及未转移路径内部释放。
 */
static cc_result_t list_manifest_paths(
    cc_json_segmented_store_t *store,
    const char *root_path,
    string_vec_t *paths
)
{
    if (!store || !root_path || !paths || !store->fs.vtable || !store->fs.vtable->list_dir) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid manifest scan request");
    }
    char **items = NULL;
    size_t count = 0;
    cc_result_t rc = store->fs.vtable->list_dir(store->fs.self, root_path, &items, &count);
    if (rc.code != CC_OK) return rc;
    for (size_t i = 0; rc.code == CC_OK && i < count; i++) {
        if (!items[i] || strcmp(items[i], ".") == 0 || strcmp(items[i], "..") == 0) continue;
        char *session_dir = join_path2(root_path, items[i]);
        if (!session_dir) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build session path");
            break;
        }
        cc_filesystem_stat_t info = {.size = sizeof(info)};
        rc = store->fs.vtable->stat(store->fs.self, session_dir, &info);
        if (rc.code != CC_OK) {
            free(session_dir);
            break;
        }
        if (info.type != CC_FS_ENTRY_DIRECTORY) {
            free(session_dir);
            continue;
        }
        char *manifest = manifest_path_for_session_dir(session_dir);
        free(session_dir);
        if (!manifest) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build manifest path");
            break;
        }
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        rc = store->fs.vtable->stat(store->fs.self, manifest, &info);
        if (rc.code == CC_OK && info.type != CC_FS_ENTRY_FILE) {
            cc_result_free(&rc);
            rc = cc_result_error(CC_ERR_STORAGE, "Session directory has no readable manifest");
        }
        if (rc.code == CC_OK) rc = string_vec_append_take(paths, manifest);
        if (rc.code != CC_OK) free(manifest);
    }
    for (size_t i = 0; i < count; i++) free(items[i]);
    free(items);
    return rc;
}

/** @brief 在实例锁内列举并复制全部会话元数据。
 * @param[in] self 必填存活后端借用，禁止并发销毁。
 * @param[out] out_sessions 必填空槽，输出合法后置 NULL，成功由 cc_session_free_array 释放。
 * @param[out] out_count 必填条数槽，输出合法后置零。
 * @return 参数、清单读取/身份/结构或分配错误，失败释放部分列表与路径向量。
 */
static cc_result_t segmented_list_sessions(void *self, cc_session_t **out_sessions, size_t *out_count)
{
    cc_json_segmented_store_t *store = (cc_json_segmented_store_t *)self;
    if (!out_sessions || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid list sessions output");
    }
    *out_sessions = NULL;
    *out_count = 0;
    if (!store) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid list sessions request");
    }
    cc_mutex_lock(store->mutex);
    string_vec_t manifests;
    memset(&manifests, 0, sizeof(manifests));
    cc_result_t rc = list_manifest_paths(store, store->root_path, &manifests);
    cc_session_t *sessions = NULL;
    size_t count = 0;
    size_t capacity = 0;
    for (size_t i = 0; rc.code == CC_OK && i < manifests.count; i++) {
        rc = append_session_from_manifest(store, manifests.items[i], &sessions, &count, &capacity);
    }
    string_vec_cleanup(&manifests);
    cc_mutex_unlock(store->mutex);
    if (rc.code != CC_OK) {
        for (size_t i = 0; i < count; i++) session_cleanup_fields(&sessions[i]);
        free(sessions);
        return rc;
    }
    *out_sessions = sessions;
    *out_count = count;
    return cc_result_ok();
}

/** @brief 递归删除指定目录及内容，失败保留未完成部分。
 * @param[in] store 已持锁且提供 exists/list/remove 的后端借用。
 * @param[in] path 必填待删除路径借用，由调用者保证指向目标会话目录。
 * @return 查询、枚举、路径分配或删除错误，缺失路径成功，临时列名内部释放。
 * @note 深度随媒体目录增长，外层排除链接改装和跨实例目录操作。
 */
static cc_result_t remove_tree(
    cc_json_segmented_store_t *store,
    const char *path
)
{
    if (!store || !path || !store->fs.vtable || !store->fs.vtable->list_dir ||
        !store->fs.vtable->remove) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid recursive remove request");
    }
    int exists = 0;
    cc_result_t rc = store->fs.vtable->exists(store->fs.self, path, &exists);
    if (rc.code != CC_OK || !exists) return rc;
    if (path_is_dir(store, path)) {
        char **items = NULL;
        size_t count = 0;
        rc = store->fs.vtable->list_dir(store->fs.self, path, &items, &count);
        for (size_t i = 0; rc.code == CC_OK && i < count; i++) {
            if (!items[i] || strcmp(items[i], ".") == 0 || strcmp(items[i], "..") == 0) continue;
            char *child = join_path2(path, items[i]);
            if (!child) {
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build removal path");
                break;
            }
            rc = remove_tree(store, child);
            free(child);
        }
        for (size_t i = 0; i < count; i++) free(items[i]);
        free(items);
    }
    if (rc.code == CC_OK) rc = store->fs.vtable->remove(store->fs.self, path);
    return rc;
}

/** @brief SDK 显式重置会话目录和全部媒体，重建空工作目录清单。
 * @param[in] self 必填存活实例借用。
 * @param[in] session_id 必填会话名借用，调用方确认所清会话并排空使用者。
 * @return 参数、删除、创建或保存结果，失败可能已删除部分历史，全部操作持实例锁。
 * @note 同会话游标先失效，此接口清持久历史，产品终端 clear 使用独立显示流程。
 */
static cc_result_t segmented_clear_session(void *self, const char *session_id)
{
    cc_json_segmented_store_t *store = (cc_json_segmented_store_t *)self;
    if (!store || !session_id) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid clear session request");
    cc_mutex_lock(store->mutex);
    invalidate_cursors_locked(store, session_id);
    char *session_dir = session_dir_path(store, session_id);
    if (!session_dir) {
        cc_mutex_unlock(store->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build session directory");
    }
    cc_result_t rc = remove_tree(store, session_dir);
    if (rc.code == CC_OK) rc = ensure_dir_recursive(store, session_dir);
    if (rc.code == CC_OK) rc = ensure_media_dirs(store, session_dir);
    if (rc.code == CC_OK) {
        char *seg = segment_path_for_session_dir(session_dir, 1);
        if (!seg) rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build segment path");
        else {
            rc = store->fs.vtable->append_bytes
                ? store->fs.vtable->append_bytes(store->fs.self, seg, NULL, 0)
                : cc_result_error(CC_ERR_UNSUPPORTED, "Filesystem cannot recreate segment");
            free(seg);
        }
    }
    if (rc.code == CC_OK) rc = save_manifest(store, session_dir, session_id, "", 1);
    free(session_dir);
    cc_mutex_unlock(store->mutex);
    return rc;
}

/** @brief 消费后端资源并解除注册游标对后端的借用。
 * @param[in,out] self 可为 NULL 的拥有实例，调用者变量自行失效。
 * @pre 调用入口已关闭且全部操作退出，存活游标需由外层最终 destroy。
 * @note 有锁时锁内标记游标失效并清其 store，随后释放路径、锁及 filesystem。
 */
static void segmented_destroy(void *self)
{
    cc_json_segmented_store_t *store = (cc_json_segmented_store_t *)self;
    if (!store) return;
    if (store->mutex) cc_mutex_lock(store->mutex);
    for (cc_segmented_cursor_t *cursor = store->cursors; cursor; cursor = cursor->next) {
        cursor->invalid = 1;
        cursor->store = NULL;
    }
    store->cursors = NULL;
    free(store->root_path);
    free(store->media_dir);
    if (store->mutex) {
        cc_mutex_unlock(store->mutex);
        cc_mutex_destroy(store->mutex);
    }
    if (store->fs.vtable && store->fs.vtable->destroy) {
        store->fs.vtable->destroy(store->fs.self);
    }
    memset(&store->fs, 0, sizeof(store->fs));
    free(store);
}

/** @brief 优先借用记录外层会话名，否则取消息内层会话名。
 * @param[in] record 可为 NULL 的稳定记录借用。
 * @return 调用期会话名借用或 NULL，无分配及所有权转移。
 */
static const char *session_record_session_id(const cc_session_record_t *record)
{
    if (!record) return NULL;
    if (record->session_id && record->session_id[0]) return record->session_id;
    if (record->type == CC_SESSION_RECORD_MESSAGE && record->data.message) {
        return record->data.message->session_id;
    }
    return NULL;
}

/** @brief 深复制单条消息或审计，外化媒体后构造完整候选记录。
 * @param[in] store 已持锁后端借用，外部同目录写者已排除。
 * @param[in] session_dir 必填已准备目录借用。
 * @param[in] session_id 必填统一会话名借用。
 * @param[in] input 必填稳定记录借用，消息/工具数据仍归调用者。
 * @param[out] out_record 必填空槽，合法入口置 NULL，成功由调用者 destroy 或挂接。
 * @return 参数、复制、媒体外化或 JSON 分配错误，失败清理内存候选但可能留媒体文件。
 */
static cc_result_t build_batch_record(
    cc_json_segmented_store_t *store,
    const char *session_dir,
    const char *session_id,
    const cc_session_record_t *input,
    cc_json_value_t **out_record
)
{
    if (!store || !session_dir || !session_id || !input || !out_record) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid segmented batch record");
    }
    *out_record = NULL;
    cc_result_t rc = cc_result_ok();
    cc_json_value_t *record = cc_json_create_object();
    if (!record) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate batch record");

    if (input->type == CC_SESSION_RECORD_MESSAGE) {
        if (!input->data.message) {
            rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Batch message is null");
        }
        cc_message_t copy;
        memset(&copy, 0, sizeof(copy));
        if (rc.code == CC_OK) rc = cc_message_copy(input->data.message, &copy);
        const char *source = role_source(copy.role);
        const char *record_id = copy.id ? copy.id : "message";
        for (size_t i = 0; rc.code == CC_OK && i < copy.content.count; i++) {
            cc_content_part_t *part = &copy.content.items[i];
            if (part->kind != CC_MEDIA_TEXT) {
                rc = externalize_artifact(store, session_dir, source, record_id, i,
                                          &part->artifact);
            }
        }
        cc_json_value_t *message = NULL;
        if (rc.code == CC_OK) rc = message_to_record_object(&copy, &message);
        if (rc.code == CC_OK) {
            if (!json_set_string_if_present(record, "type", "message")) {
                cc_json_destroy(message);
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode message record type");
            } else if (!cc_json_object_set_owned(record, "message", message)) {
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot install message record");
            }
        } else {
            cc_json_destroy(message);
        }
        cc_message_cleanup(&copy);
    } else if (input->type == CC_SESSION_RECORD_TOOL_CALL) {
        const cc_tool_call_t *call = input->data.tool_call;
        if (!call) {
            rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Batch tool call is null");
        } else {
            cc_json_value_t *tool_call = cc_json_create_object();
            if (!tool_call) {
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate tool call record");
            } else {
                if (!json_set_string_if_present(record, "type", "tool_call") ||
                    !json_set_string_if_present(tool_call, "id", call->id ? call->id : "") ||
                    !json_set_string_if_present(tool_call, "session_id", session_id) ||
                    !json_set_string_if_present(tool_call, "name", call->name ? call->name : "") ||
                    !json_set_string_if_present(tool_call, "arguments_json", call->arguments_json ? call->arguments_json : "") ||
                    !json_set_string_if_present(tool_call, "status", "completed")) {
                    cc_json_destroy(tool_call);
                    rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode tool call audit");
                } else if (!cc_json_object_set_owned(record, "tool_call", tool_call)) {
                    rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot install tool call audit");
                }
            }
        }
    } else if (input->type == CC_SESSION_RECORD_TOOL_RESULT) {
        const char *tool_call_id = input->data.tool_result.tool_call_id;
        const cc_tool_result_t *result = input->data.tool_result.result;
        cc_media_artifact_list_t artifacts;
        cc_media_artifact_list_init(&artifacts);
        if (result) rc = cc_media_artifact_list_copy(&result->artifacts, &artifacts);
        for (size_t i = 0; rc.code == CC_OK && i < artifacts.count; i++) {
            rc = externalize_artifact(store, session_dir, "tool",
                                      tool_call_id ? tool_call_id : "tool_result", i,
                                      &artifacts.items[i]);
        }
        char *artifacts_json = NULL;
        if (rc.code == CC_OK) rc = cc_media_artifact_list_to_json(&artifacts, &artifacts_json);
        cc_json_value_t *tool_result = NULL;
        if (rc.code == CC_OK) {
            tool_result = cc_json_create_object();
            if (!tool_result) rc = cc_result_error(CC_ERR_OUT_OF_MEMORY,
                                                   "Failed to allocate tool result record");
        }
        if (rc.code == CC_OK) {
            if (!json_set_string_if_present(record, "type", "tool_result") ||
                !json_set_string_if_present(tool_result, "id", tool_call_id ? tool_call_id : "") ||
                !json_set_string_if_present(tool_result, "session_id", session_id) ||
                !json_set_string_if_present(tool_result, "tool_call_id", tool_call_id ? tool_call_id : "") ||
                !cc_json_object_set_owned(tool_result, "ok", cc_json_create_bool(result && result->ok)) ||
                !json_set_string_if_present(tool_result, "text", result && result->text ? result->text : "") ||
                !json_set_string_if_present(tool_result, "error", result && result->error ? result->error : "") ||
                !json_set_string_if_present(tool_result, "metadata", result && result->metadata ? result->metadata : "") ||
                !json_set_string_if_present(tool_result, "artifacts", artifacts_json ? artifacts_json : "[]")) {
                cc_json_destroy(tool_result);
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode tool result audit");
            } else if (!cc_json_object_set_owned(record, "tool_result", tool_result)) {
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot install tool result audit");
            }
        } else {
            cc_json_destroy(tool_result);
        }
        free(artifacts_json);
        cc_media_artifact_list_cleanup(&artifacts);
    } else {
        rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Unknown session record type");
    }

    if (rc.code != CC_OK) {
        cc_json_destroy(record);
        return rc;
    }
    *out_record = record;
    return cc_result_ok();
}

/** @brief 将同会话批次编码为一条带长度与校验值的 JSONL 记录并同步。
 * @param[in] self 必填存活后端借用，外层排除销毁及同路径外部写者。
 * @param[in] records 调用期稳定记录数组借用，零条时可为 NULL，全部记录归原 owner。
 * @param[in] count 条数，零条直接成功。
 * @return 参数/身份/数组预算、准备、媒体、编码或追加结果，错误可能已有媒体/清单/行前缀。
 * @note 参数预检后持实例锁并使同会话游标失效，候选 JSON/文本全部内部释放。
 */
static cc_result_t segmented_append_records(
    void *self,
    const cc_session_record_t *records,
    size_t count
)
{
    cc_json_segmented_store_t *store = (cc_json_segmented_store_t *)self;
    if (!store || (!records && count > 0)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid segmented store batch");
    }
    if (count == 0) return cc_result_ok();
    const char *session_id = session_record_session_id(&records[0]);
    if (!session_id || !session_id[0]) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Segmented batch has no session id");
    }
    for (size_t i = 0; i < count; i++) {
        const char *candidate = session_record_session_id(&records[i]);
        if (!candidate || strcmp(candidate, session_id) != 0) {
            return cc_result_error(CC_ERR_INVALID_ARGUMENT,
                                   "A segmented batch cannot span multiple sessions");
        }
        if (records[i].type == CC_SESSION_RECORD_MESSAGE) {
            const cc_message_t *message = records[i].data.message;
            if (!message || !message->id || !message->id[0] || !message->session_id ||
                strcmp(message->session_id, session_id) != 0 ||
                strcmp(cc_message_role_string(message->role), "unknown") == 0 ||
                !cc_message_tool_outcome_valid(message))
                return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Batch message identity or role is invalid");
            if (message->content.count > CC_JSON_MAX_MESSAGE_PARTS ||
                message->tool_calls.count > CC_JSON_MAX_MESSAGE_PARTS)
                return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Batch message array is too large");
            for (size_t j = 0; j < message->tool_calls.count; j++) {
                const cc_tool_call_t *call = &message->tool_calls.items[j];
                if (!call->id || !call->id[0] || !call->name || !call->name[0])
                    return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Batch message has an invalid tool call");
                for (size_t k = 0; k < j; k++) {
                    if (strcmp(message->tool_calls.items[k].id, call->id) == 0)
                        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Batch message repeats a tool call id");
                }
            }
        } else if (records[i].type == CC_SESSION_RECORD_TOOL_CALL) {
            const cc_tool_call_t *call = records[i].data.tool_call;
            if (!call || !call->id || !call->id[0] || !call->name || !call->name[0])
                return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Batch tool call audit is invalid");
        } else if (records[i].type == CC_SESSION_RECORD_TOOL_RESULT) {
            const char *call_id = records[i].data.tool_result.tool_call_id;
            if (!call_id || !call_id[0] || !records[i].data.tool_result.result)
                return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Batch tool result audit is invalid");
        } else {
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Unknown session record type");
        }
    }

    cc_mutex_lock(store->mutex);
    invalidate_cursors_locked(store, session_id);
    char *session_dir = session_dir_path(store, session_id);
    cc_result_t rc = session_dir ? prepare_session_locked(store, session_dir, session_id, NULL)
                                 : cc_result_error(CC_ERR_OUT_OF_MEMORY,
                                                   "Failed to build session directory");

    cc_json_value_t *record_array = NULL;
    if (rc.code == CC_OK) {
        record_array = cc_json_create_array();
        if (!record_array) rc = cc_result_error(CC_ERR_OUT_OF_MEMORY,
                                                "Failed to allocate segmented batch");
    }
    for (size_t i = 0; rc.code == CC_OK && i < count; i++) {
        cc_json_value_t *record = NULL;
        rc = build_batch_record(store, session_dir, session_id, &records[i], &record);
        if (rc.code == CC_OK) {
            if (!cc_json_array_append_owned(record_array, record))
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot append batch record");
        }
        else cc_json_destroy(record);
    }

    char *records_json = NULL;
    if (rc.code == CC_OK) {
        records_json = cc_json_stringify_unformatted(record_array);
        if (!records_json) rc = cc_result_error(CC_ERR_OUT_OF_MEMORY,
                                                "Failed to serialize segmented batch records");
    }
    cc_json_value_t *batch = NULL;
    if (rc.code == CC_OK) {
        size_t records_len = strlen(records_json);
        unsigned long long hash = fnv1a_bytes((const unsigned char *)records_json, records_len);
        char checksum[17];
        snprintf(checksum, sizeof(checksum), "%016llx", hash);
        batch = cc_json_create_object();
        if (!batch) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate segmented batch envelope");
        } else {
            if (!json_set_string_if_present(batch, "type", "batch") ||
                !cc_json_object_set_owned(batch, "schema_version", cc_json_create_number(2)) ||
                !cc_json_object_set_owned(batch, "length", cc_json_create_number((double)records_len)) ||
                !json_set_string_if_present(batch, "checksum", checksum) ||
                !json_set_string_if_present(batch, "records_json", records_json))
                rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot encode batch envelope");
            else rc = append_record_line(store, session_dir, session_id, batch);
        }
    }
    cc_json_destroy(batch);
    cc_json_destroy(record_array);
    free(records_json);
    free(session_dir);
    cc_mutex_unlock(store->mutex);
    return rc;
}

/** @brief 将较旧的媒体部分替换为预算省略说明。
 * @param[in,out] part 必填独占内容部分，成功消费全部原字段，保留方向。
 * @return 成功或 OOM，失败保留原部分，持消息 owner 或后端锁。
 */
static cc_result_t describe_omitted_media(cc_content_part_t *part)
{
    char description[320];
    snprintf(description, sizeof(description),
        "[%s attachment %.96s (%zu bytes, %.64s) omitted from inline context "
        "to fit the memory budget. The original remains in session history.]",
        cc_media_kind_string(part->kind), part->artifact.id ? part->artifact.id : "",
        part->artifact.bytes, part->artifact.mime ? part->artifact.mime : "unknown MIME");
    char *text = cc_copy_string(description);
    if (!text) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot describe omitted media");
    cc_content_part_direction_t direction = part->direction;
    cc_content_part_cleanup(part);
    part->kind = CC_MEDIA_TEXT;
    part->direction = direction;
    part->text = text;
    return cc_result_ok();
}

/** @brief 按剩余编码预算回填一项，较旧超额项改为文本说明。
 * @param[in] store 已持实例锁的后端借用。
 * @param[in] session_dir 必填目录借用。
 * @param[in,out] part 必填独占部分，成功可接管 base64 或替换为文本。
 * @param[in,out] remaining 必填剩余编码字节预算，成功回填后扣除实际长度。
 * @param[in] required 非零表示当前用户必需项，超额返回 LIMIT 并保留部分。
 * @return 成功、路径/资源错误或必需项预算错误，普通不可读媒体保留元数据。
 */
static cc_result_t hydrate_context_part(cc_json_segmented_store_t *store,
    const char *session_dir, cc_content_part_t *part, size_t *remaining, int required)
{
    if (part->kind == CC_MEDIA_TEXT) return cc_result_ok();
    cc_media_artifact_t *artifact = &part->artifact;
    size_t encoded_size = artifact->data_base64 ? strlen(artifact->data_base64) : 0;
    if (!artifact->data_base64 && artifact->path && path_starts_with(artifact->path, store->media_dir)) {
        if (!media_relative_path_safe(artifact->path))
            return cc_result_error(CC_ERR_PERMISSION_DENIED, "Unsafe stored media path");
        char *path = join_path2(session_dir, artifact->path);
        if (!path) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate media path");
        size_t bytes = 0;
        cc_result_t size_rc = media_file_size(store, path, &bytes);
        free(path);
        if (size_rc.code != CC_OK) {
            if (size_rc.code == CC_ERR_OUT_OF_MEMORY) return size_rc;
            if (size_rc.code == CC_ERR_LIMIT_EXCEEDED) {
                if (required) return size_rc;
                cc_result_free(&size_rc);
                return describe_omitted_media(part);
            }
            /* 普通媒体不可读时保留原元数据，避免错误大小绕过预算。 */
            cc_result_free(&size_rc);
            return cc_result_ok();
        }
        if (bytes > (SIZE_MAX / 4U) * 3U)
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Stored media size overflows Base64 encoding");
        encoded_size = ((bytes + 2U) / 3U) * 4U;
    }
    if (encoded_size > *remaining ||
        (store->max_base64_bytes && encoded_size > store->max_base64_bytes)) {
        if (required)
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED,
                "Current user attachments exceed the inline media limits; reduce attachments or increase the limits");
        return describe_omitted_media(part);
    }
    cc_result_t rc = hydrate_artifact(store, session_dir, artifact);
    if (rc.code == CC_ERR_LIMIT_EXCEEDED && !required) {
        cc_result_free(&rc);
        return describe_omitted_media(part);
    }
    if (rc.code != CC_OK) return rc;
    encoded_size = artifact->data_base64 ? strlen(artifact->data_base64) : 0;
    if (encoded_size > *remaining ||
        (store->max_base64_bytes && encoded_size > store->max_base64_bytes)) {
        if (required)
            return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Attachment size changed beyond the inline media budget");
        return describe_omitted_media(part);
    }
    *remaining -= encoded_size;
    return cc_result_ok();
}

/** @brief 先回填当前用户附件，再从新到旧分配其他附件预算。
 * @param[in] self 必填存活后端借用，外层排除销毁。
 * @param[in] session_id 必填会话名借用。
 * @param[in,out] messages 独占消息数组，零条可为 NULL，失败保留部分回填结果。
 * @param[in] count 消息条数。
 * @param[in] required_message 当前用户索引，等于 count 表示无必需消息。
 * @param[in] max_base64_bytes 总编码字节预算，零表示不限制。
 * @return 参数、路径、回填或预算结果，实例锁覆盖修改，临时路径自行释放。
 */
static cc_result_t segmented_hydrate_context_media(void *self, const char *session_id,
    cc_message_t *messages, size_t count, size_t required_message, size_t max_base64_bytes)
{
    cc_json_segmented_store_t *store = self;
    if (!store || !session_id || (!messages && count) || required_message > count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid context media request");
    char *session_dir = session_dir_path(store, session_id);
    if (!session_dir) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate context media path");
    size_t remaining = max_base64_bytes ? max_base64_bytes : SIZE_MAX;
    cc_result_t rc = cc_result_ok();
    cc_mutex_lock(store->mutex);
    /* 优先当前用户输入，剩余预算分配最近观察，包含本轮工具输出。 */
    if (required_message < count) {
        for (size_t j = 0; rc.code == CC_OK && j < messages[required_message].content.count; j++)
            rc = hydrate_context_part(store, session_dir,
                &messages[required_message].content.items[j], &remaining, 1);
    }
    for (size_t i = count; rc.code == CC_OK && i > 0; i--) {
        if (i - 1U == required_message) continue;
        for (size_t j = messages[i - 1U].content.count; rc.code == CC_OK && j > 0; j--)
            rc = hydrate_context_part(store, session_dir, &messages[i - 1U].content.items[j - 1U], &remaining, 0);
    }
    cc_mutex_unlock(store->mutex);
    free(session_dir);
    return rc;
}

static cc_session_store_vtable_t segmented_vtable = {
    segmented_create_session,
    segmented_append_records,
    segmented_load_messages,
    segmented_list_sessions,
    segmented_clear_session,
    segmented_destroy,
    segmented_load_message_metadata,
    segmented_hydrate_context_media
};

/** @brief 按 self 与固定虚表身份识别分段端口。
 * @param[in] store 可为 NULL 的稳定端口借用，禁止并发改装或销毁。
 * @return 身份匹配为一，否则零，无磁盘访问及资源交接。
 */
int cc_json_segmented_store_is_instance(const cc_session_store_t *store)
{
    return store && store->self && store->vtable == &segmented_vtable;
}

/** @brief 精确比较实例根路径原文。
 * @param[in] store 可为 NULL 的稳定端口借用。
 * @param[in] path 可为 NULL 的路径借用，调用期间保持稳定。
 * @return 类型与路径字节均匹配为一，否则零，禁止并发销毁。
 */
int cc_json_segmented_store_matches_path(const cc_session_store_t *store, const char *path)
{
    if (!cc_json_segmented_store_is_instance(store) || !path) return 0;
    const cc_json_segmented_store_t *segmented=store->self;
    return segmented->root_path && strcmp(segmented->root_path,path)==0;
}

/** @brief 按唯一消息锚点返回旧到新历史页。
 * @param[in] handle 必填存活分段端口借用，禁止并发销毁。
 * @param[in] session_id 必填非空会话名借用。
 * @param[in] before_message_id 可为 NULL，NULL 取最新页，非空时排除锚点并扫描核对唯一性。
 * @param[in] limit 页条数，1 至 256。
 * @param[out] out_messages 必填新槽，合法入口置 NULL，成功逐项 cleanup 后 free。
 * @param[out] out_count 必填条数槽，合法入口置零。
 * @param[out] out_has_more 必填更早消息标志，合法入口置零。
 * @return 参数、锚点缺失/重复或游标结果，失败清理全部输出，同会话写入使游标失效。
 */
cc_result_t cc_json_segmented_store_load_page(
    cc_session_store_t *handle, const char *session_id, const char *before_message_id,
    size_t limit, cc_message_t **out_messages, size_t *out_count, int *out_has_more)
{
    if (!handle || handle->vtable != &segmented_vtable || !handle->self || !session_id ||
        !session_id[0] || (before_message_id && !before_message_id[0]) ||
        !limit || limit > 256U || !out_messages || !out_count || !out_has_more)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid segmented history page request");
    *out_messages = NULL;
    *out_count = 0;
    *out_has_more = 0;
    cc_segmented_cursor_t *cursor = NULL;
    cc_result_t rc = cc_segmented_cursor_open(handle, session_id, &cursor);
    message_vec_t newest = {0};
    int anchor_found = before_message_id == NULL;
    size_t count = 1;
    while (rc.code == CC_OK && count && (before_message_id || newest.count <= limit)) {
        cc_message_t *messages = NULL;
        count = 0;
        rc = cc_segmented_cursor_next(cursor, 32U, &messages, &count);
        for (size_t i = 0; rc.code == CC_OK && i < count; i++) {
            cc_message_t *message = &messages[i];
            if (before_message_id && message->id && strcmp(message->id, before_message_id) == 0) {
                if (anchor_found) rc = cc_result_error(CC_ERR_STORAGE, "History page anchor is ambiguous");
                else anchor_found = 1;
                continue;
            }
            if (anchor_found && newest.count <= limit)
                rc = message_vec_append_take(&newest, message);
        }
        for (size_t i = 0; i < count; i++) cc_message_cleanup(&messages[i]);
        free(messages);
    }
    cc_segmented_cursor_destroy(cursor);
    if (rc.code == CC_OK && !anchor_found) {
        cc_result_free(&rc);
        rc = cc_result_error(CC_ERR_NOT_FOUND, "History page anchor was not found");
    }
    if (rc.code != CC_OK) { message_vec_cleanup(&newest); return rc; }
    if (newest.count > limit) {
        *out_has_more = 1;
        cc_message_cleanup(&newest.items[--newest.count]);
    }
    for (size_t i = 0; i < newest.count / 2U; i++) {
        cc_message_t tmp = newest.items[i];
        newest.items[i] = newest.items[newest.count - 1U - i];
        newest.items[newest.count - 1U - i] = tmp;
    }
    *out_messages = newest.items;
    *out_count = newest.count;
    return cc_result_ok();
}

/** @brief 创建独占路径、文件系统和互斥量的分段后端。
 * @param[in] root_path 可为 NULL 的根路径借用，NULL 使用默认，内部复制。
 * @param[in] segment_bytes 软分段阈值，零使用 256 KiB。
 * @param[in] media_dir 可为 NULL 或空，默认 media，非空须为安全相对路径，内部复制。
 * @param[in] max_base64_bytes 普通回填按原始文件字节限制，上下文还按编码字节限制，零无限制。
 * @param[out] out_store 必填新端口槽，先清零，成功 self 由 owner 最终 vtable.destroy。
 * @return 参数、分配、文件系统能力、锁或建根目录错误，失败回收候选。
 * @note 创建根目录会写盘，父目录创建深度依后端，同路径多实例访问由外层协调。
 */
cc_result_t cc_json_segmented_store_create(
    const char *root_path,
    size_t segment_bytes,
    const char *media_dir,
    size_t max_base64_bytes,
    cc_session_store_t *out_store
)
{
    if (!out_store) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null segmented store output");
    }
    memset(out_store, 0, sizeof(*out_store));
    if (media_dir && media_dir[0] && !media_relative_path_safe(media_dir))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Media directory must be a safe relative path");
    cc_json_segmented_store_t *store = calloc(1, sizeof(*store));
    if (!store) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate segmented store");
    store->root_path = cc_copy_string(root_path ? root_path : "runtime/data/sessions");
    store->media_dir = cc_copy_string((media_dir && *media_dir) ? media_dir : CC_SEGMENTED_DEFAULT_MEDIA_DIR);
    store->segment_bytes = segment_bytes ? segment_bytes : CC_SEGMENTED_DEFAULT_BYTES;
    store->max_base64_bytes = max_base64_bytes;
    if (!store->root_path || !store->media_dir) {
        segmented_destroy(store);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy segmented store config");
    }
    cc_result_t rc = cc_filesystem_get_default(&store->fs);
    if (rc.code != CC_OK) {
        free(store->root_path);
        free(store->media_dir);
        free(store);
        return rc;
    }
    if (!store->fs.vtable || store->fs.version < 3U ||
        !(store->fs.capabilities & CC_FS_CAP_BINARY) ||
        !(store->fs.capabilities & CC_FS_CAP_APPEND) ||
        !(store->fs.capabilities & CC_FS_CAP_STAT) ||
        !(store->fs.capabilities & CC_FS_CAP_ATOMIC_REPLACE) ||
        !(store->fs.capabilities & CC_FS_CAP_FILE_SYNC)) {
        segmented_destroy(store);
        return cc_result_error(CC_ERR_UNSUPPORTED,
                               "Segmented store requires binary, append, stat, atomic replace and sync");
    }
    rc = cc_mutex_create(&store->mutex);
    if (rc.code != CC_OK) {
        segmented_destroy(store);
        return rc;
    }
    rc = ensure_dir_recursive(store, store->root_path);
    if (rc.code != CC_OK) {
        segmented_destroy(store);
        return rc;
    }
    out_store->self = store;
    out_store->vtable = &segmented_vtable;
    return cc_result_ok();
}
