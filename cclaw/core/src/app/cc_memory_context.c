#include "cc/app/cc_memory_context.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>

#define CC_MEMORY_CONTEXT_QUERY_LIMIT 4
#define CC_MEMORY_CONTEXT_QUERY_TERMS 8
#define CC_MEMORY_CONTEXT_CANDIDATES 32
#define CC_MEMORY_CONTEXT_MAX_ENTRIES 10
#define CC_MEMORY_CONTEXT_MAX_BYTES 2048U
#define CC_MEMORY_CONTEXT_HEADER "[Memory] Relevant persistent facts from previous sessions:\n"

/** @brief 本次注入独享的召回候选，按主键去重并累加词项匹配分值。 */
typedef struct memory_candidate {
    cc_memory_entry_t entry; /**< 拥有的记录副本，注入结束前释放。 */
    double score; /**< 各词项查询返回分值乘权重后的累加值。 */
} memory_candidate_t;

/**
 * @brief 按 UTF-8 字首位模式估计一个字符的字节宽度。
 * @param[in] lead 当前字节，按 ASCII 或二至四字节首位模式分类。
 * @return 估计宽度一至四，其他模式按一个字节推进，后续字节有效性由输入层处理。
 */
static size_t utf8_width(unsigned char lead)
{
    if ((lead & 0x80U) == 0) return 1;
    if ((lead & 0xe0U) == 0xc0U) return 2;
    if ((lead & 0xf0U) == 0xe0U) return 3;
    if ((lead & 0xf8U) == 0xf0U) return 4;
    return 1;
}

/**
 * @brief 判断字节是否可组成 ASCII 检索词。
 * @param[in] value 待检查字节。
 * @return ASCII 字母、数字或下划线为一，其余为零。
 */
static int ascii_word(unsigned char value)
{
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_';
}

/**
 * @brief 在八项上限内按字节去重，并复制最长 512 字节的词项。
 * @param[in,out] terms 有效词项槽数组，成功新增的字符串交调用者 free。
 * @param[in,out] count 已初始化槽数，仅成功新增时递增。
 * @param[in] text 借用的词项起点，至少有 length 个可读字节。
 * @param[in] length 词项字节数，不含输出补充的 NUL。
 * @return 已加入、重复、为空、超长、达到上限或参数为 NULL 时均为一，唯一失败值零表示新字符串分配失败。
 */
static int add_query_term(char **terms, size_t *count, const char *text, size_t length)
{
    if (!terms || !count || !text || length == 0 || length > 512U ||
        *count >= CC_MEMORY_CONTEXT_QUERY_TERMS) return 1;
    for (size_t i = 0; i < *count; i++) {
        if (strlen(terms[i]) == length && memcmp(terms[i], text, length) == 0) return 1;
    }
    char *copy = malloc(length + 1U);
    if (!copy) return 0;
    memcpy(copy, text, length);
    copy[length] = '\0';
    terms[(*count)++] = copy;
    return 1;
}

/**
 * @brief 生成整段输入、至少两字节的 ASCII 单词及相邻非 ASCII 字符片段组成的有界词项。
 * @param[in] query 有效的借用 NUL 结尾输入，整段长度不超过 512 字节时优先加入。
 * @param[out] terms 至少八个无已有资源的槽，新增词项由调用者逐项 free。
 * @param[out] out_count 进入后置零，每个成功新增词项递增，失败时保留已生成词项数供清理。
 * @return 生成完成为一，词项分配失败为零，最多八项，非 ASCII 拆分按字首估计宽度推进。
 */
static int build_query_terms(const char *query, char **terms, size_t *out_count)
{
    *out_count = 0;
    size_t length = strlen(query);
    if (length <= 512U && !add_query_term(terms, out_count, query, length)) return 0;
    for (size_t i = 0; i < length && *out_count < CC_MEMORY_CONTEXT_QUERY_TERMS;) {
        unsigned char value = (unsigned char)query[i];
        if (value < 0x80U && ascii_word(value)) {
            size_t start = i++;
            while (i < length && (unsigned char)query[i] < 0x80U &&
                   ascii_word((unsigned char)query[i])) i++;
            if (i - start >= 2U &&
                !add_query_term(terms, out_count, query + start, i - start)) return 0;
            continue;
        }
        if (value >= 0x80U) {
            size_t first = utf8_width(value);
            size_t next = i + first;
            if (next < length && (unsigned char)query[next] >= 0x80U) {
                size_t second = utf8_width((unsigned char)query[next]);
                if (next + second <= length &&
                    !add_query_term(terms, out_count, query + i, first + second)) return 0;
            }
            i += first;
            continue;
        }
        i++;
    }
    return 1;
}

/**
 * @brief 深拷贝本次候选的可选字符串及时间字段。
 * @param[in] source 有效的借用记录，各字符串可为 NULL。
 * @param[out] target 无已有资源的输出结构，进入后清零，成功拥有副本。
 * @return 成功为一，分配失败为零并通过 cc_memory_entry_free 清理部分副本。
 */
