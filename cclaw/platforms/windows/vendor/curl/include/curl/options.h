#ifndef CURLINC_OPTIONS_H
#define CURLINC_OPTIONS_H
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

typedef enum {
  CURLOT_LONG,    /* long 范围数值。 */
  CURLOT_VALUES,  /* long 形式的预定义值或位掩码。 */
  CURLOT_OFF_T,   /* curl_off_t 范围数值。 */
  CURLOT_OBJECT,  /* 普通对象指针。 */
  CURLOT_STRING,  /* NUL 结尾字符串指针。 */
  CURLOT_SLIST,   /* curl_slist 链表指针。 */
  CURLOT_CBPTR,   /* 原样传给回调的上下文指针。 */
  CURLOT_BLOB,    /* 含复制策略的 curl_blob 指针。 */
  CURLOT_FUNCTION /* 选项要求的准确函数指针类型。 */
} curl_easytype;

/* 选项元数据标志。 */

/* 为旧程序保留的兼容别名，库另有规范选项名称。 */
#define CURLOT_FLAG_ALIAS (1 << 0)

/* CURLOPTTYPE_* 的 ID 范围标识参数类别，setopt 的实参宽度须匹配对应 C 类型。 */
struct curl_easyoption {
  const char *name;
  CURLoption id;
  curl_easytype type;
  unsigned int flags;
};

/**
 * @brief 按名称查询运行库支持的 easy 选项元数据。
 * @param[in] name 省略 CURLOPT_ 的 NUL 名称借用，匹配忽略大小写。
 * @return 库拥有的只读元数据借用，未找到为 NULL，调用者保持其内容原样。
 * @note 自 7.73.0 提供，实际集合由链接库构建决定，无句柄可变状态。
 */
CURL_EXTERN const struct curl_easyoption *
curl_easy_option_by_name(const char *name);

/**
 * @brief 按 CURLOPT 数值查询规范选项元数据。
 * @param[in] id 选项 ID。
 * @return 库拥有的只读条目借用，未知 ID 为 NULL，无释放责任。
 */
CURL_EXTERN const struct curl_easyoption *
curl_easy_option_by_id(CURLoption id);

/**
 * @brief 逐项访问运行库的选项表。
 * @param[in] prev 上次返回的只读借用，NULL 从首项开始。
 * @return 后续条目借用，末尾为 NULL，表由库持有并保持只读。
 */
CURL_EXTERN const struct curl_easyoption *
curl_easy_option_next(const struct curl_easyoption *prev);

#ifdef __cplusplus
} /* 结束 C 链接声明。 */
#endif
#endif /* 结束 CURLINC_OPTIONS_H 包含保护。 */
