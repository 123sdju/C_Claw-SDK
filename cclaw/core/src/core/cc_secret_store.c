#include "cc/ports/cc_secret_store.h"

#include <stdlib.h>
#include <string.h>

void cc_secret_zero(void *data, size_t size)
{
    volatile unsigned char *bytes = (volatile unsigned char *)data;
    while (bytes && size > 0) {
        *bytes++ = 0;
        size--;
    }
}

void cc_secret_free(char *secret)
{
    if (!secret) return;
    cc_secret_zero(secret, strlen(secret));
    free(secret);
}
