#ifndef CURLINC_SYSTEM_H
#define CURLINC_SYSTEM_H
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

/* 按平台、编译器和架构选择公开 ABI，平台识别只使用编译器内建宏。
 * 专用工具链各保留独立分支，判断顺序决定实际命中项。
 * curl_off_t 的目标契约为 64 位有符号整数，宽度独立于大文件支持开关，
 * 所以采用 long long、__int64 或 LP64 下的 long 等稳定映射。
 * 历史工具链分支保留上游定义，实际目标需验证类型、格式符与常量后缀。
 * curl_socklen_t 按本平台 socket API 映射，完成宏选择后再形成公开 typedef。 */

#ifdef __DJGPP__
#  define CURL_TYPEOF_CURL_OFF_T     long long
#  define CURL_FORMAT_CURL_OFF_T     "lld"
#  define CURL_FORMAT_CURL_OFF_TU    "llu"
#  define CURL_SUFFIX_CURL_OFF_T     LL
#  define CURL_SUFFIX_CURL_OFF_TU    ULL
#  define CURL_TYPEOF_CURL_SOCKLEN_T int

#elif defined(__BORLANDC__)
#  define CURL_TYPEOF_CURL_OFF_T     __int64
#  define CURL_FORMAT_CURL_OFF_T     "I64d"
#  define CURL_FORMAT_CURL_OFF_TU    "I64u"
#  define CURL_SUFFIX_CURL_OFF_T     i64
#  define CURL_SUFFIX_CURL_OFF_TU    ui64
#  define CURL_TYPEOF_CURL_SOCKLEN_T int

#elif defined(__POCC__)
#  ifdef _MSC_VER
#    define CURL_TYPEOF_CURL_OFF_T     __int64
#    define CURL_FORMAT_CURL_OFF_T     "I64d"
#    define CURL_FORMAT_CURL_OFF_TU    "I64u"
#    define CURL_SUFFIX_CURL_OFF_T     i64
#    define CURL_SUFFIX_CURL_OFF_TU    ui64
#  else
#    define CURL_TYPEOF_CURL_OFF_T     long long
#    define CURL_FORMAT_CURL_OFF_T     "lld"
#    define CURL_FORMAT_CURL_OFF_TU    "llu"
#    define CURL_SUFFIX_CURL_OFF_T     LL
#    define CURL_SUFFIX_CURL_OFF_TU    ULL
#  endif
#  define CURL_TYPEOF_CURL_SOCKLEN_T int

#elif defined(__LCC__)
#  ifdef __MCST__ /* MCST eLbrus 编译器集合。 */
#    define CURL_TYPEOF_CURL_OFF_T     long
#    define CURL_FORMAT_CURL_OFF_T     "ld"
#    define CURL_FORMAT_CURL_OFF_TU    "lu"
#    define CURL_SUFFIX_CURL_OFF_T     L
#    define CURL_SUFFIX_CURL_OFF_TU    UL
#    define CURL_TYPEOF_CURL_SOCKLEN_T socklen_t
#    define CURL_PULL_SYS_TYPES_H      1
#    define CURL_PULL_SYS_SOCKET_H     1
#  else                /* Local/Little C 编译器常规分支。 */
#    define CURL_TYPEOF_CURL_OFF_T     long
#    define CURL_FORMAT_CURL_OFF_T     "ld"
#    define CURL_FORMAT_CURL_OFF_TU    "lu"
#    define CURL_SUFFIX_CURL_OFF_T     L
#    define CURL_SUFFIX_CURL_OFF_TU    UL
#    define CURL_TYPEOF_CURL_SOCKLEN_T int
#  endif

#elif defined(macintosh)
#  include <ConditionalMacros.h>
#  if TYPE_LONGLONG
#    define CURL_TYPEOF_CURL_OFF_T     long long
#    define CURL_FORMAT_CURL_OFF_T     "lld"
#    define CURL_FORMAT_CURL_OFF_TU    "llu"
#    define CURL_SUFFIX_CURL_OFF_T     LL
#    define CURL_SUFFIX_CURL_OFF_TU    ULL
#  else
#    define CURL_TYPEOF_CURL_OFF_T     long
#    define CURL_FORMAT_CURL_OFF_T     "ld"
#    define CURL_FORMAT_CURL_OFF_TU    "lu"
#    define CURL_SUFFIX_CURL_OFF_T     L
#    define CURL_SUFFIX_CURL_OFF_TU    UL
#  endif
#  define CURL_TYPEOF_CURL_SOCKLEN_T unsigned int