static int copy_entry(const cc_memory_entry_t *source, cc_memory_entry_t *target)
{
    memset(target, 0, sizeof(*target));
    target->created_at = source->created_at;
    target->updated_at = source->updated_at;
    if (source->key && !(target->key = cc_copy_string(source->key))) goto oom;
    if (source->value && !(target->value = cc_copy_string(source->value))) goto oom;
    if (source->category && !(target->category = cc_copy_string(source->category))) goto oom;
    if (source->session_id && !(target->session_id = cc_copy_string(source->session_id))) goto oom;
    return 1;
oom:
    cc_memory_entry_free(target);
    return 0;
}

/**
 * @brief 清理已复制候选及其数组。
 * @param[in,out] candidates 拥有的候选数组，NULL 时直接返回。
 * @param[in] count 已初始化的有效候选条数。
 * @post 有效记录的字符串和数组已释放，调用者保存的指针值由其自行清理。
 */
static void free_candidates(memory_candidate_t *candidates, size_t count)
{
    if (!candidates) return;
    for (size_t i = 0; i < count; i++) cc_memory_entry_free(&candidates[i].entry);
    free(candidates);
}

/**
 * @brief 按累计分值降序，再按记录更新时间降序比较候选。
 * @param[in] a 第一个有效借用候选。
 * @param[in] b 第二个有效借用候选。
 * @return 前者优先为负一，后者优先为一，两项排序字段相同为零。
 */
static int compare_candidates(const memory_candidate_t *a, const memory_candidate_t *b)
{
    if (a->score > b->score) return -1;
    if (a->score < b->score) return 1;
    if (a->entry.updated_at > b->entry.updated_at) return -1;
    if (a->entry.updated_at < b->entry.updated_at) return 1;
    return 0;
}

/**
 * @brief 按主键合并本词项结果，对已有候选累加分值，对新候选深拷贝。
 * @param[in,out] candidates 本次拥有的候选槽，现有项只更新分值，新项接管记录副本。
 * @param[in] candidate_count 已初始化候选条数。
 * @param[in] candidate_capacity 数组总槽数，达到上限后跳过新增项。
 * @param[in] results 借用的查询结果数组，至少有 result_count 项。
 * @param[in] result_count 待合并结果条数。
 * @param[in] query 借用的本次查询，用于再次排除摘要类别。
 * @param[in] weight 本词项分值权重，首个词项为 4.0，其他词项为 1.0。
 * @return 合并后的有效候选条数。缺少主键或正文、类别被排除、容量已满或复制失败的新增项直接跳过。
 * @note 同主键保留首次取得的正文与元数据，后续只累积分值，各次查询的存储快照由后端分别提供。
 */
static size_t merge_query_results(
    memory_candidate_t *candidates,
    size_t candidate_count,
    size_t candidate_capacity,
    const cc_memory_search_result_t *results,
    size_t result_count,
    const cc_memory_query_t *query,
    double weight
)
{
    for (size_t i = 0; i < result_count; i++) {
        const cc_memory_entry_t *entry = &results[i].entry;
        if (!entry->key || !entry->value || cc_memory_query_excludes_category(query, entry->category)) continue;
        size_t found = candidate_count;
        for (size_t j = 0; j < candidate_count; j++) {
            if (strcmp(candidates[j].entry.key, entry->key) == 0) {
                found = j;
                break;
            }
        }
        double score = results[i].score > 0.0 ? results[i].score : 1.0;
        if (found < candidate_count) {
            candidates[found].score += weight * score;
            continue;
        }
        if (candidate_count >= candidate_capacity) continue;
        if (!copy_entry(entry, &candidates[candidate_count].entry)) continue;
        candidates[candidate_count].score = weight * score;
        candidate_count++;
    }
    return candidate_count;
}

/**
 * @brief 从当前输入生成词项、合并召回结果并输出最多十条和 2048 字节的记忆提示块。
 * @param[in] store 借用的有效存储，调用期间由外层保证存活，具体查询并发由后端端口管理。
 * @param[in] query_text 借用的当前输入，NULL 或空字符串直接成功且输出为空。
 * @param[in] short_term_category 可选借用的短期类别，NULL 或空字符串采用 active_summary，另固定排除 context_summary。
 * @param[out] out_memory_block 必需的无已有资源输出槽，非 NULL 时先置空，成功正文由调用者 free。
 * @return 完成或无可用记忆为 CC_OK，空输出槽或无效存储为 CC_ERR_INVALID_ARGUMENT，词项构造和文本拼接失败原样返回。
 * @note 请求每词最多四项，候选最多 32 条，过滤、按主键合并后按累计分值和时间排序。
 * 候选数组分配、单次查询或个别候选复制失败沿现有可选召回策略跳过，全部缺失时输出为空。
 * 本函数使用独立临时资源，多次查询之间的存储版本协调及对象销毁由调用者负责。
 */
