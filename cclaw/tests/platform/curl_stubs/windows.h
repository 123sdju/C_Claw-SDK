#ifndef CC_TEST_CURL_WINDOWS_H
#define CC_TEST_CURL_WINDOWS_H

#include <pthread.h>
/* 仅提供生产 curl 文件使用的锁及序号操作，原生 Windows 实际 API 仍需原生验证。 */
typedef pthread_mutex_t SRWLOCK;
typedef int LONG;
#define SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define AcquireSRWLockExclusive(lock) ((void)pthread_mutex_lock(lock))
#define ReleaseSRWLockExclusive(lock) ((void)pthread_mutex_unlock(lock))
#define InterlockedIncrement(value) __atomic_add_fetch((value), 1, __ATOMIC_RELAXED)

#endif
