/* MPEG transport streams: .ts (188-byte packets) and Blu-ray / AVCHD .m2ts
 * (192: a 4-byte timecode first). Every program's PMT lists its elementary
 * streams; they become streams in program order (titled "Program N" when
 * there are several), so a player taking the first video and audio plays
 * the first program. Each stream's PES packets are reassembled from its PID
 * and leave as packets when the next one starts. H.264 and HEVC go out as
 * Annex B, a keyframe where an IDR / IRAP picture is; AAC arrives as ADTS
 * and leaves frame by frame with an AudioSpecificConfig from the first
 * header, as the AAC decoder wants it. AAC in LATM (LOAS frames, DVB) leaves
 * frame by frame with its headers and no config: the decoder reads them. Its
 * format comes from the first StreamMuxConfig (one program and layer).
 *
 * Times are the 90 kHz PTS / DTS, unwrapped (33 bits) and counted from the
 * first one in the file. The length comes from the last PTS near the end. A
 * seek bisects the file on PTS a little before the target, then drops
 * packets until a video keyframe. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "demux_internal.h"

#define MAX_ES 16
#define MAX_PROGRAMS 16
#define PROBE_BYTES (8 << 20)   /* how far open looks for every stream's first PES */
#define TAIL_BYTES (2 << 20)    /* how far from the end the length is looked for */
#define WRAP ((int64_t)1 << 33)
#define QUEUE 64
#define SEEK_BACK 135000        /* 1.5 s at 90 kHz: keyframes come at least this often */

typedef struct {
    int pid;
    int stream;                 /* ours, -1 if passed over */
    papaya_codec codec;
    papaya_media_kind kind;
    uint8_t *pes;
    size_t len, cap;
    int open;                   /* a PES is being collected */
    /* AAC: from the first ADTS header or StreamMuxConfig */
    int rate_index, profile, channels, rate;
    int latm, out_rate;         /* LATM: frames keep their headers; rate after SBR */
    uint8_t asc[2];
    int program;                /* its program number */
    int ready;                  /* everything open needed from this stream */
} es;

typedef struct {
    papaya_io *io;
    int psize, skip;            /* packet size, bytes before the sync byte */
    int64_t first, pos, end;
    int pmt_pid[MAX_PROGRAMS], program[MAX_PROGRAMS], pmt_done[MAX_PROGRAMS];
    int programs;
    es es[MAX_ES];
    int count;
    int64_t start90;            /* the first time in the file, -1 before */
    int64_t last90;             /* for unwrapping */
    papaya_packet *queue[QUEUE];
    int qhead, qlen;
    int wait_key;               /* after a seek: drop until a video keyframe */
    int ref;                    /* es index the length and seeks go by */
} ts;

/* ---- packets ---------------------------------------------------------------- */

static int
ts_probe(const uint8_t *head, size_t size)
{
    static const int sizes[2][2] = { { 188, 0 }, { 192, 4 } };

    for (int k = 0; k < 2; k++) {
        int ok = 1, n = 0;

        for (size_t at = (size_t)sizes[k][1]; at < size && n < 6; at += (size_t)sizes[k][0], n++)
            if (head[at] != 0x47)
                ok = 0;
        if (ok && n >= 4)
            return 90;
    }
    return 0;
}

/* The next 188-byte packet into p, or 0 at the end. Loses sync gracefully. */
static int
next_packet(ts *c, uint8_t p[188])
{
    while (c->pos + c->psize <= c->end) {
        if (papaya_io_read_at(c->io, c->pos + c->skip, p, 188) != 0)
            return 0;
        c->pos += c->psize;
        if (p[0] == 0x47)
            return 1;
        /* Lost: look for the next sync byte that repeats a packet later. */
        {
            uint8_t b[4096];
            int64_t from = c->pos - c->psize + 1;
            size_t n = (size_t)(c->end - from < (int64_t)sizeof b ? c->end - from : (int64_t)sizeof b);

            if (n < 2 || papaya_io_read_at(c->io, from, b, n) != 0)
                return 0;
            c->pos = from + (int64_t)n; /* nothing found: skip the window */
            for (size_t i = 0, next = (size_t)c->psize; i < n; i++, next++) {
                if (b[i] != 0x47 || (next < n && next < sizeof b && b[next] != 0x47))
                    continue;
                c->pos = from + (int64_t)i - c->skip; /* a sync byte, and another a packet on */
                break;
            }
        }
    }
    return 0;
}

