#ifndef CC_MEMORY_EXTRACTOR_H
#define CC_MEMORY_EXTRACTOR_H

#include "cc/core/cc_llm_usage.h"
#include "cc/ports/cc_llm_provider.h"
#include "cc/app/cc_memory_plan.h"

/** 一次记忆决策提取请求可接收的现有记录数量上限。 */
#define CC_MEMORY_EXTRACTION_MAX_EXISTING 1024U
/** 用户原输入和助手答复各自的正文字节上限，均不计结尾 NUL。 */
#define CC_MEMORY_EXTRACTION_MAX_TURN_BYTES (64U * 1024U)

/** @brief 提取出的记忆候选，返回数组拥有各字符串，使用 cc_memory_candidates_free 释放。 */
typedef struct cc_memory_candidate {
    char *text; /**< 非空记忆正文，最多 1024 字节，不计 NUL。 */
    char *category; /**< fact、preference、project 或 goal，区分大小写。 */
    char *evidence; /**< 当前 user_input 的非空逐字子串，最多 512 字节，不计 NUL。 */
    unsigned importance; /**< 重要度整数，范围 0 至 100。 */
} cc_memory_candidate_t;

/** @brief 同步模型提取的请求选项，选项本身和指针字段均在调用期间借用。 */
typedef struct cc_memory_extraction_options {
    size_t size; /**< 至少为 sizeof(cc_memory_extraction_options_t)。 */
    const char *model; /**< 传给 provider 的模型名，可为 NULL，由 provider 决定默认模型。 */
    int timeout_ms; /**< 超时时间，单位毫秒，小于等于零时使用 120000。 */
    int max_output_tokens; /**< 传给 provider 的最大输出 token 数，小于等于零时使用 1024。 */
    size_t max_response_bytes; /**< 模型正文的最大字节数，不计 NUL，零时使用 65536。 */
    cc_cancel_token_t *cancel_token; /**< 可选取消信号，发起请求前及 provider 返回成功后检查。 */
} cc_memory_extraction_options_t;

/**
 * @brief 同步发起一次独立的记忆提取请求，解析并返回最多八个候选。
 *
 * 请求不携带工具，返回工具调用会被拒绝。模型正文拒绝会截断 C 字符串的 NUL 转义，
 * 经 SDK 的尾部及 Unicode 转义检查后，
 * 顶层须为 JSON 对象并包含 memories 数组。
 * 各候选核对正文、类别、整数重要度和原输入证据，拒绝相同正文与类别的重复项，
 * 并检查正文及证据中 cc_redact_secrets 可识别的凭据。未知字段和重复键沿用 cJSON 的查找规则。
 * 证据核对采用 C 字符串子串匹配，语义真实性和完整凭据识别仍需应用判断。
 * 字符串按 NUL 结尾字节串处理，UTF-8 编码有效性沿用输入数据。
 * @pre 调用者保证 provider 及其 self 在调用期间有效，并按具体 provider 的并发契约协调共享使用。
 * 本函数没有为 provider 加锁，调用返回前须保持所有借用输入稳定。
 * @param[in,out] provider 非空 provider，须提供 vtable->chat，内部状态由同步请求更新。
 * @param[in] options 非空请求选项，size 须达到当前结构体大小。
 * @param[in] user_input 非空原输入，最多 CC_MEMORY_EXTRACTION_MAX_TURN_BYTES 字节，调用期间借用。
 * @param[in] assistant_text 助手答复，允许空字符串，最多同样的字节数，调用期间借用。
 * @param[out] out_candidates 必填独立输出，基本参数通过后置 NULL。
 * 成功时取得拥有数组，须与 out_count 一起传给 cc_memory_candidates_free。
 * @param[out] out_count 必填独立输出，基本参数通过后置零，成功时为候选数，空结果为零。
 * @param[out] out_usage 可选用量输出，基本参数通过后清零，provider 调用后复制 response.usage，
 * 因而 provider 或解析失败时也可能有已报告用量，按 usage 自身有效性字段解释。
 * @return 成功返回 CC_OK。基本参数错误返回 CC_ERR_INVALID_ARGUMENT 且保持输出原值。
 * 随后的长度、取消、分配、provider 和模型校验错误均保持候选输出为空及数量为零，
 * 无效模型字段返回 CC_ERR_MODEL，超过八项返回 CC_ERR_LIMIT_EXCEEDED。
 */
cc_result_t cc_memory_extract_candidates(
    cc_llm_provider_t *provider, const cc_memory_extraction_options_t *options,
    const char *user_input, const char *assistant_text,
    cc_memory_candidate_t **out_candidates, size_t *out_count, cc_llm_usage_t *out_usage);
/**
 * @brief 释放候选数组及每项拥有的正文、类别和证据。
 * @param[in,out] candidates 拥有数组，可为 NULL，须有至少 count 个已初始化或清零的元素。
 * @param[in] count 需要清理的元素数。
 * @post 非空数组及其元素字符串已释放，调用者保存的指针值须自行清空。
 */
void cc_memory_candidates_free(cc_memory_candidate_t *candidates, size_t count);

typedef enum cc_memory_decision_kind {
    CC_MEMORY_DECISION_ADD = 1, /**< 新增记忆。 */
    CC_MEMORY_DECISION_UPDATE = 2 /**< 更新指定现有记忆。 */
} cc_memory_decision_kind_t;

