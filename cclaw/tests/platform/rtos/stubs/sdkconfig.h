#ifndef CC_TEST_SDKCONFIG_H
#define CC_TEST_SDKCONFIG_H
/* 本模型只覆盖普通任务路径，WithCaps/PSRAM 删除交由真实 IDF 构建和板测。 */
#define CONFIG_SPIRAM 0
#define CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM 0
#endif
