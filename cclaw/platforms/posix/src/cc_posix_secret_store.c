#include "cc/ports/cc_secret_store.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief 复制 env:NAME 指定的非空环境变量。
 * @param[in,out] self 静态后端占位，可为 NULL，当前实现忽略。
 * @param[in] reference 调用期间借用的 NUL 结尾引用，NAME 必须非空。
 * @param[out] out_secret 必填空槽，成功取得独占副本并最终 cc_secret_free。
 * @return 成功为 CC_OK，参数、未设置或分配失败返回对应拥有结果，调用者释放诊断。
 * @note 参数验证后先清空输出。调用者须协调并发环境变量修改，副本产生后可独立持有。
 */
static cc_result_t env_get(void *self, const char *reference, char **out_secret)
{
    (void)self;
    if (!reference || !out_secret || strncmp(reference, "env:", 4) != 0 || !reference[4]) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Secret reference must use env:NAME");
    }
    *out_secret = NULL;
    const char *value = getenv(reference + 4);
    if (!value || !value[0]) return cc_result_error(CC_ERR_NOT_FOUND, "Environment secret is unset");
    *out_secret = cc_copy_string(value);
    return *out_secret ? cc_result_ok()
                       : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy environment secret");
}

/**
 * @brief 返回 POSIX 环境后端的只读能力限制。
 * @param[in,out] self 忽略的后端占位。
 * @param[in] reference 忽略的借用引用，可为 NULL。
 * @param[in] secret 忽略的借用正文，可为 NULL。
 * @return 始终返回 UNSUPPORTED，拥有诊断由调用者释放。
 */
static cc_result_t env_set(void *self, const char *reference, const char *secret)
{
    (void)self; (void)reference; (void)secret;
    return cc_result_error(CC_ERR_UNSUPPORTED,
                           "The POSIX environment secret backend is read-only");
}

/**
 * @brief 返回 POSIX 环境后端的删除能力限制。
 * @param[in,out] self 忽略的后端占位。
 * @param[in] reference 忽略的借用引用，可为 NULL。
 * @return 始终返回 UNSUPPORTED，拥有诊断由调用者释放。
 */
static cc_result_t env_remove(void *self, const char *reference)
{
    (void)self; (void)reference;
    return cc_result_error(CC_ERR_UNSUPPORTED,
                           "The POSIX environment secret backend is read-only");
}

/**
 * @brief 结束静态环境后端描述符的使用。
 * @param[in,out] self 忽略的占位，默认工厂交付 NULL。
 * @post 静态方法表继续由模块持有，调用者自行丢弃描述符。
 */
static void env_destroy(void *self) { (void)self; }

static const cc_secret_store_vtable_t env_vtable = {
    .size = sizeof(cc_secret_store_vtable_t),
    .version = CC_SECRET_STORE_VTABLE_VERSION,
    .capabilities = CC_SECRET_CAP_READ,
    .get = env_get,
    .set = env_set,
    .remove = env_remove,
    .destroy = env_destroy,
};

/**
 * @brief 构造只读 POSIX 环境后端描述符。
 * @param[out] out_store 必填空描述符，成功取得静态方法表及 READ 能力，self 为 NULL。
 * @return 成功为 CC_OK，空输出为 INVALID_ARGUMENT，拥有诊断由调用者释放。
 * @note 工厂无状态分配，各次调用的描述符可独立使用；复用输出前须先结束旧描述符生命周期。
 */
cc_result_t cc_secret_store_get_default(cc_secret_store_t *out_store)
{
    if (!out_store) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null secret store output");
    memset(out_store, 0, sizeof(*out_store));
    out_store->vtable = &env_vtable;
    out_store->size = sizeof(*out_store);
    out_store->version = env_vtable.version;
    out_store->capabilities = env_vtable.capabilities;
    return cc_result_ok();
}
