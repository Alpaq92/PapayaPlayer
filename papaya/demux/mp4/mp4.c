/* MP4 / MOV (ISO base media file format), plain and fragmented.
 *
 * Structured after AndroidX Media3's Mp4Extractor and BoxParser
 * (libraries/extractor/.../mp4/, Apache-2.0, Copyright (C) 2016 The Android
 * Open Source Project): the moov box is read whole, every track's sample table
 * (stts, ctts, stsc, stsz / stz2, stco / co64, stss) is expanded into one
 * array of samples, the first edit-list entry shifts its times, and packets
 * are read across tracks in file order. A fragmented file's moof boxes
 * (tfhd, tfdt, trun, with the trex defaults from mvex) are read at open and
 * appended to the same arrays, so reading and seeking work the same way; as in
 * Media3's FragmentedMp4Extractor, a traf's data starts at its moof unless
 * tfhd gives a base offset. Live, unbounded files come with HLS / DASH.
 *
 * Licensed under the Apache License, Version 2.0. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "demux_internal.h"

#define MAX_TRACKS 16
#define MAX_MOOV (256u << 20)
#define MAX_MOOF (64u << 20)
#define MAX_SAMPLES 50000000u
#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

typedef struct {
    int64_t offset;
    uint32_t size;
    int64_t dts, pts;     /* media timescale, after the edit list */
    uint8_t key;
} sample;

typedef struct {
    int stream;           /* our stream index, -1 if unused */
    papaya_stream_info *info;
    uint32_t id;          /* tkhd track_ID, what fragments refer to */
    uint32_t timescale;
    sample *samples;
    uint32_t count, cap, next;
    int64_t edit_shift;   /* subtracted from every time */
    int has_edit;         /* the edit list placed the start */
    int64_t end_dts;      /* where the next fragment sample starts, before the shift */
    uint32_t trex_duration, trex_size, trex_flags; /* fragment defaults */
    uint8_t *extradata;
    size_t extradata_size;
    char tag[5];          /* sample entry fourcc */
} track;

typedef struct {
    papaya_io *io;
    track tracks[MAX_TRACKS];
    int track_count;
    uint32_t movie_timescale;
    int fragmented;       /* moov has mvex */
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
    if (count > MAX_SAMPLES || stsz.bad)
        return -1;
    if (count == 0)
        return 0; /* a fragmented file's samples are all in its fragments */
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
        t->end_dts = dts;
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
    rd mdia, mdhd, hdlr, minf, stbl, stsd, edts, elst, tkhd;
    uint32_t handler, timescale, version;
    int64_t edit_shift = 0, edit_end = 0;
    int has_edit = 0;
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
            has_edit = 1;
            if (m->movie_timescale && dur)
                edit_end = (int64_t)(dur * 1000000 / m->movie_timescale);
            break;
        }
    }

    t = &m->tracks[m->track_count];
    memset(t, 0, sizeof *t);
    t->stream = -1;
    t->timescale = timescale;
    t->edit_shift = edit_shift;
    t->has_edit = has_edit;
    if (find_box(trak, FOURCC('t', 'k', 'h', 'd'), &tkhd)) {
        uint32_t v = u8(&tkhd);

        skip(&tkhd, 3 + (v ? 16 : 8));
        t->id = u32(&tkhd);
    }
    s = papaya_demux_add_stream(d);
    if (!s)
        return;
    parse_stsd(stsd, t, s, handler);
    if (build_samples(stbl, t, edit_shift) != 0 || (t->count == 0 && !m->fragmented)) {
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
    if (t->count)
        s->duration = (t->samples[t->count - 1].pts + 1) * 1000000 / timescale;
    /* Audio past the edit is the encoder's padding. An empty edit before
     * it delays the track, and the end with it. */
    if (handler == FOURCC('s', 'o', 'u', 'n') && edit_end > 0)
        s->gapless_end = edit_end + (edit_shift < 0 ? -edit_shift * 1000000 / timescale : 0);
    t->stream = s->index;
    t->info = s;
    m->track_count++;
}

/* iTunes' gapless tag, moov/udta/meta/ilst/"----" named iTunSMPB: hex
 * fields " 00000000 <delay> <padding> <total samples> ...". Used for the first
 * AAC track when no edit list placed its start. */
