

#include "cc/app/cc_cancel_token.h"

/**
 * @brief 验证取消源创建、未取消快照及重复取消的幂等状态。
 * @return 成功为零，创建、令牌或状态断言失败为一，成功及令牌检查失败均释放源。
 * @note token 只借用到 source 销毁，本夹具串行运行，销毁后不再查询 token。
 */
int main(void)
{
    cc_cancel_source_t *source = NULL;
    cc_result_t rc = cc_cancel_source_create(&source);
    if (rc.code != CC_OK || !source) {
        cc_result_free(&rc);
        return 1;
    }
    cc_result_free(&rc);

    cc_cancel_token_t *token = cc_cancel_source_token(source);
    if (!token) {
        cc_cancel_source_destroy(source);
        return 1;
    }
    if (cc_cancel_token_is_cancelled(token)) {
        cc_cancel_source_destroy(source);
        return 1;
    }
    cc_cancel_source_cancel(source);
    cc_cancel_source_cancel(source);
    int cancelled = cc_cancel_token_is_cancelled(token);
    cc_cancel_source_destroy(source);
    return cancelled ? 0 : 1;
}
