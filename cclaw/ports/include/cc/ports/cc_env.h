



#ifndef CC_ENV_H
#define CC_ENV_H

/*
 * 读取环境变量。
 *
 * 返回值由平台 C 库或 port 拥有，调用方不能释放，也不应长期保存后跨线程依赖其稳定性。
 * MCU/RTOS port 可以把它映射到配置表而不是进程环境。
 */
const char *cc_env_get(const char *key);

/* 读取整数环境变量；缺失或解析失败时返回 default_value。 */
int cc_env_get_int(const char *key, int default_value);

/*
 * 设置环境变量。
 *
 * POSIX/Windows 可写入进程环境；受限 MCU port 可以选择忽略或写入内存配置表。该接口
 * 不应用来分发 API key，敏感配置应由应用层安全管理。
 */
void cc_env_set(const char *key, const char *value);

#endif
