



#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_memory_store.h"
#include "cc/core/cc_tool_call.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/internal/cc_json_fields.h"
#include "cc/internal/cc_json_syntax_check.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>

/** @brief 记忆工具的固定策略和端口借用，实际存储同步由后端负责。 */
typedef struct {
    cc_memory_store_t *store; /**< owner 保持描述符及后端覆盖工具全部调用期。 */
    int read_only; /**< 非零仅允许 recall/list，创建后保持固定。 */
} cc_memory_tool_t;

/** @brief 提供固定注册名。 @param[in] self 忽略，可为 NULL。 @return 静态名称借用。 */
static const char *memory_name(void *self) { (void)self; return "memory"; }


/** @brief 按固定只读策略提供模型说明。 @param[in] self 可选状态借用，NULL 使用读写说明。 @return 静态字符串借用。 */
static const char *memory_description(void *self)
{
    cc_memory_tool_t *tool = self;
    if (tool && tool->read_only)
        return "Search long-term memory. Operations: recall(query), list(). Memory changes are managed separately.";
    return "Long-term memory storage. Use to remember important facts across sessions. "
           "Operations: store(key, value, category), recall(query), forget(key), list()";
}

/** @brief 提供当前操作集合的参数 schema。 @param[in] self 可选状态借用，NULL 使用读写 schema。 @return 静态完整 JSON 对象文本借用，具体字段组合在 call 中核对。 */
static const char *memory_schema_json(void *self)
{
    cc_memory_tool_t *tool = self;
    if (tool && tool->read_only)
        return "{"
            "\"type\":\"object\","
            "\"properties\":{"
                "\"operation\":{\"type\":\"string\",\"enum\":[\"recall\",\"list\"]},"
                "\"category\":{\"type\":\"string\",\"description\":\"Category for list\"},"
                "\"query\":{\"type\":\"string\",\"description\":\"Search query for recall\"},"
                "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":20}"
            "},"
            "\"required\":[\"operation\"]"
        "}";
    return "{"
        "\"type\":\"object\","
        "\"properties\":{"
            "\"operation\":{\"type\":\"string\",\"enum\":[\"store\",\"recall\",\"forget\",\"list\"],"
                "\"description\":\"store=save fact, recall=search, forget=delete, list=show all\"},"
            "\"key\":{\"type\":\"string\",\"description\":\"Memory key (for store/forget)\"},"
            "\"value\":{\"type\":\"string\",\"description\":\"Memory value (for store)\"},"
            "\"category\":{\"type\":\"string\",\"description\":\"Category: user_pref, project_info, lesson, rule\"},"
            "\"query\":{\"type\":\"string\",\"description\":\"Search query (for recall); narrow it if results are truncated\"},"
            "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":20,\"description\":\"Maximum entries for recall/list (default 10); output is bounded excerpts\"}"
        "},"
        "\"required\":[\"operation\"]"
    "}";
}

/**
 * @brief 追加有限字节前缀，截断时退回 UTF-8 非续字节边界。
 * @param[in,out] sb 独占的已初始化构造器，追加失败记入其 sticky 状态。
 * @param[in] text 借用 NUL 结尾正文，NULL 直接返回。
 * @param[in] bytes 最大前缀字节数，内部调用上限为 8192，须可由 int 表示。
 * @param[in,out] truncated 必需标记，实际截断时置一，原有标记保留。
 * @post 追加结果诊断被消费，输入为有效 UTF-8 时前缀保持字符边界，函数本身不验证整个编码。
 */
static void memory_append_excerpt(cc_string_builder_t *sb, const char *text,
                                  size_t bytes, int *truncated)
{
    if (!text) return;
    size_t n = 0;
    while (n < bytes && text[n]) n++;
    if (text[n]) {
        *truncated = 1;
        while (n > 0 && ((unsigned char)text[n] & 0xc0) == 0x80) n--;
    }
    cc_result_t rc = cc_string_builder_appendf(sb, "%.*s", (int)n, text);
    cc_result_free(&rc);
}

