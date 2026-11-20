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
/* 公开 multi 拉取式接口，应用决定驱动和等待时机，在同线程管理多条传输，
 * 并可合并应用自己的描述符与库 socket 进行等待。句柄及回调按各接口维护寿命。 */

/* 常规聚合入口 curl.h 已包含本头，此处保留上游直接包含 multi.h 的兼容依赖。 */
#include "curl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void CURLM;

typedef enum {
  CURLM_CALL_MULTI_PERFORM = -1, /* 历史结果值，要求尽快再次驱动 perform/socket。 */
  CURLM_OK,
  CURLM_BAD_HANDLE,      /* multi 句柄无效。 */
  CURLM_BAD_EASY_HANDLE, /* easy 句柄无效。 */
  CURLM_OUT_OF_MEMORY,   /* 内存分配失败。 */
  CURLM_INTERNAL_ERROR,  /* 库内部状态错误。 */
  CURLM_BAD_SOCKET,      /* socket 与当前集合不匹配。 */
  CURLM_UNKNOWN_OPTION,  /* 选项不受运行库支持。 */
  CURLM_ADDED_ALREADY,   /* easy 已加入 multi，重复添加失败。 */
  CURLM_RECURSIVE_API_CALL, /* 回调内发生受限制的重入。 */
  CURLM_WAKEUP_FAILURE,  /* 唤醒不受支持或执行失败。 */
  CURLM_BAD_FUNCTION_ARGUMENT, /* 函数实参无效。 */
  CURLM_ABORTED_BY_CALLBACK,
  CURLM_UNRECOVERABLE_POLL,
  CURLM_LAST
} CURLMcode;

/* socket 驱动使用同一历史再次驱动结果值的兼容名称。 */
#define CURLM_CALL_MULTI_SOCKET CURLM_CALL_MULTI_PERFORM

/* CURLMOPT_PIPELINING 的历史流水线与多路复用控制位，能力依运行库。 */
#define CURLPIPE_NOTHING   0L
#define CURLPIPE_HTTP1     1L
#define CURLPIPE_MULTIPLEX 2L

typedef enum {
  CURLMSG_NONE, /* 枚举首占位。 */
  CURLMSG_DONE, /* easy 已完成，data.result 为该传输 CURLcode。 */
  CURLMSG_LAST /* 枚举尾占位。 */
} CURLMSG;

struct CURLMsg {
  CURLMSG msg;       /* 消息种类。 */
  CURL *easy_handle; /* 对应 easy 句柄借用。 */
  union {
    void *whatever;    /* 消息种类定义的数据。 */
    CURLcode result;   /* 单条传输结果码。 */
  } data;
};
typedef struct CURLMsg CURLMsg;

/* 沿 poll 语义提供独立结构和位值，适配缺少原生 poll 的平台。 */
#define CURL_WAIT_POLLIN    0x0001
#define CURL_WAIT_POLLPRI   0x0002
#define CURL_WAIT_POLLOUT   0x0004

struct curl_waitfd {
  curl_socket_t fd;
  short events;
  short revents;
};

/**
 * @brief 创建管理多条传输的 multi 句柄。
 * @return 成功为调用者拥有的句柄，失败 NULL，最终以 multi_cleanup 回收。
 * @note 同一 multi 的驱动、配置和清理由 owner 串行协调，进程初始化另行安排。
 */
CURL_EXTERN CURLM *curl_multi_init(void);

/**
 * @brief 将 easy 纳入当前 multi 管理。
 * @param[in,out] m 当前串行使用的存活 multi 借用。
 * @param[in,out] curl 尚未加入任何 multi 的有效 easy 借用。
 * @return CURLM_OK 或句柄、重复加入、资源等错误。
 * @note multi 借用 easy 直到 remove，easy 最终由其 owner cleanup。
 */
CURL_EXTERN CURLMcode curl_multi_add_handle(CURLM *m,
                                            CURL *curl);

/**
 * @brief 解除 easy 与 multi 的管理关系。
 * @param[in,out] m 存活 multi 借用，与驱动及清理串行。
 * @param[in,out] curl 属于该 multi 的 easy 借用，移出后继续由应用持有。
 * @return CURLM_OK 或句柄、参数、重入错误，活动传输可由移出而终止。
 * @note 应用在移出后决定重新使用或消费 easy。
 */
