

#ifndef CC_MEDIA_H
#define CC_MEDIA_H

#include "cc/core/cc_result.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 文本及媒体元数据的类型标记，本模块不执行文件读取、网络访问或媒体解码。 */
typedef enum cc_media_kind {
    CC_MEDIA_TEXT = 0,
    CC_MEDIA_IMAGE = 1,
    CC_MEDIA_AUDIO = 2,
    CC_MEDIA_VIDEO = 3,
    CC_MEDIA_FILE = 4
} cc_media_kind_t;

/** @brief 内容的输入/输出标记，JSON 序列化仅将准确 OUTPUT 值写成 output，其余写成 input。 */
typedef enum cc_content_part_direction {
    CC_CONTENT_PART_INPUT = 0,
    CC_CONTENT_PART_OUTPUT = 1
} cc_content_part_direction_t;

/** @brief 调用期间稳定借用的媒体策略，前缀数组和字符串由外层保管，结构无内部锁。 */
typedef struct cc_media_limits {
    size_t max_artifacts; /**< 列表媒体项预算，零表示禁止媒体项。 */
    size_t max_artifact_bytes; /**< 声明的单项字节上限，零表示不设此上限。 */
    size_t max_base64_bytes; /**< 编码字符串字节上限，按 strlen 计量，零表示不限。 */
    int allow_inline_base64; /**< 非零允许非空内联编码，不校验编码语法。 */
    const char **allowed_mime_prefixes; /**< 借用前缀数组，数量非零时须有效。 */
    size_t allowed_mime_prefix_count; /**< 零表示接受任意非空 MIME，空前缀可匹配全部非空 MIME。 */
} cc_media_limits_t;

/**
 * @brief 一项媒体元数据，path/uri/data_base64 可并存，本层不验证真实文件与声明尺寸一致。
 * @note 需要 cleanup 的对象独占各个非空字符串，字段不得共享同一拥有分配。
 * 只读调用也允许借用字符串视图，外层保持稳定且不对该视图执行 cleanup。
 * 结构不加锁，跨线程由 owner 深拷贝或完整转移，普通浅复制不能产生两个独立拥有者。
 */
typedef struct cc_media_artifact {
    char *id;
    cc_media_kind_t kind;
    char *mime;
    char *path;
    char *uri;
    char *data_base64;
    size_t bytes; /**< 声明的原始媒体字节数，JSON 以 double 表示可能损失大整数精度。 */
    int width; /**< 声明像素宽度，非正值序列化省略。 */
    int height; /**< 声明像素高度，非正值序列化省略。 */
    int duration_ms; /**< 声明时长，单位毫秒，非正值序列化省略。 */
    char *created_at;
    char *metadata;
} cc_media_artifact_t;

/**
 * @brief 媒体数组，修改入口要求 count<=capacity 且容量与拥有分配匹配。
 * @note 只读输入允许 count>capacity 的借用视图，items 必须覆盖真实 count 项。
 * cleanup 消费实际 count 项及数组，调用方须传递可释放的拥有数组和真实边界。
 */
typedef struct cc_media_artifact_list {
    cc_media_artifact_t *items;
    size_t count;
    size_t capacity;
} cc_media_artifact_list_t;

/** @brief 消息内容项，TEXT 使用 text，其他类型使用 artifact，cleanup 同时消费两类动态字段。 */
typedef struct cc_content_part {
    cc_media_kind_t kind;
    cc_content_part_direction_t direction;
    char *text;
    cc_media_artifact_t artifact;
} cc_content_part_t;

/** @brief 内容拥有数组，修改要求真实容量，读取可接收 items 覆盖 count 的借用视图。 */
typedef struct cc_content_parts {
    cc_content_part_t *items;
    size_t count;
    size_t capacity;
} cc_content_parts_t;

/**
 * @brief 将类型标记映射为协议名称。
 * @param[in] kind 类型值，未知值按 file 映射。
 * @return 静态字符串，调用者不得修改或释放。
 */
const char *cc_media_kind_string(cc_media_kind_t kind);

/**
 * @brief 按区分大小写的协议名称解析类型。
 * @param[in] kind 借用字符串，NULL 按 TEXT，未知非空值按 FILE。
 * @return 对应类型标记，空串同其他未知值为 FILE。
 */
cc_media_kind_t cc_media_kind_from_string(const char *kind);

/**
 * @brief 将策略初始化为零媒体项并禁止非空内联编码。
 * @param[out] limits 输出结构，可为 NULL。
 * @post 非空输出全部清零，不消费此前借用的前缀数组。
 */
