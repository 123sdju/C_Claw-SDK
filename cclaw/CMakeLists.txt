



cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(c_claw VERSION 0.1.0 LANGUAGES C)
endif()

if(ESP_PLATFORM)
    set(CMAKE_C_STANDARD 23)
else()
    set(CMAKE_C_STANDARD 99)
endif()
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers")

if(NOT DEFINED CCLAW_VERSION)
    set(CCLAW_VERSION "0.1.0")
endif()

option(CC_BUILD_SHARED "Build c-claw runtime as a shared library" OFF)
if(CC_BUILD_SHARED)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
option(CC_BUILD_TESTS "Build c-claw SDK tests" OFF)

set(CC_PROFILE "core-minimal" CACHE STRING "Build profile: core-minimal")
set_property(CACHE CC_PROFILE PROPERTY STRINGS
    core-minimal
    multimodal-full
    desktop-agent
    mcu-text
    mcu-mm-lite
    esp32-tools)

if(NOT DEFINED CCLAW_PROFILE_FILE OR CCLAW_PROFILE_FILE STREQUAL "")
    set(CCLAW_PROFILE_FILE "${CMAKE_CURRENT_LIST_DIR}/profiles/${CC_PROFILE}.cmake")
endif()
set(CCLAW_PROFILE_FILE "${CCLAW_PROFILE_FILE}" CACHE FILEPATH
    "CClaw profile file; products may provide an external profile")
if(NOT EXISTS "${CCLAW_PROFILE_FILE}")
    message(FATAL_ERROR "CClaw profile file does not exist: ${CCLAW_PROFILE_FILE}")
endif()

include("${CCLAW_PROFILE_FILE}")

function(cc_profile_default var value)
    if(NOT DEFINED ${var})
        set(${var} "${value}" PARENT_SCOPE)
    endif()
endfunction()

cc_profile_default(CC_PROFILE_ENABLE_MULTIMODAL OFF)
cc_profile_default(CC_PROFILE_ENABLE_MEDIA_IMAGE OFF)
cc_profile_default(CC_PROFILE_ENABLE_MEDIA_AUDIO OFF)
cc_profile_default(CC_PROFILE_ENABLE_MEDIA_VIDEO OFF)
cc_profile_default(CC_PROFILE_ENABLE_MEDIA_FILE OFF)
cc_profile_default(CC_PROFILE_ENABLE_MEDIA_OUTPUT OFF)
cc_profile_default(CC_PROFILE_ENABLE_INLINE_BASE64 OFF)
cc_profile_default(CC_PROFILE_ENABLE_ARTIFACT_STORE OFF)
cc_profile_default(CC_PROFILE_ENABLE_STREAMING ON)
cc_profile_default(CC_PROFILE_ENABLE_ASYNC_OBSERVABILITY OFF)
cc_profile_default(CC_PROFILE_ENABLE_SESSION_STORE_MEMORY ON)
cc_profile_default(CC_PROFILE_ENABLE_SESSION_STORE_JSON_FILE ON)
cc_profile_default(CC_PROFILE_ENABLE_SESSION_STORE_SQLITE "${CC_PROFILE_ENABLE_SQLITE}")


set(CC_TARGET_PLATFORM "${CC_PROFILE_TARGET_PLATFORM}" CACHE STRING "Target platform: auto, posix, windows, esp32, freertos")
set_property(CACHE CC_TARGET_PLATFORM PROPERTY STRINGS auto posix windows esp32 freertos)

if(CC_TARGET_PLATFORM STREQUAL "auto")
    if(DEFINED ESP_PLATFORM)
        set(CC_TARGET_PLATFORM_RESOLVED "esp32")
    elseif(WIN32)
        set(CC_TARGET_PLATFORM_RESOLVED "windows")
    else()
        set(CC_TARGET_PLATFORM_RESOLVED "posix")
    endif()
else()
    set(CC_TARGET_PLATFORM_RESOLVED "${CC_TARGET_PLATFORM}")
endif()
set(CC_TARGET_PLATFORM_RESOLVED "${CC_TARGET_PLATFORM_RESOLVED}" CACHE INTERNAL "Resolved c-claw platform")


if(CC_TARGET_PLATFORM_RESOLVED STREQUAL "posix")
    set(CC_PLATFORM_ID 100)
elseif(CC_TARGET_PLATFORM_RESOLVED STREQUAL "windows")
    set(CC_PLATFORM_ID 200)
