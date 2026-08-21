

#ifndef CC_MEDIA_H
#define CC_MEDIA_H

#include "cc/core/cc_result.h"
#include <stddef.h>

/*
 * 多模态内容的媒体类型。
 *
 * TEXT 是普通对话文本；其他类型通过 artifact 描述。SDK 不在这里绑定具体
 * 图片/音频解码库，只保存协议层元数据，方便 MCU/RTOS profile 按需裁剪。
 */
typedef enum cc_media_kind {
    CC_MEDIA_TEXT = 0,
    CC_MEDIA_IMAGE = 1,
    CC_MEDIA_AUDIO = 2,
    CC_MEDIA_VIDEO = 3,
    CC_MEDIA_FILE = 4
} cc_media_kind_t;

/*
 * content part 的方向。
 *
 * INPUT 表示用户或工具输入给模型的内容，OUTPUT 表示模型或工具产出的内容。
 * 方向字段让同一套结构能被 runtime、memory 和观测事件复用，而不需要新增多套
 * 相似结构。
 */
typedef enum cc_content_part_direction {
    CC_CONTENT_PART_INPUT = 0,
    CC_CONTENT_PART_OUTPUT = 1
} cc_content_part_direction_t;

/*
 * 多模态资源限制。
 *
 * 该结构由调用方提供，不拥有 allowed_mime_prefixes 数组和其中字符串。它集中
 * 描述 artifact 个数、大小、inline base64 和 MIME 前缀策略，适合嵌入式环境把
 * 内存预算、网络预算和模型能力在进入 provider 前统一校验。
 */
typedef struct cc_media_limits {
    size_t max_artifacts;
    size_t max_artifact_bytes;
    size_t max_base64_bytes;
    int allow_inline_base64;
    const char **allowed_mime_prefixes;
    size_t allowed_mime_prefix_count;
} cc_media_limits_t;

/*
 * 一个媒体 artifact 的元数据。
 *
 * 字符串字段由 artifact 拥有，调用 cc_media_artifact_cleanup() 释放。path/uri/
 * data_base64 三种定位方式可以按平台能力选择其一；SDK 不直接打开文件或解码数据，
 * 只负责校验和序列化。结构不加锁，跨线程传递需要由上层复制或转移所有权。
 */
typedef struct cc_media_artifact {
    char *id;
    cc_media_kind_t kind;
    char *mime;
    char *path;
    char *uri;
    char *data_base64;
    size_t bytes;
    int width;
    int height;
    int duration_ms;
    char *created_at;
    char *metadata;
} cc_media_artifact_t;

/* artifact 动态数组；items 中每个元素都拥有自己的字符串字段。 */
typedef struct cc_media_artifact_list {
    cc_media_artifact_t *items;
    size_t count;
    size_t capacity;
} cc_media_artifact_list_t;

/*
 * message 中的一个 content part。
 *
 * kind 为 TEXT 时 text 字段有效；其他 kind 使用 artifact 字段。cleanup 会同时
 * 释放 text 和 artifact，因此调用方不应手动释放内部字段。
 */
typedef struct cc_content_part {
    cc_media_kind_t kind;
    cc_content_part_direction_t direction;
    char *text;
    cc_media_artifact_t artifact;
} cc_content_part_t;

/* content part 动态数组；由 message 和 context builder 共享的多模态承载结构。 */
typedef struct cc_content_parts {
    cc_content_part_t *items;
    size_t count;
    size_t capacity;
} cc_content_parts_t;

/* media kind 到协议字符串的映射；返回静态字符串，调用方不能释放。 */
const char *cc_media_kind_string(cc_media_kind_t kind);

/* 协议字符串到 media kind 的宽松解析；未知类型按 file 处理。 */
cc_media_kind_t cc_media_kind_from_string(const char *kind);

/* 初始化为文本-only 限制，适合低资源 MCU 或不支持多模态的 provider。 */
void cc_media_limits_text_only(cc_media_limits_t *limits);

/* 初始化默认多模态限制；调用方可在此基础上按产品预算继续收紧。 */
void cc_media_limits_default_multimodal(cc_media_limits_t *limits);

/* 初始化 artifact 为可 cleanup 的空对象，默认 kind 为 file。 */
void cc_media_artifact_init(cc_media_artifact_t *artifact);

