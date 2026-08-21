#include "cc/ports/cc_storage_factory.h"

#include <string.h>

/*
 * 函数 main：实现 cclaw/tests/adapters/test_storage_factory_failfast.c 中的 main 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
int main(void)
{
    cc_config_t config;
    memset(&config, 0, sizeof(config));
    config.storage_type = "definitely_unknown";
    config.storage_path = "unused";

    cc_session_store_t store;
    memset(&store, 0, sizeof(store));
    cc_result_t rc = cc_storage_factory_create_store(&config, &store);
    int failed = rc.code == CC_OK || store.vtable != NULL || store.self != NULL;
    cc_result_free(&rc);
    return failed ? 1 : 0;
}
