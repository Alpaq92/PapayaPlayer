/* The Ittiam libraries' threading layer (ithread.h) over papaya's own threads,
 * for every platform: compiled in place of their pthread-based
 * common/ithread.c (TECH-STACK.md wrapper-first policy: no patch to libavc or
 * libhevc). The functions only take caller-allocated memory of the sizes
 * reported here. A semaphore is a counter under a mutex and a condition.
 *
 * Not provided: ithread_exit (the libraries' only call is commented out) and
 * thread affinity (reported as unsupported). */
#include <stdlib.h>

#include "ih264_typedefs.h"
#include "ithread.h"
#include "papaya/thread.h"

typedef struct {
    void *(*start)(void *);
    void *arg;
} start_t;

typedef struct {
    papaya_mutex lock;
    papaya_cond posted;
    UWORD32 count;
} sem_t_;

static int
trampoline(void *p)
{
    start_t s = *(start_t *)p;

    free(p);
    s.start(s.arg);
    return 0;
}

UWORD32 ithread_get_handle_size(void) { return sizeof(papaya_thread *); }
UWORD32 ithread_get_mutex_lock_size(void) { return sizeof(papaya_mutex); }
WORD32 ithread_get_mutex_struct_size(void) { return sizeof(papaya_mutex); }
UWORD32 ithread_get_sem_struct_size(void) { return sizeof(sem_t_); }
WORD32 ithread_get_cond_struct_size(void) { return sizeof(papaya_cond); }
UWORD32 ithread_get_cond_size(void) { return sizeof(papaya_cond); }

WORD32
ithread_create(void *thread_handle, void *attribute, void *strt, void *argument)
{
    start_t *s = malloc(sizeof *s);
    papaya_thread *t;

    (void)attribute;
    if (!s)
        return -1;
    s->start = (void *(*)(void *))strt;
    s->arg = argument;
    t = papaya_thread_start(trampoline, s, "ittiam");
    if (!t) {
        free(s);
        return -1;
    }
    *(papaya_thread **)thread_handle = t;
    return 0;
}

void
ithread_exit(void *val_ptr)
{
    (void)val_ptr;
    abort(); /* never called: see the top of this file */
}

WORD32
ithread_join(void *thread_id, void **val_ptr)
{
    if (val_ptr)
        *val_ptr = NULL;
    papaya_thread_join(*(papaya_thread **)thread_id);
    return 0;
}

WORD32 ithread_mutex_init(void *mutex) { papaya_mutex_init(mutex); return 0; }
WORD32 ithread_mutex_destroy(void *mutex) { papaya_mutex_destroy(mutex); return 0; }
WORD32 ithread_mutex_lock(void *mutex) { papaya_mutex_lock(mutex); return 0; }
WORD32 ithread_mutex_unlock(void *mutex) { papaya_mutex_unlock(mutex); return 0; }

void ithread_yield(void) { papaya_sleep_us(0); }
void ithread_sleep(UWORD32 u4_time) { papaya_sleep_us((int64_t)u4_time * 1000000); }
void ithread_msleep(UWORD32 u4_time_ms) { papaya_sleep_us((int64_t)u4_time_ms * 1000); }
void ithread_usleep(UWORD32 u4_time_us) { papaya_sleep_us(u4_time_us); }

WORD32
ithread_sem_init(void *sem, WORD32 pshared, UWORD32 value)
{
    sem_t_ *s = sem;

    (void)pshared;
    papaya_mutex_init(&s->lock);
    papaya_cond_init(&s->posted);
    s->count = value;
    return 0;
}

WORD32
ithread_sem_post(void *sem)
{
    sem_t_ *s = sem;

    papaya_mutex_lock(&s->lock);
    s->count++;
    papaya_cond_signal(&s->posted);
    papaya_mutex_unlock(&s->lock);
    return 0;
}

WORD32
ithread_sem_wait(void *sem)
{
    sem_t_ *s = sem;

    papaya_mutex_lock(&s->lock);
    while (s->count == 0)
        papaya_cond_wait(&s->posted, &s->lock);
    s->count--;
    papaya_mutex_unlock(&s->lock);
    return 0;
}

WORD32
ithread_sem_destroy(void *sem)
{
    sem_t_ *s = sem;

    papaya_cond_destroy(&s->posted);
    papaya_mutex_destroy(&s->lock);
    return 0;
}

WORD32
ithread_set_affinity(WORD32 core_id)
{
    (void)core_id;
    return -1; /* left to the OS */
}

void
ithread_set_name(CHAR *pc_thread_name)
{
    (void)pc_thread_name; /* named "ittiam" at creation */
}

WORD32 ithread_cond_init(void *cond) { papaya_cond_init(cond); return 0; }
WORD32 ithread_cond_destroy(void *cond) { papaya_cond_destroy(cond); return 0; }
WORD32 ithread_cond_wait(void *cond, void *mutex) { papaya_cond_wait(cond, mutex); return 0; }
WORD32 ithread_cond_signal(void *cond) { papaya_cond_signal(cond); return 0; }
WORD32 ithread_cond_broadcast(void *cond) { papaya_cond_broadcast(cond); return 0; }
