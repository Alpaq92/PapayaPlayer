#include <stdlib.h>
#include <string.h>

#include "demux_internal.h"

#define PROBE_SIZE 4096

static const papaya_demux_backend *const backends[] = {
    &papaya_demux_webm,
    &papaya_demux_ivf,
    &papaya_demux_mp4,
    &papaya_demux_wav,
    &papaya_demux_mp3,
    &papaya_demux_adts,
    &papaya_demux_flac,
    &papaya_demux_ogg,
    &papaya_demux_ts,
};

struct papaya_demuxer {
    const papaya_demux_backend *backend;
    void *ctx;
    papaya_io *io;
    papaya_stream_info streams[PAPAYA_DEMUX_MAX_STREAMS];
    int stream_count;
    int64_t duration;
    uint8_t *cover;
    size_t cover_size;
    int cover_front;
};

#define MAX_COVER (16 << 20)

static const char *const codec_names[] = {
    [PAPAYA_CODEC_UNKNOWN] = "unknown",
    [PAPAYA_CODEC_AV1] = "AV1",
    [PAPAYA_CODEC_VP8] = "VP8",
    [PAPAYA_CODEC_VP9] = "VP9",
    [PAPAYA_CODEC_H264] = "H.264",
    [PAPAYA_CODEC_HEVC] = "HEVC",
    [PAPAYA_CODEC_MPEG2] = "MPEG-2",
    [PAPAYA_CODEC_OPUS] = "Opus",
    [PAPAYA_CODEC_VORBIS] = "Vorbis",
    [PAPAYA_CODEC_FLAC] = "FLAC",
    [PAPAYA_CODEC_AAC] = "AAC",
    [PAPAYA_CODEC_MP3] = "MP3",
    [PAPAYA_CODEC_AC3] = "AC-3",
    [PAPAYA_CODEC_EAC3] = "E-AC-3",
    [PAPAYA_CODEC_PCM_S16LE] = "PCM s16le",
    [PAPAYA_CODEC_PCM_F32LE] = "PCM f32le",
    [PAPAYA_CODEC_PCM_U8] = "PCM u8",
    [PAPAYA_CODEC_PCM_S24LE] = "PCM s24le",
    [PAPAYA_CODEC_PCM_S32LE] = "PCM s32le",
    [PAPAYA_CODEC_PCM_F64LE] = "PCM f64le",
};

const char *
papaya_codec_name(papaya_codec codec)
{
    if ((unsigned)codec >= sizeof codec_names / sizeof codec_names[0] || !codec_names[codec])
        return "unknown";
    return codec_names[codec];
}

papaya_stream_info *
papaya_demux_add_stream(papaya_demuxer *d)
{
    papaya_stream_info *s;

    if (d->stream_count >= PAPAYA_DEMUX_MAX_STREAMS)
        return NULL;
    s = &d->streams[d->stream_count];
    memset(s, 0, sizeof *s);
    s->index = d->stream_count++;
    s->duration = PAPAYA_NOPTS;
    return s;
}

void
papaya_demux_set_duration(papaya_demuxer *d, int64_t duration)
{
    d->duration = duration;
}

void
papaya_demux_set_cover(papaya_demuxer *d, const uint8_t *data, size_t size, int front)
{
    uint8_t *copy;

    if (!size || size > MAX_COVER || (d->cover && (d->cover_front || !front)))
        return;
    copy = malloc(size);
    if (!copy)
        return;
    memcpy(copy, data, size);
    free(d->cover);
    d->cover = copy;
    d->cover_size = size;
    d->cover_front = front;
}

static uint32_t
be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

void
papaya_demux_flac_picture(papaya_demuxer *d, const uint8_t *p, size_t size)
{
    size_t at = 4, n;

    /* type, MIME, description, width, height, depth, colours, data */
    if (size < 32)
        return;
    n = be32(p + at);
    if (n > size - at - 4)
        return;
    at += 4 + n;
    if (size - at < 4)
        return;
    n = be32(p + at);
    if (n > size - at - 4)
        return;
    at += 4 + n;
    if (size - at < 20)
        return;
    n = be32(p + at + 16);
    at += 20;
    if (n > size - at)
        return;
    papaya_demux_set_cover(d, p + at, n, be32(p) == 3);
}

/* Where a text of encoding enc (ID3: 0 Latin-1, 1 UTF-16 with BOM, 2 UTF-16BE,
 * 3 UTF-8) starting at p ends, past its terminator; NULL if it runs past end. */
static const uint8_t *
id3_text_end(const uint8_t *p, const uint8_t *end, int enc)
{
    if (enc == 1 || enc == 2) {
        for (; end - p >= 2; p += 2)
            if (!p[0] && !p[1])
                return p + 2;
        return NULL;
    }
    for (; p < end; p++)
        if (!*p)
            return p + 1;
    return NULL;
}

/* Undoes ID3 unsynchronisation (FF 00 -> FF) in place; the new size. */
static size_t
id3_resync(uint8_t *p, size_t n)
{
    size_t out = 0;

    for (size_t i = 0; i < n; i++) {
        p[out++] = p[i];
        if (p[i] == 0xff && i + 1 < n && !p[i + 1])
            i++;
    }
    return out;
}