#elif defined(__TANDEM)
#  ifndef __LP64
#    define CURL_TYPEOF_CURL_OFF_T     long long
#    define CURL_FORMAT_CURL_OFF_T     "lld"
#    define CURL_FORMAT_CURL_OFF_TU    "llu"
#    define CURL_SUFFIX_CURL_OFF_T     LL
#    define CURL_SUFFIX_CURL_OFF_TU    ULL
#    define CURL_TYPEOF_CURL_SOCKLEN_T int
#  else
#    define CURL_TYPEOF_CURL_OFF_T     long
#    define CURL_FORMAT_CURL_OFF_T     "ld"
#    define CURL_FORMAT_CURL_OFF_TU    "lu"
#    define CURL_SUFFIX_CURL_OFF_T     L
#    define CURL_SUFFIX_CURL_OFF_TU    UL
#    define CURL_TYPEOF_CURL_SOCKLEN_T unsigned int
#  endif

#elif defined(UNDER_CE)
#  define CURL_TYPEOF_CURL_OFF_T     __int64
#  define CURL_FORMAT_CURL_OFF_T     "I64d"
#  define CURL_FORMAT_CURL_OFF_TU    "I64u"
#  define CURL_SUFFIX_CURL_OFF_T     i64
#  define CURL_SUFFIX_CURL_OFF_TU    ui64
#  define CURL_TYPEOF_CURL_SOCKLEN_T int

#elif defined(__MINGW32__)
#  include <inttypes.h>
#  define CURL_TYPEOF_CURL_OFF_T     long long
#  define CURL_FORMAT_CURL_OFF_T     PRId64
#  define CURL_FORMAT_CURL_OFF_TU    PRIu64
#  define CURL_SUFFIX_CURL_OFF_T     LL
#  define CURL_SUFFIX_CURL_OFF_TU    ULL
#  define CURL_TYPEOF_CURL_SOCKLEN_T int
#  define CURL_PULL_SYS_TYPES_H      1

#elif defined(__VMS)
#  ifdef __VAX
#    define CURL_TYPEOF_CURL_OFF_T     long
#    define CURL_FORMAT_CURL_OFF_T     "ld"
#    define CURL_FORMAT_CURL_OFF_TU    "lu"
#    define CURL_SUFFIX_CURL_OFF_T     L
#    define CURL_SUFFIX_CURL_OFF_TU    UL
#  else
#    define CURL_TYPEOF_CURL_OFF_T     long long
#    define CURL_FORMAT_CURL_OFF_T     "lld"
#    define CURL_FORMAT_CURL_OFF_TU    "llu"
#    define CURL_SUFFIX_CURL_OFF_T     LL
#    define CURL_SUFFIX_CURL_OFF_TU    ULL
#  endif
#  define CURL_TYPEOF_CURL_SOCKLEN_T unsigned int

#elif defined(__OS400__)
#  define CURL_TYPEOF_CURL_OFF_T     long long
#  define CURL_FORMAT_CURL_OFF_T     "lld"
#  define CURL_FORMAT_CURL_OFF_TU    "llu"
#  define CURL_SUFFIX_CURL_OFF_T     LL
#  define CURL_SUFFIX_CURL_OFF_TU    ULL
#  define CURL_TYPEOF_CURL_SOCKLEN_T socklen_t
#  define CURL_PULL_SYS_TYPES_H      1
#  define CURL_PULL_SYS_SOCKET_H     1

