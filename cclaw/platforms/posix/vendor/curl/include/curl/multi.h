#ifndef CURLINC_MULTI_H
#define CURLINC_MULTI_H
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
/* multi 的公开拉取式接口，由应用选择驱动、等待和回调时机。
 * 同线程可管理多个并发传输，并把应用自身fd与curl的等待对象合并。
 * 此头只暴露公开ABI，multi/easy及应用回调的资源寿命由各接口约束。 */

/* 常规入口curl.h已包含本头，此处沿上游保留直接包含multi.h的依赖顺序。 */
#include "curl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void CURLM;

typedef enum {
  CURLM_CALL_MULTI_PERFORM = -1, /* 旧版本要求尽快再次驱动perform/socket。 */
  CURLM_OK,
  CURLM_BAD_HANDLE,      /* multi句柄无效。 */
  CURLM_BAD_EASY_HANDLE, /* easy句柄无效。 */
  CURLM_OUT_OF_MEMORY,   /* 分配失败。 */
  CURLM_INTERNAL_ERROR,  /* 库内部状态错误。 */
  CURLM_BAD_SOCKET,      /* socket与当前状态不匹配。 */
  CURLM_UNKNOWN_OPTION,  /* multi选项不受支持。 */
  CURLM_ADDED_ALREADY,   /* easy已属于multi，重复加入。 */
  CURLM_RECURSIVE_API_CALL, /* 在回调内执行了受限制的重入调用。 */
  CURLM_WAKEUP_FAILURE,  /* 唤醒能力不可用或唤醒失败。 */
  CURLM_BAD_FUNCTION_ARGUMENT, /* 参数无效。 */
  CURLM_ABORTED_BY_CALLBACK,
  CURLM_UNRECOVERABLE_POLL,
  CURLM_LAST
} CURLMcode;

/* socket驱动沿同一旧式再次驱动结果值提供兼容名称。 */
#define CURLM_CALL_MULTI_SOCKET CURLM_CALL_MULTI_PERFORM

/* CURLMOPT_PIPELINING 的历史流水线/多路复用位，实际能力由链接库决定。 */
#define CURLPIPE_NOTHING   0L
#define CURLPIPE_HTTP1     1L
#define CURLPIPE_MULTIPLEX 2L

typedef enum {
  CURLMSG_NONE, /* 枚举起始占位。 */
  CURLMSG_DONE, /* easy传输完成，data.result保存该传输结果。 */
  CURLMSG_LAST /* 枚举末尾占位。 */
} CURLMSG;

struct CURLMsg {
  CURLMSG msg;       /* 本消息种类。 */
  CURL *easy_handle; /* 消息对应的easy句柄借用。 */
  union {
    void *whatever;    /* 对应消息种类的数据。 */
    CURLcode result;   /* 单次传输结果码。 */
  } data;
};
typedef struct CURLMsg CURLMsg;

/* 以poll语义定义跨平台等待位，结构独立于系统pollfd与POLL*常量。 */
#define CURL_WAIT_POLLIN    0x0001
#define CURL_WAIT_POLLPRI   0x0002
#define CURL_WAIT_POLLOUT   0x0004

struct curl_waitfd {
  curl_socket_t fd;
  short events;
  short revents;
};

/**
 * @brief 创建管理多条传输的multi句柄。
 * @return 成功为调用者拥有的CURLM指针，失败为NULL，最终multi_cleanup。
 * @note 拥有者串行驱动同一multi并协调回调借用，进程级初始化另行安排。
 */
CURL_EXTERN CURLM *curl_multi_init(void);

/**
 * @brief 将easy句柄加入multi的传输集合。
 * @param[in,out] m 存活且由调用者串行操作的multi句柄借用。
 * @param[in,out] curl 有效且尚未加入任何multi的easy句柄借用。
 * @return CURLM_OK或重复加入、参数、资源等错误。
 * @note 成功后multi借用easy至remove，easy最终清理由其拥有者负责。
 */
