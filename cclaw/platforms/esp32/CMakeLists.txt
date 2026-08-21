



add_library(c_claw_platform_esp32_objects OBJECT
    src/cc_esp32_platform.c
    src/cc_esp32_filesystem.c
    src/cc_esp32_path.c
    src/cc_esp32_thread.c
    src/cc_esp32_secret_store.c
)

if(CC_NEEDS_HTTP_CLIENT)
    target_sources(c_claw_platform_esp32_objects PRIVATE src/cc_esp32_http_client.c)
endif()

target_link_libraries(c_claw_platform_esp32_objects PRIVATE
    c_claw_core_headers
    c_claw_ports_headers
    c_claw_project_options
)

set(CC_PLATFORM_PUBLIC_INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/include"
    PARENT_SCOPE)
set(CC_PLATFORM_PUBLIC_LIBRARIES "" PARENT_SCOPE)

if(ESP_PLATFORM)
    set(CC_ESP32_LIBC_TARGET idf::newlib)
    if(TARGET idf::esp_libc)
        set(CC_ESP32_LIBC_TARGET idf::esp_libc)
    endif()

    target_compile_definitions(c_claw_platform_esp32_objects PRIVATE
        ESP_PLATFORM=1
    )

    target_link_libraries(c_claw_platform_esp32_objects PRIVATE
        idf::freertos
        idf::heap
        ${CC_ESP32_LIBC_TARGET}
        idf::vfs
        idf::esp_system
        idf::efuse
        idf::nvs_flash
    )

    if(CC_NEEDS_HTTP_CLIENT)
        set(CC_ESP32_HTTP_LIBS
            idf::esp_event
            idf::esp_http_client
            idf::esp_netif
            idf::esp-tls
            idf::mbedtls
            idf::nvs_flash
        )
        if(TARGET idf::esp_eth)
            list(APPEND CC_ESP32_HTTP_LIBS idf::esp_eth)
        endif()
        target_link_libraries(c_claw_platform_esp32_objects PRIVATE
            ${CC_ESP32_HTTP_LIBS}
        )
        set(CC_PLATFORM_PUBLIC_LIBRARIES
            ${CC_ESP32_HTTP_LIBS}
            PARENT_SCOPE)
    endif()
endif()
