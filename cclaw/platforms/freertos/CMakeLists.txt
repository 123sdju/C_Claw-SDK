



add_library(c_claw_platform_freertos_objects OBJECT
    src/cc_freertos_platform.c
    src/cc_freertos_thread.c
    src/cc_freertos_filesystem.c
    src/cc_freertos_path.c
    src/cc_freertos_secret_store.c
)

if(CC_NEEDS_HTTP_CLIENT)
    target_sources(c_claw_platform_freertos_objects PRIVATE
        src/cc_freertos_lwip_http_client.c
    )
endif()

if(NOT DEFINED FREERTOS_KERNEL_PATH OR NOT DEFINED FREERTOS_PORT_DIR)
    message(FATAL_ERROR
        "CC_TARGET_PLATFORM=freertos requires FREERTOS_KERNEL_PATH and FREERTOS_PORT_DIR")
endif()

target_link_libraries(c_claw_platform_freertos_objects PRIVATE
    c_claw_core_headers
    c_claw_ports_headers
    c_claw_project_options
)

target_include_directories(c_claw_platform_freertos_objects PRIVATE
    "${FREERTOS_CONFIG_INCLUDE_DIR}"
    "${FREERTOS_KERNEL_PATH}/include"
    "${FREERTOS_PORT_DIR}"
)

if(CCLAW_FREERTOS_ENABLE_FATFS)
    if(NOT DEFINED FATFS_INCLUDE_DIRS)
        message(FATAL_ERROR "CCLAW_FREERTOS_ENABLE_FATFS requires FATFS_INCLUDE_DIRS")
    endif()
    target_compile_definitions(c_claw_platform_freertos_objects PRIVATE
        CCLAW_FREERTOS_ENABLE_FATFS=1
    )
    target_include_directories(c_claw_platform_freertos_objects PRIVATE
        ${FATFS_INCLUDE_DIRS}
    )
endif()

if(CC_NEEDS_HTTP_CLIENT)
    if(NOT DEFINED LWIP_INCLUDE_DIRS)
        message(FATAL_ERROR "CC_TARGET_PLATFORM=freertos with HTTP requires LWIP_INCLUDE_DIRS")
    endif()
    target_include_directories(c_claw_platform_freertos_objects PRIVATE
        ${LWIP_INCLUDE_DIRS}
    )
    if(CCLAW_FREERTOS_ENABLE_MBEDTLS)
        if(NOT DEFINED MBEDTLS_INCLUDE_DIRS)
            message(FATAL_ERROR "CCLAW_FREERTOS_ENABLE_MBEDTLS requires MBEDTLS_INCLUDE_DIRS")
        endif()
        target_compile_definitions(c_claw_platform_freertos_objects PRIVATE
            CCLAW_FREERTOS_ENABLE_MBEDTLS=1
            MBEDTLS_CONFIG_FILE=<mbedtls_config.h>
        )
        target_include_directories(c_claw_platform_freertos_objects PRIVATE
            ${MBEDTLS_INCLUDE_DIRS}
        )
    endif()
endif()

set(CC_PLATFORM_PUBLIC_INCLUDE_DIRS "" PARENT_SCOPE)
set(CC_PLATFORM_PUBLIC_LIBRARIES "" PARENT_SCOPE)
