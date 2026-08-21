



#include "cc/adapters/cc_builtin_tools.h"
#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include <stdlib.h>
#include <string.h>

/*
 * file_read 工具的私有对象。
 *
 * 工具只保存 filesystem 端口的值拷贝，不拥有 filesystem self；具体平台文件系统由
 * runtime builder 统一管理。这样同一个工具实现可以运行在 POSIX、内存文件系统或 MCU
 * 适配层上。
 */
typedef struct {
    cc_filesystem_t fs;
} cc_file_read_tool_t;

/* 返回工具注册名；registry/schema 生成和 LLM tool call 都依赖这个稳定名字。 */
static const char *file_read_name(void *self)
{
    (void)self;
    return "file_read";
}

/* 返回给模型和调试 UI 的简短说明；不分配内存，调用方不能释放返回值。 */
static const char *file_read_description(void *self)
{
    (void)self;
    return "Read the contents of a file";
}

/*
 * 返回工具参数 JSON Schema。
 *
 * core tool executor 会在执行前做最小 schema 校验；因此这里声明 path 必填且为 string，
 * 可以在进入文件系统前拦截明显错误参数。
 */
static const char *file_read_schema_json(void *self)
{
    (void)self;
    return "{"
        "\"type\":\"object\","
        "\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"Path to the file to read\"}"
        "},"
        "\"required\":[\"path\"]"
    "}";
}

/*
 * 执行文件读取。
 *
 * args_json 由 runtime 传入，out_result 由本函数填充；业务失败（JSON 错误、路径越界、
 * 文件读取失败）都通过 out_result->ok=0 返回，cc_result 仍为 OK，表示 agent 可以恢复
 * 并把错误作为 tool observation 反馈给模型。路径先拼接 workspace，再做边界检查，防止
 * `../` 等越权读取。
 */
static cc_result_t file_read_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    cc_file_read_tool_t *tool = (cc_file_read_tool_t *)self;

    memset(out_result, 0, sizeof(cc_tool_result_t));

    const char *path = NULL;
    cc_json_value_t *args = NULL;
    cc_result_t rc = cc_json_parse(args_json, &args);
    if (rc.code != CC_OK) {
        out_result->ok = 0;
        out_result->error = cc_copy_string("Failed to parse arguments JSON");
        return cc_result_ok();
    }

    cc_json_value_t *path_val = cc_json_object_get(args, "path");
    path = cc_json_string_value(path_val);

    if (!path) {
        out_result->ok = 0;
        out_result->error = cc_copy_string("Missing required parameter: path");
        cc_json_destroy(args);
        return cc_result_ok();
    }

    if (!ctx || !ctx->workspace_dir ||
        !(tool->fs.capabilities & CC_FS_CAP_ROOT_SCOPED) ||
        !tool->fs.vtable->read_relative_text) {
        out_result->ok = 0;
        out_result->error = cc_copy_string("Filesystem does not support root-scoped reads");
        cc_json_destroy(args);
        return cc_result_ok();
    }

    char *content = NULL;
    rc = tool->fs.vtable->read_relative_text(
        tool->fs.self, ctx->workspace_dir, path, &content);
    cc_json_destroy(args);

    if (rc.code != CC_OK) {
        out_result->ok = 0;
        out_result->error = cc_copy_string(rc.message ? rc.message : "Failed to read file");
        cc_result_free(&rc);
        return cc_result_ok();
    }

    out_result->ok = 1;
    out_result->text = content;
    return cc_result_ok();
}

/* 销毁 file_read 工具私有对象；filesystem 端口本身不在这里销毁。 */
static void file_read_destroy(void *self)
{
    free(self);
}

/* file_read 的 vtable，把 C 函数集合包装成面向对象风格的工具接口。 */
static cc_tool_vtable_t file_read_vtable = {
    file_read_name,
    file_read_description,
    file_read_schema_json,
    file_read_call,
    file_read_destroy
};

/*
 * 创建 file_read 工具实例。
 *
 * 成功后 out_tool 获得 self/vtable，后续由 tool registry/runtime 调用 destroy 释放 self。
 * fs 是端口值拷贝，必须在工具生命周期内保持其 self/vtable 有效。
 */
cc_result_t cc_file_read_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool)
{
    if (!out_tool) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null file read tool output");
    }
    memset(out_tool, 0, sizeof(*out_tool));
    cc_file_read_tool_t *self = calloc(1, sizeof(cc_file_read_tool_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create file read tool");

    self->fs = fs;
    out_tool->self = self;
    out_tool->vtable = &file_read_vtable;
    return cc_result_ok();
}
