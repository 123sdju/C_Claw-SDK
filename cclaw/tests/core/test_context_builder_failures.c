#include "cc/app/cc_context_builder.h"
#include "cc/core/cc_observability.h"
#include "cc/util/cc_string_builder.h"
#include "cc/util/cc_token_counter.h"
#include "cc/ports/cc_platform.h"
#include "../../core/src/app/cc_agent_runtime_internal.h"
#include "cJSON.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "断言失败，第 %d 行\n", __LINE__); return 1; } } while (0)

/** @brief 单线程故障注入状态，全部借用在所属用例返回前结束。 */
static struct {
    char *diagnostics[64]; /**< 本文件交给生产函数消费的动态诊断。 */
    size_t diagnostic_count; /**< 当前尚未观察到释放的诊断数。 */
    int fail_serialization; /**< 消息 JSON 序列化返回 OOM。 */
    int fail_summary; /**< 下一次文本摘要读取返回 OOM。 */
    int fail_builder_init; /**< 摘要提示构建器初始化返回 OOM。 */
    int fail_prompt; /**< 摘要消息构造返回 OOM。 */
    int token_total; /**< 大于零时为六条历史安排精确的总估计。 */
    int token_call; /**< 逆序估算时的调用次数。 */
    int partial_load; /**< 交付可清理数组后返回读取失败。 */
    int fail_hydrate; /**< 媒体入口返回 OOM，由整体输出撤销负责清理。 */
    int cancel_at; /**< 1 在加载返回前取消，2 在媒体加载返回前取消。 */
    cc_cancel_source_t *cancel_source; /**< 用例拥有的取消源借用。 */
    uintptr_t loaded_address; /**< 本轮后端数组首地址的数值，仅用于识别 cleanup 调用。 */
    size_t loaded_count; /**< 后端数组的实际消息数。 */
    size_t cleaned; /**< 已观察到的后端数组逐项清理数。 */
    int chat_calls; /**< 摘要 provider 调用次数。 */
    int hydrate_calls; /**< 媒体加载次数。 */
    int required_valid; /**< 必需消息是否保留最近的用户消息标识。 */
    size_t media_budget; /**< 实际传入媒体回调的字节预算。 */
    uint64_t clock_ms; /**< 本用例的单调时钟值。 */
    uint64_t observed_deadline; /**< provider 观察到的摘要截止时刻。 */
    const char *response_text; /**< 同步借用的假摘要文本。 */
    int capabilities_supported; /**< 能力查询是否支持文本输出。 */
    const char *persistent_key; /**< 假持久查询同步复制的候选键。 */
} state;

/** @brief 生成由生产路径负责释放的可计数诊断。
 * @param[in] code 所需结果码，允许 CC_OK 携带动态消息。
 * @return 拥有结果，调用者须最终通过 tracked_result_free 消费。
 */
static cc_result_t diagnostic(cc_error_code_t code)
{
    cc_result_t rc = cc_result_error(code, "可计数的动态诊断");
    if (rc.message) {
        for (size_t i = 0; i < 64; i++) {
            if (!state.diagnostics[i]) {
                state.diagnostics[i] = rc.message;
                state.diagnostic_count++;
                break;
            }
        }
    }
    return rc;
}

/** @brief 记录被消费的诊断后调用真实结果清理。
 * @param[in,out] rc 可清理的结果指针，清理后沿真实 API 归零。
 * @post 已登记的动态消息计数递减，未登记结果按普通结果处理。
 */
static void tracked_result_free(cc_result_t *rc)
{
    if (rc && rc->message) {
        for (size_t i = 0; i < 64; i++) {
            if (state.diagnostics[i] == rc->message) {
                state.diagnostics[i] = NULL;
                state.diagnostic_count--;
                break;
            }
        }
    }
    cc_result_free(rc);
}

/** @brief 记录后端拥有消息的清理，再委托真实消息清理。
 * @param[in,out] message 借用待清理元素，可为 NULL。
 * @post 消息字段释放，本用例的后端数组清理计数相应增加。
 */
