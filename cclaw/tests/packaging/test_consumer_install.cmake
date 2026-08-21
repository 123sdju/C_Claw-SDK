if(NOT DEFINED CCLAW_BUILD_DIR)
    message(FATAL_ERROR "CCLAW_BUILD_DIR is required")
endif()
if(NOT DEFINED CCLAW_SOURCE_DIR)
    message(FATAL_ERROR "CCLAW_SOURCE_DIR is required")
endif()
if(NOT DEFINED CCLAW_TEST_WORK_DIR)
    message(FATAL_ERROR "CCLAW_TEST_WORK_DIR is required")
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

if(NOT EXISTS "${prefix}/lib/pkgconfig/cclaw.pc" AND
   NOT EXISTS "${prefix}/lib64/pkgconfig/cclaw.pc")
    message(FATAL_ERROR "pkg-config file was not installed")
endif()