void cc_media_limits_text_only(cc_media_limits_t *limits);

/**
 * @brief 初始化八项、单项十 MiB、编码二 MiB 的默认媒体策略。
 * @param[out] limits 输出结构，可为 NULL。
 * @post 清零后启用内联编码，MIME 前缀数量为零，旧借用指针不被释放。
 */
void cc_media_limits_default_multimodal(cc_media_limits_t *limits);

/**
 * @brief 初始化新媒体对象。
 * @param[out] artifact 新或已清理结构，可为 NULL。
 * @post 非空对象清零并设为 FILE，原有拥有字段须由调用者提前 cleanup。
 */
void cc_media_artifact_init(cc_media_artifact_t *artifact);

/**
 * @brief 消费七个可选拥有字符串并清零对象。
 * @param[in,out] artifact 独占拥有对象，可为 NULL。
 * @post 字段归零，结构存储仍由调用者保管，清理前结束全部借用。
 */
void cc_media_artifact_cleanup(cc_media_artifact_t *artifact);

/**
 * @brief 深拷贝媒体字段，自身复制保持原对象。
 * @param[in] src 非 NULL 稳定输入，可为只读借用视图。
 * @param[out] dst 非 NULL 新或已清理对象，允许与 src 完全相同。
 * @return 成功为 CC_OK，空指针为 INVALID_ARGUMENT，复制失败为 OUT_OF_MEMORY 且不同目标已清理。
 * @note 不同目标不能与源共享需要释放的存储，返回后独立拥有各字符串，无内部锁。
 */
cc_result_t cc_media_artifact_copy(
    const cc_media_artifact_t *src,
    cc_media_artifact_t *dst
);

/**
 * @brief 核对媒体项的声明 MIME、路径子串和资源策略。
 * @param[in] artifact 非 NULL 稳定元数据，TEXT 不作为媒体项接受。
 * @param[in] limits 可空策略，NULL 仅保留非空 MIME 与路径检查，非零前缀数须有数组。
 * @return 成功为 CC_OK，坏参数为 INVALID_ARGUMENT，策略不支持为 UNSUPPORTED，path 含任意 '..' 为 PERMISSION_DENIED。
 * @note 不访问文件、不解码 Base64、不检查定位字段是否存在或实际数据大小。
 */
cc_result_t cc_media_artifact_validate(
    const cc_media_artifact_t *artifact,
    const cc_media_limits_t *limits
);

/**
 * @brief 初始化新媒体数组包装。
 * @param[out] list 新或已清理对象，可为 NULL。
 * @post 非空结构清零，此前拥有数组须先 cleanup。
 */
void cc_media_artifact_list_init(cc_media_artifact_list_t *list);

/**
 * @brief 消费媒体数组各有效项及数组分配。
 * @param[in,out] list 独占拥有数组，可为 NULL，count 必须处于真实数组边界内。
 * @post 非空包装清零，结构存储保留；items 为 NULL 时直接清空计数，不访问项。
 */
void cc_media_artifact_list_cleanup(cc_media_artifact_list_t *list);

/**
 * @brief 在扩容前取得媒体副本再追加，支持输入来自本列表元素。
 * @param[in,out] list 已初始化拥有数组，真实 count/capacity 与分配一致，独占修改。
 * @param[in] artifact 非 NULL 稳定输入，可借自 list 内项，成功后仍由原 owner 管理。
 * @return 成功为 CC_OK，坏参数/数组状态为 INVALID_ARGUMENT，容量耗尽为 LIMIT_EXCEEDED，分配失败为 OUT_OF_MEMORY。
 * @note 失败保留原有效项和 count，成功扩容会使旧 items/元素地址失效。
 */
cc_result_t cc_media_artifact_list_append(
    cc_media_artifact_list_t *list,
    const cc_media_artifact_t *artifact
);

/**
 * @brief 深拷贝媒体列表，完全自身复制为空操作。
 * @param[in] src 非 NULL 稳定列表，允许只读 count>capacity 视图，items 覆盖 count。
 * @param[out] dst 非 NULL 新或已清理拥有包装，可与 src 完全相同。
 * @return 成功为 CC_OK，参数/源形状错误为 INVALID_ARGUMENT，复制/增长失败返回相应结果并清空不同目标。
 */
cc_result_t cc_media_artifact_list_copy(
    const cc_media_artifact_list_t *src,
    cc_media_artifact_list_t *dst
);