void
papaya_demux_id3_cover(papaya_demuxer *d, papaya_io *io, int64_t pos)
{
    uint8_t h[10], *tag;
    size_t size, at = 0;
    int version;

    if (papaya_io_read_at(io, pos, h, 10) != 0 || memcmp(h, "ID3", 3) != 0)
        return;
    version = h[3];
    size = (size_t)(h[6] & 127) << 21 | (size_t)(h[7] & 127) << 14 | (size_t)(h[8] & 127) << 7 | (h[9] & 127);
    if (version < 2 || version > 4 || !size || size > MAX_COVER || !(tag = malloc(size)))
        return;
    if (papaya_io_read_at(io, pos + 10, tag, size) != 0)
        goto done;
    if (version < 4 && (h[5] & 0x80)) /* the whole tag unsynchronised */
        size = id3_resync(tag, size);
    if (version == 3 && (h[5] & 0x40) && size >= 4) /* extended header */
        at = 4 + be32(tag);
    else if (version == 4 && (h[5] & 0x40) && size >= 4)
        at = (size_t)(tag[0] & 127) << 21 | (size_t)(tag[1] & 127) << 14 | (size_t)(tag[2] & 127) << 7 | (tag[3] & 127);
    while (at < size) {
        size_t head = version == 2 ? 6 : 10, n;
        uint8_t *f = tag + at, *body, *end, *p;
        int pic, enc;

        if (size - at < head || !f[0])
            break; /* padding */
        if (version == 2)
            n = (size_t)f[3] << 16 | (size_t)f[4] << 8 | f[5];
        else if (version == 3)
            n = be32(f + 4);
        else
            n = (size_t)(f[4] & 127) << 21 | (size_t)(f[5] & 127) << 14 | (size_t)(f[6] & 127) << 7 | (f[7] & 127);
        if (n > size - at - head)
            break;
        body = f + head;
        end = body + n;
        pic = version == 2 ? !memcmp(f, "PIC", 3) : !memcmp(f, "APIC", 4);
        if (pic && n > 4) {
            if (version == 4 && (f[9] & 0x02)) /* this frame unsynchronised */
                end = body + id3_resync(body, n);
            if (version == 4 && (f[9] & 0x01)) /* a data length first */
                body += 4;
            enc = body[0];
            /* v2.2: a three-letter format; later: a MIME type to its NUL */
            p = version == 2 ? body + 4 : (uint8_t *)id3_text_end(body + 1, end, 0);
            if (p && p < end) {
                int kind = *p++;

                p = (uint8_t *)id3_text_end(p, end, enc);
                if (p && p < end)
                    papaya_demux_set_cover(d, p, (size_t)(end - p), kind == 3);
            }
        }
        at += head + n;
    }
done:
    free(tag);
}

papaya_demuxer *
papaya_demux_open(papaya_io *io)
{
    uint8_t head[PROBE_SIZE];
    size_t got = 0;
    int scores[sizeof backends / sizeof backends[0]];
    papaya_demuxer *d;

    if (!io)
        return NULL;
    if (io->seek(io->opaque, 0) == 0) {
        while (got < sizeof head) {
            int64_t n = io->read(io->opaque, head + got, sizeof head - got);

            if (n <= 0)
                break;
            got += (size_t)n;
        }
    }
    for (size_t i = 0; i < sizeof backends / sizeof backends[0]; i++)
        scores[i] = backends[i]->probe(head, got);
    d = calloc(1, sizeof *d);
    if (!d) {
        papaya_io_close(&io);
        return NULL;
    }
    d->io = io;
    /* Best score first; a backend that cannot open the file passes it on
     * (an ID3 tag, say, opens both MP3 and ADTS files). */
    for (;;) {
        size_t best = 0;

        for (size_t i = 1; i < sizeof backends / sizeof backends[0]; i++)
            if (scores[i] > scores[best])
                best = i;
        if (scores[best] <= 0)
            break;
        scores[best] = 0;
        d->backend = backends[best];
        d->stream_count = 0;
        memset(d->streams, 0, sizeof d->streams);
        d->duration = PAPAYA_NOPTS;
        io->seek(io->opaque, 0);
        d->ctx = d->backend->open(d, io);
        if (d->ctx)
            return d;
    }
    papaya_io_close(&d->io);
    free(d);
    return NULL;
}

void
papaya_demux_close(papaya_demuxer **pd)
{
    papaya_demuxer *d = *pd;

    *pd = NULL;
    if (!d)
        return;
    d->backend->close(d->ctx);
    papaya_io_close(&d->io);
    free(d->cover);
    free(d);
}

const char *papaya_demux_name(const papaya_demuxer *d) { return d->backend->name; }
int papaya_demux_stream_count(const papaya_demuxer *d) { return d->stream_count; }
int64_t papaya_demux_duration(const papaya_demuxer *d) { return d->duration; }

const uint8_t *
papaya_demux_cover(const papaya_demuxer *d, size_t *size)
{
    *size = d->cover_size;
    return d->cover;
}

const papaya_stream_info *
papaya_demux_stream(const papaya_demuxer *d, int index)
{
    if (index < 0 || index >= d->stream_count)
        return NULL;
    return &d->streams[index];
}

int
papaya_demux_read(papaya_demuxer *d, papaya_packet **packet)
{
    *packet = NULL;
    return d->backend->read(d->ctx, packet);
}

int
papaya_demux_seek(papaya_demuxer *d, int64_t pts)
{
    return d->backend->seek(d->ctx, pts);
}
