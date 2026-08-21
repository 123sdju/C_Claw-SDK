



function(cc_add_sdk_test test_name source_path)
    add_executable(${test_name} "${source_path}")
    target_link_libraries(${test_name} PRIVATE c_claw_runtime)
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endfunction()

set(C_CLAW_CORE_TESTS
    test_thread_mutex
    test_tool_registry_freeze
    test_logger_concurrent
    test_event_bus_concurrent
    test_event_bus_async
    test_runtime_concurrent_sessions
    test_runtime_builder_tool_failure_cleanup
    test_default_features
    test_runtime_request_config
    test_config_missing_defaults
    test_config_runtime_sections
    test_cancel_token
    test_plugin_protocol_envelope
    test_sse_parser
    test_runtime_stream_callback
    test_memory_query_port
    test_filesystem_binary_write
    test_path_security
    test_result_detail
    test_observability_schema
    test_observability_no_legacy_events
    test_redaction
)

foreach(test_name IN LISTS C_CLAW_CORE_TESTS)
    cc_add_sdk_test(${test_name} "core/${test_name}.c")
endforeach()

if(CC_ENABLE_RUN_QUEUE)
    cc_add_sdk_test(test_run_queue_session_serial "core/test_run_queue_session_serial.c")
    cc_add_sdk_test(test_run_queue_async_interrupt "core/test_run_queue_async_interrupt.c")
endif()

if(CC_ENABLE_TOOL_POOL)
    cc_add_sdk_test(test_tool_executor_pool_lane "core/test_tool_executor_pool_lane.c")
endif()

if(CC_ENABLE_SKILLS)
    cc_add_sdk_test(test_skill_catalog_prompt "core/test_skill_catalog_prompt.c")
endif()

if(CC_ENABLE_MCP)
    cc_add_sdk_test(test_mcp_jsonrpc_matcher "core/test_mcp_jsonrpc_matcher.c")
    cc_add_sdk_test(test_mcp_runtime_manager_fake_transport "core/test_mcp_runtime_manager_fake_transport.c")
endif()

set(C_CLAW_ADAPTER_TESTS
    test_memory_store_concurrent
    test_storage_factory_failfast
    test_tool_executor_policy_approval
    test_message_envelope_serialization
    test_multimodal_content_parts
    test_tool_schema_validation
)

if(CC_ENABLE_OPENAI AND CC_ENABLE_OLLAMA AND CC_ENABLE_ANTHROPIC)
    list(APPEND C_CLAW_ADAPTER_TESTS test_provider_stream_protocols)
endif()

if(CC_ENABLE_HTTP_TOOL)
    list(APPEND C_CLAW_ADAPTER_TESTS test_http_allowlist)
endif()

if(CC_ENABLE_SESSION_STORE_JSON_FILE)
    list(APPEND C_CLAW_ADAPTER_TESTS test_json_segmented_store)
endif()

foreach(test_name IN LISTS C_CLAW_ADAPTER_TESTS)
    cc_add_sdk_test(${test_name} "adapters/${test_name}.c")
endforeach()

if(CC_TARGET_PLATFORM_RESOLVED STREQUAL "posix" AND CC_ENABLE_SHELL)
    add_executable(test_process_capture "platform/posix/test_process_capture.c")
    target_link_libraries(test_process_capture PRIVATE c_claw_runtime)
    add_test(NAME process_capture_large_output COMMAND test_process_capture)
    set_tests_properties(process_capture_large_output PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

add_test(NAME sdk_core_forbidden_includes
    COMMAND "${CMAKE_COMMAND}"
        "-DCCLAW_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/.."
        -P "${CMAKE_CURRENT_LIST_DIR}/boundary/test_core_forbidden_includes.cmake")

add_test(NAME profile_esp32s31_tools_contract
    COMMAND "${CMAKE_COMMAND}"
        "-DCCLAW_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/.."
        -P "${CMAKE_CURRENT_LIST_DIR}/boundary/test_esp32s31_profile_contract.cmake")

add_test(NAME http_contract_source
    COMMAND "${CMAKE_COMMAND}"
        "-DCCLAW_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/.."
        -P "${CMAKE_CURRENT_LIST_DIR}/boundary/test_http_contract_source.cmake")

add_test(NAME packaging_consumer_install
    COMMAND "${CMAKE_COMMAND}"
        "-DCCLAW_BUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DCCLAW_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/.."
        "-DCCLAW_TEST_WORK_DIR=${CMAKE_BINARY_DIR}/packaging-consumer"
        -P "${CMAKE_CURRENT_LIST_DIR}/packaging/test_consumer_install.cmake")