/* The payload of a packet: its start and length, or 0 if none. */
static int
payload(const uint8_t p[188], const uint8_t **data, int *len)
{
    int afc = (p[3] >> 4) & 3, at = 4;

    if (!(afc & 1))
        return 0;
    if (afc & 2)
        at += 1 + p[4];
    if (at >= 188)
        return 0;
    *data = p + at;
    *len = 188 - at;
    return 1;
}

/* ---- tables ----------------------------------------------------------------- */

static es *
find(ts *c, int pid)
{
    for (int i = 0; i < c->count; i++)
        if (c->es[i].pid == pid)
            return &c->es[i];
    return NULL;
}

static void
parse_pat(ts *c, const uint8_t *d, int n)
{
    int ptr = d[0], len;

    d += 1 + ptr;
    n -= 1 + ptr;
    if (n < 8 || d[0] != 0x00)
        return;
    len = (d[1] & 15) << 8 | d[2];
    if (c->programs)
        return; /* the first PAT's programs */
    for (int at = 8; at + 4 <= 3 + len - 4 && at + 4 <= n && c->programs < MAX_PROGRAMS; at += 4) {
        int program = d[at] << 8 | d[at + 1];

        if (program != 0) { /* 0 is the network PID */
            c->program[c->programs] = program;
            c->pmt_pid[c->programs++] = (d[at + 2] & 0x1F) << 8 | d[at + 3];
        }
    }
}

/* What a stream type (and, for private data, its descriptors) carries. */
static papaya_codec
codec_of(int type, const uint8_t *desc, int dlen, papaya_media_kind *kind)
{
    *kind = PAPAYA_MEDIA_AUDIO;
    switch (type) {
    case 0x01:
    case 0x02: *kind = PAPAYA_MEDIA_VIDEO; return PAPAYA_CODEC_MPEG2;
    case 0x1B: *kind = PAPAYA_MEDIA_VIDEO; return PAPAYA_CODEC_H264;
    case 0x24: *kind = PAPAYA_MEDIA_VIDEO; return PAPAYA_CODEC_HEVC;
    case 0x0F:
    case 0x11: return PAPAYA_CODEC_AAC; /* ADTS, LATM */
    case 0x03:
    case 0x04: return PAPAYA_CODEC_MP3;
    case 0x81: return PAPAYA_CODEC_AC3;
    case 0x87: return PAPAYA_CODEC_EAC3;
    case 0x06:
        for (int at = 0; at + 2 <= dlen; at += 2 + desc[at + 1]) {
            int tag = desc[at], len = desc[at + 1];

            if (tag == 0x6A)
                return PAPAYA_CODEC_AC3;
            if (tag == 0x7A)
                return PAPAYA_CODEC_EAC3;
            if (tag == 0x05 && len >= 4 && at + 6 <= dlen && !memcmp(desc + at + 2, "AC-3", 4))
                return PAPAYA_CODEC_AC3;
        }
        return PAPAYA_CODEC_UNKNOWN;
    default:
        return PAPAYA_CODEC_UNKNOWN;
    }
}

/* Program k's PMT: its streams, once (a PID two programs share counts for
 * the first). */
static void
parse_pmt(ts *c, int k, const uint8_t *d, int n)
{
    int ptr = d[0], len, info;

    d += 1 + ptr;
    n -= 1 + ptr;
    if (n < 12 || d[0] != 0x02 || c->pmt_done[k] || (d[3] << 8 | d[4]) != c->program[k])
        return;
    c->pmt_done[k] = 1;
    len = (d[1] & 15) << 8 | d[2];
    info = (d[10] & 15) << 8 | d[11];
    for (int at = 12 + info; at + 5 <= 3 + len - 4 && at + 5 <= n && c->count < MAX_ES;) {
        int type = d[at], pid = (d[at + 1] & 0x1F) << 8 | d[at + 2];
        int dlen = (d[at + 3] & 15) << 8 | d[at + 4];
        es *e = &c->es[c->count];
        const uint8_t *desc = d + at + 5;
        int desc_ok = at + 5 + dlen <= n;

        at += 5 + dlen;
        if (find(c, pid))
            continue;
        memset(e, 0, sizeof *e);
        e->pid = pid;
        e->stream = -1;
        e->program = c->program[k];
        e->latm = type == 0x11;
        e->codec = codec_of(type, desc, desc_ok ? dlen : 0, &e->kind);
        if (e->codec != PAPAYA_CODEC_UNKNOWN)
            c->count++;
    }
}

/* ---- PES --------------------------------------------------------------------- */