CURL_EXTERN CURLMcode curl_multi_remove_handle(CURLM *m,
                                               CURL *curl);

/**
 * @brief 将库当前需要的 socket 加入应用 select 集合。
 * @param[in] m 当前与其他访问串行的 multi 借用。
 * @param[in,out] read_fd_set 已初始化可读集合。
 * @param[in,out] write_fd_set 已初始化可写集合。
 * @param[in,out] exc_fd_set 已初始化异常集合。
 * @param[out] max_fd 最大描述符槽，无可等待 socket 时为 -1。
 * @return CURLM_OK 或参数/句柄错误。
 * @note 调用者合并自有 fd 并查询计时，集合容量受平台 fd_set 约束。
 */
CURL_EXTERN CURLMcode curl_multi_fdset(CURLM *m,
                                       fd_set *read_fd_set,
                                       fd_set *write_fd_set,
                                       fd_set *exc_fd_set,
                                       int *max_fd);

/**
 * @brief 等待 multi 和额外描述符就绪，供应用随后继续驱动。
 * @param[in] m 当前线程使用的存活 multi 借用。
 * @param[in,out] extra_fds 调用者等待数组，零项允许 NULL，revents 返回就绪位。
 * @param[in] extra_nfds 额外数组项数。
 * @param[in] timeout_ms 非负最大等待毫秒，库内更早计时可收紧等待。
 * @param[out] ret 可选就绪描述符数量槽。
 * @return CURLM_OK 或等待/参数错误，无 fd 时可立即返回。
 * @note 等待本身不推进传输，应用在就绪或计时到期后调用驱动入口。
 */
CURL_EXTERN CURLMcode curl_multi_wait(CURLM *m,
                                      struct curl_waitfd extra_fds[],
                                      unsigned int extra_nfds,
                                      int timeout_ms,
                                      int *ret);

/**
 * @brief 等待 multi/额外 fd，支持专用跨线程 wakeup。
 * @param[in] m 存活 multi 借用，除 wakeup 外由 owner 串行使用。
 * @param[in,out] extra_fds 等待数组借用，零项允许 NULL，返回各 revents。
 * @param[in] extra_nfds 额外项数。
 * @param[in] timeout_ms 非负最大毫秒数，可受库内计时收紧。
 * @param[out] ret 可选就绪 fd 数量槽。
 * @return CURLM_OK 或等待/参数错误，无 fd 时仍按计时等待。
 * @note 应用随后继续驱动，wakeup 仅影响此 poll 路径。
 */
CURL_EXTERN CURLMcode curl_multi_poll(CURLM *m,
                                      struct curl_waitfd extra_fds[],
                                      unsigned int extra_nfds,
                                      int timeout_ms,
                                      int *ret);

/**
 * @brief 唤醒正在或随后进入的 multi_poll。
 * @param[in] m owner 保证整个调用期存活的 multi 借用。
 * @return CURLM_OK 或唤醒能力/执行错误。
 * @note 此专用入口允许其他线程调用，owner 仍需排除并发 multi_cleanup。
 */
CURL_EXTERN CURLMcode curl_multi_wakeup(CURLM *m);

/**
 * @brief 推进当前可运行的传输并同步派发库回调。
 * @param[in,out] m 当前 owner 使用的 multi 借用。
 * @param[out] running_handles 尚未完成的传输数量槽。
 * @return multi 整体结果码，单次传输结果经 info_read 读取。
 * @note 由应用按就绪和计时持续驱动，所有上下文借用覆盖驱动及最后回调。
 */
CURL_EXTERN CURLMcode curl_multi_perform(CURLM *m,
                                         int *running_handles);

/**
 * @brief 清理 multi 拥有的连接缓存与管理状态。
 * @param[in] m 拥有句柄，先结束驱动及其他线程的 wakeup 借用。
 * @return CURLM_OK 或清理错误，按实际结果结束句柄寿命。
 * @note 应用先 remove 各 easy 并分别 cleanup，缓存连接关闭时仍可能同步调用 socket 回调。
 */
CURL_EXTERN CURLMcode curl_multi_cleanup(CURLM *m);

/**
 * @brief 消费下一条 multi 完成消息。
 * @param[in,out] m 存活且本次访问串行的 multi 借用。
 * @param[out] msgs_in_queue 取出后剩余消息数量槽。
 * @return 库拥有的消息借用，当前没有消息为 NULL。
 * @note 读取 easy_handle 和 data.result 后再移除/清理，相关 easy 或 multi 清理会结束借用。
 *       应用跨该寿命保留信息时先复制字段。
 */
