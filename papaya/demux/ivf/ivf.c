/* IVF: the raw container libvpx and dav1d use for their test vectors. A
 * 32-byte header (fourcc, size, time base), then frames of size + pts + data.
 * IVF carries no keyframe flags, so seeking restarts from the first frame. */
#include <stdlib.h>
#include <string.h>

#include "demux_internal.h"

typedef struct {
    papaya_io *io;
    int64_t pos;
    uint32_t rate, scale;  /* pts * scale / rate seconds */
    int first;
} ivf;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int
ivf_probe(const uint8_t *head, size_t size)
{
    return size >= 32 && memcmp(head, "DKIF", 4) == 0 && rd16(head + 6) >= 32 ? 100 : 0;
}

static void *
ivf_open(papaya_demuxer *d, papaya_io *io)
{
    uint8_t h[32];
    papaya_stream_info *s;
    ivf *c;

    if (papaya_io_read_at(io, 0, h, sizeof h) != 0)
        return NULL;
    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->io = io;
    c->pos = rd16(h + 6);
    c->rate = rd32(h + 16);
    c->scale = rd32(h + 20);
    c->first = 1;
    if (!c->rate || !c->scale) {
        c->rate = 1000;
        c->scale = 1;
    }
    s = papaya_demux_add_stream(d);
    s->kind = PAPAYA_MEDIA_VIDEO;
    s->width = rd16(h + 12);
    s->height = rd16(h + 14);
    s->color.primaries = s->color.transfer = s->color.matrix = 2;
    if (memcmp(h + 8, "AV01", 4) == 0) {
        s->codec = PAPAYA_CODEC_AV1;
        s->codec_tag = "AV01";
    } else if (memcmp(h + 8, "VP90", 4) == 0) {
        s->codec = PAPAYA_CODEC_VP9;
        s->codec_tag = "VP90";
    } else if (memcmp(h + 8, "VP80", 4) == 0) {
        s->codec = PAPAYA_CODEC_VP8;
        s->codec_tag = "VP80";
    } else {
        s->codec_tag = "?";
    }
    return c;
}

static int
ivf_read(void *ctx, papaya_packet **packet)
{
    ivf *c = ctx;
    uint8_t fh[12];
    uint32_t size;
    uint64_t pts;
    papaya_packet *p;

    if (papaya_io_read_at(c->io, c->pos, fh, sizeof fh) != 0)
        return PAPAYA_DEMUX_EOF;
    size = rd32(fh);
    pts = (uint64_t)rd32(fh + 4) | (uint64_t)rd32(fh + 8) << 32;
    if (size == 0 || size > (256u << 20))
        return PAPAYA_DEMUX_ERROR;
    p = papaya_packet_new(NULL, size);
    if (!p)
        return PAPAYA_DEMUX_ERROR;
    if (papaya_io_read_at(c->io, c->pos + 12, papaya_buffer_data(p->buf), size) != 0) {
        papaya_packet_free(&p);
        return PAPAYA_DEMUX_ERROR;
    }
    c->pos += 12 + (int64_t)size;
    p->pts = (int64_t)(pts * c->scale * 1000000 / c->rate);
    p->stream = 0;
    if (c->first)
        p->flags |= PAPAYA_PACKET_KEY;
    c->first = 0;
    *packet = p;
    return 0;
}

static int
ivf_seek(void *ctx, int64_t pts)
{
    ivf *c = ctx;
    uint8_t h[8];

    (void)pts;
    if (papaya_io_read_at(c->io, 0, h, sizeof h) != 0)
        return PAPAYA_DEMUX_ERROR;
    c->pos = rd16(h + 6);
    c->first = 1;
    return 0;
}

static void
ivf_close(void *ctx)
{
    free(ctx);
}

const papaya_demux_backend papaya_demux_ivf = {
    "ivf", ivf_probe, ivf_open, ivf_read, ivf_seek, ivf_close,
};
