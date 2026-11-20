#ifndef CURLINC_MPRINTF_H
#define CURLINC_MPRINTF_H
/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * SPDX-License-Identifier: curl
 *
 ***************************************************************************/

#include <stdarg.h>
#include <stdio.h> /* 提供 FILE 类型。 */
#include "curl.h"  /* 提供 CURL_EXTERN。 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @def CURL_TEMP_PRINTF(fmt, arg)
 * @brief 在支持的C编译器上声明printf式格式检查属性，其他分支展开为空。
 * @param[in] fmt 格式串形参的一基序号。
 * @param[in] arg 首个可变参数的一基序号，va_list版本使用零。
 * @return 属性记号或空展开，运行期不求值这两个序号。
 * @note 编译器与C标准版本、MinGW格式约定及CURL_NO_FMT_CHECKS共同选择分支。
 */
#ifndef CURL_TEMP_PRINTF
#if (defined(__GNUC__) || defined(__clang__) ||                         \
  defined(__IAR_SYSTEMS_ICC__)) &&                                      \
  defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L) &&         \
  !defined(CURL_NO_FMT_CHECKS)
#if defined(__MINGW32__) && !defined(__clang__)
#ifdef __MINGW_PRINTF_FORMAT  /* mingw-w64 3.0.0起由stdio.h提供格式检查约定。 */
#define CURL_TEMP_PRINTF(fmt, arg) \
  __attribute__((format(__MINGW_PRINTF_FORMAT, fmt, arg)))
#else
#define CURL_TEMP_PRINTF(fmt, arg)
#endif
#else
#define CURL_TEMP_PRINTF(fmt, arg) \
  __attribute__((format(printf, fmt, arg)))
#endif
#else
#define CURL_TEMP_PRINTF(fmt, arg)
#endif
#endif

/**
 * @brief 按 libcurl 格式规则把可变参数写入 stdout。
 * @param[in] format 调用期间稳定的 NUL 结尾格式串借用。
 * @param[in] ... 与转换、宽度及精度匹配的实参借用。
 * @return 实际打印字符数，输出失败的负值规则依所链接库版本。
 * @note stdout 的并发输出交错由调用者协调，格式规则见 libcurl mprintf 文档。
 */
CURL_EXTERN int curl_mprintf(const char *format, ...)
  CURL_TEMP_PRINTF(1, 2);
/**
 * @brief 按 libcurl 格式规则向指定已打开流输出可变参数。
 * @param[in,out] fd 调用期间有效的文件流借用，拥有者负责最终关闭。
 * @param[in] format NUL 结尾格式串借用。
 * @param[in] ... 与格式匹配的实参借用。
 * @return 实际打印字符数，流错误报告按所链接库版本解释。
 * @note 操作可能已经输出前缀，调用者协调同一流的访问。
 */
CURL_EXTERN int curl_mfprintf(FILE *fd, const char *format, ...)
  CURL_TEMP_PRINTF(2, 3);
/**
 * @brief 将格式化文本及 NUL 写入调用者缓冲。
 * @param[out] buffer 调用者拥有的可写缓冲，必须覆盖完整格式化输出与 NUL。
 * @param[in] format NUL 结尾格式串借用，相关输入与输出保持有效且满足格式要求。
 * @param[in] ... 与格式匹配的实参。
 * @return 实际输出字符数，终止 NUL 不计入。
 * @note 此接口没有容量参数，输出缓冲的足够容量由调用者保证。
 */
CURL_EXTERN int curl_msprintf(char *buffer, const char *format, ...)
  CURL_TEMP_PRINTF(2, 3);
/**
 * @brief 在指定总容量内格式化文本，容量包含终止 NUL。
 * @param[out] buffer 覆盖 maxlength 字节的调用者可写缓冲。
 * @param[in] maxlength 最多写入的总字节数，非零时为 NUL 留出位置。
 * @param[in] format NUL 结尾格式串借用。
 * @param[in] ... 与格式匹配的实参。
 * @return 实际写入字符数，终止 NUL 不计入，截断时按库的实际写入语义解释。
 * @note 返回长度语义取自 libcurl mprintf 家族，调用者按有效写入范围处理缓冲。
 */
