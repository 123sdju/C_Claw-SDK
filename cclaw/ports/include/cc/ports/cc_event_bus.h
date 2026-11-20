#ifndef CC_EVENT_BUS_H
#define CC_EVENT_BUS_H

#include "cc/core/cc_result.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @brief 在发布线程或异步 worker 上处理一次已接纳事件。
 * @param[in] event_type 回调期间借用的非 NULL 主题，空串可用。
 * @param[in] event_json 回调期间借用的脱敏正文，空输入按空串提供，保留内容须自行复制。
 * @param[in,out] user_data 订阅者保管的借用上下文，允许 NULL，覆盖已接纳回调的退出。
 * @post 本次正文借用结束，异步同一订阅串行执行，同步并发或嵌套发布可重入同一 handler。
 * @note 回调可发布或以有限预算退订。flush/shutdown 会等待当前回调，拥有者在回调外完成销毁。
 */
typedef void (*cc_event_handler_fn)(
    const char *event_type,
    const char *event_json,
    void *user_data
);

/** @brief 默认同步调用，异步模式为各订阅保管独立邮箱并由共享 worker 调度。 */
typedef enum cc_event_bus_mode {
    CC_EVENT_BUS_MODE_SYNC = 0,
    CC_EVENT_BUS_MODE_ASYNC = 1,
} cc_event_bus_mode_t;

/** @brief 异步邮箱满时的处理规则，邮箱仍有空间时均正常追加。 */
typedef enum cc_event_overflow_policy {
    CC_EVENT_DROP_NEWEST = 0, /**< 丢弃本次新事件，发布仍可返回 CC_OK。 */
    CC_EVENT_DROP_OLDEST, /**< 释放最旧排队项，再追加本次事件。 */
    CC_EVENT_COALESCE_LATEST, /**< 用本次事件替换邮箱最后一个排队项。 */
    CC_EVENT_BLOCK_WITH_TIMEOUT, /**< 在订阅预算内等待空间，worker 自依赖时返回 QUEUE_FULL。 */
} cc_event_overflow_policy_t;

/** @brief 退订使用的值句柄，槽位回收时 generation 递增并跳过零。 */
typedef struct cc_event_subscription {
    uint16_t slot; /**< 总线内槽位索引，成功订阅时为 0 至 63。 */
    uint16_t generation; /**< 16 位公开代际，完整回绕后旧值可能别名，句柄只用于其订阅寿命。 */
} cc_event_subscription_t;

typedef struct cc_event_subscription_options {
    size_t size; /**< 结构字节数标识，当前实现按完整结构读取。 */
    cc_event_overflow_policy_t overflow_policy; /**< 非 NULL options 使用该枚举值，NULL options 默认 BLOCK_WITH_TIMEOUT。 */
    size_t mailbox_capacity; /**< 零采用总线默认值，正值最大归一到 16 项。 */
    int block_timeout_ms; /**< 邮箱等待总预算，单位毫秒，非正值采用 30000。 */
} cc_event_subscription_options_t;

typedef struct cc_event_bus_config {
    size_t size; /**< 结构字节数标识，当前实现按完整结构读取。 */
    cc_event_bus_mode_t mode; /**< SYNC 或 ASYNC，其他值返回参数错误。 */
    size_t worker_count; /**< 最大 64，异步零值在 ESP32/FreeRTOS 采用一，其他平台采用四。 */
    size_t mailbox_capacity; /**< 最大归一到 16，零在 ESP32/FreeRTOS 采用八，其他平台采用十六。 */
} cc_event_bus_config_t;

