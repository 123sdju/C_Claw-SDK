#include "cc/app/cc_memory_extractor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** 断言失败立即报告行号并返回一，便于保管当时的测试现场。 */
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "check failed at line %d\n", __LINE__); return 1; } } while (0)
/**
 * @brief 检查结果状态并释放按值传入结果拥有的错误消息。
 * @param[in] rc 要消费的结果，调用后消息资源已经释放。
 * @return CC_OK 为一，其他状态为零。
 */
static int ok(cc_result_t rc) { int good = rc.code == CC_OK; cc_result_free(&rc); return good; }
/** 同步模型夹具，正文由测试借出，其他字段记录实际调用。 */
typedef struct fake_state { const char *response; int calls; int valid_request; } fake_state_t;

/**
 * @brief 校验独立提取请求的角色、工具和输出预算，再返回预设正文及用量。
 * @param[in,out] self 借用 fake_state_t，递增 calls 并更新 valid_request。
 * @param[in] request 实际同步请求，调用期间借用。
 * @param[in,out] out 调用者初始化的响应，填入用量并接管复制后的文本。
 * @return 文本复制的结果，由调用者清理返回消息及响应拥有资源。
 */
static cc_result_t fake_chat(void *self, const cc_llm_chat_request_t *request, cc_llm_response_t *out)
{
    fake_state_t *state = self;
    state->calls++;
    state->valid_request = request && request->message_count == 2 && !request->tools_json &&
        !request->stream && request->messages[0].role == CC_ROLE_SYSTEM &&
        request->messages[1].role == CC_ROLE_USER && request->max_tokens == 777;
    out->usage.valid = 1;
    out->usage.prompt_tokens = 23;
    return cc_llm_response_set_text(out, state->response);
}
static const cc_llm_provider_vtable_t fake_vtable = {fake_chat, NULL, NULL, NULL};

/**
 * @brief 验证提取、来源核对、决策规划和非法 Unicode 响应的失败输出。
 * @return 全部断言通过为零，首次失败报告位置并返回一。
 * @note 成功路径清理候选、决策和计划，CHECK 早退时保留当时尚未释放资源供诊断。
 */
