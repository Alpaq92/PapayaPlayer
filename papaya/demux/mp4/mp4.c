/* MP4 / MOV (ISO base media file format), non-fragmented files.
 *
 * Structured after AndroidX Media3's Mp4Extractor and BoxParser
 * (libraries/extractor/.../mp4/, Apache-2.0, Copyright (C) 2016 The Android
 * Open Source Project): the moov box is read whole, every track's sample table
 * (stts, ctts, stsc, stsz / stz2, stco / co64, stss) is expanded into one
 * array of samples, the first edit-list entry shifts its times, and packets
 * are read across tracks in file order. Fragmented MP4 (moof) is a follow-up.
 *
 * Licensed under the Apache License, Version 2.0. */
#include <stdlib.h>
#include <string.h>

#include "demux_internal.h"

#define MAX_TRACKS 16
#define MAX_MOOV (256u << 20)
#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

typedef struct {
    int64_t offset;
    uint32_t size;
    int64_t dts, pts;     /* media timescale, after the edit list */
    uint8_t key;
} sample;

typedef struct {
    int stream;           /* our stream index, -1 if unused */
    uint32_t timescale;
    sample *samples;
    uint32_t count, next;
    uint8_t *extradata;
    size_t extradata_size;
    char tag[5];          /* sample entry fourcc */
} track;

typedef struct {
    papaya_io *io;
    track tracks[MAX_TRACKS];
    int track_count;
    uint32_t movie_timescale;
} mp4;

/* ---- big-endian reading over a buffer -------------------------------------- */

typedef struct {
    const uint8_t *p, *end;
    int bad;
} rd;

static int left(const rd *r) { return r->bad ? 0 : (int)(r->end - r->p); }

static void
skip(rd *r, size_t n)
{
    if (r->bad || (size_t)(r->end - r->p) < n) {
        r->bad = 1;
        r->p = r->end;
        return;
    }
    r->p += n;
}

static uint32_t
u8(rd *r)
{
    if (r->bad || r->p >= r->end) {
        r->bad = 1;
        return 0;
    }
    return *r->p++;
}

static uint32_t u16(rd *r) { uint32_t a = u8(r); return a << 8 | u8(r); }
static uint32_t u32(rd *r) { uint32_t a = u16(r); return a << 16 | u16(r); }
static uint64_t u64(rd *r) { uint64_t a = u32(r); return a << 32 | u32(r); }

/* The next child box of r: its type, and a reader over its payload. */
static int
next_box(rd *r, uint32_t *type, rd *payload)
{
    uint64_t size;
    const uint8_t *start = r->p;

    if (left(r) < 8)
        return 0;
    size = u32(r);
    *type = u32(r);
    if (size == 1)
        size = u64(r);
    else if (size == 0)
        size = (uint64_t)(r->end - start);
    if (r->bad || size < (uint64_t)(r->p - start) || size > (uint64_t)(r->end - start))
        return 0;
    payload->p = r->p;
    payload->end = start + size;
    payload->bad = 0;
    r->p = start + size;
    return 1;
}

/* The first child of type t inside r, searched without descending. */
static int
find_box(rd r, uint32_t t, rd *out)
{
    uint32_t type;
    rd child;

    while (next_box(&r, &type, &child))
        if (type == t) {
            *out = child;
            return 1;
        }
    return 0;
}

/* ---- sample entries (stsd) ------------------------------------------------- */

static void
set_extradata(track *t, const uint8_t *data, size_t size)
{
    free(t->extradata);
    t->extradata = malloc(size ? size : 1);
    if (!t->extradata)
        return;
    memcpy(t->extradata, data, size);
    t->extradata_size = size;
}

/* An MPEG-4 descriptor's length: up to four 7-bit groups. */
static uint32_t
desc_len(rd *r)
{
    uint32_t len = 0;

    for (int i = 0; i < 4; i++) {
        uint32_t b = u8(r);

        len = len << 7 | (b & 0x7f);
        if (!(b & 0x80))
            break;
    }
    return len;
}