CURL_EXTERN CURLMcode curl_multi_add_handle(CURLM *m,
                                            CURL *curl);

/**
 * @brief 从multi移出easy，解除该管理关系。
 * @param[in,out] m 当前multi句柄借用，与驱动和清理串行。
 * @param[in,out] curl 属于该multi的easy句柄借用，移出后由应用继续保管。
 * @return CURLM_OK或句柄、参数、重入等错误。
 * @note 活动传输可因此停止，移出后应用决定重用或easy_cleanup。
 */
CURL_EXTERN CURLMcode curl_multi_remove_handle(CURLM *m,
                                               CURL *curl);

/**
 * @brief 把当前multi需要等待的socket加入调用者的select集合。
 * @param[in] m 存活且当前访问串行的multi借用。
 * @param[in,out] read_fd_set 已初始化的可读fd集合。
 * @param[in,out] write_fd_set 已初始化的可写fd集合。
 * @param[in,out] exc_fd_set 已初始化的异常fd集合。
 * @param[out] max_fd 必需的最大描述符槽，无可等待socket时为-1。
 * @return CURLM_OK或句柄/参数错误。
 * @note 应用合并自有fd并结合multi_timeout，集合容量受平台fd_set限制。
 */
CURL_EXTERN CURLMcode curl_multi_fdset(CURLM *m,
                                       fd_set *read_fd_set,
                                       fd_set *write_fd_set,
                                       fd_set *exc_fd_set,
                                       int *max_fd);

/**
 * @brief 等待multi与额外fd的活动，供应用随后驱动传输。
 * @param[in] m 调用期间存活且由本线程使用的multi借用。
 * @param[in,out] extra_fds 调用者等待项数组，零项时允许NULL，revents返回就绪位。
 * @param[in] extra_nfds 额外等待项数量。
 * @param[in] timeout_ms 最大等待毫秒数，须非负，内部更早期限可缩短等待。
 * @param[out] ret 可选就绪描述符数量槽，允许NULL。
 * @return CURLM_OK或等待/参数错误。
 * @note 无任何可等待fd时可能立即返回，此函数只等待，应用继续驱动perform。
 */
CURL_EXTERN CURLMcode curl_multi_wait(CURLM *m,
                                      struct curl_waitfd extra_fds[],
                                      unsigned int extra_nfds,
                                      int timeout_ms,
                                      int *ret);

/**
 * @brief 等待multi和额外fd，支持跨线程wakeup中断本次等待。
 * @param[in] m 调用期间存活的multi借用，除专用wakeup外其他操作由owner串行。
 * @param[in,out] extra_fds 额外等待数组借用，零项允许NULL，返回各revents。
 * @param[in] extra_nfds 额外等待项数。
 * @param[in] timeout_ms 非负最大等待毫秒数，受库内部更早计时约束。
 * @param[out] ret 可选就绪fd数量槽，允许NULL。
 * @return CURLM_OK或参数/等待错误。
 * @note 无fd时仍按超时等待，wakeup仅影响此poll路径。
 */
CURL_EXTERN CURLMcode curl_multi_poll(CURLM *m,
                                      struct curl_waitfd extra_fds[],
                                      unsigned int extra_nfds,
                                      int timeout_ms,
                                      int *ret);

/**
 * @brief 唤醒该multi正在或随后进入的poll等待。
 * @param[in] m 由拥有者保证整个调用期存活的multi借用。
 * @return CURLM_OK或唤醒失败/能力错误。
 * @note 此专用入口允许其他线程调用，拥有者仍须防止与multi_cleanup并发。
 */
CURL_EXTERN CURLMcode curl_multi_wakeup(CURLM *m);