CURL_EXTERN CURLMsg *curl_multi_info_read(CURLM *m,
                                          int *msgs_in_queue);

/**
 * @brief 将 multi 整体结果码转为诊断说明。
 * @param[in] error 待说明结果码。
 * @return 库拥有的只读 NUL 文本借用，调用者保持内容原样，无释放责任。
 */
CURL_EXTERN const char *curl_multi_strerror(CURLMcode error);

/* socket 回调交付监听兴趣，应用以 CSELECT 就绪位调用 socket_action 驱动传输。 */
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
 * @brief 通知应用更新某个 socket 的监听兴趣。
 * @param[in] easy 相关传输句柄借用，也可能是库内部句柄。
 * @param[in] s 对应 socket 或内部通信描述符。
 * @param[in] what CURL_POLL_* 兴趣，REMOVE 表示移除监听。
 * @param[in,out] userp SOCKETDATA 安装的上下文借用。
 * @param[in,out] socketp multi_assign 关联的上下文借用，可 NULL。
 * @return 正常零，失败 -1 并中止当前 multi 在途传输。
 * @note 缓存连接清理期仍可回调，上下文存活到最后一次回调结束。
 */
typedef int (*curl_socket_callback)(CURL *easy,
                                    curl_socket_t s,
                                    int what,
                                    void *userp,
                                    void *socketp);
/**
 * @brief 通知应用替换下一次驱动 multi 的计时器。
 * @param[in] m 当前 multi 句柄借用。
 * @param[in] timeout_ms 最大等待毫秒，零安排尽快驱动，-1 删除计时器。
 * @param[in,out] userp TIMERDATA 安装的上下文借用。
 * @return 正常零，失败按运行库的回调中止规则处理。
 * @note 零超时由事件循环安排，保持 API 重入约束，计时器上下文由应用拥有。
 */
typedef int (*curl_multi_timer_callback)(CURLM *m,
                                         long timeout_ms,
                                         void *userp);

/**
 * @brief 使用旧式 socket 入口推进一个描述符。
 * @param[in,out] m 当前串行操作的 multi 借用。
 * @param[in] s 就绪 socket 或 TIMEOUT 标记。
 * @param[out] running_handles 尚在运行的传输数槽。
 * @return multi 整体结果，单次传输通过 info_read 解释。
 * @note 自 7.19.5 弃用，默认同名宏转发到 socket_action。
 */
CURL_EXTERN CURLMcode CURL_DEPRECATED(7.19.5, "Use curl_multi_socket_action()")
curl_multi_socket(CURLM *m, curl_socket_t s, int *running_handles);

/**
 * @brief 根据就绪 socket 或计时事件推进关联传输。
 * @param[in,out] m 当前 owner 独占使用的 multi 借用。
 * @param[in] s 已通知 socket，CURL_SOCKET_TIMEOUT 表示计时驱动。
 * @param[in] ev_bitmask CURL_CSELECT_IN/OUT/ERR 组合，计时事件传零。
 * @param[out] running_handles 尚运行的传输数槽。
 * @return multi 整体结果，完成消息由应用继续读取。
 * @note 依据 socket/timer 回调维护事件循环，所有借用覆盖驱动调用和回调。
 */
CURL_EXTERN CURLMcode curl_multi_socket_action(CURLM *m,
                                               curl_socket_t s,
                                               int ev_bitmask,
                                               int *running_handles);

/**
 * @brief 通过旧式接口检查全部 multi socket 并推进传输。
 * @param[in,out] m 存活且串行驱动的 multi 借用。
 * @param[out] running_handles 尚运行传输数量槽。
 * @return multi 整体结果，单次结果另由完成队列读取。
 * @note 自 7.19.5 弃用，保留此公开声明供兼容使用。
 */
CURL_EXTERN CURLMcode CURL_DEPRECATED(7.19.5, "Use curl_multi_socket_action()")
curl_multi_socket_all(CURLM *m, int *running_handles);

#ifndef CURL_ALLOW_OLD_MULTI_SOCKET
/**
 * @brief 将旧 socket 调用转发到零就绪位的 socket_action。
 * @param[in,out] x multi 句柄借用。
 * @param[in] y socket 或 TIMEOUT 标记。
 * @param[out] z 尚运行的传输数槽。
 * @return 新入口的 multi 整体结果，每个实参在展开中出现一次。
 */
