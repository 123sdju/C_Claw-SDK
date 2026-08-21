

#ifndef CC_APP_FEATURES_H
#define CC_APP_FEATURES_H

#include "cc/app/cc_runtime_features.h"

/*
 * 返回当前构建的默认 feature set。
 *
 * 返回值是静态只读描述符，调用方不能释放或修改。runtime builder 用它按 profile 装配
 * provider、tools、storage 和扩展点。
 */
const cc_runtime_feature_set_t *cc_app_default_features(void);

#endif