/**
 * @brief 将有限条记忆格式化为预算内的键与正文摘要。
 * @param[in,out] sb 独占的空构造器，追加失败通过 sticky 状态传给 take。
 * @param[in] entries 借用 count 项有效条目，字符串字段允许 NULL。
 * @param[in] count 条目数，后端结果须与实际数组容量一致。
 * @param[in] budget 最大输出字节数，内部取 1 至 8192，含截断提示，不含 NUL。
 * @post 所有追加结果诊断已归还，字段及条目截断时附提示，分配失败保持可清理构造器。
 */
static void memory_append_results(cc_string_builder_t *sb,
                                  const cc_memory_entry_t *entries, size_t count,
                                  size_t budget)
{
    const char *notice = "\n[Truncated excerpts; narrow query or lower limit for details.]\n";
    size_t reserve = strlen(notice);
    int truncated = 0;
    if (budget <= reserve + 64) {
        memory_append_excerpt(sb, "[Truncated]", budget, &truncated);
        return;
    }
    cc_result_t rc = cc_string_builder_appendf(sb, "Retrieved %zu memories (bounded results):\n", count);
    cc_result_free(&rc);
    size_t end = budget - reserve;
    for (size_t i = 0; i < count; i++) {
        if (sb->length + 16 >= end) { truncated = 1; break; }
        rc = cc_string_builder_append(sb, "- ");
        cc_result_free(&rc);
        size_t available = end - sb->length - 4;
        memory_append_excerpt(sb, entries[i].key, available < 256 ? available : 256, &truncated);
        rc = cc_string_builder_append(sb, ": ");
        cc_result_free(&rc);
        available = end - sb->length - 1;
        size_t per_value = budget / (count ? count : 1);
        memory_append_excerpt(sb, entries[i].value, available < per_value ? available : per_value, &truncated);
        rc = cc_string_builder_append(sb, "\n");
        cc_result_free(&rc);
    }
    if (truncated) {
        rc = cc_string_builder_append(sb, notice);
        cc_result_free(&rc);
    }
}

/**
 * @brief 交付可恢复业务错误，并显式报告错误副本 OOM。
 * @param[out] out_result 借用已清空结果，其 error 字段须为空。
 * @param[in] message 借用非 NULL 错误文本。
 * @return 副本成功为 CC_OK，分配失败为 OUT_OF_MEMORY，调用方消费返回诊断和结果字段。
 */
static cc_result_t memory_argument_error(cc_tool_result_t *out_result, const char *message)
{
    out_result->error = cc_copy_string(message);
    return out_result->error ? cc_result_ok() :
        cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate memory tool error");
}

/**
 * @brief 校验完整对象参数后执行固定策略允许的同步记忆操作。
 * @param[in] self 必需存活工具借用，借用 store 及其后端覆盖整个调用。
 * @param[in] args_json 借用 NUL 结尾对象，已知字段不得重复，字符串不得含解码 NUL。
 * @param[in] ctx 可选上下文借用，session_id 用于写入来源，非零结果预算收紧 recall/list 的 8192 字节上限。
 * @param[out] out_result 必需新建或已 cleanup 槽，入口清零，拥有正文或错误由调用方 cleanup。
 * @return 参数地址错误、构造失败或后端错误返回非 OK，业务参数/只读拒绝以 CC_OK 和 ok=0 返回，诊断由调用方释放。
 * @note 后端错误原样交付，写入可能已生效，确认文本分配失败也保留非 OK 供操作账本核对。
 * 只读工具仅允许 recall/list，具体受管理记录可见性由注入 store 策略决定，同步及销毁排空由 owner 协调。
 */