/* esds: the object type and the decoder-specific info (AAC's AudioSpecificConfig). */
static papaya_codec
parse_esds(rd r, track *t)
{
    uint32_t object_type = 0;

    skip(&r, 4); /* version, flags */
    if (u8(&r) != 0x03)
        return PAPAYA_CODEC_UNKNOWN;
    desc_len(&r);
    skip(&r, 2);
    {
        uint32_t flags = u8(&r);

        if (flags & 0x80)
            skip(&r, 2);
        if (flags & 0x40)
            skip(&r, u8(&r));
        if (flags & 0x20)
            skip(&r, 2);
    }
    if (u8(&r) != 0x04)
        return PAPAYA_CODEC_UNKNOWN;
    desc_len(&r);
    object_type = u8(&r);
    skip(&r, 12);
    if (left(&r) > 2 && u8(&r) == 0x05) {
        uint32_t len = desc_len(&r);

        if (!r.bad && len <= (uint32_t)left(&r))
            set_extradata(t, r.p, len);
    }
    switch (object_type) {
    case 0x40:
    case 0x66:
    case 0x67:
    case 0x68:
        return PAPAYA_CODEC_AAC;
    case 0x69:
    case 0x6B:
        return PAPAYA_CODEC_MP3;
    case 0xA5:
        return PAPAYA_CODEC_AC3;
    case 0xA6:
        return PAPAYA_CODEC_EAC3;
    default:
        return PAPAYA_CODEC_UNKNOWN;
    }
}

