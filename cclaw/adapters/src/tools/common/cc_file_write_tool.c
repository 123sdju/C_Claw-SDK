



#include "cc/adapters/cc_builtin_tools.h"
#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include <stdlib.h>
#include <string.h>

/*
 * file_write 工具的私有状态。
 *
 * 和 file_read 一样，它只借用 filesystem 端口，不拥有平台文件系统对象。工具本身保持
 * 很薄，安全边界由 path helper、policy/approval 和 filesystem adapter 共同完成。
 */
typedef struct {
    cc_filesystem_t fs;
} cc_file_write_tool_t;

/* 返回工具注册名；LLM tool call 必须使用该名字才能命中 registry。 */
static const char *file_write_name(void *self)
{
    (void)self;
    return "file_write";
}

/* 返回工具说明文本；静态字符串由工具实现持有，调用方只读使用。 */
static const char *file_write_description(void *self)
{
    (void)self;
    return "Write content to a file";
}

/*
 * 返回写文件工具的参数 schema。
 *
 * path/content 都是必填 string，core 层会先做最小校验；这里仍保留运行时检查，因为
 * adapter 不能假设所有调用方都一定经过 tool executor。
 */
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

/*
 * 执行文件写入。
 *
 * 写入工具比读取工具风险更高：目标文件可能还不存在，因此除了检查 full_path，还要检查
 * parent_dir 是否在 workspace 内，防止 `../`、prefix 绕过或符号链接相关路径逃逸。
 * 业务失败通过 out_result 返回给 agent，cc_result 保持 OK，表示这次工具错误可恢复。
 */
static cc_result_t file_write_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    cc_file_write_tool_t *tool = (cc_file_write_tool_t *)self;

    memset(out_result, 0, sizeof(cc_tool_result_t));

    cc_json_value_t *args = NULL;
    cc_result_t rc = cc_json_parse(args_json, &args);
    if (rc.code != CC_OK) {
        out_result->ok = 0;
        out_result->error = cc_copy_string("Failed to parse arguments JSON");
        return cc_result_ok();
    }

    cc_json_value_t *path_val = cc_json_object_get(args, "path");
    cc_json_value_t *content_val = cc_json_object_get(args, "content");
    const char *path = cc_json_string_value(path_val);
    const char *content = cc_json_string_value(content_val);

    if (!path || !content) {
        out_result->ok = 0;
        out_result->error = cc_copy_string("Missing required parameters: path and content");
        cc_json_destroy(args);
        return cc_result_ok();
    }

    if (!ctx || !ctx->workspace_dir ||
        !(tool->fs.capabilities & CC_FS_CAP_ROOT_SCOPED) ||
        !tool->fs.vtable->write_relative_text) {
        out_result->ok = 0;
        out_result->error = cc_copy_string("Filesystem does not support root-scoped writes");
        cc_json_destroy(args);
        return cc_result_ok();
    }
    rc = tool->fs.vtable->write_relative_text(
        tool->fs.self, ctx->workspace_dir, path, content);
    cc_json_destroy(args);

    if (rc.code != CC_OK) {
        out_result->ok = 0;
        out_result->error = cc_copy_string(rc.message ? rc.message : "Failed to write file");
        cc_result_free(&rc);
        return cc_result_ok();
    }

    out_result->ok = 1;
    out_result->text = cc_copy_string("File written successfully");
    return cc_result_ok();
}

/* 销毁 file_write 工具私有对象；filesystem 端口由 builder/platform 层管理。 */
static void file_write_destroy(void *self)
{
    free(self);
}

/* file_write vtable，把工具方法暴露给通用 tool executor。 */
static cc_tool_vtable_t file_write_vtable = {
    file_write_name,
    file_write_description,
    file_write_schema_json,
    file_write_call,
    file_write_destroy
};

/*
 * 创建 file_write 工具实例。
 *
 * 成功后 out_tool 由 registry/runtime 持有并负责调用 destroy；fs 必须在工具生命周期内
 * 有效。这里不创建目录策略，目录是否存在由 filesystem adapter 的 write_text 决定。
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