/** @brief 总线锁内复制的累计计数与高水位，延迟从信封创建计至回调完成。 */
typedef struct cc_event_bus_metrics {
    size_t size; /**< 本次输出结构字节数。 */
    uint64_t published; /**< 已进入分发阶段的发布次数，随后仍可能遇到关闭或部分投递错误。 */
    uint64_t matched; /**< 同步快照项或异步实际尝试投递的匹配次数。 */
    uint64_t hash_collisions; /**< 主题目录查询中遇到不同有效主题的累计次数。 */
    uint64_t delivered; /**< 回调完成次数，含同步及异步。 */
    uint64_t dropped_newest; /**< 丢新策略及 worker 防自依赖拒绝的累计次数。 */
    uint64_t dropped_oldest; /**< 丢弃最旧邮箱项的累计次数。 */
    uint64_t coalesced; /**< 替换最后一个邮箱项的累计次数。 */
    uint64_t block_time_ms; /**< 已记账的入队尝试耗时，单位毫秒，部分提前拒绝分支直接返回。 */
    uint64_t delivery_latency_sum_ms; /**< 信封创建至各回调完成的累计毫秒数。 */
    uint64_t delivery_latency_max_ms; /**< 信封创建至单个回调完成的最大毫秒数。 */
    size_t mailbox_high_water; /**< 任一订阅邮箱曾达到的最大条数。 */
    size_t active_subscribers; /**< 当前仍参与匹配的订阅数，退订等待中的槽已移出计数。 */
    size_t active_topics; /**< 当前有效精确主题数，通配订阅独立保管。 */
} cc_event_bus_metrics_t;

/**
 * @brief 拥有固定订阅槽、主题目录、异步邮箱及同步资源的总线。
 * @note owner 关闭外部入口并保护首次加锁登记窗口的句柄寿命，SDK 排空已登记发布者与等待 API。
 * 单一 owner 执行 destroy，可与已登记的 unsubscribe、flush、shutdown 等待调用重叠。
 */
typedef struct cc_event_bus cc_event_bus_t;

/** @brief 构造默认同步配置。 @return 按值返回完整 size、SYNC 及零值 worker/邮箱配置。 */
cc_event_bus_config_t cc_event_bus_default_config(void);
/**
 * @brief 采用默认同步配置创建总线。
 * @param[out] out_bus 必填，存在时先置 NULL，成功交付拥有句柄供 owner destroy。
 * @return 原样返回带配置创建的参数、分配或平台初始化结果。
 */
cc_result_t cc_event_bus_create(cc_event_bus_t **out_bus);
/**
 * @brief 复制配置值，创建固定容量总线及可选异步 worker。
 * @param[in] config 可选的完整配置，NULL 使用默认同步模式，仅在创建期间借用。
 * @param[out] out_bus 必填，存在时先置 NULL，成功返回拥有句柄，失败清理本次创建资源。
 * @return 成功为 CC_OK，模式或 worker 数非法为 INVALID_ARGUMENT，分配及线程端口错误向上传递。
 */
cc_result_t cc_event_bus_create_with_config(
    const cc_event_bus_config_t *config,
    cc_event_bus_t **out_bus
);

/**
 * @brief 复制主题并接纳订阅，保管借用 handler/user_data 直至回调退出与槽位回收。
 * @param[in,out] bus 借用的有效总线，关闭后返回 CANCELLED。
 * @param[in] event_type 调用期间借用，NULL 匹配所有主题，空串匹配空主题。
 * @param[in] handler 必填回调，调用时位于总线锁外，其代码覆盖订阅和全部已接纳回调。
 * @param[in,out] user_data 可选借用上下文，维持至退订排空或总线实际销毁。
 * @param[in] options 可选的完整配置，仅在本次读取，异步策略、容量及等待预算按字段归一。
 * @param[out] out_subscription 必填值输出，基础参数和策略通过后清零，成功写槽位及代际。
 * @return CC_OK 或参数、关闭、容量与分配错误，成功后由调用者保管句柄并退订或随总线清理。
 */
cc_result_t cc_event_bus_subscribe(
    cc_event_bus_t *bus,
    const char *event_type,
    cc_event_handler_fn handler,
    void *user_data,
    const cc_event_subscription_options_t *options,
    cc_event_subscription_t *out_subscription
);

