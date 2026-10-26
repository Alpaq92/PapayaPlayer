#include "papaya/clock.h"

#include "papaya/media.h"

void
papaya_clock_init(papaya_clock *c)
{
    papaya_mutex_init(&c->lock);
    c->pts = PAPAYA_NOPTS;
    c->last_update = papaya_time_us();
    c->speed = 1.0;
    c->paused = 0;
    c->serial = 0;
}

void
papaya_clock_destroy(papaya_clock *c)
{
    papaya_mutex_destroy(&c->lock);
}

void
papaya_clock_set_at(papaya_clock *c, int64_t pts, int serial, int64_t now)
{
    papaya_mutex_lock(&c->lock);
    c->pts = pts;
    c->last_update = now;
    c->serial = serial;
    papaya_mutex_unlock(&c->lock);
}

void
papaya_clock_set(papaya_clock *c, int64_t pts, int serial)
{
    papaya_clock_set_at(c, pts, serial, papaya_time_us());
}

/* Caller holds the lock. */
static int64_t
current(const papaya_clock *c, int64_t now)
{
    if (c->pts == PAPAYA_NOPTS)
        return PAPAYA_NOPTS;
    if (c->paused)
        return c->pts;
    return c->pts + (int64_t)((double)(now - c->last_update) * c->speed);
}

int64_t
papaya_clock_get(papaya_clock *c, int *serial)
{
    int64_t t;

    papaya_mutex_lock(&c->lock);
    t = current(c, papaya_time_us());
    if (serial)
        *serial = c->serial;
    papaya_mutex_unlock(&c->lock);
    return t;
}

void
papaya_clock_set_paused(papaya_clock *c, int paused)
{
    int64_t now = papaya_time_us();

    papaya_mutex_lock(&c->lock);
    c->pts = current(c, now);
    c->last_update = now;
    c->paused = paused;
    papaya_mutex_unlock(&c->lock);
}

void
papaya_clock_set_speed(papaya_clock *c, double speed)
{
    int64_t now = papaya_time_us();

    papaya_mutex_lock(&c->lock);
    c->pts = current(c, now);
    c->last_update = now;
    c->speed = speed;
    papaya_mutex_unlock(&c->lock);
}