static cc_result_t memory_call(void *self, const char *args_json,
                                const cc_tool_context_t *ctx,
                                cc_tool_result_t *out_result)
{
    if (!out_result) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null memory tool result");
    memset(out_result, 0, sizeof(*out_result));
    if (!self) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null memory tool");
    cc_memory_tool_t *tool = (cc_memory_tool_t *)self;

    cc_json_value_t *params = NULL;
    if (!args_json || cc_storage_json_has_nul_escape(args_json))
        return memory_argument_error(out_result, "Arguments must not contain decoded NUL");
    cc_result_t rc = cc_json_parse_strict(args_json, &params);
    if (rc.code != CC_OK || !params) {
        cc_result_free(&rc);
        return memory_argument_error(out_result, "Failed to parse arguments JSON");
    }
    cc_result_free(&rc);
    const char *fields[] = {"operation", "key", "value", "category", "query", "limit"};
    if (!cc_json_is_object(params) || cc_json_duplicate_known_field(params, fields, 6)) {
        cc_json_destroy(params);
        return memory_argument_error(out_result, "Arguments must be an object with unique known fields");
    }
    for (size_t i = 0; i < 5; i++) {
        cc_json_value_t *field = cc_json_object_get(params, fields[i]);
        if (field && !cc_json_is_null(field) && !cc_json_is_string(field)) {
            cc_json_destroy(params);
            return memory_argument_error(out_result, "Memory string fields must be strings or null");
        }
    }

    const char *op = cc_json_string_value(cc_json_object_get(params, "operation"));
    int limit = 10;
    cc_json_value_t *limit_value = cc_json_object_get(params, "limit");
    if (limit_value) {
        double requested = cc_json_number_value(limit_value);
        if (!cc_json_is_number(limit_value) || !(requested >= 1 && requested <= 20) ||
            requested != (double)(int)requested) {
            cc_json_destroy(params);
            return memory_argument_error(out_result, "limit must be an integer between 1 and 20");
        }
        limit = (int)requested;
    }
    size_t budget = 8192;
    if (ctx && ctx->max_result_bytes && ctx->max_result_bytes < budget)
        budget = ctx->max_result_bytes;

    if (!op) {
        cc_json_destroy(params);
        return memory_argument_error(out_result, "Missing required string field: operation");
    }
    if (tool->read_only && strcmp(op,"recall") != 0 && strcmp(op,"list") != 0) {
        cc_json_destroy(params);
        return memory_argument_error(out_result, "Memory is read-only; use recall or list");
    }

    cc_string_builder_t sb;
    rc = cc_string_builder_init(&sb);
    if (rc.code != CC_OK) {
        cc_json_destroy(params);
        return rc;
    }
    cc_result_free(&rc);

    if (strcmp(op, "store") == 0) {
        const char *key = cc_json_string_value(cc_json_object_get(params, "key"));
        const char *value = cc_json_string_value(cc_json_object_get(params, "value"));
        const char *category = cc_json_string_value(cc_json_object_get(params, "category"));

        if (!key || !value) {
            cc_string_builder_deinit(&sb);
            cc_json_destroy(params);
            return memory_argument_error(out_result, "'key' and 'value' are required for store");
        }

        if (strlen(key) > 256 || strlen(value) > 4096) {
            cc_string_builder_deinit(&sb);
            cc_json_destroy(params);
            return memory_argument_error(out_result, "key max 256 bytes, value max 4096 bytes");
        }

        cc_result_t set_rc = cc_memory_store_set(tool->store, key, value, category,
                                                   ctx ? ctx->session_id : NULL);
        if (set_rc.code != CC_OK) {
            cc_string_builder_deinit(&sb);
            cc_json_destroy(params);
            return set_rc;
        }
        cc_result_free(&set_rc);
        rc = cc_string_builder_appendf(&sb, "Memory stored: %s = %s", key, value);
        cc_result_free(&rc);

    } else if (strcmp(op, "recall") == 0) {
        const char *query = cc_json_string_value(cc_json_object_get(params, "query"));
        if (!query) query = "";

        cc_memory_entry_t *entries = NULL;
        size_t count = 0;
        cc_result_t search_rc = cc_memory_store_search(tool->store, query, limit, &entries, &count);
        if (search_rc.code != CC_OK) {
            cc_memory_entry_free_array(entries, count);
            cc_string_builder_deinit(&sb);
            cc_json_destroy(params);
            return search_rc;
        }
        if (count == 0) {
            int truncated = 0;
            memory_append_excerpt(&sb, "No matching memories found.", budget, &truncated);
        } else {
            memory_append_results(&sb, entries, count, budget);
        }
        cc_memory_entry_free_array(entries, count);
        cc_result_free(&search_rc);

    } else if (strcmp(op, "forget") == 0) {
        const char *key = cc_json_string_value(cc_json_object_get(params, "key"));
        if (!key) {
            cc_string_builder_deinit(&sb);
            cc_json_destroy(params);
            return memory_argument_error(out_result, "'key' is required for forget");
        }

        cc_result_t del_rc = cc_memory_store_delete(tool->store, key);
        if (del_rc.code != CC_OK) {
            cc_string_builder_deinit(&sb);
            cc_json_destroy(params);
            return del_rc;
        }
        cc_result_free(&del_rc);
        rc = cc_string_builder_appendf(&sb, "Memory forgotten: %s", key);
        cc_result_free(&rc);

    } else if (strcmp(op, "list") == 0) {
        const char *category = cc_json_string_value(cc_json_object_get(params, "category"));

        cc_memory_entry_t *entries = NULL;
        size_t count = 0;
        cc_result_t list_rc = cc_memory_store_list(tool->store, category, limit, &entries, &count);
        if (list_rc.code != CC_OK) {
            cc_memory_entry_free_array(entries, count);
            cc_string_builder_deinit(&sb);
            cc_json_destroy(params);
            return list_rc;
        }
        if (count == 0) {
            int truncated = 0;
            memory_append_excerpt(&sb, "No memories found.", budget, &truncated);
        } else {
            memory_append_results(&sb, entries, count, budget);
        }
        cc_memory_entry_free_array(entries, count);
        cc_result_free(&list_rc);

    } else {
        cc_string_builder_deinit(&sb);
        cc_json_destroy(params);
        return memory_argument_error(out_result, "Unknown operation; use store/recall/forget/list");
    }

    cc_json_destroy(params);
    out_result->ok = 1;
    cc_error_code_t build_error = sb.error_code;
    out_result->text = cc_string_builder_take(&sb);
    if (!out_result->text) {
        out_result->ok = 0;
        return cc_result_error(build_error != CC_OK ? build_error : CC_ERR_OUT_OF_MEMORY,
                               "Failed to build memory tool result");
    }
    return cc_result_ok();
}

