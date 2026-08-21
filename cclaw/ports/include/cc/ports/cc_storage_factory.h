



#ifndef CC_STORAGE_FACTORY_H
#define CC_STORAGE_FACTORY_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_session_store.h"
#include "cc/util/cc_config.h"

/*
 * 根据配置创建 session store。
 *
 * config 只在调用期间借用；out_store 成功后包含 self + vtable，调用方最终通过
 * out_store.vtable->destroy 或上层封装释放。factory 只选择 SDK 存储 adapter，不引入
 * 业务数据库网关。
 */
cc_result_t cc_storage_factory_create_store(
    const cc_config_t *config,
    cc_session_store_t *out_store
);

#endif