#define curl_multi_socket(x, y, z) curl_multi_socket_action(x, y, 0, z)
#endif

/**
 * @brief 查询下一次驱动 multi 前的最大允许等待。
 * @param[in] m 当前访问串行的存活 multi 借用。
 * @param[out] timeout_ms 毫秒输出槽，零立即驱动，-1 表示当前无库计时。
 * @return CURLM_OK 或句柄/查询错误，成功后使用输出。
 * @note 应用合并自有期限，本函数只查询计时。
 */
CURL_EXTERN CURLMcode curl_multi_timeout(CURLM *m,
                                         long *timeout_ms);

typedef enum {
  /* socket 监听兴趣回调。 */
  CURLOPT(CURLMOPT_SOCKETFUNCTION, CURLOPTTYPE_FUNCTIONPOINT, 1),

  /* socket 回调借用的应用上下文。 */
  CURLOPT(CURLMOPT_SOCKETDATA, CURLOPTTYPE_OBJECTPOINT, 2),

  /* 历史流水线及多路复用 long 位掩码。 */
  CURLOPT(CURLMOPT_PIPELINING, CURLOPTTYPE_LONG, 3),

  /* 计时器更新回调。 */
  CURLOPT(CURLMOPT_TIMERFUNCTION, CURLOPTTYPE_FUNCTIONPOINT, 4),

  /* timer 回调借用的应用上下文。 */
  CURLOPT(CURLMOPT_TIMERDATA, CURLOPTTYPE_OBJECTPOINT, 5),

  /* 连接缓存最大条数。 */
  CURLOPT(CURLMOPT_MAXCONNECTS, CURLOPTTYPE_LONG, 6),

  /* 单主机最大连接数。 */
  CURLOPT(CURLMOPT_MAX_HOST_CONNECTIONS, CURLOPTTYPE_LONG, 7),

  /* 历史流水线内请求数上限。 */
  CURLOPT(CURLMOPT_MAX_PIPELINE_LENGTH, CURLOPTTYPE_LONG, 8),

  /* 历史流水线可接纳的内容长度阈值。 */
  CURLOPT(CURLMOPT_CONTENT_LENGTH_PENALTY_SIZE, CURLOPTTYPE_OFF_T, 9),

  /* 历史流水线可接纳的分块长度阈值。 */
  CURLOPT(CURLMOPT_CHUNK_LENGTH_PENALTY_SIZE, CURLOPTTYPE_OFF_T, 10),

  /* 历史流水线排除的站点和端口列表。 */
  CURLOPT(CURLMOPT_PIPELINING_SITE_BL, CURLOPTTYPE_OBJECTPOINT, 11),

  /* 历史流水线排除的服务器类型列表。 */
  CURLOPT(CURLMOPT_PIPELINING_SERVER_BL, CURLOPTTYPE_OBJECTPOINT, 12),

  /* 总打开连接数量上限。 */
  CURLOPT(CURLMOPT_MAX_TOTAL_CONNECTIONS, CURLOPTTYPE_LONG, 13),

   /* HTTP/2 服务端推送接纳回调。 */
  CURLOPT(CURLMOPT_PUSHFUNCTION, CURLOPTTYPE_FUNCTIONPOINT, 14),

  /* 推送回调借用上下文。 */
  CURLOPT(CURLMOPT_PUSHDATA, CURLOPTTYPE_OBJECTPOINT, 15),

  /* 每连接并发流数量上限。 */
  CURLOPT(CURLMOPT_MAX_CONCURRENT_STREAMS, CURLOPTTYPE_LONG, 16),

  /* 网络变更后按位调整缓存和连接复用。 */
  CURLOPT(CURLMOPT_NETWORK_CHANGED, CURLOPTTYPE_LONG, 17),

  /* multi 通知回调。 */
  CURLOPT(CURLMOPT_NOTIFYFUNCTION, CURLOPTTYPE_FUNCTIONPOINT, 18),

  /* 通知回调借用上下文。 */
  CURLOPT(CURLMOPT_NOTIFYDATA, CURLOPTTYPE_OBJECTPOINT, 19),

  /* 线程式 DNS 解析器的最大线程数。 */
  CURLOPT(CURLMOPT_RESOLVE_THREADS_MAX, CURLOPTTYPE_LONG, 20),

  /* 置 1L 使用 multi 清理时的快速线程退出策略，依链接库能力。 */
  CURLOPT(CURLMOPT_QUICK_EXIT, CURLOPTTYPE_LONG, 21),

  CURLMOPT_LASTENTRY /* 枚举尾占位。 */
} CURLMoption;

