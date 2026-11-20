#include "cc/adapters/cc_managed_memory_worker.h"
#include "cc/internal/cc_alloc.h"
#include "cc/ports/cc_thread.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct cc_managed_memory_worker {
    cc_memory_store_t *store; /**< 借用的 JSON 记忆存储。 */
    cc_memory_snapshot_store_t *snapshots; /**< 借用的快照存储。 */
    cc_managed_memory_worker_options_t options; /**< 创建时复制的选项，回调上下文继续借用。 */
    cc_memory_pending_queue_t *pending; /**< 拥有的待处理队列，使用自身的队列锁。 */
    cc_mutex_t processing; /**< 串行保护处理、显式队首处置及以下提取缓存。 */
    char *cached_turn_id;
    cc_memory_plan_t cached_baseline;
    cc_memory_plan_t cached_plan;
    cc_memory_decision_t *cached_decisions;
    size_t cached_decision_count;
    int64_t cached_updated_at;
    int cached_plan_ready;
    int cached_attempt_requires_reconciliation; /**< 已安装或安装结果未知，恢复后须核对整个集合。 */
};

/* 提取器同步借用 provider。调用期间标记队首所属 session，队列跨会话时也按原会话记录观测。 */
typedef struct memory_call_context {
    cc_llm_provider_t *provider;
    const char *session_id;
    const char *turn_id;
} memory_call_context_t;

/**
 * @brief 同步借用 provider，复制请求描述并标注原队首 session/turn，step 设为辅助调用。
 *
 * 归因包装及请求副本只活在调用栈内，具体响应按 provider 契约交付。
 * @param[in] self 栈上的归因上下文，借用底层 provider 及当前队首的会话、对话 ID。
 * @param[in] request 本次同步模型请求，包装层按需要复制描述并补 session/turn 归因。
 * @param[out] response 按 provider 契约填写的响应，调用者清理其中拥有字段。
 * @return 返回实际 provider 的结果，依赖或请求无效时返回 CC_ERR_INVALID_ARGUMENT。
 */
static cc_result_t memory_call_chat(void *self, const cc_llm_chat_request_t *request,
                                    cc_llm_response_t *response)
{
    memory_call_context_t *context = self;
    if (!context || !context->provider || !context->provider->vtable ||
        !context->provider->vtable->chat || !request)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory model provider");
    cc_llm_chat_request_t attributed = *request;
    attributed.session_id = context->session_id;
    attributed.origin_turn_id = context->turn_id;
    attributed.step = -1;
    return context->provider->vtable->chat(context->provider->self, &attributed, response);
}

static const cc_llm_provider_vtable_t memory_call_vtable = { .chat = memory_call_chat };

/**
 * @brief 在处理锁保护下释放本队首拥有的基线、方案、决策及 ID，重置重试标志。
 *
 * 创建失败或销毁路径由外层保证尚无并发处理者。
 * @param[in,out] worker 缓存所属的有效处理器，调用者已持处理锁或已排空并发使用者。
 * @post 本轮 ID、基线、方案及决策已释放，全部重试标志清零。
 */
static void clear_cached_turn(cc_managed_memory_worker_t *worker)
{
    free(worker->cached_turn_id);
    worker->cached_turn_id = NULL;
    cc_memory_plan_cleanup(&worker->cached_baseline);
    cc_memory_plan_cleanup(&worker->cached_plan);
    cc_memory_decisions_free(worker->cached_decisions, worker->cached_decision_count);
    worker->cached_decisions = NULL;
    worker->cached_decision_count = 0;
    worker->cached_updated_at = 0;
    worker->cached_plan_ready = 0;
    worker->cached_attempt_requires_reconciliation = 0;
}

/**
 * @brief 比较可空字符串，两个 NULL 视为同值，其他有效字符串逐字节比较。
 * @param[in] a 第一个借用字符串，可为 NULL。
 * @param[in] b 第二个借用字符串，可为 NULL。
 * @return 同值或均为 NULL 时为一，其余为零。
 */
static int same_string(const char *a, const char *b)
{
    return (!a && !b) || (a && b && strcmp(a, b) == 0);
}

/**
 * @brief 核对受管理记录的身份、正文、来源、重要度与时间，供重试冲突判定。
 * @param[in] a 第一个借用记录，可为 NULL。
 * @param[in] b 第二个借用记录，可为 NULL。
 * @return 两个有效记录的 ID、正文及参与核对的元数据一致时为一，其余为零。
 */