/**
 * @brief 检查媒体项数量及逐项策略。
 * @param[in] list 非 NULL 稳定列表，items 覆盖 count，只读视图不依赖 capacity。
 * @param[in] limits 可为 NULL，非空时 max_artifacts 为零会拒绝任意媒体项。
 * @return 成功为 CC_OK，形状/策略参数错误为 INVALID_ARGUMENT，首个数量或媒体错误原样交付。
 */
cc_result_t cc_media_artifact_list_validate(
    const cc_media_artifact_list_t *list,
    const cc_media_limits_t *limits
);

/**
 * @brief 完整序列化媒体记录数组，任何应有字段创建失败时撤销输出。
 * @param[in] list 借用的稳定列表，非空列表须有有效 items。
 * @param[out] out_json 输出独占字符串，调用者 free，失败为 NULL。
 * @return 成功为 CC_OK，参数错误为 INVALID_ARGUMENT，bytes 的 double 舍入越过 size_t 上界为 LIMIT_EXCEEDED，其余构造失败为 OUT_OF_MEMORY。
 * @note 无内部锁，源字段保持稳定，输出槽不能覆盖源字段，结果动态诊断由接收者释放。
 */
cc_result_t cc_media_artifact_list_to_json(
    const cc_media_artifact_list_t *list,
    char **out_json
);

/**
 * @brief 完整解析媒体数组，任一损坏项或错类型字段使整个输出失败。
 * @param[in] json 稳定 NUL 文本，NULL/空串为空列表，非空文本须完整消费且不含解码 NUL。
 * @param[out] out_list 非 NULL 新或已清理拥有包装，先初始化，失败清空所有已解析项。
 * @return 成功为 CC_OK，空输出为 INVALID_ARGUMENT，schema/尾部/NUL 及通用解析失败为 JSON，字段复制/列表分配失败为 OUT_OF_MEMORY。
 * @note 通用解析入口把解析 OOM 也归为 JSON。已知字段重复含大小写变体会拒绝，缺可选字段/未知 kind 默认保留，有限正 bytes 小数截断，int 字段截断并饱和。
 */
cc_result_t cc_media_artifact_list_from_json(
    const char *json,
    cc_media_artifact_list_t *out_list
);

/**
 * @brief 拼接媒体声明的可读摘要。
 * @param[in] list 非 NULL 稳定列表，items 覆盖 count，只读视图不依赖 capacity。
 * @param[out] out_summary 非 NULL 独立输出槽，先置 NULL，成功交付 free 可释放字符串。
 * @return 成功为 CC_OK，参数/形状错误为 INVALID_ARGUMENT，构造失败交付首个拥有错误结果。
 * @note 输出槽不得覆盖输入字段，源字符串在同步构造期间保持稳定。
 */
cc_result_t cc_media_artifact_list_summarize(
    const cc_media_artifact_list_t *list,
    char **out_summary
);

/**
 * @brief 消费 text 与嵌入媒体的全部拥有字段。
 * @param[in,out] part 独占拥有内容项，可为 NULL。
 * @post 非空对象清零，结构存储保留，清理前结束字段借用。
 */
void cc_content_part_cleanup(cc_content_part_t *part);

/**
 * @brief 复制内容项的标记、可选文本与非 TEXT 媒体，自身复制保持原对象。
 * @param[in] src 非 NULL 稳定内容项。
 * @param[out] dst 非 NULL 新或已清理目标，可与 src 完全相同。
 * @return 成功为 CC_OK，空参数为 INVALID_ARGUMENT，分配失败返回 OUT_OF_MEMORY 且不同目标无半成品。
 * @note TEXT 项中未使用的 artifact 不复制，目标与源分别拥有成功复制的字段。
 */
cc_result_t cc_content_part_copy(
    const cc_content_part_t *src,
    cc_content_part_t *dst
);

/**
 * @brief 初始化新内容列表。
 * @param[out] parts 新或已清理包装，可为 NULL。
 * @post 非空结构清零，不消费此前资源。
 */
void cc_content_parts_init(cc_content_parts_t *parts);

/**
 * @brief 消费内容数组各项及数组分配。
 * @param[in,out] parts 独占拥有列表，可为 NULL，count 须覆盖真实有效项。
 * @post 包装清零，结构存储保留，items 为 NULL 时跳过项访问。
 */
void cc_content_parts_cleanup(cc_content_parts_t *parts);

/**
 * @brief 先复制文本再扩容追加，避免借用当前数组存储跨越搬迁。
 * @param[in,out] parts 已初始化拥有列表，独占修改，count/capacity 与真实分配匹配。
 * @param[in] text 同步借用文本，可为 NULL，按空串复制。
 * @param[in] direction 原样保留的方向标记。
 * @return 成功为 CC_OK，参数/形状错误为 INVALID_ARGUMENT，容量耗尽为 LIMIT_EXCEEDED，分配失败为 OUT_OF_MEMORY。
 * @note 失败保留旧项/count，成功扩容使旧元素地址失效。
 */
