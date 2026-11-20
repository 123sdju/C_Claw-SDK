#ifndef CURLINC_TYPECHECK_GCC_H
#define CURLINC_TYPECHECK_GCC_H
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

/* GCC专用编译期类型筛查。扩展一种选项类型时，同步增加选项分类、实参匹配
 * 和对应warning函数。已有类型的新选项加入相应分类表。
 * 两层if沿上游规避GCC4.3/4.4配合-Wlogical-op的历史问题。
 * 类型/宽度筛查具有近似边界，句柄、容量和对象寿命由运行期调用契约保证。 */

/**
 * @brief 为常量easy选项执行编译期参数筛查，再调用真实setopt。
 * @param[in,out] handle 当前有效的easy借用，执行语义遵循真实函数。
 * @param[in] option 选项表达式，编译器可确认为常量时按分类表检查类型。
 * @param[in] value 选项对应参数，指针和回调借用遵循具体选项。
 * @return 真实curl_easy_setopt的CURLcode。
 * @note helper检查依赖类型而非指针实际值，实际调用各参数求值一次。
 */
#define curl_easy_setopt(handle, option, value)                         \
  __extension__({                                                       \
    if(__builtin_constant_p(option)) {                                  \
      CURL_IGNORE_DEPRECATION(                                          \
        if(curlcheck_long_option(option))                               \
          if(!curlcheck_long(value))                                    \
            Wcurl_easy_setopt_err_long();                               \
        if(curlcheck_off_t_option(option))                              \
          if(!curlcheck_off_t(value))                                   \
            Wcurl_easy_setopt_err_curl_off_t();                         \
        if(curlcheck_string_option(option))                             \
          if(!curlcheck_string(value))                                  \
            Wcurl_easy_setopt_err_string();                             \
        if((option) == CURLOPT_PRIVATE) { }                             \
        if(curlcheck_write_cb_option(option))                           \
          if(!curlcheck_write_cb(value))                                \
            Wcurl_easy_setopt_err_write_callback();                     \
        if(curlcheck_curl_option(option))                               \
          if(!curlcheck_curl(value))                                    \
            Wcurl_easy_setopt_err_curl();                               \
        if((option) == CURLOPT_RESOLVER_START_FUNCTION)                 \
          if(!curlcheck_resolver_start_callback(value))                 \
            Wcurl_easy_setopt_err_resolver_start_callback();            \
        if((option) == CURLOPT_READFUNCTION)                            \
          if(!curlcheck_read_cb(value))                                 \
            Wcurl_easy_setopt_err_read_cb();                            \
        if((option) == CURLOPT_IOCTLFUNCTION)                           \
          if(!curlcheck_ioctl_cb(value))                                \
            Wcurl_easy_setopt_err_ioctl_cb();                           \
        if((option) == CURLOPT_SOCKOPTFUNCTION)                         \
          if(!curlcheck_sockopt_cb(value))                              \
            Wcurl_easy_setopt_err_sockopt_cb();                         \
        if((option) == CURLOPT_OPENSOCKETFUNCTION)                      \
          if(!curlcheck_opensocket_cb(value))                           \
            Wcurl_easy_setopt_err_opensocket_cb();                      \
        if((option) == CURLOPT_PROGRESSFUNCTION)                        \
          if(!curlcheck_progress_cb(value))                             \
            Wcurl_easy_setopt_err_progress_cb();                        \
        if((option) == CURLOPT_XFERINFOFUNCTION)                        \
          if(!curlcheck_xferinfo_cb(value))                             \
            Wcurl_easy_setopt_err_xferinfo_cb();                        \
        if((option) == CURLOPT_DEBUGFUNCTION)                           \
          if(!curlcheck_debug_cb(value))                                \
            Wcurl_easy_setopt_err_debug_cb();                           \
        if((option) == CURLOPT_SSL_CTX_FUNCTION)                        \
          if(!curlcheck_ssl_ctx_cb(value))                              \
            Wcurl_easy_setopt_err_ssl_ctx_cb();                         \
        if(curlcheck_conv_cb_option(option))                            \
          if(!curlcheck_conv_cb(value))                                 \
            Wcurl_easy_setopt_err_conv_cb();                            \
        if((option) == CURLOPT_SEEKFUNCTION)                            \
          if(!curlcheck_seek_cb(value))                                 \
            Wcurl_easy_setopt_err_seek_cb();                            \
        if((option) == CURLOPT_CHUNK_BGN_FUNCTION)                      \
          if(!curlcheck_chunk_bgn_cb(value))                            \
            Wcurl_easy_setopt_err_chunk_bgn_cb();                       \
        if((option) == CURLOPT_CHUNK_END_FUNCTION)                      \
          if(!curlcheck_chunk_end_cb(value))                            \
            Wcurl_easy_setopt_err_chunk_end_cb();                       \
        if((option) == CURLOPT_CLOSESOCKETFUNCTION)                     \
          if(!curlcheck_close_socket_cb(value))                         \
            Wcurl_easy_setopt_err_close_socket_cb();                    \
        if((option) == CURLOPT_FNMATCH_FUNCTION)                        \
          if(!curlcheck_fnmatch_cb(value))                              \
            Wcurl_easy_setopt_err_fnmatch_cb();                         \
        if((option) == CURLOPT_HSTSREADFUNCTION)                        \
          if(!curlcheck_hstsread_cb(value))                             \
            Wcurl_easy_setopt_err_hstsread_cb();                        \
        if((option) == CURLOPT_HSTSWRITEFUNCTION)                       \
          if(!curlcheck_hstswrite_cb(value))                            \
            Wcurl_easy_setopt_err_hstswrite_cb();                       \
        if((option) == CURLOPT_SSH_HOSTKEYFUNCTION)                     \
          if(!curlcheck_ssh_hostkey_cb(value))                          \
            Wcurl_easy_setopt_err_ssh_hostkey_cb();                     \
        if((option) == CURLOPT_SSH_KEYFUNCTION)                         \
          if(!curlcheck_ssh_key_cb(value))                              \
            Wcurl_easy_setopt_err_ssh_key_cb();                         \
        if((option) == CURLOPT_INTERLEAVEFUNCTION)                      \
          if(!curlcheck_interleave_cb(value))                           \
            Wcurl_easy_setopt_err_interleave_cb();                      \
        if((option) == CURLOPT_PREREQFUNCTION)                          \
          if(!curlcheck_prereq_cb(value))                               \
            Wcurl_easy_setopt_err_prereq_cb();                          \
        if((option) == CURLOPT_TRAILERFUNCTION)                         \
          if(!curlcheck_trailer_cb(value))                              \
            Wcurl_easy_setopt_err_trailer_cb();                         \
        if(curlcheck_cb_data_option(option))                            \
          if(!curlcheck_cb_data(value))                                 \
            Wcurl_easy_setopt_err_cb_data();                            \
        if((option) == CURLOPT_ERRORBUFFER)                             \
          if(!curlcheck_error_buffer(value))                            \
            Wcurl_easy_setopt_err_error_buffer();                       \
        if((option) == CURLOPT_CURLU)                                   \
          if(!curlcheck_ptr((value), CURLU))                            \
            Wcurl_easy_setopt_err_curlu();                              \
        if((option) == CURLOPT_STDERR)                                  \
          if(!curlcheck_FILE(value))                                    \
            Wcurl_easy_setopt_err_FILE();                               \
        if(curlcheck_postfields_option(option))                         \
          if(!curlcheck_postfields(value))                              \
            Wcurl_easy_setopt_err_postfields();                         \
        if((option) == CURLOPT_HTTPPOST)                                \
          if(!curlcheck_arr((value), struct curl_httppost))             \
            Wcurl_easy_setopt_err_curl_httpost();                       \
        if((option) == CURLOPT_MIMEPOST)                                \
          if(!curlcheck_ptr((value), curl_mime))                        \
            Wcurl_easy_setopt_err_curl_mimepost();                      \
        if(curlcheck_slist_option(option))                              \
          if(!curlcheck_arr((value), struct curl_slist))                \
            Wcurl_easy_setopt_err_curl_slist();                         \
        if((option) == CURLOPT_SHARE)                                   \
          if(!curlcheck_ptr((value), CURLSH))                           \
            Wcurl_easy_setopt_err_CURLSH();                             \
      )                                                                 \
    }                                                                   \
    (curl_easy_setopt)(handle, option, value);                          \
  })

/**
 * @brief 对常量信息项筛查输出指针类型，再调用真实getinfo。
 * @param[in] handle 存活的easy借用，访问由拥有者串行。
 * @param[in] info 信息项表达式，常量时按返回类型组筛查。
 * @param[out] arg 与信息项匹配的输出槽，寿命覆盖同步调用。
 * @return 真实curl_easy_getinfo的CURLcode，成功后读取输出。
 * @note 编译期类型通过后，调用者仍按实际信息项管理返回借用或拥有对象。
 */
