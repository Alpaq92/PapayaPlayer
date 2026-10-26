#ifndef PAPAYA_CLOCK_H
#define PAPAYA_CLOCK_H

/* A media clock: a position that advances with real time at a given speed
 * (TECH-STACK.md step 5). The audio output sets it from what the device has
 * actually played; video frames are shown or dropped against it. */

#include <stdint.h>

#include "papaya/export.h"
#include "papaya/thread.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct papaya_clock {
    papaya_mutex lock;
    int64_t pts;         /* media time at last_update, PAPAYA_NOPTS when unset */
    int64_t last_update; /* papaya_time_us() of the last set */
    double speed;
    int paused;
    int serial;          /* queue serial the time belongs to */
} papaya_clock;

PAPAYA_EXPORT void papaya_clock_init(papaya_clock *c);
PAPAYA_EXPORT void papaya_clock_destroy(papaya_clock *c);
/* pts was current at time now (papaya_time_us()). */
PAPAYA_EXPORT void papaya_clock_set_at(papaya_clock *c, int64_t pts, int serial, int64_t now);
PAPAYA_EXPORT void papaya_clock_set(papaya_clock *c, int64_t pts, int serial);
/* Media time now, or PAPAYA_NOPTS when unset; *serial may be NULL. */
PAPAYA_EXPORT int64_t papaya_clock_get(papaya_clock *c, int *serial);
PAPAYA_EXPORT void papaya_clock_set_paused(papaya_clock *c, int paused);
PAPAYA_EXPORT void papaya_clock_set_speed(papaya_clock *c, double speed);

#ifdef __cplusplus
}
#endif

#endif