static int64_t
pes_time(const uint8_t *p)
{
    return (int64_t)(p[0] >> 1 & 7) << 30 | (int64_t)p[1] << 22 | (int64_t)(p[2] >> 1) << 15
           | (int64_t)p[3] << 7 | p[4] >> 1;
}

/* A 90 kHz time to ours: unwrapped, from the file's start. */
static int64_t
to_us(ts *c, int64_t t90)
{
    int64_t d;

    if (c->start90 < 0)
        c->start90 = t90;
    d = (t90 - c->start90) % WRAP;
    if (d < 0)
        d += WRAP;
    /* times just before the start (audio ahead of video) stay negative */
    if (d > WRAP / 2)
        d -= WRAP;
    return d * 100 / 9;
}

/* An unsigned Exp-Golomb value at bit *bit of p (n bytes); -1 past the end. */
static int
ue(const uint8_t *p, size_t n, size_t *bit)
{
    int zeros = 0;
    unsigned v = 0;

    while (*bit < n * 8 && !(p[*bit / 8] >> (7 - *bit % 8) & 1) && zeros < 31) {
        zeros++;
        (*bit)++;
    }
    if (*bit + (size_t)zeros >= n * 8)
        return -1;
    for (int i = 0; i <= zeros; i++, (*bit)++)
        v = v << 1 | (p[*bit / 8] >> (7 - *bit % 8) & 1);
    return (int)(v - 1);
}

/* H.264 / HEVC: whether the access unit can start decoding: an IDR / IRAP
 * picture, or for H.264 an I slice (broadcast streams may carry no IDR after
 * the first; decoders start there, as with FFmpeg and Media3). */
static int
is_key(papaya_codec codec, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 4 < n; i++) {
        if (p[i] || p[i + 1] || p[i + 2] != 1)
            continue;
        if (codec == PAPAYA_CODEC_H264 && (p[i + 3] & 0x1F) == 5)
            return 1;
        if (codec == PAPAYA_CODEC_H264 && (p[i + 3] & 0x1F) == 1) {
            size_t bit = 0, left = n - i - 4 < 8 ? n - i - 4 : 8;
            int type;

            if (ue(p + i + 4, left, &bit) < 0) /* first_mb_in_slice */
                continue;
            type = ue(p + i + 4, left, &bit);
            if (type == 2 || type == 7)
                return 1;
        }
        if (codec == PAPAYA_CODEC_HEVC) {
            int t = (p[i + 3] >> 1) & 0x3F;

            if (t >= 16 && t <= 21)
                return 1;
        }
    }
    return codec != PAPAYA_CODEC_H264 && codec != PAPAYA_CODEC_HEVC;
}

static int
adts_header(const uint8_t *h, size_t n, int *size, int *head, es *e)
{
    if (n < 7 || h[0] != 0xFF || (h[1] & 0xF6) != 0xF0)
        return -1;
    *head = (h[1] & 1) ? 7 : 9;
    *size = (h[3] & 3) << 11 | h[4] << 3 | h[5] >> 5;
    if (*size <= *head || (size_t)*size > n)
        return -1;
    if (!e->ready) {
        static const int rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                       22050, 16000, 12000, 11025, 8000, 7350 };

        e->profile = h[2] >> 6;
        e->rate_index = (h[2] >> 2) & 15;
        e->channels = (h[2] & 1) << 2 | h[3] >> 6;
        if (e->rate_index > 12)
            return -1;
        e->rate = rates[e->rate_index];
        e->asc[0] = (uint8_t)((e->profile + 1) << 3 | e->rate_index >> 1);
        e->asc[1] = (uint8_t)((e->rate_index & 1) << 7 | e->channels << 3);
    }
    return 0;
}

/* ---- LATM (ISO/IEC 14496-3 1.7) ---------------------------------------------- */

typedef struct {
    const uint8_t *p;
    size_t n, bit;
} bits;

static uint32_t
get(bits *b, int count)
{
    uint32_t v = 0;

    while (count-- > 0) {
        int bit = 0;

        if (b->bit < b->n * 8)
            bit = b->p[b->bit / 8] >> (7 - b->bit % 8) & 1;
        b->bit++;
        v = v << 1 | (uint32_t)bit;
    }
    return v;
}

static uint32_t
latm_value(bits *b)
{
    uint32_t n = get(b, 2), v = 0;

    for (uint32_t i = 0; i <= n; i++)
        v = v << 8 | get(b, 8);
    return v;
}

