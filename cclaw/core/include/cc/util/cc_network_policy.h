#ifndef CC_NETWORK_POLICY_H
#define CC_NETWORK_POLICY_H

#include "cc/util/cc_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 网络访问策略。
 *
 * allowlist 由调用方拥有，策略只在调用期间借用；allow_private_networks 为 0 时默认拒绝
 * localhost、loopback、private IP、link-local 等地址，除非后续策略显式允许。
 */
typedef struct cc_network_policy {
    size_t size;
    cc_config_string_list_t allowlist;
    int allow_private_networks;
} cc_network_policy_t;

/**
 * @brief 检查 HTTP(S) URL 是否命中可解析的端点 allowlist。
 * @param[in] policy 可选稳定策略，列表数组及其字符串在本次调用期间借用，非零 count 须有数组。
 * @param[in] url 可选稳定 URL，支持 IPv6 方括号与显式端口，拒绝 userinfo。
 * @return 命中为一，空策略、无匹配和结构错误为零，显式匹配可提供私网例外。
 * @note 默认端口未归一化，星点域名只匹配子域，DNS 和重定向后复核由传输层完成。
 */
int cc_network_policy_url_allowed(
    const cc_network_policy_t *policy,
    const char *url
);

/**
 * @brief 对 URL 已通过校验后的真实连接地址应用私网开关。
 * @param[in] policy 可选稳定策略，allow_private_networks 非零放行非空地址。
 * @param[in] host 保留的主机身份参数，当前实现未读取。
 * @param[in] numeric_address 可选稳定数值地址，调用者负责取得真实、格式正确的地址。
 * @return NULL 或空地址为零，其他按固定 IPv4/IPv6 范围和私网开关返回一或零。
 * @note 此函数不执行 DNS、不重复 allowlist 匹配，IPv4 嵌入 IPv6 按尾部 IPv4 分类。
 */
int cc_network_policy_address_allowed(
    const cc_network_policy_t *policy,
    const char *host,
    const char *numeric_address
);

/**
 * @brief 使用临时默认策略判断一个 URL 的显式 allowlist 匹配。
 * @param[in] allowlist 可选稳定列表，结构按值复制，数组和字符串仅借用。
 * @param[in] url 可选稳定 URL。
 * @return 与主 URL 校验一致的一或零，空 allowlist 为拒绝。
 */
int cc_network_allowlist_url_allowed(
    const cc_config_string_list_t *allowlist,
    const char *url
);

#ifdef __cplusplus
}
#endif

#endif