static int same_record(const cc_memory_plan_record_t *a, const cc_memory_plan_record_t *b)
{
    return a && b && same_string(a->id, b->id) && same_string(a->text, b->text) &&
        same_string(a->category, b->category) && same_string(a->evidence, b->evidence) &&
        same_string(a->source_session, b->source_session) &&
        a->importance == b->importance && a->updated_at == b->updated_at;
}

/**
 * @brief 在已验证的计划中按 ID 查找，返回借用记录，计划清理或替换前使用。
 * @param[in] state 调用期间借用的已验证记忆集合。
 * @param[in] id 目标记忆的稳定 ID，调用期间借用。
 * @return 匹配 ID 的记录借用指针，未找到时为 NULL。
 */
static const cc_memory_plan_record_t *find_record(const cc_memory_plan_t *state, const char *id)
{
    for (size_t i = 0; i < state->count; i++)
        if (strcmp(state->records[i].id, id) == 0) return &state->records[i];
    return NULL;
}

/**
 * @brief 按记录 ID 比较整个受管理集合，输入顺序可以不同，元数据也参与核对。
 * @param[in] a 第一个有效的受管理集合，各记录 ID 已验证且唯一。
 * @param[in] b 第二个有效的受管理集合，各记录 ID 已验证且唯一。
 * @return 两个集合按 ID 逐项一致时为一，数量或记录不匹配时为零。
 */
static int same_state(const cc_memory_plan_t *a, const cc_memory_plan_t *b)
{
    if (a->count != b->count) return 0;
    for (size_t i = 0; i < a->count; i++)
        if (!same_record(&a->records[i], find_record(b, a->records[i].id))) return 0;
    return 1;
}

/**
 * @brief 当前状态与提取基线不同时，逐个检查 UPDATE 的原目标，其他记录可供重规划。
 *
 * 目标缺失或字段变化都要求冲突核对，避免覆盖用户的新编辑。
 * @param[in] worker 已持处理锁的处理器，从其缓存决策和提取基线核对目标。
 * @param[in] current 当前受管理集合的借用快照。
 * @return 每个 UPDATE 目标与提取基线一致时为一，目标缺失或变更时为零。
 */
static int update_targets_unchanged(const cc_managed_memory_worker_t *worker,
                                    const cc_memory_plan_t *current)
{
    for (size_t i = 0; i < worker->cached_decision_count; i++) {
        const cc_memory_decision_t *decision = &worker->cached_decisions[i];
        if (decision->kind == CC_MEMORY_DECISION_UPDATE &&
            !same_record(find_record(&worker->cached_baseline, decision->target_id),
                         find_record(current, decision->target_id))) return 0;
    }
    return 1;
}

/**
 * @brief 复制方案计数到结果。
 *
 * 计数描述计划，实际写入/出队成功还需检查处理返回值。
 * @param[in] plan 调用期间借用的已验证记忆方案。
 * @param[in,out] result 更新方案计数字段的结果结构，其他字段保持原值。
 * @post result 的方案计数字段已填写，模型用量及最终提交结果由调用方另行处理。
 */
static void describe_plan(const cc_memory_plan_t *plan, cc_managed_memory_worker_result_t *result)
{
    result->added = plan->added;
    result->changed_existing = plan->changed_existing;
    result->evicted_existing = plan->evicted_existing;
    result->evicted_new = plan->evicted_new;
    result->changed = plan->added > 0 || plan->changed_existing > 0;
}

/**
 * @brief 复制基础选项及 size 覆盖的可选回调，依次创建待处理队列和处理锁。
 *
 * 失败清理本次资源并保持输出为空，容量及策略由组合根显式提供。
 * @param[in] store 借用的 JSON 记忆存储，生命周期覆盖 worker。
 * @param[in] snapshots 借用的历史版本存储，生命周期覆盖 worker。
 * @param[in] options 显式 worker 选项，标量复制，回调上下文继续借用。
 * @param[out] out_worker 成功取得拥有的 worker，先排空处理及回调再销毁。
 * @return CC_OK 表示创建成功，参数、依赖、容量或资源分配错误向上传递。
 */
