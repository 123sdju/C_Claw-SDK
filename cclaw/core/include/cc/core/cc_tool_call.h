

#ifndef CC_TOOL_CALL_H
#define CC_TOOL_CALL_H

#include "cc/core/cc_media.h"
#include "cc/core/cc_result.h"
#include "cc/core/cc_llm_usage.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 拥有调用身份、工具名和原始参数文本的描述，没有内部锁。
 * @note 本数据层保留参数字符串，实际参数对象/schema 与调用身份约束由上层执行链校验。
 */
typedef struct cc_tool_call {
    char *id; /**< 可选拥有调用 ID。 */
    char *name; /**< 可选拥有工具名。 */
    char *arguments_json; /**< 可选拥有原始参数文本，序列化缺省为 {}。 */
} cc_tool_call_t;

/**
 * @brief 可变拥有调用数组，初始化后由 list_cleanup 消费。
 * @note 可变容器满足 count <= capacity，非零容量须有可 realloc 的数组，前 count 项有效。
 * 只读 copy/to_json 可借用含 count 项的视图而不依赖 capacity，视图不交拥有清理函数。
 */
typedef struct cc_tool_call_list {
    cc_tool_call_t *items; /**< 拥有数组，或只读入口调用期的项视图。 */
    size_t count; /**< 有效项数量。 */
    size_t capacity; /**< 可变拥有数组的元素容量。 */
} cc_tool_call_list_t;

/**
 * @brief 拥有工具业务结果和资源列表，没有内部锁。
 * @note ok 与 SDK 函数返回码独立，调用者按业务解释结果，所有字段在 cleanup/destroy 时归还。
 */
typedef struct cc_tool_result {
    int ok; /**< 按值保留的业务状态，通常非零表示成功。 */
    char *text; /**< 可选拥有结果文本。 */
    char *error; /**< 可选拥有业务错误说明。 */
    char *metadata; /**< 可选拥有元数据文本，本模块不解析其格式。 */
    cc_media_artifact_list_t artifacts; /**< 拥有输出资源及各字符串。 */
} cc_tool_result_t;

/**
 * @brief provider 交付的拥有响应，由调用者 response_free 清理。
 * @note 写入与清理由外层串行化，借用者退出后才能释放。各内容字段独立维护，set_text 只替换 text/has_text。
 */
typedef struct cc_llm_response {
    int has_text; /**< 已显式提供文本，空字符串也可置一。 */
    char *text; /**< 可选拥有整体文本。 */
    cc_content_parts_t content; /**< 独立拥有内容项。 */
    cc_media_artifact_list_t artifacts; /**< 独立拥有资源列表。 */
    cc_tool_call_list_t tool_calls; /**< 拥有工具调用列表。 */
    int finished; /**< provider 按其协议记录的完成标志。 */
    char *reasoning_content; /**< 可选拥有推理文本。 */
    cc_llm_usage_t usage; /**< 按值用量与有效性标志。 */
} cc_llm_response_t;

/**
 * @brief 深拷贝字段并创建拥有的堆调用描述。
 * @param[in] id 可选调用期 ID 借用。
 * @param[in] name 可选调用期工具名借用。
 * @param[in] arguments_json 可选调用期原始文本，NULL 复制 {}，空串保持为空，本入口不解析。
 * @param[out] out_call 必填空拥有槽，存在时先置 NULL，成功交付须 destroy 的对象。
 * @return CC_OK、参数或 OOM，失败归还部分字段，诊断由调用者释放。
 */
cc_result_t cc_tool_call_create(
    const char *id,
    const char *name,
    const char *arguments_json,
    cc_tool_call_t **out_call
);

/** @brief 消费调用字段并清零记录。 @param[in,out] call 可空初始化拥有对象，借用者已退出。 @post 字符串已归还，结构体仍由调用者拥有。 */
void cc_tool_call_cleanup(cc_tool_call_t *call);

/** @brief 消费堆调用及字段。 @param[in,out] call 可空拥有地址，借用者已退出。 @post 非空对象释放，原地址失效。 */
void cc_tool_call_destroy(cc_tool_call_t *call);

/**
 * @brief 深拷贝三个字段，自复制保持原值。
 * @param[in] src 必填稳定源借用，NULL 字段原样保留。
 * @param[out] dst 必填新对象或已 cleanup 槽，src==dst 成功跳过，其他情况先清零。
 * @return NULL 参数保持可访问目标原值，OOM 归还并清零目标，成功交付独立副本，诊断由调用者释放。
 */
