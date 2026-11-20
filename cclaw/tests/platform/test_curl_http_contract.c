#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "cc/ports/cc_http_client.h"
#include "cc/ports/cc_platform.h"
#include "cc/app/cc_cancel_token.h"
#include <curl/curl.h>
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>

/* 同一物理测试分别私有包含两端生产实现，库中的端口符号不会被此测试替换。
 * curl 使用真实公共头的类型和常量，传输、时钟、解析和分配由本翻译单元确定性驱动。 */
#undef curl_easy_setopt
#undef curl_easy_getinfo

enum model_scenario {
    MODEL_SUCCESS, MODEL_EMPTY, MODEL_RETRY, MODEL_PARTIAL_ERROR,
    MODEL_TOTAL_TIMEOUT, MODEL_FIRST_TIMEOUT, MODEL_IDLE_TIMEOUT,
    MODEL_ADDRESS_REJECT, MODEL_INFO_ERROR, MODEL_SIGPIPE
};

/**
 * @brief 模型保存的生产正文或响应头回调类型。
 * @param[in] data 仅回调期可读的二进制片段。
 * @param[in] size 元素字节数。
 * @param[in] count 元素数。
 * @param[in,out] user_data 生产回调状态的借用。
 * @return 接受的字节数，短返回表示传输中止。
 */
typedef size_t (*model_write_callback_fn)(void *data, size_t size, size_t count, void *user_data);

/** @brief easy handle 的本地替身，仅保存实际测试要驱动的选项。 */
typedef struct model_easy {
    model_write_callback_fn body;       /**< 借用的生产正文回调。 */
    model_write_callback_fn header;     /**< 借用的生产响应头回调。 */
    curl_xferinfo_callback progress;    /**< 借用的生产进度回调。 */
    curl_opensocket_callback open_socket; /**< 借用的生产套接字回调。 */
    void *body_data;                    /**< 正文回调栈状态借用。 */
    void *header_data;                  /**< 响应头回调栈状态借用。 */
    void *progress_data;                /**< 进度回调栈状态借用。 */
    void *socket_data;                  /**< 套接字回调栈状态借用。 */
    long timeout_ms;                    /**< 本次尝试剩余总预算，毫秒。 */
    long nosignal;                      /**< 生产传入的 NOSIGNAL 标记。 */
    long follow;                        /**< 生产传入的自动重定向标记。 */
} model_easy_t;

/** @brief 流式回调借用的场景状态。 */
typedef struct body_state {
    size_t bytes;                       /**< 已交付的字节数。 */
    int calls;                          /**< 回调次数。 */
    cc_error_code_t error;               /**< 交给生产层的结果码。 */
    cc_cancel_source_t *cancel_after;    /**< 回调内发出取消的借用源，可为空。 */
} body_state_t;

static enum model_scenario g_scenario;
static uint64_t g_now;
static int g_performs;
static int g_handles;
static int g_global_calls;
static int g_global_fail;
static CURLoption g_fail_option;
static int g_fail_info;
static long g_timeouts[4];
static int g_metrics;
static int g_retry_metrics;
static int g_partial_metrics;
static int g_alloc_calls;
static int g_fail_alloc;
static void *g_owned[128];
static size_t g_owned_count;
static char *g_success_message;
static int g_success_frees;
static int g_resolves;
static int g_resolve_cancel;
static int g_resolve_expire;
static cc_cancel_source_t *g_resolve_source;
static int g_mask_fail_next;

/**
 * @brief 为私有包含后更名的响应清理函数提供前置声明。
 * @param[in,out] response 私有模型响应或 NULL。
 * @post 具体清理契约与下方包含的生产定义一致。
 */
void model_response_free(cc_http_response_t *response);

/** @brief 读取模型单调时钟。 @return 当前由测试控制的毫秒值。 */
static uint64_t model_monotonic_ms(void)
{
    return g_now;
}

/**
 * @brief 对 POSIX 生产信号保护区的首次 mask 操作注入错误，其余调用真实 pthread。
 * @param[in] how 原生 mask 操作标识。
 * @param[in] set 借用新 mask，可为 NULL。
 * @param[out] old_set 可选旧 mask 输出，注入失败时保持原值。
 * @return 注入时返回 EINVAL，其余原样返回 pthread_sigmask 结果。
 */
static int model_pthread_sigmask(int how, const sigset_t *set, sigset_t *old_set)
{
    if (g_mask_fail_next) {
        g_mask_fail_next = 0;
        return EINVAL;
    }
    return pthread_sigmask(how, set, old_set);
}

/**
 * @brief 登记生产实现拥有的分配，便于断言失败清理。
 * @param[in] ptr 新分配，可为 NULL。
 * @return 原样返回 ptr，非空分配登记到固定测试表。
 */
