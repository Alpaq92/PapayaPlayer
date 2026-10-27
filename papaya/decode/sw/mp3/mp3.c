/* MP3 (and MPEG-1/2 Layer I and II) through minimp3 (CC0). Each packet is
 * one frame; minimp3 keeps the bit reservoir between frames, so a flush
 * starts it over. Gapless: samples before time 0 are dropped, and so are
 * those past the real sample count when the demuxer gives one (extradata,
 * 8 bytes little endian, from a LAME tag). */
#include <stdlib.h>
#include <string.h>

#include "decode_internal.h"
#include "minimp3.h"

typedef struct {
    mp3dec_t dec;
    papaya_frame_pool *pool;
    int64_t next_pts;
    int64_t total;   /* real samples, or -1 */
} mp3_ctx;

static int
mp3_supports(const papaya_stream_info *s)
{
    return s->codec == PAPAYA_CODEC_MP3;
}

static void *
mp3_open(const papaya_stream_info *s, papaya_frame_pool *pool)
{
    mp3_ctx *c = calloc(1, sizeof *c);

    if (!c)
        return NULL;
    c->total = -1;
    if (s->extradata && s->extradata_size == 8) {
        c->total = 0;
        for (int i = 7; i >= 0; i--)
            c->total = c->total << 8 | s->extradata[i];
    }
    mp3dec_init(&c->dec);
    c->pool = pool;
    c->next_pts = PAPAYA_NOPTS;
    return c;
}

/* Free format (bitrate index 0): minimp3 learns the frame size by finding
 * the next header, which a one-frame packet does not hold. The demuxer split
 * the frames already, so the size is the packet's; give minimp3 the header
 * and that size (both public fields of mp3dec_t) before its first frame. */
static void
seed_free_format(mp3_ctx *c, const papaya_packet *p)
{
    const uint8_t *h = p->data;
    int pad;

    if (c->dec.header[0] == 0xFF || p->size < 8 || h[0] != 0xFF || (h[1] & 0xE0) != 0xE0 || (h[2] >> 4) != 0)
        return;
    pad = (h[2] >> 1) & 1;
    if (((h[1] >> 1) & 3) == 3) /* Layer I pads by a 4-byte slot */
        pad *= 4;
    memcpy(c->dec.header, h, 4);
    c->dec.free_format_bytes = (int)p->size - pad;
}

static int
mp3_decode_packet(void *ctx, const papaya_packet *p, void (*emit)(void *, papaya_frame *), void *sink)
{
    mp3_ctx *c = ctx;
    float pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_frame_info_t info;
    papaya_frame *f;
    int n;

    if (!p)
        return 0;
    memset(&info, 0, sizeof info);
    seed_free_format(c, p);
    n = mp3dec_decode_frame(&c->dec, p->data, (int)p->size, pcm, &info);
    if (p->pts != PAPAYA_NOPTS)
        c->next_pts = p->pts;
    if (n <= 0 || info.channels < 1 || info.channels > 2 || info.hz <= 0)
        return 0; /* the reservoir is still filling, or not audio (a Xing frame) */
    {
        /* The frame's first sample, exact: times are whole samples apart. */
        int64_t start = c->next_pts == PAPAYA_NOPTS ? 0
                        : (c->next_pts * info.hz + (c->next_pts < 0 ? -500000 : 500000)) / 1000000;
        int skip = start < 0 ? (int)(-start < n ? -start : n) : 0, keep = n - skip;

        if (c->next_pts != PAPAYA_NOPTS)
            c->next_pts += (int64_t)n * 1000000 / info.hz;
        if (c->total >= 0 && start + n > c->total)
            keep = (int)(c->total - start > skip ? c->total - start - skip : 0);
        if (keep <= 0)
            return 0;
        f = papaya_frame_get_audio(c->pool, PAPAYA_SAMPLE_F32, info.channels, info.hz, keep);
        if (!f)
            return -1;
        memcpy(f->audio[0], pcm + (size_t)skip * (size_t)info.channels,
               sizeof(float) * (size_t)keep * (size_t)info.channels);
        f->pts = (start + skip) * 1000000 / info.hz;
        f->duration = (int64_t)keep * 1000000 / info.hz;
        emit(sink, f);
    }
    return 0;
}

static void
mp3_flush(void *ctx)
{
    mp3_ctx *c = ctx;

    memset(&c->dec, 0, sizeof c->dec);
    c->next_pts = PAPAYA_NOPTS;
}

static void
mp3_close(void *ctx)
{
    free(ctx);
}

const papaya_decode_backend papaya_decode_mp3 = {
    "minimp3", mp3_supports, mp3_open, mp3_decode_packet, mp3_flush, mp3_close,
};
