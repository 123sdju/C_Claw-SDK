#include "cc/ports/cc_secret_store.h"
#include "cc/internal/cc_alloc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>

#include <stdlib.h>
#include <string.h>

/**
 * @brief 检查引用前缀并借出其非空正文。
 * @param[in] reference 借用的 NUL 结尾引用，可为 NULL。
 * @param[in] scheme 必填的 NUL 结尾前缀，当前调用点传入常量。
 * @return 匹配时返回 reference 内借用地址，缺失或前缀不匹配返回 NULL。
 * @note 返回值的寿命随 reference，比较区分大小写。
 */
static const char *reference_value(const char *reference, const char *scheme)
{
    size_t length = strlen(scheme);
    if (!reference || strncmp(reference, scheme, length) != 0 || !reference[length]) return NULL;
    return reference + length;
}

/**
 * @brief 复制环境变量或 Windows 通用凭据的正文。
 * @param[in,out] self 静态后端占位，当前实现忽略。
 * @param[in] reference 借用的 env:NAME 或 cred:TARGET 引用，正文必须非空。
 * @param[out] out_secret 必填空槽，成功取得独占的 NUL 结尾分配并最终 cc_secret_free。
 * @return 成功为 CC_OK，参数、缺失、原生读取或分配失败返回对应拥有结果。
 * @note 原生凭据对象在返回前 CredFree。凭据 blob 按原始字节复制，strlen 型清除只覆盖首个 NUL 前正文。
 * 环境变量读取须由调用者协调并发修改，所有拥有诊断由调用者释放。
 */
static cc_result_t windows_secret_get(void *self, const char *reference, char **out_secret)
{
    (void)self;
    if (!reference || !out_secret) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid secret lookup");
    *out_secret = NULL;
    const char *environment_name = reference_value(reference, "env:");
    if (environment_name) {
        const char *value = getenv(environment_name);
        if (!value || !value[0]) return cc_result_error(CC_ERR_NOT_FOUND, "Environment secret is unset");
        *out_secret = cc_copy_string(value);
        return *out_secret ? cc_result_ok()
                           : cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy environment secret");
    }

    const char *target = reference_value(reference, "cred:");
    if (!target) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,
                               "Secret reference must use env:NAME or cred:TARGET");
    }
    PCREDENTIALA credential = NULL;
    if (!CredReadA(target, CRED_TYPE_GENERIC, 0, &credential)) {
        return cc_result_error(GetLastError() == ERROR_NOT_FOUND ? CC_ERR_NOT_FOUND : CC_ERR_IO,
                               "Windows Credential Manager read failed");
    }
    size_t size = (size_t)credential->CredentialBlobSize;
    char *copy = malloc(size + 1);
    if (!copy) {
        CredFree(credential);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate credential buffer");
    }
    if (size > 0) memcpy(copy, credential->CredentialBlob, size);
    copy[size] = '\0';
    CredFree(credential);
    *out_secret = copy;
    return cc_result_ok();
}

/**
 * @brief 将文本同步写入 Windows 通用凭据并采用本机持久化范围。
 * @param[in,out] self 静态后端占位，当前实现忽略。
 * @param[in] reference 调用期间借用的 cred:TARGET 引用。
 * @param[in] secret 借用的 NUL 结尾文本，长度受原生凭据 blob 上限约束。
 * @return 成功为 CC_OK，参数、长度或原生失败返回对应拥有结果，调用者释放诊断。
 * @note 原生调用期间借用输入，不保留地址，同一目标的业务顺序由使用方协调。
 */
static cc_result_t windows_secret_set(void *self, const char *reference, const char *secret)
{
    (void)self;
    const char *target = reference_value(reference, "cred:");
    if (!target || !secret) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,
                               "Writable Windows secrets must use cred:TARGET");
    }
    size_t size = strlen(secret);
    if (size > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "Credential exceeds Windows size limit");
    }
    CREDENTIALA credential;
    memset(&credential, 0, sizeof(credential));
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = (LPSTR)target;
    credential.CredentialBlobSize = (DWORD)size;
    credential.CredentialBlob = (LPBYTE)secret;
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = "CClaw";
    if (!CredWriteA(&credential, 0)) {
        return cc_result_error(CC_ERR_IO, "Windows Credential Manager write failed");
    }
    return cc_result_ok();
}

/**
 * @brief 删除 cred:TARGET 对应的 Windows 通用凭据。
 * @param[in,out] self 静态后端占位，当前实现忽略。
 * @param[in] reference 调用期间借用的 cred:TARGET 引用。
 * @return 成功为 CC_OK，目标缺失为 NOT_FOUND，参数或原生失败返回对应拥有结果。
 * @note 拥有诊断由调用者释放，同一目标的业务顺序由使用方协调。
 */
static cc_result_t windows_secret_remove(void *self, const char *reference)
{
    (void)self;
    const char *target = reference_value(reference, "cred:");
    if (!target) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,
                               "Removable Windows secrets must use cred:TARGET");
    }
    if (!CredDeleteA(target, CRED_TYPE_GENERIC, 0)) {
        return cc_result_error(GetLastError() == ERROR_NOT_FOUND ? CC_ERR_NOT_FOUND : CC_ERR_IO,
                               "Windows Credential Manager delete failed");
    }
    return cc_result_ok();
}

/**
 * @brief 结束静态 Windows 凭据后端描述符的使用。
 * @param[in,out] self 忽略的占位，默认工厂交付 NULL。
 * @post 静态方法表由模块继续持有，调用者自行丢弃描述符。
 */
static void windows_secret_destroy(void *self) { (void)self; }

static const cc_secret_store_vtable_t windows_secret_vtable = {
    .size = sizeof(cc_secret_store_vtable_t),
    .version = CC_SECRET_STORE_VTABLE_VERSION,
    .capabilities = CC_SECRET_CAP_READ | CC_SECRET_CAP_WRITE |
                    CC_SECRET_CAP_DELETE | CC_SECRET_CAP_SECURE_AT_REST,
    .get = windows_secret_get,
    .set = windows_secret_set,
    .remove = windows_secret_remove,
    .destroy = windows_secret_destroy,
};

/**
 * @brief 构造 Windows 环境查询与凭据管理器后端描述符。
 * @param[out] out_store 必填空描述符，成功取得静态方法表与能力位，self 为 NULL。
 * @return 成功为 CC_OK，空输出为 INVALID_ARGUMENT，拥有诊断由调用者释放。
 * @note SECURE_AT_REST 描述 cred 存储能力，env 引用的内容由进程环境拥有者管理。
 * 工厂无状态分配，复用输出前须先结束旧描述符生命周期。
 */
cc_result_t cc_secret_store_get_default(cc_secret_store_t *out_store)
{
    if (!out_store) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null secret store output");
    memset(out_store, 0, sizeof(*out_store));
    out_store->vtable = &windows_secret_vtable;
    out_store->size = sizeof(*out_store);
    out_store->version = windows_secret_vtable.version;
    out_store->capabilities = windows_secret_vtable.capabilities;
    return cc_result_ok();
}