elseif(CC_TARGET_PLATFORM_RESOLVED STREQUAL "esp32")
    set(CC_PLATFORM_ID 300)
elseif(CC_TARGET_PLATFORM_RESOLVED STREQUAL "freertos")
    set(CC_PLATFORM_ID 400)
else()
    message(FATAL_ERROR "Unknown CC_TARGET_PLATFORM: ${CC_TARGET_PLATFORM}")
endif()

if(CC_TARGET_PLATFORM_RESOLVED STREQUAL "esp32" AND NOT ESP_PLATFORM)
    message(FATAL_ERROR
        "CC_TARGET_PLATFORM=esp32 must be configured from ESP-IDF "
        "(ESP_PLATFORM is not set).")
endif()


if(DEFINED CC_CACHED_TARGET_PLATFORM AND
   NOT CC_CACHED_TARGET_PLATFORM STREQUAL CC_TARGET_PLATFORM_RESOLVED)
    message(FATAL_ERROR
        "This build directory was configured for '${CC_CACHED_TARGET_PLATFORM}' "
        "and is now being reconfigured for '${CC_TARGET_PLATFORM_RESOLVED}'. "
        "Use a fresh build directory.")
endif()
set(CC_CACHED_TARGET_PLATFORM "${CC_TARGET_PLATFORM_RESOLVED}"
    CACHE INTERNAL "Resolved c-claw platform profile for this build directory")

option(CC_ENABLE_FILE_TOOLS "Enable file read/write tools" ${CC_PROFILE_ENABLE_FILE_TOOLS})
option(CC_ENABLE_HTTP_TOOL "Enable http.request tool" ${CC_PROFILE_ENABLE_HTTP_TOOL})
option(CC_ENABLE_MEMORY "Enable long-term memory subsystem" ${CC_PROFILE_ENABLE_MEMORY})
option(CC_ENABLE_SHELL "Enable shell.run tool and local sandbox" ${CC_PROFILE_ENABLE_SHELL})
option(CC_ENABLE_PLUGIN "Enable external process plugin tools" ${CC_PROFILE_ENABLE_PLUGIN})
option(CC_ENABLE_MULTI_AGENT "Enable multi-agent manager core" ${CC_PROFILE_ENABLE_MULTI_AGENT})
option(CC_ENABLE_RUN_QUEUE "Enable run queue and session serialization" ${CC_PROFILE_ENABLE_RUN_QUEUE})
option(CC_ENABLE_TOOL_POOL "Enable tool executor concurrency pool" ${CC_PROFILE_ENABLE_TOOL_POOL})
option(CC_ENABLE_SKILLS "Enable AgentSkills-compatible skill catalog" ${CC_PROFILE_ENABLE_SKILLS})
option(CC_ENABLE_SKILL_WATCHER "Enable app-level skill file watcher support" ${CC_PROFILE_ENABLE_SKILL_WATCHER})
option(CC_ENABLE_PLUGIN_HOT_RELOAD "Enable plugin hot reload generation swapping" ${CC_PROFILE_ENABLE_PLUGIN_HOT_RELOAD})
option(CC_ENABLE_PLUGIN_WORKERS "Enable plugin worker pools" ${CC_PROFILE_ENABLE_PLUGIN_WORKERS})
option(CC_ENABLE_MCP "Enable MCP runtime abstractions and tool bridge" ${CC_PROFILE_ENABLE_MCP})
option(CC_ENABLE_MCP_STDIO "Enable application-provided stdio MCP transport support" ${CC_PROFILE_ENABLE_MCP_STDIO})
option(CC_ENABLE_MCP_HTTP "Enable HTTP/SSE MCP transport" ${CC_PROFILE_ENABLE_MCP_HTTP})
option(CC_ENABLE_SUBAGENTS "Enable subagent services" ${CC_PROFILE_ENABLE_SUBAGENTS})
option(CC_ENABLE_ACTIVE_MEMORY "Enable active memory hook for run context and summary writes" ${CC_PROFILE_ENABLE_ACTIVE_MEMORY})
option(CC_ENABLE_DOCKER_SANDBOX "Enable Docker sandbox adapter" ${CC_PROFILE_ENABLE_DOCKER_SANDBOX})
option(CC_ENABLE_MULTIMODAL "Enable SDK multimodal content/artifact APIs at runtime" ${CC_PROFILE_ENABLE_MULTIMODAL})
option(CC_ENABLE_MEDIA_IMAGE "Enable image modality" ${CC_PROFILE_ENABLE_MEDIA_IMAGE})
option(CC_ENABLE_MEDIA_AUDIO "Enable audio modality" ${CC_PROFILE_ENABLE_MEDIA_AUDIO})
option(CC_ENABLE_MEDIA_VIDEO "Enable video modality" ${CC_PROFILE_ENABLE_MEDIA_VIDEO})
option(CC_ENABLE_MEDIA_FILE "Enable generic file modality" ${CC_PROFILE_ENABLE_MEDIA_FILE})
option(CC_ENABLE_MEDIA_OUTPUT "Enable non-text media output" ${CC_PROFILE_ENABLE_MEDIA_OUTPUT})
option(CC_ENABLE_INLINE_BASE64 "Enable inline base64 media payloads" ${CC_PROFILE_ENABLE_INLINE_BASE64})
option(CC_ENABLE_ARTIFACT_STORE "Enable local artifact store adapters" ${CC_PROFILE_ENABLE_ARTIFACT_STORE})
option(CC_ENABLE_STREAMING "Enable streaming response/event code paths" ${CC_PROFILE_ENABLE_STREAMING})
option(CC_ENABLE_ASYNC_OBSERVABILITY "Enable async SDK observability event bus by default" ${CC_PROFILE_ENABLE_ASYNC_OBSERVABILITY})
option(CC_ENABLE_SESSION_STORE_MEMORY "Enable in-memory session store" ${CC_PROFILE_ENABLE_SESSION_STORE_MEMORY})
option(CC_ENABLE_SESSION_STORE_JSON_FILE "Enable JSON-file session store" ${CC_PROFILE_ENABLE_SESSION_STORE_JSON_FILE})
option(CC_ENABLE_SESSION_STORE_SQLITE "Enable SQLite session store" ${CC_PROFILE_ENABLE_SESSION_STORE_SQLITE})
option(CC_ENABLE_OPENAI "Enable OpenAI-compatible LLM adapter" ${CC_PROFILE_ENABLE_OPENAI})
option(CC_ENABLE_OLLAMA "Enable Ollama LLM adapter" ${CC_PROFILE_ENABLE_OLLAMA})
option(CC_ENABLE_ANTHROPIC "Enable Anthropic LLM adapter" ${CC_PROFILE_ENABLE_ANTHROPIC})

