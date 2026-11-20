
#ifndef CC_LLM_PROVIDERS_H
#define CC_LLM_PROVIDERS_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_llm_provider.h"
#include "cc/ports/cc_http_client.h"

/** @file cc_llm_providers.h
 * @brief 三类模型协议的创建契约，具体符号随对应 LLM 构建开关装入。
 * @note 模块启用与默认 feature 描述符的 compiled 标记由构建 profile 决定，声明本身不启用能力。
 * 模型请求的消息编码或工具 JSON 失败会终止构造，调用者修正配置或处理分配错误后再提交。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建使用 OpenAI Chat Completions 协议的拥有 provider。
 * @param[in,out] http_client 有效 HTTP 端口借用，覆盖所有请求、回调及 provider 销毁。
 * @param[in] base_url 调用期地址借用，NULL 使用 https://api.openai.com。
 * @param[in] api_key 可选调用期凭据借用，创建时复制，空值允许无认证请求。
 * @param[in] model 调用期模型借用，NULL 使用 gpt-4o-mini，创建时复制。
 * @param[out] out_provider 必填空资源槽，成功交付拥有 provider，失败保持原值。
 * @return 通用构造器的参数或分配结果，拥有诊断由调用者释放。
 * @note owner 关闭入口并排空请求及回调后通过 vtable destroy 消费 provider，HTTP 仍归外层拥有。
 */
cc_result_t cc_openai_provider_create(
    cc_http_client_t *http_client,
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_provider_t *out_provider
);

/**
 * @brief 创建无认证头的 Ollama /api/chat provider。
 * @param[in,out] http_client 有效 HTTP 端口借用，覆盖 provider 全部调用和销毁。
 * @param[in] base_url 调用期地址借用，NULL 使用 http://localhost:11434，创建时复制。
 * @param[in] model 调用期模型借用，NULL 使用 qwen2.5-coder:7b，创建时复制。
 * @param[out] out_provider 必填空资源槽，成功接管 provider，失败保持原值。
 * @return 通用构造结果与拥有诊断，调用者释放诊断并最终销毁成功 provider。
 * @note 流响应采用 NDJSON，调用者保持 HTTP 寿命并在销毁前关闭入口、排空在途调用及回调。
 */
cc_result_t cc_ollama_provider_create(
    cc_http_client_t *http_client,
    const char *base_url,
    const char *model,
    cc_llm_provider_t *out_provider
);

/**
 * @brief 创建默认请求五分钟提示缓存的 Anthropic Messages provider。
 * @param[in,out] http_client 有效 HTTP 借用，覆盖全部请求、回调和 provider 销毁。
 * @param[in] base_url 调用期地址借用，NULL 使用 https://api.anthropic.com。
 * @param[in] api_key 可选调用期凭据借用，创建时复制，请求以 x-api-key 头发送。
 * @param[in] model 调用期模型借用，NULL 使用 claude-3-5-haiku-latest。
 * @param[out] out_provider 必填空资源槽，成功交付拥有 provider，失败保持原值。
 * @return 带配置创建器的结果，拥有诊断由调用者释放。
 * @note 等价于 extra_body_json 为 NULL 的 configured 创建，请求级配置仍可覆盖缓存模式。
 */
cc_result_t cc_anthropic_provider_create(
    cc_http_client_t *http_client,
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_provider_t *out_provider
);

/**
 * @brief 校验缓存配置并创建具有固定默认缓存模式的 Anthropic provider。
 * @param[in,out] http_client 有效借用 HTTP，覆盖 provider 最终销毁，拥有权留在外层。
 * @param[in] base_url 调用期地址借用，NULL 使用官方默认地址，创建时复制。
 * @param[in] api_key 可选调用期凭据借用，创建时复制。
 * @param[in] model 调用期模型借用，NULL 使用默认模型，创建时复制。
 * @param[in] extra_body_json 调用期可选 JSON 配置，缺少 cache_control 使用五分钟，null 禁用，ephemeral 对象可选 5m/1h。
 * @param[out] out_provider 必填空资源槽，成功取得拥有 provider，配置或构造失败保持原值。
 * @return 配置 JSON、缓存字段参数错误或通用构造结果，拥有诊断由调用者释放。
 * @note 默认模式应用于该实例全部请求，包括摘要与记忆请求，请求自身 extra_body_json 可覆盖。
 * 其他 extra-body 字段沿现有适配范围跳过。协议状态指向不可变静态模式，跨代不共享可变配置。
 * owner 关闭入口并排空调用及回调后销毁 provider，HTTP 由外层随后释放。
 */
cc_result_t cc_anthropic_provider_create_configured(
    cc_http_client_t *http_client,
    const char *base_url,
    const char *api_key,
    const char *model,
    const char *extra_body_json,
    cc_llm_provider_t *out_provider
);

#ifdef __cplusplus
}
#endif

#endif
