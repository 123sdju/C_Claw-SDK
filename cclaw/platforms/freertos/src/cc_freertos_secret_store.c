#include "cc/ports/cc_secret_store.h"

#include <string.h>

cc_result_t cc_secret_store_get_default(cc_secret_store_t *out_store)
{
    if (!out_store) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null secret store output");
    memset(out_store, 0, sizeof(*out_store));
    return cc_result_error(CC_ERR_UNSUPPORTED,
                           "This FreeRTOS profile has no configured credential backend");
}