/**
 * @brief 驱动当前可推进的multi传输并同步派发回调。
 * @param[in,out] m 当前owner使用的multi借用。
 * @param[out] running_handles 必需的尚未完成传输数量槽。
 * @return multi整体结果码，单个传输完成与错误由info_read消费。
 * @note 应用按socket就绪与库计时持续驱动，所有回调上下文覆盖驱动调用期。
 */
CURL_EXTERN CURLMcode curl_multi_perform(CURLM *m,
                                         int *running_handles);

/**
 * @brief 清理multi拥有的连接缓存和管理资源。
 * @param[in] m 待消费的multi拥有句柄，先结束驱动及跨线程wakeup使用。
 * @return CURLM_OK或清理错误，调用后依实际结果结束该句柄生命周期。
 * @note 拥有者先remove各easy并分别cleanup，multi清理可能同步派发socket回调。
 */
CURL_EXTERN CURLMcode curl_multi_cleanup(CURLM *m);

/**
 * @brief 从multi的完成消息队列取出下一项。
 * @param[in,out] m 存活且当前访问串行的multi借用。
 * @param[out] msgs_in_queue 必需的本次取出后剩余消息数槽。
 * @return 库拥有的完成消息借用，当前没有消息时返回NULL。
 * @note 每次调用消费一项，应用读取easy_handle及data.result，长期保留须复制。
 *       移出相关easy或清理easy/multi会结束相应借用寿命。
 */
CURL_EXTERN CURLMsg *curl_multi_info_read(CURLM *m,
                                          int *msgs_in_queue);

/**
 * @brief 将multi整体结果码转换为诊断文本。
 * @param[in] error 待说明的CURLMcode。
 * @return 库拥有的只读NUL结尾字符串借用，调用者保持内容原样。
 */
CURL_EXTERN const char *curl_multi_strerror(CURLMcode error);

/* socket回调向应用交付等待兴趣，应用再以CSELECT就绪位驱动socket_action。 */
#define CURL_POLL_NONE   0
#define CURL_POLL_IN     1
#define CURL_POLL_OUT    2
#define CURL_POLL_INOUT  3
#define CURL_POLL_REMOVE 4

#define CURL_SOCKET_TIMEOUT CURL_SOCKET_BAD

#define CURL_CSELECT_IN   0x01
#define CURL_CSELECT_OUT  0x02
#define CURL_CSELECT_ERR  0x04

/**
 * @brief 通知应用某socket当前需要监听的读写兴趣或移除监听。
 * @param[in] easy 相关传输句柄借用，也可能为库内部句柄。
 * @param[in] s 对应的socket或内部通信描述符。
 * @param[in] what CURL_POLL_*兴趣，REMOVE表示移除本次监听。
 * @param[in,out] userp SOCKETDATA设置的应用上下文借用。
 * @param[in,out] socketp multi_assign关联的socket上下文借用，可为NULL。
 * @return 成功返回0，错误返回-1并使当前multi中的在途传输失败。
 * @note cleanup关闭缓存连接时仍可调用，应用上下文覆盖最后回调退出。
 */
typedef int (*curl_socket_callback)(CURL *easy,
                                    curl_socket_t s,
                                    int what,
                                    void *userp,
                                    void *socketp);
/**
 * @brief 通知应用更新下一次驱动multi所需的计时器。
 * @param[in] m 当前multi句柄借用。
 * @param[in] timeout_ms 最大等待毫秒数，零安排尽快驱动，-1删除旧计时器。
 * @param[in,out] userp TIMERDATA设置的应用上下文借用。
 * @return 成功为0，回调失败按链接库的中止语义处理。
 * @note 应用替换旧计时器，零超时通过事件循环调度，保持驱动和清理的重入约束。
 */
typedef int (*curl_multi_timer_callback)(CURLM *m,
                                         long timeout_ms,
                                         void *userp);

