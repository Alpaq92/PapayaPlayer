/* Ogg (libogg, BSD): Vorbis, Opus and FLAC streams, one or several
 * multiplexed; other logical streams (Theora, Skeleton) are passed over.
 * Each codec's headers become its extradata the way Matroska carries them:
 * Vorbis's three Xiph-laced, Opus's OpusHead, FLAC's "fLaC" + STREAMINFO.
 *
 * Times come from granule positions, which a page carries for its end: the
 * packets that complete on a page start at the previous page's granule. The
 * first packet after the start or a seek gets that time and the decoders
 * count on from there, as they do for Matroska. The length is the last
 * page's granule; a seek bisects the file on the first stream's page
 * granules. Chained files play their first link. */
#include <stdlib.h>
#include <string.h>

#include "demux_internal.h"
#include "ogg/ogg.h"

#define MAX_STREAMS 8
#define CHUNK 8192

typedef struct {
    int serial;
    int stream;            /* ours, -1 for a stream we pass over */
    papaya_codec codec;
    ogg_stream_state os;
    int headers;           /* header packets at the start of the stream */
    int skip;              /* header packets still to drop when reading */
    int rate, channels;
    int64_t preskip;       /* Opus: samples the granule counts before time 0 */
    uint8_t *extra;        /* the extradata, built from the headers */
    size_t extra_size;
    size_t header_len[3];  /* Vorbis: each header's size, for the lacing */
    int64_t granule;       /* the last page's granule, -1 if not known */
    int64_t start;         /* the granule the current page's packets start at */
    int want_pts;          /* the next packet gets a time from start */
} lstream;

typedef struct {
    papaya_io *io;
    ogg_sync_state oy;
    int64_t pos;           /* where the sync layer reads next */
    int64_t end;
    lstream s[MAX_STREAMS];
    int count;
} ogg;

static int64_t
granule_time(const lstream *s, int64_t g)
{
    g -= s->preskip;
    return g > 0 ? g * 1000000 / s->rate : 0;
}

/* The next page from the sync layer, reading more as needed; 0 at the end. */
static int
next_page(ogg *c, ogg_page *og)
{
    for (;;) {
        int r = ogg_sync_pageout(&c->oy, og);
        char *buf;
        size_t n;

        if (r == 1)
            return 1;
        if (r < 0)
            continue; /* skipped junk */
        if (c->pos >= c->end)
            return 0;
        n = (size_t)(c->end - c->pos < CHUNK ? c->end - c->pos : CHUNK);
        buf = ogg_sync_buffer(&c->oy, (long)n);
        if (!buf || papaya_io_read_at(c->io, c->pos, (uint8_t *)buf, n) != 0)
            return 0;
        ogg_sync_wrote(&c->oy, (long)n);
        c->pos += (int64_t)n;
    }
}

static lstream *
find(ogg *c, int serial)
{
    for (int i = 0; i < c->count; i++)
        if (c->s[i].serial == serial)
            return &c->s[i];
    return NULL;
}

static int
append(lstream *s, const void *data, size_t n)
{
    uint8_t *grown = realloc(s->extra, s->extra_size + n);

    if (!grown)
        return -1;
    memcpy(grown + s->extra_size, data, n);
    s->extra = grown;
    s->extra_size += n;
    return 0;
}

/* The first packet names the codec and its header count. */
static void
identify(lstream *s, const ogg_packet *op)
{
    const uint8_t *p = op->packet;
    long n = op->bytes;

    if (n >= 30 && !memcmp(p, "\x01vorbis", 7)) {
        s->codec = PAPAYA_CODEC_VORBIS;
        s->channels = p[11];
        s->rate = (int)((uint32_t)p[12] | (uint32_t)p[13] << 8 | (uint32_t)p[14] << 16 | (uint32_t)p[15] << 24);
        s->headers = 3;
    } else if (n >= 19 && !memcmp(p, "OpusHead", 8)) {
        s->codec = PAPAYA_CODEC_OPUS;
        s->channels = p[9];
        s->rate = 48000;
        s->preskip = p[10] | p[11] << 8;
        s->headers = 2; /* OpusHead, OpusTags */
        append(s, p, (size_t)n);
    } else if (n >= 13 + 34 && !memcmp(p, "\x7F" "FLAC", 5) && !memcmp(p + 9, "fLaC", 4)) {
        const uint8_t *si = p + 17;

        s->codec = PAPAYA_CODEC_FLAC;
        s->rate = si[10] << 12 | si[11] << 4 | si[12] >> 4;
        s->channels = ((si[12] >> 1) & 7) + 1;
        s->headers = 1 + (p[7] << 8 | p[8]); /* this one and the metadata after it */
        if (append(s, p + 9, 4 + 4 + 34) == 0)
            s->extra[4] |= 0x80; /* STREAMINFO is the last block the decoder sees */
    }
}

