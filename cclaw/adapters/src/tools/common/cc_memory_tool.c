



#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_memory_store.h"
#include "cc/core/cc_tool_call.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>

/*
 * memory 工具私有对象。
 *
 * store 是借用指针，不由工具销毁；这让 memory tool 可以共享 runtime/builder 注入的
 * memory store adapter。不同平台可以替换成内存、JSON 文件、SQLite 或向量库桥接实现。
 */
typedef struct {
    cc_memory_store_t *store;
} cc_memory_tool_t;

/* 返回工具注册名；一个工具名下用 operation 字段区分 store/recall/forget/list。 */
static const char *memory_name(void *self) { (void)self; return "memory"; }


/*
 * 返回工具说明。
 *
 * 说明里故意列出操作集合，帮助模型选择正确 operation；返回静态字符串，调用方不释放。
 */
static const char *memory_description(void *self)
{
    (void)self;
    return "Long-term memory storage. Use to remember important facts across sessions. "
           "Operations: store(key, value, category), recall(query), forget(key), list()";
}

/*
 * 返回 memory 工具 schema。
 *
 * schema 只要求 operation 必填，其它字段按 operation 在执行期二次校验。这样一个工具能
 * 复用同一 vtable 暴露多种 memory 操作，同时保持参数错误为可恢复 tool error。
 */
static const char *memory_schema_json(void *self)
{
    (void)self;
    return "{"
        "\"type\":\"object\","
        "\"properties\":{"
            "\"operation\":{\"type\":\"string\",\"enum\":[\"store\",\"recall\",\"forget\",\"list\"],"
                "\"description\":\"store=save fact, recall=search, forget=delete, list=show all\"},"
            "\"key\":{\"type\":\"string\",\"description\":\"Memory key (for store/forget)\"},"
            "\"value\":{\"type\":\"string\",\"description\":\"Memory value (for store)\"},"
            "\"category\":{\"type\":\"string\",\"description\":\"Category: user_pref, project_info, lesson, rule\"},"
            "\"query\":{\"type\":\"string\",\"description\":\"Search query (for recall)\"}"
        "},"
        "\"required\":[\"operation\"]"
    "}";
}

/*
 * 执行 memory 工具操作。
 *
 * 所有 store/search/list/delete 都通过 cc_memory_store_* 端口完成，工具本身不关心具体
 * 存储介质。业务失败写入 out_result，cc_result 仍返回 OK，让 agent 能把错误作为观察
 * 继续推理；真正的内存分配/非法参数错误才需要稳定错误码。
 */