static void *remember_allocation(void *ptr)
{
    if (ptr) {
        assert(g_owned_count < sizeof(g_owned) / sizeof(g_owned[0]));
        g_owned[g_owned_count++] = ptr;
    }
    return ptr;
}

/**
 * @brief 对生产 malloc 的指定调用注入失败。
 * @param[in] bytes 分配字节数。
 * @return 失败点返回 NULL，其余返回登记后由生产实现负责释放的存储。
 */
static void *model_malloc(size_t bytes)
{
    if (++g_alloc_calls == g_fail_alloc) return NULL;
    return remember_allocation(malloc(bytes));
}

/**
 * @brief 对生产 calloc 的指定调用注入失败。
 * @param[in] count 元素数。
 * @param[in] bytes 每元素字节数。
 * @return 失败点返回 NULL，其余返回登记的零初始化存储。
 */
static void *model_calloc(size_t count, size_t bytes)
{
    if (++g_alloc_calls == g_fail_alloc) return NULL;
    return remember_allocation(calloc(count, bytes));
}

/**
 * @brief 对生产扩容注入失败并在成功搬迁时更新拥有表。
 * @param[in,out] ptr 原分配，可为 NULL，失败时继续有效。
 * @param[in] bytes 新容量，生产路径均为正数。
 * @return 新分配或 NULL，遵循 realloc 的保留规则。
 */
static void *model_realloc(void *ptr, size_t bytes)
{
    if (++g_alloc_calls == g_fail_alloc) return NULL;
    size_t index = 0;
    while (index < g_owned_count && g_owned[index] != ptr) index++;
    void *next = realloc(ptr, bytes);
    if (!next) return NULL;
    if (index < g_owned_count) g_owned[index] = next;
    else remember_allocation(next);
    return next;
}

/**
 * @brief 撤销生产分配登记并释放存储，也接收 builder 交出的未登记字符串。
 * @param[in] ptr 待释放指针，可为 NULL。
 * @post 指针对应存储已经释放，登记表中不再持有该指针。
 */
static void model_free(void *ptr)
{
    for (size_t i = 0; i < g_owned_count; i++) {
        if (g_owned[i] == ptr) {
            g_owned[i] = g_owned[--g_owned_count];
            break;
        }
    }
    free(ptr);
}

/**
 * @brief 记录成功回调消息确实被生产端归还，再使用真实结果清理。
 * @param[in,out] result 待清理拥有型结果，可为 NULL。
 * @post 动态成员已释放，成功消息的释放计数更新。
 */
static void model_result_free(cc_result_t *result)
{
    if (result && g_success_message && result->message == g_success_message) {
        g_success_frees++;
        g_success_message = NULL;
    }
    cc_result_free(result);
}

/**
 * @brief 返回一个静态 IPv4 解析候选，并可在解析期间触发取消或超时。
 * @param[in] node 借用主机名，模型只检查非空。
 * @param[in] service 服务名，模型忽略。
 * @param[in] hints 借用解析选项，模型忽略。
 * @param[out] addresses 非 NULL 输出槽，写入静态链表。
 * @return 返回 0 表示解析成功，节点存储由模型保持有效。
 */
static int model_getaddrinfo(const char *node, const char *service,
    const struct addrinfo *hints, struct addrinfo **addresses)
{
    (void)service;
    (void)hints;
    static struct sockaddr_in address;
    static struct addrinfo item;
    assert(node);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    memset(&item, 0, sizeof(item));
    item.ai_addr = (struct sockaddr *)&address;
    item.ai_addrlen = sizeof(address);
    *addresses = &item;
    g_resolves++;
    if (g_resolve_cancel) cc_cancel_source_cancel(g_resolve_source);
    if (g_resolve_expire) g_now += 1000;
    return 0;
}

/**
 * @brief 结束静态解析候选的借用。
 * @param[in] addresses 模型交付的静态节点，不得由系统 freeaddrinfo 释放。
 * @post 无动态资源需要回收。
 */
static void model_freeaddrinfo(struct addrinfo *addresses)
{
    (void)addresses;
}

/**
 * @brief 为预解析及套接字地址校验交付稳定数字文本。
 * @param[in] address 借用地址，模型忽略其内容。
 * @param[in] address_len 地址字节数，模型忽略。
 * @param[out] host 写入 NUL 结尾数字地址。
 * @param[in] host_len host 容量，单位字节。
 * @param[out] service 未使用的服务输出，可为 NULL。
 * @param[in] service_len 服务输出容量，模型忽略。
 * @param[in] flags 名称解析标记，模型忽略。
 * @return 容量足够返回 0，否则返回 EAI_OVERFLOW。
 */
