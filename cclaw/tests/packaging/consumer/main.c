#include <cc/cclaw.h>

/*
 * 最小外部 consumer 入口。
 *
 * 这个文件不是 example，而是 packaging 测试 fixture：安装后的独立 CMake 工程
 * 只通过 <cc/cclaw.h> 使用 SDK，并链接 CClaw::runtime。能调用版本函数说明
 * public include 路径、导出目标和基础符号都已正确安装。
 */
int main(void)
{
    return cc_claw_version_string() ? 0 : 1;
}
