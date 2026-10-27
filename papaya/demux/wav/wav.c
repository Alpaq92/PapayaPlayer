/* WAV (RIFF / WAVE): the fmt chunk says how samples are stored, the data
 * chunk holds them. Integer PCM of 8 to 32 bits and IEEE float of 32 or 64,
 * plain or WAVE_FORMAT_EXTENSIBLE. Packets are runs of whole sample frames,
 * so seeking is exact. */
#include <stdlib.h>
#include <string.h>

#include "demux_internal.h"

#define PACKET_FRAMES 4096

typedef struct {
    papaya_io *io;
    int64_t data, data_end;   /* the samples, in bytes */
    int64_t pos;
    int block_align, rate;
} wav;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int
wav_probe(const uint8_t *head, size_t size)
{
    return size >= 12 && memcmp(head, "RIFF", 4) == 0 && memcmp(head + 8, "WAVE", 4) == 0 ? 100 : 0;
}

/* The codec for a format tag and sample width, or UNKNOWN. */
static papaya_codec
pcm_codec(uint16_t tag, int bits)
{
    if (tag == 1) {
        switch (bits) {
        case 8: return PAPAYA_CODEC_PCM_U8;
        case 16: return PAPAYA_CODEC_PCM_S16LE;
        case 24: return PAPAYA_CODEC_PCM_S24LE;
        case 32: return PAPAYA_CODEC_PCM_S32LE;
        default: return PAPAYA_CODEC_UNKNOWN;
        }
    }
    if (tag == 3)
        return bits == 32 ? PAPAYA_CODEC_PCM_F32LE : bits == 64 ? PAPAYA_CODEC_PCM_F64LE : PAPAYA_CODEC_UNKNOWN;
    return PAPAYA_CODEC_UNKNOWN;
}

static void *
wav_open(papaya_demuxer *d, papaya_io *io)
{
    int64_t pos = 12, size = io->size(io->opaque);
    uint8_t fmt[40];
    int have_fmt = 0;
    papaya_stream_info *s;
    wav *c;

    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->io = io;
    /* Chunks in any order; data is the last one we need. */
    for (;;) {
        uint8_t h[8];
        uint32_t len;

        if (papaya_io_read_at(io, pos, h, 8) != 0)
            goto fail;
        len = rd32(h + 4);
        if (memcmp(h, "fmt ", 4) == 0 && len >= 16) {
            memset(fmt, 0, sizeof fmt);
            if (papaya_io_read_at(io, pos + 8, fmt, len < sizeof fmt ? len : sizeof fmt) != 0)
                goto fail;
            have_fmt = 1;
        } else if (memcmp(h, "data", 4) == 0) {
            c->data = pos + 8;
            /* A streamed file may say 0 or 0xffffffff: then up to the end. */
            c->data_end = (len == 0 || len == 0xffffffffu || (size >= 0 && c->data + len > size))
                              ? size
                              : c->data + len;
            break;
        }
        pos += 8 + (int64_t)len + (len & 1);
    }
    if (!have_fmt || c->data_end <= c->data)
        goto fail;

    s = papaya_demux_add_stream(d);
    if (!s)
        goto fail;
    {
        uint16_t tag = rd16(fmt);

        s->kind = PAPAYA_MEDIA_AUDIO;
        s->channels = rd16(fmt + 2);
        s->sample_rate = (int)rd32(fmt + 4);
        c->block_align = rd16(fmt + 12);
        s->bits_per_sample = rd16(fmt + 14);
        if (tag == 0xFFFE) /* WAVE_FORMAT_EXTENSIBLE: the real tag opens the subformat GUID */
            tag = rd16(fmt + 24);
        s->codec = pcm_codec(tag, s->bits_per_sample);
        s->codec_tag = "WAVE";
    }
    if (s->channels < 1 || s->channels > 8 || s->sample_rate <= 0
        || c->block_align != s->channels * (s->bits_per_sample / 8))
        goto fail;
    c->rate = s->sample_rate;
    c->pos = c->data;
    if (c->data_end > 0) {
        s->duration = (c->data_end - c->data) / c->block_align * 1000000 / c->rate;
        papaya_demux_set_duration(d, s->duration);
    }
    return c;
fail:
    free(c);
    return NULL;
}

static int
wav_read(void *ctx, papaya_packet **packet)
{
    wav *c = ctx;
    int64_t n = (int64_t)PACKET_FRAMES * c->block_align;
    papaya_packet *p;

    if (c->data_end >= 0 && c->pos + c->block_align > c->data_end)
        return PAPAYA_DEMUX_EOF;
    if (c->data_end >= 0 && c->pos + n > c->data_end)
        n = (c->data_end - c->pos) / c->block_align * c->block_align;
    p = papaya_packet_new(NULL, (size_t)n);
    if (!p)
        return PAPAYA_DEMUX_ERROR;
    if (papaya_io_read_at(c->io, c->pos, papaya_buffer_data(p->buf), (size_t)n) != 0) {
        papaya_packet_free(&p);
        return PAPAYA_DEMUX_EOF; /* a cut-off file ends here */
    }
    p->stream = 0;
    p->pts = p->dts = (c->pos - c->data) / c->block_align * 1000000 / c->rate;
    p->duration = n / c->block_align * 1000000 / c->rate;
    p->flags |= PAPAYA_PACKET_KEY;
    c->pos += n;
    *packet = p;
    return 0;
}

static int
wav_seek(void *ctx, int64_t pts)
{
    wav *c = ctx;
    int64_t frame = pts > 0 ? pts * c->rate / 1000000 : 0;

    c->pos = c->data + frame * c->block_align;
    if (c->data_end >= 0 && c->pos > c->data_end)
        c->pos = c->data_end;
    return 0;
}

static void
wav_close(void *ctx)
{
    free(ctx);
}

const papaya_demux_backend papaya_demux_wav = {
    "wav", wav_probe, wav_open, wav_read, wav_seek, wav_close,
};