/* CURLMOPT_NETWORK_CHANGED 的缓存失效控制位。 */

/* 使该网络相关连接、DNS 等全部缓存失效。 */
#define CURLMNWC_CLEAR_ALL (1L << 0)

/* 关闭空闲连接并阻止后续复用，在途传输继续使用已有连接。 */
#define CURLMNWC_CLEAR_CONNS (1L << 1)

/* 清空此 multi 的 DNS 缓存，未来请求重新解析，在途传输继续使用已解析地址。 */
#define CURLMNWC_CLEAR_DNS (1L << 2)

/**
 * @brief 设置 multi 并发、连接或回调选项。
 * @param[in,out] m 当前串行操作的 multi 借用。
 * @param[in] option 单个 CURLMOPT 选项。
 * @param[in] ... 与选项匹配的 long、curl_off_t、对象或准确回调指针。
 * @return CURLM_OK 或选项、参数、资源错误。
 * @note 回调及上下文借用持续到选项结束使用且最后一次回调退出。
 */
CURL_EXTERN CURLMcode curl_multi_setopt(CURLM *m,
                                        CURLMoption option, ...);

/**
 * @brief 为已知 socket 关联应用上下文供 socket 回调借用。
 * @param[in,out] m 存活 multi 借用，遵守该 API 的回调内使用规则。
 * @param[in] sockfd 当前库已知的 socket。
 * @param[in,out] sockp 应用拥有的上下文借用，NULL 清除关联。
 * @return CURLM_OK 或未知 socket、句柄等错误。
 * @note 库保存指针，应用维持到关联及最后回调结束，移除监听后关联解除。
 */
CURL_EXTERN CURLMcode curl_multi_assign(CURLM *m,
                                        curl_socket_t sockfd, void *sockp);

/**
 * @brief 为当前 easy 集合分配以 NULL 结尾的指针数组。
 * @param[in] m 当前访问串行的 multi 借用。
 * @return 成功为接收者 curl_free 的拥有数组，失败 NULL，空集合仍返回首项 NULL 的数组。
 * @note 数组元素为 easy 借用，owner 协调遍历期的移除与清理，自 8.4.0 提供。
 */
CURL_EXTERN CURL **curl_multi_get_handles(CURLM *m);

typedef enum {
  CURLMINFO_NONE, /* 枚举首占位。 */
  /* 已加入且尚未移除的 easy 数量。 */
  CURLMINFO_XFERS_CURRENT = 1,
  /* 当前运行中的 easy 数量。 */
  CURLMINFO_XFERS_RUNNING = 2,
  /* 由于连接或并发限制等原因等待开始的 easy 数量。 */
  CURLMINFO_XFERS_PENDING = 3,
  /* 已完成且结果等待 info_read 读取的 easy 数量。 */
  CURLMINFO_XFERS_DONE = 4,
  /* 此 multi 寿命内累计加入的 easy 数量。 */
  CURLMINFO_XFERS_ADDED = 5,

  CURLMINFO_LASTENTRY /* 枚举尾占位。 */
} CURLMinfo_offt;

/**
 * @brief 读取指定阶段的 easy 数值指标。
 * @param[in] m 存活且当前串行查询的 multi 借用。
 * @param[in] info CURLMINFO 数值指标标识。
 * @param[out] pvalue 必填 curl_off_t 输出槽，成功后读取。
 * @return CURLM_OK 或句柄、指标、能力错误，运行库须提供此符号。
 * @note 当前、运行、等待、待收集与累计计数分别描述对应生命周期阶段。
 */
CURL_EXTERN CURLMcode curl_multi_get_offt(CURLM *m,
                                          CURLMinfo_offt info,
                                          curl_off_t *pvalue);

/* HTTP/2 服务端推送的接纳、拒绝及中止父传输结果。 */
#define CURL_PUSH_OK       0
#define CURL_PUSH_DENY     1
#define CURL_PUSH_ERROROUT 2 /* 自 7.72.0 提供。 */

struct curl_pushheaders;  /* 库拥有的推送头上下文前置声明。 */

