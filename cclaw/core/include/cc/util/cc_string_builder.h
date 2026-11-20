



#ifndef CC_STRING_BUILDER_H
#define CC_STRING_BUILDER_H

#include "cc/core/cc_result.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 可增长字符串构造器。
 *
 * data 由 builder 拥有，length 不含 NUL，capacity 是已分配容量。该结构不加锁，只能由
 * 一个线程写；用于 JSON/prompt/摘要等需要多次 append 的路径。
 */
typedef struct cc_string_builder {
    char *data;

    size_t length;
    size_t capacity;
    /* 首次失败后保持错误，防止忽略 append 返回值的调用方拿到半截 JSON/协议数据。 */
    cc_error_code_t error_code;

} cc_string_builder_t;

/**
 * @brief 初始化独占构造器并申请空字符串缓冲。
 * @pre sb 为新对象或已清理对象，所有操作由 owner 串行化。
 * @param[out] sb 必填输出，分配失败后仍可 deinit。
 * @return 成功为 CC_OK，NULL 为参数错误，分配失败为 OOM，结果消息由调用者释放。
 */
cc_result_t cc_string_builder_init(cc_string_builder_t *sb);

/**
 * @brief 追加完整字符串，支持输入借用当前构造器文本及其子串。
 * @param[in,out] sb 必填有效构造器，扩容可能结束当前 data 借用。
 * @param[in] text 调用期间有效的 NUL 结尾文本，NULL 为空操作并返回 CC_OK。
 * @return 参数、未初始化、sticky 或容量错误分别交付拥有结果，失败保留旧文本。
 */
cc_result_t cc_string_builder_append(cc_string_builder_t *sb, const char *text);

/**
 * @brief 按 printf 规则两次格式化并追加文本。
 * @param[in,out] sb 必填独占构造器，容量及格式化失败进入 sticky 状态。
 * @param[in] fmt 必填格式串，覆盖两次读取及中间扩容，遵守 vsnprintf 不重叠规则。
 * @param[in] ... 对应格式参数，引用内容须在两次格式化期间存活且稳定。
 * @return 成功为 CC_OK，参数、容量或格式化失败为拥有错误结果，调用者消费动态消息。
 */
cc_result_t cc_string_builder_appendf(cc_string_builder_t *sb, const char *fmt, ...);

/**
 * @brief 追加一个字节并维护终止 NUL。
 * @param[in,out] sb 必填独占有效构造器。
 * @param[in] c 输入字节，NUL 也计入 length。
 * @return 成功为 CC_OK，错误返回拥有结果，容量错误保留旧文本并记录 sticky 码。
 */
cc_result_t cc_string_builder_append_char(cc_string_builder_t *sb, char c);

/**
 * @brief 交出缓冲拥有权，失败构造器则释放残留并清零。
 * @param[in,out] sb 可选独占对象，成功交接后再次追加前须 init。
 * @return 拥有字符串由调用者 free，NULL 对应空对象或失败，旧内部借用由新 owner 管理。
 */
char *cc_string_builder_take(cc_string_builder_t *sb);

/**
 * @brief 释放缓冲并清零对象。
 * @param[in,out] sb 可选已初始化或零状态对象，NULL 为空操作。
 * @post 字符串借用结束，结构体存储仍由调用者持有，可重复清理或重新 init。
 */
void cc_string_builder_deinit(cc_string_builder_t *sb);

/**
 * @brief 清空内容和 sticky 错误，保留现有容量。
 * @param[in,out] sb 可选独占对象，NULL 为空操作。
 * @post length 为零且 error_code 为 CC_OK，无缓冲对象仍需 init 后才能追加。
 */
void cc_string_builder_clear(cc_string_builder_t *sb);

/**
 * @brief 返回成功构造器的字符串视图，失败或空对象返回静态空串。
 * @param[in] sb 可选稳定借用，owner 排除并发写入和销毁。
 * @return 借用只读字符串，禁止 free，后续修改、扩容或资源交接会改变内容或结束寿命。
 */
const char *cc_string_builder_cstr(const cc_string_builder_t *sb);

/**
 * @brief 读取 sticky 错误状态。
 * @param[in] sb 可选稳定借用构造器。
 * @return NULL 返回参数错误，其他返回 error_code，clear、take 失败收尾和 deinit 会清除错误。
 */
cc_error_code_t cc_string_builder_error(const cc_string_builder_t *sb);

#ifdef __cplusplus
}
#endif

#endif
