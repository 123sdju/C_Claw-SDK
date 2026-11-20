if(NOT DEFINED CCLAW_BUILD_DIR)
    message(FATAL_ERROR "CCLAW_BUILD_DIR is required")
endif()
if(NOT DEFINED CCLAW_SOURCE_DIR)
    message(FATAL_ERROR "CCLAW_SOURCE_DIR is required")
endif()
if(NOT DEFINED CCLAW_TEST_WORK_DIR)
    message(FATAL_ERROR "CCLAW_TEST_WORK_DIR is required")
endif()
if(NOT DEFINED CCLAW_INSTALL_LIBDIR)
    # 保留原有三个输入变量的调用方式，缺省直接读取被测试构建的安装目录。
    load_cache("${CCLAW_BUILD_DIR}" READ_WITH_PREFIX cclaw_cache_ CMAKE_INSTALL_LIBDIR)
    if(NOT DEFINED cclaw_cache_CMAKE_INSTALL_LIBDIR)
        message(FATAL_ERROR "SDK build cache does not define CMAKE_INSTALL_LIBDIR")
    endif()
    set(CCLAW_INSTALL_LIBDIR "${cclaw_cache_CMAKE_INSTALL_LIBDIR}")
endif()

set(prefix "${CCLAW_TEST_WORK_DIR}/install-prefix")
set(consumer_build "${CCLAW_TEST_WORK_DIR}/consumer-build")
if(EXISTS "${CCLAW_SOURCE_DIR}/tests/packaging/consumer")
    set(consumer_source "${CCLAW_SOURCE_DIR}/tests/packaging/consumer")
else()
    set(consumer_source "${CCLAW_SOURCE_DIR}/cclaw/tests/packaging/consumer")
endif()
file(REMOVE_RECURSE "${prefix}" "${consumer_build}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${CCLAW_BUILD_DIR}" --prefix "${prefix}"
    RESULT_VARIABLE install_rc
)
if(NOT install_rc EQUAL 0)
    message(FATAL_ERROR "C-Claw install failed: ${install_rc}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${consumer_source}"
        -B "${consumer_build}"
        "-DCMAKE_PREFIX_PATH=${prefix}"
    RESULT_VARIABLE configure_rc
)
if(NOT configure_rc EQUAL 0)
    message(FATAL_ERROR "Consumer configure failed: ${configure_rc}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}"
    RESULT_VARIABLE build_rc
)
if(NOT build_rc EQUAL 0)
    message(FATAL_ERROR "Consumer build failed: ${build_rc}")
endif()

if(IS_ABSOLUTE "${CCLAW_INSTALL_LIBDIR}")
    set(pc_file "${CCLAW_INSTALL_LIBDIR}/pkgconfig/cclaw.pc")
else()
    set(pc_file "${prefix}/${CCLAW_INSTALL_LIBDIR}/pkgconfig/cclaw.pc")
endif()
if(NOT EXISTS "${pc_file}")
    message(FATAL_ERROR "pkg-config file was not installed")
endif()
#[[
  @file 在 CCLAW_TEST_WORK_DIR 的固定子目录安装 SDK 并构建 find_package 消费者。
  输入为已有构建目录、SDK 源目录、独占工作目录及真实安装 libdir。
  install-prefix 和 consumer-build 由此脚本清理重建，调用者须为并发测试分配独立目录。
  安装、消费者配置/链接或 pc 文件检查失败时终止脚本，生成物留存供诊断。
]]
