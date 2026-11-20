# 静态守卫 HTTP 复用契约，避免仅靠文档保留关键安全与并发约束。
if(NOT DEFINED CCLAW_SOURCE_DIR)
    message(FATAL_ERROR "CCLAW_SOURCE_DIR is required")
endif()

set(ESP_HTTP "${CCLAW_SOURCE_DIR}/platforms/esp32/src/cc_esp32_http_client.c")
set(POSIX_HTTP "${CCLAW_SOURCE_DIR}/platforms/posix/src/cc_curl_http_client.c")
set(HTTP_TOOL "${CCLAW_SOURCE_DIR}/adapters/src/tools/common/cc_http_request_tool.c")
set(LLM_PROVIDER "${CCLAW_SOURCE_DIR}/adapters/src/llm/cc_http_llm_provider.c")

foreach(source_file IN ITEMS "${ESP_HTTP}" "${POSIX_HTTP}" "${HTTP_TOOL}" "${LLM_PROVIDER}")
    if(NOT EXISTS "${source_file}")
        message(FATAL_ERROR "Missing HTTP contract source: ${source_file}")
    endif()
endforeach()

file(READ "${ESP_HTTP}" esp_http_text)
file(READ "${POSIX_HTTP}" posix_http_text)
file(READ "${HTTP_TOOL}" http_tool_text)
file(READ "${LLM_PROVIDER}" llm_provider_text)

foreach(required IN ITEMS
        "http_pool_mutex_get_locked"
        "esp_http_client_delete_header"
        "method_changed"
        "ctx->pool->active_ctx"
        "http_validate_resolved_candidates"
        "esp_http_client_get_socket"
        "getpeername"
        "http_abort_from_event"
        "esp_http_client_close"
        "body_callback_error"
        "http_redirect_headers"
        "CC_HTTP_CAP_RESOLVED_ADDRESS_VALIDATION"
        "CC_HTTP_CAP_REDIRECT_VALIDATION")
    string(FIND "${esp_http_text}" "${required}" found_index)
    if(found_index EQUAL -1)
        message(FATAL_ERROR "ESP32 HTTP contract missing: ${required}")
    endif()
endforeach()

foreach(required IN ITEMS
        "request.connection_pool = \"default\""
        "request.retry_count = -1")
    string(FIND "${http_tool_text}" "${required}" found_index)
    if(found_index EQUAL -1)
        message(FATAL_ERROR "http.request contract missing: ${required}")
    endif()
endforeach()

foreach(required IN ITEMS
        "CC_HTTP_LLM_STREAM_MAX_PENDING_FRAME_BYTES"
        "Stream frame exceeded pending buffer limit")
    string(FIND "${llm_provider_text}" "${required}" found_index)
    if(found_index EQUAL -1)
        message(FATAL_ERROR "LLM stream contract missing: ${required}")
    endif()
endforeach()

set(OPENAI_PROVIDER "${CCLAW_SOURCE_DIR}/adapters/src/llm/cc_openai_provider.c")
if(NOT EXISTS "${OPENAI_PROVIDER}")
    message(FATAL_ERROR "Missing OpenAI provider source")
endif()
file(READ "${OPENAI_PROVIDER}" openai_provider_text)
foreach(required IN ITEMS
        "expected array or null"
        "cc_json_is_null(tool_calls)"
        "OpenAI stream tool_calls has invalid type")
    string(FIND "${openai_provider_text}" "${required}" found_index)
    if(found_index EQUAL -1)
        message(FATAL_ERROR "OpenAI stream compatibility contract missing: ${required}")
    endif()
endforeach()

string(FIND "${posix_http_text}" "next_http_attempt_id" found_index)
if(found_index EQUAL -1)
    message(FATAL_ERROR "POSIX attempt id contract missing")
endif()
#[[
  @file 从 CCLAW_SOURCE_DIR 读取 HTTP 相关源码并验证必要契约片段。
  检查覆盖静态文本，缺失文件或片段时终止脚本，读取内容只在本次进程中保留。
]]