static cc_result_t memory_call(void *self, const char *args_json,
                                const cc_tool_context_t *ctx,
                                cc_tool_result_t *out_result)
{
    cc_memory_tool_t *tool = (cc_memory_tool_t *)self;
    memset(out_result, 0, sizeof(*out_result));

    cc_json_value_t *params = NULL;
    cc_result_t rc = cc_json_parse(args_json, &params);
    if (rc.code != CC_OK || !params) {
        out_result->ok = 0;
        out_result->error = cc_copy_string("Failed to parse arguments JSON");
        cc_result_free(&rc);
        return cc_result_ok();
    }

    const char *op = cc_json_string_value(cc_json_object_get(params, "operation"));

    if (!op) {
        out_result->ok = 0;
        out_result->error = cc_copy_string("Missing required field: 'operation'");
        cc_json_destroy(params);
        return cc_result_ok();
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
            out_result->ok = 0;
            out_result->error = cc_copy_string("'key' and 'value' are required for store");
            cc_string_builder_deinit(&sb);
            cc_json_destroy(params);
            return cc_result_ok();
        }

        if (strlen(key) > 256 || strlen(value) > 4096) {
            out_result->ok = 0;
            out_result->error = cc_copy_string("key max 256 chars, value max 4096 chars");
            cc_string_builder_deinit(&sb);
            cc_json_destroy(params);
            return cc_result_ok();
        }

        cc_result_t set_rc = cc_memory_store_set(tool->store, key, value, category,
                                                   ctx->session_id);
        if (set_rc.code == CC_OK)
            cc_string_builder_appendf(&sb, "Memory stored: %s = %s", key, value);
        else
            cc_string_builder_appendf(&sb, "Failed to store memory: %s", set_rc.message);
        cc_result_free(&set_rc);

    } else if (strcmp(op, "recall") == 0) {
        const char *query = cc_json_string_value(cc_json_object_get(params, "query"));
        if (!query) query = "";

        cc_memory_entry_t *entries = NULL;
        size_t count = 0;
        cc_result_t search_rc = cc_memory_store_search(tool->store, query, 20, &entries, &count);
        if (search_rc.code != CC_OK || count == 0) {
            cc_string_builder_append(&sb, "No matching memories found.");
        } else {
            cc_string_builder_appendf(&sb, "Found %zu memories:\n", count);
            for (size_t i = 0; i < count; i++) {
                cc_string_builder_appendf(&sb, "- %s: %s", entries[i].key, entries[i].value);
                if (entries[i].category)
                    cc_string_builder_appendf(&sb, " [%s]", entries[i].category);
                cc_string_builder_append(&sb, "\n");
            }
        }
        cc_memory_entry_free_array(entries, count);
        cc_result_free(&search_rc);

    } else if (strcmp(op, "forget") == 0) {
        const char *key = cc_json_string_value(cc_json_object_get(params, "key"));
        if (!key) {
            out_result->ok = 0;
            out_result->error = cc_copy_string("'key' is required for forget");
            cc_string_builder_deinit(&sb);
            cc_json_destroy(params);
            return cc_result_ok();
        }

        cc_result_t del_rc = cc_memory_store_delete(tool->store, key);
        if (del_rc.code == CC_OK)
            cc_string_builder_appendf(&sb, "Memory forgotten: %s", key);
        else
            cc_string_builder_appendf(&sb, "No memory found with key: %s", key);
        cc_result_free(&del_rc);

    } else if (strcmp(op, "list") == 0) {
        const char *category = cc_json_string_value(cc_json_object_get(params, "category"));

        cc_memory_entry_t *entries = NULL;
        size_t count = 0;
        cc_result_t list_rc = cc_memory_store_list(tool->store, category, 50, &entries, &count);
        if (list_rc.code != CC_OK || count == 0) {
            cc_string_builder_append(&sb, "No memories found.");
        } else {
            cc_string_builder_appendf(&sb, "All memories (%zu total):\n", count);
            for (size_t i = 0; i < count; i++) {
                cc_string_builder_appendf(&sb, "- %s: %s", entries[i].key, entries[i].value);
                if (entries[i].category)
                    cc_string_builder_appendf(&sb, " [%s]", entries[i].category);
                cc_string_builder_append(&sb, "\n");
            }
        }
        cc_memory_entry_free_array(entries, count);
        cc_result_free(&list_rc);

    } else {
        cc_string_builder_appendf(&sb, "Unknown operation: %s. Use store/recall/forget/list", op);
    }

    cc_json_destroy(params);
    out_result->ok = 1;
    out_result->text = cc_string_builder_take(&sb);
    if (!out_result->text) {
        out_result->ok = 0;
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build memory tool result");
    }
    return cc_result_ok();
}

/* 销毁 memory 工具私有对象；store 是借用端口，不在这里释放。 */
static void memory_destroy(void *self)
{
    cc_memory_tool_t *tool = (cc_memory_tool_t *)self;
    if (!tool) return;
    free(tool);
}

/*
 * 创建 memory 工具。
 *
 * store 必须在工具生命周期内有效；成功后 out_tool 由 registry/runtime 持有。这里使用
 * static vtable，因为所有 memory 工具实例共享同一组函数，实现了 C 语言中的“类方法表”。
 */
cc_result_t cc_memory_tool_create(cc_memory_store_t *store, cc_tool_t *out_tool)
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
