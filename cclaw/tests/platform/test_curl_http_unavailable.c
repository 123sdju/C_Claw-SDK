#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "cc/ports/cc_http_client.h"
#include <assert.h>
#include <string.h>

/* 两个平台各编译一次，私有包含生产文件，仅验证没有 curl 时的公开端口行为。 */
/* 本翻译单元不启用 HTTP 工具或云 provider，保留生产平台依赖检查的正常边界。 */
#undef CC_LLM_OPENAI
#define CC_LLM_OPENAI 0
#undef CC_LLM_OLLAMA
#define CC_LLM_OLLAMA 0
#undef CC_LLM_ANTHROPIC
#define CC_LLM_ANTHROPIC 0
#undef CC_TOOL_HTTP_REQUEST
#define CC_TOOL_HTTP_REQUEST 0
#undef CC_HAS_CURL
#define CC_HAS_CURL 0
#define cc_http_client_create_default unavailable_http_create
#define cc_http_response_free unavailable_response_free
/**
 * @brief 声明更名后的生产响应清理入口，供前置静态头处理函数引用。
 * @param[in,out] response 生产响应或 NULL。
 * @post 清理行为由下方包含的生产定义提供。
 */
void unavailable_response_free(cc_http_response_t *response);
#if CC_TEST_WINDOWS_CURL
#include "../../platforms/windows/src/cc_curl_http_client.c"
#else
#include "../../platforms/posix/src/cc_curl_http_client.c"
#endif
#undef cc_http_client_create_default
#undef cc_http_response_free

static int g_policy_calls;

/**
 * @brief 记录不应在缺失传输实现时调用的 URL 策略入口。
 * @param[in] url 借用 URL，当前不读取。
 * @param[in,out] user_data 未使用，可为 NULL。
 * @return 返回允许，调用计数增加后由测试断言应仍为零。
 */
static int count_url(const char *url, void *user_data)
{
    (void)url;
    (void)user_data;
    g_policy_calls++;
    return 1;
}

/**
 * @brief 记录不应在缺失传输实现时触发的地址校验。
 * @param[in] host 借用主机，当前不读取。
 * @param[in] address 借用数字地址，当前不读取。
 * @param[in,out] user_data 未使用，可为 NULL。
 * @return 返回允许，并增加策略计数。
 */
static int count_address(const char *host, const char *address, void *user_data)
{
    (void)host;
    (void)address;
    (void)user_data;
    g_policy_calls++;
    return 1;
}

/**
 * @brief 验证无 curl 时仍可创建和销毁空能力端口，请求直接报平台不可用且不进入策略或 DNS。
 * @return 全部断言成立返回 0，任一失败由 assert 终止。
 * @note 域名使用无须解析的占位值，此模型不提供真实网络、TLS 或原生 Windows 证明。
 */
int main(void)
{
    cc_http_client_t client = {0};
    cc_result_t rc = unavailable_http_create(NULL, &client);
    assert(rc.code == CC_OK && client.self && client.capabilities == 0);
    cc_result_free(&rc);
    cc_http_request_t request = {0};
    request.url = "http://unavailable.invalid/";
    request.validate_url = count_url;
    request.validate_address = count_address;
    cc_http_response_t response;
    memset(&response, 0x5a, sizeof(response));
    rc = cc_http_client_perform(&client, &request, &response);
    assert(rc.code == CC_ERR_PLATFORM && g_policy_calls == 0);
    assert(!response.body && !response.headers && !response.header_count && !response.status_code);
    cc_result_free(&rc);
    request.header_count = 1;
    rc = cc_http_client_perform(&client, &request, &response);
    assert(rc.code == CC_ERR_INVALID_ARGUMENT && !response.body && !response.headers);
    cc_result_free(&rc);
    cc_http_client_destroy(&client);
    assert(!client.self && !client.vtable);
    return 0;
}
