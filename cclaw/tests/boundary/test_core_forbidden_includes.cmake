if(NOT DEFINED CCLAW_SOURCE_DIR)
    message(FATAL_ERROR "CCLAW_SOURCE_DIR is required")
endif()

file(GLOB_RECURSE core_files
    "${CCLAW_SOURCE_DIR}/core/include/*.h"
    "${CCLAW_SOURCE_DIR}/core/src/*.c"
    "${CCLAW_SOURCE_DIR}/core/src/*.h")

set(forbidden_include "#include[ \t]*[<\"](esp_|freertos/|driver/|windows\\.h|unistd\\.h|pthread\\.h|sys/socket|io\\.h)")
set(forbidden_core_token "(esp_timer|esp_heap_caps|heap_caps_|MALLOC_CAP_|sdkconfig|(^|[^A-Z0-9_])CONFIG_[A-Z0-9_]+|PSRAM|SPIRAM|esp_wifi|WIFI_PS_|TaskHandle_t|SemaphoreHandle_t|QueueHandle_t|xTaskCreate|vTask|uxTask)")
foreach(path IN LISTS core_files)
    file(READ "${path}" content)
    string(REGEX MATCH "${forbidden_include}" match "${content}")
    if(match)
        message(FATAL_ERROR "SDK core has forbidden platform include in ${path}: ${match}")
    endif()
    string(REGEX MATCH "${forbidden_core_token}" token_match "${content}")
    if(token_match)
        message(FATAL_ERROR "SDK core has forbidden ESP/platform-specific token in ${path}: ${token_match}")
    endif()
endforeach()