cc_result_t cc_managed_memory_worker_create(
    cc_memory_store_t *store, cc_memory_snapshot_store_t *snapshots,
    const cc_managed_memory_worker_options_t *options, cc_managed_memory_worker_t **out_worker)
{
    if (!out_worker) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null managed memory worker output");
    *out_worker = NULL;
    if (!store || !snapshots || !options || options->size < offsetof(cc_managed_memory_worker_options_t,before_ack) ||
        !options->capacity || !options->max_pending_items || !options->max_pending_bytes ||
        !options->select_eviction || !options->decide_snapshot)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Managed memory limits and policies are required");
    cc_managed_memory_worker_t *worker = calloc(1, sizeof(*worker));
    if (!worker) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate managed memory worker");
    worker->store = store; worker->snapshots = snapshots;
    memcpy(&worker->options,options,offsetof(cc_managed_memory_worker_options_t,before_ack));
    if (options->size>=offsetof(cc_managed_memory_worker_options_t,before_ack)+sizeof(options->before_ack))
        worker->options.before_ack=options->before_ack;
    if (options->size>=offsetof(cc_managed_memory_worker_options_t,before_ack_user_data)+sizeof(options->before_ack_user_data))
        worker->options.before_ack_user_data=options->before_ack_user_data;
    cc_result_t rc = cc_memory_pending_queue_create(options->max_pending_items, options->max_pending_bytes,
                                                     &worker->pending);
    if (rc.code == CC_OK) rc = cc_mutex_create(&worker->processing);
    if (rc.code != CC_OK) { cc_memory_pending_queue_destroy(worker->pending); free(worker); return rc; }
    *out_worker = worker;
    return cc_result_ok();
}

/**
 * @brief 调用者先关闭入口并排空处理/队列使用者，再释放缓存、队列和锁。
 *
 * 队列拥有的 RAM 项及预留随销毁清理，借用 store/snapshots 由外层管理。
 * @pre 处理、策略回调及队列使用者已退出，外层已关闭入口。
 * @param[in,out] worker 要销毁的处理器，可为 NULL，进入前已排空所有使用者。
 * @post pending queue、提取缓存与处理锁已释放，worker 寿命结束。
 */
void cc_managed_memory_worker_destroy(cc_managed_memory_worker_t *worker)
{
    if (!worker) return;
    clear_cached_turn(worker);
    cc_memory_pending_queue_destroy(worker->pending);
    cc_mutex_destroy(worker->processing);
    free(worker);
}

/**
 * @brief 复制一轮输入/答案到有界 FIFO，重复 turn ID 按队列内内容一致性检查。
 * @param[in,out] worker 接收已完成对话的处理器，转交其待处理队列。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @param[in] session_id 原输入所属会话标识，调用期间借用。
 * @param[in] user_input 原始用户输入，调用期间借用，复制或比对规则见函数说明。
 * @param[in] assistant_text 完成答复，可为空字符串，底层队列深拷贝。
 * @return CC_OK 表示 FIFO 已接纳或当前队列中已有相同内容，参数、容量、身份或分配错误向上传递。
 */
cc_result_t cc_managed_memory_worker_enqueue(
    cc_managed_memory_worker_t *worker, const char *turn_id, const char *session_id,
    const char *user_input, const char *assistant_text)
{
    if (!worker) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid managed memory worker");
    return cc_memory_pending_enqueue(worker->pending, turn_id, session_id, user_input, assistant_text);
}

/**
 * @brief 创建计入总项数及字节预算的 RAM 预留，成功句柄归提交者持有。
 * @param[in,out] worker 登记预留并承担容量计费的处理器。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @param[in] session_id 原输入所属会话标识，调用期间借用。
 * @param[in] user_input 原始用户输入，调用期间借用，复制或比对规则见函数说明。
 * @param[in] max_answer_bytes 答复正文的预留或扩容字节数，额外分配一个 NUL 字节。
 * @param[out] out_reservation 成功取得预留句柄，调用者串行提交、扩容或取消。
 * @return CC_OK 表示预留成功，参数、重复身份、容量或内存错误向上传递。
 */
cc_result_t cc_managed_memory_worker_reserve(cc_managed_memory_worker_t *worker,
    const char *turn_id, const char *session_id, const char *user_input,
    size_t max_answer_bytes, cc_memory_pending_reservation_t **out_reservation)
{
    if (!worker) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid managed memory worker");
    return cc_memory_pending_reserve(worker->pending,turn_id,session_id,user_input,max_answer_bytes,out_reservation);
}