static void tracked_message_cleanup(cc_message_t *message)
{
    for (size_t i = 0; i < state.loaded_count; i++)
        if ((uintptr_t)message == state.loaded_address + i * sizeof(*message)) state.cleaned++;
    cc_message_cleanup(message);
}

/** @brief 在必需的序列化阶段注入可定位 OOM，其余调用真实实现。
 * @param[in] messages 借用的稳定消息数组。
 * @param[in] count 数组项数。
 * @param[in] include_reasoning_content 传给真实序列化器的推理正文标志。
 * @param[out] out_json 新拥有槽，故障时清 NULL。
 * @return 注入 OOM 或真实序列化结果。
 */
static cc_result_t test_messages_to_json(const cc_message_t *messages, size_t count,
                                        int include_reasoning_content, char **out_json)
{
    if (state.fail_serialization) { *out_json = NULL; return diagnostic(CC_ERR_OUT_OF_MEMORY); }
    return cc_messages_to_json(messages, count, include_reasoning_content, out_json);
}

/** @brief 在用户命令分类所需的文本读取中注入 OOM。
 * @param[in] message 借用源消息。
 * @param[out] out_text 新拥有槽，故障时清 NULL。
 * @return 注入 OOM 或真实摘要结果。
 */
static cc_result_t test_text_summary(const cc_message_t *message, char **out_text)
{
    if (state.fail_summary) {
        state.fail_summary = 0;
        *out_text = NULL;
        return diagnostic(CC_ERR_OUT_OF_MEMORY);
    }
    return cc_message_get_text_summary(message, out_text);
}

/** @brief 注入摘要提示构建器的初始化失败。
 * @param[out] builder 真实实现成功时初始化的独占槽，失败注入不写该槽。
 * @return 注入 OOM 或真实初始化结果。
 */
static cc_result_t test_builder_init(cc_string_builder_t *builder)
{
    if (state.fail_builder_init) return diagnostic(CC_ERR_OUT_OF_MEMORY);
    return cc_string_builder_init(builder);
}

/** @brief 注入摘要提示消息的构造失败，其余消息仍走真实深拷贝。
 * @param[in] id 可空消息标识借用。
 * @param[in] session_id 可空会话标识借用。
 * @param[in] role 消息角色。
 * @param[in] text 文本借用。
 * @param[in] tool_call_id 可空工具关联标识借用。
 * @param[out] out_message 新拥有槽，注入失败时清 NULL。
 * @return 注入 OOM 或真实消息构造结果。
 */
static cc_result_t test_create_text(const char *id, const char *session_id,
    cc_message_role_t role, const char *text, const char *tool_call_id, cc_message_t **out_message)
{
    if (state.fail_prompt && id && strcmp(id, "summary_prompt") == 0) {
        *out_message = NULL;
        return diagnostic(CC_ERR_OUT_OF_MEMORY);
    }
    return cc_message_create_text(id, session_id, role, text, tool_call_id, out_message);
}

/** @brief 为六条历史设置稳定的逆序成本，用于阈值边界检查。
 * @param[in] json 真实序列化文本，普通模式交真实估算器。
 * @return 注入模式依次为 1、1、1、1、1、剩余成本，普通模式为真实估计。
 */
static int test_token_estimate(const char *json)
{
    if (!state.token_total) return cc_token_estimate_json_messages(json);
    return state.token_call++ < 5 ? 1 : state.token_total - 5;
}

/** @brief 返回确定的单调毫秒值以检查截止时间饱和。
 * @return 本用例设置的 uint64_t 毫秒值。
 */
static uint64_t test_clock(void) { return state.clock_ms; }

/** @brief 接收尽力完成的缓存观测并返回可计数成功诊断。
 * @param[in,out] bus 未解引用的假总线借用。
 * @param[in] event 同步借用的完整事件。
 * @return 拥有的 CC_OK 诊断，由生产函数消费。
 */
static cc_result_t test_publish(cc_event_bus_t *bus, const cc_observability_event_t *event)
{
    (void)bus; (void)event;
    return diagnostic(CC_OK);
}