static int model_getnameinfo(const struct sockaddr *address, socklen_t address_len,
    char *host, socklen_t host_len, char *service, socklen_t service_len, int flags)
{
    (void)address;
    (void)address_len;
    (void)service;
    (void)service_len;
    (void)flags;
    if (host_len < sizeof("127.0.0.1")) return EAI_OVERFLOW;
    memcpy(host, "127.0.0.1", sizeof("127.0.0.1"));
    return 0;
}

#undef CC_HAS_CURL
#define CC_HAS_CURL 1
#define malloc model_malloc
#define calloc model_calloc
#define realloc model_realloc
#define free model_free
#define cc_result_free model_result_free
#define cc_platform_monotonic_ms model_monotonic_ms
#define getaddrinfo model_getaddrinfo
#define freeaddrinfo model_freeaddrinfo
#define getnameinfo model_getnameinfo
#define pthread_sigmask model_pthread_sigmask
#define cc_http_client_create_default model_http_create
#define cc_http_response_free model_response_free
#if CC_TEST_WINDOWS_CURL
#include "../../platforms/windows/src/cc_curl_http_client.c"
#else
#include "../../platforms/posix/src/cc_curl_http_client.c"
#endif
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef cc_result_free
#undef cc_platform_monotonic_ms
#undef getaddrinfo
#undef freeaddrinfo
#undef getnameinfo
#undef pthread_sigmask
#undef cc_http_client_create_default
#undef cc_http_response_free

/**
 * @brief 模拟 curl 全局初始化，并支持首次失败后重试。
 * @param[in] flags 生产传入的全局初始化标记。
 * @return 注入失败时返回 CURLE_OUT_OF_MEMORY，其余返回 CURLE_OK。
 */
CURLcode curl_global_init(long flags)
{
    assert(flags == CURL_GLOBAL_DEFAULT);
    g_global_calls++;
    return g_global_fail ? CURLE_OUT_OF_MEMORY : CURLE_OK;
}

/** @brief 创建一个模型 easy 状态。 @return 新状态归生产清理，分配失败返回 NULL。 */
CURL *curl_easy_init(void)
{
    model_easy_t *easy = calloc(1, sizeof(*easy));
    if (easy) g_handles++;
    return (CURL *)easy;
}

/**
 * @brief 释放模型 easy 状态并核对创建清理配对。
 * @param[in] handle 模型创建的非 NULL 状态。
 * @post handle 已释放，在途模型 handle 计数减少。
 */
void curl_easy_cleanup(CURL *handle)
{
    assert(handle && g_handles > 0);
    g_handles--;
    free(handle);
}

/**
 * @brief 保存生产配置的回调和关键预算，支持指定选项安装失败。
 * @param[in,out] handle 模型 easy 状态。
 * @param[in] option curl 公共选项标识。
 * @param[in] ... 与 option 匹配的参数，类型沿真实 curl API。
 * @return 指定失败选项返回 CURLE_OUT_OF_MEMORY，其余返回 CURLE_OK。
 */
CURLcode curl_easy_setopt(CURL *handle, CURLoption option, ...)
{
    if (option == g_fail_option) return CURLE_OUT_OF_MEMORY;
    model_easy_t *easy = (model_easy_t *)handle;
    va_list args;
    va_start(args, option);
    switch (option) {
    case CURLOPT_WRITEFUNCTION: easy->body = va_arg(args, model_write_callback_fn); break;
    case CURLOPT_HEADERFUNCTION: easy->header = va_arg(args, model_write_callback_fn); break;
    case CURLOPT_XFERINFOFUNCTION: easy->progress = va_arg(args, curl_xferinfo_callback); break;
    case CURLOPT_OPENSOCKETFUNCTION: easy->open_socket = va_arg(args, curl_opensocket_callback); break;
    case CURLOPT_WRITEDATA: easy->body_data = va_arg(args, cc_curl_write_ctx_t *); break;
    case CURLOPT_HEADERDATA: easy->header_data = va_arg(args, cc_curl_write_ctx_t *); break;
    case CURLOPT_XFERINFODATA: easy->progress_data = va_arg(args, cc_curl_write_ctx_t *); break;
    case CURLOPT_OPENSOCKETDATA: easy->socket_data = va_arg(args, cc_curl_write_ctx_t *); break;
    case CURLOPT_TIMEOUT_MS: easy->timeout_ms = va_arg(args, long); break;
    case CURLOPT_NOSIGNAL: easy->nosignal = va_arg(args, long); break;
    case CURLOPT_FOLLOWLOCATION: easy->follow = va_arg(args, long); break;
    default: break;
    }
    va_end(args);
    return CURLE_OK;
}

/**
 * @brief 为模型传输返回最终 HTTP 状态或注入 getinfo 失败。
 * @param[in] handle 借用 easy 状态，当前不读取。
 * @param[in] info 必须为 CURLINFO_RESPONSE_CODE。
 * @param[out] ... 非 NULL 的 long 指针，成功写入 200。
 * @return 注入时返回 CURLE_BAD_FUNCTION_ARGUMENT，其余返回 CURLE_OK。
 */
