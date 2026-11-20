



#ifndef CC_ENV_H
#define CC_ENV_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file cc_env.h
 * @brief 环境变量端口声明，由采用此接口的外部平台实现提供链接定义。
 * @note 当前仓库内置平台没有这三个入口的实现或生产调用点。
 * 参数 NULL 支持、具体解析范围及跨线程同步须由所提供的实现明确，以下记录期望接口语义。
 */

/**
 * @brief 读取一个环境或平台配置值的借用字符串。
 * @param[in] key 调用期稳定的变量名文本，NULL 支持由实际 port 明确。
 * @return 平台保管的字符串借用，缺失值由实现约定，调用者不得修改或释放。
 * @note 借用有效期和并发 set 的失效规则由实现规定，长期使用者先取得独立副本。
 */
const char *cc_env_get(const char *key);

/**
 * @brief 按所提供 port 的整数解析规则读取变量，缺失或解析失败时采用默认值。
 * @param[in] key 调用期稳定变量名，NULL 支持同具体 port 契约。
 * @param[in] default_value 缺失或解析失败时返回的按值默认整数。
 * @return 解析结果或 default_value，溢出和尾部字符规则由实际实现明确。
 */
int cc_env_get_int(const char *key, int default_value);

/**
 * @brief 请求平台更新环境或配置表中的一个值。
 * @param[in] key 调用期稳定变量名，NULL 支持由实际 port 规定。
 * @param[in] value 调用期稳定值文本，NULL 的删除或错误语义由实现规定。
 * @post 平台按其可写能力处理请求，void 接口无法向调用者交付写入失败状态。
 * @note 输入借用覆盖同步调用，跨线程更新及后续借用寿命由实现同步，敏感配置由应用专用机制管理。
 */
void cc_env_set(const char *key, const char *value);

#ifdef __cplusplus
}
#endif

#endif