/* dOps (big endian) to OpusHead (little endian), what the Opus decoder reads. */
static void
opus_head_from_dops(rd r, track *t)
{
    uint8_t head[19 + 2 + 255];
    uint32_t channels, preskip, rate, family;
    int32_t gain;
    size_t n = 19;

    skip(&r, 1); /* version */
    channels = u8(&r);
    preskip = u16(&r);
    rate = u32(&r);
    gain = (int16_t)u16(&r);
    family = u8(&r);
    if (r.bad)
        return;
    static const uint8_t magic[8] = { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd' };

    memcpy(head, magic, sizeof magic);
    head[8] = 1;
    head[9] = (uint8_t)channels;
    head[10] = (uint8_t)preskip;
    head[11] = (uint8_t)(preskip >> 8);
    for (int i = 0; i < 4; i++)
        head[12 + i] = (uint8_t)(rate >> (8 * i));
    head[16] = (uint8_t)gain;
    head[17] = (uint8_t)((uint32_t)gain >> 8);
    head[18] = (uint8_t)family;
    if (family != 0) {
        head[19] = (uint8_t)u8(&r); /* stream count */
        head[20] = (uint8_t)u8(&r); /* coupled count */
        for (uint32_t i = 0; i < channels && i < 255; i++)
            head[21 + i] = (uint8_t)u8(&r);
        n = 21 + channels;
        if (r.bad)
            return;
    }
    set_extradata(t, head, n);
}

static void
parse_colour_boxes(rd r, papaya_stream_info *s)
{
    uint32_t type;
    rd b;

    while (next_box(&r, &type, &b)) {
        if (type == FOURCC('c', 'o', 'l', 'r') && left(&b) >= 11) {
            uint32_t kind = u32(&b);

            if (kind == FOURCC('n', 'c', 'l', 'x') || kind == FOURCC('n', 'c', 'l', 'c')) {
                s->color.primaries = (uint8_t)u16(&b);
                s->color.transfer = (uint8_t)u16(&b);
                s->color.matrix = (uint8_t)u16(&b);
                if (kind == FOURCC('n', 'c', 'l', 'x'))
                    s->color.full_range = (uint8_t)(u8(&b) >> 7);
            }
        } else if (type == FOURCC('m', 'd', 'c', 'v') && left(&b) >= 24) {
            skip(&b, 16); /* display primaries and white point */
            s->color.max_luminance = (float)u32(&b) / 10000.0f;
            s->color.min_luminance = (float)u32(&b) / 10000.0f;
        } else if (type == FOURCC('c', 'l', 'l', 'i') && left(&b) >= 4) {
            s->color.max_cll = (uint16_t)u16(&b);
            s->color.max_fall = (uint16_t)u16(&b);
        }
    }
}

static void
parse_stsd(rd r, track *t, papaya_stream_info *s, uint32_t handler)
{
    uint32_t format;
    rd entry, child;

    skip(&r, 8); /* version, flags, entry count */
    if (!next_box(&r, &format, &entry))
        return;
    s->codec = PAPAYA_CODEC_UNKNOWN;
    if (handler == FOURCC('v', 'i', 'd', 'e')) {
        s->kind = PAPAYA_MEDIA_VIDEO;
        s->color.primaries = s->color.transfer = s->color.matrix = 2;
        skip(&entry, 24);
        s->width = (int)u16(&entry);
        s->height = (int)u16(&entry);
        skip(&entry, 50);
        switch (format) {
        case FOURCC('a', 'v', 'c', '1'):
        case FOURCC('a', 'v', 'c', '3'):
            s->codec = PAPAYA_CODEC_H264;
            if (find_box(entry, FOURCC('a', 'v', 'c', 'C'), &child))
                set_extradata(t, child.p, (size_t)left(&child));
            break;
        case FOURCC('h', 'v', 'c', '1'):
        case FOURCC('h', 'e', 'v', '1'):
            s->codec = PAPAYA_CODEC_HEVC;
            if (find_box(entry, FOURCC('h', 'v', 'c', 'C'), &child))
                set_extradata(t, child.p, (size_t)left(&child));
            break;
        case FOURCC('a', 'v', '0', '1'):
            s->codec = PAPAYA_CODEC_AV1;
            if (find_box(entry, FOURCC('a', 'v', '1', 'C'), &child))
                set_extradata(t, child.p, (size_t)left(&child));
            break;
        case FOURCC('v', 'p', '0', '9'):
            s->codec = PAPAYA_CODEC_VP9;
            break;
        case FOURCC('v', 'p', '0', '8'):
            s->codec = PAPAYA_CODEC_VP8;
            break;
        case FOURCC('m', 'p', '4', 'v'):
            if (find_box(entry, FOURCC('e', 's', 'd', 's'), &child))
                parse_esds(child, t);
            break;
        default:
            break;
        }
        parse_colour_boxes(entry, s);
    } else if (handler == FOURCC('s', 'o', 'u', 'n')) {
        uint32_t version;

        s->kind = PAPAYA_MEDIA_AUDIO;
        skip(&entry, 8);
        version = u16(&entry);
        skip(&entry, 6);
        s->channels = (int)u16(&entry);
        s->bits_per_sample = (int)u16(&entry);
        skip(&entry, 4);
        s->sample_rate = (int)(u32(&entry) >> 16);
        if (version == 1)
            skip(&entry, 16);
        else if (version == 2)
            skip(&entry, 36);
        switch (format) {
        case FOURCC('m', 'p', '4', 'a'):
            /* QuickTime wraps esds in a wave box. */
            if (find_box(entry, FOURCC('e', 's', 'd', 's'), &child)
                || (find_box(entry, FOURCC('w', 'a', 'v', 'e'), &child)
                    && find_box(child, FOURCC('e', 's', 'd', 's'), &child)))
                s->codec = parse_esds(child, t);
            break;
        case FOURCC('O', 'p', 'u', 's'):
            s->codec = PAPAYA_CODEC_OPUS;
            if (find_box(entry, FOURCC('d', 'O', 'p', 's'), &child))
                opus_head_from_dops(child, t);
            s->sample_rate = 48000;
            break;
        case FOURCC('f', 'L', 'a', 'C'):
            s->codec = PAPAYA_CODEC_FLAC;
            if (find_box(entry, FOURCC('d', 'f', 'L', 'a'), &child) && left(&child) > 4) {
                /* "fLaC" and the metadata blocks after dfLa's version and flags. */
                size_t n = (size_t)left(&child) - 4;
                uint8_t *buf = malloc(4 + n);

                if (buf) {
                    static const uint8_t magic[4] = { 'f', 'L', 'a', 'C' };

                    memcpy(buf, magic, sizeof magic);
                    memcpy(buf + 4, child.p + 4, n);
                    set_extradata(t, buf, 4 + n);
                    free(buf);
                }
            }
            break;
        case FOURCC('.', 'm', 'p', '3'):
            s->codec = PAPAYA_CODEC_MP3;
            break;
        case FOURCC('a', 'c', '-', '3'):
            s->codec = PAPAYA_CODEC_AC3;
            break;
        case FOURCC('e', 'c', '-', '3'):
            s->codec = PAPAYA_CODEC_EAC3;
            break;
        case FOURCC('s', 'o', 'w', 't'):
            s->codec = PAPAYA_CODEC_PCM_S16LE;
            break;
        case FOURCC('f', 'l', '3', '2'):
            s->codec = PAPAYA_CODEC_PCM_F32LE;
            break;
        default:
            break;
        }
    }
    t->tag[0] = (char)(format >> 24);
    t->tag[1] = (char)(format >> 16);
    t->tag[2] = (char)(format >> 8);
    t->tag[3] = (char)format;
    s->codec_tag = t->tag;
}

/* ---- sample tables --------------------------------------------------------- */

/* Expands stbl into t->samples. Returns 0 or -1. */
static int
build_samples(rd stbl, track *t, int64_t edit_shift)
{
    rd stts, ctts, stsc, stsz, stco, stss;
    int has_ctts, has_stss, co64 = 0;
    uint32_t count, stsz_fixed = 0, stz2_bits = 0, chunk_count;

    if (!find_box(stbl, FOURCC('s', 't', 't', 's'), &stts) || !find_box(stbl, FOURCC('s', 't', 's', 'c'), &stsc))
        return -1;
    if (!find_box(stbl, FOURCC('s', 't', 's', 'z'), &stsz)) {
        if (!find_box(stbl, FOURCC('s', 't', 'z', '2'), &stsz))
            return -1;
        skip(&stsz, 7);
        stz2_bits = u8(&stsz);
    } else {
        skip(&stsz, 4);
        stsz_fixed = u32(&stsz);
    }
    count = u32(&stsz);
    if (!find_box(stbl, FOURCC('s', 't', 'c', 'o'), &stco)) {
        if (!find_box(stbl, FOURCC('c', 'o', '6', '4'), &stco))
            return -1;
        co64 = 1;
    }
    has_ctts = find_box(stbl, FOURCC('c', 't', 't', 's'), &ctts);
    has_stss = find_box(stbl, FOURCC('s', 't', 's', 's'), &stss);
    if (count == 0 || count > 50000000 || stsz.bad)
        return count == 0 ? 0 : -1;
    t->samples = calloc(count, sizeof *t->samples);
    if (!t->samples)
        return -1;
    t->count = count;

    /* sizes */
    for (uint32_t i = 0; i < count; i++) {
        if (stsz_fixed)
            t->samples[i].size = stsz_fixed;
        else if (stz2_bits == 4)
            t->samples[i].size = (i & 1) ? (stsz.p[-1] & 0x0f) : (u8(&stsz) >> 4);
        else if (stz2_bits == 8)
            t->samples[i].size = u8(&stsz);
        else if (stz2_bits == 16)
            t->samples[i].size = u16(&stsz);
        else
            t->samples[i].size = u32(&stsz);
    }

    /* offsets: chunks from stco / co64, samples per chunk from stsc */
    skip(&stco, 4);
    chunk_count = u32(&stco);
    skip(&stsc, 4);
    {
        uint32_t entries = u32(&stsc), first = 0, per = 0, next_first = 0, si = 0;
        uint32_t e = 0;

        if (entries) {
            first = u32(&stsc);
            per = u32(&stsc);
            skip(&stsc, 4);
            e = 1;
            next_first = e < entries ? u32(&stsc) : UINT32_MAX;
        }
        for (uint32_t c = 1; c <= chunk_count && si < count && !stco.bad; c++) {
            int64_t off = co64 ? (int64_t)u64(&stco) : (int64_t)u32(&stco);

            if (c == next_first) {
                first = next_first;
                per = u32(&stsc);
                skip(&stsc, 4);
                e++;
                next_first = e < entries ? u32(&stsc) : UINT32_MAX;
            }
            (void)first;
            for (uint32_t k = 0; k < per && si < count; k++) {
                t->samples[si].offset = off;
                off += t->samples[si].size;
                si++;
            }
        }
        if (si < count)
            t->count = count = si; /* truncated table: keep what is addressable */
    }

    /* decode times from stts, presentation offsets from ctts */
    skip(&stts, 4);
    {
        uint32_t entries = u32(&stts), si = 0;
        int64_t dts = 0;

        for (uint32_t e = 0; e < entries && si < count && !stts.bad; e++) {
            uint32_t n = u32(&stts), delta = u32(&stts);

            for (uint32_t k = 0; k < n && si < count; k++, si++) {
                t->samples[si].dts = dts;
                t->samples[si].pts = dts;
                dts += delta;
            }
        }
        for (; si < count; si++)
            t->samples[si].dts = t->samples[si].pts = dts;
    }
    if (has_ctts) {
        uint32_t version = u8(&ctts), entries, si = 0;

        skip(&ctts, 3);
        entries = u32(&ctts);
        for (uint32_t e = 0; e < entries && si < count && !ctts.bad; e++) {
            uint32_t n = u32(&ctts);
            int64_t off = version ? (int64_t)(int32_t)u32(&ctts) : (int64_t)u32(&ctts);

            for (uint32_t k = 0; k < n && si < count; k++, si++)
                t->samples[si].pts += off;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        t->samples[i].dts -= edit_shift;
        t->samples[i].pts -= edit_shift;
    }

    /* keyframes: every sample when there is no stss */
    if (has_stss) {
        uint32_t entries;

        skip(&stss, 4);
        entries = u32(&stss);
        for (uint32_t e = 0; e < entries && !stss.bad; e++) {
            uint32_t n = u32(&stss);

            if (n >= 1 && n <= count)
                t->samples[n - 1].key = 1;
        }
    } else {
        for (uint32_t i = 0; i < count; i++)
            t->samples[i].key = 1;
    }
    return 0;
}

static void
parse_trak(mp4 *m, papaya_demuxer *d, rd trak)
{
    rd mdia, mdhd, hdlr, minf, stbl, stsd, edts, elst;
    uint32_t handler, timescale, version;
    int64_t edit_shift = 0;
    papaya_stream_info *s;
    track *t;
    char lang[4] = { 0 };

    if (m->track_count >= MAX_TRACKS || !find_box(trak, FOURCC('m', 'd', 'i', 'a'), &mdia)
        || !find_box(mdia, FOURCC('m', 'd', 'h', 'd'), &mdhd) || !find_box(mdia, FOURCC('h', 'd', 'l', 'r'), &hdlr)
        || !find_box(mdia, FOURCC('m', 'i', 'n', 'f'), &minf) || !find_box(minf, FOURCC('s', 't', 'b', 'l'), &stbl)
        || !find_box(stbl, FOURCC('s', 't', 's', 'd'), &stsd))
        return;
    skip(&hdlr, 8);
    handler = u32(&hdlr);
    if (handler != FOURCC('v', 'i', 'd', 'e') && handler != FOURCC('s', 'o', 'u', 'n'))
        return;
    version = u8(&mdhd);
    skip(&mdhd, 3 + (version ? 16 : 8));
    timescale = u32(&mdhd);
    skip(&mdhd, version ? 8 : 4);
    {
        uint32_t l = u16(&mdhd);

        lang[0] = (char)(((l >> 10) & 31) + 0x60);
        lang[1] = (char)(((l >> 5) & 31) + 0x60);
        lang[2] = (char)((l & 31) + 0x60);
    }
    if (!timescale || mdhd.bad)
        return;

    /* The first non-empty edit decides where the track starts; an empty
     * first edit delays it. Further edits are rare and ignored. */
    if (find_box(trak, FOURCC('e', 'd', 't', 's'), &edts) && find_box(edts, FOURCC('e', 'l', 's', 't'), &elst)) {
        uint32_t ev = u8(&elst), entries;

        skip(&elst, 3);
        entries = u32(&elst);
        for (uint32_t e = 0; e < entries && !elst.bad; e++) {
            uint64_t dur = ev ? u64(&elst) : u32(&elst);
            int64_t media_time = ev ? (int64_t)u64(&elst) : (int64_t)(int32_t)u32(&elst);

            skip(&elst, 4);
            if (media_time == -1) {
                if (m->movie_timescale)
                    edit_shift -= (int64_t)(dur * timescale / m->movie_timescale);
                continue;
            }
            edit_shift += media_time;
            break;
        }
    }

    t = &m->tracks[m->track_count];
    memset(t, 0, sizeof *t);
    t->stream = -1;
    t->timescale = timescale;
    s = papaya_demux_add_stream(d);
    if (!s)
        return;
    parse_stsd(stsd, t, s, handler);
    if (build_samples(stbl, t, edit_shift) != 0 || t->count == 0) {
        free(t->samples);
        free(t->extradata);
        t->samples = NULL;
        t->extradata = NULL;
        s->codec = PAPAYA_CODEC_UNKNOWN;
        return;
    }
    s->extradata = t->extradata;
    s->extradata_size = t->extradata_size;
    if (strcmp(lang, "und") != 0 && lang[0] >= 'a')
        memcpy(s->language, lang, 4);
    s->duration = (t->samples[t->count - 1].pts + 1) * 1000000 / timescale;
    t->stream = s->index;
    m->track_count++;
}

/* ---- backend --------------------------------------------------------------- */

static int
mp4_probe(const uint8_t *head, size_t size)
{
    uint32_t type;

    if (size < 8)
        return 0;
    type = FOURCC(head[4], head[5], head[6], head[7]);
    if (type == FOURCC('f', 't', 'y', 'p'))
        return 100;
    if (type == FOURCC('m', 'o', 'o', 'v') || type == FOURCC('m', 'd', 'a', 't')
        || type == FOURCC('w', 'i', 'd', 'e') || type == FOURCC('f', 'r', 'e', 'e'))
        return 60;
    return 0;
}

static void
mp4_close(void *ctx)
{
    mp4 *m = ctx;

    for (int i = 0; i < m->track_count; i++) {
        free(m->tracks[i].samples);
        free(m->tracks[i].extradata);
    }
    free(m);
}

static void *
mp4_open(papaya_demuxer *d, papaya_io *io)
{
    int64_t pos = 0, size = io->size(io->opaque);
    uint8_t *moov = NULL;
    size_t moov_size = 0;
    mp4 *m;

    /* Find moov among the top-level boxes (it may follow mdat). */
    while (size < 0 || pos + 8 <= size) {
        uint8_t h[16];
        uint64_t box;
        uint32_t type, header = 8;

        if (papaya_io_read_at(io, pos, h, 8) != 0)
            break;
        box = (uint64_t)h[0] << 24 | (uint64_t)h[1] << 16 | (uint64_t)h[2] << 8 | h[3];
        type = FOURCC(h[4], h[5], h[6], h[7]);
        if (box == 1) {
            if (papaya_io_read_at(io, pos + 8, h + 8, 8) != 0)
                break;
            box = 0;
            for (int i = 8; i < 16; i++)
                box = box << 8 | h[i];
            header = 16;
        } else if (box == 0 && size >= 0) {
            box = (uint64_t)(size - pos);
        }
        if (box < header)
            break;
        if (type == FOURCC('m', 'o', 'o', 'v')) {
            if (box - header > MAX_MOOV)
                return NULL;
            moov_size = (size_t)(box - header);
            moov = malloc(moov_size ? moov_size : 1);
            if (!moov || papaya_io_read_at(io, pos + header, moov, moov_size) != 0) {
                free(moov);
                return NULL;
            }
            break;
        }
        pos += (int64_t)box;
    }
    if (!moov)
        return NULL;
    m = calloc(1, sizeof *m);
    if (!m) {
        free(moov);
        return NULL;
    }
    m->io = io;
    {
        rd r = { moov, moov + moov_size, 0 }, b;
        uint32_t type;

        if (find_box(r, FOURCC('m', 'v', 'h', 'd'), &b)) {
            uint32_t v = u8(&b);

            skip(&b, 3 + (v ? 16 : 8));
            m->movie_timescale = u32(&b);
            if (m->movie_timescale) {
                uint64_t dur = v ? u64(&b) : u32(&b);

                papaya_demux_set_duration(d, (int64_t)(dur * 1000000 / m->movie_timescale));
            }
        }
        while (next_box(&r, &type, &b))
            if (type == FOURCC('t', 'r', 'a', 'k'))
                parse_trak(m, d, b);
    }
    free(moov);
    if (m->track_count == 0) {
        mp4_close(m);
        return NULL;
    }
    return m;
}

static int
mp4_read(void *ctx, papaya_packet **packet)
{
    mp4 *m = ctx;
    track *best = NULL;
    const sample *s;
    papaya_packet *p;

    /* The pending sample that comes first in the file keeps reads sequential. */
    for (int i = 0; i < m->track_count; i++) {
        track *t = &m->tracks[i];

        if (t->next < t->count && (!best || t->samples[t->next].offset < best->samples[best->next].offset))
            best = t;
    }
    if (!best)
        return PAPAYA_DEMUX_EOF;
    s = &best->samples[best->next++];
    if (s->size == 0 || s->size > (256u << 20))
        return PAPAYA_DEMUX_ERROR;
    p = papaya_packet_new(NULL, s->size);
    if (!p)
        return PAPAYA_DEMUX_ERROR;
    if (papaya_io_read_at(m->io, s->offset, papaya_buffer_data(p->buf), s->size) != 0) {
        papaya_packet_free(&p);
        return PAPAYA_DEMUX_ERROR;
    }
    p->stream = best->stream;
    p->pts = s->pts * 1000000 / best->timescale;
    p->dts = s->dts * 1000000 / best->timescale;
    if (best->next < best->count)
        p->duration = (best->samples[best->next].dts - s->dts) * 1000000 / best->timescale;
    if (s->key)
        p->flags |= PAPAYA_PACKET_KEY;
    *packet = p;
    return 0;
}

/* The last keyframe at or before time (track units), or 0. */
static uint32_t
key_before(const track *t, int64_t time)
{
    uint32_t lo = 0, hi = t->count, best = 0;

    while (lo < hi) { /* the last sample with dts <= time */
        uint32_t mid = lo + (hi - lo) / 2;

        if (t->samples[mid].dts <= time)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (uint32_t i = lo; i-- > 0;)
        if (t->samples[i].key) {
            best = i;
            break;
        }
    return best;
}

static int
mp4_seek(void *ctx, int64_t pts)
{
    mp4 *m = ctx;
    int64_t start = pts;
    track *video = NULL;

    /* The track with sparse keyframes (video) decides: everything restarts
     * at its keyframe. */
    for (int i = 0; i < m->track_count; i++) {
        track *t = &m->tracks[i];

        if (t->count && t->samples[0].key && t->count > 1 && !t->samples[1].key)
            video = t; /* sparse keyframes: the video track */
    }
    if (video) {
        uint32_t k = key_before(video, pts * video->timescale / 1000000);

        start = video->samples[k].dts * 1000000 / video->timescale;
        video->next = k;
    }
    for (int i = 0; i < m->track_count; i++) {
        track *t = &m->tracks[i];

        if (t == video)
            continue;
        t->next = key_before(t, start * t->timescale / 1000000);
    }
    return 0;
}

const papaya_demux_backend papaya_demux_mp4 = {
    "mp4", mp4_probe, mp4_open, mp4_read, mp4_seek, mp4_close,
};
