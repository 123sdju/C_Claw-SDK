



#[[
  @brief 创建链接 SDK 的主机测试，并为它准备独立的工作目录。
  @pre c_claw_runtime 目标已创建，调用者提供当前目录可解析的测试源码。
  @param[in] test_name 同时用作可执行目标、CTest 名称及独立工作目录前缀。
  @param[in] source_path 相对本测试目录或绝对形式的源码路径。
  @post 注册目标及测试，建立 runtime/data 子目录，工作目录属性归对应测试使用。
  @return 无显式返回值，目标及 CTest 注册项由当前 CMake 构建树持有。
  @warning 目标重名、无效源码或目录创建失败由 CMake 报错，调用方负责按功能裁剪注册。
  @note 配置期串行调用，局部路径变量随调用结束释放，生成目录保留到构建树清理。
]]
function(cc_add_sdk_test test_name source_path)
    add_executable(${test_name} "${source_path}")
    target_link_libraries(${test_name} PRIVATE c_claw_runtime)
    add_test(NAME ${test_name} COMMAND ${test_name})
    set(test_work_dir "${CMAKE_CURRENT_BINARY_DIR}/${test_name}-work")
    file(MAKE_DIRECTORY "${test_work_dir}/runtime/data")
    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY "${test_work_dir}")
endfunction()

set(C_CLAW_CORE_TESTS
    test_thread_mutex
    test_tool_registry_freeze
    test_tool_registry_contract
    test_logger_concurrent
    test_event_bus_concurrent
    test_event_bus_async
    test_runtime_concurrent_sessions
    test_operation_journal
    test_storage_binding
    test_run_checkpoint
    test_cache_metrics
    test_cache_metrics_runtime
    test_runtime_stream_usage
    test_memory_extractor
    test_memory_plan
    test_memory_snapshot
    test_memory_pending_queue
    test_journaled_tool_step
    test_runtime_builder_tool_failure_cleanup
    test_runtime_builder_reload_report
    test_runtime_controller_config
    test_default_features
    test_runtime_request_config
    test_summary_cache_concurrent
    test_context_builder_failures
    test_config_missing_defaults
    test_config_allocation_failures
    test_config_runtime_sections
    test_cancel_token
    test_plugin_protocol_envelope
    test_sse_parser
    test_runtime_stream_callback
    test_memory_query_port
    test_memory_session_tail
    test_filesystem_binary_write
    test_path_security
    test_result_detail
    test_result_format
    test_tool_name
    test_json_edge_cases
    test_cjson_contract
    test_message_serialization_failures
    test_message_tool_contract
    test_media_contract
    test_util_boundaries
    test_observability_schema
    test_observability_contract
    test_observability_no_legacy_events
    test_redaction
)

# 这些测试直接创建内存 session 后端，裁剪后端时同步裁剪对应调用者。
if(NOT CC_ENABLE_SESSION_STORE_MEMORY)
    list(REMOVE_ITEM C_CLAW_CORE_TESTS
        test_runtime_concurrent_sessions test_cache_metrics_runtime
        test_runtime_stream_usage test_journaled_tool_step
        test_runtime_controller_config test_runtime_request_config
        test_summary_cache_concurrent test_runtime_stream_callback
        test_memory_session_tail)
endif()

foreach(test_name IN LISTS C_CLAW_CORE_TESTS)
    cc_add_sdk_test(${test_name} "core/${test_name}.c")
endforeach()

if(CC_ENABLE_OPENAI OR CC_ENABLE_OLLAMA OR CC_ENABLE_ANTHROPIC)
    cc_add_sdk_test(test_http_llm_provider_contract "adapters/test_http_llm_provider_contract.c")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(test_http_llm_provider_contract PRIVATE CC_TEST_HTTP_LLM_OOM=1)
        target_link_options(test_http_llm_provider_contract PRIVATE
            "-Wl,--wrap=malloc" "-Wl,--wrap=calloc" "-Wl,--wrap=free" "-Wl,--wrap=realloc")
    endif()
endif()