/* Vorbis: the three headers (back to back in extra), Xiph-laced. */
static int
lace_vorbis(lstream *s)
{
    size_t n = 1 + s->extra_size + s->header_len[0] / 255 + 1 + s->header_len[1] / 255 + 1, at = 0;
    uint8_t *out = malloc(n);

    if (!out)
        return -1;
    out[at++] = 2;
    for (int i = 0; i < 2; i++) {
        size_t len = s->header_len[i];

        for (; len >= 255; len -= 255)
            out[at++] = 255;
        out[at++] = (uint8_t)len;
    }
    memcpy(out + at, s->extra, s->extra_size);
    free(s->extra);
    s->extra = out;
    s->extra_size += at;
    return 0;
}

/* The page at or after pos (before limit): its start, serial and granule. */
static int
page_at(ogg *c, int64_t pos, int64_t limit, int64_t *start, int *serial, int64_t *granule)
{
    ogg_sync_state oy;
    ogg_page og;
    int64_t at = pos, read = pos;
    int found = 0;

    ogg_sync_init(&oy);
    while (!found && read < limit) {
        size_t n = (size_t)(limit - read < CHUNK ? limit - read : CHUNK);
        char *buf = ogg_sync_buffer(&oy, (long)n);
        long r;

        if (!buf || papaya_io_read_at(c->io, read, (uint8_t *)buf, n) != 0)
            break;
        ogg_sync_wrote(&oy, (long)n);
        read += (int64_t)n;
        while (!found && (r = ogg_sync_pageseek(&oy, &og)) != 0) {
            if (r < 0) {
                at -= r; /* skipped bytes */
                continue;
            }
            *start = at;
            *serial = ogg_page_serialno(&og);
            *granule = ogg_page_granulepos(&og);
            found = 1;
        }
    }
    ogg_sync_clear(&oy);
    return found;
}

/* Back to the top: every stream drops its headers again. */
static void
restart(ogg *c, int64_t pos)
{
    ogg_sync_reset(&c->oy);
    c->pos = pos;
    for (int i = 0; i < c->count; i++) {
        lstream *s = &c->s[i];

        ogg_stream_reset(&s->os);
        s->skip = pos == 0 ? s->headers : 0;
        s->granule = pos == 0 ? 0 : -1;
        s->start = -1;
        s->want_pts = 1;
    }
}

static void
ogg_close(void *ctx)
{
    ogg *c = ctx;

    for (int i = 0; i < c->count; i++) {
        ogg_stream_clear(&c->s[i].os);
        free(c->s[i].extra);
    }
    ogg_sync_clear(&c->oy);
    free(c);
}

/* Each stream's length: its last granule, from the end of the file. */
static void
read_lengths(ogg *c, papaya_demuxer *d)
{
    int64_t from = c->end > 65536 ? c->end - 65536 : 0, longest = 0, start, granule;
    int serial;

    while (page_at(c, from, c->end, &start, &serial, &granule)) {
        lstream *s = find(c, serial);

        if (s && s->stream >= 0 && granule >= 0) {
            papaya_stream_info *info = (papaya_stream_info *)papaya_demux_stream(d, s->stream);

            info->duration = granule_time(s, granule);
            if (info->duration > longest)
                longest = info->duration;
        }
        from = start + 1;
    }
    if (longest > 0)
        papaya_demux_set_duration(d, longest);
}

static int
ogg_probe(const uint8_t *head, size_t size)
{
    return size >= 4 && !memcmp(head, "OggS", 4) ? 100 : 0;
}

static void *
ogg_open(papaya_demuxer *d, papaya_io *io)
{
    ogg_page og;
    ogg_packet op;
    ogg *c = calloc(1, sizeof *c);
    int pending = 1;

    if (!c)
        return NULL;
    c->io = io;
    c->end = io->size(io->opaque);
    if (c->end < 0)
        c->end = INT64_MAX;
    ogg_sync_init(&c->oy);

    /* Beginning-of-stream pages name the streams; then their headers. */
    while (pending && next_page(c, &og)) {
        lstream *s = find(c, ogg_page_serialno(&og));

        if (!s) {
            if (!ogg_page_bos(&og) || c->count == MAX_STREAMS)
                continue;
            s = &c->s[c->count++];
            memset(s, 0, sizeof *s);
            s->serial = ogg_page_serialno(&og);
            s->stream = -1;
            ogg_stream_init(&s->os, s->serial);
        }
        ogg_stream_pagein(&s->os, &og);
        while (s->skip < s->headers || (s->headers == 0 && s->codec == PAPAYA_CODEC_UNKNOWN && !s->skip)) {
            if (ogg_stream_packetout(&s->os, &op) != 1)
                break;
            if (s->headers == 0) { /* the first packet */
                identify(s, &op);
                s->skip = 1;
                if (s->codec == PAPAYA_CODEC_UNKNOWN)
                    break;
                if (s->codec != PAPAYA_CODEC_VORBIS)
                    continue;
            } else {
                s->skip++;
            }
            if (s->codec == PAPAYA_CODEC_VORBIS) {
                s->header_len[s->skip - 1] = (size_t)op.bytes;
                if (append(s, op.packet, (size_t)op.bytes) != 0)
                    goto fail;
            }
        }
        pending = 0;
        for (int i = 0; i < c->count; i++)
            if (c->s[i].codec != PAPAYA_CODEC_UNKNOWN && c->s[i].skip < c->s[i].headers)
                pending = 1;
    }
    if (pending)
        goto fail;

    for (int i = 0; i < c->count; i++) {
        lstream *s = &c->s[i];
        papaya_stream_info *info;

        if (s->codec == PAPAYA_CODEC_UNKNOWN || s->rate <= 0 || s->channels < 1)
            continue;
        if (s->codec == PAPAYA_CODEC_VORBIS && lace_vorbis(s) != 0)
            continue;
        info = papaya_demux_add_stream(d);
        if (!info)
            break;
        info->kind = PAPAYA_MEDIA_AUDIO;
        info->codec = s->codec;
        info->codec_tag = "Ogg";
        info->sample_rate = s->rate;
        info->channels = s->channels;
        info->extradata = s->extra;
        info->extradata_size = s->extra_size;
        if (s->codec == PAPAYA_CODEC_OPUS) {
            info->codec_delay = s->preskip * 1000000 / 48000;
            info->seek_preroll = 80000;
        }
        s->stream = info->index;
    }
    if (papaya_demux_stream_count(d) == 0)
        goto fail;
    read_lengths(c, d);
    restart(c, 0);
    return c;
fail:
    ogg_close(c);
    return NULL;
}

