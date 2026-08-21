#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_thread.h"
#include "cc/ports/cc_platform.h"
#include "cc/util/cc_base64.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CC_SEGMENTED_DEFAULT_BYTES (256u * 1024u)
#define CC_SEGMENTED_DEFAULT_MEDIA_DIR "media"
#define CC_SEGMENTED_MAX_LINE_READ (8u * 1024u * 1024u)

typedef struct cc_json_segmented_store {
    char *root_path;
    char *media_dir;
    size_t segment_bytes;
    size_t max_base64_bytes;
    cc_filesystem_t fs;
    cc_mutex_t mutex;
} cc_json_segmented_store_t;

typedef struct string_vec {
    char **items;
    size_t count;
    size_t capacity;
} string_vec_t;

typedef struct message_vec {
    cc_message_t *items;
    size_t count;
    size_t capacity;
} message_vec_t;

/*
 * 释放 string_vec 中的所有字符串和底层数组。
 * 参数: vec - 待清理的字符串向量
 */
static void string_vec_cleanup(string_vec_t *vec)
{
    if (!vec) return;
    for (size_t i = 0; i < vec->count; i++) free(vec->items[i]);
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

/*
 * 向字符串向量追加一个字符串（接管所有权），自动扩容。
 * 参数: vec - 目标向量, text - 待追加字符串（所有权转移）
 */
static cc_result_t string_vec_append_take(string_vec_t *vec, char *text)
{
    if (!vec || !text) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid string vec append");
    if (vec->count == vec->capacity) {
        size_t next_cap = vec->capacity ? vec->capacity * 2 : 8;
        char **next = realloc(vec->items, next_cap * sizeof(char *));
        if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow string vector");
        vec->items = next;
        vec->capacity = next_cap;
    }
    vec->items[vec->count++] = text;
    return cc_result_ok();
}

/*
 * 释放 message_vec 中的所有消息和底层数组。
 * 参数: vec - 待清理的消息向量
 */
static void message_vec_cleanup(message_vec_t *vec)
{
    if (!vec) return;
    for (size_t i = 0; i < vec->count; i++) cc_message_cleanup(&vec->items[i]);
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

/*
 * 向消息向量追加一个消息（接管所有权），自动扩容。
 * 参数: vec - 目标向量, msg - 待追加消息（所有权转移）
 */
static cc_result_t message_vec_append_take(message_vec_t *vec, cc_message_t *msg)
{
    if (!vec || !msg) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid message vec append");
    if (vec->count == vec->capacity) {
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

/*
 * FNV-1a 哈希（64位），对字节数组计算确定性哈希值。
 * 参数: data - 输入字节, len - 字节长度
 * 返回: 64 位哈希值
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

/*
 * FNV-1a 哈希，对 C 字符串计算确定性哈希值。
 * 参数: text - 输入字符串
 * 返回: 64 位哈希值
 */
static unsigned long long fnv1a_text(const char *text)
{
    return fnv1a_bytes((const unsigned char *)(text ? text : ""), text ? strlen(text) : 0);
}

/*
 * 类似 printf 的格式化字符串分配，结果由调用方 free。
 * 参数: fmt - printf 格式字符串, ... - 格式参数
 * 返回: 堆分配的格式化字符串或 NULL
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

/*
 * 检查路径是否为目录。
 * 参数: path - 文件系统路径
 * 返回: 1 表示是目录, 0 表示不是或不存在
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

/*
 * 检查路径是否为普通文件。
 * 参数: path - 文件系统路径
 * 返回: 1 表示是文件, 0 表示不是或不存在
 */
static int path_is_file(cc_json_segmented_store_t *store, const char *path)
{
    if (!store || !path || !store->fs.vtable || !store->fs.vtable->stat) return 0;
    cc_filesystem_stat_t st = {.size = sizeof(st)};
    cc_result_t rc = store->fs.vtable->stat(store->fs.self, path, &st);
    int matched = rc.code == CC_OK && st.type == CC_FS_ENTRY_FILE;
    cc_result_free(&rc);
    return matched;
}

/*
 * 获取文件大小，失败或不存在时返回 0。
 * 参数: path - 文件路径
 * 返回: 文件字节大小或 0
 */
static size_t file_size_or_zero(cc_json_segmented_store_t *store, const char *path)
{
    if (!store || !path || !store->fs.vtable || !store->fs.vtable->stat) return 0;
    cc_filesystem_stat_t st = {.size = sizeof(st)};
    cc_result_t rc = store->fs.vtable->stat(store->fs.self, path, &st);
    if (rc.code != CC_OK || st.type != CC_FS_ENTRY_FILE || st.byte_size > SIZE_MAX) {
        cc_result_free(&rc);
        return 0;
    }
    cc_result_free(&rc);
    return (size_t)st.byte_size;
}

/*
 * 拼接两个路径组件，自动插入 / 分隔符。
 * 参数: a - 路径前缀, b - 路径后缀
 * 返回: 堆分配的拼接路径
 */
static char *join_path2(const char *a, const char *b)
{
    if (!a || !*a) return b ? cc_copy_string(b) : cc_copy_string("");
    if (!b || !*b) return cc_copy_string(a);
    size_t alen = strlen(a);
    int has_sep = a[alen - 1] == '/' || a[alen - 1] == '\\';
    return dup_printf("%s%s%s", a, has_sep ? "" : "/", b);
}

/*
 * 拼接三个路径组件成单个完整路径。
 * 参数: a, b, c - 三个路径片段
 * 返回: 堆分配的拼接路径或 NULL
 */
static char *join_path3(const char *a, const char *b, const char *c)
{
    char *ab = join_path2(a, b);
    if (!ab) return NULL;
    char *abc = join_path2(ab, c);
    free(ab);
    return abc;
}

/*
 * 确保单层目录存在，不存在则创建（父目录必须已存在）。
 * 参数: path - 目录路径
 */
static cc_result_t ensure_dir(cc_json_segmented_store_t *store, const char *path)
{
    if (!store || !path || !*path || !store->fs.vtable || !store->fs.vtable->make_dir) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid directory request");
    }
    if (path_is_dir(store, path)) return cc_result_ok();
    return store->fs.vtable->make_dir(store->fs.self, path);
}

/*
 * 递归确保目录路径完整存在，逐层创建缺失的目录。
 * 参数: path - 完整目录路径
 */
static cc_result_t ensure_dir_recursive(cc_json_segmented_store_t *store, const char *path)
{
    return ensure_dir(store, path);
}

/*
 * 将输入字符串转为安全的文件系统组件名（只保留 alnum/-/_/.，追加哈希后缀）。
 * 参数: text - 原始文本
 * 返回: 堆分配的安全名称
 */
static char *safe_component(const char *text)
{
    const char *safe = (text && *text) ? text : "empty";
    size_t len = strlen(safe);
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

/*
 * 根据 session_id 计算会话目录路径。
 * 参数: store - store 实例, session_id - 会话标识
 * 返回: 堆分配的目录路径
 */
static char *session_dir_path(cc_json_segmented_store_t *store, const char *session_id)
{
    char *safe = safe_component(session_id);
    if (!safe) return NULL;
    char *path = join_path2(store->root_path, safe);
    free(safe);
    return path;
}

/*
 * 根据会话目录路径获取 manifest.json 完整路径。
 * 参数: session_dir - 会话目录路径
 * 返回: 堆分配的 manifest 路径
 */
static char *manifest_path_for_session_dir(const char *session_dir)
{
    return join_path2(session_dir, "manifest.json");
}

/*
 * 生成分段文件名，格式 events-000001.jsonl。
 * 参数: index - 分段编号（从 1 开始）
 * 返回: 堆分配的文件名
 */
static char *segment_name(int index)
{
    return dup_printf("events-%06d.jsonl", index > 0 ? index : 1);
}

/*
 * 根据会话目录和分段号获取分段文件完整路径。
 * 参数: session_dir - 会话目录, index - 分段编号
 * 返回: 堆分配的路径
 */
static char *segment_path_for_session_dir(const char *session_dir, int index)
{
    char *name = segment_name(index);
    if (!name) return NULL;
    char *path = join_path2(session_dir, name);
    free(name);
    return path;
}

/*
 * 在会话目录下创建媒体子目录结构（img/audio/video/file × user/assistant/tool + misc）。
 * 参数: store - store 实例, session_dir - 会话目录
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

/*
 * 原子写入文本文件（先写 tmp、备份旧文件、rename 替换、清理备份），保证断电安全。
 * 参数: path - 目标文件路径, text - 写入内容
 */
static cc_result_t write_text_file_atomic(
    cc_json_segmented_store_t *store,
    const char *path,
    const char *text
)
{
    if (!store || !path || !store->fs.vtable || !store->fs.vtable->write_text ||
        !store->fs.vtable->atomic_replace) {
        return cc_result_error(CC_ERR_UNSUPPORTED, "Filesystem cannot durably replace JSON files");
    }
    char *tmp = dup_printf("%s.tmp", path);
    if (!tmp) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build temporary path");
    cc_result_t rc = store->fs.vtable->write_text(store->fs.self, tmp, text ? text : "");
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
    if (rc.code != CC_OK && store->fs.vtable->remove) {
        cc_result_t cleanup = store->fs.vtable->remove(store->fs.self, tmp);
        cc_result_free(&cleanup);
    }
    free(tmp);
    return rc;
}

static cc_result_t parse_json_file(
    cc_json_segmented_store_t *store,
    const char *path,
    cc_json_value_t **out_value
)
{
    if (!store || !path || !out_value || !store->fs.vtable || !store->fs.vtable->read_text) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid JSON file read");
    }
    *out_value = NULL;
    char *text = NULL;
    cc_result_t primary = store->fs.vtable->read_text(store->fs.self, path, &text);
    if (primary.code == CC_OK) {
        primary = cc_json_parse(text ? text : "", out_value);
        free(text);
        if (primary.code == CC_OK) return primary;
    }

    const char *suffixes[] = {".tmp", ".bak"};
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        char *candidate = dup_printf("%s%s", path, suffixes[i]);
        if (!candidate) {
            cc_result_free(&primary);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build recovery path");
        }
        text = NULL;
        cc_result_t read_rc = store->fs.vtable->read_text(store->fs.self, candidate, &text);
        cc_json_value_t *recovered = NULL;
        cc_result_t parse_rc = read_rc.code == CC_OK
            ? cc_json_parse(text ? text : "", &recovered) : read_rc;
        free(text);
        if (parse_rc.code == CC_OK && recovered && store->fs.vtable->atomic_replace) {
            cc_result_t replace_rc = store->fs.vtable->atomic_replace(store->fs.self,
                                                                      candidate, path);
            if (replace_rc.code == CC_OK) {
                cc_result_free(&primary);
                *out_value = recovered;
                free(candidate);
                return cc_result_ok();
            }
            cc_json_destroy(recovered);
            cc_result_free(&replace_rc);
        } else {
            cc_json_destroy(recovered);
        }
        cc_result_free(&parse_rc);
        free(candidate);
    }
    return primary;
}

/*
 * 序列化并保存 manifest.json（id, workspace_dir, status, segment_bytes, current_segment, segments 列表）。
 * 若未传 workspace_dir 则保留已有值。
 * 参数: store - store 实例, session_dir - 会话目录, session_id - 会话 ID,
 *        workspace_dir - 工作目录或 NULL, current_segment - 当前分段号
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

    char *existing_workspace = NULL;
    if (!workspace_dir) {
        cc_json_value_t *existing = NULL;
        cc_result_t load_rc = parse_json_file(store, manifest_path, &existing);
        if (load_rc.code == CC_OK && existing) {
            const char *ws = cc_json_string_value(cc_json_object_get(existing, "workspace_dir"));
            if (ws) {
                existing_workspace = cc_copy_string(ws);
                if (!existing_workspace) {
                    cc_json_destroy(existing);
                    free(manifest_path);
                    return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy manifest workspace");
                }
            }
        }
        cc_json_destroy(existing);
        cc_result_free(&load_rc);
    }
    const char *workspace_value = workspace_dir ? workspace_dir :
        (existing_workspace ? existing_workspace : "");

    cc_json_value_t *root = cc_json_create_object();
    if (!root) {
        free(existing_workspace);
        free(manifest_path);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate manifest JSON");
    }
    cc_json_object_set(root, "id", cc_json_create_string(session_id ? session_id : ""));
    cc_json_object_set(root, "workspace_dir", cc_json_create_string(workspace_value));
    cc_json_object_set(root, "status", cc_json_create_string("active"));
    cc_json_object_set(root, "segment_bytes", cc_json_create_number((double)store->segment_bytes));
    cc_json_object_set(root, "media_dir", cc_json_create_string(store->media_dir));
    cc_json_object_set(root, "current_segment", cc_json_create_number((double)current_segment));
    cc_json_value_t *segments = cc_json_create_array();
    for (int i = 1; segments && i <= current_segment; i++) {
        char *name = segment_name(i);
        cc_json_array_append(segments, cc_json_create_string(name ? name : ""));
        free(name);
    }
    cc_json_object_set(root, "segments", segments ? segments : cc_json_create_array());
    char *json = cc_json_stringify(root);
    cc_json_destroy(root);
    cc_result_t rc = json ? write_text_file_atomic(store, manifest_path, json)
                          : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to stringify manifest");
    free(json);
    free(existing_workspace);
    free(manifest_path);
    return rc;
}

/*
 * 从 manifest 读取 current_segment 值，默认返回 1。
 * 参数: session_dir - 会话目录
 * 返回: 当前分段编号
 */
static int load_current_segment(cc_json_segmented_store_t *store, const char *session_dir)
{
    int current = 1;
    char *manifest = manifest_path_for_session_dir(session_dir);
    if (!manifest) return current;
    cc_json_value_t *root = NULL;
    cc_result_t rc = parse_json_file(store, manifest, &root);
    free(manifest);
    if (rc.code == CC_OK && root) {
        int parsed = cc_json_int_value(cc_json_object_get(root, "current_segment"));
        if (parsed > 0) current = parsed;
        cc_json_destroy(root);
    }
    cc_result_free(&rc);
    return current;
}

/*
 * 创建新会话：建目录、初始化 media 子目录、创建首段文件、生成 manifest。
 * 参数: self - store 实例, session_id - 会话 ID, workspace_dir - 工作目录
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
    rc = ensure_dir_recursive(store, session_dir);
    if (rc.code == CC_OK) rc = ensure_media_dirs(store, session_dir);
    char *seg = NULL;
    if (rc.code == CC_OK) {
        seg = segment_path_for_session_dir(session_dir, 1);
        if (!seg) rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build segment path");
    }
    if (rc.code == CC_OK && !path_is_file(store, seg)) {
        if (!store->fs.vtable->append_bytes) {
            rc = cc_result_error(CC_ERR_UNSUPPORTED, "Filesystem cannot create segment files");
        } else {
            rc = store->fs.vtable->append_bytes(store->fs.self, seg, NULL, 0);
        }
    }
    if (rc.code == CC_OK) {
        char *manifest = manifest_path_for_session_dir(session_dir);
        int has_manifest = manifest && path_is_file(store, manifest);
        free(manifest);
        if (!has_manifest) {
            rc = save_manifest(store, session_dir, session_id, workspace_dir, 1);
        }
    }
    free(seg);
    cc_mutex_unlock(store->mutex);
    free(session_dir);
    return rc;
}

/*
 * 根据消息角色返回对应的媒体来源标签（assistant/tool/user）。
 * 参数: role - 消息角色枚举
 * 返回: 来源字符串
 */
static const char *role_source(cc_message_role_t role)
{
    if (role == CC_ROLE_ASSISTANT) return "assistant";
    if (role == CC_ROLE_TOOL) return "tool";
    return "user";
}

/*
 * 根据 artifact 的 MIME 类型/kind 确定媒体目录分类（img/audio/video/file/misc）。
 * 参数: artifact - 媒体 artifact
 * 返回: 目录分类名
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

/*
 * 根据 artifact 的 MIME 类型确定文件扩展名，未知类型返回 .bin。
 * 参数: artifact - 媒体 artifact
 * 返回: 扩展名字符串（含点号）
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

/*
 * 检查 path 是否以 prefix 开头（目录级匹配）。
 * 参数: path - 待检查路径, prefix - 前缀
 * 返回: 1 表示匹配, 0 表示不匹配
 */
static int path_starts_with(const char *path, const char *prefix)
{
    if (!path || !prefix) return 0;
    size_t len = strlen(prefix);
    return strncmp(path, prefix, len) == 0 &&
           (path[len] == '\0' || path[len] == '/' || path[len] == '\\');
}

/*
 * 将 base64 素材写入磁盘文件并更新 artifact 的 path（文件不存在时才写入避免重复）。
 * 参数: path - 目标文件路径, data - 二进制数据, len - 数据长度
 */
static cc_result_t write_bytes_if_missing(
    cc_json_segmented_store_t *store,
    const char *path,
    const unsigned char *data,
    size_t len
)
{
    if (!store || !store->fs.vtable || !store->fs.vtable->write_bytes) {
        return cc_result_error(CC_ERR_UNSUPPORTED, "Filesystem cannot write media bytes");
    }
    if (path_is_file(store, path)) return cc_result_ok();
    cc_result_t rc = store->fs.vtable->write_bytes(store->fs.self, path, data, len);
    if (rc.code == CC_OK && store->fs.vtable->sync_file) {
        rc = store->fs.vtable->sync_file(store->fs.self, path);
    }
    return rc;
}

/*
 * 从文件中读取所有字节，同时可计算 FNV-1a 哈希。
 * 参数: path - 文件路径, out_data - 输出数据, out_len - 输出长度, out_hash - 输出哈希（可选）
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

/*
 * 构建媒体文件相对于会话目录的路径。
 * 参数: store - store 实例, kind_dir - 媒体分类, source - 角色源, file_name - 文件名
 * 返回: 堆分配的相对路径
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

/*
 * 将媒体制品的 base64 数据外化到磁盘，更新 artifact->path 并释放 data_base64。
 * 参数: store - store 实例, session_dir - 会话目录, source - 角色源,
 *        record_id - 记录 ID, part_index - 内容部分索引, artifact - 媒体制品
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
        free(artifact->data_base64);
        artifact->data_base64 = NULL;
        return cc_result_ok();
    }
    if (!artifact->data_base64 && !artifact->path) {
        return cc_result_ok();
    }

    unsigned char *data = NULL;
    size_t data_len = 0;
    unsigned long long hash = 0;
    cc_result_t rc = cc_result_ok();
    if (artifact->data_base64 && artifact->data_base64[0]) {
        rc = cc_base64_decode(artifact->data_base64, &data, &data_len);
        if (rc.code != CC_OK) return rc;
        hash = fnv1a_bytes(data, data_len);
    } else if (artifact->path && artifact->path[0]) {
        rc = read_file_bytes(store, artifact->path, &data, &data_len, &hash);
        if (rc.code != CC_OK) {
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

/*
 * 将已外化到磁盘的媒体制品读回内存并 base64 编码，超 max_base64_bytes 则跳过。
 * 参数: store - store 实例, session_dir - 会话目录, artifact - 需要水合的媒体制品
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
    char *abs = join_path2(session_dir, artifact->path);
    if (!abs) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build media hydrate path");
    size_t size = file_size_or_zero(store, abs);
    if (store->max_base64_bytes > 0 && size > store->max_base64_bytes) {
        free(abs);
        return cc_result_ok();
    }
    unsigned char *data = NULL;
    size_t len = 0;
    cc_result_t rc = read_file_bytes(store, abs, &data, &len, NULL);
    free(abs);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return cc_result_ok();
    }
    char *base64 = NULL;
    rc = cc_base64_encode(data, len, &base64);
    free(data);
    if (rc.code != CC_OK) return rc;
    artifact->data_base64 = base64;
    if (artifact->bytes == 0) artifact->bytes = len;
    return cc_result_ok();
}

/*
 * 对消息中的所有非文本内容部分调用 hydrate_artifact 读回磁盘媒体数据。
 * 参数: store - store 实例, session_dir - 会话目录, message - 目标消息
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

/*
 * 在 JSON 对象中按 key 设置字符串值（仅当 value 非 NULL 时），简化样板代码。
 * 参数: obj - JSON 对象, key - 键名, value - 字符串值或 NULL
 */
static void json_set_string_if_present(cc_json_value_t *obj, const char *key, const char *value)
{
    if (value) cc_json_object_set(obj, key, cc_json_create_string(value));
}

/*
 * 将 cc_message_t 序列化为 JSON 记录对象（含 id/session_id/role/content_parts/tool_calls/reasoning 等字段）。
 * 参数: message - 消息对象, out_obj - 输出的 JSON 对象
 */
static cc_result_t message_to_record_object(const cc_message_t *message, cc_json_value_t **out_obj)
{
    *out_obj = NULL;
    cc_json_value_t *msg = cc_json_create_object();
    if (!msg) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate message record");
    json_set_string_if_present(msg, "id", message->id ? message->id : "");
    json_set_string_if_present(msg, "session_id", message->session_id ? message->session_id : "");
    cc_json_object_set(msg, "role", cc_json_create_string(cc_message_role_string(message->role)));
    char *content_parts = NULL;
    cc_result_t rc = cc_content_parts_to_json(&message->content, &content_parts);
    if (rc.code != CC_OK) {
        cc_json_destroy(msg);
        return rc;
    }
    cc_json_object_set(msg, "content_parts", cc_json_create_string(content_parts ? content_parts : "[]"));
    free(content_parts);
    char *tool_calls = NULL;
    rc = cc_tool_call_list_to_json(&message->tool_calls, &tool_calls);
    if (rc.code != CC_OK) {
        cc_json_destroy(msg);
        return rc;
    }
    cc_json_object_set(msg, "tool_calls", cc_json_create_string(tool_calls ? tool_calls : "[]"));
    free(tool_calls);
    json_set_string_if_present(msg, "reasoning_content", message->reasoning_content);
    json_set_string_if_present(msg, "tool_call_id", message->tool_call_id);
    json_set_string_if_present(msg, "created_at", message->created_at);
    *out_obj = msg;
    return cc_result_ok();
}

/*
 * 从 JSON 记录对象反序列化为 cc_message_t。
 * 参数: msg - JSON 记录对象, out - 输出的消息对象
 */
static cc_result_t message_from_record_object(cc_json_value_t *msg, cc_message_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!msg || !cc_json_is_object(msg)) {
        return cc_result_error(CC_ERR_JSON, "Message record must be an object");
    }
    const char *id = cc_json_string_value(cc_json_object_get(msg, "id"));
    const char *sid = cc_json_string_value(cc_json_object_get(msg, "session_id"));
    const char *role = cc_json_string_value(cc_json_object_get(msg, "role"));
    out->id = id ? cc_copy_string(id) : NULL;
    out->session_id = sid ? cc_copy_string(sid) : NULL;
    out->role = cc_message_role_from_string(role);
    cc_content_parts_init(&out->content);
    cc_tool_call_list_init(&out->tool_calls);
    const char *parts_json = cc_json_string_value(cc_json_object_get(msg, "content_parts"));
    cc_result_t rc = parts_json ? cc_content_parts_from_json(parts_json, &out->content)
                                : cc_result_ok();
    if (rc.code == CC_OK) {
        const char *tc_json = cc_json_string_value(cc_json_object_get(msg, "tool_calls"));
        if (tc_json) rc = cc_tool_call_list_from_json(tc_json, &out->tool_calls);
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
    if (rc.code != CC_OK) cc_message_cleanup(out);
    return rc;
}

/*
 * 将 JSON 记录追加到当前分段文件末尾，超出 segment_bytes 则自动滚动到新分段。
 * 参数: store - store 实例, session_dir - 会话目录, session_id - 会话 ID, record - JSON 记录
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
    if (payload_len == SIZE_MAX) {
        free(line);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Segment record length overflow");
    }
    size_t line_len = payload_len + 1;
    int current = load_current_segment(store, session_dir);
    char *seg_path = segment_path_for_session_dir(session_dir, current);
    if (!seg_path) {
        free(line);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build segment path");
    }
    size_t current_size = file_size_or_zero(store, seg_path);
    if (store->segment_bytes > 0 && current_size > 0 &&
        current_size + line_len > store->segment_bytes) {
        free(seg_path);
        current++;
        seg_path = segment_path_for_session_dir(session_dir, current);
        if (!seg_path) {
            free(line);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build next segment path");
        }
        cc_result_t rc = save_manifest(store, session_dir, session_id, NULL, current);
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
    cc_result_t rc = store->fs.vtable->append_bytes(store->fs.self, seg_path, framed, line_len);
    if (rc.code == CC_OK && store->fs.vtable->sync_file) {
        rc = store->fs.vtable->sync_file(store->fs.self, seg_path);
    }
    free(framed);
    free(line);
    free(seg_path);
    return rc;
}

/*
 * 读取 JSONL 分段文件中的所有行到 string_vec。
 * 参数: path - 分段文件路径, out_lines - 输出的行向量
 */
static cc_result_t load_segment_lines(
    cc_json_segmented_store_t *store,
    const char *path,
    string_vec_t *out_lines
)
{
    if (!store || !path || !out_lines || !store->fs.vtable ||
        !store->fs.vtable->exists || !store->fs.vtable->read_bytes) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid segment read request");
    }
    int exists = 0;
    cc_result_t rc = store->fs.vtable->exists(store->fs.self, path, &exists);
    if (rc.code != CC_OK || !exists) return rc;
    void *raw = NULL;
    size_t size = 0;
    rc = store->fs.vtable->read_bytes(store->fs.self, path, &raw, &size);
    if (rc.code != CC_OK) return rc;
    if (size > CC_SEGMENTED_MAX_LINE_READ && memchr(raw, '\n', size) == NULL) {
        free(raw);
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "JSONL record exceeds read limit");
    }
    const char *bytes = (const char *)raw;
    size_t start = 0;
    for (size_t i = 0; rc.code == CC_OK && i <= size; i++) {
        if (i < size && bytes[i] != '\n') continue;
        size_t len = i - start;
        if (len > 0 && bytes[start + len - 1] == '\r') len--;
        if (len > CC_SEGMENTED_MAX_LINE_READ) {
            rc = cc_result_error(CC_ERR_LIMIT_EXCEEDED, "JSONL record exceeds read limit");
            break;
        }
        if (len > 0) {
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

/*
 * 解析一行 JSONL 记录，若 type=="message" 则反序列化为 cc_message_t 并水合媒体。
 * 参数: store - store 实例, session_dir - 会话目录, line - JSON 行,
 *        out_message - 输出消息, out_is_message - 输出是否成功解析为消息
 */
static cc_result_t append_message_record_object(
    cc_json_segmented_store_t *store,
    const char *session_dir,
    cc_json_value_t *record,
    message_vec_t *newest
)
{
    const char *type = cc_json_string_value(cc_json_object_get(record, "type"));
    if (!type || strcmp(type, "message") != 0) {
        return cc_result_ok();
    }
    cc_message_t message;
    memset(&message, 0, sizeof(message));
    cc_result_t rc = message_from_record_object(cc_json_object_get(record, "message"), &message);
    if (rc.code == CC_OK) rc = hydrate_message(store, session_dir, &message);
    if (rc.code == CC_OK) rc = message_vec_append_take(newest, &message);
    cc_message_cleanup(&message);
    return rc;
}

static cc_result_t parse_message_records(
    cc_json_segmented_store_t *store,
    const char *session_dir,
    const char *line,
    size_t max_loaded,
    message_vec_t *newest
)
{
    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse(line, &root);
    if (rc.code != CC_OK) return rc;
    const char *type = cc_json_string_value(cc_json_object_get(root, "type"));
    if (!type || strcmp(type, "batch") != 0) {
        rc = append_message_record_object(store, session_dir, root, newest);
        cc_json_destroy(root);
        return rc;
    }

    int schema_version = cc_json_int_value(cc_json_object_get(root, "schema_version"));
    const char *records_json = cc_json_string_value(cc_json_object_get(root, "records_json"));
    const char *checksum = cc_json_string_value(cc_json_object_get(root, "checksum"));
    double length_value = cc_json_number_value(cc_json_object_get(root, "length"));
    size_t records_len = records_json ? strlen(records_json) : 0;
    unsigned long long hash = fnv1a_bytes((const unsigned char *)(records_json ? records_json : ""),
                                          records_len);
    char expected[17];
    snprintf(expected, sizeof(expected), "%016llx", hash);
    if (schema_version != 2 || !records_json || !checksum ||
        length_value < 0 || length_value > (double)SIZE_MAX ||
        (size_t)length_value != records_len || strcmp(checksum, expected) != 0) {
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_STORAGE, "Segment batch checksum or schema is invalid");
    }

    cc_json_value_t *records = NULL;
    rc = cc_json_parse(records_json, &records);
    if (rc.code != CC_OK || !cc_json_is_array(records)) {
        cc_json_destroy(records);
        cc_json_destroy(root);
        if (rc.code == CC_OK) return cc_result_error(CC_ERR_STORAGE, "Segment batch records are invalid");
        return rc;
    }
    for (int i = cc_json_array_size(records) - 1;
         rc.code == CC_OK && i >= 0 && (max_loaded == 0 || newest->count < max_loaded);
         i--) {
        rc = append_message_record_object(store, session_dir,
                                          cc_json_array_get(records, i), newest);
    }
    cc_json_destroy(records);
    cc_json_destroy(root);
    return rc;
}

/*
 * vtable 回调：从最新的分段文件按 limit 加载消息，返回按时间正序排列的数组。
 * 参数: self - store 实例, session_id - 会话 ID, limit - 最大消息数,
 *        out_messages - 输出消息数组, out_count - 输出消息数量
 */
static cc_result_t segmented_load_messages(
    void *self,
    const char *session_id,
    int limit,
    cc_message_t **out_messages,
    size_t *out_count
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
    int current = load_current_segment(store, session_dir);
    int max_loaded = limit > 0 ? limit : 0;
    message_vec_t newest;
    memset(&newest, 0, sizeof(newest));
    cc_result_t rc = cc_result_ok();
    for (int seg = current; rc.code == CC_OK && seg >= 1; seg--) {
        char *path = segment_path_for_session_dir(session_dir, seg);
        if (!path) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build segment path");
            break;
        }
        string_vec_t lines;
        memset(&lines, 0, sizeof(lines));
        rc = load_segment_lines(store, path, &lines);
        free(path);
        for (size_t idx = lines.count; rc.code == CC_OK && idx > 0; idx--) {
            if (max_loaded > 0 && newest.count >= (size_t)max_loaded) break;
            rc = parse_message_records(store, session_dir, lines.items[idx - 1],
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

/*
 * 释放 cc_session_t 中各堆分配字段并清零。
 * 参数: session - 待清理的会话对象
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

/*
 * 将 manifest 中的 status 字符串转为 cc_session_status_t 枚举。
 * 参数: status - "active"/"completed"/"error" 字符串
 * 返回: 对应的会话状态枚举值
 */
static cc_session_status_t session_status_from_manifest(const char *status)
{
    if (status && strcmp(status, "completed") == 0) return CC_SESSION_COMPLETED;
    if (status && strcmp(status, "error") == 0) return CC_SESSION_ERROR;
    return CC_SESSION_ACTIVE;
}

/*
 * 读取 manifest.json 并追加一个 cc_session_t 到会话列表中。
 * 参数: manifest_path - manifest 文件路径, sessions - 会话数组指针,
 *        count - 当前数量, capacity - 当前容量
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
    cc_result_t rc = parse_json_file(store, manifest_path, &root);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return cc_result_ok();
    }
    if (*count == *capacity) {
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
    out->id = id ? cc_copy_string(id) : cc_copy_string("");
    out->workspace_dir = ws ? cc_copy_string(ws) : cc_copy_string("");
    out->name = out->id ? cc_copy_string(out->id) : NULL;
    out->status = session_status_from_manifest(status);
    cc_json_destroy(root);
    if (!out->id || !out->workspace_dir || !out->name) {
        session_cleanup_fields(out);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy session manifest");
    }
    (*count)++;
    return cc_result_ok();
}

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
        if (!path_is_dir(store, session_dir)) {
            free(session_dir);
            continue;
        }
        char *manifest = manifest_path_for_session_dir(session_dir);
        free(session_dir);
        if (!manifest) {
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build manifest path");
            break;
        }
        if (path_is_file(store, manifest)) rc = string_vec_append_take(paths, manifest);
        else free(manifest);
    }
    for (size_t i = 0; i < count; i++) free(items[i]);
    free(items);
    return rc;
}

/*
 * 从 list_manifest_paths 获取所有 manifest.json 路径，逐个解析为 cc_session_t 返回。
 * 参数: self - store 实例, out_sessions - 输出会话数组, out_count - 输出数量
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

/*
 * vtable 回调：删除并重建会话目录，重置分段和 manifest。
 * 参数: self - store 实例, session_id - 会话 ID
 */
static cc_result_t segmented_clear_session(void *self, const char *session_id)
{
    cc_json_segmented_store_t *store = (cc_json_segmented_store_t *)self;
    if (!store || !session_id) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid clear session request");
    cc_mutex_lock(store->mutex);
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
    if (rc.code == CC_OK) rc = save_manifest(store, session_dir, session_id, NULL, 1);
    free(session_dir);
    cc_mutex_unlock(store->mutex);
    return rc;
}

/*
 * 释放 store 的所有资源：root_path、media_dir、mutex 和 store 自身。
 * 参数: self - store 实例
 */
static void segmented_destroy(void *self)
{
    cc_json_segmented_store_t *store = (cc_json_segmented_store_t *)self;
    if (!store) return;
    if (store->mutex) cc_mutex_lock(store->mutex);
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

static const char *session_record_session_id(const cc_session_record_t *record)
{
    if (!record) return NULL;
    if (record->session_id && record->session_id[0]) return record->session_id;
    if (record->type == CC_SESSION_RECORD_MESSAGE && record->data.message) {
        return record->data.message->session_id;
    }
    return NULL;
}

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
            cc_json_object_set(record, "type", cc_json_create_string("message"));
            cc_json_object_set(record, "message", message);
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
                cc_json_object_set(record, "type", cc_json_create_string("tool_call"));
                cc_json_object_set(tool_call, "id", cc_json_create_string(call->id ? call->id : ""));
                cc_json_object_set(tool_call, "session_id", cc_json_create_string(session_id));
                cc_json_object_set(tool_call, "name", cc_json_create_string(call->name ? call->name : ""));
                cc_json_object_set(tool_call, "arguments_json",
                                   cc_json_create_string(call->arguments_json ? call->arguments_json : ""));
                cc_json_object_set(tool_call, "status", cc_json_create_string("completed"));
                cc_json_object_set(record, "tool_call", tool_call);
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
            cc_json_object_set(record, "type", cc_json_create_string("tool_result"));
            cc_json_object_set(tool_result, "id", cc_json_create_string(tool_call_id ? tool_call_id : ""));
            cc_json_object_set(tool_result, "session_id", cc_json_create_string(session_id));
            cc_json_object_set(tool_result, "tool_call_id", cc_json_create_string(tool_call_id ? tool_call_id : ""));
            cc_json_object_set(tool_result, "ok", cc_json_create_bool(result && result->ok));
            cc_json_object_set(tool_result, "text", cc_json_create_string(result && result->text ? result->text : ""));
            cc_json_object_set(tool_result, "error", cc_json_create_string(result && result->error ? result->error : ""));
            cc_json_object_set(tool_result, "metadata", cc_json_create_string(result && result->metadata ? result->metadata : ""));
            cc_json_object_set(tool_result, "artifacts", cc_json_create_string(artifacts_json ? artifacts_json : "[]"));
            cc_json_object_set(record, "tool_result", tool_result);
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
    }

    cc_mutex_lock(store->mutex);
    char *session_dir = session_dir_path(store, session_id);
    cc_result_t rc = session_dir ? ensure_dir_recursive(store, session_dir)
                                 : cc_result_error(CC_ERR_OUT_OF_MEMORY,
                                                   "Failed to build session directory");
    if (rc.code == CC_OK) rc = ensure_media_dirs(store, session_dir);
    if (rc.code == CC_OK) {
        char *manifest = manifest_path_for_session_dir(session_dir);
        int has_manifest = manifest && path_is_file(store, manifest);
        free(manifest);
        if (!has_manifest) rc = save_manifest(store, session_dir, session_id, NULL, 1);
    }

    cc_json_value_t *record_array = NULL;
    if (rc.code == CC_OK) {
        record_array = cc_json_create_array();
        if (!record_array) rc = cc_result_error(CC_ERR_OUT_OF_MEMORY,
                                                "Failed to allocate segmented batch");
    }
    for (size_t i = 0; rc.code == CC_OK && i < count; i++) {
        cc_json_value_t *record = NULL;
        rc = build_batch_record(store, session_dir, session_id, &records[i], &record);
        if (rc.code == CC_OK) cc_json_array_append(record_array, record);
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
            cc_json_object_set(batch, "type", cc_json_create_string("batch"));
            cc_json_object_set(batch, "schema_version", cc_json_create_number(2));
            cc_json_object_set(batch, "length", cc_json_create_number((double)records_len));
            cc_json_object_set(batch, "checksum", cc_json_create_string(checksum));
            cc_json_object_set(batch, "records_json", cc_json_create_string(records_json));
            rc = append_record_line(store, session_dir, session_id, batch);
        }
    }
    cc_json_destroy(batch);
    cc_json_destroy(record_array);
    free(records_json);
    free(session_dir);
    cc_mutex_unlock(store->mutex);
    return rc;
}

static cc_session_store_vtable_t segmented_vtable = {
    segmented_create_session,
    segmented_append_records,
    segmented_load_messages,
    segmented_list_sessions,
    segmented_clear_session,
    segmented_destroy
};

/*
 * 创建 JSON 分段 session store 实例，初始化路径、分段阈值、媒体目录和 mutex。
 * 参数: root_path - 存储根目录, segment_bytes - 分段字节阈值, media_dir - 媒体目录名,
 *        max_base64_bytes - 最大 base64 回填大小, out_store - 输出 session store 句柄
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
    if (!store->fs.vtable || store->fs.version < CC_FILESYSTEM_VTABLE_VERSION ||
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
