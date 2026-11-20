#ifndef CC_JSON_FIELDS_H
#define CC_JSON_FIELDS_H

#include "cc/util/cc_json.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 查找已知业务字段的忽略大小写重复，未知字段保持由调用者解释。
 * @param[in] object 调用期间借用的有效对象，整个检查期间不得修改。
 * @param[in] fields 借用的已知字段名数组，每项为非 NULL 字符串，count 为零时可为 NULL。
 * @param[in] count 已知字段名数量，各名称按忽略大小写语义互不重复。
 * @return 首个发现重复的 fields 项借用，无重复时 NULL，借用寿命与 fields 中字符串一致。
 * @note 无分配，按字段表顺序检查，复杂度为字段数乘对象成员数，不限制未知字段数量。
 */
const char *cc_json_duplicate_known_field(
    const cc_json_value_t *object,
    const char *const *fields,
    size_t count);

#ifdef __cplusplus
}
#endif

#endif
