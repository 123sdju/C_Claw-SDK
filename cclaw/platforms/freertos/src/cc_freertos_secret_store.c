#include "cc/ports/cc_secret_store.h"

#include <string.h>

/**
 * @brief 返回通用 FreeRTOS profile 尚未配置凭据后端的状态。
 * @param[out] out_store 必填空描述符，有效时清零且保持无资源状态。
 * @return 有效输出返回 UNSUPPORTED，空输出返回 INVALID_ARGUMENT，拥有诊断由调用者释放。
 * @note BSP 可提供适合自身持久化能力的实现，默认入口无共享可变状态。
 */
cc_result_t cc_secret_store_get_default(cc_secret_store_t *out_store)
{
    if (!out_store) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null secret store output");
    memset(out_store, 0, sizeof(*out_store));
    return cc_result_error(CC_ERR_UNSUPPORTED,
                           "This FreeRTOS profile has no configured credential backend");
}
