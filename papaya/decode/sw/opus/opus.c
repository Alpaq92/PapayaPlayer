/* Opus through libopus (BSD). The stream's OpusHead (Matroska CodecPrivate,
 * or the first Ogg packet) gives channels, pre-skip, gain and channel mapping;
 * every mapping family goes through the multistream decoder. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "decode_internal.h"
#include "opus_multistream.h"

#define OPUS_RATE 48000
#define MAX_FRAME (OPUS_RATE * 120 / 1000) /* 120 ms */

typedef struct {
    OpusMSDecoder *dec;
    papaya_frame_pool *pool;
    int channels, family;
    int preskip, to_skip;
    float gain;
    int64_t next_pts;
} opus_ctx;

static int
opus_supports(const papaya_stream_info *s)
{
    return s->codec == PAPAYA_CODEC_OPUS;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void *
opus_open(const papaya_stream_info *s, papaya_frame_pool *pool)
{
    const uint8_t *h = s->extradata;
    unsigned char mapping[255];
    int streams, coupled, err;
    opus_ctx *c;

    /* OpusHead: magic, version, channels, pre-skip, rate, gain, family, ... */
    if (!h || s->extradata_size < 19 || memcmp(h, "OpusHead", 8) != 0 || (h[8] >> 4) != 0)
        return NULL;
    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->pool = pool;
    c->channels = h[9];
    c->preskip = rd16(h + 10);
    c->gain = (float)(int16_t)rd16(h + 16);
    c->family = h[18];
    if (c->channels < 1 || c->channels > 8)
        goto fail;
    if (c->family == 0) {
        if (c->channels > 2)
            goto fail;
        streams = 1;
        coupled = c->channels - 1;
        mapping[0] = 0;
        mapping[1] = 1;
    } else {
        if (s->extradata_size < 21u + (size_t)c->channels)
            goto fail;
        streams = h[19];
        coupled = h[20];
        memcpy(mapping, h + 21, (size_t)c->channels);
    }
    c->dec = opus_multistream_decoder_create(OPUS_RATE, c->channels, streams, coupled, mapping, &err);
    if (!c->dec || err != OPUS_OK)
        goto fail;
    if (c->gain != 0.0f)
        opus_multistream_decoder_ctl(c->dec, OPUS_SET_GAIN((int)c->gain));
    c->to_skip = c->preskip;
    c->next_pts = PAPAYA_NOPTS;
    return c;
fail:
    if (c->dec)
        opus_multistream_decoder_destroy(c->dec);
    free(c);
    return NULL;
}

static int
opus_decode_packet(void *ctx, const papaya_packet *p, void (*emit)(void *, papaya_frame *), void *sink)
{
    opus_ctx *c = ctx;
    papaya_frame *f;
    int n, skip;

    if (!p)
        return 0; /* Opus keeps no frames back */
    f = papaya_frame_get_audio(c->pool, PAPAYA_SAMPLE_F32, c->channels, OPUS_RATE, MAX_FRAME);
    if (!f)
        return -1;
    n = opus_multistream_decode_float(c->dec, p->data, (opus_int32)p->size, (float *)f->audio[0],
                                      MAX_FRAME, 0);
    if (n < 0) {
        papaya_frame_unref(&f);
        return -1;
    }
    if (p->pts != PAPAYA_NOPTS)
        c->next_pts = p->pts;
    skip = c->to_skip < n ? c->to_skip : n;
    c->to_skip -= skip;
    if (skip)
        memmove(f->audio[0], (float *)f->audio[0] + (size_t)skip * (size_t)c->channels,
                sizeof(float) * (size_t)(n - skip) * (size_t)c->channels);
    f->samples = n - skip;
    f->pts = c->next_pts;
    f->duration = (int64_t)f->samples * 1000000 / OPUS_RATE;
    if (c->next_pts != PAPAYA_NOPTS)
        c->next_pts += (int64_t)n * 1000000 / OPUS_RATE;
    if (c->family == 1)
        papaya_reorder_vorbis_to_wave((float *)f->audio[0], f->samples, c->channels);
    if (f->samples == 0) {
        papaya_frame_unref(&f);
        return 0;
    }
    emit(sink, f);
    return 0;
}

static void
opus_flush(void *ctx)
{
    opus_ctx *c = ctx;

    opus_multistream_decoder_ctl(c->dec, OPUS_RESET_STATE);
    c->next_pts = PAPAYA_NOPTS;
    c->to_skip = 0;
}

static void
opus_close(void *ctx)
{
    opus_ctx *c = ctx;

    opus_multistream_decoder_destroy(c->dec);
    free(c);
}

const papaya_decode_backend papaya_decode_opus = {
    "libopus", opus_supports, opus_open, opus_decode_packet, opus_flush, opus_close,
};