static const int aac_rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                   22050, 16000, 12000, 11025, 8000, 7350 };

static int
rate_of(bits *b)
{
    uint32_t i = get(b, 4);

    return i == 15 ? (int)get(b, 24) : i < 13 ? aac_rates[i] : 0;
}

/* A LOAS frame's AudioMuxElement (after the 3-byte sync and length): the
 * format from its StreamMuxConfig, if it carries one. 0 when found. */
static int
latm_config(const uint8_t *p, size_t n, es *e)
{
    bits b = { p, n, 0 };
    uint32_t version, aot, config;

    if (get(&b, 1)) /* useSameStreamMux */
        return -1;
    version = get(&b, 1);
    if (version && get(&b, 1)) /* audioMuxVersionA: reserved */
        return -1;
    if (version)
        latm_value(&b); /* taraBufferFullness */
    get(&b, 1 + 6);     /* allStreamsSameTimeFraming, numSubFrames */
    if (get(&b, 4) != 0 || get(&b, 3) != 0)
        return -1;      /* more than one program or layer */
    if (version)
        latm_value(&b); /* ascLen */
    aot = get(&b, 5);
    if (aot == 31)
        aot = 32 + get(&b, 6);
    e->rate = rate_of(&b);
    config = get(&b, 4);
    e->out_rate = e->rate;
    if (aot == 5 || aot == 29)
        e->out_rate = rate_of(&b); /* explicit SBR */
    if (b.bit > n * 8 || e->rate <= 0 || e->out_rate <= 0 || config < 1 || config > 7)
        return -1;
    e->channels = config == 7 ? 8 : (int)config;
    return 0;
}

static void
push(ts *c, papaya_packet *p)
{
    if (c->qlen == QUEUE) {
        papaya_packet_free(&p);
        return;
    }
    c->queue[(c->qhead + c->qlen++) % QUEUE] = p;
}

/* A finished PES: its packets onto the queue (none while open probes). */
static void
finish_pes(ts *c, es *e, int probing)
{
    const uint8_t *p = e->pes, *data;
    size_t n = e->len, hl;
    int64_t pts = PAPAYA_NOPTS, dts = PAPAYA_NOPTS;

    e->open = 0;
    e->len = 0;
    if (n < 9 || p[0] || p[1] || p[2] != 1)
        return;
    hl = 9 + (size_t)p[8];
    if (hl > n)
        return;
    if ((p[7] & 0x80) && p[8] >= 5)
        pts = pes_time(p + 9);
    if ((p[7] & 0x40) && p[8] >= 10)
        dts = pes_time(p + 14);
    data = p + hl;
    n -= hl;
    if (probing && pts != PAPAYA_NOPTS && c->start90 < 0)
        c->start90 = dts != PAPAYA_NOPTS ? dts : pts; /* the file's first time */
    if (e->codec == PAPAYA_CODEC_AAC && e->latm) {
        int i = 0;

        for (size_t at = 0; at + 3 <= n && data[at] == 0x56 && (data[at + 1] & 0xE0) == 0xE0; i++) {
            size_t size = 3 + (size_t)((data[at + 1] & 0x1F) << 8 | data[at + 2]);
            papaya_packet *out;

            if (size > n - at)
                break;
            if (!e->ready && latm_config(data + at + 3, size - 3, e) == 0)
                e->ready = 1;
            if (e->ready && !probing && e->stream >= 0) { /* frames before a config are no use */
                out = papaya_packet_new(data + at, size);
                if (!out)
                    return;
                out->stream = e->stream;
                out->flags |= PAPAYA_PACKET_KEY;
                out->pts = out->dts = pts == PAPAYA_NOPTS ? PAPAYA_NOPTS
                                                          : to_us(c, pts + (int64_t)i * 1024 * 90000 / e->rate);
                push(c, out);
            }
            at += size;
        }
        return;
    }
    if (e->codec == PAPAYA_CODEC_AAC) {
        int size, head, i = 0;

        for (size_t at = 0; adts_header(data + at, n - at, &size, &head, e) == 0; at += (size_t)size, i++) {
            papaya_packet *out;

            e->ready = 1;
            if (probing || e->stream < 0)
                continue;
            out = papaya_packet_new(data + at + head, (size_t)(size - head));
            if (!out)
                return;
            out->stream = e->stream;
            out->flags |= PAPAYA_PACKET_KEY;
            out->pts = out->dts = pts == PAPAYA_NOPTS ? PAPAYA_NOPTS
                                                      : to_us(c, pts + (int64_t)i * 1024 * 90000 / e->rate);
            push(c, out);
        }
        return;
    }
    e->ready = 1;
    if (probing)
        return;
    if (e->stream < 0 || n == 0)
        return;
    {
        papaya_packet *out = papaya_packet_new(data, n);

        if (!out)
            return;
        out->stream = e->stream;
        out->pts = pts == PAPAYA_NOPTS ? PAPAYA_NOPTS : to_us(c, pts);
        out->dts = dts == PAPAYA_NOPTS ? out->pts : to_us(c, dts);
        if (is_key(e->codec, data, n))
            out->flags |= PAPAYA_PACKET_KEY;
        push(c, out);
    }
}