cc_result_t cc_memory_context_inject_filtered(
    cc_memory_store_t *store,
    const char *query_text,
    const char *short_term_category,
    char **out_memory_block
)
{
    if (!out_memory_block) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null memory context output");
    }
    *out_memory_block = NULL;
    if (!store || !store->vtable) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory context store");
    }
    if (!query_text || !query_text[0]) return cc_result_ok();

    char *terms[CC_MEMORY_CONTEXT_QUERY_TERMS] = {0};
    size_t term_count = 0;
    if (!build_query_terms(query_text, terms, &term_count)) {
        for (size_t i = 0; i < term_count; i++) free(terms[i]);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build memory query terms");
    }
    if (term_count == 0) return cc_result_ok();

    memory_candidate_t *candidates = calloc(CC_MEMORY_CONTEXT_CANDIDATES, sizeof(*candidates));
    if (!candidates) {
        for (size_t i = 0; i < term_count; i++) free(terms[i]);
        return cc_result_ok();
    }
    size_t candidate_count = 0;
    for (size_t ti = 0; ti < term_count; ti++) {
        cc_memory_query_t query = {
            .size = sizeof(query),
            .query = terms[ti],
            .category = NULL,
            .session_id = NULL,
            .limit = CC_MEMORY_CONTEXT_QUERY_LIMIT,
            .exclude_category = "context_summary",
            .exclude_category2 = short_term_category && short_term_category[0] ?
                short_term_category : "active_summary",
        };
        cc_memory_search_result_t *results = NULL;
        size_t result_count = 0;
        cc_result_t query_rc = cc_memory_store_query(store, &query, &results, &result_count);
        if (query_rc.code == CC_OK) {
            candidate_count = merge_query_results(candidates, candidate_count,
                CC_MEMORY_CONTEXT_CANDIDATES, results, result_count, &query, ti == 0 ? 4.0 : 1.0);
        }
        cc_result_free(&query_rc);
        cc_memory_search_result_free_array(results, result_count);
    }
    for (size_t i = 0; i < term_count; i++) free(terms[i]);
    if (candidate_count == 0) {
        free_candidates(candidates, candidate_count);
        return cc_result_ok();
    }

    for (size_t i = 0; i < candidate_count; i++) {
        size_t best = i;
        for (size_t j = i + 1U; j < candidate_count; j++) {
            if (compare_candidates(&candidates[j], &candidates[best]) < 0) best = j;
        }
        if (best != i) {
            memory_candidate_t tmp = candidates[i];
            candidates[i] = candidates[best];
            candidates[best] = tmp;
        }
    }

    cc_string_builder_t sb;
    cc_result_t rc = cc_string_builder_init(&sb);
    if (rc.code != CC_OK) {
        free_candidates(candidates, candidate_count);
        return rc;
    }
    cc_result_free(&rc);

    size_t emitted = 0;
    for (size_t i = 0; i < candidate_count && emitted < CC_MEMORY_CONTEXT_MAX_ENTRIES; i++) {
        const cc_memory_entry_t *entry = &candidates[i].entry;
        size_t line_bytes = strlen(entry->key) + strlen(entry->value) + 5U;
        if (entry->category && entry->category[0]) line_bytes += strlen(entry->category) + 13U;
        size_t header_bytes = emitted == 0 ? strlen(CC_MEMORY_CONTEXT_HEADER) : 0U;
        if (sb.length > CC_MEMORY_CONTEXT_MAX_BYTES ||
            header_bytes > CC_MEMORY_CONTEXT_MAX_BYTES - sb.length ||
            line_bytes > CC_MEMORY_CONTEXT_MAX_BYTES - sb.length - header_bytes) continue;

        if (emitted == 0) {
            rc = cc_string_builder_append(&sb, CC_MEMORY_CONTEXT_HEADER);
            if (rc.code != CC_OK) break;
            cc_result_free(&rc);
        }
        rc = cc_string_builder_appendf(&sb, "- %s: %s", entry->key, entry->value);
        if (rc.code != CC_OK) break;
        cc_result_free(&rc);
        if (entry->category && entry->category[0]) {
            rc = cc_string_builder_appendf(&sb, " (category: %s)", entry->category);
            if (rc.code != CC_OK) break;
            cc_result_free(&rc);
        }
        rc = cc_string_builder_append(&sb, "\n");
        if (rc.code != CC_OK) break;
        cc_result_free(&rc);
        emitted++;
    }

    free_candidates(candidates, candidate_count);
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&sb);
        return rc;
    }
    if (emitted == 0) {
        cc_string_builder_deinit(&sb);
        return cc_result_ok();
    }

    *out_memory_block = cc_string_builder_take(&sb);
    return *out_memory_block ? cc_result_ok() :
        cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build memory context");
}

/**
 * @brief 使用默认短期类别 active_summary 调用过滤注入入口。
 * @param[in] store 借用的有效存储，调用期间保持存活。
 * @param[in] session_text 借用的当前用户输入，NULL 或空字符串不产生记忆块。
 * @param[out] out_memory_block 必需输出，初始化及拥有关系遵循 inject_filtered，成功字符串由调用者 free。
 * @return inject_filtered 的结果原样返回。
 */
cc_result_t cc_memory_context_inject(
    cc_memory_store_t *store, const char *session_text, char **out_memory_block)
{
    return cc_memory_context_inject_filtered(store, session_text, NULL, out_memory_block);
}
