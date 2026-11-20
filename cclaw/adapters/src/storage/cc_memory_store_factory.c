



#include "cc/ports/cc_memory_store.h"
#include "cc/ports/cc_memory_tool_factory.h"
#include "cc/ports/cc_platform.h"
#include <stdlib.h>
#include <string.h>

#ifndef CC_DEFAULT_MEMORY_PATH
#define CC_DEFAULT_MEMORY_PATH "runtime/data/memory.json"
#endif

/**
 * @brief 清空输出并显式报告平台禁用记忆存储。
 * @param[out] out_store 必需新描述符，入口清零，失败保持空。
 * @return NULL 输出为 INVALID_ARGUMENT，其余为 PLATFORM，拥有诊断由调用方释放。
 */
cc_result_t cc_memory_store_create_noop(cc_memory_store_t *out_store)
{
    if (!out_store) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null memory store output");
    memset(out_store, 0, sizeof(*out_store));
    return cc_result_error(CC_ERR_PLATFORM, "Memory store disabled on this platform");
}

/** @brief 创建内存记忆后端。 @param[out] out_store 必需新描述符，成功 self 由调用者销毁。 @return 参数、分配或成功拥有结果。 */
cc_result_t cc_memory_store_create_inmem(cc_memory_store_t *out_store);

/* 持久化后端按当前功能宏引用，工厂自身只选择后端及默认路径。 */
#if CC_MEMORY_STORE_JSON_FILE
/** @brief 创建 JSON 文件后端。 @param[out] out_store 必需新描述符，成功对象由调用者销毁。 @param[in] file_path 调用期路径借用，后端复制配置。 @return 参数、文件、分配或成功拥有结果。 */
cc_result_t cc_memory_store_create_json_file(cc_memory_store_t *out_store, const char *file_path);
#endif
#if CC_STORAGE_SQLITE

/** @brief 创建 SQLite 记忆后端。 @param[out] out_store 必需新描述符，成功对象由调用者销毁。 @param[in] db_path 调用期路径借用。 @return 参数、数据库、分配或成功拥有结果。 */
cc_result_t cc_memory_store_create_sqlite(cc_memory_store_t *out_store, const char *db_path);
#endif

/**
 * @brief 按精确后端名选择工厂，直接交付创建结果。
 * @param[out] out_store 必需新描述符，入口清零，成功后拥有后端，调用方最终 destroy。
 * @param[in] backend 必需调用期借用，支持 inmem、json_file、sqlite、noop 和 none。
 * @param[in] path 调用期可选路径借用，NULL 时持久化后端使用 CC_DEFAULT_MEMORY_PATH，空串交后端解释。
 * @return 参数错误、已禁用后端错误或后端拥有结果，指定后端失败直接返回，诊断由调用方释放。
 */
cc_result_t cc_memory_store_factory_create(
    cc_memory_store_t *out_store,
    const char *backend,
    const char *path
)
{
    if (out_store) memset(out_store, 0, sizeof(*out_store));
    if (!out_store || !backend)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid factory arguments");

    if (strcmp(backend, "inmem") == 0)
        return cc_memory_store_create_inmem(out_store);
    if (strcmp(backend, "json_file") == 0) {
#if CC_MEMORY_STORE_JSON_FILE
        return cc_memory_store_create_json_file(out_store, path ? path : CC_DEFAULT_MEMORY_PATH);
#else
        return cc_result_error(CC_ERR_UNSUPPORTED, "JSON file memory backend disabled in this build");
#endif
    }
    if (strcmp(backend, "sqlite") == 0) {
#if CC_STORAGE_SQLITE
        return cc_memory_store_create_sqlite(out_store, path ? path : CC_DEFAULT_MEMORY_PATH);
#else
        return cc_result_error(CC_ERR_PLATFORM, "SQLite memory backend disabled in this build");
#endif
    }
    if (strcmp(backend, "noop") == 0 || strcmp(backend, "none") == 0)
        return cc_memory_store_create_noop(out_store);

    return cc_result_errf(CC_ERR_INVALID_ARGUMENT, "Unknown memory backend: %s", backend);
}