/* One transport packet: tables, or a piece of some PES. */
static void
take(ts *c, const uint8_t p[188], int probing)
{
    int pid = (p[1] & 0x1F) << 8 | p[2], start = p[1] & 0x40, len;
    const uint8_t *d;
    es *e;

    if ((p[1] & 0x80) || !payload(p, &d, &len))
        return; /* transport error, or no payload */
    if (pid == 0) {
        if (start)
            parse_pat(c, d, len);
        return;
    }
    for (int k = 0; k < c->programs; k++)
        if (pid == c->pmt_pid[k]) {
            if (start)
                parse_pmt(c, k, d, len);
            return;
        }
    e = find(c, pid);
    if (!e)
        return;
    if (start) {
        if (e->open)
            finish_pes(c, e, probing);
        e->open = 1;
        e->len = 0;
    }
    if (!e->open)
        return; /* the middle of a PES we did not see start */
    if (e->len + (size_t)len > e->cap) {
        size_t cap = (e->len + (size_t)len) * 2;
        uint8_t *grown = cap <= (64u << 20) ? realloc(e->pes, cap) : NULL;

        if (!grown) {
            e->open = 0;
            return;
        }
        e->pes = grown;
        e->cap = cap;
    }
    memcpy(e->pes + e->len, d, (size_t)len);
    e->len += (size_t)len;
}

/* ---- open / read ------------------------------------------------------------- */

static void
reset(ts *c, int64_t pos)
{
    c->pos = pos;
    for (int i = 0; i < c->count; i++) {
        c->es[i].open = 0;
        c->es[i].len = 0;
    }
    while (c->qlen) {
        papaya_packet *p = c->queue[c->qhead];

        papaya_packet_free(&p);
        c->qhead = (c->qhead + 1) % QUEUE;
        c->qlen--;
    }
    c->qhead = 0;
}

/* The first PES-start PTS of es e at or after pos (packet-aligned): its time
 * and position, or -1. */
static int64_t
pts_after(ts *c, int64_t pos, int64_t limit, const es *e, int64_t *at)
{
    uint8_t p[188];

    c->pos = pos;
    while (c->pos < limit && next_packet(c, p)) {
        const uint8_t *d;
        int len;

        if (((p[1] & 0x1F) << 8 | p[2]) != e->pid || !(p[1] & 0x40) || !payload(p, &d, &len))
            continue;
        if (len >= 14 && !d[0] && !d[1] && d[2] == 1 && (d[7] & 0x80)) {
            *at = c->pos - c->psize;
            return pes_time(d + 9);
        }
    }
    return -1;
}

static void
ts_close(void *ctx)
{
    ts *c = ctx;

    reset(c, 0);
    for (int i = 0; i < c->count; i++)
        free(c->es[i].pes);
    free(c);
}

