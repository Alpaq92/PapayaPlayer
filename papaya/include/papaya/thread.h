#ifndef PAPAYA_THREAD_H
#define PAPAYA_THREAD_H

/* Threads, mutexes and condition variables over Win32 or pthreads. The core
 * does not use SDL for these: papaya_core may be a shared library while SDL is
 * linked statically into the application. */

#include <stdint.h>

#include "papaya/export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct papaya_thread papaya_thread;
typedef int (*papaya_thread_fn)(void *arg);

/* Large enough for SRWLOCK / CONDITION_VARIABLE and pthread_mutex_t /
 * pthread_cond_t on every supported platform (checked in thread.c). */
typedef struct { _Alignas(8) unsigned char opaque[64]; } papaya_mutex;
typedef struct { _Alignas(8) unsigned char opaque[64]; } papaya_cond;

PAPAYA_EXPORT papaya_thread *papaya_thread_start(papaya_thread_fn fn, void *arg, const char *name);
/* Waits for the thread and frees it; returns the thread function's result. */
PAPAYA_EXPORT int papaya_thread_join(papaya_thread *t);

PAPAYA_EXPORT void papaya_mutex_init(papaya_mutex *m);
PAPAYA_EXPORT void papaya_mutex_destroy(papaya_mutex *m);
PAPAYA_EXPORT void papaya_mutex_lock(papaya_mutex *m);
PAPAYA_EXPORT void papaya_mutex_unlock(papaya_mutex *m);

PAPAYA_EXPORT void papaya_cond_init(papaya_cond *c);
PAPAYA_EXPORT void papaya_cond_destroy(papaya_cond *c);
PAPAYA_EXPORT void papaya_cond_wait(papaya_cond *c, papaya_mutex *m);
/* Returns 0 when signalled, 1 on timeout. */
PAPAYA_EXPORT int papaya_cond_timedwait(papaya_cond *c, papaya_mutex *m, int64_t timeout_us);
PAPAYA_EXPORT void papaya_cond_signal(papaya_cond *c);
PAPAYA_EXPORT void papaya_cond_broadcast(papaya_cond *c);

/* Monotonic time in microseconds, from an arbitrary origin. */
PAPAYA_EXPORT int64_t papaya_time_us(void);
PAPAYA_EXPORT void papaya_sleep_us(int64_t us);

#ifdef __cplusplus
}
#endif

#endif