/**
 * @brief 使用预分配答案缓冲交接到 FIFO，成功消费预留句柄，失败保留。
 * @param[in,out] worker 接收预留提交的处理器，通过队列锁完成交接。
 * @param[in,out] reservation 仍属于该队列的预留句柄，成功消费后置空。
 * @param[in] assistant_text 完成答复，可为空字符串，复制到已有预留缓冲。
 * @return CC_OK 表示句柄已消费并转入 FIFO，答复超长或句柄无效时返回错误并保留原预留。
 */
cc_result_t cc_managed_memory_worker_commit(cc_managed_memory_worker_t *worker,
    cc_memory_pending_reservation_t **reservation, const char *assistant_text)
{
    if (!worker) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid managed memory worker");
    return cc_memory_pending_commit(worker->pending,reservation,assistant_text);
}

/**
 * @brief 在同一队列预算内扩大原答案缓冲，失败保留既有容量及句柄。
 * @param[in,out] worker 预留所属处理器，通过队列锁扩大容量并增加计费。
 * @param[in,out] reservation 仍属于该队列的预留句柄。
 * @param[in] max_answer_bytes 答复正文的预留或扩容字节数，额外分配一个 NUL 字节。
 * @return CC_OK 表示容量已足够或扩容成功，错误时保留既有预留、容量和计费。
 */
cc_result_t cc_managed_memory_worker_expand_reservation(cc_managed_memory_worker_t *worker,
    cc_memory_pending_reservation_t *reservation, size_t max_answer_bytes)
{
    if (!worker) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid managed memory worker");
    return cc_memory_pending_expand(worker->pending,reservation,max_answer_bytes);
}

/**
 * @brief 核对预留身份与 turn/session/输入，调用中借用参数。
 * @param[in] worker 借用的处理器，本函数仅通过队列锁核对预留。
 * @param[in] reservation 仍属于该队列的预留句柄。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @param[in] session_id 原输入所属会话标识，调用期间借用。
 * @param[in] user_input 原始用户输入，调用期间借用，复制或比对规则见函数说明。
 * @return CC_OK 表示预留身份和原输入匹配，句柄无效或内容冲突返回相应错误。
 */
cc_result_t cc_managed_memory_worker_verify_reservation(cc_managed_memory_worker_t *worker,
    cc_memory_pending_reservation_t *reservation, const char *turn_id,
    const char *session_id, const char *user_input)
{
    if (!worker) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid managed memory worker");
    return cc_memory_pending_verify(worker->pending,reservation,turn_id,session_id,user_input);
}

/**
 * @brief 返回独立预留副本，调用者用 cc_memory_pending_item_cleanup 清理。
 * @param[in] worker 借用的处理器，本函数仅通过队列锁复制预留。
 * @param[in] reservation 仍属于该队列的预留句柄。
 * @param[out] out_item 无已有资源的输出结构，成功取得原输入副本，答案为空字符串，调用者 cleanup。
 * @return CC_OK 表示输出拥有副本，参数、句柄或分配错误向上传递。
 */
cc_result_t cc_managed_memory_worker_inspect_reservation(cc_managed_memory_worker_t *worker,
    cc_memory_pending_reservation_t *reservation, cc_memory_pending_item_t *out_item)
{
    if (!worker) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid managed memory worker");
    return cc_memory_pending_inspect_reservation(worker->pending,reservation,out_item);
}

/**
 * @brief 取消尚未提交的预留并归还预算，成功后调用者句柄清空。
 * @param[in,out] worker 预留所属处理器，取消后归还其队列容量。
 * @param[in,out] reservation 仍属于该队列的预留句柄，成功消费后置空。
 * @return CC_OK 表示预留已释放且句柄置空，句柄无效时返回错误。
 */
cc_result_t cc_managed_memory_worker_cancel_reservation(cc_managed_memory_worker_t *worker,
    cc_memory_pending_reservation_t **reservation)
{
    if (!worker) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid managed memory worker");
    return cc_memory_pending_cancel(worker->pending,reservation);
}

