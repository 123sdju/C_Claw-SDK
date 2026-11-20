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
#include "curl.h"  /* 提供 CURL_EXTERN 声明修饰。 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 在受支持编译器上附加临时 printf 参数检查属性。
 * @param[in] fmt 格式串在函数参数中的一基位置。
 * @param[in] arg 首个可变实参位置，va_list 形式使用零。
 * @return 属性声明片段，编译器不支持时为空，文件末尾撤销此宏。
 */
#ifndef CURL_TEMP_PRINTF
#if (defined(__GNUC__) || defined(__clang__) ||                         \
  defined(__IAR_SYSTEMS_ICC__)) &&                                      \
  defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L) &&         \
  !defined(CURL_NO_FMT_CHECKS)
#if defined(__MINGW32__) && !defined(__clang__)
#ifdef __MINGW_PRINTF_FORMAT  /* mingw-w64 3.0.0 起由 stdio.h 提供格式约定。 */
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
 * @brief 按 libcurl 格式规则向 stdout 输出。
 * @param[in] format 调用期稳定的 NUL 格式串借用。
 * @param[in] ... 与转换、宽度和精度匹配的实参借用。
 * @return 实际打印字符数，流错误依链接库语义报告。
 * @note 调用者协调共享流输出顺序，输入只在同步调用期间借用。
 */
CURL_EXTERN int curl_mprintf(const char *format, ...)
  CURL_TEMP_PRINTF(1, 2);
/**
 * @brief 按 libcurl 格式规则写入已打开文件流。
 * @param[in,out] fd 存活 FILE 借用，最终关闭由 owner 负责。
 * @param[in] format NUL 格式串借用。
 * @param[in] ... 与格式匹配的实参借用。
 * @return 实际打印字符数，流失败按链接库报告，可能已输出前缀。
 * @note 同一流的并发顺序由调用者协调。
 */
CURL_EXTERN int curl_mfprintf(FILE *fd, const char *format, ...)
  CURL_TEMP_PRINTF(2, 3);
/**
 * @brief 将完整格式化文本及 NUL 写入调用者缓冲。
 * @param[out] buffer 容量足以覆盖完整输出和 NUL 的独占可写数组。
 * @param[in] format 调用期 NUL 格式串借用。
 * @param[in] ... 类型匹配的实参借用。
 * @return 实际输出字符数，不计 NUL，完整容量由调用者保证。
 */
CURL_EXTERN int curl_msprintf(char *buffer, const char *format, ...)
  CURL_TEMP_PRINTF(2, 3);
/**
 * @brief 在总容量内写入格式化文本并预留终止位置。
 * @param[out] buffer 覆盖 maxlength 字节的独占可写缓冲。
 * @param[in] maxlength 含 NUL 的总写入容量，非零时预留终止位置。
 * @param[in] format NUL 格式串借用。
 * @param[in] ... 匹配格式的实参借用。
 * @return 实际写入字符数，不计 NUL，按 libcurl 家族的截断长度语义解释。
 */
CURL_EXTERN int curl_msnprintf(char *buffer, size_t maxlength,
                               const char *format, ...)
  CURL_TEMP_PRINTF(3, 4);
/**
 * @brief 使用 va_list 向 stdout 输出。
 * @param[in] format NUL 格式串借用。
 * @param[in,out] args 已初始化参数遍历状态，调用后按消费处理。
 * @return 实际打印字符数，错误依链接库规则。
 * @note 调用者负责 va_end，重用前先 va_copy，并协调共享流。
 */
CURL_EXTERN int curl_mvprintf(const char *format, va_list args)
  CURL_TEMP_PRINTF(1, 0);
/**
 * @brief 将 va_list 格式化输出到指定流。
 * @param[in,out] fd 调用期 FILE 借用，owner 负责关闭及共享访问顺序。
 * @param[in] format NUL 格式串借用。
 * @param[in,out] args 已初始化遍历状态，调用者在使用后 va_end。
 * @return 实际输出字符数，流失败按运行库语义报告。
 */
CURL_EXTERN int curl_mvfprintf(FILE *fd, const char *format, va_list args)
  CURL_TEMP_PRINTF(2, 0);
/**
 * @brief 将 va_list 格式化为完整缓冲文本。
 * @param[out] buffer 调用者拥有的足量可写缓冲，含完整输出及 NUL。
 * @param[in] format NUL 格式串借用。
 * @param[in,out] args 已初始化遍历状态，调用者负责 va_end，重用预先 va_copy。
 * @return 实际输出字符数，不计 NUL，无容量检查参数。
 */
CURL_EXTERN int curl_mvsprintf(char *buffer, const char *format, va_list args)
  CURL_TEMP_PRINTF(2, 0);
/**
 * @brief 在含 NUL 的总容量内格式化 va_list。
 * @param[out] buffer 覆盖 maxlength 字节的独占可写缓冲。
 * @param[in] maxlength 最大写入容量，非零时保留 NUL。
 * @param[in] format 调用期 NUL 格式串借用。
 * @param[in,out] args 已初始化遍历状态，由调用者结束生命周期。
 * @return 实际写入字符数，不计 NUL，截断语义沿 libcurl 规则。
 */
CURL_EXTERN int curl_mvsnprintf(char *buffer, size_t maxlength,
                                const char *format, va_list args)
  CURL_TEMP_PRINTF(3, 0);
/**
 * @brief 为格式化文本分配独立字符串。
 * @param[in] format 调用期 NUL 格式串借用。
 * @param[in] ... 与格式匹配的实参借用。
 * @return 成功交付调用者 curl_free 的拥有文本，分配失败 NULL。
 */
CURL_EXTERN char *curl_maprintf(const char *format, ...)
  CURL_TEMP_PRINTF(1, 2);
/**
 * @brief 从 va_list 分配格式化文本。
 * @param[in] format 调用期 NUL 格式串借用。
 * @param[in,out] args 已初始化遍历状态，调用者负责 va_end。
 * @return 成功为接收者 curl_free 的独立拥有文本，失败 NULL。
 * @note 输入只借用本次同步调用，再次遍历由调用者预先 va_copy。
 */
CURL_EXTERN char *curl_mvaprintf(const char *format, va_list args)
  CURL_TEMP_PRINTF(1, 0);

#undef CURL_TEMP_PRINTF

#ifdef __cplusplus
} /* 结束 C 链接声明。 */
#endif

#endif /* 结束 CURLINC_MPRINTF_H 头文件保护。 */
