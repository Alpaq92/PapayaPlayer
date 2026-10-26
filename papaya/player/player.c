#include "papaya/player.h"

#include <stdatomic.h>
#include <stdlib.h>

#include "papaya/audio_out.h"
#include "papaya/clock.h"
#include "papaya/decode.h"
#include "papaya/queue.h"
#include "papaya/thread.h"

/* Packets travel with the packet queue's serial; a seek flushes the queues,
 * which bumps it, and everything older is dropped downstream. A NULL packet
 * marks the end of the stream. */
typedef struct {
    papaya_frame *frame;
    int serial;
} frame_item;

typedef struct {
    int index;                  /* stream index, -1 if none */
    papaya_queue *packets;
    papaya_decoder *decoder;
    papaya_frame_pool *pool;
    papaya_thread *thread;
    atomic_int finished;        /* decoder drained at end of stream */
} track;

struct papaya_player {
    papaya_demuxer *demux;
    papaya_player_config config;
    track video, audio;

    papaya_queue *frames;       /* decoded video, frame_item */
    papaya_audio_out *ao;
    papaya_clock clock;         /* audio clock, or system clock without audio */

    papaya_thread *demux_thread;
    papaya_mutex lock;
    papaya_cond wakeup;         /* demux thread: seek requested or quitting */
    int64_t seek_to;            /* PAPAYA_NOPTS: none pending */
    atomic_llong seek_target;   /* decode threads drop frames before this */
    atomic_int quit;
    atomic_int demux_eof;
    atomic_int paused;
    atomic_int dropped;

    /* UI thread only */
    papaya_frame *current;
    int current_serial;
};

static void
free_packet(void *item)
{
    papaya_packet *p = item;

    papaya_packet_free(&p);
}

static void
free_frame_item(void *item)
{
    frame_item *fi = item;

    papaya_frame_unref(&fi->frame);
    free(fi);
}

static void
wake(papaya_player *p)
{
    if (p->config.wake)
        p->config.wake(p->config.wake_user);
}

static size_t
packet_bytes(const papaya_packet *pkt)
{
    return pkt ? pkt->size + sizeof *pkt : 0;
}

/* ---- demux thread --------------------------------------------------------- */

static void
flush_all(papaya_player *p, int64_t target)
{
    atomic_store(&p->seek_target, target);
    if (p->video.packets)
        papaya_queue_flush(p->video.packets);
    if (p->audio.packets) {
        int serial = papaya_queue_flush(p->audio.packets);

        if (p->ao)
            papaya_audio_out_flush(p->ao, serial);
    }
    /* Decoded video of the old serial is dropped by the UI thread, the only
     * reader of p->frames, so it never loses a frame it is looking at. */
    atomic_store(&p->video.finished, 0);
    atomic_store(&p->audio.finished, 0);
    atomic_store(&p->demux_eof, 0);
}

static int
demux_main(void *arg)
{
    papaya_player *p = arg;

    while (!atomic_load(&p->quit)) {
        papaya_packet *pkt;
        int64_t seek;
        int r;

        papaya_mutex_lock(&p->lock);
        seek = p->seek_to;
        p->seek_to = PAPAYA_NOPTS;
        papaya_mutex_unlock(&p->lock);
        if (seek != PAPAYA_NOPTS) {
            int64_t from = seek;

            if (p->audio.index >= 0) {
                int64_t preroll = papaya_demux_stream(p->demux, p->audio.index)->seek_preroll;

                if (preroll > 0 && preroll != PAPAYA_NOPTS)
                    from -= preroll;
            }
            papaya_demux_seek(p->demux, from < 0 ? 0 : from);
            flush_all(p, seek);
            wake(p);
        }
        if (atomic_load(&p->demux_eof)) {
            papaya_mutex_lock(&p->lock);
            if (p->seek_to == PAPAYA_NOPTS && !atomic_load(&p->quit))
                papaya_cond_timedwait(&p->wakeup, &p->lock, 100000);
            papaya_mutex_unlock(&p->lock);
            continue;
        }
        r = papaya_demux_read(p->demux, &pkt);
        if (r != 0) {
            /* End of stream (or an unreadable rest): drain both decoders. */
            if (p->video.packets)
                papaya_queue_push(p->video.packets, NULL, 0, 0);
            if (p->audio.packets)
                papaya_queue_push(p->audio.packets, NULL, 0, 0);
            atomic_store(&p->demux_eof, 1);
            continue;
        }
        if (pkt->stream == p->video.index)
            papaya_queue_push(p->video.packets, pkt, packet_bytes(pkt), pkt->duration > 0 ? pkt->duration : 0);
        else if (pkt->stream == p->audio.index)
            papaya_queue_push(p->audio.packets, pkt, packet_bytes(pkt), pkt->duration > 0 ? pkt->duration : 0);
        else
            papaya_packet_free(&pkt);
    }
    return 0;
}

