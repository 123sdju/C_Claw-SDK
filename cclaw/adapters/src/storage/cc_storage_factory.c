



#include "cc/ports/cc_storage_factory.h"
#include "cc/ports/cc_platform.h"
#include "cc/adapters/cc_json_file_session_store.h"
#include <string.h>

#ifndef CC_DEFAULT_STORAGE_PATH
#define CC_DEFAULT_STORAGE_PATH "runtime/data/sessions"
#endif

#ifndef CC_DEFAULT_JSON_FILE_STORAGE_PATH
#define CC_DEFAULT_JSON_FILE_STORAGE_PATH "runtime/data/sessions.json"
#endif

/** @brief 各后端声明跟随裁剪宏，未编译后端的选择返回 UNSUPPORTED。 */
#if CC_STORAGE_SQLITE

/**
 * @brief 打开或创建 SQLite 会话库，具体初始化和失败清理由后端实现。
 * @param[in] db_path 调用期间借用数据库路径，NULL 使用 SQLite 后端默认值。
 * @param[out] out_store 新拥有槽，成功交付后由调用者排空并 destroy。
 * @return 后端创建结果，失败不转交实例，拥有诊断交调用者。
 */
cc_result_t cc_sqlite_session_store_create(const char *db_path, cc_session_store_t *out_store);
#endif

#if CC_STORAGE_JSON_SEGMENTED
/**
 * @brief 创建 JSON 分段会话后端，恢复与路径检查由该后端完成。
 * @param[in] root_path 调用期借用根目录，本工厂已选择配置值或编译默认值。
 * @param[in] segment_bytes 分段字节阈值，零值由后端解释。
 * @param[in] media_dir 调用期借用媒体子目录，可为 NULL，默认由后端选择。
 * @param[in] max_base64_bytes 上下文媒体编码字节预算，零值由后端解释。
 * @param[out] out_store 新拥有槽，成功后由调用者排空并 destroy。
 * @return 后端创建结果，失败不转交实例，拥有诊断交调用者。
 */
cc_result_t cc_json_segmented_store_create(
    const char *root_path,
    size_t segment_bytes,
    const char *media_dir,
    size_t max_base64_bytes,
    cc_session_store_t *out_store
);
#endif

#if CC_STORAGE_MEMORY
/**
 * @brief 创建带内部 mutex 的内存会话后端。
 * @param[out] out_store 新拥有槽，成功后由调用者排空并 destroy。
 * @return 后端创建结果，失败归还局部资源，拥有诊断交调用者。
 */
cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);
#endif

/**
 * @brief 按准确配置名称选择一个已编译的会话后端并交付其拥有实例。
 * @param[in] config 非 NULL 调用期稳定配置，字符串只在创建期间借用。
 * @param[out] out_store 必填新拥有槽，先清零，成功后调用者排空并通过 vtable destroy。
 * @return 成功为 CC_OK，参数/未知类型为 INVALID_ARGUMENT，禁用后端为 UNSUPPORTED，创建失败原样返回。
 * @note local_file/json 均选择单文件 JSON，json_segmented 使用分段目录。
 * SQLite 失败直接上报，后端负责局部清理，工厂不执行跨后端回退或 BSP 挂载。
 */
cc_result_t cc_storage_factory_create_store(
    const cc_config_t *config,
    cc_session_store_t *out_store
)
{
    if (!out_store)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null config or out_store");


    memset(out_store, 0, sizeof(cc_session_store_t));
    if (!config)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null config or out_store");


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
#if CC_STORAGE_JSON_SEGMENTED
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
