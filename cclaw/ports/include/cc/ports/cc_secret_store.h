#ifndef CC_SECRET_STORE_H
#define CC_SECRET_STORE_H

#include "cc/core/cc_result.h"

#include <stddef.h>
#include <stdint.h>

#define CC_SECRET_STORE_VTABLE_VERSION 1U

enum {
    CC_SECRET_CAP_READ = 1ULL << 0,
    CC_SECRET_CAP_WRITE = 1ULL << 1,
    CC_SECRET_CAP_DELETE = 1ULL << 2,
    CC_SECRET_CAP_SECURE_AT_REST = 1ULL << 3,
};

typedef struct cc_secret_store_vtable cc_secret_store_vtable_t;

typedef struct cc_secret_store {
    void *self;
    const cc_secret_store_vtable_t *vtable;
    size_t size;
    unsigned version;
    uint64_t capabilities;
} cc_secret_store_t;

struct cc_secret_store_vtable {
    size_t size;
    unsigned version;
    uint64_t capabilities;

    /* out_secret is heap-owned by the caller and must be wiped before free. */
    cc_result_t (*get)(void *self, const char *reference, char **out_secret);
    cc_result_t (*set)(void *self, const char *reference, const char *secret);
    cc_result_t (*remove)(void *self, const char *reference);
    void (*destroy)(void *self);
};

/* Returns the platform credential backend. It never falls back to plaintext files. */
cc_result_t cc_secret_store_get_default(cc_secret_store_t *out_store);

/* Volatile wipe helper for temporary secret buffers. */
void cc_secret_zero(void *data, size_t size);
void cc_secret_free(char *secret);

#endif