/* ---- decode threads ------------------------------------------------------- */

typedef struct {
    papaya_player *p;
    track *t;
    int serial;
} decode_args;

/* Hands one decoded frame on. Returns < 0 when shutting down. */
static int
deliver(papaya_player *p, track *t, papaya_frame *f, int serial)
{
    int64_t target = atomic_load(&p->seek_target);

    if (serial != papaya_queue_serial(t->packets)) {
        papaya_frame_unref(&f);
        return 0;
    }
    /* Exact seek: frames before the target are decoded but not shown. */
    if (target != PAPAYA_NOPTS && f->pts != PAPAYA_NOPTS && f->pts + (f->duration > 0 ? f->duration : 0) <= target) {
        papaya_frame_unref(&f);
        return 0;
    }
    if (t == &p->audio) {
        int r = papaya_audio_out_write(p->ao, f, serial);

        papaya_frame_unref(&f);
        return r;
    } else {
        frame_item *fi = malloc(sizeof *fi);

        if (!fi) {
            papaya_frame_unref(&f);
            return -1;
        }
        fi->frame = f;
        fi->serial = serial;
        if (papaya_queue_push(p->frames, fi, 0, 0) != PAPAYA_QUEUE_OK) {
            free_frame_item(fi);
            return -1;
        }
        wake(p);
        return 0;
    }
}

static int
decode_main(void *arg)
{
    papaya_player *p = ((decode_args *)arg)->p;
    track *t = ((decode_args *)arg)->t;
    int last_serial = -1;

    free(arg);
    while (!atomic_load(&p->quit)) {
        void *item;
        int serial, r;
        papaya_frame *f;

        if (papaya_queue_pop(t->packets, &item, &serial, -1) != PAPAYA_QUEUE_OK)
            break;
        if (serial != last_serial) {
            if (last_serial >= 0)
                papaya_decoder_flush(t->decoder);
            last_serial = serial;
        }
        r = papaya_decoder_send(t->decoder, item);
        free_packet(item);
        (void)r; /* a broken packet is skipped; the next keyframe recovers */
        while (papaya_decoder_receive(t->decoder, &f) == PAPAYA_DECODE_OK)
            if (deliver(p, t, f, serial) < 0)
                return 0;
        if (!item) {
            papaya_decoder_flush(t->decoder); /* ready for a seek after the end */
            atomic_store(&t->finished, 1);
            wake(p);
        }
    }
    return 0;
}

/* ---- open / close --------------------------------------------------------- */

static int
open_track(track *t, const papaya_stream_info *s, papaya_queue_limits limits)
{
    t->pool = papaya_frame_pool_new();
    t->packets = papaya_queue_new(limits, free_packet);
    if (!t->pool || !t->packets)
        return -1;
    t->decoder = papaya_decoder_open(s, t->pool);
    return t->decoder ? 0 : -1;
}

static void
close_track(track *t)
{
    papaya_decoder_close(&t->decoder);
    papaya_queue_free(&t->packets);
    papaya_frame_pool_release(&t->pool);
}

static int
start_decoder(papaya_player *p, track *t, const char *name)
{
    decode_args *a = malloc(sizeof *a);

    if (!a)
        return -1;
    a->p = p;
    a->t = t;
    a->serial = 0;
    t->thread = papaya_thread_start(decode_main, a, name);
    if (!t->thread) {
        free(a);
        return -1;
    }
    return 0;
}

