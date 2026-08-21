



set(C_CLAW_ADAPTER_SOURCES
    src/cc_default_features.c
    src/storage/cc_storage_factory.c
    src/sandbox/cc_policy_engine_impl.c
)

if(CC_ENABLE_SESSION_STORE_JSON_FILE)
    list(APPEND C_CLAW_ADAPTER_SOURCES
        src/storage/cc_json_file_store.c
        src/storage/cc_json_segmented_store.c
    )
endif()

if(CC_ENABLE_SESSION_STORE_MEMORY)
    list(APPEND C_CLAW_ADAPTER_SOURCES src/storage/cc_memory_session_store.c)
endif()

if(CC_ENABLE_MEMORY)
    list(APPEND C_CLAW_ADAPTER_SOURCES
        src/storage/cc_memory_store_factory.c
        src/storage/cc_inmem_memory_store.c
    )
    if(CC_ENABLE_SESSION_STORE_JSON_FILE)
        list(APPEND C_CLAW_ADAPTER_SOURCES src/storage/cc_json_file_memory_store.c)
    endif()
endif()

if(CC_ENABLE_ARTIFACT_STORE)
    list(APPEND C_CLAW_ADAPTER_SOURCES src/storage/cc_memory_artifact_store.c)
endif()


if(CC_ENABLE_SESSION_STORE_SQLITE)
    list(APPEND C_CLAW_ADAPTER_SOURCES
        vendor/sqlite/sqlite3.c
        src/storage/cc_sqlite_session_store.c
        src/storage/cc_sqlite_memory_store.c
    )
    set_source_files_properties(vendor/sqlite/sqlite3.c PROPERTIES
        COMPILE_OPTIONS "-Wno-implicit-fallthrough")
endif()


if(CC_ENABLE_HTTP_TOOL)
    list(APPEND C_CLAW_ADAPTER_SOURCES src/tools/common/cc_http_request_tool.c)
endif()

if(CC_ENABLE_FILE_TOOLS)
    list(APPEND C_CLAW_ADAPTER_SOURCES
        src/tools/common/cc_file_read_tool.c
        src/tools/common/cc_file_write_tool.c
    )
endif()

if(CC_ENABLE_MEMORY)
    list(APPEND C_CLAW_ADAPTER_SOURCES src/tools/common/cc_memory_tool.c)
endif()


if(CC_ENABLE_OPENAI OR CC_ENABLE_OLLAMA OR CC_ENABLE_ANTHROPIC)
    list(APPEND C_CLAW_ADAPTER_SOURCES src/llm/cc_http_llm_provider.c)
endif()
if(CC_ENABLE_OPENAI)
    list(APPEND C_CLAW_ADAPTER_SOURCES src/llm/cc_openai_provider.c)
endif()
if(CC_ENABLE_OLLAMA)
    list(APPEND C_CLAW_ADAPTER_SOURCES src/llm/cc_ollama_provider.c)
endif()
if(CC_ENABLE_ANTHROPIC)
    list(APPEND C_CLAW_ADAPTER_SOURCES src/llm/cc_anthropic_provider.c)
endif()


add_library(c_claw_adapters_headers INTERFACE)
target_include_directories(c_claw_adapters_headers INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/vendor/sqlite>"
    "$<INSTALL_INTERFACE:include>"
)

add_library(c_claw_adapters_objects OBJECT ${C_CLAW_ADAPTER_SOURCES})
target_link_libraries(c_claw_adapters_objects PRIVATE
    c_claw_adapters_headers
    c_claw_core_headers
    c_claw_ports_headers
    c_claw_project_options
)
