#include "cc/ports/cc_secret_store.h"

#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

typedef struct cc_esp32_secret_store {
    uint64_t capabilities;
} cc_esp32_secret_store_t;

/**
 * @brief 将可选 nvs 前缀的引用映射到一至十五字节的原生键。
 * @param[in] reference 调用期间借用的 NUL 结尾引用，接受 nvs:KEY 或裸键。
 * @param[out] out_key 成功返回 reference 内的借用地址，失败保留原槽值。
 * @return 成功为 CC_OK，空参数或长度无效为 INVALID_ARGUMENT，拥有诊断由调用者释放。
 * @note 使用期间 reference 保持有效且稳定，函数不分配键副本。
 */
static cc_result_t secret_key(const char *reference, const char **out_key)
{
    if (!reference || !out_key) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid secret reference");
    const char *key = strncmp(reference, "nvs:", 4) == 0 ? reference + 4 : reference;
    size_t length = strlen(key);
    if (length == 0 || length > 15) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "NVS secret key must be 1..15 bytes");
    }
    *out_key = key;
    return cc_result_ok();
}

/**
 * @brief 在 cclaw_sec 命名空间测量并读取一个拥有的字符串。
 * @param[in,out] self 借用存活后端，当前读取只使用原生 NVS 状态。
 * @param[in] reference 调用期间借用的引用，映射规则由 secret_key 定义。
 * @param[out] out_secret 必填空槽，成功取得独占字符串并最终 cc_secret_free，失败为 NULL。
 * @return 成功为 CC_OK，键缺失为 NOT_FOUND，分配失败为 OUT_OF_MEMORY，原生读取失败为 STORAGE。
 * 拥有诊断由调用者释放。
 * @note 测量和读取间允许原生值变化，失败清除已分配容量并关闭句柄；实例寿命由外层保证。
 */