#define curl_easy_getinfo(handle, info, arg)                            \
  __extension__({                                                       \
    if(__builtin_constant_p(info)) {                                    \
      CURL_IGNORE_DEPRECATION(                                          \
        if(curlcheck_string_info(info))                                 \
          if(!curlcheck_arr((arg), char *))                             \
            Wcurl_easy_getinfo_err_string();                            \
        if(curlcheck_long_info(info))                                   \
          if(!curlcheck_arr((arg), long))                               \
            Wcurl_easy_getinfo_err_long();                              \
        if(curlcheck_double_info(info))                                 \
          if(!curlcheck_arr((arg), double))                             \
            Wcurl_easy_getinfo_err_double();                            \
        if(curlcheck_slist_info(info))                                  \
          if(!curlcheck_arr((arg), struct curl_slist *))                \
            Wcurl_easy_getinfo_err_curl_slist();                        \
        if(curlcheck_tlssessioninfo_info(info))                         \
          if(!curlcheck_arr((arg), struct curl_tlssessioninfo *))       \
            Wcurl_easy_getinfo_err_curl_tlssessioninfo();               \
        if(curlcheck_certinfo_info(info))                               \
          if(!curlcheck_arr((arg), struct curl_certinfo *))             \
            Wcurl_easy_getinfo_err_curl_certinfo();                     \
        if(curlcheck_socket_info(info))                                 \
          if(!curlcheck_arr((arg), curl_socket_t))                      \
            Wcurl_easy_getinfo_err_curl_socket();                       \
        if(curlcheck_off_t_info(info))                                  \
          if(!curlcheck_arr((arg), curl_off_t))                         \
            Wcurl_easy_getinfo_err_curl_off_t();                        \
      )                                                                 \
    }                                                                   \
    (curl_easy_getinfo)(handle, info, arg);                             \
  })

/**
 * @brief 筛查常量multi选项的数值、上下文及回调类型并转调真实函数。
 * @param[in,out] handle 存活且访问串行的multi借用。
 * @param[in] option 要设置的multi选项表达式。
 * @param[in] value 匹配该选项的参数，借用期沿multi真实契约。
 * @return 真实curl_multi_setopt的CURLMcode。
 */
#define curl_multi_setopt(handle, option, value)                        \
  __extension__({                                                       \
    if(__builtin_constant_p(option)) {                                  \
      if(curlcheck_long_option(option))                                 \
        if(!curlcheck_long(value))                                      \
          Wcurl_multi_setopt_err_long();                                \
      if(curlcheck_off_t_option(option))                                \
        if(!curlcheck_off_t(value))                                     \
          Wcurl_multi_setopt_err_curl_off_t();                          \
      if(curlcheck_multicb_data_option(option))                         \
        if(!curlcheck_cb_data(value))                                   \
          Wcurl_multi_setopt_err_cb_data();                             \
      if(curlcheck_charpp_option(option))                               \
        if(!curlcheck_ptrptr(value, char))                              \
          Wcurl_multi_setopt_err_charpp();                              \
      if((option) == CURLMOPT_NOTIFYFUNCTION)                           \
        if(!curlcheck_multinotify_cb(value))                            \
          Wcurl_multi_setopt_err_notifycb();                            \
      if((option) == CURLMOPT_PUSHFUNCTION)                             \
        if(!curlcheck_multipush_cb(value))                              \
          Wcurl_multi_setopt_err_pushcb();                              \
      if((option) == CURLMOPT_SOCKETFUNCTION)                           \
        if(!curlcheck_multisocket_cb(value))                            \
          Wcurl_multi_setopt_err_socketcb();                            \
      if((option) == CURLMOPT_TIMERFUNCTION)                            \
        if(!curlcheck_multitimer_cb(value))                             \
          Wcurl_multi_setopt_err_timercb();                             \
    }                                                                   \
    (curl_multi_setopt)(handle, option, value);                         \
  })

/* 判定multi选项是否接收借给回调的上下文。 */
#define curlcheck_multicb_data_option(option)                           \
  ((option) == CURLMOPT_NOTIFYDATA ||                                   \
   (option) == CURLMOPT_PUSHDATA ||                                     \
   (option) == CURLMOPT_SOCKETDATA ||                                   \
   (option) == CURLMOPT_TIMERDATA ||                                    \
   0)

/* 判定multi选项是否接收字符串指针数组。 */
#define curlcheck_charpp_option(option)                                 \
  ((option) == CURLMOPT_PIPELINING_SERVER_BL ||                         \
   (option) == CURLMOPT_PIPELINING_SITE_BL ||                           \
   0)

/* 类型筛查timer回调或与NULL类型相容的表达式。 */
#define curlcheck_multitimer_cb(expr)                                   \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_multi_timer_callback))

/* 类型筛查socket回调或与NULL类型相容的表达式。 */
#define curlcheck_multisocket_cb(expr)                                  \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_socket_callback))

/* 类型筛查HTTP/2推送回调。 */
#define curlcheck_multipush_cb(expr)                                    \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_push_callback))

/* 类型筛查multi通知回调。 */
#define curlcheck_multinotify_cb(expr)                                  \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_notify_callback))

/**
 * @brief 用固定三个宏参数转调share选项入口。
 * @param[in,out] share 存活的共享对象借用。
 * @param[in] opt 共享选项表达式。
 * @param[in] param 由该选项规定类型与寿命的参数。
 * @return 真实curl_share_setopt的CURLSHcode。
 * @note 此转发约束宏实参数量，具体参数类型仍由调用者遵守。
 */
#define curl_share_setopt(share, opt, param) \
  (curl_share_setopt)(share, opt, param)

/**
 * @brief 定义带GCC warning属性的内部静态空函数，供错误类型分支触发诊断。
 * @param[in] id 生成函数的标识符记号，该函数自身无形参。
 * @param[in] message 编译期诊断字符串字面量。
 * @return 生成的函数无返回值，空汇编与noinline保持可诊断的调用位置。
 * @note 这组函数不拥有运行期资源，只有编译器保留错误分支时触发警告。
 */
#define CURLWARNING(id, message)                                        \
  static void __attribute__((__warning__(message)))                     \
  __attribute__((__unused__)) __attribute__((__noinline__))             \
  id(void) { __asm__(""); }

/** @brief 诊断multi数值选项的long实参类型错误。 @return 无返回值，触发编译警告。 */
CURLWARNING(Wcurl_multi_setopt_err_long,
            "curl_multi_setopt expects a long argument")
/** @brief 诊断multi大整数选项的curl_off_t类型错误。 @return 无返回值，触发编译警告。 */
CURLWARNING(Wcurl_multi_setopt_err_curl_off_t,
            "curl_multi_setopt expects a curl_off_t argument")
/** @brief 诊断multi回调上下文参数的类型/宽度错误。 @return 无返回值。 */
CURLWARNING(Wcurl_multi_setopt_err_cb_data,
            "curl_multi_setopt expects a 'void *' argument")
