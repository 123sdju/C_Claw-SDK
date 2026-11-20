#pragma once
#include <stdint.h>
#include <stdio.h>
#define _O_RDONLY 0
#define _O_WRONLY 1
/** @brief 原生句柄转交模型CRT fd。 @param[in] handle 拥有句柄值，成功由fd接管。 @param[in] flags 访问模式。 @return fd或-1，失败不接管。 */
int _open_osfhandle(intptr_t handle, int flags);
/** @brief CRT fd转交模型FILE。 @param[in] fd 拥有描述符，成功由FILE接管。 @param[in] mode 借用模式串。 @return FILE或NULL，失败不消费fd。 */
FILE *_fdopen(int fd, const char *mode);
/** @brief 关闭尚未转给FILE的CRT fd。 @param[in] fd 拥有描述符。 @return 成功零，重复关闭触发断言。 */
int _close(int fd);
