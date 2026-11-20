#ifndef CC_TEST_ESP32_FILESYSTEM_LOG_H
#define CC_TEST_ESP32_FILESYSTEM_LOG_H

/* 将实际端口的警告交给测试内无分配的日志替身。 */
#define ESP_LOGW(...) fixture_log(__VA_ARGS__)

#endif