#elif defined(__MVS__)
#  ifdef _LONG_LONG
#    define CURL_TYPEOF_CURL_OFF_T     long long
#    define CURL_FORMAT_CURL_OFF_T     "lld"
#    define CURL_FORMAT_CURL_OFF_TU    "llu"
#    define CURL_SUFFIX_CURL_OFF_T     LL
#    define CURL_SUFFIX_CURL_OFF_TU    ULL
#  else /* _LP64 与默认分支。 */
#    define CURL_TYPEOF_CURL_OFF_T     long
#    define CURL_FORMAT_CURL_OFF_T     "ld"
#    define CURL_FORMAT_CURL_OFF_TU    "lu"
#    define CURL_SUFFIX_CURL_OFF_T     L
#    define CURL_SUFFIX_CURL_OFF_TU    UL
#  endif
#  define CURL_TYPEOF_CURL_SOCKLEN_T socklen_t
#  define CURL_PULL_SYS_TYPES_H      1
#  define CURL_PULL_SYS_SOCKET_H     1

#elif defined(__370__)
#  if defined(__IBMC__) || defined(__IBMCPP__)
#    ifdef _LONG_LONG
#      define CURL_TYPEOF_CURL_OFF_T     long long
#      define CURL_FORMAT_CURL_OFF_T     "lld"
#      define CURL_FORMAT_CURL_OFF_TU    "llu"
#      define CURL_SUFFIX_CURL_OFF_T     LL
#      define CURL_SUFFIX_CURL_OFF_TU    ULL
#    else /* _LP64 与默认分支。 */
#      define CURL_TYPEOF_CURL_OFF_T     long
#      define CURL_FORMAT_CURL_OFF_T     "ld"
#      define CURL_FORMAT_CURL_OFF_TU    "lu"
#      define CURL_SUFFIX_CURL_OFF_T     L
#      define CURL_SUFFIX_CURL_OFF_TU    UL
#    endif
#    define CURL_TYPEOF_CURL_SOCKLEN_T socklen_t
#    define CURL_PULL_SYS_TYPES_H      1
#    define CURL_PULL_SYS_SOCKET_H     1
#  endif

#elif defined(TPF)
#  define CURL_TYPEOF_CURL_OFF_T     long
#  define CURL_FORMAT_CURL_OFF_T     "ld"
#  define CURL_FORMAT_CURL_OFF_TU    "lu"
#  define CURL_SUFFIX_CURL_OFF_T     L
#  define CURL_SUFFIX_CURL_OFF_TU    UL
#  define CURL_TYPEOF_CURL_SOCKLEN_T int

#elif defined(__TINYC__) /* Tiny C，即 tcc 编译器。 */
#  define CURL_TYPEOF_CURL_OFF_T     long long
#  define CURL_FORMAT_CURL_OFF_T     "lld"
#  define CURL_FORMAT_CURL_OFF_TU    "llu"
#  define CURL_SUFFIX_CURL_OFF_T     LL
#  define CURL_SUFFIX_CURL_OFF_TU    ULL
#  define CURL_TYPEOF_CURL_SOCKLEN_T socklen_t
#  define CURL_PULL_SYS_TYPES_H      1
#  define CURL_PULL_SYS_SOCKET_H     1

#elif defined(__SUNPRO_C) || defined(__SUNPRO_CC) /* Oracle Solaris Studio 编译器。 */
#  if !defined(__LP64) && (defined(__ILP32) ||                          \
                           defined(__i386) ||                           \
                           defined(__sparcv8) ||                        \
                           defined(__sparcv8plus))
#    define CURL_TYPEOF_CURL_OFF_T     long long
#    define CURL_FORMAT_CURL_OFF_T     "lld"
#    define CURL_FORMAT_CURL_OFF_TU    "llu"
#    define CURL_SUFFIX_CURL_OFF_T     LL
#    define CURL_SUFFIX_CURL_OFF_TU    ULL
#  elif defined(__LP64) || \
        defined(__amd64) || defined(__sparcv9)
#    define CURL_TYPEOF_CURL_OFF_T     long
#    define CURL_FORMAT_CURL_OFF_T     "ld"
#    define CURL_FORMAT_CURL_OFF_TU    "lu"
#    define CURL_SUFFIX_CURL_OFF_T     L
#    define CURL_SUFFIX_CURL_OFF_TU    UL
#  endif
#  define CURL_TYPEOF_CURL_SOCKLEN_T socklen_t
#  define CURL_PULL_SYS_TYPES_H      1
#  define CURL_PULL_SYS_SOCKET_H     1