cc_result_t cc_content_parts_append_text(
    cc_content_parts_t *parts,
    const char *text,
    cc_content_part_direction_t direction
);

/**
 * @brief 先深拷贝媒体再扩容追加，支持借用本列表内的嵌入媒体。
 * @param[in,out] parts 已初始化、独占修改的拥有列表。
 * @param[in] artifact 非 NULL 稳定媒体输入，成功后原 owner 仍拥有原字段。
 * @param[in] direction 原样保留的方向标记。
 * @return 成功为 CC_OK，坏参数/形状、容量耗尽或 OOM 返回相应错误，旧项/count 保留。
 */
cc_result_t cc_content_parts_append_artifact(
    cc_content_parts_t *parts,
    const cc_media_artifact_t *artifact,
    cc_content_part_direction_t direction
);

/**
 * @brief 逐项深拷贝内容列表，自身复制保持原对象。
 * @param[in] src 非 NULL 稳定只读输入，items 覆盖 count，capacity 可小于 count。
 * @param[out] dst 非 NULL 新或已清理包装，可与 src 完全相同。
 * @return 成功为 CC_OK，源形状/空参数为 INVALID_ARGUMENT，复制失败返回原错误并清空不同目标。
 */
cc_result_t cc_content_parts_copy(
    const cc_content_parts_t *src,
    cc_content_parts_t *dst
);

/**
 * @brief 收集非 TEXT 媒体副本并检查总数和各项策略。
 * @param[in] parts 非 NULL 稳定只读列表，items 覆盖 count，不要求读取视图具有可写容量。
 * @param[in] limits 可空媒体策略。
 * @return 成功为 CC_OK，参数/分配/数量或单项策略错误原样交付，临时副本内部释放。
 * @note 文本不计媒体数量，验证会临时分配深拷贝，无内部锁。
 */
cc_result_t cc_content_parts_validate(
    const cc_content_parts_t *parts,
    const cc_media_limits_t *limits
);

/**
 * @brief 完整序列化文本及媒体内容，任一字段或挂接失败时撤销输出。
 * @param[in] parts 借用的稳定列表，非空列表须有有效 items。
 * @param[out] out_json 输出独占字符串，调用者 free，失败为 NULL。
 * @return 成功为 CC_OK，参数错误为 INVALID_ARGUMENT，媒体 bytes 数值表示越界为 LIMIT_EXCEEDED，构造失败为 OUT_OF_MEMORY。
 * @note 不修改源内容，输出槽不能覆盖源字段，并发写入须由调用者排除。
 */
cc_result_t cc_content_parts_to_json(
    const cc_content_parts_t *parts,
    char **out_json
);

/**
 * @brief 完整解析内容数组，任一坏项、重复已知字段或错类型字段使整列表失败。
 * @param[in] json 稳定 NUL 文本，NULL/空串为空列表，拒绝尾垃圾和解码 NUL。
 * @param[out] out_parts 非 NULL 新或已清理输出，先初始化，失败撤销所有已追加项。
 * @return 成功为 CC_OK，空输出为 INVALID_ARGUMENT，schema/通用解析失败为 JSON，字段或列表分配失败为 OUT_OF_MEMORY，诊断含项索引/字段。
 * @note 通用解析入口无法单独区分解析 OOM，仍返回 JSON，失败输出全部撤销。
 * @note 保留单个大小写键、缺可选字段、未知类型/方向默认和旧式平铺 artifact 字段。
 */
cc_result_t cc_content_parts_from_json(
    const char *json,
    cc_content_parts_t *out_parts
);

/**
 * @brief 保留文本并为非 TEXT 项拼接媒体占位符，项间加换行。
 * @param[in] parts 非 NULL 稳定只读列表，items 覆盖 count。
 * @param[out] out_summary 非 NULL 独立输出槽，先置 NULL，成功接管 free 可释放字符串。
 * @return 成功为 CC_OK，空参数/形状错误为 INVALID_ARGUMENT，构造失败返回首个错误并清理缓冲。
 * @note 输出槽不得覆盖输入字段，源字段借用覆盖同步构造，无内部锁。
 */
cc_result_t cc_content_parts_text_summary(
    const cc_content_parts_t *parts,
    char **out_summary
);

#ifdef __cplusplus
}
#endif

#endif