/**
 * @brief 按序号提取当前 PUSH_PROMISE 头。
 * @param[in] h 当前推送回调期有效的上下文借用。
 * @param[in] num 零起点且小于 num_headers 的下标。
 * @return 本回调内有效的库字符串借用，未找到 NULL，跨回调保留需复制。
 */
CURL_EXTERN char *curl_pushheader_bynum(struct curl_pushheaders *h,
                                        size_t num);
/**
 * @brief 按名称提取当前推送头。
 * @param[in] h 本次推送回调上下文借用。
 * @param[in] name NUL 名称借用，可使用协议伪头名称。
 * @return 仅本回调内有效的库字符串借用，未命中 NULL，无释放责任。
 */
CURL_EXTERN char *curl_pushheader_byname(struct curl_pushheaders *h,
                                         const char *name);

/**
 * @brief 决定是否接纳服务端推送的新 HTTP/2 流。
 * @param[in] parent 推送来源的父 easy 借用。
 * @param[in,out] easy 库新建推送 easy，返回 OK 后应用承担最终 cleanup。
 * @param[in] num_headers 当前 PUSH_PROMISE 头数量。
 * @param[in] headers 仅本次回调期有效的头访问借用。
 * @param[in,out] userp PUSHDATA 安装的上下文借用。
 * @return PUSH_OK 接纳，PUSH_DENY 拒绝，PUSH_ERROROUT 同时中止父传输。
 * @note 接纳后由库自动加入 multi，拒绝由库销毁新 easy，应用按结果交接拥有权。
 */
typedef int (*curl_push_callback)(CURL *parent,
                                  CURL *easy,
                                  size_t num_headers,
                                  struct curl_pushheaders *headers,
                                  void *userp);

/**
 * @brief 复制当前等待 fd 列表，或以零容量查询所需项数。
 * @param[in] m 存活且本次访问串行的 multi 借用。
 * @param[out] ufds 调用者拥有的可写数组，size 为零可省略。
 * @param[in] size 可容纳的 curl_waitfd 条数。
 * @param[out] fd_count 所需等待项数槽，供不足时安排新缓冲。
 * @return CURLM_OK 或参数/容量错误，应用随后等待并继续驱动。
 * @note 当前 socket 集合借用受后续传输变化影响。
 */
CURL_EXTERN CURLMcode curl_multi_waitfds(CURLM *m,
                                         struct curl_waitfd *ufds,
                                         unsigned int size,
                                         unsigned int *fd_count);

/* 安装通知回调并启用对应种类后收集和派发的通知。 */
#define CURLMNOTIFY_INFO_READ    0
#define CURLMNOTIFY_EASY_DONE    1

/**
 * @brief 接收已启用的 multi 通知。
 * @param[in] m 当前 multi 借用。
 * @param[in] notification CURLMNOTIFY 通知种类。
 * @param[in] easy 对应 easy 借用，具体种类可无对应句柄。
 * @param[in,out] user_data NOTIFYDATA 安装的上下文借用。
 * @return 无返回值，应用保持上下文覆盖全部回调。
 * @note 通知 API 自 8.17.0 提供，回调内操作遵守运行库的重入约束。
 */
typedef void (*curl_notify_callback)(CURLM *m,
                                     unsigned int notification,
                                     CURL *easy,
                                     void *user_data);

/**
 * @brief 停止收集指定通知种类。
 * @param[in,out] m 当前串行操作的存活 multi 借用。
 * @param[in] notification 待停用通知种类。
 * @return CURLM_OK 或句柄、种类、能力错误，运行库须有对应符号。
 * @note owner 继续维持已执行中回调上下文的借用寿命。
 */
CURL_EXTERN CURLMcode curl_multi_notify_disable(CURLM *m,
                                                unsigned int notification);

/**
 * @brief 为 multi 启用指定通知的收集与派发。
 * @param[in,out] m 当前串行操作的存活 multi 借用。
 * @param[in] notification 待启用通知种类。
 * @return CURLM_OK 或参数/能力错误，重复启用已开启种类可成功。
 * @note 同时安装 NOTIFYFUNCTION 才派发，回调和上下文保持到最后使用结束。
 */
CURL_EXTERN CURLMcode curl_multi_notify_enable(CURLM *m,
                                               unsigned int notification);

#ifdef __cplusplus
} /* 结束 C 链接声明。 */
#endif

#endif