#elif defined(__xlc__) /* IBM xlc 编译器。 */
#  ifndef _LP64
#    define CURL_TYPEOF_CURL_OFF_T     long long
#    define CURL_FORMAT_CURL_OFF_T     "lld"
#    define CURL_FORMAT_CURL_OFF_TU    "llu"
#    define CURL_SUFFIX_CURL_OFF_T     LL
#    define CURL_SUFFIX_CURL_OFF_TU    ULL
#  else
#    define CURL_TYPEOF_CURL_OFF_T     long
#    define CURL_FORMAT_CURL_OFF_T     "ld"
#    define CURL_FORMAT_CURL_OFF_TU    "lu"
#    define CURL_SUFFIX_CURL_OFF_T     L
#    define CURL_SUFFIX_CURL_OFF_TU    UL
#  endif
#  define CURL_TYPEOF_CURL_SOCKLEN_T socklen_t
#  define CURL_PULL_SYS_TYPES_H      1
#  define CURL_PULL_SYS_SOCKET_H     1

#elif defined(__hpux) /* HP aCC 编译器。 */
#  ifndef _LP64
#    define CURL_TYPEOF_CURL_OFF_T     long long
#    define CURL_FORMAT_CURL_OFF_T     "lld"
#    define CURL_FORMAT_CURL_OFF_TU    "llu"
#    define CURL_SUFFIX_CURL_OFF_T     LL
#    define CURL_SUFFIX_CURL_OFF_TU    ULL
#  else
#    define CURL_TYPEOF_CURL_OFF_T     long
#    define CURL_FORMAT_CURL_OFF_T     "ld"
#    define CURL_FORMAT_CURL_OFF_TU    "lu"
#    define CURL_SUFFIX_CURL_OFF_T     L
#    define CURL_SUFFIX_CURL_OFF_TU    UL
#  endif
#  define CURL_TYPEOF_CURL_SOCKLEN_T socklen_t
#  define CURL_PULL_SYS_TYPES_H      1
#  define CURL_PULL_SYS_SOCKET_H     1

/* MSVC 判断保留在倒数第二个编译器分支。 */

#elif defined(_MSC_VER)
#  if (_MSC_VER >= 1800)
#    include <inttypes.h>
#    define CURL_FORMAT_CURL_OFF_T     PRId64
#    define CURL_FORMAT_CURL_OFF_TU    PRIu64
#  else
#    define CURL_FORMAT_CURL_OFF_T     "I64d"
#    define CURL_FORMAT_CURL_OFF_TU    "I64u"
#  endif
#  define CURL_TYPEOF_CURL_OFF_T     __int64
#  define CURL_SUFFIX_CURL_OFF_T     i64
#  define CURL_SUFFIX_CURL_OFF_TU    ui64
#  define CURL_TYPEOF_CURL_SOCKLEN_T int

/* 通用 GCC 兼容判断置于编译器分支末尾。 */

#elif defined(__GNUC__) && !defined(_SCO_DS)
#  if !defined(__LP64__) &&                                             \
  (defined(__ILP32__) || defined(__i386__) || defined(__hppa__) ||      \
   defined(__ppc__) || defined(__powerpc__) || defined(__arm__) ||      \
   defined(__sparc__) || defined(__mips__) || defined(__sh__) ||        \
   defined(__XTENSA__) ||                                               \
   (defined(__SIZEOF_LONG__) && __SIZEOF_LONG__ == 4) ||                \
   (defined(__LONG_MAX__) && __LONG_MAX__ == 2147483647L))
#    define CURL_TYPEOF_CURL_OFF_T     long long
#    define CURL_FORMAT_CURL_OFF_T     "lld"
#    define CURL_FORMAT_CURL_OFF_TU    "llu"
#    define CURL_SUFFIX_CURL_OFF_T     LL
#    define CURL_SUFFIX_CURL_OFF_TU    ULL
#    if (__GNUC__ >= 4) || \
       ((__GNUC__ == 3) && defined(__GNUC_MINOR__) && (__GNUC_MINOR__ >= 4))