/**
 * @brief 持处理锁处理一个 FIFO 队首，按存储修订号提交方案并在确认后清缓存。
 *
 * 首次成功提取移交基线、决策和时间到缓存。重试遇到基线变化时核对 UPDATE 原目标，
 * 目标仍一致才沿用决策重规划。安装结果待核对时，当前集合须等于缓存方案或基线。
 * managed_apply 报告已安装，或失败后查询可写性报错、返回不可写，均保留安装核对标志。
 * 快照关联和 before_ack 失败也保留队首，所有出口清理本次临时副本。
 * @note 模型、淘汰策略、快照策略和 before_ack 均在处理锁内执行，遵守公共接口的回调重入约束。
 * @param[in,out] worker 本次获取处理锁的处理器，成功后清除队首及缓存，失败保留重试状态。
 * @param[in] provider 本次同步借用的模型 provider，跨调用并发由外层协调。
 * @param[in] options 本次提取选项，调用期间借用。
 * @param[in] updated_at 应用提供的非负更新时间，worker 首次成功提取后固定该值。
 * @param[out] out_result 参数合法后先清零，计划计数和本次提取用量可能在后续失败前已填写。
 * @return CC_OK 表示队首已确认出队，空队列为 CC_ERR_NOT_FOUND，其他错误保留队首和已有提取缓存。
 */
