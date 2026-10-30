/* MPEG audio files (MP3, and MPEG-1/2/2.5 Layer I and II): ID3v2 tags are
 * skipped, a Xing / Info / VBRI frame gives the length and the seek table
 * and is not played, and the rest is split into frames by their headers. A
 * byte that only looks like a header is passed over: a frame counts only when
 * the next one follows where its size says. Free-format streams take their
 * frame size from the distance between the first two headers.
 *
 * Gapless: a LAME / Lavc tag in the Xing frame gives the encoder delay and
 * padding (plus the decoder's own 529 samples, as minimp3_ex counts them).
 * Packet times start at the first real sample, so the delay comes out
 * negative, and the stream's extradata carries the real sample count (8
 * bytes, little endian) for the decoder to drop the padding. */
#include <stdlib.h>
#include <string.h>

#include "demux_internal.h"

#define SCAN_LIMIT (1 << 20) /* how far to look for the next frame */

typedef struct {
    int version;  /* 3: MPEG-1, 2: MPEG-2, 0: MPEG-2.5 */
    int layer;    /* 1, 2 or 3 */
    int rate, bitrate, pad, channels, samples, size;
} mpa;

typedef struct {
    papaya_io *io;
    int64_t first, end, pos;  /* audio frames are in [first, end) */
    int64_t frame;            /* index of the frame at pos */
    int64_t duration;
    mpa ref;                  /* the first frame: what later ones must match */
    int free_size;            /* free format: frame size without padding */
    uint32_t xing_bytes;
    uint8_t toc[100];
    int has_toc;
    int delay, padding;       /* gapless: samples to drop at either end */
    uint8_t total[8];         /* extradata: the real sample count */
} mp3;

static const uint16_t bitrates[5][15] = {
    { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 }, /* MPEG-1 Layer I */
    { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 },    /* MPEG-1 Layer II */
    { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 },     /* MPEG-1 Layer III */
    { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 },    /* MPEG-2 Layer I */
    { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },         /* MPEG-2 Layer II, III */
};

/* A frame header, or -1. size is 0 for free format. */
static int
parse(const uint8_t *h, mpa *o)
{
    static const int rates[3] = { 44100, 48000, 32000 };
    int bits, br, sr, table;

    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0)
        return -1;
    o->version = (h[1] >> 3) & 3;
    bits = (h[1] >> 1) & 3;
    br = h[2] >> 4;
    sr = (h[2] >> 2) & 3;
    if (o->version == 1 || bits == 0 || br == 15 || sr == 3)
        return -1;
    o->layer = 4 - bits;
    o->rate = rates[sr] >> (o->version == 3 ? 0 : o->version == 2 ? 1 : 2);
    table = o->version == 3 ? o->layer - 1 : o->layer == 1 ? 3 : 4;
    o->bitrate = bitrates[table][br];
    o->pad = (h[2] >> 1) & 1;
    o->channels = (h[3] >> 6) == 3 ? 1 : 2;
    o->samples = o->layer == 1 ? 384 : o->layer == 2 || o->version == 3 ? 1152 : 576;
    if (!o->bitrate)
        o->size = 0;
    else if (o->layer == 1)
        o->size = (12 * o->bitrate * 1000 / o->rate + o->pad) * 4;
    else
        o->size = (o->layer == 3 && o->version != 3 ? 72 : 144) * o->bitrate * 1000 / o->rate + o->pad;
    return 0;
}

static int
same_stream(const mpa *a, const mpa *b)
{
    return a->version == b->version && a->layer == b->layer && a->rate == b->rate;
}

static int
frame_size(const mp3 *c, const mpa *h)
{
    return h->bitrate ? h->size : c->free_size ? c->free_size + h->pad * (h->layer == 1 ? 4 : 1) : 0;
}

/* The header at pos, if it is one and belongs to this stream. */
static int
header_at(mp3 *c, int64_t pos, mpa *h, const mpa *ref)
{
    uint8_t b[4];

    if (pos + 4 > c->end || papaya_io_read_at(c->io, pos, b, 4) != 0 || parse(b, h) != 0)
        return -1;
    return ref && !same_stream(h, ref) ? -1 : 0;
}

/* The first frame at or after pos whose successor is where its size says
 * (or the file ends there). ref NULL accepts any stream. */
