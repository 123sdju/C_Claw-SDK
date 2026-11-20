#ifndef CC_LLM_USAGE_H
#define CC_LLM_USAGE_H

#include <stdint.h>

/* 模型供应商用量的按值快照，缺失或无法观察的计数与明确报告零通过独立标志区分。
 * valid 表示适配器观察到可用总量或缓存报告，缓存计数字段自身仍须检查对应 valid。
 * 流式事件与最终响应的合并规则由具体 provider 实现，无动态资源或独立锁。 */
typedef struct cc_llm_usage {
    int valid;
    uint64_t prompt_tokens;
    uint64_t completion_tokens;
    uint64_t cached_tokens;
    uint64_t cache_write_tokens;
    int cached_tokens_valid;
    int cache_write_tokens_valid;
    /* 字段存在但类型或数值错误时仍保留 reported。
     * 对只填写原 validity 标志的适配器，有效缓存计数也意味着已报告。 */
    int cached_tokens_reported;
    int cache_write_tokens_reported;
} cc_llm_usage_t;

#endif