static int
ogg_read(void *ctx, papaya_packet **packet)
{
    ogg *c = ctx;
    ogg_packet op;
    ogg_page og;

    for (;;) {
        for (int i = 0; i < c->count; i++) {
            lstream *s = &c->s[i];
            papaya_packet *p;

            if (s->stream < 0)
                continue;
            while (ogg_stream_packetout(&s->os, &op) == 1) {
                if (s->skip > 0) { /* a header */
                    s->skip--;
                    continue;
                }
                /* FLAC may leave its header count 0 (unknown): metadata
                 * packets are the ones that are not frames. */
                if (s->codec == PAPAYA_CODEC_FLAC && (op.bytes < 2 || op.packet[0] != 0xFF))
                    continue;
                p = papaya_packet_new(op.packet, (size_t)op.bytes);
                if (!p)
                    return PAPAYA_DEMUX_ERROR;
                p->stream = s->stream;
                p->flags |= PAPAYA_PACKET_KEY;
                p->pts = p->dts = PAPAYA_NOPTS;
                if (s->want_pts && s->start >= 0) {
                    p->pts = p->dts = granule_time(s, s->start);
                    s->want_pts = 0;
                }
                *packet = p;
                return 0;
            }
        }
        if (!next_page(c, &og))
            return PAPAYA_DEMUX_EOF;
        {
            lstream *s = find(c, ogg_page_serialno(&og));

            if (!s)
                continue;
            if (ogg_page_bos(&og) && c->pos > 0 && s->skip == 0 && s->granule > 0)
                return PAPAYA_DEMUX_EOF; /* a chained file's next link */
            ogg_stream_pagein(&s->os, &og);
            s->start = s->granule;
            if (ogg_page_granulepos(&og) >= 0)
                s->granule = ogg_page_granulepos(&og);
        }
    }
}

static int
ogg_seek(void *ctx, int64_t pts)
{
    ogg *c = ctx;
    lstream *ref = NULL;
    int64_t lo = 0, hi = c->end == INT64_MAX ? 0 : c->end, best = 0, best_granule = 0;

    for (int i = 0; i < c->count && !ref; i++)
        if (c->s[i].stream >= 0)
            ref = &c->s[i];
    if (!ref || pts <= 0) {
        restart(c, 0);
        return 0;
    }
    /* The last page of ref whose granule is at or before the target; reading
     * goes on from the page after it, whose packets start at that granule. */
    while (hi - lo > CHUNK) {
        int64_t mid = lo + (hi - lo) / 2, start, granule;
        int serial;

        int before = page_at(c, mid, hi, &start, &serial, &granule)
                     && (serial != ref->serial || granule < 0 || granule_time(ref, granule) <= pts);

        if (!before) { /* no page in the upper half, or it is past the target */
            hi = mid;
            continue;
        }
        if (serial == ref->serial && granule >= 0) {
            best = start;
            best_granule = granule;
        }
        lo = start + 1;
    }
    /* The last stretch, page by page: pages before lo end at or before the
     * target, and pages are coarse enough to straddle where bisection
     * stopped. */
    for (int64_t pos = lo, start, granule; ; pos = start + 1) {
        int serial;

        if (!page_at(c, pos, c->end, &start, &serial, &granule))
            break;
        if (serial != ref->serial || granule < 0)
            continue;
        if (granule_time(ref, granule) > pts)
            break;
        best = start;
        best_granule = granule;
    }
    if (best == 0 || granule_time(ref, best_granule) == 0) {
        restart(c, 0);
        return 0;
    }
    restart(c, best);
    {
        ogg_page og;

        /* The page at best itself: its packets end before the target. */
        if (next_page(c, &og) && ogg_page_serialno(&og) == ref->serial)
            ref->granule = best_granule;
    }
    return 0;
}

const papaya_demux_backend papaya_demux_ogg = {
    "ogg", ogg_probe, ogg_open, ogg_read, ogg_seek, ogg_close,
};
