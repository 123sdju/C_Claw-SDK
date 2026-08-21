#ifndef CC_ID_H
#define CC_ID_H

#include "cc/core/cc_result.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 生成由平台 CSPRNG 驱动的 UUIDv4，可选前缀不参与 UUID 本体。 */
cc_result_t cc_id_generate_uuid_v4(const char *prefix, char **out_id);

/* 生成非零 64 位随机 ID；不使用时间、计数器或弱随机 fallback。 */
cc_result_t cc_id_generate_u64(uint64_t *out_id);

#ifdef __cplusplus
}
#endif

#endif
