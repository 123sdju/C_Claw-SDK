#include "cc/core/cc_result.h"

#include <string.h>

/*
 * 验证 cc_result_with_detail 会深拷贝结构化错误 detail。
 *
 * provider 429/5xx 等错误会通过 detail 暴露 http_status、retry_after 和脱敏 body；本测试
 * 确认调用方释放原 detail 后 result 仍拥有自己的副本。
 */
int main(void)
{
    cc_error_detail_t detail;
    memset(&detail, 0, sizeof(detail));
    detail.size = sizeof(detail);
    detail.http_status = 429;
    detail.retry_after_ms = 2000;
    detail.recoverable = 1;
    detail.error_code = "rate_limit";
    detail.provider_error_code = "too_many_requests";
    detail.raw_redacted_body = "{\"token\":\"[REDACTED]\"}";

    cc_result_t result = cc_result_with_detail(
        CC_ERR_RATE_LIMIT,
        "rate limited",
        &detail);

    int failed = 0;
    if (result.size != sizeof(result)) failed = 1;
    if (result.code != CC_ERR_RATE_LIMIT) failed = 1;
    if (!result.detail) failed = 1;
    if (result.detail && result.detail->http_status != 429) failed = 1;
    if (result.detail && result.detail->retry_after_ms != 2000) failed = 1;
    if (result.detail && !result.detail->recoverable) failed = 1;
    if (result.detail && strcmp(result.detail->provider_error_code, "too_many_requests") != 0) failed = 1;
    if (result.detail && strcmp(result.detail->raw_redacted_body, "{\"token\":\"[REDACTED]\"}") != 0) failed = 1;

    cc_result_free(&result);
    return failed ? 1 : 0;
}