if(CC_PROFILE STREQUAL "esp32-tools" OR CCLAW_PROFILE_LOCK_OPTIONS)
    set(CC_ENABLE_FILE_TOOLS ${CC_PROFILE_ENABLE_FILE_TOOLS}
        CACHE BOOL "Enable file read/write tools" FORCE)
    set(CC_ENABLE_HTTP_TOOL ${CC_PROFILE_ENABLE_HTTP_TOOL}
        CACHE BOOL "Enable http.request tool" FORCE)
    set(CC_ENABLE_MEMORY ${CC_PROFILE_ENABLE_MEMORY}
        CACHE BOOL "Enable long-term memory subsystem" FORCE)
    set(CC_ENABLE_SHELL ${CC_PROFILE_ENABLE_SHELL}
        CACHE BOOL "Enable shell.run tool and local sandbox" FORCE)
    set(CC_ENABLE_PLUGIN ${CC_PROFILE_ENABLE_PLUGIN}
        CACHE BOOL "Enable external process plugin tools" FORCE)
    set(CC_ENABLE_MULTI_AGENT ${CC_PROFILE_ENABLE_MULTI_AGENT}
        CACHE BOOL "Enable multi-agent manager core" FORCE)
    set(CC_ENABLE_RUN_QUEUE ${CC_PROFILE_ENABLE_RUN_QUEUE}
        CACHE BOOL "Enable run queue and session serialization" FORCE)
    set(CC_ENABLE_TOOL_POOL ${CC_PROFILE_ENABLE_TOOL_POOL}
        CACHE BOOL "Enable tool executor concurrency pool" FORCE)
    set(CC_ENABLE_SKILLS ${CC_PROFILE_ENABLE_SKILLS}
        CACHE BOOL "Enable AgentSkills-compatible skill catalog" FORCE)
    set(CC_ENABLE_SKILL_WATCHER ${CC_PROFILE_ENABLE_SKILL_WATCHER}
        CACHE BOOL "Enable app-level skill file watcher support" FORCE)
    set(CC_ENABLE_PLUGIN_HOT_RELOAD ${CC_PROFILE_ENABLE_PLUGIN_HOT_RELOAD}
        CACHE BOOL "Enable plugin hot reload generation swapping" FORCE)
    set(CC_ENABLE_PLUGIN_WORKERS ${CC_PROFILE_ENABLE_PLUGIN_WORKERS}
        CACHE BOOL "Enable plugin worker pools" FORCE)
    set(CC_ENABLE_MCP ${CC_PROFILE_ENABLE_MCP}
        CACHE BOOL "Enable MCP runtime abstractions and tool bridge" FORCE)
    set(CC_ENABLE_MCP_STDIO ${CC_PROFILE_ENABLE_MCP_STDIO}
        CACHE BOOL "Enable application-provided stdio MCP transport support" FORCE)
    set(CC_ENABLE_MCP_HTTP ${CC_PROFILE_ENABLE_MCP_HTTP}
        CACHE BOOL "Enable HTTP/SSE MCP transport" FORCE)
    set(CC_ENABLE_SUBAGENTS ${CC_PROFILE_ENABLE_SUBAGENTS}
        CACHE BOOL "Enable subagent services" FORCE)
    set(CC_ENABLE_ACTIVE_MEMORY ${CC_PROFILE_ENABLE_ACTIVE_MEMORY}
        CACHE BOOL "Enable active memory hook for run context and summary writes" FORCE)
    set(CC_ENABLE_DOCKER_SANDBOX ${CC_PROFILE_ENABLE_DOCKER_SANDBOX}
        CACHE BOOL "Enable Docker sandbox adapter" FORCE)
    set(CC_ENABLE_OPENAI ${CC_PROFILE_ENABLE_OPENAI}
        CACHE BOOL "Enable OpenAI-compatible LLM adapter" FORCE)
    set(CC_ENABLE_OLLAMA ${CC_PROFILE_ENABLE_OLLAMA}
        CACHE BOOL "Enable Ollama LLM adapter" FORCE)
    set(CC_ENABLE_ANTHROPIC ${CC_PROFILE_ENABLE_ANTHROPIC}
        CACHE BOOL "Enable Anthropic LLM adapter" FORCE)
    set(CC_ENABLE_MULTIMODAL ${CC_PROFILE_ENABLE_MULTIMODAL}
        CACHE BOOL "Enable SDK multimodal content/artifact APIs at runtime" FORCE)
    set(CC_ENABLE_MEDIA_IMAGE ${CC_PROFILE_ENABLE_MEDIA_IMAGE}
        CACHE BOOL "Enable image modality" FORCE)
    set(CC_ENABLE_MEDIA_AUDIO ${CC_PROFILE_ENABLE_MEDIA_AUDIO}
        CACHE BOOL "Enable audio modality" FORCE)
    set(CC_ENABLE_MEDIA_VIDEO ${CC_PROFILE_ENABLE_MEDIA_VIDEO}
        CACHE BOOL "Enable video modality" FORCE)
    set(CC_ENABLE_MEDIA_FILE ${CC_PROFILE_ENABLE_MEDIA_FILE}
        CACHE BOOL "Enable generic file modality" FORCE)
    set(CC_ENABLE_MEDIA_OUTPUT ${CC_PROFILE_ENABLE_MEDIA_OUTPUT}
        CACHE BOOL "Enable non-text media output" FORCE)
    set(CC_ENABLE_INLINE_BASE64 ${CC_PROFILE_ENABLE_INLINE_BASE64}
        CACHE BOOL "Enable inline base64 media payloads" FORCE)
    set(CC_ENABLE_ARTIFACT_STORE ${CC_PROFILE_ENABLE_ARTIFACT_STORE}
        CACHE BOOL "Enable local artifact store adapters" FORCE)
    set(CC_ENABLE_SESSION_STORE_MEMORY ${CC_PROFILE_ENABLE_SESSION_STORE_MEMORY}
        CACHE BOOL "Enable in-memory session store" FORCE)
    set(CC_ENABLE_SESSION_STORE_JSON_FILE ${CC_PROFILE_ENABLE_SESSION_STORE_JSON_FILE}
        CACHE BOOL "Enable JSON-file session store" FORCE)
    set(CC_ENABLE_SESSION_STORE_SQLITE ${CC_PROFILE_ENABLE_SESSION_STORE_SQLITE}
        CACHE BOOL "Enable SQLite session store" FORCE)
    set(CC_ENABLE_STREAMING ${CC_PROFILE_ENABLE_STREAMING}
        CACHE BOOL "Enable streaming response/event code paths" FORCE)
    set(CC_ENABLE_ASYNC_OBSERVABILITY ${CC_PROFILE_ENABLE_ASYNC_OBSERVABILITY}
        CACHE BOOL "Enable async SDK observability event bus by default" FORCE)