CURLcode curl_easy_getinfo(CURL *handle, CURLINFO info, ...)
{
    (void)handle;
    assert(info == CURLINFO_RESPONSE_CODE);
    if (g_fail_info) return CURLE_BAD_FUNCTION_ARGUMENT;
    va_list args;
    va_start(args, info);
    *va_arg(args, long *) = 200;
    va_end(args);
    return CURLE_OK;
}

/**
 * @brief 提供模型错误描述。
 * @param[in] code curl 错误码，模型不区分文本。
 * @return 进程期只读文本，无须释放。
 */
const char *curl_easy_strerror(CURLcode code)
{
    (void)code;
    return "model curl failure";
}

/**
 * @brief 深拷贝一条头行到模型链表尾部。
 * @param[in,out] list 当前模型链表，可为 NULL。
 * @param[in] data 借用的 NUL 结尾头行。
 * @return 更新后的链表头，分配失败返回 NULL 并保留原链表。
 */
struct curl_slist *curl_slist_append(struct curl_slist *list, const char *data)
{
    struct curl_slist *item = calloc(1, sizeof(*item));
    if (!item) return NULL;
    item->data = strdup(data);
    if (!item->data) {
        free(item);
        return NULL;
    }
    if (!list) return item;
    struct curl_slist *tail = list;
    while (tail->next) tail = tail->next;
    tail->next = item;
    return list;
}

/**
 * @brief 释放模型头链表及其独占文本。
 * @param[in] list 链表头，可为 NULL。
 * @post 全部节点及文本已释放。
 */
void curl_slist_free_all(struct curl_slist *list)
{
    while (list) {
        struct curl_slist *next = list->next;
        free(list->data);
        free(list);
        list = next;
    }
}

/**
 * @brief 以非 NUL 终止行交付状态和最终字段，验证中间响应块清理。
 * @param[in,out] easy 借用模型状态，回调可能保存生产错误。
 * @return 所有头行均被接受返回 1，任一中止返回 0。
 */
static int send_model_headers(model_easy_t *easy)
{
    const char first[] = "HTTP/1.1 100 Continue: interim\r\n";
    const char old[] = "Old: gone\r\n";
    const char final[] = "HTTP/1.1 200 OK: final\r\n";
    const char field[] = "Content-Type: application/octet-stream\r\n";
    return easy->header((void *)first, 1, sizeof(first) - 1, easy->header_data) == sizeof(first) - 1 &&
        easy->header((void *)old, 1, sizeof(old) - 1, easy->header_data) == sizeof(old) - 1 &&
        easy->header((void *)final, 1, sizeof(final) - 1, easy->header_data) == sizeof(final) - 1 &&
        easy->header((void *)field, 1, sizeof(field) - 1, easy->header_data) == sizeof(field) - 1;
}

/**
 * @brief 根据场景驱动真实生产回调及其错误交接。
 * @param[in,out] handle 模型 easy 状态，已完成生产配置。
 * @return 对应 curl 传输结果，时间推进由场景确定。
 */
CURLcode curl_easy_perform(CURL *handle)
{
    model_easy_t *easy = (model_easy_t *)handle;
    assert(easy->nosignal == 1 && easy->follow == 0);
    assert(g_performs < 4);
    g_timeouts[g_performs] = easy->timeout_ms;
    g_performs++;
#if !CC_TEST_WINDOWS_CURL
    if (g_scenario == MODEL_SIGPIPE) {
        assert(raise(SIGPIPE) == 0);
        return CURLE_SEND_ERROR;
    }
#endif
    if (g_scenario == MODEL_RETRY && g_performs == 1) {
        g_now += 40;
        return CURLE_COULDNT_CONNECT;
    }
    if (g_scenario == MODEL_TOTAL_TIMEOUT) {
        g_now += (uint64_t)easy->timeout_ms;
        return CURLE_OPERATION_TIMEDOUT;
    }
    if (g_scenario == MODEL_FIRST_TIMEOUT) {
        g_now += 60;
        return easy->progress(easy->progress_data, 0, 0, 0, 0)
            ? CURLE_ABORTED_BY_CALLBACK : CURLE_OK;
    }
    if (g_scenario == MODEL_ADDRESS_REJECT) {
        struct curl_sockaddr address;
        memset(&address, 0, sizeof(address));
        address.family = AF_INET;
        address.addrlen = sizeof(struct sockaddr_in);
        assert(easy->open_socket);
        assert(easy->open_socket(easy->socket_data, CURLSOCKTYPE_IPCXN, &address) == CURL_SOCKET_BAD);
        return CURLE_COULDNT_CONNECT;
    }
    if (!send_model_headers(easy)) return CURLE_WRITE_ERROR;
    if (g_scenario == MODEL_EMPTY || g_scenario == MODEL_INFO_ERROR) return CURLE_OK;
    const char bytes[] = {'a', '\0', 'b', 'x'};
    if (easy->body((void *)bytes, 1, sizeof(bytes), easy->body_data) != sizeof(bytes)) return CURLE_WRITE_ERROR;
    if (g_scenario == MODEL_IDLE_TIMEOUT) g_now += 60;
    if (easy->progress(easy->progress_data, 4, 4, 0, 0)) return CURLE_ABORTED_BY_CALLBACK;
    return g_scenario == MODEL_PARTIAL_ERROR ? CURLE_RECV_ERROR : CURLE_OK;
}