/** @brief 诊断multi字符串列表参数类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_multi_setopt_err_charpp,
            "curl_multi_setopt expects a 'char **' argument")
/** @brief 诊断multi推送回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_multi_setopt_err_pushcb,
            "curl_multi_setopt expects a curl_push_callback argument")
/** @brief 诊断multi通知回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_multi_setopt_err_notifycb,
            "curl_multi_setopt expects a curl_notify_callback argument")
/** @brief 诊断multi socket回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_multi_setopt_err_socketcb,
            "curl_multi_setopt expects a curl_socket_callback argument")
/** @brief 诊断multi timer回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_multi_setopt_err_timercb,
            "curl_multi_setopt expects a curl_multi_timer_callback argument")

/** @brief 诊断easy long选项实参类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_long,
            "curl_easy_setopt expects a long argument")
/** @brief 诊断easy curl_off_t选项实参类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_curl_off_t,
            "curl_easy_setopt expects a curl_off_t argument")
/** @brief 诊断easy字符串选项的指针/数组类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_string,
            "curl_easy_setopt expects a "
            "string ('char *' or char[]) argument")
/** @brief 诊断easy正文/响应头写回调类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_write_callback,
            "curl_easy_setopt expects a curl_write_callback argument")
/** @brief 诊断DNS解析开始回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_resolver_start_callback,
            "curl_easy_setopt expects a "
            "curl_resolver_start_callback argument")
/** @brief 诊断上传读取回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_read_cb,
            "curl_easy_setopt expects a curl_read_callback argument")
/** @brief 诊断历史IO控制回调类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_ioctl_cb,
            "curl_easy_setopt expects a curl_ioctl_callback argument")
/** @brief 诊断socket选项回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_sockopt_cb,
            "curl_easy_setopt expects a curl_sockopt_callback argument")
/** @brief 诊断socket创建回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_opensocket_cb,
            "curl_easy_setopt expects a "
            "curl_opensocket_callback argument")
/** @brief 诊断旧double进度回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_progress_cb,
            "curl_easy_setopt expects a curl_progress_callback argument")
/** @brief 诊断curl_off_t进度回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_xferinfo_cb,
            "curl_easy_setopt expects a curl_xferinfo_callback argument")
/** @brief 诊断调试数据回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_debug_cb,
            "curl_easy_setopt expects a curl_debug_callback argument")
/** @brief 诊断TLS上下文回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_ssl_ctx_cb,
            "curl_easy_setopt expects a curl_ssl_ctx_callback argument")
/** @brief 诊断历史字符转换回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_conv_cb,
            "curl_easy_setopt expects a curl_conv_callback argument")
/** @brief 诊断上传定位回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_seek_cb,
            "curl_easy_setopt expects a curl_seek_callback argument")
/** @brief 诊断easy回调上下文的类型/宽度错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_cb_data,
            "curl_easy_setopt expects a "
            "private data pointer as argument")
/** @brief 诊断文件块开始回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_chunk_bgn_cb,
            "curl_easy_setopt expects a curl_chunk_bgn_callback argument")
/** @brief 诊断文件块结束回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_chunk_end_cb,
            "curl_easy_setopt expects a curl_chunk_end_callback argument")
/** @brief 诊断socket关闭回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_close_socket_cb,
            "curl_easy_setopt expects a curl_closesocket_callback argument")
/** @brief 诊断文件名匹配回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_fnmatch_cb,
            "curl_easy_setopt expects a curl_fnmatch_callback argument")
/** @brief 诊断HSTS读取回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_hstsread_cb,
            "curl_easy_setopt expects a curl_hstsread_callback argument")
/** @brief 诊断HSTS写入回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_hstswrite_cb,
            "curl_easy_setopt expects a curl_hstswrite_callback argument")
/** @brief 诊断SSH公钥检查回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_ssh_key_cb,
            "curl_easy_setopt expects a curl_sshkeycallback argument")
/** @brief 诊断SSH主机密钥回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_ssh_hostkey_cb,
            "curl_easy_setopt expects a curl_sshhostkeycallback argument")
/** @brief 诊断交错协议数据回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_interleave_cb,
            "curl_easy_setopt expects a curl_interleave_callback argument")
/** @brief 诊断连接后请求前置回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_prereq_cb,
            "curl_easy_setopt expects a curl_prereq_callback argument")
/** @brief 诊断请求尾部头生成回调的原型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_trailer_cb,
            "curl_easy_setopt expects a curl_trailerfunc_ok argument")
/** @brief 诊断错误缓冲的可写字符类型错误。 @return 无返回值，容量另由调用者保证。 */
CURLWARNING(Wcurl_easy_setopt_err_error_buffer,
            "curl_easy_setopt expects a "
            "char buffer of CURL_ERROR_SIZE as argument")
/** @brief 诊断URL解析句柄参数类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_curlu,
            "curl_easy_setopt expects a 'CURLU *' argument")
/** @brief 诊断easy依赖句柄参数类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_curl,
            "curl_easy_setopt expects a 'CURL *' argument")
/** @brief 诊断FILE流参数类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_FILE,
            "curl_easy_setopt expects a 'FILE *' argument")
/** @brief 诊断POST正文的字节指针参数类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_postfields,
            "curl_easy_setopt expects a 'void *' or 'char *' argument")
/** @brief 诊断历史表单链表的指针类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_curl_httpost,
            "curl_easy_setopt expects a 'struct curl_httppost *' "
            "argument")
/** @brief 诊断MIME正文对象的指针类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_curl_mimepost,
            "curl_easy_setopt expects a 'curl_mime *' "
            "argument")
/** @brief 诊断字符串链表参数类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_curl_slist,
            "curl_easy_setopt expects a 'struct curl_slist *' argument")
/** @brief 诊断共享对象句柄类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_setopt_err_CURLSH,
            "curl_easy_setopt expects a CURLSH* argument")
/** @brief 诊断getinfo字符串输出槽的指针层级错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_getinfo_err_string,
            "curl_easy_getinfo expects a pointer to 'char *'")
/** @brief 诊断getinfo long输出槽类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_getinfo_err_long,
            "curl_easy_getinfo expects a pointer to long")
/** @brief 诊断getinfo double输出槽类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_getinfo_err_double,
            "curl_easy_getinfo expects a pointer to double")
/** @brief 诊断getinfo字符串链表输出槽类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_getinfo_err_curl_slist,
            "curl_easy_getinfo expects a pointer to 'struct curl_slist *'")
/** @brief 诊断getinfo TLS会话结构输出槽类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_getinfo_err_curl_tlssessioninfo,
            "curl_easy_getinfo expects a pointer to "
            "'struct curl_tlssessioninfo *'")
/** @brief 诊断getinfo证书信息输出槽类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_getinfo_err_curl_certinfo,
            "curl_easy_getinfo expects a pointer to "
            "'struct curl_certinfo *'")
/** @brief 诊断getinfo socket输出槽类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_getinfo_err_curl_socket,
            "curl_easy_getinfo expects a pointer to curl_socket_t")
/** @brief 诊断getinfo curl_off_t输出槽类型错误。 @return 无返回值。 */
CURLWARNING(Wcurl_easy_getinfo_err_curl_off_t,
            "curl_easy_getinfo expects a pointer to curl_off_t")

/* 按实参类型分组的 easy 选项常量表。 */

/* 按枚举范围判断选项是否属于 long 数值组。 */
#define curlcheck_long_option(option)                                   \
  (0 < (option) && (option) < CURLOPTTYPE_OBJECTPOINT)

#define curlcheck_off_t_option(option)                                  \
  (((option) > CURLOPTTYPE_OFF_T) && ((option) < CURLOPTTYPE_BLOB))

/* 判断依赖 easy 句柄的两个选项。 */
#define curlcheck_curl_option(option)                                   \
  ((option) == CURLOPT_STREAM_DEPENDS ||                                \
   (option) == CURLOPT_STREAM_DEPENDS_E ||                              \
   0)

