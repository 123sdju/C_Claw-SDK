

#ifndef CC_APP_FEATURES_H
#define CC_APP_FEATURES_H

#include "cc/app/cc_runtime_features.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 获取由当前编译开关组成的静态默认能力集合。
 * @return 始终返回只读借用，集合、描述符及回调在程序存活期有效，所有权由模块保管。
 * @note builder 借用此集合覆盖全部 generation 的创建和最终清理；可并发读取静态描述符。
 * compiled 为零的 provider/tool 仍保留描述符，可选工厂缺失由对应字段或描述符表达。
 */
const cc_runtime_feature_set_t *cc_app_default_features(void);

#ifdef __cplusplus
}
#endif

#endif