/**
 * @brief 沿旧式socket接口驱动指定描述符。
 * @param[in,out] m 当前串行操作的multi借用。
 * @param[in] s 就绪socket或TIMEOUT标记。
 * @param[out] running_handles 必需的仍运行传输数槽。
 * @return multi整体结果码，单个传输结果读取info_read。
 * @note 此声明自7.19.5弃用，缺省同名宏转发为ev_bitmask为零的socket_action。
 */
CURL_EXTERN CURLMcode CURL_DEPRECATED(7.19.5, "Use curl_multi_socket_action()")
curl_multi_socket(CURLM *m, curl_socket_t s, int *running_handles);

/**
 * @brief 根据一个socket的就绪活动或库计时事件推进相关传输。
 * @param[in,out] m 当前owner独占调用期的multi借用。
 * @param[in] s 已通知的socket，CURL_SOCKET_TIMEOUT表示计时驱动。
 * @param[in] ev_bitmask CURL_CSELECT_IN/OUT/ERR组合，计时驱动按接口约定传零。
 * @param[out] running_handles 必需的仍运行传输数槽。
 * @return multi整体结果码，完成消息另由info_read读取。
 * @note 应用按socket与timer回调维护兴趣，再继续驱动，回调上下文覆盖调用。
 */
CURL_EXTERN CURLMcode curl_multi_socket_action(CURLM *m,
                                               curl_socket_t s,
                                               int ev_bitmask,
                                               int *running_handles);

/**
 * @brief 沿旧式接口检查所有multi socket并推进传输。
 * @param[in,out] m 存活且串行驱动的multi借用。
 * @param[out] running_handles 必需的仍运行传输数槽。
 * @return multi整体结果码。
 * @note 自7.19.5弃用，保留公开ABI声明，单传输结果继续通过完成消息解释。
 */
CURL_EXTERN CURLMcode CURL_DEPRECATED(7.19.5, "Use curl_multi_socket_action()")
curl_multi_socket_all(CURLM *m, int *running_handles);

#ifndef CURL_ALLOW_OLD_MULTI_SOCKET
/**
 * @brief 将旧socket调用转发到socket_action，提供零就绪位。
 * @param[in,out] x multi借用，对应新接口m。
 * @param[in] y socket或TIMEOUT标记，对应新接口s。
 * @param[out] z 仍运行传输数槽，对应running_handles。
 * @return socket_action的multi整体结果码，各实参在展开中出现一次。
 */
#define curl_multi_socket(x, y, z) curl_multi_socket_action(x, y, 0, z)
#endif

/**
 * @brief 查询multi下一次应被驱动前允许的最大等待时间。
 * @param[in] m 当前访问串行的multi借用。
 * @param[out] timeout_ms 必需的毫秒输出槽，零表示立即驱动，-1表示当前无库计时。
 * @return CURLM_OK或句柄/查询错误，成功后解释输出。
 * @note 此值供应用事件循环合并自身期限使用，查询本身不推进网络传输。
 */
CURL_EXTERN CURLMcode curl_multi_timeout(CURLM *m,
                                         long *timeout_ms);