/**
 * @brief 停止订阅匹配、丢弃其排队事件，并在预算内等待原订阅已接纳回调退出。
 * @param[in,out] bus 借用的有效总线，等待者在锁内登记并由销毁屏障覆盖。
 * @param[in] subscription 本总线成功返回的值句柄，旧代际返回 NOT_FOUND。
 * @param[in] timeout_ms 非负毫秒预算，零仅检查当前是否已排空。
 * @return 排空为 CC_OK，参数非法为 INVALID_ARGUMENT，代际不匹配为 NOT_FOUND，仍有回调为 TIMEOUT。
 * @note 超时后，已接纳同步快照中的回调仍可能尚待开始，user_data 继续保持到回调退出。
 * 最后一个回调返回时自动回收槽位，原句柄重试可返回 NOT_FOUND，内部等待按宽 epoch 隔离新订阅。
 */
cc_result_t cc_event_bus_unsubscribe(
    cc_event_bus_t *bus,
    cc_event_subscription_t subscription,
    int timeout_ms
);

/**
 * @brief 脱敏并复制事件，按订阅快照同步调用或分发到异步邮箱。
 * @param[in,out] bus 借用的有效总线，首次锁内接纳后保留 publisher 登记至全部投递尝试结束。
 * @param[in] event_type 调用期间借用的非 NULL 主题，空串可用，按字节区分主题。
 * @param[in] event_json 调用期间借用的正文，允许 NULL，脱敏后复制为信封拥有正文。
 * @return 同步回调结束或异步投递尝试完成为 CC_OK，丢新/合并策略也可成功；分配、关闭、等待或防自依赖错误返回非 OK。
 * @note 异步多个订阅可能部分投递后返回首个错误。内部 epoch 防止等待期间向复用槽的新订阅投递。
 */
cc_result_t cc_event_bus_publish(
    cc_event_bus_t *bus,
    const char *event_type,
    const char *event_json
);

/**
 * @brief 在当前模式等待已接纳发布者、邮箱和回调共同排空。
 * @param[in,out] bus 借用的有效总线，本次等待登记由 owner 销毁屏障覆盖。
 * @param[in] timeout_ms 非负毫秒预算，零仅检查当前状态。
 * @return 空闲为 CC_OK，参数非法为 INVALID_ARGUMENT，预算内未排空为 TIMEOUT。
 * @note flush 保持发布入口开放，外层关闭入口后可用作稳定排空屏障。handler 内调用会等待自身退出。
 */
cc_result_t cc_event_bus_flush(cc_event_bus_t *bus, int timeout_ms);
/**
 * @brief 关闭发布及订阅接纳，排空已接纳事件，并串行 join 异步 worker。
 * @param[in,out] bus 借用的有效总线，可与其他已登记 shutdown 调用重叠。
 * @param[in] timeout_ms 非负毫秒预算，用于排空和等待其他 join 拥有者；worker 退出后的实际 join 沿线程端口等待。
 * @return 成功为 CC_OK，参数、排空超时或线程 join 错误向上传递，失败保留关闭状态与可重试资源。
 */
cc_result_t cc_event_bus_shutdown(cc_event_bus_t *bus, int timeout_ms);
/**
 * @brief 在总线锁内复制指标，提供独立值快照。
 * @param[in] bus 可选借用总线，NULL 仍提供清零指标及有效 size。
 * @param[out] out_metrics 可选值输出，NULL 直接返回，其他情况先清零。
 * @post 输出为本次观察到的累计计数，拥有关系保持在总线内部。
 */
void cc_event_bus_get_metrics(cc_event_bus_t *bus, cc_event_bus_metrics_t *out_metrics);
/**
 * @brief 用三十秒总预算关闭并排空总线及已登记等待 API，成功后释放全部资源。
 * @pre 单一 owner 已关闭新入口，并维持首次加锁登记窗口的对象寿命，当前位于 handler 之外。
 * @param[in,out] bus 待消费拥有句柄，允许 NULL。
 * @post 排空成功后句柄失效，owner 清空外部引用；超时或 join 失败则保留关闭的总线，owner 保持句柄供稍后重试。
 */
void cc_event_bus_destroy(cc_event_bus_t *bus);

#endif