/**
 * @brief 同步接收二进制片段，交付拥有型成功消息或指定错误，并可触发取消。
 * @param[in] data 借用 len 字节，只在回调期间读取。
 * @param[in] len 片段字节数。
 * @param[in,out] user_data 指向独占 body_state_t。
 * @return 结果拥有权交给生产 HTTP 层，成功消息用于验证其清理。
 */
static cc_result_t receive_body(const char *data, size_t len, void *user_data)
{
    body_state_t *state = user_data;
    assert(len == 4 && data[0] == 'a' && data[1] == '\0' && data[2] == 'b');
    state->bytes += len;
    state->calls++;
    if (state->cancel_after) cc_cancel_source_cancel(state->cancel_after);
    cc_result_t result = cc_result_error(state->error, "owned callback message");
    if (state->error == CC_OK) {
        assert(!g_success_message);
        g_success_message = result.message;
    }
    return result;
}

/**
 * @brief 记录每次尝试的重试和流片段标记。
 * @param[in] metrics 仅本次回调借用的快照。
 * @param[in,out] user_data 未使用，可为 NULL。
 * @post 全局计数增加，借用指针不保存到回调外。
 */
static void receive_metrics(const cc_http_attempt_metrics_t *metrics, void *user_data)
{
    (void)user_data;
    assert(metrics && metrics->attempt_index == g_metrics);
    g_metrics++;
    g_retry_metrics += metrics->retry_performed;
    g_partial_metrics += metrics->partial_stream;
}

/**
 * @brief 允许预解析候选，在实际打开套接字的第二次校验时拒绝。
 * @param[in] host 借用主机，必须非空。
 * @param[in] address 借用数字地址，必须非空。
 * @param[in,out] user_data 可选调用计数槽，NULL 表示全部允许。
 * @return 第一次校验允许，第二次拒绝，无计数槽时返回 1。
 */
static int validate_address(const char *host, const char *address, void *user_data)
{
    assert(host && address);
    int *calls = user_data;
    return calls ? ++*calls == 1 : 1;
}

/**
 * @brief 拒绝 URL，验证策略失败前已经初始化响应槽。
 * @param[in] url 借用 URL，模型不读取。
 * @param[in,out] user_data 未使用，可为 NULL。
 * @return 始终返回 0。
 */
static int reject_url(const char *url, void *user_data)
{
    (void)url;
    (void)user_data;
    return 0;
}

/**
 * @brief 重置场景计数，保留客户端拥有的生产分配及全局初始化状态。
 * @param[in] scenario 下一次运行场景。
 * @post 时钟、注入项及观测计数归位，不释放在途资源。
 */
static void reset_scenario(enum model_scenario scenario)
{
    assert(g_handles == 0 && !g_success_message);
    g_scenario = scenario;
    g_now = 100;
    g_performs = g_metrics = g_retry_metrics = g_partial_metrics = 0;
    g_alloc_calls = g_fail_alloc = g_success_frees = 0;
    g_fail_option = 0;
    g_fail_info = g_resolves = g_resolve_cancel = g_resolve_expire = 0;
    g_resolve_source = NULL;
    g_mask_fail_next = 0;
    memset(g_timeouts, 0, sizeof(g_timeouts));
}

/**
 * @brief 创建一个用于各定向场景的零借用默认请求。
 * @return 按值返回以 100ms 总预算和两次追加机会执行的请求。
 */
static cc_http_request_t make_request(void)
{
    cc_http_request_t request = {0};
    request.url = "http://example.test/path";
    request.timeout_ms = 100;
    request.retry_count = 2;
    request.on_attempt_metrics = receive_metrics;
    return request;
}

/**
 * @brief 验证缓冲及流式的二进制交付、响应块隔离和拥有型成功结果清理。
 * @param[in,out] client 已创建的模型客户端，本函数不销毁。
 * @post 所有本次响应及结果释放，客户端继续有效。
 */
