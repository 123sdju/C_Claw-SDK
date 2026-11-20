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
  CURLOT_LONG,    /* 取值范围内的 long。 */
  CURLOT_VALUES,  /* 预定义集合或位掩码，参数仍为 long。 */
  CURLOT_OFF_T,   /* 取值范围内的 curl_off_t。 */
  CURLOT_OBJECT,  /* 普通对象指针 void *。 */
  CURLOT_STRING,  /* NUL 结尾字符缓冲指针。 */
  CURLOT_SLIST,   /* curl_slist 链表指针。 */
  CURLOT_CBPTR,   /* 原样传给回调的上下文指针。 */
  CURLOT_BLOB,    /* curl_blob 指针，复制策略由 flags 指定。 */
  CURLOT_FUNCTION /* 选项对应的准确函数指针类型。 */
} curl_easytype;

/* 选项元数据标志位。 */

/* 此名称为兼容别名，元数据同时保留规范名称的选项。 */
#define CURLOT_FLAG_ALIAS (1 << 0)

/* CURLOPTTYPE_* 的 ID 范围仍编码 setopt 参数类别，实际宽度须匹配对应 C 类型。 */
struct curl_easyoption {
  const char *name;
  CURLoption id;
  curl_easytype type;
  unsigned int flags;
};

/**
 * @brief 按名称查询链接库支持的 easy 选项元数据。
 * @param[in] name 省略 CURLOPT_ 前缀的 NUL 结尾名称借用，比较不区分大小写。
 * @return 库拥有的只读元数据借用，找不到时为 NULL，调用者保持其内容原样。
 * @note 此接口自7.73.0提供，结果反映实际链接库的选项集合。
 */
CURL_EXTERN const struct curl_easyoption *
curl_easy_option_by_name(const char *name);

/**
 * @brief 根据 CURLOPT_* 数值查询规范 easy 选项元数据。
 * @param[in] id 待查询的选项 ID。
 * @return 找到时为库拥有的只读借用，未知 ID 为 NULL。
 * @note 复制指针只产生别名，调用者无释放责任。
 */
CURL_EXTERN const struct curl_easyoption *
curl_easy_option_by_id(CURLoption id);

/**
 * @brief 依次访问链接库的 easy 选项元数据。
 * @param[in] prev 上次返回的有效只读条目借用，NULL 从首项开始。
 * @return 后续只读条目借用，遍历结束返回 NULL。
 * @note 表由库拥有，调用者依据返回项继续遍历并保持条目内容原样。
 */
CURL_EXTERN const struct curl_easyoption *
curl_easy_option_next(const struct curl_easyoption *prev);

#ifdef __cplusplus
} /* 结束 C 链接声明。 */
#endif
#endif /* 结束 CURLINC_OPTIONS_H 包含保护。 */
