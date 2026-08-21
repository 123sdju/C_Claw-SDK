#include "cc/ports/cc_secret_store.h"
#include "cc/internal/cc_alloc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>

#include <stdlib.h>
#include <string.h>

static const char *reference_value(const char *reference, const char *scheme)
{
    size_t length = strlen(scheme);
    if (!reference || strncmp(reference, scheme, length) != 0 || !reference[length]) return NULL;
    return reference + length;
}

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