static void test_body_and_headers(cc_http_client_t *client)
{
    reset_scenario(MODEL_SUCCESS);
    cc_http_request_t request = make_request();
    cc_http_response_t response = {0};
    cc_result_t rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_OK && response.status_code == 200);
    assert(response.body_size == 4 && response.body[1] == '\0' && response.body[4] == '\0');
    assert(response.header_count == 1 && !strcmp(response.headers[0].name, "Content-Type"));
    cc_result_free(&rc);
    model_response_free(&response);
    body_state_t state = {0};
    request.on_body = receive_body;
    request.user_data = &state;
    reset_scenario(MODEL_SUCCESS);
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_OK && state.bytes == 4 && response.body_size == 0);
    assert(g_success_frees == 1);
    cc_result_free(&rc);
    model_response_free(&response);
    reset_scenario(MODEL_SUCCESS);
    request.max_response_bytes = 4;
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_OK && response.body_size == 4 && response.body[1] == '\0');
    cc_result_free(&rc);
    model_response_free(&response);
    reset_scenario(MODEL_SUCCESS);
    request.max_response_bytes = 3;
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_LIMIT_EXCEEDED && !response.body && !response.headers);
    assert(g_performs == 1 && g_success_frees == 0);
    cc_result_free(&rc);
}

/**
 * @brief 验证总期限共享、首片段和空闲超时、部分流不可重试及回调错误优先。
 * @param[in,out] client 借用模型客户端。
 * @post 所有响应、令牌及结果均已释放。
 */
static void test_budgets_and_retry(cc_http_client_t *client)
{
    cc_http_request_t request = make_request();
    cc_http_response_t response = {0};
    reset_scenario(MODEL_RETRY);
    cc_result_t rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_OK && g_performs == 2 && g_timeouts[0] == 100 && g_timeouts[1] == 60);
    assert(g_metrics == 2 && g_retry_metrics == 1);
    cc_result_free(&rc);
    model_response_free(&response);
    reset_scenario(MODEL_RETRY);
    request.first_byte_timeout_ms = 30;
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_TIMEOUT && g_performs == 1 && !response.body);
    cc_result_free(&rc);
    request.first_byte_timeout_ms = 0;
    reset_scenario(MODEL_TOTAL_TIMEOUT);
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_TIMEOUT && g_performs == 1);
    cc_result_free(&rc);
    reset_scenario(MODEL_FIRST_TIMEOUT);
    request.first_byte_timeout_ms = 50;
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_TIMEOUT && g_performs == 1);
    cc_result_free(&rc);
    request.first_byte_timeout_ms = 0;
    request.idle_timeout_ms = 50;
    reset_scenario(MODEL_IDLE_TIMEOUT);
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_TIMEOUT && !response.body);
    cc_result_free(&rc);
    request.idle_timeout_ms = 0;
    body_state_t state = {0};
    request.on_body = receive_body;
    request.user_data = &state;
    reset_scenario(MODEL_PARTIAL_ERROR);
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_NETWORK && g_performs == 1 && g_partial_metrics == 1);
    assert(g_success_frees == 1);
    cc_result_free(&rc);
    reset_scenario(MODEL_SUCCESS);
    state.error = CC_ERR_TOOL;
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_TOOL && rc.message && g_performs == 1);
    cc_result_free(&rc);
    reset_scenario(MODEL_SUCCESS);
    cc_cancel_source_t *source = NULL;
    rc = cc_cancel_source_create(&source);
    assert(rc.code == CC_OK);
    cc_result_free(&rc);
    state.error = CC_OK;
    state.cancel_after = source;
    request.cancel_token = cc_cancel_source_token(source);
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_CANCELLED && g_performs == 1 && g_success_frees == 1);
    cc_result_free(&rc);
    cc_cancel_source_destroy(source);
    reset_scenario(MODEL_RETRY);
    cc_http_client_options_t options = {0};
    options.retry_count = 3;
    options.tls_verify = 1;
    cc_http_client_t retrying_client = {0};
    rc = model_http_create(&options, &retrying_client);
    assert(rc.code == CC_OK);
    cc_result_free(&rc);
    request = make_request();
    request.retry_count = -1;
    rc = cc_http_client_perform(&retrying_client, &request, &response);
    assert(rc.code == CC_ERR_NETWORK && g_performs == 1 && g_retry_metrics == 0);
    cc_result_free(&rc);
    cc_http_client_destroy(&retrying_client);
}

/**
 * @brief 验证参数、取消、期限及策略失败不交付半响应，解析返回后重新检查预算。
 * @param[in,out] client 借用客户端。
 * @post 临时取消令牌与结果已释放，输出保持为空。
 */
