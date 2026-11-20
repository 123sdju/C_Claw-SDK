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

/* 发布流程生成的编译头版本信息，自 7.11.2 起独立成头。
 * 此处的 8.21.0-DEV 描述本份头文件，运行库版本与能力由实际链接库决定。 */

/* 包的版权说明字符串。 */
#define LIBCURL_COPYRIGHT "Daniel Stenberg, <daniel@haxx.se>."

/* 生成本份头的包版本，包含开发版后缀。 */
#define LIBCURL_VERSION "8.21.0-DEV"

/* 主、次及补丁版本分量。 */
#define LIBCURL_VERSION_MAJOR 8
#define LIBCURL_VERSION_MINOR 21
#define LIBCURL_VERSION_PATCH 0
/* 24 位可比较编译版本，三段各占 8 位，布局为 0xXXYYZZ，不编码预发布后缀。
 * 1.2.0 为 0x010200，9.11.7 为 0x090b07。configure 读取完整字面值，保留生成数值。 */
#define LIBCURL_VERSION_NUM 0x081500

/* 完整包日期由打包流程填入，格式为 YYYY-MM-DD，当前快照保留未发布占位。 */
#define LIBCURL_TIMESTAMP "[unreleased]"

/**
 * @brief 把三段版本组合成 24 位整数表达式。
 * @param[in] x 0 至 255 的主版本整数。
 * @param[in] y 0 至 255 的次版本整数。
 * @param[in] z 0 至 255 的补丁整数。
 * @return 各实参出现一次的打包值，无分配或共享状态。
 */
#define CURL_VERSION_BITS(x, y, z) ((x) << 16 | (y) << 8 | (z))
/**
 * @brief 检查本份编译头版本是否达到指定版本。
 * @param[in] x 主版本整数，各分量范围为 0 至 255。
 * @param[in] y 次版本整数。
 * @param[in] z 补丁整数。
 * @return 大于等于指定打包版本为真，运行库能力由运行时另行查询。
 */
#define CURL_AT_LEAST_VERSION(x, y, z) \
  (LIBCURL_VERSION_NUM >= CURL_VERSION_BITS(x, y, z))

#endif /* 结束 CURLINC_CURLVER_H 包含保护。 */
