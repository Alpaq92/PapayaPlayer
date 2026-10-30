/* Native FLAC files: "fLaC", metadata blocks, then frames. STREAMINFO (sent
 * to the decoder as "fLaC" + that one block) gives the rate and length.
 * Frames are found by their headers, which count only when their CRC-8
 * holds; a frame ends where the next one starts, which must be one block
 * later (so a sync pattern inside audio data, CRC and all, is passed over). Every header says its
 * first sample, so times are exact, and a seek bisects the file on frame
 * headers (no seek table needed). A leading ID3v2 tag is skipped. */
#include <stdlib.h>
#include <string.h>

#include "demux_internal.h"

#define WINDOW 65536

typedef struct {
    papaya_io *io;
    int64_t first, end, pos;      /* frames are in [first, end) */
    int rate, min_block, max_block, bits, channels;
    int64_t total;                /* samples, 0 if unknown */
    uint8_t info[4 + 4 + 34];     /* extradata: "fLaC" + STREAMINFO */
    uint8_t *buf;                 /* a window of the file at buf_pos */
    int64_t buf_pos;
    size_t buf_len, buf_cap;
} flac;

static uint8_t
crc8(const uint8_t *p, size_t n)
{
    uint8_t c = 0;

    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++)
            c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

/* A frame header at p (n bytes there): its first sample, or -1; *block is
 * its sample count. */
static int64_t
frame_header(const flac *c, const uint8_t *p, size_t n, int *block)
{
    int bs, sr, ch, ss, len = 4, extra;
    uint64_t num;
    int ones = 0;

    if (n < 6 || p[0] != 0xFF || (p[1] & 0xFE) != 0xF8)
        return -1;
    bs = p[2] >> 4;
    sr = p[2] & 15;
    ch = p[3] >> 4;
    ss = (p[3] >> 1) & 7;
    if (bs == 0 || sr == 15 || ch > 10 || ss == 3 || (p[3] & 1))
        return -1;
    /* The frame or sample number, UTF-8 style: leading ones say its length. */
    while (ones < 8 && (p[4] & (0x80 >> ones)))
        ones++;
    if (ones == 1 || ones > 7)
        return -1;
    num = ones ? (uint64_t)(p[4] & (0x7F >> ones)) : p[4];
    for (size_t i = 1; i < (size_t)(ones ? ones : 1); i++) {
        if (4 + i >= n || (p[4 + i] & 0xC0) != 0x80)
            return -1;
        num = num << 6 | (p[4 + i] & 0x3F);
    }
    len += ones ? ones : 1;
    extra = (bs == 6 ? 1 : bs == 7 ? 2 : 0) + (sr == 12 ? 1 : sr == 13 || sr == 14 ? 2 : 0);
    {
        size_t crc_at = (size_t)len + (size_t)extra;

        if (crc_at + 1 > n || crc8(p, crc_at) != p[crc_at])
            return -1;
    }
    *block = bs == 1 ? 192 : bs <= 5 ? 576 << (bs - 2) : bs == 6 ? p[len] + 1
             : bs == 7 ? (p[len] << 8 | p[len + 1]) + 1 : 256 << (bs - 8);
    /* Fixed block size (bit clear): the number counts frames. */
    return (p[1] & 1) ? (int64_t)num : (int64_t)num * c->min_block;
}

/* Fills the window to cover [pos, pos + want), as far as the file goes. */
static int
window(flac *c, int64_t pos, size_t want)
{
    size_t n;

    if (pos >= c->buf_pos && pos + (int64_t)want <= c->buf_pos + (int64_t)c->buf_len)
        return 0;
    if (want > c->buf_cap) {
        uint8_t *grown = realloc(c->buf, want);

        if (!grown)
            return -1;
        c->buf = grown;
        c->buf_cap = want;
    }
    n = (size_t)(c->end - pos < (int64_t)want ? c->end - pos : (int64_t)want);
    c->buf_pos = pos;
    c->buf_len = 0;
    if (n && papaya_io_read_at(c->io, pos, c->buf, n) != 0)
        return -1;
    c->buf_len = n;
    return 0;
}

/* The next frame header at or after pos (starting at sample expect, unless
 * that is -1): its offset, first sample and block size. */
static int
next_frame(flac *c, int64_t pos, int64_t expect, int64_t *at, int64_t *sample, int *block)
{
    while (pos + 6 <= c->end) {
        if (window(c, pos, WINDOW) != 0)
            return -1;
        for (size_t i = 0; i + 6 <= c->buf_len; i++) {
            const uint8_t *p = c->buf + i;

            if (p[0] != 0xFF)
                continue;
            *sample = frame_header(c, p, c->buf_len - i, block);
            if (*sample >= 0 && (expect < 0 || *sample == expect)) {
                *at = pos + (int64_t)i;
                return 0;
            }
        }
        if (c->buf_len < 6)
            break;
        pos += (int64_t)c->buf_len - 5;
    }
    return -1;
}

static int
flac_probe(const uint8_t *head, size_t size)
{
    if (size >= 4 && !memcmp(head, "fLaC", 4))
        return 100;
    return size >= 10 && !memcmp(head, "ID3", 3) ? 40 : 0;
}