/* 显式列举接收字符指针的选项，指针实际有效性沿具体运行期契约。 */
#define curlcheck_string_option(option)                                 \
  ((option) == CURLOPT_ABSTRACT_UNIX_SOCKET ||                          \
   (option) == CURLOPT_ACCEPT_ENCODING ||                               \
   (option) == CURLOPT_ALTSVC ||                                        \
   (option) == CURLOPT_CAINFO ||                                        \
   (option) == CURLOPT_CAPATH ||                                        \
   (option) == CURLOPT_COOKIE ||                                        \
   (option) == CURLOPT_COOKIEFILE ||                                    \
   (option) == CURLOPT_COOKIEJAR ||                                     \
   (option) == CURLOPT_COOKIELIST ||                                    \
   (option) == CURLOPT_CRLFILE ||                                       \
   (option) == CURLOPT_CUSTOMREQUEST ||                                 \
   (option) == CURLOPT_DEFAULT_PROTOCOL ||                              \
   (option) == CURLOPT_DNS_INTERFACE ||                                 \
   (option) == CURLOPT_DNS_LOCAL_IP4 ||                                 \
   (option) == CURLOPT_DNS_LOCAL_IP6 ||                                 \
   (option) == CURLOPT_DNS_SERVERS ||                                   \
   (option) == CURLOPT_DOH_URL ||                                       \
   (option) == CURLOPT_ECH ||                                           \
   (option) == CURLOPT_EGDSOCKET ||                                     \
   (option) == CURLOPT_FTP_ACCOUNT ||                                   \
   (option) == CURLOPT_FTP_ALTERNATIVE_TO_USER ||                       \
   (option) == CURLOPT_FTPPORT ||                                       \
   (option) == CURLOPT_HAPROXY_CLIENT_IP ||                             \
   (option) == CURLOPT_HSTS ||                                          \
   (option) == CURLOPT_INTERFACE ||                                     \
   (option) == CURLOPT_ISSUERCERT ||                                    \
   (option) == CURLOPT_KEYPASSWD ||                                     \
   (option) == CURLOPT_KRBLEVEL ||                                      \
   (option) == CURLOPT_LOGIN_OPTIONS ||                                 \
   (option) == CURLOPT_MAIL_AUTH ||                                     \
   (option) == CURLOPT_MAIL_FROM ||                                     \
   (option) == CURLOPT_NETRC_FILE ||                                    \
   (option) == CURLOPT_NOPROXY ||                                       \
   (option) == CURLOPT_PASSWORD ||                                      \
   (option) == CURLOPT_PINNEDPUBLICKEY ||                               \
   (option) == CURLOPT_PRE_PROXY ||                                     \
   (option) == CURLOPT_PROTOCOLS_STR ||                                 \
   (option) == CURLOPT_PROXY ||                                         \
   (option) == CURLOPT_PROXY_CAINFO ||                                  \
   (option) == CURLOPT_PROXY_CAPATH ||                                  \
   (option) == CURLOPT_PROXY_CRLFILE ||                                 \
   (option) == CURLOPT_PROXY_ISSUERCERT ||                              \
   (option) == CURLOPT_PROXY_KEYPASSWD ||                               \
   (option) == CURLOPT_PROXY_PINNEDPUBLICKEY ||                         \
   (option) == CURLOPT_PROXY_SERVICE_NAME ||                            \
   (option) == CURLOPT_PROXY_SSL_CIPHER_LIST ||                         \
   (option) == CURLOPT_PROXY_SSLCERT ||                                 \
   (option) == CURLOPT_PROXY_SSLCERTTYPE ||                             \
   (option) == CURLOPT_PROXY_SSLKEY ||                                  \
   (option) == CURLOPT_PROXY_SSLKEYTYPE ||                              \
   (option) == CURLOPT_PROXY_TLS13_CIPHERS ||                           \
   (option) == CURLOPT_PROXY_TLSAUTH_PASSWORD ||                        \
   (option) == CURLOPT_PROXY_TLSAUTH_TYPE ||                            \
   (option) == CURLOPT_PROXY_TLSAUTH_USERNAME ||                        \
   (option) == CURLOPT_PROXYPASSWORD ||                                 \
   (option) == CURLOPT_PROXYUSERNAME ||                                 \
   (option) == CURLOPT_PROXYUSERPWD ||                                  \
   (option) == CURLOPT_RANDOM_FILE ||                                   \
   (option) == CURLOPT_RANGE ||                                         \
   (option) == CURLOPT_REDIR_PROTOCOLS_STR ||                           \
   (option) == CURLOPT_REFERER ||                                       \
   (option) == CURLOPT_REQUEST_TARGET ||                                \
   (option) == CURLOPT_RTSP_SESSION_ID ||                               \
   (option) == CURLOPT_RTSP_STREAM_URI ||                               \
   (option) == CURLOPT_RTSP_TRANSPORT ||                                \
   (option) == CURLOPT_SASL_AUTHZID ||                                  \
   (option) == CURLOPT_SERVICE_NAME ||                                  \
   (option) == CURLOPT_SOCKS5_GSSAPI_SERVICE ||                         \
   (option) == CURLOPT_SSH_HOST_PUBLIC_KEY_MD5 ||                       \
   (option) == CURLOPT_SSH_HOST_PUBLIC_KEY_SHA256 ||                    \
   (option) == CURLOPT_SSH_KNOWNHOSTS ||                                \
   (option) == CURLOPT_SSH_PRIVATE_KEYFILE ||                           \
   (option) == CURLOPT_SSH_PUBLIC_KEYFILE ||                            \
   (option) == CURLOPT_SSLCERT ||                                       \
   (option) == CURLOPT_SSLCERTTYPE ||                                   \
   (option) == CURLOPT_SSLENGINE ||                                     \
   (option) == CURLOPT_SSLKEY ||                                        \
   (option) == CURLOPT_SSLKEYTYPE ||                                    \
   (option) == CURLOPT_SSL_CIPHER_LIST ||                               \
   (option) == CURLOPT_SSL_EC_CURVES ||                                 \
   (option) == CURLOPT_SSL_SIGNATURE_ALGORITHMS ||                      \
   (option) == CURLOPT_TLS13_CIPHERS ||                                 \
   (option) == CURLOPT_TLSAUTH_PASSWORD ||                              \
   (option) == CURLOPT_TLSAUTH_TYPE ||                                  \
   (option) == CURLOPT_TLSAUTH_USERNAME ||                              \
   (option) == CURLOPT_UNIX_SOCKET_PATH ||                              \
   (option) == CURLOPT_URL ||                                           \
   (option) == CURLOPT_USERAGENT ||                                     \
   (option) == CURLOPT_USERNAME ||                                      \
   (option) == CURLOPT_AWS_SIGV4 ||                                     \
   (option) == CURLOPT_USERPWD ||                                       \
   (option) == CURLOPT_XOAUTH2_BEARER ||                                \
   0)

/* 判断响应头或正文写回调选项。 */
#define curlcheck_write_cb_option(option)                               \
  ((option) == CURLOPT_HEADERFUNCTION ||                                \
   (option) == CURLOPT_WRITEFUNCTION)

/* 判断历史字符转换回调选项。 */
#define curlcheck_conv_cb_option(option)                                \
  ((option) == CURLOPT_CONV_TO_NETWORK_FUNCTION ||                      \
   (option) == CURLOPT_CONV_FROM_NETWORK_FUNCTION ||                    \
   (option) == CURLOPT_CONV_FROM_UTF8_FUNCTION)

/* 列举同步或后续回调使用的上下文选项，实际借用寿命沿具体选项契约。 */
#define curlcheck_cb_data_option(option)                                \
  ((option) == CURLOPT_CHUNK_DATA ||                                    \
   (option) == CURLOPT_CLOSESOCKETDATA ||                               \
   (option) == CURLOPT_DEBUGDATA ||                                     \
   (option) == CURLOPT_FNMATCH_DATA ||                                  \
   (option) == CURLOPT_HEADERDATA ||                                    \
   (option) == CURLOPT_HSTSREADDATA ||                                  \
   (option) == CURLOPT_HSTSWRITEDATA ||                                 \
   (option) == CURLOPT_INTERLEAVEDATA ||                                \
   (option) == CURLOPT_IOCTLDATA ||                                     \
   (option) == CURLOPT_OPENSOCKETDATA ||                                \
   (option) == CURLOPT_PREREQDATA ||                                    \
   (option) == CURLOPT_XFERINFODATA ||                                  \
   (option) == CURLOPT_READDATA ||                                      \
   (option) == CURLOPT_SEEKDATA ||                                      \
   (option) == CURLOPT_SOCKOPTDATA ||                                   \
   (option) == CURLOPT_SSH_KEYDATA ||                                   \
   (option) == CURLOPT_SSL_CTX_DATA ||                                  \
   (option) == CURLOPT_WRITEDATA ||                                     \
   (option) == CURLOPT_RESOLVER_START_DATA ||                           \
   (option) == CURLOPT_TRAILERDATA ||                                   \
   (option) == CURLOPT_SSH_HOSTKEYDATA ||                               \
   0)

/* 判断 POST 正文与复制 POST 正文的选项。 */
#define curlcheck_postfields_option(option)                             \
  ((option) == CURLOPT_POSTFIELDS ||                                    \
   (option) == CURLOPT_COPYPOSTFIELDS ||                                \
   0)

/* 列举接收 curl_slist 指针的选项。 */
#define curlcheck_slist_option(option)                                  \
  ((option) == CURLOPT_HTTP200ALIASES ||                                \
   (option) == CURLOPT_HTTPHEADER ||                                    \
   (option) == CURLOPT_MAIL_RCPT ||                                     \
   (option) == CURLOPT_POSTQUOTE ||                                     \
   (option) == CURLOPT_PREQUOTE ||                                      \
   (option) == CURLOPT_PROXYHEADER ||                                   \
   (option) == CURLOPT_QUOTE ||                                         \
   (option) == CURLOPT_RESOLVE ||                                       \
   (option) == CURLOPT_TELNETOPTIONS ||                                 \
   (option) == CURLOPT_CONNECT_TO ||                                    \
   0)

/* 按输出槽类型分组的信息项，分类只依赖常量编码。 */

/* 判断字符串输出组，PRIVATE 沿通用私有指针契约另行处理。 */
#define curlcheck_string_info(info)                                     \
  (CURLINFO_STRING < (info) && (info) < CURLINFO_LONG &&                \
   (info) != CURLINFO_PRIVATE)

/* 判断需要 long 输出槽的信息组。 */
#define curlcheck_long_info(info)                                       \
  (CURLINFO_LONG < (info) && (info) < CURLINFO_DOUBLE)

/* 判断需要 double 输出槽的信息组。 */
#define curlcheck_double_info(info)                                     \
  (CURLINFO_DOUBLE < (info) && (info) < CURLINFO_SLIST)

/* 判断字符串链表指针输出槽的信息项。 */
#define curlcheck_slist_info(info)                                      \
  (((info) == CURLINFO_SSL_ENGINES) ||                                  \
   ((info) == CURLINFO_COOKIELIST))

/* 判断 TLS 后端会话结构指针输出槽的信息项。 */
#define curlcheck_tlssessioninfo_info(info)                             \
  (((info) == CURLINFO_TLS_SSL_PTR) ||                                  \
   ((info) == CURLINFO_TLS_SESSION))

/* 判断证书信息结构指针输出槽的信息项。 */
#define curlcheck_certinfo_info(info) ((info) == CURLINFO_CERTINFO)

/* 判断需要 curl_socket_t 输出槽的信息组。 */
#define curlcheck_socket_info(info)                                     \
  (CURLINFO_SOCKET < (info) && (info) < CURLINFO_OFF_T)

/* 判断需要 curl_off_t 输出槽的信息组。 */
#define curlcheck_off_t_info(info)                                      \
  (CURLINFO_OFF_T < (info))

/* 以下 helper 仅按 GCC 类型兼容性和 sizeof 筛查。实参来自真实 setopt 表达式，
 * 必须保留在 typeof/sizeof 的未求值语境中，避免额外求值副作用。
 * 指针可复用 ptr/arr，其他类型需独立规则，兼容细节沿 GCC 内建类型谓词。 */

/* 仅比较指针宽度，等宽整数或数组也可能通过，调用者仍须提供有效上下文指针。 */
#define curlcheck_any_ptr(expr)                                         \
  (sizeof(expr) == sizeof(void *))

