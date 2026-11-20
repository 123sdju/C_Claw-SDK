



#ifndef CC_PATH_H
#define CC_PATH_H

#include "cc/core/cc_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 按当前平台规则复制或拼接路径字符串。
 * @param[in] base 调用期借用的基路径，NULL 时通常复制 child。
 * @param[in] child 调用期借用的子路径，NULL 时通常复制 base。
 * @return 成功取得调用方 free 的新字符串，双 NULL、分配或可检测的长度超限返回 NULL。
 * @note POSIX 将空参数按缺省处理，绝对 child 覆盖 base；Windows、ESP32、FreeRTOS
 *       保留其分隔符拼接规则，非 NULL 空字符串及绝对 child 仍可参与拼接。
 *       函数不固定文件系统对象或执行根约束 I/O，路径编码按具体平台解释。
 */
char *cc_path_join(const char *base, const char *child);

/**
 * @brief 按当前平台实现取得规范化路径字符串。
 * @param[in] path 调用期借用的 NUL 结尾路径，NULL 返回 NULL。
 * @return 成功返回调用方 free 的字符串，参数、容量或分配失败可返回 NULL。
 * @note POSIX 优先 realpath 目标或直接父目录，其余词法绝对化；Windows 通过 ANSI
 *       GetFullPathName 词法绝对化，原生转换失败时沿既有规则复制原输入；ESP32 以斜杠为根
 *       词法折叠，FreeRTOS 保留相对或绝对形式。仅 POSIX 成功 realpath 的部分解析链接，
 *       多级不存在路径的祖先链接及目录并发变化仍需后端根约束 I/O 处理。
 */
char *cc_path_canonical(const char *path);

/**
 * @brief 以平台路径规则判断相同根或完整组件前缀关系。
 * @param[in] base_dir 借用根字符串，NULL 返回 0。
 * @param[in] path 借用目标字符串，NULL 返回 0。
 * @return 满足该平台字符串边界判断返回 1，其余或规范化分配失败返回 0。
 * @note POSIX/Windows/ESP32 先规范化再比较，Windows 大小写不敏感，FreeRTOS 当前直接
 *       比较输入，要求调用者先规范化。该查询不持有目标对象，不能替代实际根约束访问。
 */
int cc_path_is_within(const char *base_dir, const char *path);

/**
 * @brief 按平台分隔符词法取得父目录。
 * @param[in] path 借用的 NUL 结尾路径，NULL 返回 NULL。
 * @return 返回调用方 free 的字符串，无分隔符通常返回点，分配失败返回 NULL。
 * @note POSIX 拒绝空输入并剥离尾斜杠，Windows 处理两种分隔符及盘根，ESP32/FreeRTOS
 *       按最后一个斜杠截取，空输入或末尾分隔符的具体结果沿后端规则。
 */
char *cc_path_dirname(const char *path);

/**
 * @brief 查询平台是否能确认路径存在。
 * @param[in] path 借用路径，NULL 返回 0。
 * @return 可确认存在返回 1，其余返回 0，不提供详细错误，FreeRTOS 默认实现始终返回 0。
 */
int cc_path_exists(const char *path);

#ifdef __cplusplus
}
#endif

#endif