static void test_preflight_and_policy(cc_http_client_t *client)
{
    cc_http_request_t request = make_request();
    cc_http_response_t response;
    memset(&response, 0x5a, sizeof(response));
    reset_scenario(MODEL_SUCCESS);
    request.validate_url = reject_url;
    cc_result_t rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_PERMISSION_DENIED && !response.body && !response.headers && g_performs == 0);
    cc_result_free(&rc);
    request.validate_url = NULL;
    request.header_count = 1;
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_INVALID_ARGUMENT && !response.body);
    cc_result_free(&rc);
    request.header_count = 0;
    request.deadline_ms = 99;
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_TIMEOUT && g_resolves == 0);
    cc_result_free(&rc);
    request.deadline_ms = 1000;
    cc_cancel_source_t *source = NULL;
    rc = cc_cancel_source_create(&source);
    assert(rc.code == CC_OK);
    cc_result_free(&rc);
    request.cancel_token = cc_cancel_source_token(source);
    request.validate_address = validate_address;
    cc_cancel_source_cancel(source);
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_CANCELLED && g_resolves == 0);
    cc_result_free(&rc);
    cc_cancel_source_destroy(source);
    source = NULL;
    rc = cc_cancel_source_create(&source);
    assert(rc.code == CC_OK);
    cc_result_free(&rc);
    request.cancel_token = cc_cancel_source_token(source);
    reset_scenario(MODEL_SUCCESS);
    g_resolve_cancel = 1;
    g_resolve_source = source;
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_CANCELLED && g_resolves == 1 && g_performs == 0);
    cc_result_free(&rc);
    cc_cancel_source_destroy(source);
    request.cancel_token = NULL;
    reset_scenario(MODEL_SUCCESS);
    g_resolve_expire = 1;
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_TIMEOUT && g_performs == 0);
    cc_result_free(&rc);
    reset_scenario(MODEL_ADDRESS_REJECT);
    int address_calls = 0;
    request.validate_address_user_data = &address_calls;
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_PERMISSION_DENIED && g_performs == 1 && g_retry_metrics == 0);
    cc_result_free(&rc);
}

/**
 * @brief 穷举正常与空正文路径的生产分配失败，验证配置、头和正文没有遗留。
 * @param[in,out] client 借用客户端，拥有表基线只包含该客户端。
 * @post 每个失败结果均释放，分配表恢复到调用前大小。
 */
static void test_allocation_and_setup(cc_http_client_t *client)
{
    size_t baseline = g_owned_count;
    for (int empty = 0; empty < 2; empty++) {
        reset_scenario(empty ? MODEL_EMPTY : MODEL_SUCCESS);
        cc_http_request_t request = make_request();
        cc_http_response_t response = {0};
        cc_result_t rc = cc_http_client_perform(client, &request, &response);
        assert(rc.code == CC_OK);
        int allocations = g_alloc_calls;
        cc_result_free(&rc);
        model_response_free(&response);
        assert(g_owned_count == baseline);
        for (int failure = 1; failure <= allocations; failure++) {
            reset_scenario(empty ? MODEL_EMPTY : MODEL_SUCCESS);
            g_fail_alloc = failure;
            rc = cc_http_client_perform(client, &request, &response);
            assert(rc.code == CC_ERR_OUT_OF_MEMORY && !response.body && !response.headers);
            cc_result_free(&rc);
            assert(g_owned_count == baseline && g_handles == 0);
        }
    }
    reset_scenario(MODEL_SUCCESS);
    g_fail_option = CURLOPT_SSL_VERIFYPEER;
    cc_http_request_t request = make_request();
    cc_http_response_t response = {0};
    cc_result_t rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_OUT_OF_MEMORY && g_performs == 0 && g_handles == 0);
    cc_result_free(&rc);
    reset_scenario(MODEL_INFO_ERROR);
    g_fail_info = 1;
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_NETWORK && !response.headers && !response.body);
    cc_result_free(&rc);
}

/**
 * @brief 直接覆盖真实静态回调的计数溢出防护，避免构造无法分配的网络输入。
 * @post 所有错误结果释放，合成计数恢复后响应能够安全清理。
 */
static void test_callback_boundaries(void)
{
    reset_scenario(MODEL_SUCCESS);
    cc_http_response_t response = {0};
    response.header_count = SIZE_MAX / sizeof(*response.headers);
    cc_result_t rc = response_header_append(&response, "X: y", 4);
    assert(rc.code == CC_ERR_LIMIT_EXCEEDED);
    cc_result_free(&rc);
    response.header_count = 0;
    assert(!copy_trimmed_header_piece("x", SIZE_MAX));
    cc_curl_write_ctx_t ctx = {0};
    ctx.response = &response;
    ctx.deadline_ms = UINT64_MAX;
    assert(cc_curl_write_header("x", SIZE_MAX, 2, &ctx) == 0);
    assert(ctx.callback_error.code == CC_ERR_LIMIT_EXCEEDED);
    cc_result_free(&ctx.callback_error);
    memset(&ctx.callback_error, 0, sizeof(ctx.callback_error));
    assert(cc_curl_write_body("x", SIZE_MAX, 2, &ctx) == 0);
    assert(ctx.callback_error.code == CC_ERR_LIMIT_EXCEEDED);
    cc_result_free(&ctx.callback_error);
    model_response_free(&response);
}

