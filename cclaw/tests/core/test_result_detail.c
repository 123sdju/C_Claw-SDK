#include "cc/core/cc_result.h"

#include <string.h>
#include <stdlib.h>

/**
 * @brief 构造拥有的错误详情并释放原字段，再核对 result 独立副本和状态数值。
 * @return 所有分配及深拷贝断言成功为零，失败为一，源详情和结果各清理一次。
 * @note 本夹具串行运行，结果复制应在原详情存活时完成，随后仅访问独立结果。
 */
int main(void)
{
    cc_error_detail_t detail;
    memset(&detail, 0, sizeof(detail));
    detail.size = sizeof(detail);
    detail.http_status = 429;
    detail.retry_after_ms = 2000;
    detail.recoverable = 1;
    detail.error_code = malloc(sizeof("rate_limit"));
    detail.provider_error_code = malloc(sizeof("too_many_requests"));
    detail.raw_redacted_body = malloc(sizeof("{\"token\":\"[REDACTED]\"}"));
    if (!detail.error_code || !detail.provider_error_code || !detail.raw_redacted_body) {
        cc_error_detail_cleanup(&detail);
        return 1;
    }
    memcpy(detail.error_code, "rate_limit", sizeof("rate_limit"));
    memcpy(detail.provider_error_code, "too_many_requests", sizeof("too_many_requests"));
    memcpy(detail.raw_redacted_body, "{\"token\":\"[REDACTED]\"}", sizeof("{\"token\":\"[REDACTED]\"}"));

    cc_result_t result = cc_result_with_detail(
        CC_ERR_RATE_LIMIT,
        "rate limited",
        &detail);

    int failed = result.detail &&
        (result.detail->error_code == detail.error_code ||
         result.detail->provider_error_code == detail.provider_error_code ||
         result.detail->raw_redacted_body == detail.raw_redacted_body);
    cc_error_detail_cleanup(&detail);
    if (result.size != sizeof(result)) failed = 1;
    if (result.code != CC_ERR_RATE_LIMIT) failed = 1;
    if (!result.detail) failed = 1;
    if (result.detail && result.detail->http_status != 429) failed = 1;
    if (result.detail && result.detail->retry_after_ms != 2000) failed = 1;
    if (result.detail && !result.detail->recoverable) failed = 1;
    if (!result.message || strcmp(result.message, "rate limited") != 0) failed = 1;
    if (result.detail && (!result.detail->error_code || strcmp(result.detail->error_code, "rate_limit") != 0)) failed = 1;
    if (result.detail && (!result.detail->provider_error_code || strcmp(result.detail->provider_error_code, "too_many_requests") != 0)) failed = 1;
    if (result.detail && (!result.detail->raw_redacted_body || strcmp(result.detail->raw_redacted_body, "{\"token\":\"[REDACTED]\"}") != 0)) failed = 1;

    cc_result_free(&result);
    return failed ? 1 : 0;
}
