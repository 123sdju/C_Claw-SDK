




set(C_CLAW_CORE_SOURCES
    vendor/cjson/cJSON.c
    src/core/cc_result.c
    src/core/cc_message.c
    src/core/cc_session.c
    src/core/cc_tool_call.c
    src/core/cc_media.c
    src/core/cc_tool_registry.c
    src/core/cc_event_bus.c
    src/core/cc_observability.c
    src/core/cc_memory_entry.c
    src/core/cc_memory_store.c
    src/core/cc_secret_store.c
    src/core/cc_id.c
    src/util/cc_string_builder.c
    src/util/cc_base64.c
    src/util/cc_json.c
    src/util/cc_config.c
    src/util/cc_network_policy.c
    src/util/cc_redaction.c
    src/util/cc_token_counter.c
    src/util/cc_logger.c
    src/app/cc_context_builder.c
    src/app/cc_tool_executor.c
    src/app/cc_session_manager.c
    src/app/cc_cancel_token.c
    src/app/cc_plugin_protocol.c
    src/app/cc_sse_parser.c
    src/app/cc_agent_runtime.c
    src/app/cc_runtime_controller.c
    src/app/cc_runtime_builder.c
    src/app/cc_memory_context.c
)

if(CC_ENABLE_RUN_QUEUE)
    list(APPEND C_CLAW_CORE_SOURCES
        src/app/cc_run_queue.c
    )
endif()

if(CC_ENABLE_TOOL_POOL)
    list(APPEND C_CLAW_CORE_SOURCES
        src/app/cc_tool_executor_pool.c
    )
endif()

if(CC_ENABLE_SKILLS)
    list(APPEND C_CLAW_CORE_SOURCES
        src/app/cc_skill_catalog.c
    )
endif()

if(CC_ENABLE_PLUGIN_HOT_RELOAD OR CC_ENABLE_MCP OR CC_ENABLE_TOOL_POOL)
    list(APPEND C_CLAW_CORE_SOURCES
        src/app/cc_tool_registry_snapshot.c
    )
endif()

if(CC_ENABLE_MCP)
    list(APPEND C_CLAW_CORE_SOURCES
        src/app/cc_mcp_runtime_manager.c
    )
endif()

if(CC_ENABLE_MULTI_AGENT AND CC_ENABLE_RUN_QUEUE)
    list(APPEND C_CLAW_CORE_SOURCES
        src/app/cc_agent_manager.c
    )
endif()

add_library(c_claw_core_headers INTERFACE)
target_include_directories(c_claw_core_headers INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/vendor/cjson>"
    "$<INSTALL_INTERFACE:include>"
)

add_library(c_claw_core_objects OBJECT ${C_CLAW_CORE_SOURCES})
target_link_libraries(c_claw_core_objects PRIVATE
    c_claw_core_headers
    c_claw_ports_headers
    c_claw_project_options
)

add_library(c_claw_core STATIC $<TARGET_OBJECTS:c_claw_core_objects>)
target_link_libraries(c_claw_core PUBLIC
    c_claw_core_headers
    c_claw_ports_headers
    c_claw_project_options
)
