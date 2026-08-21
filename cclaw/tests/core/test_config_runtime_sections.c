

#include "cc/util/cc_config.h"

#include <stdio.h>
#include <string.h>

/* 空安全字符串相等 helper，减少断言处重复判空。 */
static int streq(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

/* 写入临时非法配置并断言 cc_config_load 返回 invalid argument 且 message 命中关键字。 */
static int load_invalid_config_fails(const char *json, const char *needle)
{
    const char *path = "__c_claw_invalid_config_test__.json";
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fputs(json, f);
    fclose(f);

    cc_config_t config;
    cc_result_t rc = cc_config_load(path, &config);
    remove(path);
    int ok = rc.code == CC_ERR_INVALID_ARGUMENT &&
        rc.message &&
        strstr(rc.message, needle) != NULL;
    cc_result_free(&rc);
    cc_config_destroy(&config);
    return ok;
}

/* 写入临时配置并断言返回指定错误码和错误信息关键字。 */
static int load_config_fails(const char *json, cc_error_code_t code, const char *needle)
{
    const char *path = "__c_claw_unsupported_config_test__.json";
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fputs(json, f);
    fclose(f);

    cc_config_t config;
    cc_result_t rc = cc_config_load(path, &config);
    remove(path);
    int ok = rc.code == code &&
        rc.message &&
        strstr(rc.message, needle) != NULL;
    cc_result_free(&rc);
    cc_config_destroy(&config);
    return ok;
}

/* 写入临时合法配置并加载到调用方提供的 config，成功后由调用方 destroy。 */
static int load_config_ok(const char *json, cc_config_t *config)
{
    const char *path = "__c_claw_supported_config_test__.json";
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fputs(json, f);
    fclose(f);

    cc_result_t rc = cc_config_load(path, config);
    remove(path);
    int ok = rc.code == CC_OK;
    cc_result_free(&rc);
    return ok;
}

/*
 * 验证 runtime 相关配置 section。
 *
 * 覆盖 queue/agents/tools/plugins/skills/MCP/active memory/multimodal 的解析、默认裁剪宏下
 * 的 unsupported 行为，以及几个关键非法配置的校验错误。
 */
int main(void)
{
    const char *path = "__c_claw_runtime_sections_test__.json";
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    fputs(
        "{"
        "\"queue\":{\"lanes\":{\"main\":2,\"subagent\":3,\"plugin\":4,\"mcp\":5},"
        "\"perSessionConcurrency\":1,\"mode\":\"collect\",\"debounceMs\":125,"
        "\"maxPendingPerSession\":7},"
        "\"agents\":{\"defaults\":{\"model\":\"base-model\",\"workspace\":\"./ws\","
        "\"agentDir\":\".agents/default\",\"systemPromptFile\":\"system.md\","
        "\"skills\":[\"core\"]},\"list\":[{\"id\":\"reviewer\",\"model\":\"review-model\","
        "\"skills\":[\"code-review\"]}]},"
        "\"tools\":{\"enabled\":[\"file_read\",\"plugin.echo\"],\"defaultTimeoutMs\":1234,"
        "\"networkAllowlist\":[\"api.example.com\",\"*.trusted.local\"],"
        "\"perTool\":{\"plugin.echo\":{\"concurrency\":2,\"timeoutMs\":4567}}},"
        "\"plugins\":{\"hotReload\":true,\"reloadDebounceMs\":88,\"entries\":{"
        "\"echo\":{\"command\":\"python3\",\"args\":[\"plugin.py\"],\"workers\":2,"
        "\"timeoutMs\":9000,\"maxInFlight\":3,\"restartOnCrash\":false,"
        "\"skills\":[\"plugins/echo/skills\"],\"tools\":[{\"name\":\"plugin.echo\","
        "\"description\":\"Echo input\",\"parameters\":{\"type\":\"object\"}}]}}},"
        "\"skills\":{\"load\":{\"watch\":true,\"watchDebounceMs\":77,"
        "\"extraDirs\":[\".agents/skills\",\"~/.cclaw/skills\"]}},"
        "\"mcp\":{\"enabled\":true,\"sessionIdleTtlMs\":60000,\"servers\":{"
        "\"fs\":{\"transport\":\"stdio\",\"command\":\"node\",\"args\":[\"server.js\"],"
        "\"cwd\":\"./mcp\",\"connectionTimeoutMs\":3210}}},"
        "\"memory\":{\"active\":{\"enabled\":true,\"writeSummary\":true,"
        "\"maxValueChars\":256,\"category\":\"turn_summary\"}}"
        "}", f);
    fclose(f);

    cc_config_t config;
    cc_result_t rc = cc_config_load(path, &config);
    remove(path);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return 1;
    }
    cc_result_free(&rc);

    int ok = 1;
    ok = ok && config.queue.main_concurrency == 2;
    ok = ok && config.queue.subagent_concurrency == 3;
    ok = ok && config.queue.plugin_concurrency == 4;
    ok = ok && config.queue.mcp_concurrency == 5;
    ok = ok && streq(config.queue.mode, "collect");
    ok = ok && config.queue.debounce_ms == 125;
    ok = ok && config.queue.max_pending_per_session == 7;

    ok = ok && streq(config.agents.defaults.model, "base-model");
    ok = ok && streq(config.agents.defaults.agent_dir, ".agents/default");
    ok = ok && config.agents.defaults.skills.count == 1;
    ok = ok && config.agents.profile_count == 1;
    ok = ok && streq(config.agents.profiles[0].id, "reviewer");

    ok = ok && config.tools.enabled.count == 2;
    ok = ok && config.enabled_tools_count == 2;
    ok = ok && config.tools.default_timeout_ms == 1234;
    ok = ok && config.tools.network_allowlist.count == 2;
    ok = ok && streq(config.tools.network_allowlist.items[0], "api.example.com");
    ok = ok && config.tools.policy_count == 1;
    ok = ok && streq(config.tools.policies[0].name, "plugin.echo");
    ok = ok && config.tools.policies[0].concurrency == 2;
    ok = ok && config.tools.policies[0].timeout_ms == 4567;

    ok = ok && config.plugins.hot_reload == 1;
    ok = ok && config.plugins.reload_debounce_ms == 88;
    ok = ok && config.plugins.entry_count == 1;
    ok = ok && streq(config.plugins.entries[0].id, "echo");
    ok = ok && config.plugins.entries[0].workers == 2;
    ok = ok && config.plugins.entries[0].restart_on_crash == 0;
    ok = ok && config.plugins.entries[0].tool_count == 1;
    ok = ok && streq(config.plugins.entries[0].tools[0].name, "plugin.echo");

    ok = ok && config.skills.watch == 1;
    ok = ok && config.skills.watch_debounce_ms == 77;
    ok = ok && config.skills.extra_dirs.count == 2;

    ok = ok && config.mcp.enabled == 1;
    ok = ok && config.mcp.session_idle_ttl_ms == 60000;
    ok = ok && config.mcp.server_count == 1;
    ok = ok && streq(config.mcp.servers[0].name, "fs");
    ok = ok && streq(config.mcp.servers[0].transport, "stdio");
    ok = ok && config.mcp.servers[0].connection_timeout_ms == 3210;
    ok = ok && config.active_memory_enabled == 1;
    ok = ok && config.active_memory_write_summary == 1;
    ok = ok && config.active_memory_max_value_chars == 256;
    ok = ok && streq(config.active_memory_category, "turn_summary");
    ok = ok && config.multimodal.input.image == 0;
    ok = ok && config.multimodal.output.image == 0;

    cc_config_destroy(&config);

#if CC_ENABLE_MULTIMODAL && CC_ENABLE_MEDIA_IMAGE
    cc_config_t mm_config;
    if (load_config_ok(
        "{\"multimodal\":{\"input\":{\"image\":true},"
        "\"output\":{\"image\":false},\"limits\":{\"maxArtifacts\":4,"
        "\"maxArtifactBytes\":4096,\"allowedMimePrefixes\":[\"image/\"]}}}",
        &mm_config)) {
        ok = ok && mm_config.multimodal.input.image == 1;
        ok = ok && mm_config.multimodal.output.image == 0;
        ok = ok && mm_config.multimodal.limits.max_artifacts == 4;
        ok = ok && mm_config.multimodal.limits.allowed_mime_prefixes.count == 1;
        ok = ok && streq(mm_config.multimodal.limits.allowed_mime_prefixes.items[0], "image/");
        cc_config_destroy(&mm_config);
    } else {
        ok = 0;
    }
#else
    ok = ok && load_config_fails(
        "{\"multimodal\":{\"input\":{\"image\":true}}}",
        CC_ERR_UNSUPPORTED,
        "multimodal");
#endif

    ok = ok && load_invalid_config_fails(
        "{\"mcp\":{\"enabled\":true,\"servers\":{\"bad\":{\"transport\":\"streamable_http\"}}}}",
        "requires url");
    ok = ok && load_invalid_config_fails(
        "{\"mcp\":{\"enabled\":true,\"servers\":{\"bad\":{\"transport\":\"websocket\",\"url\":\"http://x\"}}}}",
        "unknown transport");
    ok = ok && load_invalid_config_fails(
        "{\"plugins\":{\"entries\":{\"bad\":{\"enabled\":true,\"workers\":0,\"command\":\"python3\"}}}}",
        "workers");
    return ok ? 0 : 1;
}