typedef enum {
  /* socket兴趣回调函数指针。 */
  CURLOPT(CURLMOPT_SOCKETFUNCTION, CURLOPTTYPE_FUNCTIONPOINT, 1),

  /* socket回调借用的上下文。 */
  CURLOPT(CURLMOPT_SOCKETDATA, CURLOPTTYPE_OBJECTPOINT, 2),

  /* 历史流水线及多路复用控制，参数为long位掩码。 */
  CURLOPT(CURLMOPT_PIPELINING, CURLOPTTYPE_LONG, 3),

  /* 计时器更新回调函数指针。 */
  CURLOPT(CURLMOPT_TIMERFUNCTION, CURLOPTTYPE_FUNCTIONPOINT, 4),

  /* timer回调借用的上下文。 */
  CURLOPT(CURLMOPT_TIMERDATA, CURLOPTTYPE_OBJECTPOINT, 5),

  /* 连接缓存最大条目数。 */
  CURLOPT(CURLMOPT_MAXCONNECTS, CURLOPTTYPE_LONG, 6),

  /* 单主机最大连接数。 */
  CURLOPT(CURLMOPT_MAX_HOST_CONNECTIONS, CURLOPTTYPE_LONG, 7),

  /* 历史流水线请求数限制。 */
  CURLOPT(CURLMOPT_MAX_PIPELINE_LENGTH, CURLOPTTYPE_LONG, 8),

  /* 历史流水线中可接纳的内容长度阈值。 */
  CURLOPT(CURLMOPT_CONTENT_LENGTH_PENALTY_SIZE, CURLOPTTYPE_OFF_T, 9),

  /* 历史流水线中可接纳的分块长度阈值。 */
  CURLOPT(CURLMOPT_CHUNK_LENGTH_PENALTY_SIZE, CURLOPTTYPE_OFF_T, 10),

  /* 历史流水线排除的站点与端口列表。 */
  CURLOPT(CURLMOPT_PIPELINING_SITE_BL, CURLOPTTYPE_OBJECTPOINT, 11),

  /* 历史流水线排除的服务器类型列表。 */
  CURLOPT(CURLMOPT_PIPELINING_SERVER_BL, CURLOPTTYPE_OBJECTPOINT, 12),

  /* 总活动连接数量限制。 */
  CURLOPT(CURLMOPT_MAX_TOTAL_CONNECTIONS, CURLOPTTYPE_LONG, 13),

   /* HTTP/2服务端推送接纳回调。 */
  CURLOPT(CURLMOPT_PUSHFUNCTION, CURLOPTTYPE_FUNCTIONPOINT, 14),

  /* 推送回调借用的上下文。 */
  CURLOPT(CURLMOPT_PUSHDATA, CURLOPTTYPE_OBJECTPOINT, 15),

  /* 单连接最大并发流数量。 */
  CURLOPT(CURLMOPT_MAX_CONCURRENT_STREAMS, CURLOPTTYPE_LONG, 16),

  /* 网络变更后按位调整缓存和连接复用。 */
  CURLOPT(CURLMOPT_NETWORK_CHANGED, CURLOPTTYPE_LONG, 17),

  /* multi通知回调函数指针。 */
  CURLOPT(CURLMOPT_NOTIFYFUNCTION, CURLOPTTYPE_FUNCTIONPOINT, 18),

  /* 通知回调借用的上下文。 */
  CURLOPT(CURLMOPT_NOTIFYDATA, CURLOPTTYPE_OBJECTPOINT, 19),

  /* 线程式DNS解析器最大线程数。 */
  CURLOPT(CURLMOPT_RESOLVE_THREADS_MAX, CURLOPTTYPE_LONG, 20),

  /* 置1L选择multi清理时的快速线程退出策略，依链接库能力。 */
  CURLOPT(CURLMOPT_QUICK_EXIT, CURLOPTTYPE_LONG, 21),

  CURLMOPT_LASTENTRY /* 枚举末尾占位。 */
} CURLMoption;

/* CURLMOPT_NETWORK_CHANGED 参数的缓存失效位。 */

/* 使与该网络相关的连接和DNS等缓存全部失效。 */
#define CURLMNWC_CLEAR_ALL (1L << 0)

/* 空闲连接关闭，后续复用被禁止，在途传输继续使用已有连接。 */
#define CURLMNWC_CLEAR_CONNS (1L << 1)

/* 清除该multi的DNS缓存，后续重新解析，在途传输继续使用已解析地址。 */
#define CURLMNWC_CLEAR_DNS (1L << 2)

/**
 * @brief 设置multi的单个选项，改变并发、连接或回调配置。
 * @param[in,out] m 存活且当前访问串行的multi借用。
 * @param[in] option CURLMOPT_*选项标识。
 * @param[in] ... 与选项类型匹配的long、curl_off_t、对象或函数指针。
 * @return CURLM_OK或未知选项、参数、资源错误。
 * @note 回调和上下文的借用持续到选项结束使用及最后回调退出。
 */
