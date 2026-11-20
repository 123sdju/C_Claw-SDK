

#ifndef CC_TOOL_EXECUTOR_POOL_H
#define CC_TOOL_EXECUTOR_POOL_H

#include "cc/app/cc_cancel_token.h"
#include "cc/core/cc_result.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 用互斥量和条件变量按 lane 名称限制工具并发的执行池。
 *
 * 池负责许可计数，工具调用及执行超时由外层 executor 实现。
 * owner 在销毁前关闭入口、排空等待调用并归还全部票据，池本身没有关闭或排空入口。
 */
typedef struct cc_tool_executor_pool cc_tool_executor_pool_t;

/** @brief 单个命名 lane 的并发与工具超时配置，创建时复制名称。 */
typedef struct cc_tool_executor_pool_policy {
    const char *name; /**< 创建期间借用，NULL 跳过本项，空字符串可作为名称。 */
    int concurrency; /**< 同时持有许可的上限，小于等于零归一为一。 */
    int timeout_ms; /**< 工具默认超时毫秒数，小于等于零使用池默认值，等待许可的期限另行提供。 */
} cc_tool_executor_pool_policy_t;

/** @brief 执行池创建配置，未知名称按默认值建立独立 lane。 */
typedef struct cc_tool_executor_pool_config {
    int default_concurrency; /**< 动态 lane 默认并发，小于等于零归一为一。 */
    int default_timeout_ms; /**< 默认工具超时，单位毫秒，小于等于零使用 30000。 */
    const cc_tool_executor_pool_policy_t *policies; /**< 创建期间借用的数组，数量非零时须非 NULL。 */
    size_t policy_count; /**< 策略条目数，同名条目均保存，按名称查找时使用最先配置的一项。 */
} cc_tool_executor_pool_config_t;

/**
 * @brief 成功取得的 lane 许可，调用者须向原池恰好归还一次。
 *
 * 票据只记录 lane 索引，外层负责池身份及一次归还约束，失败输出保持原值。
 */
typedef struct cc_tool_executor_pool_ticket {
    size_t lane_index; /**< 原池内部的稳定 lane 索引，新增 lane 保持既有索引。 */
} cc_tool_executor_pool_ticket_t;

/** @brief 锁内复制的 lane 指标，名称继续借用池内字符串。 */
typedef struct cc_tool_executor_pool_lane_metrics {
    const char *name;        /**< 池拥有的名称借用，寿命至池销毁，使用时须继续保持池或 generation 有效。 */
    int concurrency;         /**< 配置并发上限 */
    int timeout_ms;          /**< 配置超时毫秒数 */
    int in_flight;           /**< 当前执行中 */
    int waiting;             /**< 当前等待获取 */
    int max_waiting;         /**< 历史最大等待数 */
    unsigned long acquired;  /**< 累计获取次数 */
    unsigned long released;  /**< 累计释放次数 */
    unsigned long cancelled; /**< 累计因取消或截止时间到达而拒绝获取的次数。 */
} cc_tool_executor_pool_lane_metrics_t;

/** @brief 同一次锁内扫描得到的总量和可选 lane 明细。 */
typedef struct cc_tool_executor_pool_metrics {
    int default_concurrency;    /**< 默认并发上限 */
    int default_timeout_ms;     /**< 默认超时毫秒数 */
    size_t lane_count;          /**< 已配置及动态建立的 lane 总数。 */
    size_t reported_lane_count; /**< 实际填充的 lane 条目数 */
    int total_in_flight;        /**< 全部 lane 执行中总数 */
    int total_waiting;          /**< 全部 lane 等待中总数 */
    int max_total_waiting;      /**< 历史最大总等待数 */
    unsigned long acquired;     /**< 累计总获取次数 */
    unsigned long released;     /**< 累计总释放次数 */
    unsigned long cancelled;    /**< 累计因取消或截止时间到达而拒绝获取的次数。 */
    cc_tool_executor_pool_lane_metrics_t *lanes; /**< 调用者提供的明细数组借用，可为 NULL，名称字段仍借用池。 */
} cc_tool_executor_pool_metrics_t;

/**
 * @brief 返回默认执行池配置。
 * @return 按值返回并发四、工具超时 30000 毫秒、空策略数组的配置。
 */
cc_tool_executor_pool_config_t cc_tool_executor_pool_default_config(void);

/**
 * @brief 分配池及同步资源，按配置预建 lane。
 * @param[in] config 可选借用配置，NULL 使用默认值，数组和名称只借用到创建返回。
 * @param[out] out_pool 必填拥有对象输出，成功由调用者 destroy，失败保持原值。
 * @return 成功返回 CC_OK，输出缺失或非零数量配空策略数组返回 CC_ERR_INVALID_ARGUMENT，
 * 资源或 lane 名称分配失败返回对应错误，失败清理本次已分配池。
 */
cc_result_t cc_tool_executor_pool_create(
    const cc_tool_executor_pool_config_t *config,
    cc_tool_executor_pool_t **out_pool
);

/**
 * @brief 释放所有 lane 名称、数组、同步资源和池对象。
 * @pre owner 已关闭入口，排空 acquire、查询和 release 调用，并归还全部票据。
 * @param[in,out] pool 待销毁池，可为 NULL。
 * @post 池及借出的名称均失效，销毁后禁止继续访问原句柄或票据。
 */
