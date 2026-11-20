



add_library(c_claw_ports_headers INTERFACE)
target_include_directories(c_claw_ports_headers INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
#[[
  @file 发布平台无关 port 头接口，分别设置构建树与安装树的包含路径。
  此目录只创建 interface 目标，具体 port 对象由所选平台目录提供。
]]