/**
 * @brief 统计 64 位无符号整数的置位数量。
 * @param[in] x 可转为 unsigned long long 的值，展开出现一次。
 * @return 0 至 64 的置位数，无分配或共享状态。
 */
#      define CURL_POPCOUNT64(x)       __builtin_popcountll(x)
/**
 * @brief 计算非零 64 位整数的低位连续零数量。
 * @param[in] x 转换为 unsigned long long 后必须非零，展开出现一次。
 * @return 0 至 63 的低位零数，零输入不满足内建函数前提。
 */
#      define CURL_CTZ64(x)            __builtin_ctzll(x)
#    endif
#  elif defined(__LP64__) || \
        defined(__x86_64__) || defined(__ppc64__) || defined(__sparc64__) || \
        defined(__e2k__) || \
        (defined(__SIZEOF_LONG__) && __SIZEOF_LONG__ == 8) || \
        (defined(__LONG_MAX__) && __LONG_MAX__ == 9223372036854775807L)
#    define CURL_TYPEOF_CURL_OFF_T     long
#    define CURL_FORMAT_CURL_OFF_T     "ld"
#    define CURL_FORMAT_CURL_OFF_TU    "lu"
#    define CURL_SUFFIX_CURL_OFF_T     L
#    define CURL_SUFFIX_CURL_OFF_TU    UL
#    if (__GNUC__ >= 4) || \
       ((__GNUC__ == 3) && defined(__GNUC_MINOR__) && (__GNUC_MINOR__ >= 4))
/**
 * @brief 在 LP64 分支统计 unsigned long 的置位数。
 * @param[in] x 可转为 64 位 unsigned long 的值，展开出现一次。
 * @return 0 至 64 的置位数，无拥有对象。
 */
#      define CURL_POPCOUNT64(x)       __builtin_popcountl(x)
/**
 * @brief 在 LP64 分支计算非零 unsigned long 的尾零数。
 * @param[in] x 转换后的 64 位值必须非零，展开出现一次。
 * @return 0 至 63 的连续低位零数量，无共享状态。
 */
#      define CURL_CTZ64(x)            __builtin_ctzl(x)
#    endif
#  endif
#  define CURL_TYPEOF_CURL_SOCKLEN_T socklen_t
#  define CURL_PULL_SYS_TYPES_H      1
#  define CURL_PULL_SYS_SOCKET_H     1

#else
/* 未匹配专用工具链时采用旧式 32 位环境的默认映射。 */
#  define CURL_TYPEOF_CURL_OFF_T     long long
#  define CURL_FORMAT_CURL_OFF_T     "lld"
#  define CURL_FORMAT_CURL_OFF_TU    "llu"
#  define CURL_SUFFIX_CURL_OFF_T     LL
#  define CURL_SUFFIX_CURL_OFF_TU    ULL
#  define CURL_TYPEOF_CURL_SOCKLEN_T int
#endif

#ifdef _AIX
/* AIX 的系统类型需要 sys/poll.h。 */
#define CURL_PULL_SYS_POLL_H
#endif

/* 上方类型映射需要系统基础类型时补入 sys/types.h。 */
#ifdef CURL_PULL_SYS_TYPES_H
#  include <sys/types.h>
#endif

/* 采用系统 socket 类型的分支在 typedef 前补入 sys/socket.h。 */
#ifdef CURL_PULL_SYS_SOCKET_H
#  include <sys/socket.h>
#endif

/* 平台要求 poll 系统类型时补入 sys/poll.h。 */
#ifdef CURL_PULL_SYS_POLL_H
#  include <sys/poll.h>
#endif

/* 形成 socket 长度的公开 ABI 类型。 */
#ifdef CURL_TYPEOF_CURL_SOCKLEN_T
  typedef CURL_TYPEOF_CURL_SOCKLEN_T curl_socklen_t;
#endif

/* 形成文件偏移与内容长度的公开 ABI 类型。 */

#ifdef CURL_TYPEOF_CURL_OFF_T
  typedef CURL_TYPEOF_CURL_OFF_T curl_off_t;
#endif

#endif /* 结束 CURLINC_SYSTEM_H 包含保护。 */
