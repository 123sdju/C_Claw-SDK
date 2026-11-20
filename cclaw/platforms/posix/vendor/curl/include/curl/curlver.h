#ifndef CURLINC_CURLVER_H
#define CURLINC_CURLVER_H
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

/* libcurl 头文件版本信息，发布流程生成，自 7.11.2 起独立为此头。
 * 这些宏描述本份头文件，实际链接库版本通过 curl_version_info 查询。 */

/* 全局包版权说明字符串。 */
#define LIBCURL_COPYRIGHT "Daniel Stenberg, <daniel@haxx.se>."

/* 本份头文件来源的 libcurl 包版本。 */
#define LIBCURL_VERSION "8.21.0-DEV"

/* 拆分的主版本、次版本和补丁版本常量。 */
#define LIBCURL_VERSION_MAJOR 8
#define LIBCURL_VERSION_MINOR 21
#define LIBCURL_VERSION_PATCH 0
/* 编译期版本用 0xXXYYZZ 的24位整数表示，三段各占8位，不含预发布后缀。
 * 例如1.2.0为0x010200，9.11.7为0x090b07，可直接比较大小。
 * 配置工具读取完整十六进制字面量，因此此处直接保留生成值。 */
#define LIBCURL_VERSION_NUM 0x081500

/* 完整源码包日期由打包流程填入，正式包采用 YYYY-MM-DD。
 * 此源码快照保留未发布标记，版本控制中的该占位值不代表实际链接库构建日期。 */
#define LIBCURL_TIMESTAMP "[unreleased]"

/**
 * @brief 将三段编译期版本打包为可比较的24位整数表达式。
 * @param[in] x 主版本整数，各段输入范围为0至255。
 * @param[in] y 次版本整数。
 * @param[in] z 补丁版本整数。
 * @return 按主、次、补丁各8位组合的版本值，各实参在表达式中出现一次。
 */
#define CURL_VERSION_BITS(x, y, z) ((x) << 16 | (y) << 8 | (z))
/**
 * @brief 判断本份头文件版本是否达到指定三段版本。
 * @param[in] x 主版本整数，各段范围为0至255。
 * @param[in] y 次版本整数。
 * @param[in] z 补丁版本整数。
 * @return 编译期头版本大于等于输入版本时为真。
 * @note 实际链接库能力仍须按运行库版本及构建能力判断。
 */
#define CURL_AT_LEAST_VERSION(x, y, z) \
  (LIBCURL_VERSION_NUM >= CURL_VERSION_BITS(x, y, z))

#endif /* 结束 CURLINC_CURLVER_H 包含保护。 */