static void *
flac_open(papaya_demuxer *d, papaya_io *io)
{
    int64_t pos = 0, first_sample;
    int block;
    uint8_t b[10];
    papaya_stream_info *s;
    flac *c;

    while (papaya_io_read_at(io, pos, b, 10) == 0 && !memcmp(b, "ID3", 3)) {
        papaya_demux_id3_cover(d, io, pos);
        pos += 10 + ((int64_t)(b[6] & 127) << 21 | (b[7] & 127) << 14 | (b[8] & 127) << 7 | (b[9] & 127))
               + ((b[5] & 0x10) ? 10 : 0);
    }
    if (papaya_io_read_at(io, pos, b, 4) != 0 || memcmp(b, "fLaC", 4) != 0)
        return NULL;
    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->io = io;
    c->end = io->size(io->opaque);
    if (c->end < 0)
        c->end = INT64_MAX;
    pos += 4;
    /* Metadata blocks: STREAMINFO first, then any PICTURE; the rest skipped. */
    for (int last = 0, first = 1; !last; first = 0) {
        uint8_t h[4];
        int64_t len;

        if (papaya_io_read_at(io, pos, h, 4) != 0)
            goto fail;
        last = h[0] >> 7;
        len = (int64_t)h[1] << 16 | h[2] << 8 | h[3];
        if (first) {
            const uint8_t *si = c->info + 8;

            if ((h[0] & 127) != 0 || len != 34 || papaya_io_read_at(io, pos + 4, c->info + 8, 34) != 0)
                goto fail;
            memcpy(c->info, "fLaC", 4);
            c->info[4] = 0x80; /* STREAMINFO, the last block the decoder sees */
            c->info[5] = 0;
            c->info[6] = 0;
            c->info[7] = 34;
            c->min_block = si[0] << 8 | si[1];
            c->max_block = si[2] << 8 | si[3];
            c->rate = si[10] << 12 | si[11] << 4 | si[12] >> 4;
            c->channels = ((si[12] >> 1) & 7) + 1;
            c->bits = ((si[12] & 1) << 4 | si[13] >> 4) + 1;
            c->total = (int64_t)(si[13] & 15) << 32 | (int64_t)si[14] << 24 | si[15] << 16 | si[16] << 8 | si[17];
        }
        else if ((h[0] & 127) == 6 && len > 0 && len <= (16 << 20)) {
            uint8_t *pic = malloc((size_t)len);

            if (pic && papaya_io_read_at(io, pos + 4, pic, (size_t)len) == 0)
                papaya_demux_flac_picture(d, pic, (size_t)len);
            free(pic);
        }
        pos += 4 + len;
    }
    if (c->rate <= 0 || c->min_block < 16)
        goto fail;
    if (next_frame(c, pos, -1, &c->first, &first_sample, &block) != 0)
        goto fail;
    c->pos = c->first;

    s = papaya_demux_add_stream(d);
    if (!s)
        goto fail;
    s->kind = PAPAYA_MEDIA_AUDIO;
    s->codec = PAPAYA_CODEC_FLAC;
    s->codec_tag = "fLaC";
    s->sample_rate = c->rate;
    s->channels = c->channels;
    s->bits_per_sample = c->bits;
    s->extradata = c->info;
    s->extradata_size = sizeof c->info;
    if (c->total > 0) {
        s->duration = c->total * 1000000 / c->rate;
        papaya_demux_set_duration(d, s->duration);
    }
    return c;
fail:
    free(c->buf);
    free(c);
    return NULL;
}

static int
flac_read(void *ctx, papaya_packet **packet)
{
    flac *c = ctx;
    int64_t sample, next, next_sample, size;
    int block, next_block;
    papaya_packet *p;

    if (next_frame(c, c->pos, -1, &c->pos, &sample, &block) != 0)
        return PAPAYA_DEMUX_EOF;
    /* The frame ends at the next header, one block on, or the end of the file. */
    if (next_frame(c, c->pos + 6, sample + block, &next, &next_sample, &next_block) != 0)
        next = c->end == INT64_MAX ? c->buf_pos + (int64_t)c->buf_len : c->end;
    size = next - c->pos;
    if (size <= 0 || size > (16 << 20))
        return PAPAYA_DEMUX_EOF;
    p = papaya_packet_new(NULL, (size_t)size);
    if (!p)
        return PAPAYA_DEMUX_ERROR;
    if (papaya_io_read_at(c->io, c->pos, papaya_buffer_data(p->buf), (size_t)size) != 0) {
        papaya_packet_free(&p);
        return PAPAYA_DEMUX_EOF;
    }
    p->stream = 0;
    p->pts = p->dts = sample * 1000000 / c->rate;
    p->flags |= PAPAYA_PACKET_KEY;
    c->pos = next;
    *packet = p;
    return 0;
}

/* Bisects on frame headers for the last frame starting at or before the
 * target sample. */
static int
flac_seek(void *ctx, int64_t pts)
{
    flac *c = ctx;
    int64_t target = pts > 0 ? pts * c->rate / 1000000 : 0;
    int64_t lo = c->first, hi = c->end == INT64_MAX ? c->first : c->end, best = c->first;
    int block;

    while (hi - lo > WINDOW / 4) {
        int64_t mid = lo + (hi - lo) / 2, at, sample;
        int block;

        if (next_frame(c, mid, -1, &at, &sample, &block) != 0 || sample > target) {
            hi = mid;
        } else {
            best = at;
            lo = at + 1;
        }
    }
    /* Walk the last stretch frame by frame. */
    for (int64_t at = best, sample; next_frame(c, at, -1, &at, &sample, &block) == 0 && sample <= target; at++)
        best = at;
    c->pos = best;
    return 0;
}

static void
flac_close(void *ctx)
{
    flac *c = ctx;

    free(c->buf);
    free(c);
}

const papaya_demux_backend papaya_demux_flac = {
    "flac", flac_probe, flac_open, flac_read, flac_seek, flac_close,
};
