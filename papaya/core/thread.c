#include "papaya/thread.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include <process.h>
#include <windows.h>

_Static_assert(sizeof(SRWLOCK) <= sizeof(papaya_mutex), "papaya_mutex too small");
_Static_assert(sizeof(CONDITION_VARIABLE) <= sizeof(papaya_cond), "papaya_cond too small");

struct papaya_thread {
    HANDLE handle;
    papaya_thread_fn fn;
    void *arg;
    int result;
    wchar_t name[32];
};

static unsigned __stdcall
thread_main(void *p)
{
    papaya_thread *t = p;

    if (t->name[0])
        SetThreadDescription(GetCurrentThread(), t->name);
    t->result = t->fn(t->arg);
    return 0;
}

papaya_thread *
papaya_thread_start(papaya_thread_fn fn, void *arg, const char *name)
{
    papaya_thread *t = calloc(1, sizeof *t);

    if (!t)
        return NULL;
    t->fn = fn;
    t->arg = arg;
    if (name)
        MultiByteToWideChar(CP_UTF8, 0, name, -1, t->name, (int)(sizeof t->name / sizeof t->name[0]) - 1);
    /* _beginthreadex returns its HANDLE as a uintptr_t. */
    t->handle = (HANDLE)_beginthreadex(NULL, 0, thread_main, t, 0, NULL); // NOLINT(performance-no-int-to-ptr)
    if (!t->handle) {
        free(t);
        return NULL;
    }
    return t;
}

int
papaya_thread_join(papaya_thread *t)
{
    int r;

    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    r = t->result;
    free(t);
    return r;
}

void papaya_mutex_init(papaya_mutex *m) { InitializeSRWLock((SRWLOCK *)m->opaque); }
void papaya_mutex_destroy(papaya_mutex *m) { (void)m; }
void papaya_mutex_lock(papaya_mutex *m) { AcquireSRWLockExclusive((SRWLOCK *)m->opaque); }
void papaya_mutex_unlock(papaya_mutex *m) { ReleaseSRWLockExclusive((SRWLOCK *)m->opaque); }

void papaya_cond_init(papaya_cond *c) { InitializeConditionVariable((CONDITION_VARIABLE *)c->opaque); }
void papaya_cond_destroy(papaya_cond *c) { (void)c; }

void
papaya_cond_wait(papaya_cond *c, papaya_mutex *m)
{
    SleepConditionVariableSRW((CONDITION_VARIABLE *)c->opaque, (SRWLOCK *)m->opaque, INFINITE, 0);
}

int
papaya_cond_timedwait(papaya_cond *c, papaya_mutex *m, int64_t timeout_us)
{
    DWORD ms = timeout_us <= 0 ? 0 : (DWORD)((timeout_us + 999) / 1000);

    if (SleepConditionVariableSRW((CONDITION_VARIABLE *)c->opaque, (SRWLOCK *)m->opaque, ms, 0))
        return 0;
    return 1;
}

void papaya_cond_signal(papaya_cond *c) { WakeConditionVariable((CONDITION_VARIABLE *)c->opaque); }
void papaya_cond_broadcast(papaya_cond *c) { WakeAllConditionVariable((CONDITION_VARIABLE *)c->opaque); }

int64_t
papaya_time_us(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;

    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (int64_t)(now.QuadPart / freq.QuadPart * 1000000
                     + now.QuadPart % freq.QuadPart * 1000000 / freq.QuadPart);
}

void
papaya_sleep_us(int64_t us)
{
    Sleep(us <= 0 ? 0 : (DWORD)((us + 999) / 1000));
}

#else

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#if defined(__linux__)
#include <sys/prctl.h>
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h>
#endif

_Static_assert(sizeof(pthread_mutex_t) <= sizeof(papaya_mutex), "papaya_mutex too small");
_Static_assert(sizeof(pthread_cond_t) <= sizeof(papaya_cond), "papaya_cond too small");