cc_result_t cc_tool_call_copy(const cc_tool_call_t *src, cc_tool_call_t *dst);

/** @brief 清零列表记录。 @param[out] list 可空新对象或已 cleanup 槽。 @post 非空目标为空列表，已有拥有资源须在调用前清理。 */
void cc_tool_call_list_init(cc_tool_call_list_t *list);

/** @brief 消费列表有效项和数组并清零。 @param[in,out] list 可空初始化拥有列表，借用者已退出。 @post 资源归还，记录可再次使用，借用视图须由原 owner 清理。 */
void cc_tool_call_list_cleanup(cc_tool_call_list_t *list);

/**
 * @brief 快照源字段后扩容并追加独立副本，支持同列表现有元素作为输入。
 * @param[in,out] list 必填独占拥有列表，满足可变容量约定。
 * @param[in] call 必填调用期描述借用，内层字符串由原 owner 保持到复制结束。
 * @return 参数、容量溢出或 OOM，成功 count 加一，失败保留旧有效项和 count，已成功扩容的 capacity 可增加，诊断由调用者释放。
 */
cc_result_t cc_tool_call_list_append(
    cc_tool_call_list_t *list,
    const cc_tool_call_t *call
);

/**
 * @brief 从借用字段构造临时描述并复用深拷贝追加。
 * @param[in,out] list 必填独占拥有列表。
 * @param[in] id 可选调用期 ID 借用，可来自列表已有项。
 * @param[in] name 可选调用期工具名借用。
 * @param[in] arguments_json 可选调用期参数文本，NULL 使用 {}，其他原样复制。
 * @return 追加结果，输入拥有权保持，诊断由调用者释放。
 */
cc_result_t cc_tool_call_list_append_values(
    cc_tool_call_list_t *list,
    const char *id,
    const char *name,
    const char *arguments_json
);

/**
 * @brief 深拷贝列表全部有效项，自复制保持原对象。
 * @param[in] src 必填稳定只读列表，非零 count 须有对应可读数组，capacity 可为视图缺省值。
 * @param[out] dst 必填新对象或已 cleanup 槽，src==dst 成功跳过，其他情况先清零。
 * @return 参数、尺寸或 OOM，复制失败归还部分目标，成功取得独立拥有列表，诊断由调用者释放。
 */
cc_result_t cc_tool_call_list_copy(
    const cc_tool_call_list_t *src,
    cc_tool_call_list_t *dst
);

/**
 * @brief 完整序列化工具调用数组，arguments 保留 JSON 字符串形式。
 * @param[in] list 借用的稳定列表，非空列表须有有效 items。
 * @param[out] out_json 输出独占字符串，调用者 free，失败为 NULL。
 * @return 成功为 CC_OK，参数、尺寸或分配错误的诊断由调用者释放。
 * @note 任一字段构造失败会撤销整份输出，源列表并发写入由调用者排除。
 */
cc_result_t cc_tool_call_list_to_json(
    const cc_tool_call_list_t *list,
    char **out_json
);

/**
 * @brief 严格解析 function 调用数组，任一项失败时撤销整列表。
 * @param[in] json 可选调用期文本借用，C NULL/空串表示空列表，其他输入须完整数组且没有解码 NUL。
 * @param[out] out_list 必填新对象或已 cleanup 槽，存在时先初始化，成功交付拥有列表。
 * @return CC_OK、参数、JSON/项字段、容量或分配错误，失败清理全部输出，诊断包含项索引/字段位置。
 * @note 每项及 function 须为对象，字段名忽略大小写，重复已知字段拒绝，未知字段跳过。
 * id/name 缺失/null 保留 NULL，arguments 缺失/null 使用 {}，已存在其他值须为字符串。
 * type 若存在须为字符串，其业务值沿既有规则忽略。arguments 作为文本保留，内部 JSON/schema 留给上层校验。
 */
cc_result_t cc_tool_call_list_from_json(
    const char *json,
    cc_tool_call_list_t *out_list
);

