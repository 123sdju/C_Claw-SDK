

#ifndef CC_RUNTIME_BUILDER_H
#define CC_RUNTIME_BUILDER_H

#include "cc/app/cc_agent_manager.h"
#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_runtime_features.h"
#include "cc/util/cc_config.h"
#include "cc/ports/cc_logger.h"

#include <stdint.h>


/**
 * @brief 拥有共享端口和当前/退役 runtime 代际的装配根。
 * features、恢复账本与检查点由外层保活。代际锁协调取得、重载、维护与关闭，
 * owner 先关闭入口并排空外部 API，再按 destroy 结果等待或重试释放。
 */
typedef struct cc_runtime_builder cc_runtime_builder_t;
/** @brief 持有一份引用即保活该代 runtime、provider、工具、技能和扩展状态的租约。 */
typedef struct cc_runtime_generation cc_runtime_generation_t;

/**
 * @brief 调用者拥有的按值重载报告，固定数组中的组件名和诊断可能截断。
 * 接纳重载后初始化，成功填写代号及已创建组件标志，准备失败记录组件并保留旧当前代。
 * 参数、关闭或重载已在进行的早期拒绝保留原报告，工厂文件或外部副作用由各组件约定。
 */
typedef struct cc_runtime_reload_report {
    unsigned long old_generation;
    unsigned long new_generation;
    int tools_reloaded;
    int plugins_reloaded;
    int mcp_reloaded;
    int skills_reloaded;
    int tool_pool_reloaded;
    int rolled_back;
    char failed_component[32];
    char message[160];
    cc_runtime_diagnostics_t diagnostics;
} cc_runtime_reload_report_t;

/**
 * @brief 按配置和工厂装配共享端口、初始代及可选 manager/队列。
 * @param[in] config 非空配置，本次调用期间稳定，各工厂按自身契约复制需长期使用的内容。
 * @param[in] features 非空借用工厂集合及描述符，须覆盖 builder 和全部代的创建及销毁回调。
 * @param[out] out_builder 必填，基础参数、必需工厂及非零计数的描述符数组检查通过后置 NULL，成功取得拥有句柄。
 * @return CC_OK 或参数、工厂、分配和平台错误，失败清理本次已装配资源，创建记忆失败可降为诊断。
 * 非零 provider/tool 数量缺少数组时在平台初始化前返回参数错误，保留 out_builder 原值。
 */
cc_result_t cc_runtime_builder_create(
    const cc_config_t *config,
    const cc_runtime_feature_set_t *features,
    cc_runtime_builder_t **out_builder
);

/**
 * @brief 在代际静止时配置借用账本及工具必需标志。
 * @param[in,out] builder 非空存活对象，无租约、重载、退役代及锁外回收。
 * @param[in] journal 可选外部账本，覆盖 builder 与所有代的使用期。
 * @param[in] required 非零要求工具必须有账本，NULL 账本配合 required 关闭工具派发。
 * @return CC_OK 或参数/状态错误，已有检查点时须保留非空 required 账本，失败保持原配置。
 */
cc_result_t cc_runtime_builder_set_operation_journal(
    cc_runtime_builder_t *builder, cc_operation_journal_t *journal, int required);
/**
 * @brief 在尚未附加恢复资源的空闲实例上要求账本，暂时关闭工具派发。
 * @param[in,out] builder 非空对象，无活动代际操作或既有账本/检查点。
 * @return CC_OK 或参数/状态错误，成功后普通模型请求仍可运行。
 */
cc_result_t cc_runtime_builder_require_recovery(cc_runtime_builder_t *builder);

/**
 * @brief 在代际静止时配置检查点与精确历史查询，清除先前 visitor。
 * @param[in,out] builder 非空对象，无重载、租约、退役代或回收。
 * @param[in] checkpoints 借用外部存储，与 history_find 同时为 NULL 可清除该配置。
 * @param[in] history_find 配套借用回调，存在检查点时必填，覆盖全部代的使用期。
 * @return CC_OK 或参数/状态错误，附加非空检查点须先配置非空 required 账本，失败保持原配置。
 */
cc_result_t cc_runtime_builder_set_run_recovery(
    cc_runtime_builder_t *builder, cc_run_checkpoint_store_t *checkpoints,
    cc_run_history_find_fn history_find);

/**
 * @brief 原子附加 required 账本、检查点和精确查询，不提供历史 visitor。
 * @param[in,out] builder 非空空闲对象，尚无既有恢复资源或代际使用者。
 * @param[in] journal 必填借用账本，覆盖 builder 及全部代。
 * @param[in] checkpoints 必填借用检查点存储，寿命同账本。
 * @param[in] history_find 必填借用精确查询回调。
 * @return CC_OK 或参数/状态错误，失败保留全部原配置。
 */