static void *json_allocations[128]; /**< 钩子独占分配登记，仅在观测故障循环内启用。 */
static size_t json_live, json_calls, json_fail_at;

/** @brief 记录真实 cJSON 分配并在指定次序注入 OOM。
 * @param[in] bytes 请求字节数。
 * @return 拥有分配或 NULL，钩子期间的分配由 test_free 消费。
 */
static void *test_json_malloc(size_t bytes)
{
    if (++json_calls == json_fail_at) return NULL;
    void *memory = malloc(bytes);
    if (memory) for (size_t i = 0; i < 128; i++) if (!json_allocations[i]) {
        json_allocations[i] = memory; json_live++; break;
    }
    return memory;
}

/** @brief 注销 cJSON 分配后释放，也接受生产路径的普通堆缓冲。
 * @param[in,out] memory 待释放分配，可为 NULL。
 * @post 分配寿命结束，登记分配的存活计数递减。
 */
static void test_free(void *memory)
{
    if (memory) for (size_t i = 0; i < 128; i++) if (json_allocations[i] == memory) {
        json_allocations[i] = NULL; json_live--; break;
    }
    free(memory);
}

/* 仅把本翻译单元中的生产调用接到注入点，真实 SDK 依赖继续链接普通实现。 */
#define cc_result_free tracked_result_free
#define cc_message_cleanup tracked_message_cleanup
#define cc_messages_to_json test_messages_to_json
#define cc_message_get_text_summary test_text_summary
#define cc_string_builder_init test_builder_init
#define cc_message_create_text test_create_text
#define cc_token_estimate_json_messages test_token_estimate
#define cc_platform_monotonic_ms test_clock
#define cc_observability_publish test_publish
#define free test_free
#ifdef CC_CONTEXT_MEDIA_MAX_BYTES
#undef CC_CONTEXT_MEDIA_MAX_BYTES
#endif
#define CC_CONTEXT_MEDIA_MAX_BYTES (512U * 1024U)
#ifndef CC_CONTEXT_TEST_SOURCE
#define CC_CONTEXT_TEST_SOURCE "../../core/src/app/cc_context_builder.c"
#endif
#include CC_CONTEXT_TEST_SOURCE
#undef cc_result_free
#undef cc_message_cleanup
#undef cc_messages_to_json
#undef cc_message_get_text_summary
#undef cc_string_builder_init
#undef cc_message_create_text
#undef cc_token_estimate_json_messages
#undef cc_platform_monotonic_ms
#undef cc_observability_publish
#undef free

/** @brief 消费结果并判定成功。
 * @param[in] rc 按值接收的拥有结果。
 * @return CC_OK 返回 1，其他返回 0。
 */
static int ok(cc_result_t rc)
{
    int success = rc.code == CC_OK;
    tracked_result_free(&rc);
    return success;
}

/** @brief 交付六条独立用户消息，可在交付后返回失败或触发取消。
 * @param[in] self 未使用的后端上下文。
 * @param[in] session_id 借用会话键，复制进消息。
 * @param[in] limit 未使用的加载上限，本 fixture 总是交付六条。
 * @param[out] out_messages 新拥有数组槽，成功及部分失败均可清理六项。
 * @param[out] out_count 实际交付数，分配失败为零。
 * @return 真实消息构造错误、注入的 IO 错误或可计数 CC_OK。
 */
static cc_result_t load_history(void *self, const char *session_id, int limit,
    cc_message_t **out_messages, size_t *out_count)
{
    (void)self; (void)limit;
    *out_count = 0;
    *out_messages = calloc(6, sizeof(**out_messages));
    if (!*out_messages) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "历史数组分配失败");
    for (size_t i = 0; i < 6; i++) {
        char id[16]; snprintf(id, sizeof(id), "user-%zu", i);
        cc_message_t *message = NULL;
        cc_result_t rc = cc_message_create_text(id, session_id, CC_ROLE_USER,
            "ordinary user question with stable history", NULL, &message);
        if (rc.code != CC_OK) return rc;
        cc_result_free(&rc);
        (*out_messages)[i] = *message;
        free(message);
        (*out_count)++;
    }
    state.loaded_address = (uintptr_t)*out_messages;
    state.loaded_count = *out_count;
    if (state.cancel_at == 1) cc_cancel_source_cancel(state.cancel_source);
    return diagnostic(state.partial_load ? CC_ERR_IO : CC_OK);
}

