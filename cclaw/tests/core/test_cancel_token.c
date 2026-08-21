

#include "cc/app/cc_cancel_token.h"

/*
 * 验证 cancel source/token 的基本生命周期。
 *
 * token 是 source 的借用视图；cancel 可重复调用且应保持幂等，source 销毁负责释放 token。
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