/** @brief 消费工具私有状态。 @param[in] self 交接独占状态，NULL 安全。 @pre owner 已排空调用，store 借用已结束。 @post 工具状态释放，store 的拥有责任继续归其创建方。 */
static void memory_destroy(void *self)
{
    cc_memory_tool_t *tool = (cc_memory_tool_t *)self;
    if (!tool) return;
    free(tool);
}

/**
 * @brief 创建借用存储描述符的固定策略工具。
 * @param[in] store 必需借用描述符，owner 维持其位置和后端覆盖工具寿命，方法能力在调用时检查。
 * @param[in] read_only 非零选择 recall/list，零选择读写操作。
 * @param[out] out_tool 必需新描述符，入口清零，成功交付拥有 self 和静态方法表。
 * @return 参数错误或 OOM，失败输出为空，成功由创建方销毁或成功交接 registry。
 */
static cc_result_t memory_tool_create(cc_memory_store_t *store, int read_only, cc_tool_t *out_tool)
{
    if (!out_tool) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null memory tool output");
    }
    memset(out_tool, 0, sizeof(*out_tool));
    if (!store) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null memory store for memory tool");
    }
    cc_memory_tool_t *tool = calloc(1, sizeof(cc_memory_tool_t));
    if (!tool) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate memory tool");

    tool->store = store;
    tool->read_only = read_only;

    static const cc_tool_vtable_t vtable = {
        .name = memory_name,
        .description = memory_description,
        .schema_json = memory_schema_json,
        .call = memory_call,
        .destroy = memory_destroy,
    };

    out_tool->self = tool;
    out_tool->vtable = &vtable;
    return cc_result_ok();
}

/** @brief 创建读写记忆工具。 @param[in] store 覆盖工具寿命的存储借用。 @param[out] out_tool 必需空槽，成功对象由调用方销毁或交接。 @return 参数、分配或成功拥有结果，失败空输出。 */
cc_result_t cc_memory_tool_create(cc_memory_store_t *store, cc_tool_t *out_tool)
{
    return memory_tool_create(store, 0, out_tool);
}

/** @brief 创建仅可 recall/list 的记忆工具。 @param[in] store 覆盖工具寿命的存储借用。 @param[out] out_tool 必需空槽，成功对象由调用方销毁或交接。 @return 参数、分配或成功拥有结果，失败空输出，store 可见性策略仍由后端决定。 */
cc_result_t cc_memory_tool_create_readonly(cc_memory_store_t *store, cc_tool_t *out_tool)
{
    return memory_tool_create(store, 1, out_tool);
}
