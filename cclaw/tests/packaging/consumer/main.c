#include <cc/cclaw.h>

#if !defined(__cplusplus) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L)
#error "CClaw C consumers require C11 or newer"
#endif

/**
 * @brief 通过安装后的公共聚合头及导出 CMake 目标调用版本符号。
 * @return 获得常驻版本字符串借用时零，NULL 时一，无动态资源。
 * @note 独立 consumer 工程链接 CClaw::runtime，验证安装头路径、导出依赖及基础符号。
 */
int main(void)
{
    return cc_claw_version_string() ? 0 : 1;
}