/* 释放 artifact 拥有的字符串字段并清零对象；不释放 artifact 指针本身。 */
void cc_media_artifact_cleanup(cc_media_artifact_t *artifact);

/* 深拷贝 artifact；dst 会被初始化，失败时不会留下半拷贝资源。 */
cc_result_t cc_media_artifact_copy(
    const cc_media_artifact_t *src,
    cc_media_artifact_t *dst
);

/* 按 limits 校验单个 artifact 的 MIME、路径和大小策略。 */
cc_result_t cc_media_artifact_validate(
    const cc_media_artifact_t *artifact,
    const cc_media_limits_t *limits
);

/* 初始化 artifact list；适合栈对象或结构体成员。 */
void cc_media_artifact_list_init(cc_media_artifact_list_t *list);

/* 清理 list 中每个 artifact 及数组本身；不释放 list 指针。 */
void cc_media_artifact_list_cleanup(cc_media_artifact_list_t *list);

/* 追加 artifact 的深拷贝；list 自动扩容，调用方仍拥有原始 artifact。 */
cc_result_t cc_media_artifact_list_append(
    cc_media_artifact_list_t *list,
    const cc_media_artifact_t *artifact
);

/* 深拷贝 artifact list；失败时自动清理 dst。 */
cc_result_t cc_media_artifact_list_copy(
    const cc_media_artifact_list_t *src,
    cc_media_artifact_list_t *dst
);

/* 校验 list 总数和每个 artifact；用于 provider 请求前的能力/资源检查。 */
cc_result_t cc_media_artifact_list_validate(
    const cc_media_artifact_list_t *list,
    const cc_media_limits_t *limits
);

/* 序列化 artifact list；返回 JSON 字符串由调用方 free()。 */
cc_result_t cc_media_artifact_list_to_json(
    const cc_media_artifact_list_t *list,
    char **out_json
);

/* 从 JSON 解析 artifact list；out_list 成功或失败后都可安全 cleanup。 */
cc_result_t cc_media_artifact_list_from_json(
    const char *json,
    cc_media_artifact_list_t *out_list
);

/* 生成人类可读摘要；返回字符串由调用方 free()，常用于日志和 memory。 */
cc_result_t cc_media_artifact_list_summarize(
    const cc_media_artifact_list_t *list,
    char **out_summary
);

/* 清理单个 content part；不释放 part 指针本身。 */
void cc_content_part_cleanup(cc_content_part_t *part);

/* 深拷贝 content part；文本和 artifact 都会获得独立所有权。 */
cc_result_t cc_content_part_copy(
    const cc_content_part_t *src,
    cc_content_part_t *dst
);

/* 初始化 content parts 动态数组。 */
void cc_content_parts_init(cc_content_parts_t *parts);

/* 清理 content parts 中所有元素和数组内存。 */
void cc_content_parts_cleanup(cc_content_parts_t *parts);

/* 追加文本 part；text 为 NULL 时按空字符串处理并深拷贝。 */
cc_result_t cc_content_parts_append_text(
    cc_content_parts_t *parts,
    const char *text,
    cc_content_part_direction_t direction
);

/* 追加 artifact part；artifact 会被深拷贝到 parts 内部。 */
cc_result_t cc_content_parts_append_artifact(
    cc_content_parts_t *parts,
    const cc_media_artifact_t *artifact,
    cc_content_part_direction_t direction
);

/* 深拷贝 content parts；失败时清理 dst。 */
cc_result_t cc_content_parts_copy(
    const cc_content_parts_t *src,
    cc_content_parts_t *dst
);

/* 校验 content parts 中的非文本 artifact 是否满足 limits。 */
cc_result_t cc_content_parts_validate(
    const cc_content_parts_t *parts,
    const cc_media_limits_t *limits
);

/* 序列化 content parts；返回 JSON 字符串由调用方 free()。 */
cc_result_t cc_content_parts_to_json(
    const cc_content_parts_t *parts,
    char **out_json
);

/* 从 JSON 解析 content parts；失败时清理已经追加的内容。 */
cc_result_t cc_content_parts_from_json(
    const char *json,
    cc_content_parts_t *out_parts
);

/* 生成文本摘要；非文本 artifact 会转成人类可读占位信息。 */
cc_result_t cc_content_parts_text_summary(
    const cc_content_parts_t *parts,
    char **out_summary
);

#endif