/* 与 NULL 的类型相容即可通过，未检查表达式值是否为零。 */
#define curlcheck_NULL(expr)                                            \
  (__builtin_types_compatible_p(__typeof__(expr), __typeof__(NULL)))

/* 接受 type 指针、const type 指针或与 NULL 类型相容的表达式。 */
#define curlcheck_ptr(expr, type)                                       \
  (curlcheck_NULL(expr) ||                                              \
   __builtin_types_compatible_p(__typeof__(expr), type *) ||            \
   __builtin_types_compatible_p(__typeof__(expr), const type *))

/* 接受列出的二级指针、指针数组或与 NULL 类型相容的表达式。 */
#define curlcheck_ptrptr(expr, type)                                    \
  (curlcheck_NULL(expr) ||                                              \
   __builtin_types_compatible_p(__typeof__(expr), type **) ||           \
   __builtin_types_compatible_p(__typeof__(expr), type *[]) ||          \
   __builtin_types_compatible_p(__typeof__(expr), const type *[]) ||    \
   __builtin_types_compatible_p(__typeof__(expr), const type **))

/* 在 ptr 规则外接受 type 数组，未检查数组元素数量。 */
#define curlcheck_arr(expr, type)                                       \
  (curlcheck_ptr((expr), type) ||                                       \
   __builtin_types_compatible_p(__typeof__(expr), type []))

/* 接受 char、有符号字节和无符号字节的指针/数组类型，不验证 NUL 或编码。 */
#define curlcheck_string(expr)                                          \
  (curlcheck_arr((expr), char) ||                                       \
   curlcheck_arr((expr), signed char) ||                                \
   curlcheck_arr((expr), unsigned char))

/* 接受 CURL 指针或与 NULL 类型相容的表达式。 */
#define curlcheck_curl(expr)                                            \
  (curlcheck_NULL(expr) ||                                              \
   __builtin_types_compatible_p(__typeof__(expr), CURL *))

/* long 与 int 异宽时只接收 long 族，同宽时还接收 int 及提升为 int 的短整数族。
 * 兼容有符号和无符号形式，具体数值范围仍由运行期选项决定。 */
#define curlcheck_long(expr)                                            \
  (                                                                     \
  ((sizeof(long) != sizeof(int)) &&                                     \
   (__builtin_types_compatible_p(__typeof__(expr), long) ||             \
    __builtin_types_compatible_p(__typeof__(expr), signed long) ||      \
    __builtin_types_compatible_p(__typeof__(expr), unsigned long)))     \
  ||                                                                    \
  ((sizeof(long) == sizeof(int)) &&                                     \
  (__builtin_types_compatible_p(__typeof__(expr), long) ||              \
   __builtin_types_compatible_p(__typeof__(expr), signed long) ||       \
   __builtin_types_compatible_p(__typeof__(expr), unsigned long) ||     \
   __builtin_types_compatible_p(__typeof__(expr), int) ||               \
   __builtin_types_compatible_p(__typeof__(expr), signed int) ||        \
   __builtin_types_compatible_p(__typeof__(expr), unsigned int) ||      \
   __builtin_types_compatible_p(__typeof__(expr), short) ||             \
   __builtin_types_compatible_p(__typeof__(expr), signed short) ||      \
   __builtin_types_compatible_p(__typeof__(expr), unsigned short) ||    \
   __builtin_types_compatible_p(__typeof__(expr), char) ||              \
   __builtin_types_compatible_p(__typeof__(expr), signed char) ||       \
   __builtin_types_compatible_p(__typeof__(expr), unsigned char)))      \
                                                                  )

/* 要求与 curl_off_t 的实际 typedef 类型兼容。 */
#define curlcheck_off_t(expr)                                           \
  (__builtin_types_compatible_p(__typeof__(expr), curl_off_t))

/* 只检查可写 char 指针/数组或 NULL 相容类型，容量须由调用者保证至少 CURL_ERROR_SIZE。 */
#define curlcheck_error_buffer(expr)                                    \
  (curlcheck_NULL(expr) ||                                              \
   __builtin_types_compatible_p(__typeof__(expr), char *) ||            \
   __builtin_types_compatible_p(__typeof__(expr), char[]))

/* 禁用的严格分支只接收 void/FILE 指针，当前启用分支只比较指针宽度。 */
#if 0
#define curlcheck_cb_data(expr)                                         \
  (curlcheck_ptr((expr), void) ||                                       \
   curlcheck_ptr((expr), FILE))
#else /* 保留上游较宽松的上下文兼容范围。 */
#define curlcheck_cb_data(expr)                                         \
  curlcheck_any_ptr(expr)
#endif

/* 接受 FILE 指针或与 NULL 类型相容的表达式。 */
#define curlcheck_FILE(expr)                                            \
  (curlcheck_NULL(expr) ||                                              \
   (__builtin_types_compatible_p(__typeof__(expr), FILE *)))

/* 接受 void 指针、char 或 unsigned char 数组/指针类型，正文长度和寿命另行保证。 */
#define curlcheck_postfields(expr)                                      \
  (curlcheck_ptr((expr), void) ||                                       \
   curlcheck_arr((expr), char) ||                                       \
   curlcheck_arr((expr), unsigned char))

/* 同时接受函数表达式与函数指针，使两者沿同一回调 typedef 筛查。 */
#define curlcheck_cb_compatible(func, type)                             \
  (__builtin_types_compatible_p(__typeof__(func), type) ||              \
   __builtin_types_compatible_p(__typeof__(func) *, type))

/* 筛查解析开始回调，仍接受 NULL 相容类型。 */
#define curlcheck_resolver_start_callback(expr)                         \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_resolver_start_callback))

/* 下方 typedef 供编译期匹配。实际回调线程/重入约束沿选项及驱动入口契约，
 * owner 保证句柄、缓冲和 userdata 覆盖回调寿命，并同步共享可变上下文。 */
/* 筛查上传读取回调，保留 fread 及下方六种兼容原型。 */
#define curlcheck_read_cb(expr)                                         \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), __typeof__(fread) *) ||              \
   curlcheck_cb_compatible((expr), curl_read_callback) ||               \
   curlcheck_cb_compatible((expr), Wcurl_read_callback1) ||             \
   curlcheck_cb_compatible((expr), Wcurl_read_callback2) ||             \
   curlcheck_cb_compatible((expr), Wcurl_read_callback3) ||             \
   curlcheck_cb_compatible((expr), Wcurl_read_callback4) ||             \
   curlcheck_cb_compatible((expr), Wcurl_read_callback5) ||             \
   curlcheck_cb_compatible((expr), Wcurl_read_callback6))
/** @brief 上传读取的字符缓冲兼容原型，所有指针仅借用。
 * @param[out] buffer 容纳 size*nmemb 字节的缓冲。 @param[in] size 元素字节数。
 * @param[in] nmemb 元素数。 @param[in,out] userdata 用户上下文借用。
 * @return 已填字节数、零结束或读回调控制值，资源释放沿实际 read 契约。
 */
typedef size_t (*Wcurl_read_callback1)(char *buffer, size_t size, size_t nmemb, void *userdata);
/** @brief 只读上下文的上传读取兼容原型。
 * @param[out] buffer 借用输出缓冲。 @param[in] size 元素字节数。 @param[in] nmemb 元素数。
 * @param[in] userdata 只读上下文借用。 @return 已填字节数、零或读控制值，无所有权交接。
 */
typedef size_t (*Wcurl_read_callback2)(char *buffer, size_t size, size_t nmemb, const void *userdata);
/** @brief FILE 上下文的上传读取兼容原型。
 * @param[out] buffer 借用输出缓冲。 @param[in] size 元素字节数。 @param[in] nmemb 元素数。
 * @param[in,out] stream 存活 FILE 借用，关闭由 owner 管理。 @return 已填字节数或读控制值。
 */
typedef size_t (*Wcurl_read_callback3)(char *buffer, size_t size, size_t nmemb, FILE *stream);
/** @brief void 缓冲的上传读取兼容原型。
 * @param[out] buffer 借用输出缓冲。 @param[in] size 元素字节数。 @param[in] nmemb 元素数。
 * @param[in,out] userdata 上下文借用。 @return 已填字节数或读控制值，无资源交接。
 */
typedef size_t (*Wcurl_read_callback4)(void *buffer, size_t size, size_t nmemb, void *userdata);
/** @brief void 缓冲及只读上下文的上传兼容原型。
 * @param[out] buffer 借用输出缓冲。 @param[in] size 元素字节数。 @param[in] nmemb 元素数。
 * @param[in] userdata 只读借用。 @return 已填字节数或读控制值，无资源交接。
 */
typedef size_t (*Wcurl_read_callback5)(void *buffer, size_t size, size_t nmemb, const void *userdata);
/** @brief void 缓冲及 FILE 上下文的上传兼容原型。
 * @param[out] buffer 借用输出缓冲。 @param[in] size 元素字节数。 @param[in] nmemb 元素数。
 * @param[in,out] stream FILE 借用，由 owner 关闭。 @return 已填字节数或读控制值。
 */
