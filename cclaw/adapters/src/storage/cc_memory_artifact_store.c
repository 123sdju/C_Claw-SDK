#include "cc/ports/cc_artifact_store.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>
#include <string.h>

/*
 * 内存 artifact 记录。
 *
 * session_id 用于按会话过滤，artifact 本身通过 cc_media_artifact_copy 深拷贝，避免调用方
 * 释放原 artifact 后 store 中出现悬空指针。
 */
typedef struct memory_artifact_record {
    char *session_id;
    cc_media_artifact_t artifact;
} memory_artifact_record_t;

/*
 * 内存 artifact store 私有状态。
 *
 * 当前实现没有 mutex，适合单线程测试或由上层保证串行访问的轻量 profile；需要多线程
 * 并发 artifact 写入时，应在 adapter 外层加锁或扩展本实现。
 */
typedef struct memory_artifact_store {
    memory_artifact_record_t *items;
    size_t count;
    size_t capacity;
} memory_artifact_store_t;

/* 释放一条记录的会话 id 和 artifact 深拷贝字段，并把槽位清零。 */
static void record_cleanup(memory_artifact_record_t *record)
{
    if (!record) return;
    free(record->session_id);
    cc_media_artifact_cleanup(&record->artifact);
    memset(record, 0, sizeof(*record));
}

/*
 * 确保数组有一个可写槽位。
 *
 * 扩容时初始化新增区域，便于后续 destroy/cleanup 对未使用槽位保持安全。
 */
static cc_result_t reserve_record(memory_artifact_store_t *store)
{
    if (store->count < store->capacity) return cc_result_ok();
    size_t next_capacity = store->capacity ? store->capacity * 2 : 8;
    memory_artifact_record_t *next = realloc(
        store->items, next_capacity * sizeof(memory_artifact_record_t));
    if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow artifact store");
    memset(next + store->capacity, 0,
        (next_capacity - store->capacity) * sizeof(memory_artifact_record_t));
    store->items = next;
    store->capacity = next_capacity;
    return cc_result_ok();
}

/*
 * 根据 artifact_id 查找数组下标。
 *
 * 返回 long 是为了用 -1 表示未找到；调用者在转换成 size_t 前必须先判断 >= 0。
 */
static long find_record(memory_artifact_store_t *store, const char *artifact_id)
{
    if (!store || !artifact_id) return -1;
    for (size_t i = 0; i < store->count; i++) {
        if (store->items[i].artifact.id &&
            strcmp(store->items[i].artifact.id, artifact_id) == 0) {
            return (long)i;
        }
    }
    return -1;
}

/*
 * 保存或替换 artifact。
 *
 * id 已存在时先清理旧记录再覆盖；否则追加新记录。函数成功后 store 拥有 artifact 的
 * 深拷贝，调用方仍然负责释放传入 artifact。
 */
static cc_result_t memory_artifact_put(
    void *self,
    const char *session_id,
    const cc_media_artifact_t *artifact
)
{
    memory_artifact_store_t *store = (memory_artifact_store_t *)self;
    if (!store || !artifact || !artifact->id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid artifact put argument");
    }

    long existing = find_record(store, artifact->id);
    memory_artifact_record_t next_record;
    memset(&next_record, 0, sizeof(next_record));
    next_record.session_id = session_id ? cc_copy_string(session_id) : NULL;
    if (session_id && !next_record.session_id) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy artifact session id");
    }
    cc_result_t rc = cc_media_artifact_copy(artifact, &next_record.artifact);
    if (rc.code != CC_OK) {
        record_cleanup(&next_record);
        return rc;
    }

    if (existing >= 0) {
        record_cleanup(&store->items[existing]);
        store->items[existing] = next_record;
        return cc_result_ok();
    }

    rc = reserve_record(store);
    if (rc.code != CC_OK) {
        record_cleanup(&next_record);
        return rc;
    }
    store->items[store->count++] = next_record;
    return cc_result_ok();
}