CURL_EXTERN CURLMcode curl_multi_setopt(CURLM *m,
                                        CURLMoption option, ...);

/**
 * @brief 为multi管理的socket关联应用上下文，供socket回调原样借用。
 * @param[in,out] m 存活的multi句柄借用，遵循该API允许的回调内调用规则。
 * @param[in] sockfd 当前已知的socket描述符。
 * @param[in,out] sockp 应用拥有的上下文借用，NULL清除当前关联。
 * @return CURLM_OK或未知socket、句柄等错误。
 * @note 库保存指针，应用负责清理及覆盖后续回调的寿命，移除监听会解除关联。
 */
CURL_EXTERN CURLMcode curl_multi_assign(CURLM *m,
                                        curl_socket_t sockfd, void *sockp);

/**
 * @brief 复制当前multi包含的easy指针集合，末项以NULL结束。
 * @param[in] m 存活且访问串行的multi借用。
 * @return 成功为调用者curl_free的拥有数组，失败为NULL，空集合仍返回含NULL的数组。
 * @note 数组中的easy只借用，拥有者协调遍历期间的移除和清理，本函数自8.4.0提供。
 */
CURL_EXTERN CURL **curl_multi_get_handles(CURLM *m);

typedef enum {
  CURLMINFO_NONE, /* 枚举起始占位。 */
  /* 已加入且尚未移除的easy数量。 */
  CURLMINFO_XFERS_CURRENT = 1,
  /* 当前正在运行的easy数量。 */
  CURLMINFO_XFERS_RUNNING = 2,
  /* 因连接/并发等限制等待开始的easy数量。 */
  CURLMINFO_XFERS_PENDING = 3,
  /* 已完成且其结果仍等待info_read读取的easy数量。 */
  CURLMINFO_XFERS_DONE = 4,
  /* 此multi生命周期累计加入的easy数量。 */
  CURLMINFO_XFERS_ADDED = 5,

  CURLMINFO_LASTENTRY /* 枚举末尾占位。 */
} CURLMinfo_offt;

/**
 * @brief 读取指定CURLMINFO_*数值指标。
 * @param[in] m 存活且当前访问串行的multi借用。
 * @param[in] info 数值指标枚举。
 * @param[out] pvalue 必需的curl_off_t输出槽，成功后读取。
 * @return CURLM_OK或句柄、指标、能力错误，链接库须提供该公开符号。
 * @note 当前、运行、排队、待收集和累计数量分别反映不同生命周期阶段。
 */
CURL_EXTERN CURLMcode curl_multi_get_offt(CURLM *m,
                                          CURLMinfo_offt info,
                                          curl_off_t *pvalue);

/* HTTP/2推送接纳、拒绝与中止父传输的回调结果值。 */
#define CURL_PUSH_OK       0
#define CURL_PUSH_DENY     1
#define CURL_PUSH_ERROROUT 2 /* 自7.72.0提供。 */

struct curl_pushheaders;  /* 库拥有的推送头访问上下文前置声明。 */

/**
 * @brief 在推送回调中按序号读取PUSH_PROMISE头。
 * @param[in] h 本次推送回调借用的头上下文。
 * @param[in] num 从零开始且小于num_headers的序号。
 * @return 回调期间有效的库拥有字符串借用，无对应项为NULL。
 * @note 应用保持返回内容原样，回调外保留须复制。
 */
CURL_EXTERN char *curl_pushheader_bynum(struct curl_pushheaders *h,
                                        size_t num);
/**
 * @brief 在推送回调中按名称查询PUSH_PROMISE头。
 * @param[in] h 当前推送回调的头上下文借用。
 * @param[in] name NUL结尾头名称借用，允许对应协议的伪头名称。
 * @return 本次回调内有效的字符串借用，未找到为NULL。
 * @note 返回数据由库保管，跨回调使用须取得独立副本。
 */
