



#include "cc/ports/cc_storage_factory.h"
#include "cc/ports/cc_platform.h"
#include <string.h>

#ifndef CC_DEFAULT_STORAGE_PATH
#define CC_DEFAULT_STORAGE_PATH "runtime/data/sessions"
#endif

#ifndef CC_DEFAULT_JSON_FILE_STORAGE_PATH
#define CC_DEFAULT_JSON_FILE_STORAGE_PATH "runtime/data/sessions.json"
#endif

/*
 * 下方 create 函数由各存储 adapter 实现。
 *
 * 这里用编译宏包住声明，避免在裁剪 profile 中引用未编译的后端。嵌入式 SDK 常用这种
 * “能力宏 + 工厂函数”的方式把 SQLite/文件系统等重量依赖从 MCU profile 中剔除。
 */
#if CC_STORAGE_SQLITE

/*
 * SQLite session store 前向声明，由 cc_sqlite_session_store.c 实现。
 * 参数: db_path - 数据库文件路径, out_store - 输出 store 句柄
 */
cc_result_t cc_sqlite_session_store_create(const char *db_path, cc_session_store_t *out_store);
#endif

/*
 * JSON 单文件 session store 前向声明，由 cc_json_file_store.c 实现。
 * 参数: file_path - JSON 文件路径, out_store - 输出 store 句柄
 */
#if CC_STORAGE_JSON_FILE
cc_result_t cc_json_file_store_create(const char *file_path, cc_session_store_t *out_store);
/*
 * JSON 分段存储 session store 前向声明，由 cc_json_segmented_store.c 实现。
 * 参数: root_path - 根目录路径, segment_bytes - 分段大小阈值, media_dir - 媒体子目录名,
 *        max_base64_bytes - 最大 base64 内联大小, out_store - 输出 store 句柄
 */
cc_result_t cc_json_segmented_store_create(
    const char *root_path,
    size_t segment_bytes,
    const char *media_dir,
    size_t max_base64_bytes,
    cc_session_store_t *out_store
);
#endif

/*
 * 纯内存 session store 前向声明，由 cc_memory_session_store.c 实现。
 * 参数: out_store - 输出 store 句柄
 */
#if CC_STORAGE_MEMORY
cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);
#endif

/*
 * 根据配置创建 session store。
 *
 * out_store 由调用方提供，成功后获得后端 self/vtable；销毁通过 vtable->destroy 完成。
 * storage_type 支持 memory/local_file/json/sqlite。SQLite 创建失败时当前策略降级到 JSON，
 * 这是运行时可用性优先的取舍；如果产品要求 fail-fast，可以在上层配置策略中收紧。
 */
cc_result_t cc_storage_factory_create_store(
    const cc_config_t *config,
    cc_session_store_t *out_store
)
{
    if (!config || !out_store)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null config or out_store");


    memset(out_store, 0, sizeof(cc_session_store_t));


    if (config->storage_type && strcmp(config->storage_type, "memory") == 0) {
#if CC_STORAGE_MEMORY
        return cc_memory_session_store_create(out_store);
#else
        return cc_result_error(CC_ERR_UNSUPPORTED, "Memory session store disabled in this build");
#endif
    }


    if (config->storage_type && strcmp(config->storage_type, "local_file") == 0) {
#if CC_STORAGE_JSON_FILE
        const char *path = config->storage_path ? config->storage_path : CC_DEFAULT_JSON_FILE_STORAGE_PATH;
        return cc_json_file_store_create(path, out_store);
#else
        return cc_result_error(CC_ERR_UNSUPPORTED, "JSON file session store disabled in this build");
#endif
    }


    if (config->storage_type && strcmp(config->storage_type, "json") == 0) {
#if CC_STORAGE_JSON_FILE
        const char *path = config->storage_path ? config->storage_path : CC_DEFAULT_JSON_FILE_STORAGE_PATH;
        return cc_json_file_store_create(path, out_store);
#else
        return cc_result_error(CC_ERR_UNSUPPORTED, "JSON file session store disabled in this build");
#endif
    }


    if (config->storage_type && strcmp(config->storage_type, "json_segmented") == 0) {
#if CC_STORAGE_JSON_FILE
        const char *path = config->storage_path ? config->storage_path : CC_DEFAULT_STORAGE_PATH;
        return cc_json_segmented_store_create(
            path,
            config->session_segment_bytes,
            config->session_media_dir,
            config->multimodal.limits.max_base64_bytes,
            out_store);
#else
        return cc_result_error(CC_ERR_UNSUPPORTED, "Segmented JSON session store disabled in this build");
#endif
    }


    if (config->storage_type && strcmp(config->storage_type, "sqlite") == 0) {
#if CC_STORAGE_SQLITE
        return cc_sqlite_session_store_create(config->storage_path, out_store);
#else
        return cc_result_error(CC_ERR_UNSUPPORTED, "SQLite session store disabled in this build");
#endif
    }



    return cc_result_errf(
        CC_ERR_INVALID_ARGUMENT,
        "Unknown session storage type: %s",
        (config->storage_type && config->storage_type[0]) ? config->storage_type : "(none)");
}
