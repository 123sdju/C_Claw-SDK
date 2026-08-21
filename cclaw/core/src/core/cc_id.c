#include "cc/core/cc_id.h"
#include "cc/ports/cc_platform.h"
#include "cc/internal/cc_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cc_result_t cc_id_generate_u64(uint64_t *out_id)
{
    if (!out_id) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null ID output");
    *out_id = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        uint64_t candidate = 0;
        if (cc_platform_random_bytes(&candidate, sizeof(candidate)) != 0) {
            return cc_result_error(CC_ERR_UNSUPPORTED,
                "Platform has no cryptographically secure random source");
        }
        if (candidate != 0) {
            *out_id = candidate;
            return cc_result_ok();
        }
    }
    return cc_result_error(CC_ERR_PLATFORM, "Secure random source returned only zero IDs");
}

cc_result_t cc_id_generate_uuid_v4(const char *prefix, char **out_id)
{
    if (!out_id) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null ID output");
    *out_id = NULL;
    unsigned char bytes[16];
    if (cc_platform_random_bytes(bytes, sizeof(bytes)) != 0) {
        return cc_result_error(CC_ERR_UNSUPPORTED,
            "Platform has no cryptographically secure random source");
    }
    bytes[6] = (unsigned char)((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = (unsigned char)((bytes[8] & 0x3fU) | 0x80U);
    const char *safe_prefix = prefix ? prefix : "";
    size_t prefix_len = strlen(safe_prefix);
    if (prefix_len > SIZE_MAX - 37U) {
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED, "ID prefix is too long");
    }
    size_t size = prefix_len + 37U;
    char *id = (char *)malloc(size);
    if (!id) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate ID");
    int written = snprintf(id, size,
        "%s%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        safe_prefix,
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
        bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]);
    if (written < 0 || (size_t)written >= size) {
        free(id);
        return cc_result_error(CC_ERR_UNKNOWN, "Failed to format UUID");
    }
    *out_id = id;
    return cc_result_ok();
}