void cc_tool_executor_pool_destroy(cc_tool_executor_pool_t *pool);

/**
 * @brief 无取消信号及截止时间地等待并取得指定 lane 许可。
 * @param[in,out] pool 非空池，调用和后续票据使用期间由 owner 保证存活。
 * @param[in] lane_name 非空名称指针，空字符串可用，调用期间借用，未知名称按默认配置建立。
 * @param[out] out_ticket 必填独立输出，成功取得许可后必须向原池 release 一次，失败保持原值。
 * @return 返回参数、lane 分配或许可取得结果，CC_OK 表示已占用一个许可。
 * lane 满时可持续等待，工具超时配置用于外层执行阶段。
 */
cc_result_t cc_tool_executor_pool_acquire(
    cc_tool_executor_pool_t *pool,
    const char *lane_name,
    cc_tool_executor_pool_ticket_t *out_ticket
);


/**
 * @brief 使用可选取消令牌等待 lane 许可。
 * @param[in,out] pool 非空有效池，许可使用至归还期间保持存活。
 * @param[in] lane_name 非空指针的借用名称，未知名称按默认值创建。
 * @param[in] cancel_token 可选借用令牌，调用期间有效，等待中周期检查。
 * @param[out] out_ticket 必填独立输出，成功取得许可，失败保持原值。
 * @return 已取消返回 CC_ERR_CANCELLED，其他返回带截止时间入口的结果，成功须向原池归还一次。
 */
cc_result_t cc_tool_executor_pool_acquire_with_cancel(
    cc_tool_executor_pool_t *pool,
    const char *lane_name,
    cc_cancel_token_t *cancel_token,
    cc_tool_executor_pool_ticket_t *out_ticket
);

/**
 * @brief 在可选取消及单调时钟截止时间约束下等待 lane 许可。
 * @param[in,out] pool 非空池，内部锁保护名称查找、数组扩展及许可计数。
 * @param[in] lane_name 非空借用名称，调用期间稳定，空字符串可用。
 * @param[in] cancel_token 可选借用取消令牌，调用期间有效。
 * @param[in] deadline_ms 绝对单调时钟毫秒时间，零表示无截止时间。
 * @param[out] out_ticket 必填独立输出，成功填写许可索引，失败保持原值。
 * @return 成功返回 CC_OK，取消返回 CC_ERR_CANCELLED，到期返回 CC_ERR_TIMEOUT，参数或 lane 分配失败返回对应错误。
 * 先取得或创建 lane，再检查取消、到期和容量，失败也可能已新增空 lane。
 * 等待以 50 毫秒片段重新核对条件，实际响应还取决于端口与调度。
 */
cc_result_t cc_tool_executor_pool_acquire_with_deadline(
    cc_tool_executor_pool_t *pool,
    const char *lane_name,
    cc_cancel_token_t *cancel_token,
    uint64_t deadline_ms,
    cc_tool_executor_pool_ticket_t *out_ticket
);

/**
 * @brief 归还原池的一份 lane 许可并广播等待者。
 * @param[in,out] pool 票据所属池，可为 NULL，非空时寿命由外层保持。
 * @param[in] ticket 本次成功获取且尚未归还的原池票据，调用者保证恰好归还一次。
 * @post 有效且仍占用的 lane 计数减一并累计 released，非空池均广播，NULL 直接返回。
 */
void cc_tool_executor_pool_release(
    cc_tool_executor_pool_t *pool,
    cc_tool_executor_pool_ticket_t ticket
);

/**
 * @brief 查询 lane 的工具默认超时，未知名称会按默认配置建立 lane。
 * @param[in,out] pool 借用池，可为 NULL，查询可能扩展其 lane 数组。
 * @param[in] lane_name 借用名称，可为 NULL，空字符串可用。
 * @return 参数为空返回零，找到或建立成功返回该 lane 毫秒超时，建立失败返回池默认超时。
 */
int cc_tool_executor_pool_timeout_ms(
    cc_tool_executor_pool_t *pool,
    const char *lane_name
);

/**
 * @brief 在一次锁内扫描中复制总量及能放入调用者缓冲区的 lane 指标。
 * @param[in] pool 非空借用池，查询及明细名称使用期间保持有效。
 * @param[out] out_metrics 必填独立输出，参数有效后先清零，lanes 借用传入数组。
 * @param[out] lane_buffer 可选明细数组，名称字段借用池内字符串至池销毁，调用者保管数组存储。
 * @param[in] lane_buffer_count 明细数组元素容量，NULL 数组时实际 reported_lane_count 为零。
 * @return 参数缺失返回 CC_ERR_INVALID_ARGUMENT 并保留输出，成功返回 CC_OK。
 * lane_count 总是报告完整数量，reported_lane_count 报告实际复制数量，未写入的缓冲区元素保持原值。
 */
cc_result_t cc_tool_executor_pool_get_metrics(
    cc_tool_executor_pool_t *pool,
    cc_tool_executor_pool_metrics_t *out_metrics,
    cc_tool_executor_pool_lane_metrics_t *lane_buffer,
    size_t lane_buffer_count
);

#ifdef __cplusplus
}
#endif

#endif