cc_result_t cc_runtime_builder_set_recovery_bundle(
    cc_runtime_builder_t *builder, cc_operation_journal_t *journal,
    cc_run_checkpoint_store_t *checkpoints, cc_run_history_find_fn history_find);

/**
 * @brief 在代际锁内原子附加完整恢复 bundle 和可选有界内存历史 visitor。
 * @param[in,out] builder 非空空闲对象，尚无既有账本、检查点、引用、重载、退役代或回收。
 * @param[in] journal 必填借用账本，覆盖 builder 和全部代。
 * @param[in] checkpoints 必填借用检查点存储，寿命同账本。
 * @param[in] history_find 必填借用精确查询函数。
 * @param[in] history_visit 可选借用完整历史访问函数，调用上下文由存储端口提供。
 * @return CC_OK 或参数/状态错误，成功同时更新 builder 与当前 runtime，失败保留原配置。
 */
cc_result_t cc_runtime_builder_set_recovery_bundle_with_visitor(
    cc_runtime_builder_t *builder, cc_operation_journal_t *journal,
    cc_run_checkpoint_store_t *checkpoints, cc_run_history_find_fn history_find,
    cc_run_history_visit_fn history_visit);

/**
 * @brief 在锁内取得当前代的一份引用，允许重载期间继续使用已发布当前代。
 * @param[in,out] builder 存活借用装配根，API 期间由外层保活。
 * @param[out] out_generation 必填，参数有效后置 NULL，成功写须配对一次 release 的租约。
 * @return CC_OK 或参数/关闭状态错误，关闭后拒绝新租约。
 */
cc_result_t cc_runtime_builder_acquire_generation(
    cc_runtime_builder_t *builder,
    cc_runtime_generation_t **out_generation
);
/**
 * @brief 归还一份代际引用，退役代最后一份释放时同步清理其拥有资源。
 * @param[in] generation 消费尚未归还的引用，允许 NULL。
 * @post 该份租约及其派生借用结束，退役代可能已销毁，owner 的回收屏障保护清理过程。
 */
void cc_runtime_generation_release(cc_runtime_generation_t *generation);
/**
 * @brief 由有效代际引用借出 runtime。
 * @param[in] generation 尚未 release 的引用，允许 NULL。
 * @return NULL 代返回 NULL，其他为该代 runtime 借用，在此引用 release 前结束使用。
 */
cc_agent_runtime_t *cc_runtime_generation_runtime(cc_runtime_generation_t *generation);
/**
 * @brief 读取有效代际引用的固定版本号。
 * @param[in] generation 尚未 release 的引用，允许 NULL。
 * @return 非空代的 id，NULL 返回零。
 */
unsigned long cc_runtime_generation_id(const cc_runtime_generation_t *generation);
/**
 * @brief 借用该代 provider 端口，借用止于当前引用 release。
 * @param[in] generation 有效持有的代际引用，允许 NULL。
 * @return 存在 provider vtable 时返回端口，其他情况 NULL，调用者不销毁。
 * @note 外层按 provider 的实际并发能力同步共享请求。
 */
cc_llm_provider_t *cc_runtime_generation_borrow_provider(cc_runtime_generation_t *generation);
/**
 * @brief 借用跨 generation 保持的 builder 记忆端口。
 * @param[in] builder 存活对象，允许 NULL，借用期由外层排除销毁。
 * @return self/vtable 均存在时返回端口，否则 NULL，直接使用者须在 builder 销毁前退出。
 */
cc_memory_store_t *cc_runtime_builder_borrow_memory_store(cc_runtime_builder_t *builder);

/**
 * @brief 在 builder 代际锁内同步执行记忆维护。
 * @param[in,out] store 当前有效记忆端口，缺失时 NULL，仅本次回调期间借用。
 * @param[in,out] user_data 外层同步借用上下文，允许 NULL。
 * @return 拥有的维护结果交给调用方，存储副作用按回调实现解释。
 * @note 外层先排空直接使用者，回调禁止重入 builder API 或保存端口。
 */
typedef cc_result_t (*cc_runtime_memory_maintenance_fn)(cc_memory_store_t *store, void *user_data);
/**
 * @brief 无引用、退役代、回收或重载时取得维护屏障，锁内调用回调。
 * @param[in,out] builder 非空存活对象，关闭后拒绝维护。
 * @param[in] callback 必填同步维护函数，端口缺失仍会以 NULL 调用。
 * @param[in,out] user_data 回调借用上下文，允许 NULL，覆盖同步调用。
 * @return 参数/状态错误或回调拥有结果，由调用方 cc_result_free。
 */
