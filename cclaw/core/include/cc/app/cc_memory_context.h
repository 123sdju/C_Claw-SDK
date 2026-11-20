



#ifndef CC_MEMORY_CONTEXT_H
#define CC_MEMORY_CONTEXT_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_memory_store.h"

/**
 * @brief 检索与当前输入有关的跨会话记忆，生成有界上下文文本。
 * @param[in] store 借用的有效存储及 vtable，调用期间保持有效，后端负责各次查询的同步。
 * @param[in] session_text 借用的当前真实用户输入，NULL 或空字符串成功返回空输出。
 * @param[out] out_memory_block 必需的无已有资源输出槽，非 NULL 时先置空，成功取得正文后由调用者 free。
 * @return 完成或无可用记忆为 CC_OK，无效存储或空输出槽为 CC_ERR_INVALID_ARGUMENT，词项构造内存不足及文本拼接错误向上传递。
 * @note 固定排除 context_summary 和 active_summary，最多生成十条、2048 字节，预算包含标题及分类标签。
 * 候选数组或个别复制分配失败、查询失败按可选召回策略跳过，可能成功返回空输出。
 * 调用者协调存储销毁及跨查询版本，函数本次临时资源彼此独立。
 */
cc_result_t cc_memory_context_inject(
    cc_memory_store_t *store,
    const char *session_text,
    char **out_memory_block
);

/**
 * @brief 在查询和候选合并时排除上下文摘要及配置的短期类别，再生成记忆文本。
 * @param[in] store 借用的有效存储及 vtable，调用期间保持存活。
 * @param[in] session_text 借用的当前输入，NULL 或空字符串直接成功返回空输出。
 * @param[in] short_term_category 借用的可选短期类别，NULL 或空字符串采用 active_summary，固定另排除 context_summary。
 * @param[out] out_memory_block 必需的无已有资源输出槽，非 NULL 时先置空，成功正文归调用者 free。
 * @return 与默认入口相同的参数、词项构造和文本拼接结果，无候选时为 CC_OK 且输出为空。
 * @note 拆分整段、ASCII 单词及相邻非 ASCII 片段，最多八个词项，每词请求四条，合并最多 32 个主键。
 * 累计分值和更新时间决定输出顺序。资源、预算和可选召回失败策略与默认入口相同。
 */
cc_result_t cc_memory_context_inject_filtered(
    cc_memory_store_t *store, const char *session_text, const char *short_term_category,
    char **out_memory_block);

#endif