static void
parse_itunsmpb(mp4 *m, rd moov)
{
    rd udta, meta, ilst, item, part;
    uint32_t type;
    char text[160];
    unsigned long long field[4];
    track *t = NULL;

    if (!find_box(moov, FOURCC('u', 'd', 't', 'a'), &udta) || !find_box(udta, FOURCC('m', 'e', 't', 'a'), &meta))
        return;
    if (left(&meta) >= 4 && !meta.p[0] && !meta.p[1] && !meta.p[2] && !meta.p[3])
        skip(&meta, 4); /* ISO meta is a full box; QuickTime's is not */
    if (!find_box(meta, FOURCC('i', 'l', 's', 't'), &ilst))
        return;
    for (int i = 0; i < m->track_count; i++)
        if (m->tracks[i].info && m->tracks[i].info->codec == PAPAYA_CODEC_AAC) {
            t = &m->tracks[i];
            break;
        }
    if (!t || t->has_edit || !t->info->sample_rate)
        return;
    while (next_box(&ilst, &type, &item)) {
        int named = 0;

        if (type != FOURCC('-', '-', '-', '-'))
            continue;
        while (next_box(&item, &type, &part)) {
            size_t n;

            if (type == FOURCC('n', 'a', 'm', 'e')) {
                skip(&part, 4);
                named = left(&part) == 8 && memcmp(part.p, "iTunSMPB", 8) == 0;
            } else if (type == FOURCC('d', 'a', 't', 'a') && named) {
                int64_t rate = t->info->sample_rate, delay;
                const uint8_t *asc = t->extradata;

                skip(&part, 8); /* type, locale */
                n = (size_t)left(&part);
                if (n >= sizeof text)
                    n = sizeof text - 1;
                memcpy(text, part.p, n);
                text[n] = 0;
                if (sscanf(text, "%llx %llx %llx %llx", &field[0], &field[1], &field[2], &field[3]) != 4)
                    return;
                /* The counts are output samples: twice the core rate with
                 * explicitly signalled SBR (HE-AAC, object type 5 or 29). */
                if (asc && t->extradata_size >= 1 && ((asc[0] >> 3) == 5 || (asc[0] >> 3) == 29))
                    rate *= 2;
                delay = (int64_t)(field[1] * t->timescale / (uint64_t)rate);
                if (delay <= 0 || field[3] == 0)
                    return;
                for (uint32_t s = 0; s < t->count; s++) {
                    t->samples[s].dts -= delay;
                    t->samples[s].pts -= delay;
                }
                t->edit_shift += delay;
                t->info->gapless_end = (int64_t)(field[3] * 1000000 / (uint64_t)rate);
                t->info->duration = t->info->gapless_end;
                return;
            }
        }
    }
}

/* ---- top-level boxes ---------------------------------------------------------- */

/* The box at pos: its type, whole size and header size. -1 at the end. */
static int
top_box(papaya_io *io, int64_t pos, int64_t size, uint32_t *type, uint64_t *box, uint32_t *header)
{
    uint8_t h[16];

    if ((size >= 0 && pos + 8 > size) || papaya_io_read_at(io, pos, h, 8) != 0)
        return -1;
    *box = (uint64_t)h[0] << 24 | (uint64_t)h[1] << 16 | (uint64_t)h[2] << 8 | h[3];
    *type = FOURCC(h[4], h[5], h[6], h[7]);
    *header = 8;
    if (*box == 1) {
        if (papaya_io_read_at(io, pos + 8, h + 8, 8) != 0)
            return -1;
        *box = 0;
        for (int i = 8; i < 16; i++)
            *box = *box << 8 | h[i];
        *header = 16;
    } else if (*box == 0 && size >= 0) {
        *box = (uint64_t)(size - pos);
    }
    return *box < *header ? -1 : 0;
}

/* ---- fragments --------------------------------------------------------------- */

static track *
track_by_id(mp4 *m, uint32_t id)
{
    for (int i = 0; i < m->track_count; i++)
        if (m->tracks[i].id == id)
            return &m->tracks[i];
    return NULL;
}

/* mvex: each track's trex defaults. */
static void
parse_mvex(mp4 *m, rd mvex)
{
    uint32_t type;
    rd b;

    while (next_box(&mvex, &type, &b)) {
        track *t;

        if (type != FOURCC('t', 'r', 'e', 'x'))
            continue;
        skip(&b, 4);
        t = track_by_id(m, u32(&b));
        skip(&b, 4); /* sample description index */
        if (t && !b.bad) {
            t->trex_duration = u32(&b);
            t->trex_size = u32(&b);
            t->trex_flags = u32(&b);
        }
    }
}

static int
add_sample(track *t, int64_t offset, uint32_t size, int64_t dts, int64_t cts, uint32_t flags)
{
    sample *s;

    if (t->count == t->cap) {
        uint32_t cap = t->cap ? t->cap * 2 : 1024;
        sample *grown;

        if (cap > MAX_SAMPLES)
            return -1;
        grown = realloc(t->samples, sizeof *grown * cap);
        if (!grown)
            return -1;
        t->samples = grown;
        t->cap = cap;
    }
    s = &t->samples[t->count++];
    s->offset = offset;
    s->size = size;
    s->dts = dts - t->edit_shift;
    s->pts = dts + cts - t->edit_shift;
    s->key = (flags & 0x10000) == 0; /* sample_is_non_sync_sample */
    return 0;
}

