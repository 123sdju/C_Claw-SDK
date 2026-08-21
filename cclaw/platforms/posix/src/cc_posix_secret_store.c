#include "cc/ports/cc_secret_store.h"
#include "cc/internal/cc_alloc.h"

#include <stdlib.h>
#include <string.h>

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

static cc_result_t env_set(void *self, const char *reference, const char *secret)
{
    (void)self; (void)reference; (void)secret;
    return cc_result_error(CC_ERR_UNSUPPORTED,
                           "The POSIX environment secret backend is read-only");
}

static cc_result_t env_remove(void *self, const char *reference)
{
    (void)self; (void)reference;
    return cc_result_error(CC_ERR_UNSUPPORTED,
                           "The POSIX environment secret backend is read-only");
}

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
