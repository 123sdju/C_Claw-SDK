



#include "cc/app/cc_memory_context.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>

/*
 * 从 memory store 检索并构造 prompt 片段。
 *
 * 当前实现走简单 search top-10，并把结果格式化成 [Memory] block。检索失败或无结果不视为
 * runtime 失败，而是返回空 memory block，避免 memory 后端故障阻塞主对话。
 */
cc_result_t cc_memory_context_inject(
    cc_memory_store_t *store,
    const char *session_text,
    char **out_memory_block
)
{


    if (!store || !store->vtable || !out_memory_block) {
        if (out_memory_block) *out_memory_block = NULL;
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid memory context arguments");
    }

    cc_memory_entry_t *entries = NULL;
    size_t count = 0;



    cc_result_t rc = cc_memory_store_search(store,
        session_text ? session_text : "", 32, &entries, &count);



    if (rc.code != CC_OK || count == 0) {
        cc_result_free(&rc);
        *out_memory_block = NULL;
        return cc_result_ok();
    }
    cc_result_free(&rc);



    cc_string_builder_t sb;
    rc = cc_string_builder_init(&sb);
    if (rc.code != CC_OK) {
        cc_memory_entry_free_array(entries, count);
        return rc;
    }
    cc_result_free(&rc);



    cc_string_builder_append(&sb, "[Memory] The following are persistent facts from previous sessions:");
    cc_string_builder_append(&sb, "\n");

    size_t emitted = 0;
    for (size_t i = 0; i < count && emitted < 10; i++) {
        /* Context-summary cache records are transport metadata, not user memory facts. */
        if (entries[i].category &&
            strcmp(entries[i].category, "context_summary") == 0) {
            continue;
        }


        cc_string_builder_appendf(&sb, "- %s: %s", entries[i].key, entries[i].value);
        if (entries[i].category)
            cc_string_builder_appendf(&sb, " (category: %s)", entries[i].category);
        cc_string_builder_append(&sb, "\n");
        emitted++;
    }



    cc_memory_entry_free_array(entries, count);



    *out_memory_block = cc_string_builder_take(&sb);
    return *out_memory_block ? cc_result_ok() :
        cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build memory context");
}
