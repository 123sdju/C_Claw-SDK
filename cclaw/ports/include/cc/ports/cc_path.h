



#ifndef CC_PATH_H
#define CC_PATH_H

#include "cc/core/cc_result.h"

/*
 * 拼接 base 和 child 路径。
 *
 * 返回字符串由调用方 free()。该函数只负责平台分隔符拼接，不等同于安全校验；写文件
 * 前仍需要 canonical + within 检查。
 */
char *cc_path_join(const char *base, const char *child);

/*
 * 返回路径的规范化形式。
 *
 * 返回字符串由调用方 free()。平台实现应尽量解析 "."、".." 和符号链接；对于不存在
 * 的目标，写入路径需要先 canonical parent dir，再检查边界。
 */
char *cc_path_canonical(const char *path);

/*
 * 判断 path 是否位于 base_dir 内。
 *
 * 返回 1 表示在工作区边界内，0 表示不在或无法确认。该函数是文件工具防止路径穿越、
 * symlink 和 prefix 绕过的核心安全原语。
 */
int cc_path_is_within(const char *base_dir, const char *path);

/* 返回路径的父目录字符串；返回值由调用方 free()。 */
char *cc_path_dirname(const char *path);

/* 查询路径是否存在；返回 1/0，不提供详细错误信息。 */
int cc_path_exists(const char *path);

#endif
