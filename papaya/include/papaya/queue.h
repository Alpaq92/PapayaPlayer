#ifndef PAPAYA_QUEUE_H
#define PAPAYA_QUEUE_H

/* A bounded, blocking FIFO between two pipeline stages (TECH-STACK.md step 5).
 * Limits are in bytes and in duration, whichever is reached first, plus a
 * count. Every item carries the queue's serial at push time; a flush (on seek)
 * bumps the serial so consumers can tell stale items from new ones. */

#include <stddef.h>
#include <stdint.h>

#include "papaya/export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct papaya_queue papaya_queue;
typedef void (*papaya_queue_free_fn)(void *item);

typedef struct papaya_queue_limits {
    size_t max_bytes;      /* 0: no limit */
    int64_t max_duration;  /* microseconds; 0: no limit */
    int max_items;         /* 0: no limit */
} papaya_queue_limits;

enum {
    PAPAYA_QUEUE_OK = 0,
    PAPAYA_QUEUE_ABORTED = -1,  /* papaya_queue_abort was called */
    PAPAYA_QUEUE_TIMEOUT = -2,
    PAPAYA_QUEUE_EMPTY = -3,
};

PAPAYA_EXPORT papaya_queue *papaya_queue_new(papaya_queue_limits limits, papaya_queue_free_fn free_fn);
/* Frees the queue and every item still in it. */
PAPAYA_EXPORT void papaya_queue_free(papaya_queue **q);

/* Blocks while the queue is full. An empty queue always accepts one item, so
 * an item larger than the limits cannot block forever. */
PAPAYA_EXPORT int papaya_queue_push(papaya_queue *q, void *item, size_t bytes, int64_t duration);
/* timeout_us < 0 waits forever, 0 does not wait. *serial may be NULL. */
PAPAYA_EXPORT int papaya_queue_pop(papaya_queue *q, void **item, int *serial, int64_t timeout_us);
/* Like pop without removing; the item stays owned by the queue. */
PAPAYA_EXPORT int papaya_queue_peek(papaya_queue *q, void **item, int *serial);

/* Drops every item and increments the serial; returns the new serial. */
PAPAYA_EXPORT int papaya_queue_flush(papaya_queue *q);
PAPAYA_EXPORT int papaya_queue_serial(papaya_queue *q);
/* Wakes and fails every blocked and future push / pop until resumed. */
PAPAYA_EXPORT void papaya_queue_abort(papaya_queue *q);
PAPAYA_EXPORT void papaya_queue_resume(papaya_queue *q);

PAPAYA_EXPORT int papaya_queue_count(papaya_queue *q);
PAPAYA_EXPORT size_t papaya_queue_bytes(papaya_queue *q);
PAPAYA_EXPORT int64_t papaya_queue_duration(papaya_queue *q);

#ifdef __cplusplus
}
#endif

#endif
