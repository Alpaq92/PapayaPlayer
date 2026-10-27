/* Uncompressed PCM, little endian: unsigned 8-bit, signed 16 / 24 / 32-bit
 * and float 32 / 64-bit, interleaved in WAVE channel order. Converted to our
 * interleaved float. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "decode_internal.h"

typedef struct {
    papaya_frame_pool *pool;
    papaya_codec codec;
    int channels, rate, bytes;  /* bytes per sample */
} pcm_ctx;

static int
pcm_bytes(papaya_codec codec)
{
    switch (codec) {
    case PAPAYA_CODEC_PCM_U8: return 1;
    case PAPAYA_CODEC_PCM_S16LE: return 2;
    case PAPAYA_CODEC_PCM_S24LE: return 3;
    case PAPAYA_CODEC_PCM_S32LE:
    case PAPAYA_CODEC_PCM_F32LE: return 4;
    case PAPAYA_CODEC_PCM_F64LE: return 8;
    default: return 0;
    }
}

static int
pcm_supports(const papaya_stream_info *s)
{
    return pcm_bytes(s->codec) != 0;
}

static void *
pcm_open(const papaya_stream_info *s, papaya_frame_pool *pool)
{
    pcm_ctx *c;

    if (s->channels < 1 || s->channels > 8 || s->sample_rate <= 0)
        return NULL;
    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->pool = pool;
    c->codec = s->codec;
    c->channels = s->channels;
    c->rate = s->sample_rate;
    c->bytes = pcm_bytes(s->codec);
    return c;
}

static float
sample(const pcm_ctx *c, const uint8_t *p)
{
    switch (c->codec) {
    case PAPAYA_CODEC_PCM_U8:
        return (float)(p[0] - 128) * (1.0f / 128.0f);
    case PAPAYA_CODEC_PCM_S16LE:
        return (float)(int16_t)(p[0] | p[1] << 8) * (1.0f / 32768.0f);
    case PAPAYA_CODEC_PCM_S24LE:
        /* into the top of an int32, so the sign comes along */
        return (float)(int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24)
               * (1.0f / 2147483648.0f);
    case PAPAYA_CODEC_PCM_S32LE:
        return (float)(int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24)
               * (1.0f / 2147483648.0f);
    case PAPAYA_CODEC_PCM_F32LE: {
        uint32_t u = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        float f;

        memcpy(&f, &u, sizeof f);
        return f;
    }
    case PAPAYA_CODEC_PCM_F64LE: {
        uint64_t u = 0;
        double f;

        for (int i = 7; i >= 0; i--)
            u = u << 8 | p[i];
        memcpy(&f, &u, sizeof f);
        return (float)f;
    }
    default:
        return 0.0f;
    }
}

static int
pcm_decode_packet(void *ctx, const papaya_packet *p, void (*emit)(void *, papaya_frame *), void *sink)
{
    pcm_ctx *c = ctx;
    int frame_bytes = c->bytes * c->channels, n;
    papaya_frame *f;
    float *out;

    if (!p)
        return 0;
    n = (int)(p->size / (size_t)frame_bytes);
    if (n <= 0)
        return 0;
    f = papaya_frame_get_audio(c->pool, PAPAYA_SAMPLE_F32, c->channels, c->rate, n);
    if (!f)
        return -1;
    out = (float *)f->audio[0];
    for (int i = 0; i < n * c->channels; i++)
        out[i] = sample(c, p->data + (size_t)i * (size_t)c->bytes);
    f->pts = p->pts;
    f->duration = (int64_t)n * 1000000 / c->rate;
    emit(sink, f);
    return 0;
}

static void
pcm_flush(void *ctx)
{
    (void)ctx;
}

static void
pcm_close(void *ctx)
{
    free(ctx);
}

const papaya_decode_backend papaya_decode_pcm = {
    "pcm", pcm_supports, pcm_open, pcm_decode_packet, pcm_flush, pcm_close,
};