typedef size_t (*Wcurl_read_callback6)(void *buffer, size_t size, size_t nmemb, FILE *stream);

/* 筛查下载写回调，兼容 read 族、fwrite 和下方六种只读缓冲原型。 */
#define curlcheck_write_cb(expr)                                        \
  (curlcheck_read_cb(expr) ||                                           \
   curlcheck_cb_compatible((expr), __typeof__(fwrite) *) ||             \
   curlcheck_cb_compatible((expr), curl_write_callback) ||              \
   curlcheck_cb_compatible((expr), Wcurl_write_callback1) ||            \
   curlcheck_cb_compatible((expr), Wcurl_write_callback2) ||            \
   curlcheck_cb_compatible((expr), Wcurl_write_callback3) ||            \
   curlcheck_cb_compatible((expr), Wcurl_write_callback4) ||            \
   curlcheck_cb_compatible((expr), Wcurl_write_callback5) ||            \
   curlcheck_cb_compatible((expr), Wcurl_write_callback6))
/** @brief 只读字符下载缓冲兼容原型。
 * @param[in] buffer 回调期字节借用。 @param[in] size 元素字节数。 @param[in] nmemb 元素数。
 * @param[in,out] userdata 上下文借用。 @return 已消费字节数或写控制值，异步保留须复制缓冲。
 */
typedef size_t (*Wcurl_write_callback1)(const char *buffer, size_t size, size_t nmemb, void *userdata);
/** @brief 只读缓冲及只读上下文的下载兼容原型。
 * @param[in] buffer 回调期字节借用。 @param[in] size 元素字节数。 @param[in] nmemb 元素数。
 * @param[in] userdata 上下文借用。 @return 已消费字节数或写控制值，无资源交接。
 */
typedef size_t (*Wcurl_write_callback2)(const char *buffer, size_t size, size_t nmemb, const void *userdata);
/** @brief FILE 目的上下文的字符下载兼容原型。
 * @param[in] buffer 回调期字节借用。 @param[in] size 元素字节数。 @param[in] nmemb 元素数。
 * @param[in,out] stream FILE 借用，由 owner 关闭。 @return 已消费字节数或写控制值。
 */
typedef size_t (*Wcurl_write_callback3)(const char *buffer, size_t size, size_t nmemb, FILE *stream);
/** @brief 只读 void 下载缓冲兼容原型。
 * @param[in] buffer 回调期字节借用。 @param[in] size 元素字节数。 @param[in] nmemb 元素数。
 * @param[in,out] userdata 上下文借用。 @return 已消费字节数或写控制值，无资源交接。
 */
typedef size_t (*Wcurl_write_callback4)(const void *buffer, size_t size, size_t nmemb, void *userdata);
/** @brief 只读 void 缓冲和上下文的下载兼容原型。
 * @param[in] buffer 回调期字节借用。 @param[in] size 元素字节数。 @param[in] nmemb 元素数。
 * @param[in] userdata 上下文借用。 @return 已消费字节数或写控制值，无资源交接。
 */
typedef size_t (*Wcurl_write_callback5)(const void *buffer, size_t size, size_t nmemb, const void *userdata);
/** @brief FILE 目的上下文的 void 下载缓冲兼容原型。
 * @param[in] buffer 回调期字节借用。 @param[in] size 元素字节数。 @param[in] nmemb 元素数。
 * @param[in,out] stream FILE 借用，由 owner 关闭。 @return 已消费字节数或写控制值。
 */
typedef size_t (*Wcurl_write_callback6)(const void *buffer, size_t size, size_t nmemb, FILE *stream);

/* 筛查历史 IO 控制回调及四种命令/上下文变体。 */
#define curlcheck_ioctl_cb(expr)                                        \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_ioctl_callback) ||              \
   curlcheck_cb_compatible((expr), Wcurl_ioctl_callback1) ||            \
   curlcheck_cb_compatible((expr), Wcurl_ioctl_callback2) ||            \
   curlcheck_cb_compatible((expr), Wcurl_ioctl_callback3) ||            \
   curlcheck_cb_compatible((expr), Wcurl_ioctl_callback4))
/** @brief 整数命令的历史 IO 控制兼容原型。
 * @param[in] easy 回调期句柄借用。 @param[in] command 操作命令。
 * @param[in,out] userdata 上下文借用。 @return CURLIOE_* 状态，无所有权交接。
 */
typedef curlioerr (*Wcurl_ioctl_callback1)(CURL *easy, int command, void *userdata);
/** @brief 整数命令和只读上下文的历史 IO 兼容原型。
 * @param[in] easy 句柄借用。 @param[in] command 操作命令。 @param[in] userdata 上下文借用。
 * @return CURLIOE_* 状态，所有指针寿命沿实际回调契约。
 */
typedef curlioerr (*Wcurl_ioctl_callback2)(CURL *easy, int command, const void *userdata);
/** @brief 枚举命令的历史 IO 控制兼容原型。
 * @param[in] easy 句柄借用。 @param[in] command 操作命令。 @param[in,out] userdata 上下文借用。
 * @return CURLIOE_* 状态，无所有权交接。
 */
typedef curlioerr (*Wcurl_ioctl_callback3)(CURL *easy, curliocmd command, void *userdata);
/** @brief 枚举命令和只读上下文的历史 IO 兼容原型。
 * @param[in] easy 句柄借用。 @param[in] command 操作命令。 @param[in] userdata 上下文借用。
 * @return CURLIOE_* 状态，无所有权交接。
 */
typedef curlioerr (*Wcurl_ioctl_callback4)(CURL *easy, curliocmd command, const void *userdata);

/* 筛查 socket 选项回调及两种上下文变体。 */
#define curlcheck_sockopt_cb(expr)                                      \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_sockopt_callback) ||            \
   curlcheck_cb_compatible((expr), Wcurl_sockopt_callback1) ||          \
   curlcheck_cb_compatible((expr), Wcurl_sockopt_callback2))
/** @brief socket 选项回调的可写上下文兼容原型。
 * @param[in,out] userdata 上下文借用。 @param[in] socket_fd libcurl socket 借用。
 * @param[in] purpose socket 用途。 @return CURL_SOCKOPT_* 状态，不接管 socket 所有权。
 */
typedef int (*Wcurl_sockopt_callback1)(void *userdata, curl_socket_t socket_fd, curlsocktype purpose);
/** @brief socket 选项回调的只读上下文兼容原型。
 * @param[in] userdata 上下文借用。 @param[in] socket_fd libcurl socket 借用。
 * @param[in] purpose socket 用途。 @return CURL_SOCKOPT_* 状态，无资源交接。
 */
typedef int (*Wcurl_sockopt_callback2)(const void *userdata, curl_socket_t socket_fd, curlsocktype purpose);

/* 筛查 socket 创建回调及地址/上下文的 const 兼容变体。 */
#define curlcheck_opensocket_cb(expr)                                   \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_opensocket_callback) ||         \
   curlcheck_cb_compatible((expr), Wcurl_opensocket_callback1) ||       \
   curlcheck_cb_compatible((expr), Wcurl_opensocket_callback2) ||       \
   curlcheck_cb_compatible((expr), Wcurl_opensocket_callback3) ||       \
   curlcheck_cb_compatible((expr), Wcurl_opensocket_callback4))
/** @brief 创建 socket 的可写地址兼容原型。
 * @param[in,out] userdata 上下文借用。 @param[in] purpose socket 用途。
 * @param[in,out] address 回调期地址借用。 @return 新 socket 或 CURL_SOCKET_BAD，成功句柄交 libcurl 关闭流程。
 */
typedef curl_socket_t (*Wcurl_opensocket_callback1)
  (void *userdata, curlsocktype purpose, struct curl_sockaddr *address);
/** @brief 创建 socket 的只读地址兼容原型。
 * @param[in,out] userdata 上下文借用。 @param[in] purpose socket 用途。 @param[in] address 地址借用。
 * @return 新 socket 或 CURL_SOCKET_BAD，成功交 libcurl 关闭流程。
 */
typedef curl_socket_t (*Wcurl_opensocket_callback2)
  (void *userdata, curlsocktype purpose, const struct curl_sockaddr *address);
/** @brief 创建 socket 的只读上下文兼容原型。
 * @param[in] userdata 上下文借用。 @param[in] purpose socket 用途。 @param[in,out] address 地址借用。
 * @return 新 socket 或 CURL_SOCKET_BAD，成功交 libcurl 关闭流程。
 */
typedef curl_socket_t (*Wcurl_opensocket_callback3)
  (const void *userdata, curlsocktype purpose, struct curl_sockaddr *address);
/** @brief 创建 socket 的只读上下文和地址兼容原型。
 * @param[in] userdata 上下文借用。 @param[in] purpose socket 用途。 @param[in] address 地址借用。
 * @return 新 socket 或 CURL_SOCKET_BAD，成功交 libcurl 关闭流程。
 */
