



add_library(c_claw_ports_headers INTERFACE)
target_include_directories(c_claw_ports_headers INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
