/* Win32 implementation of the Ittiam libraries' threading layer (ithread.h),
 * compiled in place of their pthread-based common/ithread.c on Windows
 * (TECH-STACK.md wrapper-first policy: no patch to libavc, libhevc or libmpeg2).
 * The same file serves all three; each build puts that library's common/
 * directory on the include path. */
#include <limits.h>
#include <process.h>
#include <stdlib.h>
#include <windows.h>

#if __has_include("ih264_typedefs.h")
#include "ih264_typedefs.h"
#elif __has_include("ihevc_typedefs.h")
#include "ihevc_typedefs.h"
#else
#include "iv_datatypedef.h"
#endif
#include "ithread.h"

typedef struct {
    void *(*start)(void *);
    void *arg;
} ithread_start_t;

static unsigned __stdcall
ithread_trampoline(void *p)
{
    ithread_start_t s = *(ithread_start_t *)p;

    free(p);
    s.start(s.arg);
    return 0;
}

UWORD32
ithread_get_handle_size(void)
{
    return sizeof(HANDLE);
}

UWORD32
ithread_get_mutex_lock_size(void)
{
    return sizeof(SRWLOCK);
}

WORD32
ithread_create(void *thread_handle, void *attribute, void *strt, void *argument)
{
    ithread_start_t *s = malloc(sizeof *s);
    uintptr_t h;

    (void)attribute;
    if (!s)
        return -1;
    s->start = (void *(*)(void *))strt;
    s->arg = argument;
    h = _beginthreadex(NULL, 0, ithread_trampoline, s, 0, NULL);
    if (!h) {
        free(s);
        return -1;
    }
    *(HANDLE *)thread_handle = (HANDLE)h;
    return 0;
}

void
ithread_exit(void *val_ptr)
{
    (void)val_ptr;
    _endthreadex(0);
}

WORD32
ithread_join(void *thread_id, void **val_ptr)
{
    HANDLE h = *(HANDLE *)thread_id;

    if (val_ptr)
        *val_ptr = NULL;
    if (WaitForSingleObject(h, INFINITE) != WAIT_OBJECT_0)
        return -1;
    CloseHandle(h);
    return 0;
}

WORD32
ithread_get_mutex_struct_size(void)
{
    return sizeof(SRWLOCK);
}

WORD32
ithread_mutex_init(void *mutex)
{
    InitializeSRWLock((SRWLOCK *)mutex);
    return 0;
}

WORD32
ithread_mutex_destroy(void *mutex)
{
    (void)mutex;
    return 0;
}

WORD32
ithread_mutex_lock(void *mutex)
{
    AcquireSRWLockExclusive((SRWLOCK *)mutex);
    return 0;
}

WORD32
ithread_mutex_unlock(void *mutex)
{
    ReleaseSRWLockExclusive((SRWLOCK *)mutex);
    return 0;
}

void
ithread_yield(void)
{
    SwitchToThread();
}

void
ithread_sleep(UWORD32 u4_time)
{
    Sleep(u4_time * 1000);
}

void
ithread_msleep(UWORD32 u4_time_ms)
{
    Sleep(u4_time_ms);
}

void
ithread_usleep(UWORD32 u4_time_us)
{
    Sleep((u4_time_us + 999) / 1000);
}

UWORD32
ithread_get_sem_struct_size(void)
{
    return sizeof(HANDLE);
}

WORD32
ithread_sem_init(void *sem, WORD32 pshared, UWORD32 value)
{
    HANDLE h;

    (void)pshared;
    h = CreateSemaphoreW(NULL, (LONG)value, LONG_MAX, NULL);
    if (!h)
        return -1;
    *(HANDLE *)sem = h;
    return 0;
}

WORD32
ithread_sem_post(void *sem)
{
    return ReleaseSemaphore(*(HANDLE *)sem, 1, NULL) ? 0 : -1;
}

WORD32
ithread_sem_wait(void *sem)
{
    return WaitForSingleObject(*(HANDLE *)sem, INFINITE) == WAIT_OBJECT_0 ? 0 : -1;
}

WORD32
ithread_sem_destroy(void *sem)
{
    return CloseHandle(*(HANDLE *)sem) ? 0 : -1;
}

WORD32
ithread_set_affinity(WORD32 core_id)
{
    if (core_id < 0 || core_id >= (WORD32)(sizeof(DWORD_PTR) * CHAR_BIT))
        return -1;
    return SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << core_id) ? 0 : -1;
}

void
ithread_set_name(CHAR *pc_thread_name)
{
    (void)pc_thread_name;
}

WORD32
ithread_get_cond_struct_size(void)
{
    return sizeof(CONDITION_VARIABLE);
}

UWORD32
ithread_get_cond_size(void)
{
    return sizeof(CONDITION_VARIABLE);
}

WORD32
ithread_cond_init(void *cond)
{
    InitializeConditionVariable((CONDITION_VARIABLE *)cond);
    return 0;
}

WORD32
ithread_cond_destroy(void *cond)
{
    (void)cond;
    return 0;
}

WORD32
ithread_cond_wait(void *cond, void *mutex)
{
    return SleepConditionVariableSRW((CONDITION_VARIABLE *)cond, (SRWLOCK *)mutex, INFINITE, 0)
        ? 0 : -1;
}

WORD32
ithread_cond_signal(void *cond)
{
    WakeConditionVariable((CONDITION_VARIABLE *)cond);
    return 0;
}

WORD32
ithread_cond_broadcast(void *cond)
{
    WakeAllConditionVariable((CONDITION_VARIABLE *)cond);
    return 0;
}
