#include "cc/ports/cc_secret_store.h"

#include <stdlib.h>
#include <string.h>

#include "esp_efuse.h"
#include "nvs.h"
#include "nvs_flash.h"

typedef struct cc_esp32_secret_store {
    uint64_t capabilities;
} cc_esp32_secret_store_t;

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
    char *value = malloc(length);
    if (!value) {
        nvs_close(handle);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate secret value");
    }
    err = nvs_get_str(handle, key, value, &length);
    nvs_close(handle);
    if (err != ESP_OK) {
        cc_secret_zero(value, length);
        free(value);
        return cc_result_error(CC_ERR_STORAGE, "Cannot read secret value");
    }
    *out_secret = value;
    return cc_result_ok();
}

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

static cc_result_t nvs_secret_remove(void *self, const char *reference)
{
    (void)self;
    const char *key = NULL;
    cc_result_t rc = secret_key(reference, &key);
    if (rc.code != CC_OK) return rc;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("cclaw_sec", NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle) nvs_close(handle);
    return err == ESP_OK ? cc_result_ok()
                         : cc_result_error(CC_ERR_STORAGE, "Cannot remove secret value");
}

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

cc_result_t cc_secret_store_get_default(cc_secret_store_t *out_store)
{
    if (!out_store) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null secret store output");
    memset(out_store, 0, sizeof(*out_store));
    esp_err_t init_err = nvs_flash_init();
    if (init_err != ESP_OK) {
        return cc_result_error(CC_ERR_STORAGE, "Cannot initialize NVS secret backend");
    }
    cc_esp32_secret_store_t *store = calloc(1, sizeof(*store));
    if (!store) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate secret store");
    store->capabilities = nvs_secret_vtable.capabilities;
    if (esp_efuse_is_flash_encryption_enabled()) {
        store->capabilities |= CC_SECRET_CAP_SECURE_AT_REST;
    }
    out_store->self = store;
    out_store->vtable = &nvs_secret_vtable;
    out_store->size = sizeof(*out_store);
    out_store->version = nvs_secret_vtable.version;
    out_store->capabilities = store->capabilities;
    return cc_result_ok();
}