/* One traf of the moof at moof_pos. */
static int
parse_traf(mp4 *m, rd traf, int64_t moof_pos)
{
    uint32_t type, tf, duration, size, flags;
    int64_t base, data;
    track *t;
    rd b;

    if (!find_box(traf, FOURCC('t', 'f', 'h', 'd'), &b))
        return 0;
    tf = u32(&b) & 0xffffff;
    t = track_by_id(m, u32(&b));
    if (!t || b.bad)
        return 0; /* a track we do not play */
    base = (tf & 0x1) ? (int64_t)u64(&b) : moof_pos;
    if (tf & 0x2)
        skip(&b, 4);
    duration = (tf & 0x8) ? u32(&b) : t->trex_duration;
    size = (tf & 0x10) ? u32(&b) : t->trex_size;
    flags = (tf & 0x20) ? u32(&b) : t->trex_flags;
    if (b.bad)
        return -1;
    if (find_box(traf, FOURCC('t', 'f', 'd', 't'), &b)) {
        uint32_t v = u8(&b);

        skip(&b, 3);
        t->end_dts = v ? (int64_t)u64(&b) : (int64_t)u32(&b);
    }

    /* Each trun's data follows the previous one's unless it gives an offset. */
    data = base;
    while (next_box(&traf, &type, &b)) {
        uint32_t head, version, rf, count, first = 0;

        if (type != FOURCC('t', 'r', 'u', 'n'))
            continue;
        head = u32(&b);
        version = head >> 24;
        rf = head & 0xffffff;
        count = u32(&b);
        if (rf & 0x1)
            data = base + (int32_t)u32(&b);
        if (rf & 0x4)
            first = u32(&b);
        if (b.bad || count > MAX_SAMPLES)
            return -1;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t d = (rf & 0x100) ? u32(&b) : duration;
            uint32_t n = (rf & 0x200) ? u32(&b) : size;
            uint32_t f = (rf & 0x400) ? u32(&b) : (i == 0 && (rf & 0x4)) ? first : flags;
            int64_t cts = 0;

            if (rf & 0x800)
                cts = version ? (int64_t)(int32_t)u32(&b) : (int64_t)u32(&b);
            if (b.bad || add_sample(t, data, n, t->end_dts, cts, f) != 0)
                return -1;
            data += n;
            t->end_dts += d;
        }
    }
    return 0;
}

/* Every top-level moof, in file order. */
static int
scan_fragments(mp4 *m, papaya_io *io)
{
    int64_t pos = 0, size = io->size(io->opaque);
    uint32_t type, header;
    uint64_t box;

    while (top_box(io, pos, size, &type, &box, &header) == 0) {
        if (type == FOURCC('m', 'o', 'o', 'f')) {
            uint8_t *buf;
            rd r, traf;
            uint32_t t;
            int err = 0;

            if (box - header > MAX_MOOF)
                return -1;
            buf = malloc((size_t)(box - header) + 1);
            if (!buf || papaya_io_read_at(io, pos + header, buf, (size_t)(box - header)) != 0) {
                free(buf);
                return -1;
            }
            r.p = buf;
            r.end = buf + (box - header);
            r.bad = 0;
            while (!err && next_box(&r, &t, &traf))
                if (t == FOURCC('t', 'r', 'a', 'f'))
                    err = parse_traf(m, traf, pos);
            free(buf);
            if (err)
                return -1;
        }
        pos += (int64_t)box;
    }
    return 0;
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

/* Reads the fragments, then sets the durations the moov could not know. */
static int
fragments_done(mp4 *m, papaya_demuxer *d, papaya_io *io)
{
    int64_t longest = 0;

    if (scan_fragments(m, io) != 0)
        return -1;
    for (int i = 0; i < m->track_count; i++) {
        track *t = &m->tracks[i];

        if (!t->count)
            continue;
        t->info->duration = (t->samples[t->count - 1].pts + 1) * 1000000 / t->timescale;
        if (t->info->duration > longest)
            longest = t->info->duration;
    }
    if (papaya_demux_duration(d) <= 0)
        papaya_demux_set_duration(d, longest);
    return 0;
}

static void *
mp4_open(papaya_demuxer *d, papaya_io *io)
{
    int64_t pos = 0, size = io->size(io->opaque);
    uint8_t *moov = NULL;
    size_t moov_size = 0;
    mp4 *m;

    uint32_t type, header;
    uint64_t box;

    /* Find moov among the top-level boxes (it may follow mdat). */
    while (top_box(io, pos, size, &type, &box, &header) == 0) {
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
        rd r = { moov, moov + moov_size, 0 }, b, mvex;

        m->fragmented = find_box(r, FOURCC('m', 'v', 'e', 'x'), &mvex);
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
        parse_itunsmpb(m, (rd){ moov, moov + moov_size, 0 });
        if (m->fragmented)
            parse_mvex(m, mvex);
    }
    free(moov);
    if (m->track_count == 0 || (m->fragmented && fragments_done(m, d, io) != 0)) {
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