/*
 * 按 id 读取 artifact。
 *
 * out_artifact 由调用方提供，成功后其中字段为深拷贝，调用方需要调用
 * cc_media_artifact_cleanup。
 */
static cc_result_t memory_artifact_get(
    void *self,
    const char *artifact_id,
    cc_media_artifact_t *out_artifact
)
{
    memory_artifact_store_t *store = (memory_artifact_store_t *)self;
    if (!out_artifact) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null artifact output");
    }
    long index = find_record(store, artifact_id);
    if (index < 0) {
        return cc_result_error(CC_ERR_NOT_FOUND, "Artifact not found");
    }
    return cc_media_artifact_copy(&store->items[index].artifact, out_artifact);
}

/*
 * 列举 artifact。
 *
 * session_id 为空时返回全部 artifact；非空时只返回该会话的记录。out_artifacts 由本函数
 * 初始化，调用方最终使用 cc_media_artifact_list_cleanup 释放。
 */
static cc_result_t memory_artifact_list(
    void *self,
    const char *session_id,
    cc_media_artifact_list_t *out_artifacts
)
{
    memory_artifact_store_t *store = (memory_artifact_store_t *)self;
    if (!store || !out_artifacts) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid artifact list argument");
    }
    cc_media_artifact_list_init(out_artifacts);
    for (size_t i = 0; i < store->count; i++) {
        if (session_id && (!store->items[i].session_id ||
            strcmp(store->items[i].session_id, session_id) != 0)) {
            continue;
        }
        cc_result_t rc = cc_media_artifact_list_append(
            out_artifacts, &store->items[i].artifact);
        if (rc.code != CC_OK) {
            cc_media_artifact_list_cleanup(out_artifacts);
            return rc;
        }
    }
    return cc_result_ok();
}

/*
 * 删除指定 artifact。
 *
 * 删除后通过前移元素压缩数组，并清零尾部槽位，避免 destroy 阶段重复释放已经移动的记录。
 */
static cc_result_t memory_artifact_remove(void *self, const char *artifact_id)
{
    memory_artifact_store_t *store = (memory_artifact_store_t *)self;
    long index = find_record(store, artifact_id);
    if (index < 0) return cc_result_error(CC_ERR_NOT_FOUND, "Artifact not found");
    record_cleanup(&store->items[index]);
    size_t pos = (size_t)index;
    for (size_t i = pos + 1; i < store->count; i++) {
        store->items[i - 1] = store->items[i];
    }
    store->count--;
    memset(&store->items[store->count], 0, sizeof(store->items[store->count]));
    return cc_result_ok();
}

/* 销毁内存 artifact store 及其全部记录。 */
static void memory_artifact_destroy(void *self)
{
    memory_artifact_store_t *store = (memory_artifact_store_t *)self;
    if (!store) return;
    for (size_t i = 0; i < store->count; i++) record_cleanup(&store->items[i]);
    free(store->items);
    free(store);
}

/* artifact store vtable，把内存实现绑定到 cc_artifact_store_t 端口。 */
static const cc_artifact_store_vtable_t memory_artifact_vtable = {
    memory_artifact_put,
    memory_artifact_get,
    memory_artifact_list,
    memory_artifact_remove,
    memory_artifact_destroy
};

/*
 * 创建内存 artifact store。
 *
 * 成功后 out_store 获得 self/vtable；该实现不落盘，适合测试和小型运行时缓存。
 */
cc_result_t cc_memory_artifact_store_create(cc_artifact_store_t *out_store)
{
    if (!out_store) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null artifact store output");
    }
    memset(out_store, 0, sizeof(*out_store));
    memory_artifact_store_t *store = calloc(1, sizeof(memory_artifact_store_t));
    if (!store) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate artifact store");
    }
    out_store->self = store;
    out_store->vtable = &memory_artifact_vtable;
    return cc_result_ok();
}