# 此只读源码检查以 SDK 根为路径基准，覆盖独立工作目录的默认设置。
set_tests_properties(test_observability_no_legacy_events PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/..")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    target_compile_definitions(test_cjson_contract PRIVATE CC_TEST_CJSON_THREADS=1 CC_TEST_CJSON_FORMAT=1)
    target_link_libraries(test_cjson_contract PRIVATE Threads::Threads)
    target_link_options(test_cjson_contract PRIVATE "-Wl,--wrap=snprintf")
    target_compile_definitions(test_sse_parser PRIVATE CC_TEST_SSE_OOM=1)
    target_link_options(test_sse_parser PRIVATE "-Wl,--wrap=calloc" "-Wl,--wrap=realloc" "-Wl,--wrap=free")
    target_compile_definitions(test_storage_binding PRIVATE CC_BINDING_TEST_WRAP_FREE=1)
    target_link_options(test_storage_binding PRIVATE "-Wl,--wrap=malloc" "-Wl,--wrap=free")
    cc_add_sdk_test(test_rtos_path_contract "platform/test_rtos_path_contract.c")
    cc_add_sdk_test(test_freertos_filesystem_disabled "platform/test_freertos_filesystem_contract.c")
    cc_add_sdk_test(test_freertos_filesystem_lfn "platform/test_freertos_filesystem_contract.c")
    cc_add_sdk_test(test_freertos_filesystem_sfn "platform/test_freertos_filesystem_contract.c")
    foreach(fatfs_model IN ITEMS test_freertos_filesystem_disabled test_freertos_filesystem_lfn test_freertos_filesystem_sfn)
        target_include_directories(${fatfs_model} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/platform/fatfs_stubs")
    endforeach()
    target_compile_definitions(test_freertos_filesystem_disabled PRIVATE CCLAW_FREERTOS_ENABLE_FATFS=0)
    target_compile_definitions(test_freertos_filesystem_lfn PRIVATE CCLAW_FREERTOS_ENABLE_FATFS=1 FF_USE_LFN=1 FF_FS_LOCK=2)
    target_compile_definitions(test_freertos_filesystem_sfn PRIVATE CCLAW_FREERTOS_ENABLE_FATFS=1 FF_USE_LFN=0 FF_FS_LOCK=0)
    cc_add_sdk_test(test_esp32_filesystem_contract "platform/test_esp32_filesystem_contract.c")
    cc_add_sdk_test(test_esp32_filesystem_sfn "platform/test_esp32_filesystem_contract.c")
    foreach(esp_fs_model IN ITEMS test_esp32_filesystem_contract test_esp32_filesystem_sfn)
        target_include_directories(${esp_fs_model} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/platform/esp32_filesystem_stubs")
    endforeach()
    target_compile_definitions(test_esp32_filesystem_sfn PRIVATE CC_TEST_ESP_SFN=1)
    target_compile_definitions(test_observability_contract PRIVATE CC_TEST_OBSERVABILITY_FAILURES=1)
    target_link_options(test_observability_contract PRIVATE
        "-Wl,--wrap=malloc" "-Wl,--wrap=calloc" "-Wl,--wrap=realloc")
    target_compile_definitions(test_tool_registry_contract PRIVATE CC_TEST_TOOL_REGISTRY_OOM=1)
    target_link_options(test_tool_registry_contract PRIVATE
        "-Wl,--wrap=malloc" "-Wl,--wrap=calloc" "-Wl,--wrap=free")
    target_compile_definitions(test_message_tool_contract PRIVATE CC_TEST_MESSAGE_TOOL_OOM=1)
    target_link_options(test_message_tool_contract PRIVATE
        "-Wl,--wrap=malloc" "-Wl,--wrap=calloc" "-Wl,--wrap=realloc" "-Wl,--wrap=free")
    cc_add_sdk_test(test_posix_filesystem_contract "platform/test_posix_filesystem_contract.c")
    cc_add_sdk_test(test_windows_filesystem_contract "platform/test_windows_filesystem_contract.c")
    target_include_directories(test_windows_filesystem_contract PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/filesystem_stubs")
    target_compile_definitions(test_util_boundaries PRIVATE CC_TEST_UTIL_FAILURES=1)
    target_link_options(test_util_boundaries PRIVATE
        "-Wl,--wrap=realloc" "-Wl,--wrap=malloc" "-Wl,--wrap=vsnprintf")
    cc_add_sdk_test(test_freertos_thread_contract_stubs "platform/rtos/test_thread_contract_stubs.c")
    cc_add_sdk_test(test_esp32_thread_contract_stubs "platform/rtos/test_thread_contract_stubs.c")
    foreach(thread_model IN ITEMS test_freertos_thread_contract_stubs test_esp32_thread_contract_stubs)
        target_include_directories(${thread_model} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/platform/rtos/stubs")
        target_link_libraries(${thread_model} PRIVATE Threads::Threads)
    endforeach()
    target_compile_definitions(test_esp32_thread_contract_stubs PRIVATE CC_TEST_ESP32_THREAD=1)
    cc_add_sdk_test(test_windows_thread_contract_stubs "platform/windows/test_thread_contract_stubs.c")
    target_include_directories(test_windows_thread_contract_stubs PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/platform/windows/stubs")
    cc_add_sdk_test(test_windows_process_contract_stubs "platform/windows/test_process_contract_stubs.c")
    target_include_directories(test_windows_process_contract_stubs PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/platform/windows/process_stubs")
    cc_add_sdk_test(test_posix_curl_http_contract "platform/test_curl_http_contract.c")
    cc_add_sdk_test(test_windows_curl_http_contract "platform/test_curl_http_contract.c")
    foreach(curl_model IN ITEMS test_posix_curl_http_contract test_windows_curl_http_contract)
        target_include_directories(${curl_model} PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/../platforms/posix/vendor/curl/include")
        target_link_libraries(${curl_model} PRIVATE Threads::Threads)
    endforeach()
    target_compile_definitions(test_posix_curl_http_contract PRIVATE CC_TEST_WINDOWS_CURL=0)
    target_compile_definitions(test_windows_curl_http_contract PRIVATE CC_TEST_WINDOWS_CURL=1)
    target_include_directories(test_windows_curl_http_contract PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/platform/curl_stubs")
    cc_add_sdk_test(test_posix_curl_http_unavailable "platform/test_curl_http_unavailable.c")
    cc_add_sdk_test(test_windows_curl_http_unavailable "platform/test_curl_http_unavailable.c")
    foreach(curl_unavailable IN ITEMS test_posix_curl_http_unavailable test_windows_curl_http_unavailable)
        target_link_libraries(${curl_unavailable} PRIVATE Threads::Threads)
    endforeach()
    target_compile_definitions(test_posix_curl_http_unavailable PRIVATE CC_TEST_WINDOWS_CURL=0)
    target_compile_definitions(test_windows_curl_http_unavailable PRIVATE CC_TEST_WINDOWS_CURL=1)
    target_include_directories(test_windows_curl_http_unavailable PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/platform/curl_stubs")
    if(TARGET test_runtime_controller_config)
        target_compile_definitions(test_runtime_controller_config PRIVATE CC_TEST_GENERATION_ADMISSION=1)
        target_link_options(test_runtime_controller_config PRIVATE "-Wl,--wrap=cc_runtime_builder_acquire_generation")
    endif()
    target_compile_definitions(test_runtime_builder_reload_report PRIVATE CC_TEST_BUILDER_RELOAD_REPORT=1)
    target_link_options(test_runtime_builder_reload_report PRIVATE "-Wl,--wrap=cc_mutex_unlock")
    if(CC_ENABLE_TOOL_POOL)
        target_compile_definitions(test_runtime_builder_tool_failure_cleanup PRIVATE CC_TEST_BUILDER_POLICY_OOM=1)
        target_link_options(test_runtime_builder_tool_failure_cleanup PRIVATE
            "-Wl,--wrap=cc_tool_registry_freeze" "-Wl,--wrap=malloc")
    endif()
    target_compile_definitions(test_event_bus_concurrent PRIVATE CC_TEST_EVENT_LIFETIME_BARRIERS=1)
    target_link_options(test_event_bus_concurrent PRIVATE
        "-Wl,--wrap=cc_cond_timedwait" "-Wl,--wrap=cc_thread_join" "-Wl,--wrap=cc_platform_monotonic_ms")
    target_compile_definitions(test_event_bus_async PRIVATE CC_TEST_EVENT_ALLOCATION_FAILURES=1)
    target_link_options(test_event_bus_async PRIVATE "-Wl,--wrap=malloc")
    target_compile_definitions(test_result_format PRIVATE CC_TEST_RESULT_FORMAT_FAILURES=1)
    target_link_options(test_result_format PRIVATE "-Wl,--wrap=vsnprintf" "-Wl,--wrap=malloc")
    target_compile_definitions(test_tool_name PRIVATE CC_TEST_TOOL_NAME_OOM=1)
    target_link_options(test_tool_name PRIVATE "-Wl,--wrap=malloc")
endif()

if(CC_ENABLE_RUN_QUEUE)
    cc_add_sdk_test(test_run_queue_session_serial "core/test_run_queue_session_serial.c")
    cc_add_sdk_test(test_run_queue_async_interrupt "core/test_run_queue_async_interrupt.c")
    if(CC_ENABLE_MULTI_AGENT AND CC_ENABLE_SESSION_STORE_MEMORY)
        cc_add_sdk_test(test_agent_manager_lifetime "core/test_agent_manager_lifetime.c")
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            target_compile_definitions(test_agent_manager_lifetime PRIVATE CC_TEST_MANAGER_QUEUE_PUBLICATION=1)
            target_link_options(test_agent_manager_lifetime PRIVATE
                "-Wl,--wrap=cc_run_queue_submit_with_token"
                "-Wl,--wrap=cc_run_queue_collect")
        endif()
    endif()
endif()

if(CC_ENABLE_MEMORY)
    cc_add_sdk_test(test_memory_context_filter "core/test_memory_context_filter.c")
    if(CC_ENABLE_SESSION_STORE_MEMORY)
        cc_add_sdk_test(test_memory_context_trust "core/test_memory_context_trust.c")
    endif()
    if(CC_ENABLE_MEMORY_STORE_JSON_FILE)
        cc_add_sdk_test(test_memory_store_recovery "adapters/test_memory_store_recovery.c")
        cc_add_sdk_test(test_managed_memory_transaction "adapters/test_managed_memory_transaction.c")
        cc_add_sdk_test(test_managed_memory_worker "adapters/test_managed_memory_worker.c")
    endif()
endif()

if(CC_ENABLE_TOOL_POOL)
    cc_add_sdk_test(test_tool_executor_pool_lane "core/test_tool_executor_pool_lane.c")
    cc_add_sdk_test(test_tool_lane_names "core/test_tool_lane_names.c")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(test_tool_lane_names PRIVATE CC_TEST_TOOL_LANE_ALLOCATION=1)
        target_link_options(test_tool_lane_names PRIVATE "-Wl,--wrap=malloc")
    endif()
endif()

if(CC_ENABLE_SKILLS)
    cc_add_sdk_test(test_skill_catalog_prompt "core/test_skill_catalog_prompt.c")
endif()

if(CC_ENABLE_MCP)
    cc_add_sdk_test(test_mcp_jsonrpc_matcher "core/test_mcp_jsonrpc_matcher.c")
    cc_add_sdk_test(test_mcp_runtime_manager_fake_transport "core/test_mcp_runtime_manager_fake_transport.c")
    cc_add_sdk_test(test_mcp_tool_namespace "core/test_mcp_tool_namespace.c")
    cc_add_sdk_test(test_mcp_protocol_lifecycle "core/test_mcp_protocol_lifecycle.c")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(test_mcp_protocol_lifecycle PRIVATE CC_MCP_TEST_WRAP_FREE=1)
        target_link_options(test_mcp_protocol_lifecycle PRIVATE "-Wl,--wrap=free")
        foreach(mcp_clock_test IN ITEMS test_mcp_runtime_manager_fake_transport test_mcp_protocol_lifecycle)
            target_compile_definitions(${mcp_clock_test} PRIVATE CC_MCP_TEST_WRAP_CLOCK=1)
            target_link_options(${mcp_clock_test} PRIVATE "-Wl,--wrap=cc_platform_monotonic_ms")
        endforeach()
    endif()
endif()

set(C_CLAW_ADAPTER_TESTS
    test_http_tool_policy_contract
    test_memory_storage_contract
    test_inmem_memory_store_failures
    test_memory_tool_budget
    test_memory_store_concurrent
    test_storage_factory_failfast
    test_tool_executor_policy_approval
    test_message_envelope_serialization
    test_multimodal_content_parts
    test_tool_schema_validation
)

if(NOT CC_ENABLE_SESSION_STORE_MEMORY)
    list(REMOVE_ITEM C_CLAW_ADAPTER_TESTS
        test_memory_store_concurrent test_message_envelope_serialization
        test_multimodal_content_parts test_tool_executor_policy_approval
        test_tool_schema_validation)
endif()

if(NOT CC_ENABLE_MEMORY)
    list(REMOVE_ITEM C_CLAW_ADAPTER_TESTS test_memory_tool_budget)
endif()

if(CC_ENABLE_FILE_TOOLS)
    list(APPEND C_CLAW_ADAPTER_TESTS test_file_tool_contract)
endif()

if(CC_ENABLE_OPENAI AND CC_ENABLE_OLLAMA AND CC_ENABLE_ANTHROPIC)
    list(APPEND C_CLAW_ADAPTER_TESTS test_provider_stream_protocols)
endif()
if(CC_ENABLE_ANTHROPIC)
    list(APPEND C_CLAW_ADAPTER_TESTS test_anthropic_usage)
endif()

if(CC_ENABLE_HTTP_TOOL)
    list(APPEND C_CLAW_ADAPTER_TESTS test_http_allowlist)
endif()

if(CC_ENABLE_SESSION_STORE_SQLITE)
    list(APPEND C_CLAW_ADAPTER_TESTS test_sqlite_memory_failures test_sqlite_session_store_failures)
endif()

if(CC_ENABLE_SESSION_STORE_JSON_FILE)
    list(APPEND C_CLAW_ADAPTER_TESTS test_json_file_store_failures)
endif()

if(CC_ENABLE_SESSION_STORE_JSON_SEGMENTED AND CC_ENABLE_MEMORY_STORE_JSON_FILE)
    list(APPEND C_CLAW_ADAPTER_TESTS test_json_segmented_store test_segmented_recovery test_run_resume test_segmented_store_failures)
    if(CC_ENABLE_SESSION_STORE_MEMORY)
        list(APPEND C_CLAW_ADAPTER_TESTS test_tool_outcome_storage test_recovery_bootstrap)
    endif()
endif()

foreach(test_name IN LISTS C_CLAW_ADAPTER_TESTS)
    cc_add_sdk_test(${test_name} "adapters/${test_name}.c")
endforeach()

if(TARGET test_tool_executor_policy_approval AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_tool_executor_policy_approval PRIVATE CC_TEST_EXECUTOR_DISPATCH_CLOCK=1)
    target_link_options(test_tool_executor_policy_approval PRIVATE
        "-Wl,--wrap=cc_platform_monotonic_ms" "-Wl,--wrap=free")
endif()

if(CC_ENABLE_HTTP_TOOL)
    target_compile_definitions(test_http_tool_policy_contract PRIVATE CC_TEST_HTTP_TOOL_CONTRACT=1)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_http_tool_policy_contract PRIVATE CC_TEST_HTTP_POLICY_OOM=1)
    target_link_options(test_http_tool_policy_contract PRIVATE
        "-Wl,--wrap=malloc" "-Wl,--wrap=calloc" "-Wl,--wrap=free")
endif()

if(TARGET test_file_tool_contract AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_file_tool_contract PRIVATE CC_TEST_FILE_TOOL_OOM=1)
    target_link_options(test_file_tool_contract PRIVATE
        "-Wl,--wrap=malloc" "-Wl,--wrap=calloc" "-Wl,--wrap=free")
endif()

if(TARGET test_provider_stream_protocols AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_provider_stream_protocols PRIVATE CC_TEST_PROVIDER_OOM=1)
    target_link_options(test_provider_stream_protocols PRIVATE "-Wl,--wrap=malloc")
endif()

if(CC_TARGET_PLATFORM_RESOLVED STREQUAL "posix" AND CC_ENABLE_SHELL)
    add_executable(test_process_capture "platform/posix/test_process_capture.c")
    target_link_libraries(test_process_capture PRIVATE c_claw_runtime)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(test_process_capture PRIVATE CC_TEST_PROCESS_FAILURES=1)
        target_link_options(test_process_capture PRIVATE
            "-Wl,--wrap=realloc" "-Wl,--wrap=fcntl" "-Wl,--wrap=waitpid")
    endif()
    add_test(NAME process_capture_large_output COMMAND test_process_capture)
    set_tests_properties(process_capture_large_output PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

add_test(NAME sdk_core_forbidden_includes
    COMMAND "${CMAKE_COMMAND}"
        "-DCCLAW_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/.."
        -P "${CMAKE_CURRENT_LIST_DIR}/boundary/test_core_forbidden_includes.cmake")

add_test(NAME http_contract_source
    COMMAND "${CMAKE_COMMAND}"
        "-DCCLAW_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/.."
        -P "${CMAKE_CURRENT_LIST_DIR}/boundary/test_http_contract_source.cmake")

add_test(NAME packaging_consumer_install
    COMMAND "${CMAKE_COMMAND}"
        "-DCCLAW_BUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DCCLAW_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/.."
        "-DCCLAW_TEST_WORK_DIR=${CMAKE_BINARY_DIR}/packaging-consumer"
        "-DCCLAW_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
        -P "${CMAKE_CURRENT_LIST_DIR}/packaging/test_consumer_install.cmake")