/** @brief 核对媒体预算和当前用户下标，并可触发末次取消。
 * @param[in] self 未使用的后端上下文。
 * @param[in] session_id 未使用的会话键。
 * @param[in,out] messages 借用输出数组，本回调只检查标识。
 * @param[in] count 实际数组项数。
 * @param[in] required_message 必需用户消息下标。
 * @param[in] max_base64_bytes 生产构建器传入的总编码预算。
 * @return 可计数的成功诊断或注入 OOM，交生产函数消费或向调用者转交。
 */
static cc_result_t hydrate(void *self, const char *session_id, cc_message_t *messages,
    size_t count, size_t required_message, size_t max_base64_bytes)
{
    (void)self; (void)session_id;
    state.hydrate_calls++;
    state.media_budget = max_base64_bytes;
    state.required_valid = required_message < count && messages[required_message].id &&
        strcmp(messages[required_message].id, "user-5") == 0;
    if (state.cancel_at == 2) cc_cancel_source_cancel(state.cancel_source);
    return diagnostic(state.fail_hydrate ? CC_ERR_OUT_OF_MEMORY : CC_OK);
}

/** @brief 返回短摘要并记录生产请求的截止时刻。
 * @param[in] self 未使用的 provider 上下文。
 * @param[in] request 同步借用的摘要请求。
 * @param[out] response 独占响应槽，取得假摘要副本。
 * @return 文本设置错误或可计数 CC_OK，由生产摘要路径消费。
 */
static cc_result_t chat(void *self, const cc_llm_chat_request_t *request, cc_llm_response_t *response)
{
    (void)self;
    state.chat_calls++;
    state.observed_deadline = request->deadline_ms;
    cc_result_t rc = cc_llm_response_set_text(response, state.response_text ? state.response_text : "s");
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    return diagnostic(CC_OK);
}

/** @brief 用动态成功诊断检查运行时能力校验的正常与拒绝分支。
 * @param[in] self 未使用的 provider 上下文。
 * @param[out] capabilities 完整能力槽，写入本测试支持的文本能力。
 * @return 带动态消息的 CC_OK，真实 runtime 必须消费该消息。
 */
static cc_result_t capabilities(void *self, cc_llm_provider_capabilities_t *capabilities)
{
    (void)self;
    memset(capabilities, 0, sizeof(*capabilities));
    capabilities->size = sizeof(*capabilities);
    capabilities->text_input = 1;
    capabilities->text_output = state.capabilities_supported;
    return cc_result_error(CC_OK, "能力查询的成功诊断");
}

static const cc_session_store_vtable_t sessions = {
    .load_messages = load_history, .hydrate_context_media = hydrate
};
static const cc_llm_provider_vtable_t provider = {.chat = chat, .capabilities = capabilities};

/** @brief 创建测试拥有的运行时，存储及 provider 继续借用静态虚表。
 * @param[in] registry 借用已冻结的空工具注册表。
 * @param[out] out_runtime 成功取得拥有实例，失败保持原槽。
 * @return 真实创建结果，调用者消费诊断。
 */
static cc_result_t create_runtime(cc_tool_registry_t *registry, cc_agent_runtime_t **out_runtime)
{
    cc_agent_runtime_deps_t deps = {0};
    deps.size = sizeof(deps); deps.store.vtable = &sessions;
    deps.llm.vtable = &provider; deps.tool_registry = registry;
    cc_agent_runtime_options_t options = {0}; options.size = sizeof(options);
    options.config.model = "fake"; options.config.system_prompt = "system";
    options.config.context_keep_recent = 1;
    return cc_agent_runtime_create(&deps, &options, out_runtime);
}

