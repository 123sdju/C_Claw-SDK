#ifndef CURLINC_HEADER_H
#define CURLINC_HEADER_H
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

#ifdef __cplusplus
extern "C" {
#endif

struct curl_header {
  char *name;    /* 库拥有的名称借用，大小写可与查询名不同。 */
  char *value;
  size_t amount; /* 查询范围内同名头的总数。 */
  size_t index;  /* 本项的零起点序号。 */
  unsigned int origin; /* 实际来源位。 */
  void *anchor; /* 库内部锚点，调用者保持原值。 */
};

/* 查询时可组合筛选的 HTTP 响应来源。 */
#define CURLH_HEADER    (1 << 0) /* 普通响应头。 */
#define CURLH_TRAILER   (1 << 1) /* 正文后的尾部头。 */
#define CURLH_CONNECT   (1 << 2) /* 代理 CONNECT 响应头。 */
#define CURLH_1XX       (1 << 3) /* 中间 1xx 响应头。 */
#define CURLH_PSEUDO    (1 << 4) /* HTTP/2、HTTP/3 伪头。 */

typedef enum {
  CURLHE_OK,
  CURLHE_BADINDEX,      /* 同名头存在而序号无对应项。 */
  CURLHE_MISSING,       /* 缺少指定名称。 */
  CURLHE_NOHEADERS,     /* 尚未收到响应头。 */
  CURLHE_NOREQUEST,     /* 请求序号无对应项。 */
  CURLHE_OUT_OF_MEMORY, /* 分配失败。 */
  CURLHE_BAD_ARGUMENT,  /* 参数无效。 */
  CURLHE_NOT_BUILT_IN   /* 运行库构建关闭此能力。 */
} CURLHcode;

/**
 * @brief 在指定请求和来源范围内查询同名响应头。
 * @param[in] curl 存活 easy 借用，与该句柄其他调用串行。
 * @param[in] name 不含冒号的 NUL 名称借用，匹配忽略大小写。
 * @param[in] nameindex 同名头的零起点下标。
 * @param[in] origin 允许的 CURLH_* 来源组合。
 * @param[in] request 请求序号，-1 为最后请求。
 * @param[out] hout 成功取得库拥有的结构及字符串借用。
 * @return CURLHE_OK 或名称、序号、参数、内存、能力错误。
 * @note 后续同句柄查询可覆盖结构，保留时复制，句柄清理结束全部借用。
 */
CURL_EXTERN CURLHcode curl_easy_header(CURL *curl,
                                       const char *name,
                                       size_t nameindex,
                                       unsigned int origin,
                                       int request,
                                       struct curl_header **hout);

/**
 * @brief 遍历指定请求及来源范围内已经收到的响应头。
 * @param[in] curl 存活 easy 借用，遍历与其他访问串行。
 * @param[in] origin 来源位筛选组合。
 * @param[in] request 请求序号，-1 选最后请求。
 * @param[in] prev 上次结果借用，NULL 从首项开始。
 * @return 下一条库拥有的头借用，结束或范围无效为 NULL，调用者保持原内容。
 * @note 借用结构及字符串依赖句柄和后续查询，长期保留需复制。
 */
CURL_EXTERN struct curl_header *curl_easy_nextheader(CURL *curl,
                                                     unsigned int origin,
                                                     int request,
                                                     struct curl_header *prev);

#ifdef __cplusplus
} /* 结束 C 链接声明。 */
#endif

#endif /* 结束 CURLINC_HEADER_H 头文件保护。 */
