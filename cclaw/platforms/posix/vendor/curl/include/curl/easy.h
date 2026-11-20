#ifndef CURLINC_EASY_H
#define CURLINC_EASY_H
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

/* curl_blob 的复制策略位，结构本身仅在设置选项期间借用。 */
#define CURL_BLOB_COPY   1 /* libcurl 复制 data 指向的内容。 */
#define CURL_BLOB_NOCOPY 0 /* data 由调用者保管至该选项结束使用。 */

struct curl_blob {
  void *data;
  size_t len;
  unsigned int flags; /* 仅定义最低位，其余保留位须为零。 */
};

/**
 * @brief 创建由调用者拥有的独立 easy 传输句柄。
 * @return 成功返回句柄，失败返回 NULL，使用结束调用 curl_easy_cleanup。
 * @note 进程级初始化由拥有者在并发使用前安排，同一 easy 句柄的调用须串行。
 */
CURL_EXTERN CURL *curl_easy_init(void);
/**
 * @brief 按选项声明的实际参数类型设置 easy 句柄。
 * @param[in,out] curl 存活且当前没有传输在途的 easy 句柄借用。
 * @param[in] option 要设置的单个选项。
 * @param[in] ... 与该选项匹配的 long、curl_off_t、对象指针或回调。
 * @return CURLE_OK 或选项、参数、资源等错误。
 * @note 选项持续作用于后续传输，指针/回调/正文的复制或借用期限由该选项规定。
 */
CURL_EXTERN CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...);
/**
 * @brief 在当前线程同步执行一次已配置的传输并派发相应回调。
 * @param[in,out] curl 独占调用期的 easy 句柄，相关借用与回调数据保持有效。
 * @return CURLE_OK 或传输/回调错误，HTTP 状态依选项及 getinfo 单独解释。
 * @note 同一句柄须串行，回调在返回前结束，结果仍可能表示远端已发生部分动作。
 */
CURL_EXTERN CURLcode curl_easy_perform(CURL *curl);
/**
 * @brief 释放 easy 句柄及其内部连接和传输资源。
 * @param[in] curl 待消费的拥有句柄，NULL 可用。
 * @return 无返回值。
 * @note 拥有者先使句柄离开 multi 并结束在途操作，清理期间可能触发协议回调。
 */
CURL_EXTERN void curl_easy_cleanup(CURL *curl);

/**
 * @brief 读取指定传输信息，并按该信息项的类型写入调用者输出。
 * @param[in] curl 存活的 easy 句柄借用，读取与该句柄其他调用串行。
 * @param[in] info 要查询的信息项。
 * @param[out] ... 该信息项要求的准确类型指针，调用者提供有效输出槽。
 * @return CURLE_OK 或信息项/参数错误，成功后才使用输出。
 * @note 传输结果在完成后查询，少数即时信息按各项契约使用。返回指针通常借自句柄，
 *       CERTINFO、slist 等信息各有自身寿命与释放规则，长期保留须遵守具体信息项。
 */
CURL_EXTERN CURLcode curl_easy_getinfo(CURL *curl, CURLINFO info, ...);

/**
 * @brief 复制配置选项，创建另一个可独立使用的 easy 句柄。
 * @param[in] curl 当前没有并发使用的源句柄借用。
 * @return 成功为新拥有句柄，失败为 NULL，新旧句柄分别 cleanup。
 * @note 原选项中的外部指针仍可共享借用，拥有者保持其寿命覆盖两个句柄。
 *       新句柄不继承活动连接、会话状态及 share 绑定，缓存细节依所链接版本。
 */
CURL_EXTERN CURL *curl_easy_duphandle(CURL *curl);

/**
 * @brief 把 easy 的配置选项恢复为默认值，继续使用原拥有句柄。
 * @param[in,out] curl 存活且没有并发传输的句柄借用。
 * @return 无返回值。
 * @note 已有连接、会话/DNS/cookie缓存、share及部分协议缓存继续存在，
 *       重置后的每次传输须重新设置本次所需的选项与借用。
 */
CURL_EXTERN void curl_easy_reset(CURL *curl);

/**
 * @brief 从已建立的 CONNECT_ONLY 连接非阻塞接收原始字节。
 * @param[in,out] curl 连接成功且由本线程串行操作的 easy 句柄借用。
 * @param[out] buffer 调用者拥有的可写缓冲，至少覆盖 buflen 字节。
 * @param[in] buflen 本次最大接收容量，单位字节。
 * @param[out] n 必需的实际接收字节数槽，成功零字节表示连接关闭。
 * @return CURLE_OK、暂时无数据的 CURLE_AGAIN 或连接错误。
 * @note 先读取库/TLS已缓存的数据，AGAIN 后再按实际socket就绪等待并重试。
 */
CURL_EXTERN CURLcode curl_easy_recv(CURL *curl, void *buffer, size_t buflen,
                                    size_t *n);

/**
 * @brief 向已建立的 CONNECT_ONLY 连接非阻塞发送原始字节。
 * @param[in,out] curl 连接成功且由本线程串行操作的 easy 句柄借用。
 * @param[in] buffer 同步调用期间稳定的发送字节借用。
 * @param[in] buflen 待发送字节数。
 * @param[out] n 必需的实际发送字节数槽，调用者处理成功短写。
 * @return CURLE_OK、暂时不能写入的 CURLE_AGAIN 或传输错误。
 * @note AGAIN 后按socket可写状态等待，已发送前缀的副作用按实际字节数处理。
 */
CURL_EXTERN CURLcode curl_easy_send(CURL *curl, const void *buffer,
                                    size_t buflen, size_t *n);

/**
 * @brief 对句柄连接缓存执行协议支持的保活维护检查。
 * @param[in,out] curl 存活且与其他调用串行的 easy 句柄借用。
 * @return CURLE_OK 或维护错误，具体效果取决于协议与链接库版本。
 * @note 调用者主动驱动，间隔由 CURLOPT_UPKEEP_INTERVAL_MS 控制，可能产生网络写入。
 */
CURL_EXTERN CURLcode curl_easy_upkeep(CURL *curl);

#ifdef __cplusplus
} /* 结束 C 链接声明。 */
#endif

#endif