endif()

option(CC_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# Feature dependencies are contracts, not best-effort runtime fallbacks.  Reject
# impossible combinations while configuring so platform profiles cannot silently
# compile code whose owner subsystem is absent.
if((CC_ENABLE_MCP_STDIO OR CC_ENABLE_MCP_HTTP) AND NOT CC_ENABLE_MCP)
    message(FATAL_ERROR "MCP transports require CC_ENABLE_MCP=ON")
endif()
if(CC_ENABLE_SUBAGENTS AND NOT CC_ENABLE_MULTI_AGENT)
    message(FATAL_ERROR "CC_ENABLE_SUBAGENTS requires CC_ENABLE_MULTI_AGENT=ON")
endif()
if(CC_ENABLE_SUBAGENTS AND NOT CC_ENABLE_RUN_QUEUE)
    message(FATAL_ERROR "CC_ENABLE_SUBAGENTS requires CC_ENABLE_RUN_QUEUE=ON")
endif()
if(CC_ENABLE_ACTIVE_MEMORY AND NOT CC_ENABLE_MEMORY)
    message(FATAL_ERROR "CC_ENABLE_ACTIVE_MEMORY requires CC_ENABLE_MEMORY=ON")
endif()
if(CC_ENABLE_SKILL_WATCHER AND NOT CC_ENABLE_SKILLS)
    message(FATAL_ERROR "CC_ENABLE_SKILL_WATCHER requires CC_ENABLE_SKILLS=ON")
endif()
if((CC_ENABLE_PLUGIN_HOT_RELOAD OR CC_ENABLE_PLUGIN_WORKERS) AND NOT CC_ENABLE_PLUGIN)
    message(FATAL_ERROR "Plugin hot reload/workers require CC_ENABLE_PLUGIN=ON")
endif()
if((CC_ENABLE_MEDIA_IMAGE OR CC_ENABLE_MEDIA_AUDIO OR CC_ENABLE_MEDIA_VIDEO OR
    CC_ENABLE_MEDIA_FILE OR CC_ENABLE_MEDIA_OUTPUT OR CC_ENABLE_INLINE_BASE64 OR
    CC_ENABLE_ARTIFACT_STORE) AND NOT CC_ENABLE_MULTIMODAL)
    message(FATAL_ERROR "Media, inline Base64, and artifact store features require CC_ENABLE_MULTIMODAL=ON")
endif()

message(STATUS "c-claw profile: ${CC_PROFILE}")
message(STATUS "c-claw target platform: ${CC_TARGET_PLATFORM_RESOLVED}")


set(THREADS_PREFER_PTHREAD_FLAG ON)
if(NOT CC_TARGET_PLATFORM_RESOLVED STREQUAL "esp32" AND NOT CC_TARGET_PLATFORM_RESOLVED STREQUAL "freertos")
    find_package(Threads REQUIRED)
endif()


set(CC_NEEDS_HTTP_CLIENT OFF)
if(CC_ENABLE_HTTP_TOOL OR CC_ENABLE_OPENAI OR CC_ENABLE_OLLAMA OR CC_ENABLE_ANTHROPIC OR CC_ENABLE_MCP_HTTP)
    set(CC_NEEDS_HTTP_CLIENT ON)
endif()

if(CC_TARGET_PLATFORM_RESOLVED STREQUAL "windows" AND CC_ENABLE_DOCKER_SANDBOX)
    message(STATUS "Docker sandbox is disabled for Windows targets")
    set(CC_ENABLE_DOCKER_SANDBOX OFF CACHE BOOL "Enable Docker sandbox adapter" FORCE)
endif()


if(NOT DEFINED CC_PROFILE_DEFAULT_CONFIG_PATH)
    set(CC_PROFILE_DEFAULT_CONFIG_PATH "")
endif()
if(NOT DEFINED CC_PROFILE_DEFAULT_DATA_DIR)
    set(CC_PROFILE_DEFAULT_DATA_DIR "${CMAKE_BINARY_DIR}/${CC_PROFILE}/runtime/data")
endif()
if(NOT DEFINED CC_PROFILE_DEFAULT_WORKSPACE_PATH)
    set(CC_PROFILE_DEFAULT_WORKSPACE_PATH "${CMAKE_BINARY_DIR}/${CC_PROFILE}/runtime/workspace")
endif()
if(NOT DEFINED CC_PROFILE_DEFAULT_STORAGE_PATH)
    if(CC_ENABLE_SESSION_STORE_SQLITE)
        set(CC_PROFILE_DEFAULT_STORAGE_PATH "${CC_PROFILE_DEFAULT_DATA_DIR}/c-claw.db")
    else()
        set(CC_PROFILE_DEFAULT_STORAGE_PATH "${CC_PROFILE_DEFAULT_DATA_DIR}/sessions")
    endif()
endif()
if(NOT DEFINED CC_PROFILE_DEFAULT_MEMORY_PATH)
    set(CC_PROFILE_DEFAULT_MEMORY_PATH "${CC_PROFILE_DEFAULT_DATA_DIR}/memory.json")
endif()
if(NOT DEFINED CC_PROFILE_DEFAULT_SOUL_FILE)
    set(CC_PROFILE_DEFAULT_SOUL_FILE "")
endif()
if(NOT DEFINED CC_PROFILE_DEFAULT_USER_FILE)
    set(CC_PROFILE_DEFAULT_USER_FILE "")
endif()

set(CC_DEFAULT_CONFIG_PATH_VALUE "${CC_PROFILE_DEFAULT_CONFIG_PATH}")
set(CC_DEFAULT_DATA_DIR_VALUE "${CC_PROFILE_DEFAULT_DATA_DIR}")
set(CC_DEFAULT_WORKSPACE_PATH_VALUE "${CC_PROFILE_DEFAULT_WORKSPACE_PATH}")
set(CC_DEFAULT_STORAGE_PATH_VALUE "${CC_PROFILE_DEFAULT_STORAGE_PATH}")
set(CC_DEFAULT_MEMORY_PATH_VALUE "${CC_PROFILE_DEFAULT_MEMORY_PATH}")
set(CC_DEFAULT_SOUL_FILE_VALUE "${CC_PROFILE_DEFAULT_SOUL_FILE}")
set(CC_DEFAULT_USER_FILE_VALUE "${CC_PROFILE_DEFAULT_USER_FILE}")


if(NOT CC_TARGET_PLATFORM_RESOLVED STREQUAL "esp32" AND NOT CC_TARGET_PLATFORM_RESOLVED STREQUAL "freertos")
    file(MAKE_DIRECTORY
        "${CC_DEFAULT_DATA_DIR_VALUE}"
        "${CC_DEFAULT_WORKSPACE_PATH_VALUE}"
    )
endif()


add_library(c_claw_project_options INTERFACE)
target_compile_definitions(c_claw_project_options INTERFACE
    CC_PLATFORM=${CC_PLATFORM_ID}
    CC_TOOL_SHELL_RUN=$<BOOL:${CC_ENABLE_SHELL}>
    CC_SANDBOX_LOCAL=$<BOOL:${CC_ENABLE_SHELL}>
    CC_SANDBOX_DOCKER=$<BOOL:${CC_ENABLE_DOCKER_SANDBOX}>
    CC_TOOL_HTTP_REQUEST=$<BOOL:${CC_ENABLE_HTTP_TOOL}>
    CC_TOOL_FILE_READ=$<BOOL:${CC_ENABLE_FILE_TOOLS}>
    CC_TOOL_FILE_WRITE=$<BOOL:${CC_ENABLE_FILE_TOOLS}>
    CC_TOOL_PLUGIN=$<BOOL:${CC_ENABLE_PLUGIN}>
    CC_ENABLE_MULTI_AGENT=$<BOOL:${CC_ENABLE_MULTI_AGENT}>
    CC_ENABLE_RUN_QUEUE=$<BOOL:${CC_ENABLE_RUN_QUEUE}>
    CC_ENABLE_TOOL_POOL=$<BOOL:${CC_ENABLE_TOOL_POOL}>
    CC_ENABLE_SKILLS=$<BOOL:${CC_ENABLE_SKILLS}>
    CC_ENABLE_SKILL_WATCHER=$<BOOL:${CC_ENABLE_SKILL_WATCHER}>
    CC_ENABLE_PLUGIN_HOT_RELOAD=$<BOOL:${CC_ENABLE_PLUGIN_HOT_RELOAD}>
    CC_ENABLE_PLUGIN_WORKERS=$<BOOL:${CC_ENABLE_PLUGIN_WORKERS}>
    CC_ENABLE_MCP=$<BOOL:${CC_ENABLE_MCP}>
    CC_ENABLE_MCP_STDIO=$<BOOL:${CC_ENABLE_MCP_STDIO}>
    CC_ENABLE_MCP_HTTP=$<BOOL:${CC_ENABLE_MCP_HTTP}>
    CC_ENABLE_SUBAGENTS=$<BOOL:${CC_ENABLE_SUBAGENTS}>
    CC_ENABLE_ACTIVE_MEMORY=$<BOOL:${CC_ENABLE_ACTIVE_MEMORY}>
    CC_ENABLE_MULTIMODAL=$<BOOL:${CC_ENABLE_MULTIMODAL}>
    CC_ENABLE_MEDIA_IMAGE=$<BOOL:${CC_ENABLE_MEDIA_IMAGE}>
    CC_ENABLE_MEDIA_AUDIO=$<BOOL:${CC_ENABLE_MEDIA_AUDIO}>
    CC_ENABLE_MEDIA_VIDEO=$<BOOL:${CC_ENABLE_MEDIA_VIDEO}>
    CC_ENABLE_MEDIA_FILE=$<BOOL:${CC_ENABLE_MEDIA_FILE}>
    CC_ENABLE_MEDIA_OUTPUT=$<BOOL:${CC_ENABLE_MEDIA_OUTPUT}>
    CC_ENABLE_INLINE_BASE64=$<BOOL:${CC_ENABLE_INLINE_BASE64}>
    CC_ENABLE_ARTIFACT_STORE=$<BOOL:${CC_ENABLE_ARTIFACT_STORE}>
    CC_ENABLE_STREAMING=$<BOOL:${CC_ENABLE_STREAMING}>
    CC_ENABLE_ASYNC_OBSERVABILITY=$<BOOL:${CC_ENABLE_ASYNC_OBSERVABILITY}>
    CC_STORAGE_SQLITE=$<BOOL:${CC_ENABLE_SESSION_STORE_SQLITE}>
    CC_STORAGE_JSON_FILE=$<BOOL:${CC_ENABLE_SESSION_STORE_JSON_FILE}>
    CC_STORAGE_MEMORY=$<BOOL:${CC_ENABLE_SESSION_STORE_MEMORY}>
    CC_LLM_OPENAI=$<BOOL:${CC_ENABLE_OPENAI}>
    CC_LLM_OLLAMA=$<BOOL:${CC_ENABLE_OLLAMA}>
    CC_LLM_ANTHROPIC=$<BOOL:${CC_ENABLE_ANTHROPIC}>
    CC_HAS_MEMORY=$<BOOL:${CC_ENABLE_MEMORY}>
    CC_DEFAULT_CONFIG_PATH="${CC_DEFAULT_CONFIG_PATH_VALUE}"
    CC_DEFAULT_DATA_DIR="${CC_DEFAULT_DATA_DIR_VALUE}"
    CC_DEFAULT_WORKSPACE_PATH="${CC_DEFAULT_WORKSPACE_PATH_VALUE}"
    CC_DEFAULT_STORAGE_PATH="${CC_DEFAULT_STORAGE_PATH_VALUE}"
    CC_DEFAULT_MEMORY_PATH="${CC_DEFAULT_MEMORY_PATH_VALUE}"
    CC_DEFAULT_SOUL_FILE="${CC_DEFAULT_SOUL_FILE_VALUE}"
    CC_DEFAULT_USER_FILE="${CC_DEFAULT_USER_FILE_VALUE}"
)

if(CC_ENABLE_SANITIZERS)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(c_claw_project_options INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(c_claw_project_options INTERFACE
            -fsanitize=address,undefined
        )
    else()
        message(WARNING "CC_ENABLE_SANITIZERS is only configured for GCC/Clang")
    endif()
endif()

if(CC_TARGET_PLATFORM_RESOLVED STREQUAL "windows" AND NOT MINGW)
    target_compile_definitions(c_claw_project_options INTERFACE
        _CRT_SECURE_NO_WARNINGS
        _WIN32_WINNT=0x0600)
elseif(CC_TARGET_PLATFORM_RESOLVED STREQUAL "freertos")
    target_compile_definitions(c_claw_project_options INTERFACE
        CC_FREERTOS_PLATFORM=1
        CC_HAS_NETWORK=$<BOOL:${CC_NEEDS_HTTP_CLIENT}>)
elseif(NOT CC_TARGET_PLATFORM_RESOLVED STREQUAL "esp32")
    target_compile_definitions(c_claw_project_options INTERFACE
        _POSIX_C_SOURCE=200809L
        _XOPEN_SOURCE=700)
endif()


add_subdirectory(ports)
add_subdirectory(core)
add_subdirectory(adapters)
add_subdirectory("platforms/${CC_TARGET_PLATFORM_RESOLVED}")


set(CC_PLATFORM_OBJECT_TARGET "c_claw_platform_${CC_TARGET_PLATFORM_RESOLVED}_objects")
add_library(cclaw_core ALIAS c_claw_core)
add_library(cclaw_ports ALIAS c_claw_ports_headers)
add_library(cclaw_adapters ALIAS c_claw_adapters_objects)
add_library(cclaw_platform_${CC_TARGET_PLATFORM_RESOLVED} ALIAS ${CC_PLATFORM_OBJECT_TARGET})

set(CC_RUNTIME_LIBRARY_TYPE STATIC)
if(CC_BUILD_SHARED)
    set(CC_RUNTIME_LIBRARY_TYPE SHARED)
endif()

add_library(c_claw_runtime ${CC_RUNTIME_LIBRARY_TYPE}
    $<TARGET_OBJECTS:c_claw_core_objects>
    $<TARGET_OBJECTS:c_claw_adapters_objects>
    $<TARGET_OBJECTS:${CC_PLATFORM_OBJECT_TARGET}>
)
add_library(CClaw::runtime ALIAS c_claw_runtime)
add_library(cclaw_runtime ALIAS c_claw_runtime)
set_target_properties(c_claw_runtime PROPERTIES
    EXPORT_NAME runtime
    VERSION "${CCLAW_VERSION}"
    SOVERSION "0"
)
target_link_libraries(c_claw_runtime PUBLIC
    c_claw_core_headers
    c_claw_adapters_headers
    c_claw_ports_headers
    c_claw_project_options
)

if(NOT CC_TARGET_PLATFORM_RESOLVED STREQUAL "esp32" AND NOT CC_TARGET_PLATFORM_RESOLVED STREQUAL "freertos")
    target_link_libraries(c_claw_runtime PUBLIC Threads::Threads)
    if(CMAKE_DL_LIBS)
        target_link_libraries(c_claw_runtime PUBLIC ${CMAKE_DL_LIBS})
    endif()
endif()

if(CC_PLATFORM_PUBLIC_LIBRARIES)
    target_link_libraries(c_claw_runtime PUBLIC ${CC_PLATFORM_PUBLIC_LIBRARIES})
endif()
if(CC_PLATFORM_PUBLIC_INCLUDE_DIRS)
    target_include_directories(c_claw_runtime PRIVATE ${CC_PLATFORM_PUBLIC_INCLUDE_DIRS})
endif()

if(NOT ESP_PLATFORM)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS
        c_claw_runtime
        c_claw_project_options
        c_claw_core_headers
        c_claw_adapters_headers
        c_claw_ports_headers
        EXPORT CClawTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    install(DIRECTORY
        "${CMAKE_CURRENT_LIST_DIR}/core/include/cc"
        "${CMAKE_CURRENT_LIST_DIR}/adapters/include/cc"
        "${CMAKE_CURRENT_LIST_DIR}/ports/include/cc"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    set(CCLAW_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/CClaw")
    install(EXPORT CClawTargets
        NAMESPACE CClaw::
        FILE CClawTargets.cmake
        DESTINATION "${CCLAW_CMAKE_INSTALL_DIR}"
    )

    configure_package_config_file(
        "${CMAKE_CURRENT_LIST_DIR}/../cmake/CClawConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/CClawConfig.cmake"
        INSTALL_DESTINATION "${CCLAW_CMAKE_INSTALL_DIR}"
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/CClawConfigVersion.cmake"
        VERSION "${CCLAW_VERSION}"
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/CClawConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/CClawConfigVersion.cmake"
        DESTINATION "${CCLAW_CMAKE_INSTALL_DIR}"
    )

    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/../cmake/cclaw.pc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cclaw.pc"
        @ONLY
    )
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cclaw.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
    )
endif()

if(CC_BUILD_TESTS AND NOT ESP_PLATFORM)
    enable_testing()
    add_subdirectory(tests)
endif()
