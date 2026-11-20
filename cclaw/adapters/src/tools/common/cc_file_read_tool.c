



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
 * file_read 私有状态只保存 filesystem 的浅拷贝，创建者保持其 self/vtable
 * 覆盖工具调用期，工具销毁仅释放本状态。根目录约束由 read_relative_text 实现。
 */
typedef struct {
    cc_filesystem_t fs;
} cc_file_read_tool_t;

/** @brief 提供稳定注册名。 @param[in] self 忽略的状态借用，可为 NULL。 @return 静态只读名称，无释放责任。 */
static const char *file_read_name(void *self)
{
    (void)self;
    return "file_read";
}

/** @brief 提供读取说明。 @param[in] self 忽略的状态借用，可为 NULL。 @return 静态只读描述。 */
static const char *file_read_description(void *self)
{
    (void)self;
    return "Read the contents of a file";
}

/** @brief 声明必填字符串 path。 @param[in] self 忽略的状态借用，可为 NULL。 @return 静态 JSON Schema 对象文本，调用者只读借用。 */
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

/**
 * @brief 严格校验读取参数，再委托具有根目录约束的文本入口。
 * @param[in] self 必填存活工具状态借用，文件系统 owner 覆盖整个同步调用。
 * @param[in] args_json 调用期稳定参数文本，须为完整对象，path 为单个字符串字段。
 * @param[in] ctx 调用期上下文借用，缺少 workspace_dir 作为业务失败反馈。
 * @param[out] out_result 必填新建或已 cleanup 槽，先清零，成功正文或错误副本交调用者 cleanup。
 * @return 参数地址错误为 INVALID_ARGUMENT，结果副本 OOM 为 OUT_OF_MEMORY，其他业务失败以 CC_OK 和 ok=0 返回，诊断由调用者释放。
 * @note 路径检查及链接规则沿后端，失败可能已有部分读取缓冲并在本函数归还。
 * 不保留上下文，也不自行截取 max_result_bytes，完整文件读取后由 executor 检查输出预算。
 * 并发文件访问由后端及业务同步，销毁前 owner 排空调用。
 */
static cc_result_t file_read_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
)
{
    if (!out_result) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null file read result");
    memset(out_result, 0, sizeof(cc_tool_result_t));
    if (!self) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null file read tool");
    cc_file_read_tool_t *tool = (cc_file_read_tool_t *)self;
    const char *error = "Failed to parse arguments JSON";
    cc_json_value_t *args = NULL;
    char *content = NULL;
    cc_result_t rc = cc_result_ok();
    if (!args_json || cc_storage_json_has_nul_escape(args_json)) goto error;
    rc = cc_json_parse_strict(args_json, &args);
    if (rc.code != CC_OK) goto error;
    const char *fields[] = {"path"};
    if (!cc_json_is_object(args) || cc_json_duplicate_known_field(args, fields, 1)) {
        error = "Arguments must be an object with one path field";
        goto error;
    }
    cc_json_value_t *path_val = cc_json_object_get(args, "path");
    const char *path = cc_json_string_value(path_val);
    if (!path) {
        error = "Missing required string parameter: path";
        goto error;
    }
    if (!ctx || !ctx->workspace_dir ||
        !tool->fs.vtable ||
        !(tool->fs.capabilities & CC_FS_CAP_ROOT_SCOPED) ||
        !tool->fs.vtable->read_relative_text) {
        error = "Filesystem does not support root-scoped reads";
        goto error;
    }
    cc_result_free(&rc);
    rc = tool->fs.vtable->read_relative_text(
        tool->fs.self, ctx->workspace_dir, path, &content);
    if (rc.code != CC_OK) {
        error = rc.message ? rc.message : "Failed to read file";
        goto error;
    }
    if (!content) {
        error = "Filesystem returned no file content";
        goto error;
    }
    cc_json_destroy(args);
    cc_result_free(&rc);
    out_result->ok = 1;
    out_result->text = content;
    return cc_result_ok();

error:
    out_result->error = cc_copy_string(error);
    cc_json_destroy(args);
    free(content);
    cc_result_free(&rc);
    return out_result->error ? cc_result_ok() :
        cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate file read error");
}

/** @brief 消费读取工具状态。 @param[in,out] self 独占堆状态，可为 NULL。 @pre owner 已关闭入口并排空调用。 @post 状态释放，借用的 filesystem 由其 owner 继续管理。 */
static void file_read_destroy(void *self)
{
    free(self);
}

/* 静态方法表在工具整个生命周期内保持有效。 */
static cc_tool_vtable_t file_read_vtable = {
    file_read_name,
    file_read_description,
    file_read_schema_json,
    file_read_call,
    file_read_destroy
};

/**
 * @brief 创建借用文件系统的读取工具，不探测后端能力。
 * @param[in] fs 浅拷贝端口，创建者保持 self/vtable 覆盖后续调用，缺少根约束支持在 call 反馈。
 * @param[out] out_tool 必填空描述符，先清零，成功交付拥有状态和静态方法表。
 * @return 参数或 OOM 错误，失败无新状态，成功由调用者 destroy 或成功转交 registry，诊断由调用者释放。
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