/**
 * @brief 深拷贝可选字段，创建拥有工具业务结果。
 * @param[in] ok 业务状态，按值保存。
 * @param[in] text 可选调用期结果文本借用。
 * @param[in] error 可选调用期错误说明借用。
 * @param[in] metadata 可选调用期元数据借用。
 * @param[out] out_result 必填空拥有槽，存在时先置 NULL，成功交付须 destroy 的对象。
 * @return CC_OK、参数或 OOM，失败归还部分资源，诊断由调用者释放。
 */
cc_result_t cc_tool_result_create(
    int ok,
    const char *text,
    const char *error,
    const char *metadata,
    cc_tool_result_t **out_result
);

/** @brief 消费业务结果字段及资源列表。 @param[in,out] result 可空初始化拥有对象，借用者已退出。 @post 全部字段归还并清零，结构体留归调用者。 */
void cc_tool_result_cleanup(cc_tool_result_t *result);

/** @brief 消费堆业务结果及全部字段。 @param[in,out] result 可空拥有地址，借用者已退出。 @post 非空对象释放，原地址失效。 */
void cc_tool_result_destroy(cc_tool_result_t *result);

/**
 * @brief 向业务结果追加资源副本。
 * @param[in,out] result 必填独占拥有结果，资源列表满足可变容量约定。
 * @param[in] artifact 必填调用期借用，可来自同列表现有项。
 * @return 参数或媒体追加结果，失败不增加项数，诊断由调用者释放。
 */
cc_result_t cc_tool_result_add_artifact(
    cc_tool_result_t *result,
    const cc_media_artifact_t *artifact
);

/**
 * @brief 深拷贝完整资源列表后替换旧列表。
 * @param[in,out] result 必填独占拥有结果。
 * @param[in] artifacts 必填稳定源借用，允许为 result 自身列表。
 * @return 参数或复制错误保持旧列表，成功归还旧值并接管副本，诊断由调用者释放。
 */
cc_result_t cc_tool_result_set_artifacts(
    cc_tool_result_t *result,
    const cc_media_artifact_list_t *artifacts
);

/**
 * @brief 按值取走完整拥有资源列表。
 * @param[in,out] result 可空独占结果，非空时资源列表置空。
 * @return 拥有列表，调用者用媒体 list_cleanup 消费，NULL 输入返回零值空列表。
 */
cc_media_artifact_list_t cc_tool_result_take_artifacts(cc_tool_result_t *result);

/**
 * @brief 清零并初始化响应嵌套列表。
 * @param[out] response 必填新对象或已 free 槽，已有资源须先释放。
 * @return NULL 为 INVALID_ARGUMENT，成功为 CC_OK，诊断由调用者释放。
 */
cc_result_t cc_llm_response_init(cc_llm_response_t *response);

/** @brief 消费响应全部拥有字段并清零。 @param[in,out] response 可空初始化拥有响应，借用者已退出。 @post 记录可再次使用，结构体继续由调用者拥有。 */
void cc_llm_response_free(cc_llm_response_t *response);

/**
 * @brief 先复制再替换整体响应文本，并置 has_text。
 * @param[in,out] response 必填独占响应，其他内容/资源/工具字段独立保留。
 * @param[in] text 可选调用期文本借用，允许指向旧文本及其内部，NULL 采用空串。
 * @return 参数或 OOM 保持旧文本及标志，成功接管副本并置 has_text=1，诊断由调用者释放。
 */
cc_result_t cc_llm_response_set_text(cc_llm_response_t *response, const char *text);

/**
 * @brief 向统一响应追加独立工具调用副本。
 * @param[in,out] response 必填独占响应，工具列表满足可变容量约定。
 * @param[in] id 可选调用期 ID 借用。
 * @param[in] name 可选调用期工具名借用。
 * @param[in] arguments_json 可选原始参数文本借用，NULL 使用 {}。
 * @return 参数或列表追加结果，输入拥有权保持，诊断由调用者释放。
 */
cc_result_t cc_llm_response_add_tool_call(
    cc_llm_response_t *response,
    const char *id,
    const char *name,
    const char *arguments_json
);

/**
 * @brief 向响应资源数组追加独立副本。
 * @param[in,out] response 必填独占响应，资源数组满足可变容量约定。
 * @param[in] artifact 必填调用期资源借用，可来自同列表现有项。
 * @return 参数或媒体追加结果，其他响应字段保持，诊断由调用者释放。
 */
cc_result_t cc_llm_response_add_artifact(
    cc_llm_response_t *response,
    const cc_media_artifact_t *artifact
);

#ifdef __cplusplus
}
#endif

#endif
