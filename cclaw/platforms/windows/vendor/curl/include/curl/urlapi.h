#ifndef CURLINC_URLAPI_H
#define CURLINC_URLAPI_H
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

#include "curl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* URL API 的稳定错误枚举，数值顺序属于公开 ABI。 */
typedef enum {
  CURLUE_OK,
  CURLUE_BAD_HANDLE,          /* 1，URL 句柄无效。 */
  CURLUE_BAD_PARTPOINTER,     /* 2，部件输出指针无效。 */
  CURLUE_MALFORMED_INPUT,     /* 3，输入 URL 格式无效。 */
  CURLUE_BAD_PORT_NUMBER,     /* 4，端口值无效。 */
  CURLUE_UNSUPPORTED_SCHEME,  /* 5，协议方案缺少支持。 */
  CURLUE_URLDECODE,           /* 6，URL 解码失败。 */
  CURLUE_OUT_OF_MEMORY,       /* 7，内存分配失败。 */
  CURLUE_USER_NOT_ALLOWED,    /* 8，当前策略拒绝用户信息。 */
  CURLUE_UNKNOWN_PART,        /* 9，部件类型未知。 */
  CURLUE_NO_SCHEME,           /* 10，缺少协议方案。 */
  CURLUE_NO_USER,             /* 11，缺少用户名。 */
  CURLUE_NO_PASSWORD,         /* 12，缺少口令。 */
  CURLUE_NO_OPTIONS,          /* 13，缺少登录选项。 */
  CURLUE_NO_HOST,             /* 14，缺少主机。 */
  CURLUE_NO_PORT,             /* 15，缺少端口。 */
  CURLUE_NO_QUERY,            /* 16，缺少查询。 */
  CURLUE_NO_FRAGMENT,         /* 17，缺少片段。 */
  CURLUE_NO_ZONEID,           /* 18，缺少 IPv6 区域标识。 */
  CURLUE_BAD_FILE_URL,        /* 19，file URL 无效。 */
  CURLUE_BAD_FRAGMENT,        /* 20，片段格式无效。 */
  CURLUE_BAD_HOSTNAME,        /* 21，主机名无效。 */
  CURLUE_BAD_IPV6,            /* 22，IPv6 地址无效。 */
  CURLUE_BAD_LOGIN,           /* 23，登录信息无效。 */
  CURLUE_BAD_PASSWORD,        /* 24，口令格式无效。 */
  CURLUE_BAD_PATH,            /* 25，路径格式无效。 */
  CURLUE_BAD_QUERY,           /* 26，查询格式无效。 */
  CURLUE_BAD_SCHEME,          /* 27，协议方案格式无效。 */
  CURLUE_BAD_SLASHES,         /* 28，斜杠形式无效。 */
  CURLUE_BAD_USER,            /* 29，用户名格式无效。 */
  CURLUE_LACKS_IDN,           /* 30，缺少所需 IDN 能力。 */
  CURLUE_TOO_LARGE,           /* 31，输入超过库限制。 */
  CURLUE_LAST
} CURLUcode;

typedef enum {
  CURLUPART_URL,
  CURLUPART_SCHEME,
  CURLUPART_USER,
  CURLUPART_PASSWORD,
  CURLUPART_OPTIONS,
  CURLUPART_HOST,
  CURLUPART_PORT,
  CURLUPART_PATH,
  CURLUPART_QUERY,
  CURLUPART_FRAGMENT,
  CURLUPART_ZONEID /* 自 7.65.0 提供的 IPv6 区域标识。 */
} CURLUPart;

