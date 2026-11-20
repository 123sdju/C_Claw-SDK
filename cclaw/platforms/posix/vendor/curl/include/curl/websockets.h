#ifndef CURLINC_WEBSOCKETS_H
#define CURLINC_WEBSOCKETS_H
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

struct curl_ws_frame {
  int age;              /* 当前结构版本为零。 */
  int flags;            /* 本帧的 CURLWS_* 属性。 */
  curl_off_t offset;    /* 当前数据在该帧负载内的字节偏移。 */
  curl_off_t bytesleft; /* 此数据块之后待接收的帧负载字节数。 */
  size_t len;           /* 本次数据块的字节数。 */
};

/* 数据类型、控制帧、消息续帧与分次发送属性位。 */
#define CURLWS_TEXT       (1 << 0)
#define CURLWS_BINARY     (1 << 1)
#define CURLWS_CONT       (1 << 2)
#define CURLWS_CLOSE      (1 << 3)
#define CURLWS_PING       (1 << 4)
#define CURLWS_OFFSET     (1 << 5)

/**
 * @brief 从已完成握手的 WebSocket 连接接收有界负载片段。
 * @param[in,out] curl 以 CONNECT_ONLY=2 完成握手的句柄借用，同句柄调用串行。
 * @param[out] buffer 调用者拥有的可写字节缓冲，覆盖 buflen 容量。
 * @param[in] buflen 本次最大接收字节数。
 * @param[out] recv 必需的实际接收字节数槽，分片组装由调用者负责。
 * @param[out] metap 成功时交付本帧元数据只读借用。
 * @return CURLE_OK、暂时无数据的 CURLE_AGAIN、关闭连接或其他协议/能力错误。
 * @note 下一次 WebSocket 调用会使元数据借用失效，检查 bytesleft 判断帧是否完整。
 */
CURL_EXTERN CURLcode curl_ws_recv(CURL *curl, void *buffer, size_t buflen,
                                  size_t *recv,
                                  const struct curl_ws_frame **metap);

/* curl_ws_send 额外支持的 PONG 控制帧位。 */
#define CURLWS_PONG       (1 << 6)

/**
 * @brief 向已建立的 WebSocket 连接发送一段负载。
 * @param[in,out] curl 连接完成且与其他调用串行的句柄借用。
 * @param[in] buffer_arg 调用期间稳定的负载借用，覆盖 buflen 字节。
 * @param[in] buflen 本次待发送字节数。
 * @param[out] sent 必需的实际负载发送数槽，成功短写后继续发送剩余部分。
 * @param[in] fragsize 通常为零，OFFSET 分次发送首调用指定总帧长，后续为零。
 * @param[in] flags 帧类型及 CONT/OFFSET 等属性，RAW_MODE 按原始模式置零。
 * @return CURLE_OK、需等待socket可写的 CURLE_AGAIN 或发送错误。
 * @note 回调内使用可能阻塞到完成或失败，调用者保管输入及相关上下文的寿命。
 */
CURL_EXTERN CURLcode curl_ws_send(CURL *curl, const void *buffer_arg,
                                  size_t buflen, size_t *sent,
                                  curl_off_t fragsize,
                                  unsigned int flags);

/**
 * @brief 在 WebSocket 读取回调内为下一段发送负载建立帧头。
 * @param[in,out] curl 当前传输句柄借用，受该传输的回调串行规则约束。
 * @param[in] flags 本帧的数据/控制类型及分片属性。
 * @param[in] frame_len 要建立的完整帧负载长度，单位字节。
 * @return CURLE_OK 或前帧未完成、原始模式、分配等错误。
 * @note 自8.16.0提供，链接库须有此符号与WS能力，回调随后提供该帧负载。
 */
CURL_EXTERN CURLcode curl_ws_start_frame(CURL *curl,
                                         unsigned int flags,
                                         curl_off_t frame_len);

/* CURLOPT_WS_OPTIONS 的原始帧模式与自动 PONG 控制位。 */
#define CURLWS_RAW_MODE   (1L << 0)
#define CURLWS_NOAUTOPONG (1L << 1)

/**
 * @brief 在 WebSocket 写回调期间查询本次数据对应的帧信息。
 * @param[in] curl 当前回调所属的存活传输句柄借用。
 * @return 本次回调内有效的元数据只读借用，非适用调用环境时为 NULL。
 * @note 元数据由库保管，异步使用须先复制，本函数不转移所有权。
 */
CURL_EXTERN const struct curl_ws_frame *curl_ws_meta(CURL *curl);

#ifdef __cplusplus
}
#endif

#endif /* 结束 CURLINC_WEBSOCKETS_H 包含保护。 */
