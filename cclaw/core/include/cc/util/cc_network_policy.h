#ifndef CC_NETWORK_POLICY_H
#define CC_NETWORK_POLICY_H

#include "cc/util/cc_config.h"

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

/*
 * 判断 URL 是否被策略允许。
 *
 * 支持 host、host:port、*.domain、scheme://host、scheme://host:port。格式错误、userinfo、
 * unsupported scheme 默认拒绝；redirect 后的最终 URL 也必须重新调用该函数校验。
 */
int cc_network_policy_url_allowed(
    const cc_network_policy_t *policy,
    const char *url
);

/* DNS/连接层地址校验；域名 allowlist 命中后仍会拒绝意外解析到私网的地址。 */
int cc_network_policy_address_allowed(
    const cc_network_policy_t *policy,
    const char *host,
    const char *numeric_address
);

/* 只基于 allowlist 判断 URL；适合工具配置测试和旧调用路径。 */
int cc_network_allowlist_url_allowed(
    const cc_config_string_list_t *allowlist,
    const char *url
);

#endif