cc_result_t cc_managed_memory_worker_process_one(
    cc_managed_memory_worker_t *worker, cc_llm_provider_t *provider,
    const cc_memory_extraction_options_t *options, int64_t updated_at,
    cc_managed_memory_worker_result_t *out_result)
{
    if (!worker || !provider || !options || !out_result || updated_at < 0)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid managed memory processing request");
    memset(out_result, 0, sizeof(*out_result));
    cc_mutex_lock(worker->processing);
    cc_memory_pending_item_t item = {0};
    cc_memory_plan_t current = {0}, plan = {0};
    cc_memory_decision_t *decisions = NULL;
    size_t decision_count = 0;
    uint64_t revision = 0;
    char *snapshot_id = NULL;
    int installed = 0, ready = 0, new_cache = 0;
    cc_result_t rc = cc_memory_pending_peek(worker->pending, &item);
    if (rc.code == CC_OK && worker->cached_turn_id && strcmp(worker->cached_turn_id, item.turn_id) != 0)
        rc = cc_result_error(CC_ERR_INVALID_STATE, "Cached memory turn is not the pending head");
    if (rc.code == CC_OK) rc = cc_json_memory_write_ready(worker->store, &ready);
    if (rc.code == CC_OK && !ready)
        rc = cc_result_error(CC_ERR_STORAGE, "Memory storage needs recovery before retry");
    if (rc.code == CC_OK) rc = cc_json_memory_managed_list(worker->store, &current, &revision);
    if (rc.code == CC_OK && !worker->cached_turn_id) {
        char *turn_id = cc_copy_string(item.turn_id);
        if (!turn_id) rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot retain pending memory turn");
        if (rc.code == CC_OK) {
            memory_call_context_t context = { provider, item.session_id, item.turn_id };
            cc_llm_provider_t attributed = { &context, &memory_call_vtable };
            rc = cc_memory_extract_decisions(&attributed, options, item.user_input,
                item.assistant_text, current.records, current.count, &decisions, &decision_count, &out_result->model_usage);
        }
        if (rc.code == CC_OK) {
            worker->cached_turn_id = turn_id; turn_id = NULL;
            worker->cached_baseline = current; memset(&current, 0, sizeof(current));
            worker->cached_decisions = decisions; worker->cached_decision_count = decision_count;
            decisions = NULL; decision_count = 0;
            worker->cached_updated_at = updated_at;
            new_cache = 1;
        }
        free(turn_id);
    }
    int already_installed = 0;
    if (rc.code == CC_OK && worker->cached_attempt_requires_reconciliation) {
        if (same_state(&current, &worker->cached_plan)) already_installed = 1;
        else if (same_state(&current, &worker->cached_baseline)) worker->cached_attempt_requires_reconciliation = 0;
        else rc = cc_result_error(CC_ERR_INVALID_STATE, "Indeterminate memory write conflicts with current state");
    }
    if (rc.code == CC_OK && !already_installed && !new_cache &&
        !same_state(&current, &worker->cached_baseline)) {
        if (!update_targets_unchanged(worker, &current))
            rc = cc_result_error(CC_ERR_INVALID_STATE, "Pending memory update conflicts with a newer edit");
        else {
            rc = cc_memory_plan_from_decisions(current.records, current.count,
                worker->cached_decisions, worker->cached_decision_count, item.user_input, item.session_id,
                worker->cached_updated_at, worker->options.capacity, worker->options.select_eviction,
                worker->options.policy_user_data, &plan);
            if (rc.code == CC_OK) {
                cc_memory_plan_cleanup(&worker->cached_baseline);
                cc_memory_plan_cleanup(&worker->cached_plan);
                worker->cached_baseline = current; memset(&current, 0, sizeof(current));
                worker->cached_plan = plan; memset(&plan, 0, sizeof(plan));
                worker->cached_plan_ready = 1;
            }
        }
    }
    if (rc.code == CC_OK && !already_installed && !worker->cached_plan_ready) {
        rc = cc_memory_plan_from_decisions(worker->cached_baseline.records, worker->cached_baseline.count,
            worker->cached_decisions, worker->cached_decision_count, item.user_input, item.session_id,
            worker->cached_updated_at, worker->options.capacity, worker->options.select_eviction,
            worker->options.policy_user_data, &plan);
        if (rc.code == CC_OK) {
            worker->cached_plan = plan; memset(&plan, 0, sizeof(plan));
            worker->cached_plan_ready = 1;
        }
    }
    if (rc.code == CC_OK) describe_plan(&worker->cached_plan, out_result);
    if (rc.code == CC_OK && !already_installed && out_result->changed && item.snapshot_id) {
        rc = cc_memory_snapshot_confirm(worker->snapshots, item.snapshot_id);
        if (rc.code == CC_ERR_INVALID_STATE) {
            cc_result_free(&rc);
            rc = cc_memory_pending_clear_snapshot(worker->pending, item.turn_id, item.snapshot_id);
            if (rc.code == CC_OK) { free(item.snapshot_id); item.snapshot_id = NULL; }
        }
    }
    if (rc.code == CC_OK && !already_installed && out_result->changed) {
        cc_memory_snapshot_reason_t reason = worker->options.decide_snapshot(
            &worker->cached_plan, worker->cached_baseline.count, worker->options.policy_user_data);
        if (reason != 0 && reason != CC_MEMORY_SNAPSHOT_REWRITE)
            rc = cc_result_error(CC_ERR_INVALID_ARGUMENT, "Automatic memory snapshot policy is invalid");
        else {
            /* 保留本轮已经建立的重写快照关联，后续重新评估的修改条数可以减少。 */
            if (item.snapshot_id) reason = CC_MEMORY_SNAPSHOT_REWRITE;
            rc = cc_json_memory_managed_apply(worker->store, revision, &worker->cached_plan,
                reason ? worker->snapshots : NULL, reason, item.snapshot_id,
                &snapshot_id, &installed);
            if (snapshot_id && !item.snapshot_id) {
                cc_result_t remember = cc_memory_pending_take_snapshot(worker->pending, item.turn_id, &snapshot_id);
                if (rc.code == CC_OK && remember.code != CC_OK) rc = remember;
                else cc_result_free(&remember);
            }
            if (installed) worker->cached_attempt_requires_reconciliation = 1;
            else if (rc.code != CC_OK) {
                int write_ready = 0;
                cc_result_t inspection = cc_json_memory_write_ready(worker->store, &write_ready);
                if (inspection.code != CC_OK || !write_ready)
                    worker->cached_attempt_requires_reconciliation = 1;
                cc_result_free(&inspection);
            }
        }
    }
    if (rc.code == CC_OK && worker->options.before_ack)
        rc=worker->options.before_ack(worker->options.before_ack_user_data);
    if (rc.code == CC_OK) {
        rc = cc_memory_pending_ack(worker->pending, item.turn_id);
        if (rc.code == CC_OK) clear_cached_turn(worker);
    }
    free(snapshot_id);
    cc_memory_decisions_free(decisions, decision_count);
    cc_memory_plan_cleanup(&current); cc_memory_plan_cleanup(&plan);
    cc_memory_pending_item_cleanup(&item);
    cc_mutex_unlock(worker->processing);
    return rc;
}

/**
 * @brief 用户定向处理当前队首，processing 锁内核对 ID 与缓存所属。
 *
 * 尚待安装核对的尝试拒绝直接重规划或丢弃，同时检查存储可写性，避免结果未知时移除队首。
 * discard 成功移除队首，replan 保留输入并解除快照关联，历史快照文件由快照存储管理。
 * 两者完成后释放旧提取缓存，下次处理可重新调用模型。
 * @param[in,out] worker 队首所属处理器，在其处理锁内核对和处置。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @param[in] discard 非零时丢弃队首，为零时保留原输入并重规划。
 * @return CC_OK 表示显式处理成功，待核对安装或身份冲突为状态错误，store 阻断为存储错误。
 */
