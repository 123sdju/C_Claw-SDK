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
  char *name;    /* 名称借用，大小写可与查询名不同。 */
  char *value;
  size_t amount; /* 查询范围内同名头的数量。 */
  size_t index;  /* 本项从零开始的序号。 */
  unsigned int origin; /* 下方来源位中的实际来源。 */
  void *anchor; /* libcurl 内部借用锚点，调用者保持原值。 */
};

/* HTTP 响应头的来源位，可在查询时组合筛选。 */
#define CURLH_HEADER    (1 << 0) /* 普通服务器响应头。 */
#define CURLH_TRAILER   (1 << 1) /* 正文后的尾部头。 */
#define CURLH_CONNECT   (1 << 2) /* 代理 CONNECT 响应头。 */
#define CURLH_1XX       (1 << 3) /* 1xx 中间响应头。 */
#define CURLH_PSEUDO    (1 << 4) /* HTTP/2、HTTP/3 伪头。 */

typedef enum {
  CURLHE_OK,
  CURLHE_BADINDEX,      /* 同名头存在，指定序号无对应项。 */
  CURLHE_MISSING,       /* 没有指定名称的头。 */
  CURLHE_NOHEADERS,     /* 当前尚未收到任何头。 */
  CURLHE_NOREQUEST,     /* 请求序号没有对应请求。 */
  CURLHE_OUT_OF_MEMORY, /* 处理时分配失败。 */
  CURLHE_BAD_ARGUMENT,  /* 参数无效。 */
  CURLHE_NOT_BUILT_IN   /* 链接库构建未启用此 API。 */
} CURLHcode;

/**
 * @brief 查询一次传输中指定请求和来源范围的同名响应头。
 * @param[in] curl 存活的 easy 句柄借用，查询与句柄其他调用串行。
 * @param[in] name 不含冒号的 NUL 结尾头名称借用，比较不区分大小写。
 * @param[in] nameindex 同名头从零开始的序号。
 * @param[in] origin 允许的 CURLH_* 来源位组合。
 * @param[in] request 重定向/认证请求从零开始的序号，-1 选择最后请求。
 * @param[out] hout 必需的结果槽，成功返回句柄拥有的结构与字符串借用。
 * @return CURLHE_OK 或缺头、序号、参数、内存及构建能力错误。
 * @note 后续同句柄头查询可覆盖结构，长期保留须复制，cleanup 后借用失效。
 */
CURL_EXTERN CURLHcode curl_easy_header(CURL *curl,
                                       const char *name,
                                       size_t nameindex,
                                       unsigned int origin,
                                       int request,
                                       struct curl_header **hout);

/**
 * @brief 按来源和请求范围遍历已经接收的响应头。
 * @param[in] curl 存活的 easy 句柄借用，遍历与其他句柄调用串行。
 * @param[in] origin 允许的 CURLH_* 来源位组合。
 * @param[in] request 请求序号，-1 表示最后请求。
 * @param[in] prev 上次返回的头借用，NULL 从该范围首项开始。
 * @return 下一个句柄拥有的头借用，没有后续项或范围无效时为 NULL。
 * @note 返回结构和字符串禁止释放或修改，跨后续查询/传输保留时先复制。
 */
CURL_EXTERN struct curl_header *curl_easy_nextheader(CURL *curl,
                                                     unsigned int origin,
                                                     int request,
                                                     struct curl_header *prev);

#ifdef __cplusplus
} /* 结束 C 链接声明。 */
#endif

#endif /* 结束 CURLINC_HEADER_H 包含保护。 */