#if !CC_TEST_WINDOWS_CURL
/**
 * @brief 用真实 pthread 信号状态验证生产保护区，覆盖新信号、旧 pending、原屏蔽和进入失败。
 * @param[in,out] client 借用模型客户端，不改变其配置。
 * @post 所有结果和响应清理，测试开始前的线程 mask 恢复，测试产生的 pending 均消费。
 * @note 只验证 Linux 当前线程信号行为，真实 TLS 后端和 Apple 信号实现仍需原生验证。
 */
static void test_sigpipe_guard(cc_http_client_t *client)
{
    sigset_t only_pipe, original, unblocked, current, pending;
    sigemptyset(&only_pipe);
    sigaddset(&only_pipe, SIGPIPE);
    assert(pthread_sigmask(SIG_BLOCK, &only_pipe, &original) == 0);
    assert(sigpending(&pending) == 0 && !sigismember(&pending, SIGPIPE));
    unblocked = original;
    sigdelset(&unblocked, SIGPIPE);
    assert(pthread_sigmask(SIG_SETMASK, &unblocked, NULL) == 0);
    reset_scenario(MODEL_SIGPIPE);
    cc_http_request_t request = make_request();
    request.retry_count = 0;
    cc_http_response_t response = {0};
    cc_result_t rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_NETWORK && g_performs == 1);
    cc_result_free(&rc);
    assert(pthread_sigmask(SIG_BLOCK, NULL, &current) == 0 && !sigismember(&current, SIGPIPE));
    assert(sigpending(&pending) == 0 && !sigismember(&pending, SIGPIPE));
    assert(pthread_sigmask(SIG_BLOCK, &only_pipe, NULL) == 0);
    reset_scenario(MODEL_SIGPIPE);
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_NETWORK);
    cc_result_free(&rc);
    assert(sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE));
    assert(pthread_sigmask(SIG_BLOCK, NULL, &current) == 0 && sigismember(&current, SIGPIPE));
    int received;
    assert(sigwait(&only_pipe, &received) == 0 && received == SIGPIPE);
    assert(raise(SIGPIPE) == 0);
    reset_scenario(MODEL_SIGPIPE);
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_NETWORK);
    cc_result_free(&rc);
    assert(sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE));
    assert(sigwait(&only_pipe, &received) == 0 && received == SIGPIPE);
    assert(pthread_sigmask(SIG_SETMASK, &unblocked, NULL) == 0);
    reset_scenario(MODEL_SUCCESS);
    g_mask_fail_next = 1;
    rc = cc_http_client_perform(client, &request, &response);
    assert(rc.code == CC_ERR_PLATFORM && g_performs == 0 && !response.body && !response.headers);
    cc_result_free(&rc);
    assert(pthread_sigmask(SIG_BLOCK, NULL, &current) == 0 && !sigismember(&current, SIGPIPE));
    assert(pthread_sigmask(SIG_SETMASK, &original, NULL) == 0);
}
#endif

/**
 * @brief 执行两种编译变体共用的 curl 生产控制流回归。
 * @return 全部断言成立返回 0，失败由 assert 终止。
 * @note 模型覆盖生产回调、清理及预算，真实 libcurl、DNS、TLS、Windows ABI 和网络调度需独立原生验证。
 */
int main(void)
{
    reset_scenario(MODEL_SUCCESS);
    cc_http_client_t client = {0};
    g_global_fail = 1;
    cc_result_t rc = model_http_create(NULL, &client);
    assert(rc.code == CC_ERR_OUT_OF_MEMORY && !client.self && g_global_calls == 1);
    cc_result_free(&rc);
    g_global_fail = 0;
    g_fail_alloc = 1;
    rc = model_http_create(NULL, &client);
    assert(rc.code == CC_ERR_OUT_OF_MEMORY && !client.self && g_global_calls == 2);
    cc_result_free(&rc);
    g_fail_alloc = 0;
    rc = model_http_create(NULL, &client);
    assert(rc.code == CC_OK && g_global_calls == 2);
    cc_result_free(&rc);
    test_body_and_headers(&client);
    test_budgets_and_retry(&client);
    test_preflight_and_policy(&client);
    test_allocation_and_setup(&client);
    test_callback_boundaries();
#if !CC_TEST_WINDOWS_CURL
    test_sigpipe_guard(&client);
#endif
    rc = cc_http_client_reset_connections(&client);
    assert(rc.code == CC_OK);
    cc_result_free(&rc);
    cc_http_client_destroy(&client);
    assert(g_owned_count == 0 && g_handles == 0 && !g_success_message);
    return 0;
}
