#include "cc/ports/cc_sandbox.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief 归还沙箱结果的两个独立拥有字符串并清零记录。
 * @param[in,out] result 可空、已初始化且独占的结果，stdout/stderr 为独立兼容 free 分配。
 * @post 两个字符串归还且结构清零，容器继续归 caller，空结果可重复清理。
 * @note 调用前 owner 等待执行与所有结果借用退出，本函数不终止进程或销毁沙箱实例。
 */
void cc_sandbox_result_free(cc_sandbox_result_t *result)
{
    if (!result) return;
    free(result->stdout_text);
    free(result->stderr_text);
    memset(result, 0, sizeof(*result));
}