CURL_EXTERN int curl_msnprintf(char *buffer, size_t maxlength,
                               const char *format, ...)
  CURL_TEMP_PRINTF(3, 4);
/**
 * @brief 把 va_list 按 libcurl 格式规则输出到 stdout。
 * @param[in] format NUL 结尾格式串借用。
 * @param[in,out] args 已初始化的参数遍历状态，调用后按已消费处理。
 * @return 实际打印字符数，流错误按所链接版本解释。
 * @note 调用者负责 va_end，再次遍历需预先 va_copy，并协调共享流。
 */
CURL_EXTERN int curl_mvprintf(const char *format, va_list args)
  CURL_TEMP_PRINTF(1, 0);
/**
 * @brief 把 va_list 的格式化结果写入指定流。
 * @param[in,out] fd 存活的文件流借用，流拥有者负责关闭和并发协调。
 * @param[in] format NUL 结尾格式串借用。
 * @param[in,out] args 已初始化的参数遍历状态，调用后由调用者 va_end。
 * @return 实际打印字符数，流失败按所链接库版本解释。
 */
CURL_EXTERN int curl_mvfprintf(FILE *fd, const char *format, va_list args)
  CURL_TEMP_PRINTF(2, 0);
/**
 * @brief 把 va_list 格式化为调用者拥有的完整 NUL 结尾文本。
 * @param[out] buffer 容量足够容纳完整输出及 NUL 的可写缓冲。
 * @param[in] format NUL 结尾格式串借用。
 * @param[in,out] args 已初始化的参数遍历状态，调用者结束其生命周期。
 * @return 实际输出字符数，不计终止 NUL。
 * @note 完整容量由调用者保证，重用参数列表须在调用前 va_copy。
 */
CURL_EXTERN int curl_mvsprintf(char *buffer, const char *format, va_list args)
  CURL_TEMP_PRINTF(2, 0);
/**
 * @brief 在总容量内把 va_list 格式化为有界文本。
 * @param[out] buffer 调用者拥有的可写缓冲，覆盖 maxlength 字节。
 * @param[in] maxlength 总写入容量，包含 NUL，非零时保留终止位置。
 * @param[in] format NUL 结尾格式串借用。
 * @param[in,out] args 已初始化的参数遍历状态，调用者负责 va_end。
 * @return 实际写入字符数，不计终止 NUL，长度语义沿 libcurl 家族。
 */
CURL_EXTERN int curl_mvsnprintf(char *buffer, size_t maxlength,
                                const char *format, va_list args)
  CURL_TEMP_PRINTF(3, 0);
/**
 * @brief 为格式化文本分配独立 NUL 结尾结果。
 * @param[in] format NUL 结尾格式串借用。
 * @param[in] ... 与格式匹配的实参借用。
 * @return 成功交付调用者拥有的字符串，失败返回 NULL，由接收者 curl_free。
 */
CURL_EXTERN char *curl_maprintf(const char *format, ...)
  CURL_TEMP_PRINTF(1, 2);
/**
 * @brief 根据 va_list 分配并返回独立格式化字符串。
 * @param[in] format NUL 结尾格式串借用。
 * @param[in,out] args 已初始化的参数遍历状态，调用者负责 va_end。
 * @return 成功为接收者 curl_free 的拥有字符串，失败为 NULL。
 * @note 参数借用仅覆盖同步调用，再次遍历前由调用者准备 va_copy。
 */
CURL_EXTERN char *curl_mvaprintf(const char *format, va_list args)
  CURL_TEMP_PRINTF(1, 0);

#undef CURL_TEMP_PRINTF

#ifdef __cplusplus
} /* 结束 C 链接声明。 */
#endif

#endif /* 结束 CURLINC_MPRINTF_H 包含保护。 */