/** @brief 检查读取/估算/分类错误与两个取消时点均撤销全部输出。
 * @param[in,out] runtime 本测试独占运行时，结束前还原存储。
 * @return 全部断言通过返回 0，否则返回 1。
 */
static int test_required_failures(cc_agent_runtime_t *runtime)
{
    const struct {
        int partial_load, serialization, summary, cancel_at, missing_store, hydrate;
        cc_error_code_t expected;
    } cases[] = {
        {1, 0, 0, 0, 0, 0, CC_ERR_IO},
        {0, 1, 0, 0, 0, 0, CC_ERR_OUT_OF_MEMORY},
        {0, 0, 1, 0, 0, 0, CC_ERR_OUT_OF_MEMORY},
        {0, 0, 0, 1, 0, 0, CC_ERR_CANCELLED},
        {0, 0, 0, 2, 0, 0, CC_ERR_CANCELLED},
        {0, 0, 0, 0, 1, 0, CC_ERR_STORAGE},
        {0, 0, 0, 0, 0, 1, CC_ERR_OUT_OF_MEMORY},
        {1, 0, 0, 1, 0, 0, CC_ERR_IO},
        {0, 0, 0, 2, 0, 1, CC_ERR_OUT_OF_MEMORY},
    };
    for (size_t mode = 0; mode < sizeof(cases) / sizeof(cases[0]); mode++) {
        state.partial_load = cases[mode].partial_load;
        state.fail_serialization = cases[mode].serialization;
        state.fail_summary = cases[mode].summary;
        state.fail_hydrate = cases[mode].hydrate;
        state.cancel_at = cases[mode].cancel_at;
        state.loaded_address = 0; state.loaded_count = state.cleaned = 0;
        CHECK(ok(cc_cancel_source_create(&state.cancel_source)));
        cc_session_store_t saved = runtime->store;
        if (cases[mode].missing_store) runtime->store.vtable = NULL;
        cc_message_t *messages = (void *)1; size_t count = 42;
        cc_result_t rc = cc_context_builder_build_messages(runtime, "failure", "system",
            cc_cancel_source_token(state.cancel_source), &messages, &count);
        runtime->store = saved;
        CHECK(rc.code == cases[mode].expected);
        tracked_result_free(&rc);
        CHECK(!messages && !count && state.cleaned == state.loaded_count);
        CHECK(state.diagnostic_count == 0);
        cc_cancel_source_destroy(state.cancel_source); state.cancel_source = NULL;
    }
    state.partial_load = state.fail_serialization = state.fail_summary = state.fail_hydrate = state.cancel_at = 0;
    return 0;
}

/** @brief 验证正常回调的成功诊断被消费及阈值 9、10 对 9.5 的严格大于语义。
 * @param[in,out] runtime 独占运行时，用例期间调整其固定配置。
 * @return 全部断言通过返回 0，否则返回 1，正常路径释放上下文。
 */
static int test_threshold_and_success(cc_agent_runtime_t *runtime)
{
    int fixed = 0;
    CHECK(ok(fixed_context_cost(runtime, "system", "", &fixed)));
    runtime->config.context_window_tokens = fixed + 10;
    runtime->config.max_tokens = 0;
    runtime->config.context_compress_threshold = 0.95;
    for (int total = 9; total <= 10; total++) {
        state.token_total = total; state.token_call = state.chat_calls = 0;
        cc_message_t *messages = NULL; size_t count = 0;
        CHECK(ok(cc_context_builder_build_messages(runtime, total == 9 ? "nine" : "ten",
                                                  "system", NULL, &messages, &count)));
        CHECK(state.chat_calls == (total == 10));
        CHECK(state.required_valid && state.media_budget == 512U * 1024U);
        CHECK(state.diagnostic_count == 0);
        free_loaded_messages(messages, count);
    }
    runtime->config.context_keep_recent = INT_MAX;
    runtime->config.context_compress_threshold = INFINITY;
    state.token_call = 0;
    cc_message_t *messages = NULL; size_t count = 0;
    CHECK(ok(cc_context_builder_build_messages(runtime, "infinite", "system", NULL, &messages, &count)));
    free_loaded_messages(messages, count);
    state.token_total = 0;
    runtime->config.context_keep_recent = 1;
    runtime->config.context_window_tokens = 0;
    return 0;
}

