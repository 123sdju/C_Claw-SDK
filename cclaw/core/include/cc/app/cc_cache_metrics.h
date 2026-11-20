#ifndef CC_CACHE_METRICS_H
#define CC_CACHE_METRICS_H

#include "cc/core/cc_result.h"

typedef struct cc_cache_metrics cc_cache_metrics_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建仅根据已观察 SDK 事件累计的进程期指标对象。
 * @param[out] out_metrics 必填空拥有槽，入口清零，成功最终 destroy。
 * @return 成功为 CC_OK，参数、分配或锁创建失败返回拥有结果，调用者释放诊断。
 * @note 内部锁保护四个模型槽及摘要/记忆计数，事件交付按总线实际范围观察。
 */
cc_result_t cc_cache_metrics_create(cc_cache_metrics_t **out_metrics);

/**
 * @brief 消费指标对象及其私有锁。
 * @param[in,out] metrics 独占拥有对象，可为 NULL。
 * @pre 拥有者已关闭入口并等待全部已接纳方法及事件回调退出。
 * @post 对象与原别名失效。
 */
void cc_cache_metrics_destroy(cc_cache_metrics_t *metrics);

/**
 * @brief 设置缺少事件级来源时使用的后备 provider 名称。
 * @param[in,out] metrics 借用存活对象。
 * @param[in] provider 调用期间稳定的非 NULL 名称，少于 128 字节，返回前复制。
 * @return 成功为 CC_OK，参数错误返回拥有结果，调用者释放诊断。
 * @note 事件自身 provider 优先，原生锁失败按 void mutex 端口的实际能力处理。
 */
cc_result_t cc_cache_metrics_set_provider(cc_cache_metrics_t *metrics, const char *provider);

/**
 * @brief 观察用量或摘要缓存事件并更新内部饱和计数。
 * @param[in,out] metrics 借用存活对象。
 * @param[in] event_type 调用期借用的事件名，其他事件类型成功跳过。
 * @param[in] event_json 调用期借用的完整对象文本，解码 NUL 和坏文本返回错误。
 * @return 成功为 CC_OK，参数或解析失败返回拥有结果，调用者释放诊断。
 * @note 缺失、无效和明确零的缓存报告分别计数，名称过长或模型槽满计入遗漏。
 * 共享字段在同一锁期更新，拥有者覆盖全部已接纳调用的生命周期。
 */
cc_result_t cc_cache_metrics_observe(cc_cache_metrics_t *metrics, const char *event_type,
                                     const char *event_json);

/**
 * @brief 构造一个锁期内一致的指标 JSON 快照。
 * @param[in,out] metrics 借用存活对象。
 * @param[out] out_json 必填空拥有槽，入口清零，成功字符串由调用者 free。
 * @return 成功为 CC_OK，参数或构造分配失败返回拥有结果，调用者释放诊断。
 * @note u64 累计值经 JSON double 编码可能舍入，实际饱和标志及可用样本命中率单独输出。
 */
cc_result_t cc_cache_metrics_snapshot(cc_cache_metrics_t *metrics, char **out_json);

#ifdef __cplusplus
}
#endif

#endif
