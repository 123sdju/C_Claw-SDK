#ifndef CC_ID_H
#define CC_ID_H

#include "cc/core/cc_result.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 生成具有可选前缀的小写 UUIDv4，随机字节由平台提供。
 * @param[in] prefix 调用期间借用的 NUL 结尾前缀，NULL 为无前缀。
 * @param[out] out_id 必填空槽，成功取得独占字符串，调用者 free，失败为 NULL。
 * @return 成功为 CC_OK，参数、随机源、容量、分配或格式化失败返回拥有诊断，调用者释放。
 * @note 输出不借用 prefix，并发能力与随机质量由所选平台随机源保证。
 */
cc_result_t cc_id_generate_uuid_v4(const char *prefix, char **out_id);

/**
 * @brief 用平台随机源生成非零的 64 位标识，全零最多重试八次。
 * @param[out] out_id 必填输出槽，成功非零，其他有效输出路径为零。
 * @return 成功为 CC_OK，空输出为 INVALID_ARGUMENT，源不可用为 UNSUPPORTED，
 * 八次全零为 PLATFORM，拥有诊断由调用者 cc_result_free。
 * @note 无共享标识计数，调用期间输出槽独占，平台随机源负责其并发条件。
 */
cc_result_t cc_id_generate_u64(uint64_t *out_id);

#ifdef __cplusplus
}
#endif

#endif
