#include "papaya/queue.h"

#include <stdlib.h>

#include "papaya/thread.h"

typedef struct entry {
    void *item;
    size_t bytes;
    int64_t duration;
    int serial;
    struct entry *next;
} entry;

struct papaya_queue {
    papaya_mutex lock;
    papaya_cond changed;
    papaya_queue_limits limits;
    papaya_queue_free_fn free_fn;
    entry *head, *tail, *spare;
    int count, serial, aborted;
    size_t bytes;
    int64_t duration;
};

papaya_queue *
papaya_queue_new(papaya_queue_limits limits, papaya_queue_free_fn free_fn)
{
    papaya_queue *q = calloc(1, sizeof *q);

    if (!q)
        return NULL;
    papaya_mutex_init(&q->lock);
    papaya_cond_init(&q->changed);
    q->limits = limits;
    q->free_fn = free_fn;
    return q;
}

static void
drop_all(papaya_queue *q)
{
    while (q->head) {
        entry *e = q->head;

        q->head = e->next;
        if (q->free_fn)
            q->free_fn(e->item);
        e->next = q->spare;
        q->spare = e;
    }
    q->tail = NULL;
    q->count = 0;
    q->bytes = 0;
    q->duration = 0;
}

void
papaya_queue_free(papaya_queue **pq)
{
    papaya_queue *q = *pq;

    *pq = NULL;
    if (!q)
        return;
    drop_all(q);
    while (q->spare) {
        entry *e = q->spare;

        q->spare = e->next;
        free(e);
    }
    papaya_cond_destroy(&q->changed);
    papaya_mutex_destroy(&q->lock);
    free(q);
}

static int
full(const papaya_queue *q)
{
    if (q->count == 0)
        return 0;
    return (q->limits.max_bytes && q->bytes >= q->limits.max_bytes)
        || (q->limits.max_duration && q->duration >= q->limits.max_duration)
        || (q->limits.max_items && q->count >= q->limits.max_items);
}

int
papaya_queue_push(papaya_queue *q, void *item, size_t bytes, int64_t duration)
{
    entry *e;

    papaya_mutex_lock(&q->lock);
    while (!q->aborted && full(q))
        papaya_cond_wait(&q->changed, &q->lock);
    if (q->aborted) {
        papaya_mutex_unlock(&q->lock);
        return PAPAYA_QUEUE_ABORTED;
    }
    e = q->spare;
    if (e)
        q->spare = e->next;
    else if (!(e = malloc(sizeof *e))) {
        papaya_mutex_unlock(&q->lock);
        return PAPAYA_QUEUE_ABORTED;
    }
    e->item = item;
    e->bytes = bytes;
    e->duration = duration > 0 ? duration : 0;
    e->serial = q->serial;
    e->next = NULL;
    if (q->tail)
        q->tail->next = e;
    else
        q->head = e;
    q->tail = e;
    q->count++;
    q->bytes += bytes;
    q->duration += e->duration;
    papaya_cond_broadcast(&q->changed);
    papaya_mutex_unlock(&q->lock);
    return PAPAYA_QUEUE_OK;
}

int
papaya_queue_pop(papaya_queue *q, void **item, int *serial, int64_t timeout_us)
{
    int64_t deadline = timeout_us > 0 ? papaya_time_us() + timeout_us : 0;
    entry *e;

    papaya_mutex_lock(&q->lock);
    while (!q->aborted && !q->head) {
        if (timeout_us == 0) {
            papaya_mutex_unlock(&q->lock);
            return PAPAYA_QUEUE_EMPTY;
        }
        if (timeout_us < 0) {
            papaya_cond_wait(&q->changed, &q->lock);
        } else {
            int64_t left = deadline - papaya_time_us();

            if (left <= 0) {
                papaya_mutex_unlock(&q->lock);
                return PAPAYA_QUEUE_TIMEOUT;
            }
            papaya_cond_timedwait(&q->changed, &q->lock, left);
        }
    }
    if (q->aborted) {
        papaya_mutex_unlock(&q->lock);
        return PAPAYA_QUEUE_ABORTED;
    }
    e = q->head;
    q->head = e->next;
    if (!q->head)
        q->tail = NULL;
    q->count--;
    q->bytes -= e->bytes;
    q->duration -= e->duration;
    *item = e->item;
    if (serial)
        *serial = e->serial;
    e->next = q->spare;
    q->spare = e;
    papaya_cond_broadcast(&q->changed);
    papaya_mutex_unlock(&q->lock);
    return PAPAYA_QUEUE_OK;
}

int
papaya_queue_peek(papaya_queue *q, void **item, int *serial)
{
    int r = PAPAYA_QUEUE_EMPTY;

    papaya_mutex_lock(&q->lock);
    if (q->aborted) {
        r = PAPAYA_QUEUE_ABORTED;
    } else if (q->head) {
        *item = q->head->item;
        if (serial)
            *serial = q->head->serial;
        r = PAPAYA_QUEUE_OK;
    }
    papaya_mutex_unlock(&q->lock);
    return r;
}

int
papaya_queue_flush(papaya_queue *q)
{
    int serial;

    papaya_mutex_lock(&q->lock);
    drop_all(q);
    serial = ++q->serial;
    papaya_cond_broadcast(&q->changed);
    papaya_mutex_unlock(&q->lock);
    return serial;
}

int
papaya_queue_serial(papaya_queue *q)
{
    int serial;

    papaya_mutex_lock(&q->lock);
    serial = q->serial;
    papaya_mutex_unlock(&q->lock);
    return serial;
}

void
papaya_queue_abort(papaya_queue *q)
{
    papaya_mutex_lock(&q->lock);
    q->aborted = 1;
    papaya_cond_broadcast(&q->changed);
    papaya_mutex_unlock(&q->lock);
}

void
papaya_queue_resume(papaya_queue *q)
{
    papaya_mutex_lock(&q->lock);
    q->aborted = 0;
    papaya_mutex_unlock(&q->lock);
}

int
papaya_queue_count(papaya_queue *q)
{
    int n;

    papaya_mutex_lock(&q->lock);
    n = q->count;
    papaya_mutex_unlock(&q->lock);
    return n;
}

size_t
papaya_queue_bytes(papaya_queue *q)
{
    size_t n;

    papaya_mutex_lock(&q->lock);
    n = q->bytes;
    papaya_mutex_unlock(&q->lock);
    return n;
}

int64_t
papaya_queue_duration(papaya_queue *q)
{
    int64_t n;

    papaya_mutex_lock(&q->lock);
    n = q->duration;
    papaya_mutex_unlock(&q->lock);
    return n;
}
