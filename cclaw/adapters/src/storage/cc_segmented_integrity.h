#ifndef CC_SEGMENTED_INTEGRITY_H
#define CC_SEGMENTED_INTEGRITY_H

#include "cc/core/cc_result.h"
#include "cc_json_syntax_check.h"
#include <string.h>

/* 仅对成功读取后证实无效的历史数据附加此标记。
 * 文件系统、分配、并发变化失败各自向上传播，降级对话依赖确证的数据损坏。 */
#define CC_SEGMENTED_INTEGRITY_ERROR "segmented_history_integrity"

/**
 * @brief 构造带分段历史完整性标记的存储错误。
 * @param[in] message 诊断文本借用，由结果构造器复制，允许 NULL。
 * @return 拥有诊断及详情的结果值，调用者调用 cc_result_free 释放。
 * @note 详情中的静态标记经构造器复制。详情分配失败时按核心结果契约降级。
 */
static inline cc_result_t cc_segmented_integrity_error(const char *message)
{
    cc_error_detail_t detail = {0};
    detail.size = sizeof(detail);
    detail.error_code = (char *)CC_SEGMENTED_INTEGRITY_ERROR;
    return cc_result_with_detail(CC_ERR_STORAGE, message, &detail);
}

/**
 * @brief 检查一个结果是否带有确证的分段历史损坏标记。
 * @param[in] result 只读结果借用，允许 NULL，调用期间禁止并发修改或释放。
 * @return 同时满足存储错误与完整性标记时为一，否则为零。
 */
static inline int cc_segmented_is_integrity_error(const cc_result_t *result)
{
    return result && result->code == CC_ERR_STORAGE && result->detail &&
        result->detail->error_code &&
        strcmp(result->detail->error_code, CC_SEGMENTED_INTEGRITY_ERROR) == 0;
}

/**
 * @brief 用无分配语法检查区分 JSON 数据损坏和解析器的资源失败。
 * @param[in] result 按值接管的解析结果，转换时释放旧诊断，否则原样交还。
 * @param[in] text 同次解析的 NUL 结尾文本借用，语法检查期间保持有效。
 * @return 语法确证无效时返回完整性错误，其他情况保留原结果，调用者释放诊断。
 * @note 无共享可变状态，调用者保证 text 与 result 的生命周期覆盖调用。
 */
static inline cc_result_t cc_segmented_classify_json_failure(cc_result_t result,
                                                              const char *text)
{
    if (result.code == CC_ERR_JSON && cc_json_syntax_check(text) == CC_JSON_SYNTAX_INVALID) {
        cc_result_free(&result);
        return cc_segmented_integrity_error("Session history contains malformed JSON");
    }
    return result;
}

#endif
