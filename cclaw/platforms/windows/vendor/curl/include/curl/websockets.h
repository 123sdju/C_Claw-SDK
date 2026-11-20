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
  int flags;            /* CURLWS_* 帧属性。 */
  curl_off_t offset;    /* 当前数据在帧负载中的字节偏移。 */
  curl_off_t bytesleft; /* 本块之后仍待接收的负载字节数。 */
  size_t len;           /* 当前数据块的字节数。 */
};

/* 帧数据类型、控制种类及分片发送属性。 */
#define CURLWS_TEXT       (1 << 0)
#define CURLWS_BINARY     (1 << 1)
#define CURLWS_CONT       (1 << 2)
#define CURLWS_CLOSE      (1 << 3)
#define CURLWS_PING       (1 << 4)
#define CURLWS_OFFSET     (1 << 5)

/**
 * @brief 从已握手的 WebSocket 连接接收负载片段。
 * @param[in,out] curl 以 CONNECT_ONLY=2 完成握手的 easy 借用，同句柄调用串行。
 * @param[out] buffer 独占可写缓冲，覆盖 buflen 字节。
 * @param[in] buflen 最大接收容量。
 * @param[out] recv 实际负载接收数槽，调用者负责分片组装。
 * @param[out] metap 当前帧只读元数据借用槽。
 * @return CURLE_OK、CURLE_AGAIN 或连接关闭/协议/能力错误。
 * @note 后续 WebSocket 调用可使元数据失效，检查 bytesleft 判断当前帧完成。
 */
CURL_EXTERN CURLcode curl_ws_recv(CURL *curl, void *buffer, size_t buflen,
                                  size_t *recv,
                                  const struct curl_ws_frame **metap);

/* 供 curl_ws_send 使用的 PONG 控制帧位。 */
#define CURLWS_PONG       (1 << 6)

/**
 * @brief 发送已建立连接上的一段 WebSocket 负载。
 * @param[in,out] curl 当前与其他访问串行的句柄借用。
 * @param[in] buffer_arg 本次调用稳定的负载借用，覆盖 buflen 字节。
 * @param[in] buflen 本次发送字节数。
 * @param[out] sent 实际负载发送数槽，短写由调用者继续处理。
 * @param[in] fragsize 通常为零，OFFSET 分次发送的首调用提供完整帧长，后续为零。
 * @param[in] flags 帧类型及 CONT/OFFSET 属性，原始模式按接口要求传零。
 * @return CURLE_OK、需等可写的 CURLE_AGAIN 或发送错误。
 * @note 回调内调用可能阻塞到完成或失败，输入及上下文覆盖完整调用。
 */
CURL_EXTERN CURLcode curl_ws_send(CURL *curl, const void *buffer_arg,
                                  size_t buflen, size_t *sent,
                                  curl_off_t fragsize,
                                  unsigned int flags);

/**
 * @brief 在 WebSocket 读取回调中为随后提供的负载缓冲帧头。
 * @param[in,out] curl 当前传输句柄借用，遵循该传输的回调串行规则。
 * @param[in] flags 本帧的数据、控制及分片属性。
 * @param[in] frame_len 完整帧负载长度，单位字节。
 * @return CURLE_OK 或前帧未提供完整、原始模式、分配及协议错误。
 * @note 自 8.16.0 提供，实际链接库须支持符号和 WebSocket 能力，读取回调随后交付负载。
 */
CURL_EXTERN CURLcode curl_ws_start_frame(CURL *curl,
                                         unsigned int flags,
                                         curl_off_t frame_len);

/* CURLOPT_WS_OPTIONS 的原始模式和关闭自动 PONG 控制位。 */
#define CURLWS_RAW_MODE   (1L << 0)
#define CURLWS_NOAUTOPONG (1L << 1)

/**
 * @brief 在 WebSocket 写回调内查询当前数据块的帧信息。
 * @param[in] curl 本次回调所属的存活 easy 借用。
 * @return 仅本回调期有效的只读元数据借用，环境不适用时 NULL。
 * @note 库保管元数据，跨回调使用需复制，无释放责任。
 */
CURL_EXTERN const struct curl_ws_frame *curl_ws_meta(CURL *curl);

#ifdef __cplusplus
}
#endif

#endif /* 结束 CURLINC_WEBSOCKETS_H 包含保护。 */