static int
sync(mp3 *c, int64_t pos, int64_t *found, mpa *h, const mpa *ref)
{
    uint8_t buf[4096];
    int64_t limit = pos + SCAN_LIMIT;

    while (pos + 4 <= c->end && pos < limit) {
        size_t n = (size_t)(c->end - pos < (int64_t)sizeof buf ? c->end - pos : (int64_t)sizeof buf);

        if (papaya_io_read_at(c->io, pos, buf, n) != 0)
            return -1;
        for (size_t i = 0; i + 4 <= n; i++) {
            mpa next;
            int size;

            if (buf[i] != 0xFF || parse(buf + i, h) != 0 || (ref && !same_stream(h, ref)))
                continue;
            size = frame_size(c, h);
            if (size == 0) { /* free format, size unknown yet: the next header decides */
                *found = pos + (int64_t)i;
                return 0;
            }
            if (pos + (int64_t)i + size == c->end || header_at(c, pos + (int64_t)i + size, &next, h) == 0) {
                *found = pos + (int64_t)i;
                return 0;
            }
        }
        pos += (int64_t)n - 3;
    }
    return -1;
}

static int64_t
be32(const uint8_t *p)
{
    return (int64_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
}

/* A Xing / Info / VBRI frame: its counts, and 1 (it is not audio). */
static int
read_vbr_header(mp3 *c, int64_t pos, const mpa *h, int64_t *frames)
{
    uint8_t f[200];
    int side = h->version == 3 ? (h->channels == 1 ? 17 : 32) : (h->channels == 1 ? 9 : 17);
    int size = frame_size(c, h), crc;
    const uint8_t *x;

    if (size > (int)sizeof f) /* the tags sit in the first bytes */
        size = (int)sizeof f;
    if (size < 40 || papaya_io_read_at(c->io, pos, f, (size_t)size) != 0)
        return 0;
    crc = (f[1] & 1) ? 0 : 2; /* protection bit clear: a CRC follows the header */
    x = f + 4 + crc + side;
    if (h->layer == 3 && 4 + side + 8 <= size && (!memcmp(x, "Xing", 4) || !memcmp(x, "Info", 4))) {
        int64_t flags = be32(x + 4);
        const uint8_t *q = x + 8;

        if ((flags & 1) && q + 4 <= f + size) {
            *frames = be32(q);
            q += 4;
        }
        if ((flags & 2) && q + 4 <= f + size) {
            c->xing_bytes = (uint32_t)be32(q);
            q += 4;
        }
        if ((flags & 4) && q + 100 <= f + size) {
            memcpy(c->toc, q, 100);
            c->has_toc = 1;
            q += 100;
        }
        if (flags & 8)
            q += 4; /* quality */
        /* An encoder tag (LAME, Lavc, ...): delay and padding 21 bytes in. */
        if ((flags & 1) && q + 36 <= f + size && *q) {
            c->delay = ((q[21] << 4) | (q[22] >> 4)) + 529;
            c->padding = (((q[22] & 0xF) << 8) | q[23]) - 529;
            if (c->padding < 0)
                c->padding = 0;
        }
        return 1;
    }
    if (36 + 18 <= size && !memcmp(f + 36, "VBRI", 4)) {
        c->xing_bytes = (uint32_t)be32(f + 36 + 10);
        *frames = be32(f + 36 + 14);
        return 1;
    }
    return 0;
}

static int
mp3_probe(const uint8_t *head, size_t size)
{
    mpa h, next;

    if (size >= 10 && !memcmp(head, "ID3", 3))
        return 60; /* ID3 tags sit on other formats too, rarely */
    if (size < 4 || parse(head, &h) != 0)
        return 0;
    if (h.size == 0)
        return 20; /* free format: nothing to check it by */
    /* One header can be chance; two in a row are not. */
    if ((size_t)h.size + 4 <= size)
        return parse(head + h.size, &next) == 0 && same_stream(&h, &next) ? 80 : 0;
    return 20;
}

static void *
mp3_open(papaya_demuxer *d, papaya_io *io)
{
    papaya_stream_info *s;
    int64_t pos = 0, frames = 0;
    uint8_t b[10];
    mp3 *c;

    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->io = io;
    c->end = io->size(io->opaque);
    if (c->end < 0)
        c->end = INT64_MAX;
    /* ID3v2 tags, possibly several; then ID3v1 at the very end. */
    while (papaya_io_read_at(io, pos, b, 10) == 0 && !memcmp(b, "ID3", 3)) {
        int64_t len = (int64_t)(b[6] & 127) << 21 | (b[7] & 127) << 14 | (b[8] & 127) << 7 | (b[9] & 127);

        papaya_demux_id3_cover(d, io, pos);
        pos += 10 + len + ((b[5] & 0x10) ? 10 : 0);
    }
    if (c->end != INT64_MAX && c->end >= 128) {
        uint8_t t[3];

        if (papaya_io_read_at(io, c->end - 128, t, 3) == 0 && !memcmp(t, "TAG", 3))
            c->end -= 128;
    }
    if (sync(c, pos, &pos, &c->ref, NULL) != 0)
        goto fail;
    if (!c->ref.bitrate) { /* free format: the distance to the next header */
        int64_t next;
        mpa h;

        if (sync(c, pos + 4, &next, &h, &c->ref) != 0)
            goto fail;
        c->free_size = (int)(next - pos) - c->ref.pad * (c->ref.layer == 1 ? 4 : 1);
        if (c->free_size <= 4)
            goto fail;
    }
    c->first = pos;
    if (read_vbr_header(c, pos, &c->ref, &frames))
        c->first = pos + frame_size(c, &c->ref);
    c->pos = c->first;

    s = papaya_demux_add_stream(d);
    if (!s)
        goto fail;
    s->kind = PAPAYA_MEDIA_AUDIO;
    s->codec = PAPAYA_CODEC_MP3;
    s->codec_tag = c->ref.layer == 3 ? "mp3" : c->ref.layer == 2 ? "mp2" : "mp1";
    s->sample_rate = c->ref.rate;
    s->channels = c->ref.channels;
    if (frames > 0 && c->delay) {
        int64_t total = frames * c->ref.samples - c->delay - c->padding;

        if (total > 0) {
            for (int i = 0; i < 8; i++)
                c->total[i] = (uint8_t)(total >> (8 * i));
            s->extradata = c->total;
            s->extradata_size = sizeof c->total;
            c->duration = total * 1000000 / c->ref.rate;
        }
    }
    if (frames > 0 && !c->duration)
        c->duration = frames * c->ref.samples * 1000000 / c->ref.rate;
    if (!c->duration && c->ref.bitrate && c->end != INT64_MAX) /* constant bitrate */
        c->duration = (c->end - c->first) * 8000 / c->ref.bitrate;
    if (c->duration > 0) {
        s->duration = c->duration;
        papaya_demux_set_duration(d, c->duration);
    }
    return c;
fail:
    free(c);
    return NULL;
}

static int
mp3_read(void *ctx, papaya_packet **packet)
{
    mp3 *c = ctx;
    papaya_packet *p;
    mpa h;
    int size;

    if (header_at(c, c->pos, &h, &c->ref) != 0) { /* lost: find the next frame */
        if (sync(c, c->pos + 1, &c->pos, &h, &c->ref) != 0)
            return PAPAYA_DEMUX_EOF;
    }
    size = frame_size(c, &h);
    if (size < 4 || c->pos + size > c->end)
        return PAPAYA_DEMUX_EOF;
    p = papaya_packet_new(NULL, (size_t)size);
    if (!p)
        return PAPAYA_DEMUX_ERROR;
    if (papaya_io_read_at(c->io, c->pos, papaya_buffer_data(p->buf), (size_t)size) != 0) {
        papaya_packet_free(&p);
        return PAPAYA_DEMUX_EOF;
    }
    p->stream = 0;
    p->pts = p->dts = (c->frame * h.samples - c->delay) * 1000000 / h.rate;
    p->duration = (int64_t)h.samples * 1000000 / h.rate;
    p->flags |= PAPAYA_PACKET_KEY;
    c->pos += size;
    c->frame++;
    *packet = p;
    return 0;
}

static int
mp3_seek(void *ctx, int64_t pts)
{
    mp3 *c = ctx;
    int64_t target, found;
    mpa h;

    if (pts <= 0 || c->duration <= 0) {
        c->pos = c->first;
        c->frame = 0;
        return 0;
    }
    if (pts > c->duration)
        pts = c->duration;
    if (c->has_toc && c->xing_bytes) { /* the Xing table: byte positions per percent */
        double pct = (double)pts * 100.0 / (double)c->duration;
        int i = pct >= 99.0 ? 99 : (int)pct;
        double a = c->toc[i], b = i < 99 ? c->toc[i + 1] : 256.0;

        target = c->first + (int64_t)((a + (b - a) * (pct - i)) / 256.0 * c->xing_bytes);
    } else { /* constant bitrate, or an even spread */
        target = c->first + (int64_t)((double)(c->end - c->first) * (double)pts / (double)c->duration);
    }
    if (sync(c, target, &found, &h, &c->ref) != 0)
        return PAPAYA_DEMUX_ERROR;
    c->pos = found;
    c->frame = (pts * c->ref.rate / 1000000 + c->delay) / c->ref.samples; /* the frame it should be */
    return 0;
}

static void
mp3_close(void *ctx)
{
    free(ctx);
}

const papaya_demux_backend papaya_demux_mp3 = {
    "mp3", mp3_probe, mp3_open, mp3_read, mp3_seek, mp3_close,
};
