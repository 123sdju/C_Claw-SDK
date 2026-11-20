#include "cc/ports/cc_artifact_store.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/** @brief 一条拥有会话过滤键和独立媒体字段的内存记录。 */
typedef struct memory_artifact_record {
    char *session_id;
    cc_media_artifact_t artifact;
} memory_artifact_record_t;

/** @brief 无内部锁的拥有数组，全部访问和销毁由调用者串行化。 */
typedef struct memory_artifact_store {
    memory_artifact_record_t *items;
    size_t count;
    size_t capacity;
} memory_artifact_store_t;

/**
 * @brief 释放一条记录的会话键和媒体字段后清零。
 * @param[in,out] record 独占拥有槽，可为 NULL，字段借用者已退出。
 * @post 非空记录清零，槽存储继续归数组 owner。
 */
static void record_cleanup(memory_artifact_record_t *record)
{
    if (!record) return;
    free(record->session_id);
    cc_media_artifact_cleanup(&record->artifact);
    memset(record, 0, sizeof(*record));
}

/**
 * @brief 为内部数组预留一个追加槽，扩容时清零新增区域。
 * @param[in,out] store 非 NULL、计数容量与实际分配相符的独占状态。
 * @return 成功为 CC_OK，容量耗尽为 LIMIT_EXCEEDED，分配失败为 OUT_OF_MEMORY。
 * @note 失败保留原数组和计数，成功扩容会使旧元素地址失效。
 */
static cc_result_t reserve_record(memory_artifact_store_t *store)
{
    if (store->count < store->capacity) return cc_result_ok();
    size_t maximum = SIZE_MAX / sizeof(*store->items);
    if (store->capacity >= maximum)
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Artifact capacity overflow");
    size_t next_capacity = store->capacity > maximum / 2 ? maximum : store->capacity * 2;
    if (next_capacity == 0) next_capacity = maximum < 8 ? maximum : 8;
    memory_artifact_record_t *next = realloc(
        store->items, next_capacity * sizeof(memory_artifact_record_t));
    if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow artifact store");
    memset(next + store->capacity, 0,
        (next_capacity - store->capacity) * sizeof(memory_artifact_record_t));
    store->items = next;
    store->capacity = next_capacity;
    return cc_result_ok();
}

/**
 * @brief 按准确媒体 ID 查找第一条记录。
 * @param[in] store 调用期稳定借用状态，须非 NULL。
 * @param[in] artifact_id 借用 NUL 字符串，须非 NULL。
 * @return 命中下标或 store->count 哨兵，返回类型覆盖完整数组下标。
 */
static size_t find_record(const memory_artifact_store_t *store, const char *artifact_id)
{
    for (size_t i = 0; i < store->count; i++) {
        if (store->items[i].artifact.id &&
            strcmp(store->items[i].artifact.id, artifact_id) == 0) {
            return i;
        }
    }
    return store->count;
}

