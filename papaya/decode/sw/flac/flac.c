/* FLAC through libFLAC (BSD). libFLAC reads a byte stream, so the stream
 * header ("fLaC" + metadata, from the codec private data) and then each packet
 * are fed through its read callback, one frame per packet. */
#include <stdlib.h>
#include <string.h>

#include "FLAC/stream_decoder.h"
#include "decode_internal.h"

typedef struct {
    FLAC__StreamDecoder *dec;
    papaya_frame_pool *pool;
    const uint8_t *in;
    size_t in_size;
    void (*emit)(void *, papaya_frame *);
    void *sink;
    int64_t pts;
    int failed;
} flac_ctx;

static int
flac_supports(const papaya_stream_info *s)
{
    return s->codec == PAPAYA_CODEC_FLAC;
}

static FLAC__StreamDecoderReadStatus
on_read(const FLAC__StreamDecoder *dec, FLAC__byte buffer[], size_t *bytes, void *opaque)
{
    flac_ctx *c = opaque;
    size_t n = *bytes < c->in_size ? *bytes : c->in_size;

    (void)dec;
    if (n == 0) {
        *bytes = 0;
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT; /* wait for the next packet */
    }
    memcpy(buffer, c->in, n);
    c->in += n;
    c->in_size -= n;
    *bytes = n;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderWriteStatus
on_write(const FLAC__StreamDecoder *dec, const FLAC__Frame *frame, const FLAC__int32 *const buffer[],
         void *opaque)
{
    flac_ctx *c = opaque;
    int ch = (int)frame->header.channels, n = (int)frame->header.blocksize;
    float scale = 1.0f / (float)(1u << (frame->header.bits_per_sample - 1));
    papaya_frame *f;
    float *out;

    (void)dec;
    if (ch < 1 || ch > 8)
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    f = papaya_frame_get_audio(c->pool, PAPAYA_SAMPLE_F32, ch, (int)frame->header.sample_rate, n);
    if (!f)
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    out = (float *)f->audio[0];
    for (int i = 0; i < n; i++)
        for (int k = 0; k < ch; k++)
            *out++ = (float)buffer[k][i] * scale;
    f->pts = c->pts;
    f->duration = (int64_t)n * 1000000 / (int64_t)frame->header.sample_rate;
    if (c->pts != PAPAYA_NOPTS)
        c->pts += f->duration;
    c->emit(c->sink, f);
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void
on_error(const FLAC__StreamDecoder *dec, FLAC__StreamDecoderErrorStatus status, void *opaque)
{
    (void)dec;
    (void)status;
    ((flac_ctx *)opaque)->failed = 1;
}

static void
discard(void *sink, papaya_frame *frame)
{
    (void)sink;
    papaya_frame_unref(&frame);
}

static int
start(flac_ctx *c, const papaya_stream_info *s)
{
    if (FLAC__stream_decoder_init_stream(c->dec, on_read, NULL, NULL, NULL, NULL, on_write, NULL,
                                         on_error, c) != FLAC__STREAM_DECODER_INIT_STATUS_OK)
        return -1;
    c->in = s->extradata;
    c->in_size = s->extradata_size;
    c->emit = discard;
    return FLAC__stream_decoder_process_until_end_of_metadata(c->dec) ? 0 : -1;
}

static void *
flac_open(const papaya_stream_info *s, papaya_frame_pool *pool)
{
    flac_ctx *c;

    if (!s->extradata || s->extradata_size < 4 + 38 || memcmp(s->extradata, "fLaC", 4) != 0)
        return NULL;
    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->pool = pool;
    c->pts = PAPAYA_NOPTS;
    c->dec = FLAC__stream_decoder_new();
    if (!c->dec || start(c, s) != 0) {
        if (c->dec)
            FLAC__stream_decoder_delete(c->dec);
        free(c);
        return NULL;
    }
    return c;
}

static int
flac_decode_packet(void *ctx, const papaya_packet *p, void (*emit)(void *, papaya_frame *), void *sink)
{
    flac_ctx *c = ctx;

    if (!p)
        return 0;
    if (FLAC__stream_decoder_get_state(c->dec) == FLAC__STREAM_DECODER_ABORTED)
        FLAC__stream_decoder_flush(c->dec);
    c->in = p->data;
    c->in_size = p->size;
    c->emit = emit;
    c->sink = sink;
    c->failed = 0;
    if (p->pts != PAPAYA_NOPTS)
        c->pts = p->pts;
    FLAC__stream_decoder_process_single(c->dec);
    if (FLAC__stream_decoder_get_state(c->dec) == FLAC__STREAM_DECODER_ABORTED)
        FLAC__stream_decoder_flush(c->dec);
    return c->failed ? -1 : 0;
}

static void
flac_flush(void *ctx)
{
    flac_ctx *c = ctx;

    FLAC__stream_decoder_flush(c->dec);
    c->pts = PAPAYA_NOPTS;
}

static void
flac_close(void *ctx)
{
    flac_ctx *c = ctx;

    FLAC__stream_decoder_delete(c->dec);
    free(c);
}

const papaya_decode_backend papaya_decode_flac = {
    "libFLAC", flac_supports, flac_open, flac_decode_packet, flac_flush, flac_close,
};
