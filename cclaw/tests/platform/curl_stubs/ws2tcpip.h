#ifndef CC_TEST_CURL_WS2TCPIP_H
#define CC_TEST_CURL_WS2TCPIP_H

#include "winsock2.h"
/* 对应 API 在模型翻译单元中重命名到确定性解析替身。 */
#define GetAddrInfoA getaddrinfo
#define FreeAddrInfoA freeaddrinfo
#define GetNameInfoA getnameinfo

#endif