/**
 * @brief 完整复制新记录后按全局 ID 替换或追加媒体元数据。
 * @param[in,out] self 非 NULL 存活状态，调用者排除所有并发访问。
 * @param[in] session_id 可空会话过滤键，调用期间稳定借用，空串作为实际键保存。
 * @param[in] artifact 非 NULL、具有非 NULL id 的稳定输入，字段可借自旧记录。
 * @return 成功为 CC_OK，空参数为 INVALID_ARGUMENT，复制或扩容错误原样返回。
 * @note 失败保留原记录和 count，成功后原输入仍归调用者，替换可更改关联会话。
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

    size_t existing = find_record(store, artifact->id);
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

    if (existing < store->count) {
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

/**
 * @brief 取得指定媒体 ID 的独立元数据副本。
 * @param[in] self 非 NULL 存活状态，调用者保持记录稳定。
 * @param[in] artifact_id 非 NULL 调用期借用 ID，空串可匹配旧空 ID。
 * @param[out] out_artifact 非 NULL 新或已清理对象，先初始化，成功后调用者 cleanup。
 * @return 参数错误为 INVALID_ARGUMENT，缺失为 NOT_FOUND，字段复制失败返回对应结果。
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
    cc_media_artifact_init(out_artifact);
    if (!store || !artifact_id)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid artifact get argument");
    size_t index = find_record(store, artifact_id);
    if (index == store->count) {
        return cc_result_error(CC_ERR_NOT_FOUND, "Artifact not found");
    }
    return cc_media_artifact_copy(&store->items[index].artifact, out_artifact);
}

/**
 * @brief 按插入顺序复制全部或指定会话媒体，替换保持原位置。
 * @param[in] self 非 NULL 稳定状态，无内部锁。
 * @param[in] session_id 可空调用期过滤键，NULL 选全部，空串只匹配空串。
 * @param[out] out_artifacts 非 NULL 新/已清理包装，先清空，成功后调用者 list_cleanup。
 * @return 参数错误为 INVALID_ARGUMENT，复制失败撤销整列表并返回对应错误。
 */
static cc_result_t memory_artifact_list(
    void *self,
    const char *session_id,
    cc_media_artifact_list_t *out_artifacts
)
{
    memory_artifact_store_t *store = (memory_artifact_store_t *)self;
    if (!out_artifacts) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid artifact list argument");
    }
    cc_media_artifact_list_init(out_artifacts);
    if (!store) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null artifact store");
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

/**
 * @brief 删除指定 ID 的元数据并前移后续拥有记录。
 * @param[in,out] self 非 NULL 独占状态，删除前结束该项借用。
 * @param[in] artifact_id 非 NULL 调用期借用 ID，可来自被删记录。
 * @return 命中删除为 CC_OK，空参数为 INVALID_ARGUMENT，缺失为 NOT_FOUND。
 * @note 保留数组容量，清零移动后的尾槽，本层不删除 path/uri 所指内容。
 */
static cc_result_t memory_artifact_remove(void *self, const char *artifact_id)
{
    memory_artifact_store_t *store = (memory_artifact_store_t *)self;
    if (!store || !artifact_id)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid artifact remove argument");
    size_t index = find_record(store, artifact_id);
    if (index == store->count) return cc_result_error(CC_ERR_NOT_FOUND, "Artifact not found");
    record_cleanup(&store->items[index]);
    size_t pos = (size_t)index;
    for (size_t i = pos + 1; i < store->count; i++) {
        store->items[i - 1] = store->items[i];
    }
    store->count--;
    memset(&store->items[store->count], 0, sizeof(store->items[store->count]));
    return cc_result_ok();
}

/**
 * @brief 消费全部媒体、会话键、数组和后端状态。
 * @param[in,out] self 可空独占状态，owner 已关闭入口并排空全部方法及字段借用。
 * @post 非空 self 及旧别名失效，调用者持有的端口描述符须自行清空。
 */
static void memory_artifact_destroy(void *self)
{
    memory_artifact_store_t *store = (memory_artifact_store_t *)self;
    if (!store) return;
    for (size_t i = 0; i < store->count; i++) record_cleanup(&store->items[i]);
    free(store->items);
    free(store);
}

/** @brief 进程期只读方法表，实例共同借用。 */
static const cc_artifact_store_vtable_t memory_artifact_vtable = {
    memory_artifact_put,
    memory_artifact_get,
    memory_artifact_list,
    memory_artifact_remove,
    memory_artifact_destroy
};

/**
 * @brief 创建不落盘、由外层串行访问的媒体元数据后端。
 * @param[out] out_store 非 NULL 空拥有槽，先清零，成功交付 self 和静态方法表。
 * @return 成功为 CC_OK，空输出为 INVALID_ARGUMENT，状态分配失败为 OUT_OF_MEMORY。
 * @note 成功后由调用者排空入口并调用 destroy，失败输出保持空，无媒体文件创建。
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
