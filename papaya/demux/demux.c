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
};

struct papaya_demuxer {
    const papaya_demux_backend *backend;
    void *ctx;
    papaya_io *io;
    papaya_stream_info streams[PAPAYA_DEMUX_MAX_STREAMS];
    int stream_count;
    int64_t duration;
};

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
    free(d);
}

const char *papaya_demux_name(const papaya_demuxer *d) { return d->backend->name; }
int papaya_demux_stream_count(const papaya_demuxer *d) { return d->stream_count; }
int64_t papaya_demux_duration(const papaya_demuxer *d) { return d->duration; }

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