/** @brief 验证派生摘要构造错误清理、截止时刻饱和与响应体超限回退。
 * @param[in,out] runtime 独占运行时，测试后归还加载的历史及所有局部资源。
 * @return 所有断言通过返回 0，否则返回 1。
 */
static int test_summary_fallbacks(cc_agent_runtime_t *runtime)
{
    cc_message_t *history = NULL; size_t count = 0;
    CHECK(ok(load_history(NULL, "summary", 6, &history, &count)));
    for (int mode = 0; mode < 4; mode++) {
        state.fail_builder_init = mode == 0; state.fail_prompt = mode == 1;
        state.clock_ms = UINT64_MAX - 10; state.observed_deadline = 0;
        runtime->config.limits.provider_timeout_ms = 100;
        runtime->config.limits.max_output_bytes = mode == 3 ? 1 : 0;
        state.response_text = mode == 3 ? "too long" : "s";
        char *summary = (void *)1;
        int compressed = try_compress_history(runtime, "summary", NULL, history, 0, count,
                                             NULL, NULL, &summary);
        CHECK(compressed == (mode == 2));
        CHECK((summary != NULL) == (mode == 2));
        if (mode >= 2) CHECK(state.observed_deadline == UINT64_MAX);
        free(summary);
        CHECK(state.diagnostic_count == 0);
    }
    state.fail_builder_init = state.fail_prompt = 0; state.response_text = NULL;
    runtime->config.limits.max_output_bytes = 0;
    free_loaded_messages(history, count);
    return 0;
}

/** @brief 对观测 JSON 的每个单点分配失败验证子节点及成功诊断全部消费。
 * @param[in,out] runtime 独占运行时，临时假总线仅由被替换的发布函数接收。
 * @return 所有分配故障通过返回 0，否则返回 1，正常路径恢复 cJSON 默认钩子。
 */
static int test_observation_oom(cc_agent_runtime_t *runtime)
{
    cJSON_Hooks hooks = {test_json_malloc, test_free};
    cJSON_InitHooks(&hooks);
    runtime->event_bus = (void *)1;
    json_calls = json_fail_at = 0;
    publish_summary_cache_event(runtime, "session", "run", "miss", "none", "reason");
    size_t allocations = json_calls;
    CHECK(json_live == 0 && state.diagnostic_count == 0);
    for (size_t fail = 1; fail <= allocations; fail++) {
        json_calls = 0; json_fail_at = fail;
        publish_summary_cache_event(runtime, "session", "run", "miss", "none", "reason");
        CHECK(json_live == 0 && state.diagnostic_count == 0);
    }
    runtime->event_bus = NULL; cJSON_InitHooks(NULL);
    return 0;
}

/** @brief 交付一条拥有的持久摘要候选以核对完整键匹配。
 * @param[in] self 未使用的后端上下文。
 * @param[in] query 稳定条件，复制其会话标识。
 * @param[out] out_results 新拥有结果数组槽，失败也交付可清理的部分项。
 * @param[out] out_count 已交付结果数，数组分配失败为零。
 * @return 分配失败为 OOM，其余为 CC_OK，字段由核心包装或调用者清理。
 */
static cc_result_t query_summary(void *self, const cc_memory_query_t *query,
    cc_memory_search_result_t **out_results, size_t *out_count)
{
    (void)self;
    *out_count = 0;
    *out_results = calloc(1, sizeof(**out_results));
    if (!*out_results) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "候选数组分配失败");
    *out_count = 1;
    cc_memory_entry_t *entry = &(*out_results)[0].entry;
    entry->key = cc_copy_string(state.persistent_key);
    entry->value = cc_copy_string("s");
    entry->category = cc_copy_string("context_summary");
    entry->session_id = cc_copy_string(query->session_id);
    if (!entry->key || !entry->value || !entry->category || !entry->session_id)
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "候选字段分配失败");
    return cc_result_ok();
}