papaya_player *
papaya_player_open(const char *path, const papaya_player_config *config)
{
    /* Packet queues: a few seconds or a few tens of MB, whichever first. */
    papaya_queue_limits video_limits = { .max_bytes = 64u << 20, .max_duration = 5000000, .max_items = 600 };
    papaya_queue_limits audio_limits = { .max_bytes = 4u << 20, .max_duration = 5000000, .max_items = 1000 };
    papaya_player *p = calloc(1, sizeof *p);

    if (!p)
        return NULL;
    if (config)
        p->config = *config;
    p->video.index = p->audio.index = -1;
    p->seek_to = PAPAYA_NOPTS;
    atomic_init(&p->seek_target, PAPAYA_NOPTS);
    atomic_init(&p->paused, 1);
    papaya_mutex_init(&p->lock);
    papaya_cond_init(&p->wakeup);
    papaya_clock_init(&p->clock);
    p->current_serial = -1;

    p->demux = papaya_demux_open(papaya_io_open_file(path));
    if (!p->demux)
        goto fail;
    for (int i = 0; i < papaya_demux_stream_count(p->demux); i++) {
        const papaya_stream_info *s = papaya_demux_stream(p->demux, i);
        track *t = s->kind == PAPAYA_MEDIA_VIDEO ? &p->video : &p->audio;

        if (t->index >= 0)
            continue;
        if (open_track(t, s, s->kind == PAPAYA_MEDIA_VIDEO ? video_limits : audio_limits) == 0)
            t->index = i;
        else
            close_track(t);
    }
    if (p->video.index < 0 && p->audio.index < 0)
        goto fail;
    if (p->audio.index >= 0) {
        const papaya_stream_info *s = papaya_demux_stream(p->demux, p->audio.index);
        papaya_audio_out_config ac = { .sample_rate = s->sample_rate, .channels = s->channels,
                                       .clock = &p->clock, .null_device = p->config.null_audio };

        /* Opus always decodes at 48 kHz, whatever the container says. */
        if (s->codec == PAPAYA_CODEC_OPUS)
            ac.sample_rate = 48000;
        p->ao = papaya_audio_out_open(&ac);
        if (!p->ao) {
            close_track(&p->audio);
            p->audio.index = -1;
            if (p->video.index < 0)
                goto fail;
        } else {
            papaya_audio_out_set_paused(p->ao, 1);
        }
    }
    p->frames = papaya_queue_new((papaya_queue_limits){ .max_items = 8 }, free_frame_item);
    if (!p->frames)
        goto fail;
    if ((p->video.index >= 0 && start_decoder(p, &p->video, "video decode") != 0)
        || (p->audio.index >= 0 && start_decoder(p, &p->audio, "audio decode") != 0))
        goto fail;
    p->demux_thread = papaya_thread_start(demux_main, p, "demux");
    if (!p->demux_thread)
        goto fail;
    papaya_clock_set_paused(&p->clock, 1);
    return p;
fail:
    papaya_player_close(&p);
    return NULL;
}

void
papaya_player_close(papaya_player **pp)
{
    papaya_player *p = *pp;

    *pp = NULL;
    if (!p)
        return;
    atomic_store(&p->quit, 1);
    papaya_mutex_lock(&p->lock);
    papaya_cond_broadcast(&p->wakeup);
    papaya_mutex_unlock(&p->lock);
    if (p->video.packets)
        papaya_queue_abort(p->video.packets);
    if (p->audio.packets)
        papaya_queue_abort(p->audio.packets);
    if (p->frames)
        papaya_queue_abort(p->frames);
    if (p->ao)
        papaya_audio_out_abort(p->ao);
    if (p->demux_thread)
        papaya_thread_join(p->demux_thread);
    if (p->video.thread)
        papaya_thread_join(p->video.thread);
    if (p->audio.thread)
        papaya_thread_join(p->audio.thread);
    papaya_audio_out_close(&p->ao);
    papaya_frame_unref(&p->current);
    papaya_queue_free(&p->frames);
    close_track(&p->video);
    close_track(&p->audio);
    papaya_demux_close(&p->demux);
    papaya_clock_destroy(&p->clock);
    papaya_cond_destroy(&p->wakeup);
    papaya_mutex_destroy(&p->lock);
    free(p);
}

/* ---- control -------------------------------------------------------------- */

void
papaya_player_set_paused(papaya_player *p, int paused)
{
    atomic_store(&p->paused, paused);
    if (p->ao)
        papaya_audio_out_set_paused(p->ao, paused);
    else
        papaya_clock_set_paused(&p->clock, paused);
    wake(p);
}

int papaya_player_paused(papaya_player *p) { return atomic_load(&p->paused); }

void
papaya_player_seek(papaya_player *p, int64_t pts)
{
    int64_t duration = papaya_demux_duration(p->demux);

    if (pts < 0)
        pts = 0;
    if (duration != PAPAYA_NOPTS && pts > duration)
        pts = duration;
    papaya_mutex_lock(&p->lock);
    p->seek_to = pts;
    papaya_cond_broadcast(&p->wakeup);
    papaya_mutex_unlock(&p->lock);
    /* Until the new audio plays, the clock shows the target. */
    papaya_clock_set(&p->clock, pts, -1);
    if (atomic_load(&p->paused))
        papaya_clock_set_paused(&p->clock, 1);
}