typedef struct cc_memory_decision {
    cc_memory_decision_kind_t kind; /**< 新增或更新动作。 */
    char *target_id; /**< UPDATE 拥有的非空现有记录 ID，提取出的 ADD 使用 NULL。 */
    cc_memory_candidate_t candidate; /**< 决策拥有的候选内容，随决策数组一起释放。 */
} cc_memory_decision_t;

/**
 * @brief 同步对照当前用户输入与已有记忆，提取最多八项新增或更新决策。
 *
 * 候选内容遵循 cc_memory_extract_candidates 的字节限制、证据与可检测凭据检查。
 * UPDATE 的 target_id 必须匹配 existing 中的 ID，同一批决策只能更新每个 ID 一次。
 * ADD 接受缺失、非字符串或空字符串的 target_id。
 * 现有记录编码后的整份请求 JSON 最多 512 KiB。
 * @pre provider 的存活、共享并发与输入稳定性要求同 cc_memory_extract_candidates。
 * @param[in,out] provider 非空且提供 chat 的借用 provider。
 * @param[in] options 非空请求选项，默认值与候选提取接口相同。
 * @param[in] user_input 非空当前用户输入，最多 CC_MEMORY_EXTRACTION_MAX_TURN_BYTES 字节，调用期间借用。
 * @param[in] assistant_text 助手答复，可为空字符串，最多同样的字节数，调用期间借用。
 * @param[in] existing 现有记录数组，existing_count 为零时可为 NULL，调用期间借用。
 * 每项 size 须足够，ID、正文、类别须非空且分别不超过 512、1024、128 字节，重要度最多 100，ID 须唯一。
 * @param[in] existing_count 现有记录数，不得超过 CC_MEMORY_EXTRACTION_MAX_EXISTING。
 * @param[out] out_decisions 必填独立输出，基本参数通过后置 NULL，成功取得拥有数组，使用 cc_memory_decisions_free 释放。
 * @param[out] out_count 必填独立输出，基本参数通过后置零，成功返回决策数，空结果为零。
 * @param[out] out_usage 可选用量输出，初始化及失败时的可用性同候选提取接口。
 * @return 返回参数、输入大小、取消、分配、provider 或模型校验结果。
 * 基本参数错误保持输出原值，通过基本参数检查后的失败保持决策为空和数量为零。
 */
cc_result_t cc_memory_extract_decisions(
    cc_llm_provider_t *provider, const cc_memory_extraction_options_t *options,
    const char *user_input, const char *assistant_text,
    const cc_memory_plan_record_t *existing, size_t existing_count,
    cc_memory_decision_t **out_decisions, size_t *out_count, cc_llm_usage_t *out_usage);
/**
 * @brief 释放决策数组、目标 ID 及每项拥有的候选字符串。
 * @param[in,out] decisions 拥有数组，可为 NULL，须有至少 count 个已初始化或清零的元素。
 * @param[in] count 需要清理的元素数。
 * @post 非空数组及其拥有字符串已释放，调用者保存的指针值须自行清空。
 */
void cc_memory_decisions_free(cc_memory_decision_t *decisions, size_t count);

/**
 * @brief 再次核对决策并构建包含现有记忆的完整提案。
 *
 * 新增项生成 mem- 前缀 UUID，更新项保留指定 ID 并替换正文及来源元数据。
 * 字段、证据子串及可检测凭据检查在构建前重新执行，目标存在性由 cc_memory_plan_build 核对。
 * @param[in] existing 现有记录数组，数量为零时可为 NULL，调用期间借用，须满足 plan_build 的记录契约。
 * @param[in] existing_count 现有记录数，此接口沿用 plan_build 的容量检查。
 * @param[in] decisions 决策数组，数量为零时可为 NULL，调用期间借用，UPDATE 目标须唯一。
 * @param[in] decision_count 决策数，最多八项。
 * @param[in] user_input 非空原用户输入，作为证据子串核对来源，调用期间借用。
 * @param[in] source_session 非空来源会话 ID，计划复制此字符串。
 * @param[in] updated_at 应用提供的非负更新时间，所有变更记录使用此值，单位由应用统一约定。
 * @param[in] capacity 最终允许的记录数，须大于零。
 * @param[in] select_eviction 超出容量时同步调用的淘汰选择器，可为 NULL，缺少必要策略时返回容量错误。
 * 回调借用完整拟议记录集，须返回范围内索引，并保持输入数组稳定。
 * @param[in] user_data 传给选择器的借用上下文，可为 NULL。
 * @param[out] out_plan 必填独立输出，基本参数通过后清零，须先释放其原有内容。
 * 成功取得拥有完整计划，使用 cc_memory_plan_cleanup 释放，通过基本参数检查后的失败保持为空。
 * @return 返回字段或动作错误、凭据检查错误、UUID 生成错误，或 plan_build 的复制、目标、容量及回调结果。
 * 基本参数错误返回 CC_ERR_INVALID_ARGUMENT 并保持输出原值。
 */
cc_result_t cc_memory_plan_from_decisions(
    const cc_memory_plan_record_t *existing, size_t existing_count,
    const cc_memory_decision_t *decisions, size_t decision_count,
    const char *user_input, const char *source_session, int64_t updated_at,
    size_t capacity, cc_memory_eviction_select_fn select_eviction, void *user_data,
    cc_memory_plan_t *out_plan);

#endif
