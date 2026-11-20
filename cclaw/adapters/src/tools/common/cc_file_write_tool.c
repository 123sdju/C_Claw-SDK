



#include "cc/adapters/cc_builtin_tools.h"
#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_json_fields.h"
#include "cc/internal/cc_json_syntax_check.h"
#include "cc/internal/cc_alloc.h"
#include <stdlib.h>
#include <string.h>

/*
 * file_write 保存 filesystem 浅拷贝，后端 self/vtable 由创建者保持有效。
 * 根相对路径的访问约束由 write_relative_text 实现，工具不销毁后端。
 */
typedef struct {
    cc_filesystem_t fs;
} cc_file_write_tool_t;

/** @brief 提供稳定注册名。 @param[in] self 忽略的状态借用，可为 NULL。 @return 静态只读名称。 */
static const char *file_write_name(void *self)
{
    (void)self;
    return "file_write";
}

/** @brief 提供写入说明。 @param[in] self 忽略的状态借用，可为 NULL。 @return 静态只读描述。 */
static const char *file_write_description(void *self)
{
    (void)self;
    return "Write content to a file";
}

/** @brief 声明必填字符串 path/content。 @param[in] self 忽略的状态借用，可为 NULL。 @return 静态 JSON Schema 对象文本，调用者只读借用。 */
static const char *file_write_schema_json(void *self)
{
    (void)self;
    return "{"
        "\"type\":\"object\","
        "\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"Path to the file to write\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"Content to write\"}"
        "},"
        "\"required\":[\"path\",\"content\"]"
    "}";
}

/**
 * @brief 严格校验参数后委托根相对文本写入，成功生成独立确认文本。
 * @param[in] self 必填存活状态借用，filesystem owner 覆盖整个调用。
 * @param[in] args_json 调用期稳定完整对象，path/content 各为单个字符串字段，正文允许空串。
 * @param[in] ctx 调用期上下文借用，缺少 workspace_dir 作为业务失败反馈。
 * @param[out] out_result 必填新建或已 cleanup 槽，先清零，输出字符串交调用者 cleanup。
 * @return 参数地址错误为 INVALID_ARGUMENT，结果副本 OOM 为 OUT_OF_MEMORY，其余业务失败以 CC_OK 和 ok=0 反馈，诊断由调用者释放。
 * @note 根约束、父目录及部分写入语义沿后端，失败可能已改动目标，确认文本 OOM 发生时写入已成功。
 * 不自行执行写入大小、取消或超时策略，executor 在调用前后管理对应边界，输出预算由 executor 复核。
 * 工具不保留输入，文件竞争由业务及后端同步，销毁前排空调用。
 */
static cc_result_t file_write_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    if (!out_result) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null file write result");
    memset(out_result, 0, sizeof(cc_tool_result_t));
    if (!self) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null file write tool");
    cc_file_write_tool_t *tool = (cc_file_write_tool_t *)self;
    const char *error = "Failed to parse arguments JSON";
    cc_json_value_t *args = NULL;
    cc_result_t rc = cc_result_ok();
    if (!args_json || cc_storage_json_has_nul_escape(args_json)) goto error;
    rc = cc_json_parse_strict(args_json, &args);
    if (rc.code != CC_OK) goto error;
    const char *fields[] = {"path", "content"};
    if (!cc_json_is_object(args) || cc_json_duplicate_known_field(args, fields, 2)) {
        error = "Arguments must be an object with one path and one content field";
        goto error;
    }
    cc_json_value_t *path_val = cc_json_object_get(args, "path");
    cc_json_value_t *content_val = cc_json_object_get(args, "content");
    const char *path = cc_json_string_value(path_val);
    const char *content = cc_json_string_value(content_val);

    if (!path || !content) {
        error = "Missing required string parameters: path and content";
        goto error;
    }

    if (!ctx || !ctx->workspace_dir ||
        !tool->fs.vtable ||
        !(tool->fs.capabilities & CC_FS_CAP_ROOT_SCOPED) ||
        !tool->fs.vtable->write_relative_text) {
        error = "Filesystem does not support root-scoped writes";
        goto error;
    }
    cc_result_free(&rc);
    rc = tool->fs.vtable->write_relative_text(
        tool->fs.self, ctx->workspace_dir, path, content);
    if (rc.code != CC_OK) {
        error = rc.message ? rc.message : "Failed to write file";
        goto error;
    }
    cc_json_destroy(args);
    cc_result_free(&rc);
    out_result->text = cc_copy_string("File written successfully");
    if (!out_result->text) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY,
            "File was written, but its confirmation could not be allocated; do not repeat the write solely for this error");
    }
    out_result->ok = 1;
    return cc_result_ok();

error:
    out_result->error = cc_copy_string(error);
    cc_json_destroy(args);
    cc_result_free(&rc);
    return out_result->error ? cc_result_ok() :
        cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate file write error");
}

/** @brief 消费写入工具状态。 @param[in,out] self 独占堆状态，可为 NULL。 @pre owner 已关闭入口并排空调用。 @post 状态释放，filesystem 仍由其创建者管理。 */
static void file_write_destroy(void *self)
{
    free(self);
}

/* 静态方法表覆盖工具整个生命周期。 */
static cc_tool_vtable_t file_write_vtable = {
    file_write_name,
    file_write_description,
    file_write_schema_json,
    file_write_call,
    file_write_destroy
};

/**
 * @brief 创建借用文件系统的写入工具，后端能力在 call 校验。
 * @param[in] fs 浅拷贝端口，创建者保持 self/vtable 覆盖后续调用。
 * @param[out] out_tool 必填空描述符，先清零，成功交付拥有状态和静态方法表。
 * @return 参数或 OOM 错误，失败无新状态，成功由调用者 destroy 或转交 registry，诊断由调用者释放。
 */
cc_result_t cc_file_write_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool)
{
    if (!out_tool) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null file write tool output");
    }
    memset(out_tool, 0, sizeof(*out_tool));
    cc_file_write_tool_t *self = calloc(1, sizeof(cc_file_write_tool_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create file write tool");

    self->fs = fs;
    out_tool->self = self;
    out_tool->vtable = &file_write_vtable;
    return cc_result_ok();
}