static cc_result_t nvs_secret_get(void *self, const char *reference, char **out_secret)
{
    (void)self;
    if (!out_secret) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null secret output");
    *out_secret = NULL;
    const char *key = NULL;
    cc_result_t rc = secret_key(reference, &key);
    if (rc.code != CC_OK) return rc;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("cclaw_sec", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return cc_result_error(CC_ERR_NOT_FOUND, "Secret namespace is empty");
    if (err != ESP_OK) return cc_result_error(CC_ERR_STORAGE, "Cannot open secret namespace");
    size_t length = 0;
    err = nvs_get_str(handle, key, NULL, &length);
    if (err != ESP_OK || length == 0) {
        nvs_close(handle);
        return err == ESP_ERR_NVS_NOT_FOUND
            ? cc_result_error(CC_ERR_NOT_FOUND, "Secret reference was not found")
            : cc_result_error(CC_ERR_STORAGE, "Cannot measure secret value");
    }
    size_t capacity = length;
    char *value = malloc(capacity);
    if (!value) {
        nvs_close(handle);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate secret value");
    }
    err = nvs_get_str(handle, key, value, &length);
    nvs_close(handle);
    if (err != ESP_OK) {
        cc_secret_zero(value, capacity);
        free(value);
        return err == ESP_ERR_NVS_NOT_FOUND
            ? cc_result_error(CC_ERR_NOT_FOUND, "Secret reference was not found")
            : cc_result_error(CC_ERR_STORAGE, "Cannot read secret value");
    }
    *out_secret = value;
    return cc_result_ok();
}

/**
 * @brief 同步设置命名空间中的文本并提交 NVS。
 * @param[in,out] self 借用存活后端，当前方法使用全局 NVS 命名空间。
 * @param[in] reference 调用期间借用的平台引用。
 * @param[in] secret 调用期间借用的 NUL 结尾正文，须非 NULL。
 * @return 成功为 CC_OK，参数错误或存储失败为对应拥有结果，调用者释放诊断。
 * @note set/commit 由原生后端执行，失败后使用方可重新读取核对最终值，同键业务事务由使用方同步。
 */
static cc_result_t nvs_secret_set(void *self, const char *reference, const char *secret)
{
    (void)self;
    if (!secret) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null secret value");
    const char *key = NULL;
    cc_result_t rc = secret_key(reference, &key);
    if (rc.code != CC_OK) return rc;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("cclaw_sec", NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_set_str(handle, key, secret);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle) nvs_close(handle);
    return err == ESP_OK ? cc_result_ok()
                         : cc_result_error(CC_ERR_STORAGE, "Cannot persist secret value");
}

/**
 * @brief 删除指定 NVS 键并提交，命名空间或键缺失时按完成处理。
 * @param[in,out] self 借用存活后端。
 * @param[in] reference 调用期间借用的平台引用。
 * @return 成功为 CC_OK，参数或原生失败返回对应拥有结果，调用者释放诊断。
 * @note 每条原生调用同步返回，跨调用的同键业务顺序由使用方协调。
 */
static cc_result_t nvs_secret_remove(void *self, const char *reference)
{
    (void)self;
    const char *key = NULL;
    cc_result_t rc = secret_key(reference, &key);
    if (rc.code != CC_OK) return rc;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("cclaw_sec", NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return cc_result_ok();
    if (err != ESP_OK) return cc_result_error(CC_ERR_STORAGE, "Cannot open secret namespace");
    if (err == ESP_OK) err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle) nvs_close(handle);
    return err == ESP_OK ? cc_result_ok()
                         : cc_result_error(CC_ERR_STORAGE, "Cannot remove secret value");
}

/**
 * @brief 消费工厂创建的后端状态。
 * @param[in,out] self 独占的堆状态，可为 NULL。
 * @pre 拥有者已关闭入口并排空全部在途方法。
 * @post 状态分配已释放，旧 self 及其别名失效。
 */
static void nvs_secret_destroy(void *self) { free(self); }

static const cc_secret_store_vtable_t nvs_secret_vtable = {
    .size = sizeof(cc_secret_store_vtable_t),
    .version = CC_SECRET_STORE_VTABLE_VERSION,
    .capabilities = CC_SECRET_CAP_READ | CC_SECRET_CAP_WRITE | CC_SECRET_CAP_DELETE,
    .get = nvs_secret_get,
    .set = nvs_secret_set,
    .remove = nvs_secret_remove,
    .destroy = nvs_secret_destroy,
};

/**
 * @brief 初始化默认 NVS 并创建具有基础读写删除能力的后端。
 * @param[out] out_store 必填空描述符，成功交付拥有状态，调用者排空后通过 vtable destroy 释放。
 * @return 成功为 CC_OK，参数、初始化或分配失败返回对应拥有结果，调用者释放诊断。
 * @note 初始化错误保留 NVS 内容并返回修复提示。实例报告 READ/WRITE/DELETE 基础能力，
 * 实际分区加密状态由 BSP 的 NVS 初始化配置负责。
 */
cc_result_t cc_secret_store_get_default(cc_secret_store_t *out_store)
{
    if (!out_store) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null secret store output");
    memset(out_store, 0, sizeof(*out_store));
    esp_err_t init_err = nvs_flash_init();
    if (init_err != ESP_OK) {
        return cc_result_errf(CC_ERR_STORAGE,
            "NVS secret storage unavailable (%s); data preserved, repair is required",
            esp_err_to_name(init_err));
    }
    cc_esp32_secret_store_t *store = calloc(1, sizeof(*store));
    if (!store) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate secret store");
    store->capabilities = nvs_secret_vtable.capabilities;
    out_store->self = store;
    out_store->vtable = &nvs_secret_vtable;
    out_store->size = sizeof(*out_store);
    out_store->version = nvs_secret_vtable.version;
    out_store->capabilities = store->capabilities;
    return cc_result_ok();
}
