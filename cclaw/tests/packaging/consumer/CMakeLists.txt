cmake_minimum_required(VERSION 3.16)
project(cclaw_consumer_test LANGUAGES C)

find_package(CClaw CONFIG REQUIRED)

add_executable(cclaw_consumer main.c)
# 从较低的调用方标准开始，验证已安装目标会传播 C11 最低要求。
set_target_properties(cclaw_consumer PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)
target_link_libraries(cclaw_consumer PRIVATE CClaw::runtime)
#[[
  @file 通过已安装的 CClaw::runtime 构建极小下游程序，校验导出包可被发现并链接。
  包搜索根由外部测试提供，所有消费者生成物位于其独立构建目录。
]]
