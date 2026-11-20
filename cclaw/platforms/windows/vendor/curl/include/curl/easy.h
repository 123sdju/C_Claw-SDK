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

/* curl_blob 内容复制策略，结构本身在设置选项期间借用。 */
#define CURL_BLOB_COPY   1 /* 库保存 data 内容副本。 */
#define CURL_BLOB_NOCOPY 0 /* 调用者保持 data 存活到选项结束使用。 */

struct curl_blob {
  void *data;
  size_t len;
  unsigned int flags; /* 最低位表示复制策略，其余保留位清零。 */
};

/**
 * @brief 创建调用者独立拥有的 easy 句柄。
 * @return 成功为新句柄，失败为 NULL，最终以 curl_easy_cleanup 回收。
 * @note 进程初始化由 owner 提前完成，同一句柄访问和销毁由 owner 串行化。
 */
CURL_EXTERN CURL *curl_easy_init(void);
/**
 * @brief 根据选项规定的准确 C 参数类型设置传输配置。
 * @param[in,out] curl 存活且当前独占操作期的 easy 借用。
 * @param[in] option 目标选项标识。
 * @param[in] ... 与选项匹配的 long、curl_off_t、对象指针或回调。
 * @return CURLE_OK 或参数、能力、资源错误，复制/借用期限按具体选项执行。
 */
CURL_EXTERN CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...);
/**
 * @brief 在本线程驱动完整同步传输并派发回调。
 * @param[in,out] curl 独占调用期句柄借用，输入正文及回调上下文保持有效。
 * @return CURLE_OK 或传输/回调错误，HTTP 状态通过选项和 getinfo 解释。
 * @note 返回前回调结束，传输错误可伴随已发生的远端动作，同句柄调用须串行。
 */
CURL_EXTERN CURLcode curl_easy_perform(CURL *curl);
/**
 * @brief 消费 easy 句柄及库内部资源。
 * @param[in] curl 拥有句柄，可 NULL，先结束在途传输并从 multi 移出。
 * @return 无返回值，清理期间协议回调上下文仍需存活。
 */
CURL_EXTERN void curl_easy_cleanup(CURL *curl);

/**
 * @brief 查询指定传输信息并写入类型匹配的输出槽。
 * @param[in] curl 存活句柄借用，查询与其他访问串行，传输结果在完成后读取。
 * @param[in] info 指定的信息项。
 * @param[out] ... 该项要求的准确类型输出指针，成功后才能使用字段。
 * @return CURLE_OK 或信息项/参数错误。
 * @note 即时信息及返回指针按具体项契约解释，多数字符串由句柄借出，slist 等项有独立释放规则。
 */
CURL_EXTERN CURLcode curl_easy_getinfo(CURL *curl, CURLINFO info, ...);

/**
 * @brief 复制配置并创建另一个独立 easy 句柄。
 * @param[in] curl 当前未被并发使用的源句柄借用。
 * @return 成功为新拥有句柄，失败为 NULL，新旧句柄分别 cleanup。
 * @note 外部指针选项继续共享借用，输入覆盖两个句柄的使用期。新句柄不继承活动连接、
 *       SSL 会话、cookie 和 share 关联，其他缓存细节依链接库版本。
 */
CURL_EXTERN CURL *curl_easy_duphandle(CURL *curl);

/**
 * @brief 将 easy 选项恢复默认并保留句柄本身。
 * @param[in,out] curl 存活且无并发传输的句柄借用。
 * @return 无返回值，后续传输重新安装所需选项与借用。
 * @note 连接、会话/DNS/cookie 缓存及 share 等状态继续存在，重置与销毁由 owner 串行化。
 */
CURL_EXTERN void curl_easy_reset(CURL *curl);

/**
 * @brief 从成功建立的 CONNECT_ONLY 连接非阻塞读取原始字节。
 * @param[in,out] curl 当前串行使用的连接句柄借用。
 * @param[out] buffer 调用者拥有的可写缓冲，覆盖 buflen 字节。
 * @param[in] buflen 接收容量。
 * @param[out] n 实际接收数槽，成功零字节表示连接关闭。
 * @return CURLE_OK、暂时无数据的 CURLE_AGAIN 或连接错误。
 * @note 先消费库和 TLS 缓存，AGAIN 后按实际 socket 就绪状态等待再试。
 */
CURL_EXTERN CURLcode curl_easy_recv(CURL *curl, void *buffer, size_t buflen,
                                    size_t *n);

/**
 * @brief 向已建立的 CONNECT_ONLY 连接非阻塞发送字节。
 * @param[in,out] curl 独占本次调用的句柄借用。
 * @param[in] buffer 调用期稳定的发送缓冲借用。
 * @param[in] buflen 发送字节数。
 * @param[out] n 实际发送数槽，成功短写由调用者继续处理。
 * @return CURLE_OK、需等可写的 CURLE_AGAIN 或传输错误，保留实际已发送前缀的语义。
 */
CURL_EXTERN CURLcode curl_easy_send(CURL *curl, const void *buffer,
                                    size_t buflen, size_t *n);

/**
 * @brief 对句柄连接缓存执行所支持协议的保活维护。
 * @param[in,out] curl 当前与其他访问串行的存活句柄借用。
 * @return CURLE_OK 或维护错误，实际效果依协议及运行库版本。
 * @note owner 主动驱动检查，间隔由 UPKEEP_INTERVAL_MS 控制，可能发送网络数据。
 */
CURL_EXTERN CURLcode curl_easy_upkeep(CURL *curl);

#ifdef __cplusplus
} /* 结束 C 链接声明。 */
#endif

#endif
