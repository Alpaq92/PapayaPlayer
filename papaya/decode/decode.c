#include <stdlib.h>
#include <string.h>

#include "decode_internal.h"

static const papaya_decode_backend *const backends[] = {
    &papaya_decode_opus,
    &papaya_decode_vorbis,
    &papaya_decode_flac,
    &papaya_decode_dav1d,
    &papaya_decode_vpx,
    &papaya_decode_avc,
    &papaya_decode_hevc,
};

#define MAX_PENDING 64

struct papaya_decoder {
    const papaya_decode_backend *backend;
    void *ctx;
    papaya_frame *pending[MAX_PENDING];
    int head, count;
    int draining, drained;
};

papaya_decoder *
papaya_decoder_open(const papaya_stream_info *stream, papaya_frame_pool *pool)
{
    for (size_t i = 0; i < sizeof backends / sizeof backends[0]; i++) {
        papaya_decoder *d;

        if (!backends[i]->supports(stream))
            continue;
        d = calloc(1, sizeof *d);
        if (!d)
            return NULL;
        d->backend = backends[i];
        d->ctx = backends[i]->open(stream, pool);
        if (d->ctx)
            return d;
        free(d);
    }
    return NULL;
}

static void
drop_pending(papaya_decoder *d)
{
    while (d->count) {
        papaya_frame_unref(&d->pending[d->head]);
        d->head = (d->head + 1) % MAX_PENDING;
        d->count--;
    }
    d->head = 0;
}

void
papaya_decoder_close(papaya_decoder **pd)
{
    papaya_decoder *d = *pd;

    *pd = NULL;
    if (!d)
        return;
    drop_pending(d);
    d->backend->close(d->ctx);
    free(d);
}

const char *
papaya_decoder_name(const papaya_decoder *d)
{
    return d->backend->name;
}

static void
emit(void *sink, papaya_frame *frame)
{
    papaya_decoder *d = sink;

    if (d->count == MAX_PENDING) {
        papaya_frame_unref(&frame); /* the caller is not receiving; drop */
        return;
    }
    d->pending[(d->head + d->count) % MAX_PENDING] = frame;
    d->count++;
}

int
papaya_decoder_send(papaya_decoder *d, const papaya_packet *packet)
{
    int r;

    if (d->draining)
        return PAPAYA_DECODE_EOF;
    if (!packet)
        d->draining = 1;
    r = d->backend->decode(d->ctx, packet, emit, d);
    if (!packet)
        d->drained = 1;
    return r < 0 ? PAPAYA_DECODE_ERROR : PAPAYA_DECODE_OK;
}

int
papaya_decoder_receive(papaya_decoder *d, papaya_frame **frame)
{
    *frame = NULL;
    if (!d->count)
        return d->drained ? PAPAYA_DECODE_EOF : PAPAYA_DECODE_AGAIN;
    *frame = d->pending[d->head];
    d->head = (d->head + 1) % MAX_PENDING;
    d->count--;
    return PAPAYA_DECODE_OK;
}

void
papaya_decoder_flush(papaya_decoder *d)
{
    drop_pending(d);
    d->draining = d->drained = 0;
    if (d->backend->flush)
        d->backend->flush(d->ctx);
}

/* Vorbis order -> WAVE order, as source index per output channel. */
static const unsigned char vorbis_to_wave[9][8] = {
    { 0 },
    { 0 },
    { 0, 1 },
    { 0, 2, 1 },
    { 0, 1, 2, 3 },
    { 0, 2, 1, 3, 4 },
    { 0, 2, 1, 5, 3, 4 },
    { 0, 2, 1, 6, 5, 3, 4 },
    { 0, 2, 1, 7, 5, 6, 3, 4 },
};

void
papaya_reorder_vorbis_to_wave(float *samples, int frames, int channels)
{
    float tmp[8];

    if (channels < 3 || channels > 8 || channels == 4)
        return;
    for (int i = 0; i < frames; i++, samples += channels) {
        memcpy(tmp, samples, sizeof(float) * (size_t)channels);
        for (int c = 0; c < channels; c++)
            samples[c] = tmp[vorbis_to_wave[channels][c]];
    }
}