typedef curl_socket_t (*Wcurl_opensocket_callback4)
  (const void *userdata, curlsocktype purpose, const struct curl_sockaddr *address);

/* 筛查历史 double 进度回调及两种上下文变体。 */
#define curlcheck_progress_cb(expr)                                     \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_progress_callback) ||           \
   curlcheck_cb_compatible((expr), Wcurl_progress_callback1) ||         \
   curlcheck_cb_compatible((expr), Wcurl_progress_callback2))
/** @brief 历史 double 进度回调兼容原型。
 * @param[in,out] userdata 上下文借用。 @param[in] download_total 下载总量。 @param[in] download_now 已下载量。
 * @param[in] upload_total 上传总量。 @param[in] upload_now 已上传量。 @return 零继续，非零中止，无资源交接。
 */
typedef int (*Wcurl_progress_callback1)(void *userdata,
    double download_total, double download_now, double upload_total, double upload_now);
/** @brief 只读上下文的 double 进度兼容原型。
 * @param[in] userdata 上下文借用。 @param[in] download_total 下载总量。 @param[in] download_now 已下载量。
 * @param[in] upload_total 上传总量。 @param[in] upload_now 已上传量。 @return 零继续，非零中止，无资源交接。
 */
typedef int (*Wcurl_progress_callback2)(const void *userdata,
    double download_total, double download_now, double upload_total, double upload_now);

/* 筛查 curl_off_t 进度回调，仍接受 NULL 相容类型。 */
#define curlcheck_xferinfo_cb(expr)                                     \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_xferinfo_callback))

/* 筛查调试回调及字符符号性/const 上下文变体。 */
#define curlcheck_debug_cb(expr)                                        \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_debug_callback) ||              \
   curlcheck_cb_compatible((expr), Wcurl_debug_callback1) ||            \
   curlcheck_cb_compatible((expr), Wcurl_debug_callback2) ||            \
   curlcheck_cb_compatible((expr), Wcurl_debug_callback3) ||            \
   curlcheck_cb_compatible((expr), Wcurl_debug_callback4) ||            \
   curlcheck_cb_compatible((expr), Wcurl_debug_callback5) ||            \
   curlcheck_cb_compatible((expr), Wcurl_debug_callback6) ||            \
   curlcheck_cb_compatible((expr), Wcurl_debug_callback7) ||            \
   curlcheck_cb_compatible((expr), Wcurl_debug_callback8))
/** @brief 字符调试数据兼容原型，缓冲只借用至回调返回。
 * @param[in] easy 句柄借用。 @param[in] type 数据种类。 @param[in] data 借用字节，无 NUL 保证。
 * @param[in] size 字节数。 @param[in,out] userdata 上下文借用。 @return 按调试协议返回零，libcurl 忽略返回值。
 */
typedef int (*Wcurl_debug_callback1)(CURL *easy, curl_infotype type, char *data, size_t size, void *userdata);
/** @brief 字符数据和只读上下文的调试兼容原型。
 * @param[in] easy 句柄借用。 @param[in] type 数据种类。 @param[in] data 回调期字节借用。
 * @param[in] size 字节数。 @param[in] userdata 上下文借用。 @return 零，libcurl 忽略返回值，无资源交接。
 */
typedef int (*Wcurl_debug_callback2)(CURL *easy, curl_infotype type, char *data, size_t size, const void *userdata);
/** @brief 只读字符数据的调试兼容原型。
 * @param[in] easy 句柄借用。 @param[in] type 数据种类。 @param[in] data 回调期字节借用。
 * @param[in] size 字节数。 @param[in,out] userdata 上下文借用。 @return 零，libcurl 忽略返回值。
 */
typedef int (*Wcurl_debug_callback3)(CURL *easy, curl_infotype type, const char *data, size_t size, void *userdata);
/** @brief 只读字符和上下文的调试兼容原型。
 * @param[in] easy 句柄借用。 @param[in] type 数据种类。 @param[in] data 回调期字节借用。
 * @param[in] size 字节数。 @param[in] userdata 上下文借用。 @return 零，libcurl 忽略返回值，无资源交接。
 */
typedef int (*Wcurl_debug_callback4)(CURL *easy, curl_infotype type, const char *data, size_t size, const void *userdata);
/** @brief 无符号字节数据的调试兼容原型。
 * @param[in] easy 句柄借用。 @param[in] type 数据种类。 @param[in] data 回调期字节借用。
 * @param[in] size 字节数。 @param[in,out] userdata 上下文借用。 @return 零，libcurl 忽略返回值。
 */
typedef int (*Wcurl_debug_callback5)(CURL *easy, curl_infotype type, unsigned char *data, size_t size, void *userdata);
/** @brief 无符号字节和只读上下文的调试兼容原型。
 * @param[in] easy 句柄借用。 @param[in] type 数据种类。 @param[in] data 回调期字节借用。
 * @param[in] size 字节数。 @param[in] userdata 上下文借用。 @return 零，libcurl 忽略返回值。
 */
typedef int (*Wcurl_debug_callback6)(CURL *easy, curl_infotype type, unsigned char *data, size_t size, const void *userdata);
/** @brief 只读无符号数据的调试兼容原型。
 * @param[in] easy 句柄借用。 @param[in] type 数据种类。 @param[in] data 回调期字节借用。
 * @param[in] size 字节数。 @param[in,out] userdata 上下文借用。 @return 零，libcurl 忽略返回值。
 */
typedef int (*Wcurl_debug_callback7)(CURL *easy, curl_infotype type, const unsigned char *data, size_t size, void *userdata);
/** @brief 只读无符号数据和上下文的调试兼容原型。
 * @param[in] easy 句柄借用。 @param[in] type 数据种类。 @param[in] data 回调期字节借用。
 * @param[in] size 字节数。 @param[in] userdata 上下文借用。 @return 零，libcurl 忽略返回值，无资源交接。
 */
typedef int (*Wcurl_debug_callback8)(CURL *easy, curl_infotype type, const unsigned char *data, size_t size, const void *userdata);

/* 筛查 TLS 上下文回调，下方变体保留 TLS 后端和 const 兼容范围。 */
#define curlcheck_ssl_ctx_cb(expr)                                      \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_ssl_ctx_callback) ||            \
   curlcheck_cb_compatible((expr), Wcurl_ssl_ctx_callback1) ||          \
   curlcheck_cb_compatible((expr), Wcurl_ssl_ctx_callback2) ||          \
   curlcheck_cb_compatible((expr), Wcurl_ssl_ctx_callback3) ||          \
   curlcheck_cb_compatible((expr), Wcurl_ssl_ctx_callback4) ||          \
   curlcheck_cb_compatible((expr), Wcurl_ssl_ctx_callback5) ||          \
   curlcheck_cb_compatible((expr), Wcurl_ssl_ctx_callback6) ||          \
   curlcheck_cb_compatible((expr), Wcurl_ssl_ctx_callback7) ||          \
   curlcheck_cb_compatible((expr), Wcurl_ssl_ctx_callback8))
/** @brief 不透明 TLS 上下文的初始化兼容原型。
 * @param[in] easy 句柄借用。 @param[in,out] ssl_context TLS 后端上下文借用。
 * @param[in,out] userdata 用户上下文借用。 @return CURLE_OK 继续，其他 CURLcode 中止，无所有权交接。
 */
typedef CURLcode (*Wcurl_ssl_ctx_callback1)(CURL *easy, void *ssl_context, void *userdata);
/** @brief 只读用户上下文的 TLS 初始化兼容原型。
 * @param[in] easy 句柄借用。 @param[in,out] ssl_context 后端上下文借用。 @param[in] userdata 用户借用。
 * @return CURLE_OK 继续，其他中止，指针均只借用。
 */
typedef CURLcode (*Wcurl_ssl_ctx_callback2)(CURL *easy, void *ssl_context, const void *userdata);
/** @brief 只读后端上下文的 TLS 兼容原型。
 * @param[in] easy 句柄借用。 @param[in] ssl_context 后端借用。 @param[in,out] userdata 用户借用。
 * @return CURLE_OK 继续，其他中止，无资源交接。
 */
typedef CURLcode (*Wcurl_ssl_ctx_callback3)(CURL *easy, const void *ssl_context, void *userdata);
/** @brief 两类上下文均只读的 TLS 兼容原型。
 * @param[in] easy 句柄借用。 @param[in] ssl_context 后端借用。 @param[in] userdata 用户借用。
 * @return CURLE_OK 继续，其他中止，无资源交接。
 */
typedef CURLcode (*Wcurl_ssl_ctx_callback4)(CURL *easy, const void *ssl_context, const void *userdata);
#ifdef HEADER_SSL_H
/* 仅当此前已包含 OpenSSL 头且定义 HEADER_SSL_H 时可使用 SSL_CTX 原型。
 * 本头先包含时采用下方 void 指针别名，兼容结果因此受包含顺序影响。 */
/** @brief OpenSSL 可写上下文兼容原型。
 * @param[in] easy 句柄借用。 @param[in,out] ssl_context SSL_CTX 借用，禁止释放。
 * @param[in,out] userdata 用户借用。 @return CURLE_OK 继续，其他中止。
 */