CURL_EXTERN char *curl_pushheader_byname(struct curl_pushheaders *h,
                                         const char *name);

/**
 * @brief 接纳或拒绝服务端提供的新HTTP/2推送流。
 * @param[in] parent 推送来源的父easy句柄借用。
 * @param[in,out] easy 库新建的推送easy，返回OK后应用接管最终清理责任。
 * @param[in] num_headers 本次PUSH_PROMISE头数量。
 * @param[in] headers 仅本次回调期间有效的头访问上下文借用。
 * @param[in,out] userp PUSHDATA安装的应用上下文借用。
 * @return PUSH_OK接纳、PUSH_DENY拒绝、PUSH_ERROROUT同时使父传输失败。
 * @note OK后由库自动加入multi，DENY时由库销毁新easy，应用按结果处理拥有权。
 */
typedef int (*curl_push_callback)(CURL *parent,
                                  CURL *easy,
                                  size_t num_headers,
                                  struct curl_pushheaders *headers,
                                  void *userp);

/**
 * @brief 复制multi当前poll等待项，或查询所需条数。
 * @param[in] m 当前访问串行的multi借用。
 * @param[out] ufds 调用者拥有的等待项缓冲，size为零时可省略。
 * @param[in] size 缓冲容纳的curl_waitfd条数。
 * @param[out] fd_count 所需等待项数量槽，容量不足时也用于安排新缓冲。
 * @return CURLM_OK或容量/参数错误，应用随后自行等待并驱动multi。
 * @note 查询只描述当前集合，socket借用寿命受后续传输变化约束。
 */
CURL_EXTERN CURLMcode curl_multi_waitfds(CURLM *m,
                                         struct curl_waitfd *ufds,
                                         unsigned int size,
                                         unsigned int *fd_count);

/* 安装回调并启用对应种类后派发的multi通知。 */
#define CURLMNOTIFY_INFO_READ    0
#define CURLMNOTIFY_EASY_DONE    1

/**
 * @brief 接收已通过NOTIFYFUNCTION安装且启用的multi通知。
 * @param[in] m 当前multi句柄借用。
 * @param[in] notification 已启用的CURLMNOTIFY_*通知种类。
 * @param[in] easy 通知对应的easy借用，具体通知可无对应easy。
 * @param[in,out] user_data NOTIFYDATA安装的应用上下文借用。
 * @return 无返回值。
 * @note 上下文覆盖全部回调，回调内重入按链接库API规则，启用通知自8.17.0提供。
 */
typedef void (*curl_notify_callback)(CURLM *m,
                                     unsigned int notification,
                                     CURL *easy,
                                     void *user_data);

/**
 * @brief 停止收集指定multi通知种类。
 * @param[in,out] m 存活且当前访问串行的multi借用。
 * @param[in] notification 要停用的CURLMNOTIFY_*种类。
 * @return CURLM_OK或句柄、种类、能力错误。
 * @note 调用者仍负责此前已在执行的回调上下文寿命，链接库须提供此符号。
 */
CURL_EXTERN CURLMcode curl_multi_notify_disable(CURLM *m,
                                                unsigned int notification);

/**
 * @brief 为multi启用指定通知种类的收集与派发。
 * @param[in,out] m 存活且当前访问串行的multi借用。
 * @param[in] notification CURLMNOTIFY_*通知种类。
 * @return CURLM_OK或参数/能力错误，重复启用已启用种类仍可成功。
 * @note 同时安装NOTIFYFUNCTION才派发通知，回调及上下文保持到最后使用结束。
 */
CURL_EXTERN CURLMcode curl_multi_notify_enable(CURLM *m,
                                               unsigned int notification);

#ifdef __cplusplus
} /* 结束 C 链接声明。 */
#endif

#endif