static cc_result_t resolve_pending_head(cc_managed_memory_worker_t *worker, const char *turn_id, int discard)
{
    if (!worker || !turn_id || !turn_id[0])
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pending memory resolution");
    cc_mutex_lock(worker->processing);
    cc_memory_pending_item_t item = {0};
    cc_result_t rc = cc_memory_pending_peek(worker->pending, &item);
    if (rc.code == CC_OK && strcmp(item.turn_id, turn_id) != 0)
        rc = cc_result_error(CC_ERR_INVALID_STATE, "Memory resolution is not for the pending head");
    if (rc.code == CC_OK && worker->cached_turn_id && strcmp(worker->cached_turn_id, turn_id) != 0)
        rc = cc_result_error(CC_ERR_INVALID_STATE, "Cached memory turn is not the pending head");
    if (rc.code == CC_OK && worker->cached_attempt_requires_reconciliation)
        rc = cc_result_error(CC_ERR_INVALID_STATE, "Resolve the uncertain installed memory write first");
    int ready = 0;
    if (rc.code == CC_OK) rc = cc_json_memory_write_ready(worker->store, &ready);
    if (rc.code == CC_OK && !ready)
        rc = cc_result_error(CC_ERR_STORAGE, "Memory storage needs recovery before resolution");
    if (rc.code == CC_OK && discard) rc = cc_memory_pending_ack(worker->pending, turn_id);
    if (rc.code == CC_OK && !discard && item.snapshot_id)
        rc = cc_memory_pending_clear_snapshot(worker->pending, turn_id, item.snapshot_id);
    if (rc.code == CC_OK) clear_cached_turn(worker);
    cc_memory_pending_item_cleanup(&item);
    cc_mutex_unlock(worker->processing);
    return rc;
}

/**
 * @brief 显式重规划当前队首，保留输入并清除可重新评估的缓存。
 * @param[in,out] worker 待重规划队首所属处理器，转交 resolve_pending_head 获取处理锁。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @return CC_OK 表示保留队首输入并清理可重新提取的缓存，核对或存储错误保留原状态。
 */
cc_result_t cc_managed_memory_worker_replan_head(cc_managed_memory_worker_t *worker, const char *turn_id)
{
    return resolve_pending_head(worker, turn_id, 0);
}

/**
 * @brief 显式丢弃当前队首，按内部核对规则确认后释放队列项。
 * @param[in,out] worker 待丢弃队首所属处理器，转交 resolve_pending_head 获取处理锁。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @return CC_OK 表示匹配队首已移除，核对或存储错误保留原状态。
 */
cc_result_t cc_managed_memory_worker_discard_head(cc_managed_memory_worker_t *worker, const char *turn_id)
{
    return resolve_pending_head(worker, turn_id, 1);
}

/**
 * @brief 读取 FIFO 和未提交预留的总项数及计费字节，空 worker 返回零。
 * @param[in] worker 待查询处理器，可为 NULL。
 * @param[out] out_pending 可选输出，FIFO 项与预留合计数量。
 * @param[out] out_bytes 可选输出，包含完整答复预留的计费字节数。
 * @post 可选输出已按同一次队列锁快照填写，空 worker 或 queue 输出零。
 */
void cc_managed_memory_worker_status(cc_managed_memory_worker_t *worker,
                                     size_t *out_pending, size_t *out_bytes)
{
    cc_memory_pending_status(worker ? worker->pending : NULL, out_pending, out_bytes);
}

/**
 * @brief 分别读取 FIFO 项、预留和总计费字节，队列自身锁保护各字段快照。
 * @param[in] worker 待查询处理器，可为 NULL。
 * @param[out] out_ready 可选输出，FIFO 中已提交项数。
 * @param[out] out_reserved 可选输出，尚未提交的预留数。
 * @param[out] out_bytes 可选输出，包含完整答复预留的计费字节数。
 * @post 可选输出已按同一次队列锁快照填写，空 worker 或 queue 输出零。
 */
void cc_managed_memory_worker_status_detail(cc_managed_memory_worker_t *worker,
    size_t *out_ready, size_t *out_reserved, size_t *out_bytes)
{
    cc_memory_pending_status_detail(worker ? worker->pending : NULL,out_ready,out_reserved,out_bytes);
}
