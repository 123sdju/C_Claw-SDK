
#ifndef CC_LLM_PROVIDERS_H
#define CC_LLM_PROVIDERS_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_llm_provider.h"
#include "cc/ports/cc_http_client.h"

/*
 * 创建 OpenAI-compatible provider。
 *
 * base_url/api_key/model 在创建时复制；out_provider 成功后通过 vtable destroy 释放。
 */
cc_result_t cc_openai_provider_create(
    cc_http_client_t *http_client,
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_provider_t *out_provider
);

/* 创建 Ollama provider；通常不需要 api_key，base_url/model 在创建时复制。 */
cc_result_t cc_ollama_provider_create(
    cc_http_client_t *http_client,
    const char *base_url,
    const char *model,
    cc_llm_provider_t *out_provider
);

/* 创建 Anthropic provider；api_key/model 在创建时复制，错误语义映射到 cc_result_t。 */
cc_result_t cc_anthropic_provider_create(
    cc_http_client_t *http_client,
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_provider_t *out_provider
);

#endif
