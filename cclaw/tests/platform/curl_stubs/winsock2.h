#ifndef CC_TEST_CURL_WINSOCK2_H
#define CC_TEST_CURL_WINSOCK2_H

/* Linux 模型使用 POSIX 地址布局，只检验生产控制流，不模拟 Windows ABI 或真实 Winsock。 */
#include <sys/socket.h>
#include <netdb.h>
typedef struct addrinfo ADDRINFOA;
typedef struct addrinfo *PADDRINFOA;
/* Winsock 提供此常量，Linux 严格特性宏可能隐藏同名扩展。 */
#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif

#endif
