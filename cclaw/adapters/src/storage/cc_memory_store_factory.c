



#include "cc/ports/cc_memory_store.h"
#include "cc/ports/cc_platform.h"
#include <stdlib.h>
#include <string.h>

#ifndef CC_DEFAULT_MEMORY_PATH
#define CC_DEFAULT_MEMORY_PATH "runtime/data/memory.json"
#endif

/*
 * no-op memory store factory。
 *
 * 在不支持长期记忆的平台上返回稳定错误，而不是构造一个会静默丢数据的 store。上层
 * runtime builder 会把该错误视作可降级能力，保持核心 agent 仍可启动。
 */
cc_result_t cc_memory_store_create_noop(cc_memory_store_t *out_store)
{
    (void)out_store;
    return cc_result_error(CC_ERR_PLATFORM, "Memory store disabled on this platform");
}

/* 内存 memory store 后端总是轻量可用，适合测试和裁剪 profile。 */
cc_result_t cc_memory_store_create_inmem(cc_memory_store_t *out_store);

/*
 * JSON/SQLite 后端按编译宏声明。
 *
 * 这样 MCU/RTOS profile 可以不链接文件系统或 SQLite 依赖，而 desktop profile 仍能启用
 * 持久化记忆。
 */
#if CC_STORAGE_JSON_FILE
cc_result_t cc_memory_store_create_json_file(cc_memory_store_t *out_store, const char *file_path);
#endif
#if CC_STORAGE_SQLITE

/*
 * SQLite 后端的 memory store 前向声明，由 cc_sqlite_memory_store.c 实现。
 * 参数: out_store - 输出 store 句柄, db_path - SQLite 数据库路径
 */
cc_result_t cc_memory_store_create_sqlite(cc_memory_store_t *out_store, const char *db_path);
#endif

/*
 * 根据 backend 名称创建 memory store。
 *
 * out_store 成功后持有后端 self/vtable；backend 支持 inmem/json_file/sqlite/noop/none。
 * path 为空时使用默认路径。该工厂只选择后端，不把 embedding/vector 能力强塞进 core，
 * 仍然保持 memory query port 与具体检索实现解耦。
 */
cc_result_t cc_memory_store_factory_create(
    cc_memory_store_t *out_store,
    const char *backend,
    const char *path
)
{
    if (!out_store || !backend)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid factory arguments");

    if (strcmp(backend, "inmem") == 0)
        return cc_memory_store_create_inmem(out_store);
    if (strcmp(backend, "json_file") == 0) {
#if CC_STORAGE_JSON_FILE
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