/** @brief 验证持久前驱只接受与当前历史完全对应的规范键。
 * @param[in,out] runtime 独占实例，临时借用的记忆端口在函数内恢复。
 * @return 所有断言通过返回 0，否则返回 1，正常路径释放候选和历史。
 */
static int test_persistent_key(cc_agent_runtime_t *runtime)
{
    static const cc_memory_store_vtable_t memory_vtable = {.query = query_summary};
    cc_memory_store_t memory = {.vtable = &memory_vtable};
    cc_memory_store_t *saved_store = runtime->memory_store;
    int saved_available = runtime->summary_cache_namespace_available;
    runtime->memory_store = &memory;
    runtime->summary_cache_namespace_available = 1;
    cc_message_t *history = NULL; size_t count = 0;
    CHECK(ok(load_history(NULL, "persistent", 6, &history, &count)));
    uint64_t fingerprint = 0;
    CHECK(history_fingerprint(history, 3, &fingerprint));
    char canonical[160], malformed[192];
    CHECK(summary_persistent_key(runtime, "persistent", fingerprint, 3, canonical, sizeof(canonical)));
    int suffix[7] = {600, 500, 400, 300, 200, 100, 0};
    for (int mode = 0; mode < 3; mode++) {
        snprintf(malformed, sizeof(malformed), "%s%s", canonical,
                 mode == 1 ? "junk" : mode == 2 ? "18446744073709551616" : "");
        state.persistent_key = malformed;
        size_t summarized = 42;
        char *text = summary_persistent_predecessor(runtime, "persistent", history, 6, suffix, &summarized);
        CHECK((text != NULL) == (mode == 0));
        CHECK(summarized == (mode == 0 ? 3U : 0U));
        free(text);
    }
    free_loaded_messages(history, count);
    runtime->memory_store = saved_store;
    runtime->summary_cache_namespace_available = saved_available;
    state.persistent_key = NULL;
    CHECK(state.diagnostic_count == 0);
    return 0;
}

/** @brief 在无需真实巨大分配的边界状态验证向量扩容拒绝字节乘法溢出。
 * @return 返回限制错误且原向量字段保持不变为 0，否则返回 1。
 */
static int test_vector_limit(void)
{
    cc_message_t source = {0};
    message_vec_t vector = {0};
    vector.count = vector.capacity = SIZE_MAX / sizeof(*vector.items);
    cc_result_t rc = message_vec_append_copy(&vector, &source);
    CHECK(rc.code == CC_ERR_LIMIT_EXCEEDED && !vector.items && vector.count == vector.capacity);
    tracked_result_free(&rc);
    return 0;
}

/** @brief 执行真实生产函数的故障、预算及结果拥有权回归。
 * @return 全部断言通过返回 0，否则返回 1。正常路径销毁运行时和注册表。
 */
int main(void)
{
    cc_tool_registry_t *registry = NULL; cc_agent_runtime_t *runtime = NULL;
    CHECK(ok(cc_tool_registry_create(&registry)) && ok(cc_tool_registry_freeze(registry)));
    state.capabilities_supported = 1;
    CHECK(ok(create_runtime(registry, &runtime)));
    CHECK(test_required_failures(runtime) == 0);
    CHECK(test_threshold_and_success(runtime) == 0);
    CHECK(test_summary_fallbacks(runtime) == 0);
    CHECK(test_observation_oom(runtime) == 0);
    CHECK(test_persistent_key(runtime) == 0);
    CHECK(test_vector_limit() == 0);
    cc_agent_runtime_destroy(runtime); runtime = NULL;
    state.capabilities_supported = 0;
    cc_result_t rc = create_runtime(registry, &runtime);
    CHECK(rc.code == CC_ERR_UNSUPPORTED && !runtime);
    tracked_result_free(&rc);
    CHECK(state.diagnostic_count == 0);
    cc_tool_registry_destroy(registry);
    return 0;
}