typedef CURLcode (*Wcurl_ssl_ctx_callback5)(CURL *easy, SSL_CTX *ssl_context, void *userdata);
/** @brief OpenSSL 上下文与只读用户数据兼容原型。
 * @param[in] easy 句柄借用。 @param[in,out] ssl_context SSL_CTX 借用。 @param[in] userdata 用户借用。
 * @return CURLE_OK 继续，其他中止，SSL_CTX 仍由 TLS owner 管理。
 */
typedef CURLcode (*Wcurl_ssl_ctx_callback6)(CURL *easy, SSL_CTX *ssl_context, const void *userdata);
/** @brief 只读 OpenSSL 上下文兼容原型。
 * @param[in] easy 句柄借用。 @param[in] ssl_context SSL_CTX 借用。 @param[in,out] userdata 用户借用。
 * @return CURLE_OK 继续，其他中止，无资源交接。
 */
typedef CURLcode (*Wcurl_ssl_ctx_callback7)(CURL *easy, const SSL_CTX *ssl_context, void *userdata);
/** @brief OpenSSL 与用户上下文均只读的兼容原型。
 * @param[in] easy 句柄借用。 @param[in] ssl_context SSL_CTX 借用。 @param[in] userdata 用户借用。
 * @return CURLE_OK 继续，其他中止，无资源交接。
 */
typedef CURLcode (*Wcurl_ssl_ctx_callback8)(CURL *easy, const SSL_CTX *ssl_context, const void *userdata);
#else
/** @brief 无 OpenSSL 类型时沿 callback1 原型及相同借用/返回契约。 */
typedef Wcurl_ssl_ctx_callback1 Wcurl_ssl_ctx_callback5;
/** @brief 无 OpenSSL 类型时保留上游 callback1 别名，const 范围沿别名实际原型。 */
typedef Wcurl_ssl_ctx_callback1 Wcurl_ssl_ctx_callback6;
/** @brief 无 OpenSSL 类型时沿 callback1 原型，后端上下文均不交接所有权。 */
typedef Wcurl_ssl_ctx_callback1 Wcurl_ssl_ctx_callback7;
/** @brief 无 OpenSSL 类型时沿 callback1 原型及 CURLcode 返回契约。 */
typedef Wcurl_ssl_ctx_callback1 Wcurl_ssl_ctx_callback8;
#endif

/* 筛查历史字符转换回调及四种缓冲 const/void 变体。 */
#define curlcheck_conv_cb(expr)                                         \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_conv_callback) ||               \
   curlcheck_cb_compatible((expr), Wcurl_conv_callback1) ||             \
   curlcheck_cb_compatible((expr), Wcurl_conv_callback2) ||             \
   curlcheck_cb_compatible((expr), Wcurl_conv_callback3) ||             \
   curlcheck_cb_compatible((expr), Wcurl_conv_callback4))
/** @brief 字符转换的可写字符缓冲兼容原型。
 * @param[in,out] buffer 回调期借用缓冲，原位转换。 @param[in] length 字节数。
 * @return CURLE_OK 成功，其他转换错误，无资源交接。
 */
typedef CURLcode (*Wcurl_conv_callback1)(char *buffer, size_t length);
/** @brief 字符转换的只读字符兼容原型，保留上游筛查范围。
 * @param[in] buffer 回调期借用缓冲。 @param[in] length 字节数。 @return CURLcode 转换结果，无资源交接。
 */
typedef CURLcode (*Wcurl_conv_callback2)(const char *buffer, size_t length);
/** @brief 字符转换的可写 void 缓冲兼容原型。
 * @param[in,out] buffer 回调期借用缓冲。 @param[in] length 字节数。 @return CURLcode 转换结果，无资源交接。
 */
typedef CURLcode (*Wcurl_conv_callback3)(void *buffer, size_t length);
/** @brief 字符转换的只读 void 缓冲兼容原型。
 * @param[in] buffer 回调期借用缓冲。 @param[in] length 字节数。 @return CURLcode 转换结果，无资源交接。
 */
typedef CURLcode (*Wcurl_conv_callback4)(const void *buffer, size_t length);

/* 筛查上传定位回调，保留返回 CURLcode 的既有兼容变体。 */
#define curlcheck_seek_cb(expr)                                         \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_seek_callback) ||               \
   curlcheck_cb_compatible((expr), Wcurl_seek_callback1) ||             \
   curlcheck_cb_compatible((expr), Wcurl_seek_callback2))
/** @brief 上传定位的既有 CURLcode 返回兼容原型。
 * @param[in,out] userdata 数据源上下文借用。 @param[in] offset 定位偏移。 @param[in] origin 定位基准。
 * @return 实际定位协议使用 CURL_SEEKFUNC_* 结果，本 typedef 保留上游 CURLcode 返回类型。
 */
typedef CURLcode (*Wcurl_seek_callback1)(void *userdata, curl_off_t offset, int origin);
/** @brief 只读数据源上下文的定位兼容原型。
 * @param[in] userdata 上下文借用。 @param[in] offset 定位偏移。 @param[in] origin 定位基准。
 * @return 沿 CURL_SEEKFUNC_* 协议解释兼容返回类型，无资源交接。
 */
typedef CURLcode (*Wcurl_seek_callback2)(const void *userdata, curl_off_t offset, int origin);

/* 筛查通配下载文件块开始回调及信息指针变体。 */
#define curlcheck_chunk_bgn_cb(expr)                                    \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_chunk_bgn_callback) ||          \
   curlcheck_cb_compatible((expr), Wcurl_chunk_bgn_callback1) ||        \
   curlcheck_cb_compatible((expr), Wcurl_chunk_bgn_callback2))
/** @brief 通配下载文件块开始的信息结构兼容原型。
 * @param[in] file_info 回调期文件信息借用。 @param[in,out] userdata 用户借用。 @param[in] remains 剩余文件数。
 * @return CURL_CHUNK_BGN_FUNC_* 结果，异步保存信息须复制，禁止释放借用结构。
 */
typedef long (*Wcurl_chunk_bgn_callback1)(struct curl_fileinfo *file_info, void *userdata, int remains);
/** @brief 不透明文件信息的块开始兼容原型。
 * @param[in] file_info 回调期信息借用。 @param[in,out] userdata 用户借用。 @param[in] remains 剩余文件数。
 * @return CURL_CHUNK_BGN_FUNC_* 结果，无所有权交接。
 */
typedef long (*Wcurl_chunk_bgn_callback2)(void *file_info, void *userdata, int remains);

/* 筛查文件块结束回调。 */
#define curlcheck_chunk_end_cb(expr)                                    \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_chunk_end_callback))

/* 筛查 socket 关闭回调。 */
#define curlcheck_close_socket_cb(expr)                                 \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_closesocket_callback))

/* 筛查文件名匹配回调。 */
#define curlcheck_fnmatch_cb(expr)                                      \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_fnmatch_callback))

/* 筛查 HSTS 读取回调。 */
#define curlcheck_hstsread_cb(expr)                                     \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_hstsread_callback))

/* 筛查 HSTS 写入回调。 */
#define curlcheck_hstswrite_cb(expr)                                    \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_hstswrite_callback))

/* 筛查 SSH 主机公钥回调。 */
#define curlcheck_ssh_hostkey_cb(expr)                                  \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_sshhostkeycallback))

/* 筛查 SSH 已知主机密钥核对回调。 */
#define curlcheck_ssh_key_cb(expr)                                      \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_sshkeycallback))

/* 筛查交错协议数据回调及两种缓冲原型。 */
#define curlcheck_interleave_cb(expr)                                   \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), Wcurl_interleave_callback1) ||       \
   curlcheck_cb_compatible((expr), Wcurl_interleave_callback2))
/** @brief void 缓冲的交错协议数据兼容原型。
 * @param[in] buffer 回调期字节借用。 @param[in] size 元素字节数。 @param[in] nmemb 元素数。
 * @param[in,out] userdata 用户借用。 @return 实际消费字节数，未完整接收沿协议报错，无资源交接。
 */
typedef size_t (*Wcurl_interleave_callback1)(void *buffer, size_t size, size_t nmemb, void *userdata);
/** @brief 字符缓冲的交错协议数据兼容原型。
 * @param[in] buffer 回调期字节借用。 @param[in] size 元素字节数。 @param[in] nmemb 元素数。
 * @param[in,out] userdata 用户借用。 @return 实际消费字节数，无所有权交接。
 */
typedef size_t (*Wcurl_interleave_callback2)(char *buffer, size_t size, size_t nmemb, void *userdata);

/* 筛查连接建立后的请求前置回调。 */
#define curlcheck_prereq_cb(expr)                                       \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_prereq_callback))

/* 筛查请求尾部头字段生成回调。 */
#define curlcheck_trailer_cb(expr)                                      \
  (curlcheck_NULL(expr) ||                                              \
   curlcheck_cb_compatible((expr), curl_trailer_callback))

#endif /* GCC 类型筛查头保护结束。 */