static void *
ts_open(papaya_demuxer *d, papaya_io *io)
{
    uint8_t head[4 + 188 * 4], p[188];
    ts *c = calloc(1, sizeof *c);
    int64_t limit;

    if (!c)
        return NULL;
    c->io = io;
    c->start90 = -1;
    c->ref = -1;
    c->end = io->size(io->opaque);
    if (c->end < 0)
        c->end = INT64_MAX;
    if (papaya_io_read_at(io, 0, head, sizeof head) != 0)
        goto fail;
    c->psize = head[0] == 0x47 && head[188] == 0x47 ? 188 : 192;
    c->skip = c->psize == 192 ? 4 : 0;

    /* The tables, then every stream's first PES (AAC's config is in it). */
    limit = c->end < PROBE_BYTES ? c->end : PROBE_BYTES;
    while (c->pos < limit && next_packet(c, p)) {
        int all;

        take(c, p, 1);
        all = c->count > 0;
        for (int k = 0; k < c->programs; k++)
            if (!c->pmt_done[k])
                all = 0;
        for (int i = 0; i < c->count; i++)
            if (!c->es[i].ready)
                all = 0;
        if (all)
            break;
    }
    if (c->count == 0)
        goto fail;
    for (int i = 0; i < c->count; i++) {
        es *e = &c->es[i];
        papaya_stream_info *s;

        if (!e->ready || (e->codec == PAPAYA_CODEC_AAC && e->rate <= 0))
            continue;
        s = papaya_demux_add_stream(d);
        if (!s)
            break;
        s->kind = e->kind;
        s->codec = e->codec;
        s->codec_tag = "MPEG-TS";
        if (e->codec == PAPAYA_CODEC_AAC) {
            s->sample_rate = e->latm ? e->out_rate : e->rate;
            s->channels = e->channels;
            s->codec_tag = e->latm ? "LATM" : "MPEG-TS";
            if (!e->latm) {
                s->extradata = e->asc;
                s->extradata_size = sizeof e->asc;
            }
        }
        if (c->programs > 1)
            snprintf(s->title, sizeof s->title, "Program %d", e->program);
        if (e->kind == PAPAYA_MEDIA_VIDEO)
            s->color.primaries = s->color.transfer = s->color.matrix = 2; /* from the bitstream */
        e->stream = s->index;
        if (c->ref < 0 || (e->kind == PAPAYA_MEDIA_VIDEO && c->es[c->ref].kind != PAPAYA_MEDIA_VIDEO))
            c->ref = i;
    }
    if (c->ref < 0)
        goto fail;

    /* The length: the last PTS of the reference stream near the end. */
    if (c->end != INT64_MAX) {
        int64_t from = c->end - TAIL_BYTES > 0 ? c->end - TAIL_BYTES : 0, at, last = -1, t;

        from -= from % c->psize;
        while ((t = pts_after(c, from, c->end, &c->es[c->ref], &at)) >= 0) {
            last = t;
            from = at + c->psize;
        }
        if (last >= 0 && c->start90 >= 0)
            papaya_demux_set_duration(d, to_us(c, last));
    }
    reset(c, 0);
    return c;
fail:
    ts_close(c);
    return NULL;
}

static int
ts_read(void *ctx, papaya_packet **packet)
{
    ts *c = ctx;
    uint8_t p[188];

    for (;;) {
        while (c->qlen) {
            papaya_packet *out = c->queue[c->qhead];

            c->qhead = (c->qhead + 1) % QUEUE;
            c->qlen--;
            if (c->wait_key) {
                /* After a seek, everything waits for a video keyframe. */
                if (!(c->es[c->ref].stream == out->stream && (out->flags & PAPAYA_PACKET_KEY))) {
                    papaya_packet_free(&out);
                    continue;
                }
                c->wait_key = 0;
            }
            *packet = out;
            return 0;
        }
        if (!next_packet(c, p)) {
            /* The end: whatever PES is still open is complete. */
            for (int i = 0; i < c->count; i++)
                if (c->es[i].open)
                    finish_pes(c, &c->es[i], 0);
            if (!c->qlen)
                return PAPAYA_DEMUX_EOF;
            continue;
        }
        take(c, p, 0);
    }
}

static int
ts_seek(void *ctx, int64_t pts)
{
    ts *c = ctx;
    const es *ref = &c->es[c->ref];
    int64_t target = pts * 9 / 100 - SEEK_BACK, lo = 0, hi = c->end == INT64_MAX ? 0 : c->end, best = 0;

    if (pts <= 0 || c->start90 < 0) {
        reset(c, 0);
        c->wait_key = 0;
        return 0;
    }
    /* Bisect for the last reference PES starting at or before the target. */
    while (hi - lo > (int64_t)64 * c->psize) {
        int64_t mid = lo + (hi - lo) / 2, at, t;

        mid -= mid % c->psize;
        t = pts_after(c, mid, hi, ref, &at);
        if (t < 0) {
            hi = mid;
            continue;
        }
        t = (t - c->start90) % WRAP;
        if (t < 0)
            t += WRAP;
        if (t <= target) {
            best = at;
            lo = at + c->psize;
        } else {
            hi = mid;
        }
    }
    reset(c, best);
    c->wait_key = ref->kind == PAPAYA_MEDIA_VIDEO;
    return 0;
}

const papaya_demux_backend papaya_demux_ts = {
    "mpegts", ts_probe, ts_open, ts_read, ts_seek, ts_close,
};
