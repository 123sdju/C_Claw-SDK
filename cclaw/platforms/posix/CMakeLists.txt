add_library(c_claw_platform_posix_objects OBJECT
    src/cc_posix_platform.c
    src/cc_posix_filesystem.c
    src/cc_posix_path.c
    src/cc_posix_thread.c
    src/cc_posix_secret_store.c
)

set(CC_PLATFORM_PUBLIC_INCLUDE_DIRS "")
set(CC_PLATFORM_PUBLIC_LIBRARIES "")

if(CC_NEEDS_HTTP_CLIENT)
    set(CC_PLATFORM_CURL_EXTRA_LIBRARIES "" CACHE STRING
        "Extra transitive libraries required by a manually located static libcurl")
    find_package(CURL QUIET)
    if(CURL_FOUND)
        set(CC_PLATFORM_CURL_LIBRARIES CURL::libcurl)
    else()
        find_library(CC_PLATFORM_CURL_LIBRARY
            NAMES curl libcurl libcurl.so.4
            DOC "Path to the POSIX libcurl library")
        find_path(CC_PLATFORM_CURL_INCLUDE_DIR
            NAMES curl/curl.h
            DOC "Path to the POSIX libcurl include directory")

        if(CC_PLATFORM_CURL_LIBRARY AND CC_PLATFORM_CURL_INCLUDE_DIR)
            set(CC_PLATFORM_CURL_LIBRARIES "${CC_PLATFORM_CURL_LIBRARY}")
            list(APPEND CC_PLATFORM_CURL_LIBRARIES ${CC_PLATFORM_CURL_EXTRA_LIBRARIES})
        endif()
    endif()

    if(NOT CC_PLATFORM_CURL_LIBRARIES)
        message(FATAL_ERROR
            "POSIX HTTP-backed features are enabled but libcurl was not found. "
            "Set CURL_DIR, CC_PLATFORM_CURL_LIBRARY, and CC_PLATFORM_CURL_INCLUDE_DIR "
            "when using a non-standard or cross-compiled libcurl installation. "
            "For a manually located static libcurl, also set CC_PLATFORM_CURL_EXTRA_LIBRARIES "
            "to its SSL/zlib/etc. transitive dependencies.")
    endif()

    target_sources(c_claw_platform_posix_objects PRIVATE src/cc_curl_http_client.c)
    list(APPEND CC_PLATFORM_PUBLIC_LIBRARIES ${CC_PLATFORM_CURL_LIBRARIES})
    if(CC_PLATFORM_CURL_INCLUDE_DIR)
        list(APPEND CC_PLATFORM_PUBLIC_INCLUDE_DIRS "${CC_PLATFORM_CURL_INCLUDE_DIR}")
    endif()
endif()

if(CC_ENABLE_SHELL OR CC_ENABLE_PLUGIN OR CC_ENABLE_DOCKER_SANDBOX OR CC_ENABLE_MCP_STDIO)
    target_sources(c_claw_platform_posix_objects PRIVATE src/cc_posix_process.c)
endif()

target_link_libraries(c_claw_platform_posix_objects PRIVATE
    c_claw_core_headers
    c_claw_ports_headers
    c_claw_project_options
    Threads::Threads
)

if(CC_PLATFORM_CURL_LIBRARIES)
    target_link_libraries(c_claw_platform_posix_objects PRIVATE ${CC_PLATFORM_CURL_LIBRARIES})
endif()
if(CC_PLATFORM_PUBLIC_INCLUDE_DIRS)
    target_include_directories(c_claw_platform_posix_objects PRIVATE
        ${CC_PLATFORM_PUBLIC_INCLUDE_DIRS}
    )
endif()

set(CC_PLATFORM_PUBLIC_INCLUDE_DIRS ${CC_PLATFORM_PUBLIC_INCLUDE_DIRS} PARENT_SCOPE)
set(CC_PLATFORM_PUBLIC_LIBRARIES ${CC_PLATFORM_PUBLIC_LIBRARIES} PARENT_SCOPE)