int main(void)
{
    fake_state_t state = {0};
    cc_llm_provider_t provider = {&state, &fake_vtable};
    cc_memory_extraction_options_t options = {0};
    options.size = sizeof(options);
    options.model = "model";
    options.max_output_tokens = 777;
    const char *user = "我喜欢蓝色，并正在开发 CClaw。";
    state.response = "{\"memories\":["
        "{\"text\":\"喜欢蓝色\",\"category\":\"preference\",\"importance\":70,\"evidence\":\"喜欢蓝色\"},"
        "{\"text\":\"正在开发 CClaw\",\"category\":\"project\",\"importance\":90,\"evidence\":\"正在开发 CClaw\"}]}";
    cc_memory_candidate_t *candidates = NULL;
    size_t count = 0;
    cc_llm_usage_t usage = {0};
    CHECK(ok(cc_memory_extract_candidates(&provider, &options, user, "了解。", &candidates, &count, &usage)));
    CHECK(state.calls == 1 && state.valid_request && count == 2 && usage.valid && usage.prompt_tokens == 23);
    CHECK(strcmp(candidates[0].category, "preference") == 0 && candidates[0].importance == 70);
    CHECK(strcmp(candidates[1].evidence, "正在开发 CClaw") == 0);
    cc_memory_candidates_free(candidates, count);

    state.response = "{\"memories\":[{\"text\":\"invented\",\"category\":\"fact\","
                     "\"importance\":80,\"evidence\":\"not in user text\"}]}";
    candidates = NULL; count = 0;
    cc_result_t rc = cc_memory_extract_candidates(&provider, &options, user, "response", &candidates, &count, NULL);
    CHECK(rc.code == CC_ERR_MODEL && !candidates && count == 0);
    cc_result_free(&rc);
    state.response = "{\"memories\":[{\"text\":\"x\",\"category\":\"fact\",\"importance\":101,\"evidence\":\"喜欢\"}]}";
    rc = cc_memory_extract_candidates(&provider, &options, user, "response", &candidates, &count, NULL);
    CHECK(rc.code == CC_ERR_MODEL && !candidates && count == 0);
    cc_result_free(&rc);
    state.response = "{\"memories\":[]}";
    CHECK(ok(cc_memory_extract_candidates(&provider, &options, user, "response", &candidates, &count, NULL)));
    CHECK(!candidates && count == 0 && state.calls == 4);
    state.response = "{\"memories\":[{\"text\":\"api_key=EXAMPLEONLY\",\"category\":\"fact\","
                     "\"importance\":80,\"evidence\":\"api_key=EXAMPLEONLY\"}]}";
    rc = cc_memory_extract_candidates(&provider, &options, "api_key=EXAMPLEONLY", "response", &candidates, &count, NULL);
    CHECK(rc.code == CC_ERR_MODEL && !candidates && count == 0);
    cc_result_free(&rc);

    cc_memory_plan_record_t previous = {0};
    previous.size = sizeof(previous); previous.id = "color"; previous.text = "喜欢蓝色";
    previous.category = "preference"; previous.importance = 70;
    state.response = "{\"memories\":["
        "{\"action\":\"update\",\"target_id\":\"color\",\"text\":\"喜欢绿色\","
        "\"category\":\"preference\",\"importance\":80,\"evidence\":\"喜欢绿色\"},"
        "{\"action\":\"add\",\"target_id\":\"\",\"text\":\"开发 CClaw\","
        "\"category\":\"project\",\"importance\":90,\"evidence\":\"开发 CClaw\"}]}";
    cc_memory_decision_t *decisions = NULL;
    CHECK(ok(cc_memory_extract_decisions(&provider, &options, "喜欢绿色，开发 CClaw", "好的",
                                          &previous, 1, &decisions, &count, &usage)));
    CHECK(count == 2 && decisions[0].kind == CC_MEMORY_DECISION_UPDATE &&
          strcmp(decisions[0].target_id, "color") == 0 && decisions[1].kind == CC_MEMORY_DECISION_ADD);
    cc_memory_plan_t plan = {0};
    CHECK(ok(cc_memory_plan_from_decisions(&previous, 1, decisions, count, "喜欢绿色，开发 CClaw",
                                            "session-1", 123, 4, NULL, NULL, &plan)));
    CHECK(plan.count == 2 && plan.changed_existing == 1 && plan.added == 1 &&
          strcmp(plan.records[0].text, "喜欢绿色") == 0 && plan.records[1].importance == 90);
    cc_memory_plan_cleanup(&plan);
    cc_memory_decisions_free(decisions, count);
    state.response = "{\"memories\":[{\"action\":\"update\",\"target_id\":\"missing\","
        "\"text\":\"喜欢绿色\",\"category\":\"preference\",\"importance\":80,\"evidence\":\"喜欢绿色\"}]}";
    decisions = NULL; count = 0;
    rc = cc_memory_extract_decisions(&provider, &options, "喜欢绿色", "好的", &previous, 1, &decisions, &count, NULL);
    CHECK(rc.code == CC_ERR_MODEL && !decisions && count == 0); cc_result_free(&rc);

    /* 非法转义曾可能把正文或字段名截成合法 C 字符串，候选与决策均须拒绝整批。 */
    const char *invalid_responses[] = {
        "{\"memories\":[{\"action\":\"add\",\"text\":\"fact\\uZZZZtail\","
        "\"category\":\"fact\",\"importance\":70,\"evidence\":\"喜欢\"}]}",
        "{\"memories\":[{\"action\":\"add\",\"text\":\"fact\",\"category\":\"fact\","
        "\"importance\":70,\"evidence\\uZZZZtail\":\"喜欢\"}]}",
        "{\"memories\":[{\"action\":\"add\",\"text\":\"fact\",\"category\":\"fact\","
        "\"importance\":70,\"evidence\":\"喜欢\",\"extra\":{\"value\":\"\\u12Q4\"}}]}",
        "{\"memories\":[{\"action\":\"add\",\"text\":\"fact\\u0000tail\","
        "\"category\":\"fact\",\"importance\":70,\"evidence\":\"喜欢\"}]}",
        "{\"memories\":[{\"action\":\"add\",\"text\":\"fact\",\"category\":\"fact\","
        "\"importance\":70,\"evidence\\u0000tail\":\"喜欢\"}]}"
    };
    for (size_t i = 0; i < sizeof(invalid_responses) / sizeof(invalid_responses[0]); i++) {
        state.response = invalid_responses[i];
        candidates = NULL;
        count = 0;
        rc = cc_memory_extract_candidates(&provider, &options, user, "response",
                                           &candidates, &count, &usage);
        CHECK(rc.code == CC_ERR_MODEL && !candidates && count == 0 && usage.valid);
        cc_result_free(&rc);
        decisions = NULL;
        rc = cc_memory_extract_decisions(&provider, &options, user, "response",
                                         &previous, 1, &decisions, &count, &usage);
        CHECK(rc.code == CC_ERR_MODEL && !decisions && count == 0 && usage.valid);
        cc_result_free(&rc);
    }

    return 0;
}