void
papaya_player_set_volume(papaya_player *p, float volume)
{
    if (p->ao)
        papaya_audio_out_set_volume(p->ao, volume);
}

int64_t
papaya_player_position(papaya_player *p)
{
    int64_t t = papaya_clock_get(&p->clock, NULL);

    return t == PAPAYA_NOPTS ? 0 : t;
}

int64_t papaya_player_duration(papaya_player *p) { return papaya_demux_duration(p->demux); }
int papaya_player_dropped(papaya_player *p) { return atomic_load(&p->dropped); }

const papaya_stream_info *
papaya_player_video_stream(papaya_player *p)
{
    return p->video.index >= 0 ? papaya_demux_stream(p->demux, p->video.index) : NULL;
}

const papaya_stream_info *
papaya_player_audio_stream(papaya_player *p)
{
    return p->audio.index >= 0 ? papaya_demux_stream(p->demux, p->audio.index) : NULL;
}

int
papaya_player_ended(papaya_player *p)
{
    if (!atomic_load(&p->demux_eof))
        return 0;
    if (p->video.index >= 0 && (!atomic_load(&p->video.finished) || papaya_queue_count(p->frames) > 0))
        return 0;
    if (p->audio.index >= 0 && (!atomic_load(&p->audio.finished) || papaya_audio_out_queued(p->ao) > 0))
        return 0;
    return 1;
}

/* ---- video timing (UI thread) --------------------------------------------- */

/* The media time to show now: the clock once it belongs to the current
 * serial; before that, nothing is due yet except the first frame. */
static int64_t
show_time(papaya_player *p, int serial)
{
    int clock_serial;
    int64_t t = papaya_clock_get(&p->clock, &clock_serial);

    if (p->audio.index < 0 || clock_serial == serial || clock_serial == -1)
        return t;
    return PAPAYA_NOPTS;
}

papaya_frame *
papaya_player_video_frame(papaya_player *p)
{
    papaya_frame *shown = NULL;
    int serial;

    if (p->video.index < 0)
        return NULL;
    serial = papaya_queue_serial(p->video.packets);
    if (p->current_serial != serial)
        papaya_frame_unref(&p->current);
    for (;;) {
        void *item;
        int64_t now;
        frame_item *fi;

        if (papaya_queue_peek(p->frames, &item, NULL) != PAPAYA_QUEUE_OK)
            break;
        fi = item;
        if (fi->serial != serial) {
            papaya_queue_pop(p->frames, &item, NULL, 0);
            free_frame_item(item);
            continue;
        }
        now = show_time(p, serial);
        /* Without audio the first frame after open or seek starts the clock. */
        if (p->audio.index < 0 && (!p->current || now == PAPAYA_NOPTS)) {
            papaya_clock_set(&p->clock, fi->frame->pts, serial);
            if (atomic_load(&p->paused))
                papaya_clock_set_paused(&p->clock, 1);
            now = fi->frame->pts;
        }
        if (p->current && (now == PAPAYA_NOPTS || fi->frame->pts > now))
            break;
        papaya_queue_pop(p->frames, &item, NULL, 0);
        if (shown) {
            atomic_fetch_add(&p->dropped, 1);
            papaya_frame_unref(&shown);
        }
        shown = fi->frame;
        free(fi);
        if (!p->current)
            break; /* the first frame shows at once, even while paused */
        papaya_frame_unref(&p->current);
        p->current = papaya_frame_ref(shown);
    }
    if (!shown)
        return NULL;
    papaya_frame_unref(&p->current);
    p->current = papaya_frame_ref(shown);
    p->current_serial = serial;
    return shown;
}

int64_t
papaya_player_next_frame_in(papaya_player *p)
{
    void *item;
    int64_t now;

    if (p->video.index < 0 || papaya_queue_peek(p->frames, &item, NULL) != PAPAYA_QUEUE_OK)
        return -1;
    now = show_time(p, ((frame_item *)item)->serial);
    if (now == PAPAYA_NOPTS || atomic_load(&p->paused))
        return -1;
    return ((frame_item *)item)->frame->pts > now ? ((frame_item *)item)->frame->pts - now : 0;
}