cc_result_t cc_runtime_builder_maintain_memory_quiescent(
    cc_runtime_builder_t *builder, cc_runtime_memory_maintenance_fn callback, void *user_data);

/**
 * @brief 借用可选的多 agent 管理器。
 * @param[in] builder 存活对象，允许 NULL，外层排除并发销毁。
 * @return 未启用 multi-agent/queue 或对象为空时 NULL，其他借用在 manager 销毁前结束。
 */
cc_agent_manager_t *cc_runtime_builder_agent_manager(cc_runtime_builder_t *builder);

/**
 * @brief 借用启动或最近成功重载发布的诊断集合。
 * @param[in] builder 存活对象，允许 NULL，读取期间外层排除成功重载写入及销毁。
 * @return NULL 对象返回 NULL，其他为只读借用，调用者不释放或修改。
 */
const cc_runtime_diagnostics_t *cc_runtime_builder_diagnostics(cc_runtime_builder_t *builder);

/**
 * @brief 按新配置准备并发布 provider、runtime、工具、技能与池的新代。
 * @param[in,out] builder 存活对象，共享 store、policy、sandbox、HTTP、队列及恢复配置保留。
 * @param[in] config 调用期间稳定的非空配置，工厂复制所需长期内容。
 * @return CC_OK 或参数、状态、工厂/资源错误，失败保留旧当前代，返回结果由调用方清理。
 */
cc_result_t cc_runtime_builder_reload(
    cc_runtime_builder_t *builder,
    const cc_config_t *config
);

/**
 * @brief 构建新代后锁内提交并返回报告，旧代保留到其全部租约归还。
 * @param[in,out] builder 存活对象，关闭或另一重载准备期间拒绝调用。
 * @param[in] config 非空借用配置，覆盖本次调用，复用创建时的 features 及共享端口。
 * @param[out] out_report 可选调用方报告，接纳后初始化，早期参数/状态拒绝保留原值。
 * @return CC_OK 或参数、状态、工厂及资源错误，失败保留旧当前代并清理新资源。
 * @note 报告的回滚标志表示代际未切换，工厂文件及外部系统副作用由对应实现负责。
 */
cc_result_t cc_runtime_builder_reload_with_report(
    cc_runtime_builder_t *builder,
    const cc_config_t *config,
    cc_runtime_reload_report_t *out_report
);

/**
 * @brief 在代际锁内发布关闭并唤醒等待者。
 * @param[in,out] builder 存活借用对象，允许 NULL。
 * @post 非空对象拒绝新代际租约与重载，任务取消、排空和释放由后续关闭步骤负责。
 */
void cc_runtime_builder_request_shutdown(cc_runtime_builder_t *builder);

/**
 * @brief 借用 builder 拥有的 logger。
 * @param[in] builder 存活对象，允许 NULL，外层排除销毁。
 * @return NULL 对象返回 NULL，其他为只读拥有关系下的日志接口借用，调用者不销毁。
 */
cc_logger_t *cc_runtime_builder_logger(cc_runtime_builder_t *builder);

/**
 * @brief 请求该 builder 的 HTTP 后端重置连接池。
 * @param[in,out] builder 存活非空对象，HTTP 及其回调寿命由外层保证。
 * @return 参数错误或 HTTP 端口原样结果，连接/在途请求影响由后端契约定义。
 */
cc_result_t cc_runtime_builder_reset_http_connections(cc_runtime_builder_t *builder);

/**
 * @brief 关闭准入、排空队列与代际/事件屏障，再按依赖次序释放装配根。
 * @pre owner 已关闭入口并排空外部 API，当前线程位于该 builder 待退出的任务/回调之外。
 * @param[in,out] builder 消费拥有对象，NULL 成功，失败仍由调用者保管并可重试。
 * @param[in] timeout_ms 毫秒预算，代际和事件共享剩余预算，队列按其 UINT_MAX 等待约定处理。
 * @return 完成释放为 CC_OK，队列、代际或事件排空失败向上传递，保留尚未释放的依赖。
 * @note 失败后保持关闭，部分上层对象可能已释放并置空，成功后句柄失效。
 * 具体端口析构及锁等待按各自契约执行，调用方不能把预算解释为全函数硬实时上限。
 */
cc_result_t cc_runtime_builder_destroy(cc_runtime_builder_t *builder, uint32_t timeout_ms);

#endif
