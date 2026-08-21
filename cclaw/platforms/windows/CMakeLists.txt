



add_library(c_claw_platform_windows_objects OBJECT
    src/cc_windows_platform.c
    src/cc_windows_filesystem.c
    src/cc_windows_path.c
    src/cc_windows_thread.c
    src/cc_windows_secret_store.c
)

set(CC_PLATFORM_PUBLIC_INCLUDE_DIRS "")
set(CC_PLATFORM_PUBLIC_LIBRARIES "")

if(CC_NEEDS_HTTP_CLIENT)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        set(CC_PLATFORM_CURL_LIBRARIES CURL::libcurl)
    else()
        find_library(CC_PLATFORM_CURL_LIBRARY NAMES curl libcurl)
        find_path(CC_PLATFORM_CURL_INCLUDE_DIR NAMES curl/curl.h
            PATHS "${CMAKE_CURRENT_LIST_DIR}/vendor/curl/include")
        if(CC_PLATFORM_CURL_LIBRARY AND CC_PLATFORM_CURL_INCLUDE_DIR)
            set(CC_PLATFORM_CURL_LIBRARIES "${CC_PLATFORM_CURL_LIBRARY}")
        endif()
    endif()

    if(NOT CC_PLATFORM_CURL_LIBRARIES)
        message(FATAL_ERROR
            "Windows HTTP-backed features are enabled but libcurl was not found.")
    endif()
    target_sources(c_claw_platform_windows_objects PRIVATE src/cc_curl_http_client.c)
    list(APPEND CC_PLATFORM_PUBLIC_LIBRARIES ${CC_PLATFORM_CURL_LIBRARIES})
    if(DEFINED CC_PLATFORM_CURL_INCLUDE_DIR)
        list(APPEND CC_PLATFORM_PUBLIC_INCLUDE_DIRS "${CC_PLATFORM_CURL_INCLUDE_DIR}")
    endif()
endif()

if(CC_ENABLE_SHELL OR CC_ENABLE_PLUGIN OR CC_ENABLE_DOCKER_SANDBOX OR CC_ENABLE_MCP_STDIO)
    target_sources(c_claw_platform_windows_objects PRIVATE src/cc_windows_process.c)
endif()

target_link_libraries(c_claw_platform_windows_objects PRIVATE
    c_claw_core_headers
    c_claw_ports_headers
    c_claw_project_options
    advapi32
    bcrypt
    ws2_32
)

if(CC_PLATFORM_CURL_LIBRARIES)
    target_link_libraries(c_claw_platform_windows_objects PRIVATE ${CC_PLATFORM_CURL_LIBRARIES})
endif()
if(CC_PLATFORM_PUBLIC_INCLUDE_DIRS)
    target_include_directories(c_claw_platform_windows_objects PRIVATE
        ${CC_PLATFORM_PUBLIC_INCLUDE_DIRS}
    )
endif()

set(CC_PLATFORM_PUBLIC_INCLUDE_DIRS ${CC_PLATFORM_PUBLIC_INCLUDE_DIRS} PARENT_SCOPE)
set(CC_PLATFORM_PUBLIC_LIBRARIES ${CC_PLATFORM_PUBLIC_LIBRARIES} PARENT_SCOPE)
