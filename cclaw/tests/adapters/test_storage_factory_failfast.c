#include "cc/ports/cc_storage_factory.h"

#include <string.h>

/** @brief 验证未知 storage_type 返回失败且交付空端口。
 * @return 所有断言成立为零，错误地创建实例或留下半端口为一。
 * @note 配置及字面量只同步借用，返回诊断本层释放，意外创建的实例也在返回前销毁。
 * 单线程测试，端口输出由本栈帧独占。
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
    if (store.vtable && store.vtable->destroy) store.vtable->destroy(store.self);
    return failed ? 1 : 0;
}