#define CURLU_DEFAULT_PORT (1 << 0)       /* 提取时返回协议默认端口。 */
#define CURLU_NO_DEFAULT_PORT (1 << 1)    /* 等于协议默认值的端口按省略处理。 */
#define CURLU_DEFAULT_SCHEME (1 << 2)     /* 缺少方案时采用库的默认方案。 */
#define CURLU_NON_SUPPORT_SCHEME (1 << 3) /* 允许解析库未实现传输的方案。 */
#define CURLU_PATH_AS_IS (1 << 4)         /* 保留路径中的点段。 */
#define CURLU_DISALLOW_USER (1 << 5)      /* 拒绝用户名和口令部分。 */
#define CURLU_URLDECODE (1 << 6)          /* get 时执行 URL 解码。 */
#define CURLU_URLENCODE (1 << 7)          /* set 时执行 URL 编码。 */
#define CURLU_APPENDQUERY (1 << 8)        /* 追加表单形式的查询部分。 */
#define CURLU_GUESS_SCHEME (1 << 9)       /* 沿 curl 历史规则推测方案。 */
#define CURLU_NO_AUTHORITY (1 << 10)      /* 未知方案可使用空 authority。 */
#define CURLU_ALLOW_SPACE (1 << 11)       /* 允许 URL 含空格。 */
#define CURLU_PUNYCODE (1 << 12)          /* 以 Punycode 形式提取主机名。 */
#define CURLU_PUNY2IDN (1 << 13)          /* 从 Punycode 转为国际化域名。 */
#define CURLU_GET_EMPTY (1 << 14)         /* 提取 URL 或部件时允许空查询和空片段。 */
#define CURLU_NO_GUESS_SCHEME (1 << 15)   /* get 要求显式方案。 */

typedef struct Curl_URL CURLU;

/**
 * @brief 创建独立拥有的 URL 解析/构造句柄。
 * @return 成功为新 CURLU，失败为 NULL，最终以 curl_url_cleanup 回收。
 * @note 同一句柄的查询、修改和销毁由 owner 串行化。
 */
CURL_EXTERN CURLU *curl_url(void);

/**
 * @brief 释放 URL 句柄及内部部件存储。
 * @param[in] u 待消费的拥有句柄，可 NULL，先结束在途调用。
 * @return 无返回值，get 已交付的独立字符串由接收者另行 curl_free。
 */
CURL_EXTERN void curl_url_cleanup(CURLU *u);

/**
 * @brief 复制 URL 句柄及部件内容。
 * @param[in] in 调用期稳定且无并发修改的源句柄借用。
 * @return 成功为独立拥有句柄，失败 NULL，新旧句柄分别 cleanup。
 */
CURL_EXTERN CURLU *curl_url_dup(const CURLU *in);

/**
 * @brief 提取完整 URL 或部件并分配独立文本。
 * @param[in] u 调用期稳定的句柄借用，与修改和销毁串行。
 * @param[in] what 目标部件或完整 URL 枚举。
 * @param[out] part 必填新字符串槽，成功移交 curl_free 的拥有文本。
 * @param[in] flags 本查询支持的端口、解码、方案等位选项。
 * @return CURLUE_OK 或部件缺失、参数、编码、容量及分配错误，成功后读取输出。
 * @note 成功文本寿命独立于 URL 句柄。
 */
CURL_EXTERN CURLUcode curl_url_get(const CURLU *u, CURLUPart what,
                                   char **part, unsigned int flags);

/**
 * @brief 设置完整 URL 或指定部件，输入在调用内复制。
 * @param[in,out] u 当前独占使用的 URL 句柄借用。
 * @param[in] what 要修改的部件枚举。
 * @param[in] part NUL 文本借用，NULL 清除部件。
 * @param[in] flags 编码、点段、追加查询及方案策略组合。
 * @return CURLUE_OK 或格式、策略、构建能力、容量及分配错误。
 * @note owner 协调同一句柄访问，复制完成后输入可结束寿命。
 */
CURL_EXTERN CURLUcode curl_url_set(CURLU *u, CURLUPart what,
                                   const char *part, unsigned int flags);

/**
 * @brief 把 URL 结果码转换成诊断文本。
 * @param[in] error 待说明结果码。
 * @return 库拥有的只读 NUL 文本借用，调用者保持原内容，无释放责任。
 */
CURL_EXTERN const char *curl_url_strerror(CURLUcode error);

#ifdef __cplusplus
} /* 结束 C 链接声明。 */
#endif

#endif /* 结束 CURLINC_URLAPI_H 包含保护。 */