struct papaya_thread {
    pthread_t handle;
    papaya_thread_fn fn;
    void *arg;
    int result;
    char name[16];
};

static void *
thread_main(void *p)
{
    papaya_thread *t = p;

    if (t->name[0]) {
#if defined(__APPLE__)
        pthread_setname_np(t->name);
#elif defined(__linux__)
        prctl(PR_SET_NAME, t->name);
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
        pthread_set_name_np(pthread_self(), t->name);
#elif defined(__NetBSD__)
        pthread_setname_np(pthread_self(), "%s", t->name);
#endif
    }
    t->result = t->fn(t->arg);
    return NULL;
}

papaya_thread *
papaya_thread_start(papaya_thread_fn fn, void *arg, const char *name)
{
    papaya_thread *t = calloc(1, sizeof *t);

    if (!t)
        return NULL;
    t->fn = fn;
    t->arg = arg;
    if (name) {
        strncpy(t->name, name, sizeof t->name - 1);
    }
    if (pthread_create(&t->handle, NULL, thread_main, t) != 0) {
        free(t);
        return NULL;
    }
    return t;
}

int
papaya_thread_join(papaya_thread *t)
{
    int r;

    pthread_join(t->handle, NULL);
    r = t->result;
    free(t);
    return r;
}

void papaya_mutex_init(papaya_mutex *m) { pthread_mutex_init((pthread_mutex_t *)m->opaque, NULL); }
void papaya_mutex_destroy(papaya_mutex *m) { pthread_mutex_destroy((pthread_mutex_t *)m->opaque); }
void papaya_mutex_lock(papaya_mutex *m) { pthread_mutex_lock((pthread_mutex_t *)m->opaque); }
void papaya_mutex_unlock(papaya_mutex *m) { pthread_mutex_unlock((pthread_mutex_t *)m->opaque); }

void
papaya_cond_init(papaya_cond *c)
{
    pthread_condattr_t a;

    pthread_condattr_init(&a);
#if !defined(__APPLE__)
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
#endif
    pthread_cond_init((pthread_cond_t *)c->opaque, &a);
    pthread_condattr_destroy(&a);
}

void papaya_cond_destroy(papaya_cond *c) { pthread_cond_destroy((pthread_cond_t *)c->opaque); }

void
papaya_cond_wait(papaya_cond *c, papaya_mutex *m)
{
    pthread_cond_wait((pthread_cond_t *)c->opaque, (pthread_mutex_t *)m->opaque);
}

int
papaya_cond_timedwait(papaya_cond *c, papaya_mutex *m, int64_t timeout_us)
{
    struct timespec ts;
    int r;

    if (timeout_us < 0)
        timeout_us = 0;
#if defined(__APPLE__)
    ts.tv_sec = (time_t)(timeout_us / 1000000);
    ts.tv_nsec = (long)(timeout_us % 1000000 * 1000);
    r = pthread_cond_timedwait_relative_np((pthread_cond_t *)c->opaque,
                                           (pthread_mutex_t *)m->opaque, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += (time_t)(timeout_us / 1000000);
    ts.tv_nsec += (long)(timeout_us % 1000000 * 1000);
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    r = pthread_cond_timedwait((pthread_cond_t *)c->opaque, (pthread_mutex_t *)m->opaque, &ts);
#endif
    return r == ETIMEDOUT ? 1 : 0;
}

void papaya_cond_signal(papaya_cond *c) { pthread_cond_signal((pthread_cond_t *)c->opaque); }
void papaya_cond_broadcast(papaya_cond *c) { pthread_cond_broadcast((pthread_cond_t *)c->opaque); }

int64_t
papaya_time_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void
papaya_sleep_us(int64_t us)
{
    struct timespec ts;

    if (us <= 0) {
        sched_yield(); /* as Sleep(0) on Windows: let another thread run */
        return;
    }
    ts.tv_sec = (time_t)(us / 1000000);
    ts.tv_nsec = (long)(us % 1000000 * 1000);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

#endif
