



#ifndef CC_STORAGE_FACTORY_H
#define CC_STORAGE_FACTORY_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_session_store.h"
#include "cc/util/cc_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 按配置选择已编译的 memory、单文件 JSON、分段 JSON 或 SQLite 会话后端。
 * @param[in] config 非 NULL 稳定配置，创建期借用，后端复制需长期保管的路径。
 * @param[out] out_store 必填新拥有槽，先清零，成功后 owner 排空并调用 vtable->destroy。
 * @return 成功为 CC_OK，空配置或未知名称为 INVALID_ARGUMENT，未编译后端为 UNSUPPORTED，后端失败原样返回。
 * @note 名称准确匹配，local_file 与 json 同选单文件 JSON。NULL 单文件/分段路径使用编译默认值，
 * SQLite NULL 路径交该后端解释。工厂不回退其他后端、不创建挂载，也不保证跨后端数据迁移。
 */
cc_result_t cc_storage_factory_create_store(
    const cc_config_t *config,
    cc_session_store_t *out_store
);

#ifdef __cplusplus
}
#endif

#endif
