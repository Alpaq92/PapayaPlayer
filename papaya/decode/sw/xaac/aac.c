/* AAC through libxaac (Apache-2.0): LC, HE-AAC v1/v2, xHE-AAC (USAC), LD and
 * ELD. The stream's AudioSpecificConfig (MP4 esds, Matroska CodecPrivate)
 * initializes the decoder in MP4 mode, then each packet is one raw access
 * unit. For AAC-LC and HE-AAC the decoder finishes its init only on the
 * first access unit, which it reads ahead of decoding without consuming.
 * libxaac's own flush resets most of its configuration, so a flush
 * builds a fresh decoder from the saved config instead, as Android does.
 * Gapless: samples before time 0 (the encoder's priming, placed there by an
 * edit list or iTunSMPB) are dropped, and so are those from the stream's
 * gapless_end on. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "decode_internal.h"
#include "ixheaac_error_standards.h"
#include "ixheaac_type_def.h"
#include "ixheaacd_aac_config.h"
#include "ixheaacd_apicmd_standards.h"
#include "ixheaacd_memory_standards.h"

/* libxaac declares its entry point only in its test program. */
IA_ERRORCODE ixheaacd_dec_api(pVOID p_ia_module_obj, WORD32 i_cmd, WORD32 i_idx, pVOID pv_value);

#define MAX_BLOCKS 16 /* the API object, the memory tables and its buffers */

typedef struct {
    pVOID api;
    void *blocks[MAX_BLOCKS];
    int nblocks;
    UWORD8 *in;
    int16_t *out; /* 16-bit interleaved PCM */
    WORD32 in_size;
    WORD32 init_done;
} xaac_dec;

typedef struct {
    xaac_dec d;
    papaya_frame_pool *pool;
    uint8_t *config;
    size_t config_size;
    int64_t next_pts;
    int64_t gapless_end; /* microseconds, 0 if none */
} aac_ctx;

static int
aac_supports(const papaya_stream_info *s)
{
    return s->codec == PAPAYA_CODEC_AAC;
}

static IA_ERRORCODE
api(xaac_dec *d, WORD32 cmd, WORD32 idx, void *value)
{
    return ixheaacd_dec_api(d->api, cmd, idx, value);
}

static int
fatal(IA_ERRORCODE e)
{
    return (e & IA_FATAL_ERROR) != 0;
}

/* An aligned block that dec_free releases. */
static void *
block(xaac_dec *d, WORD32 size, WORD32 align)
{
    uint8_t *p;

    if (d->nblocks == MAX_BLOCKS || size < 0 || align < 1)
        return NULL;
    p = calloc(1, (size_t)size + (size_t)align);
    if (!p)
        return NULL;
    d->blocks[d->nblocks++] = p;
    return p + ((size_t)align - (uintptr_t)p % (size_t)align) % (size_t)align;
}

static void
dec_free(xaac_dec *d)
{
    for (int i = 0; i < d->nblocks; i++)
        free(d->blocks[i]);
    memset(d, 0, sizeof *d);
}

static int
set(xaac_dec *d, WORD32 idx, UWORD32 value)
{
    return fatal(api(d, IA_API_CMD_SET_CONFIG_PARAM, idx, &value)) ? -1 : 0;
}

static UWORD32
get(xaac_dec *d, WORD32 idx)
{
    UWORD32 value = 0;

    if (api(d, IA_API_CMD_GET_CONFIG_PARAM, idx, &value) != IA_NO_ERROR)
        return 0;
    return value;
}

/* Feeds the input buffer's first size bytes to the init. */
static int
init_with(xaac_dec *d, WORD32 size)
{
    if (fatal(api(d, IA_API_CMD_SET_INPUT_BYTES, 0, &size)) ||
        fatal(api(d, IA_API_CMD_INIT, IA_CMD_TYPE_INIT_PROCESS, NULL)) ||
        fatal(api(d, IA_API_CMD_INIT, IA_CMD_TYPE_INIT_DONE_QUERY, &d->init_done)))
        return -1;
    return 0;
}

/* Creates the decoder and feeds it the AudioSpecificConfig. */
static int
dec_open(xaac_dec *d, const uint8_t *config, size_t config_size)
{
    WORD32 size = 0, n = 0;
    void *tabs;

    memset(d, 0, sizeof *d);
    if (fatal(ixheaacd_dec_api(NULL, IA_API_CMD_GET_API_SIZE, 0, &size)))
        goto fail;
    d->api = block(d, size, 4);
    if (!d->api || fatal(api(d, IA_API_CMD_INIT, IA_CMD_TYPE_INIT_API_PRE_CONFIG_PARAMS, NULL)))
        goto fail;
    if (set(d, IA_XHEAAC_DEC_CONFIG_PARAM_MP4FLAG, 1) || set(d, IA_XHEAAC_DEC_CONFIG_PARAM_PCM_WDSZ, 16) ||
        set(d, IA_XHEAAC_DEC_CONFIG_PARAM_MAX_CHANNEL, 8) ||
        set(d, IA_XHEAAC_DEC_CONFIG_ERROR_CONCEALMENT, 1))
        goto fail;

    /* The memory tables, then each buffer they ask for. */
    if (fatal(api(d, IA_API_CMD_GET_MEMTABS_SIZE, 0, &size)))
        goto fail;
    tabs = block(d, size, 4);
    if (!tabs || fatal(api(d, IA_API_CMD_SET_MEMTABS_PTR, 0, tabs)) ||
        fatal(api(d, IA_API_CMD_INIT, IA_CMD_TYPE_INIT_API_POST_CONFIG_PARAMS, NULL)) ||
        fatal(api(d, IA_API_CMD_GET_N_MEMTABS, 0, &n)))
        goto fail;
    for (WORD32 i = 0; i < n; i++) {
        WORD32 align = 0, type = 0;
        void *p;

        if (fatal(api(d, IA_API_CMD_GET_MEM_INFO_SIZE, i, &size)) ||
            fatal(api(d, IA_API_CMD_GET_MEM_INFO_ALIGNMENT, i, &align)) ||
            fatal(api(d, IA_API_CMD_GET_MEM_INFO_TYPE, i, &type)))
            goto fail;
        p = block(d, size, align);
        if (!p || fatal(api(d, IA_API_CMD_SET_MEM_PTR, i, p)))
            goto fail;
        if (type == IA_MEMTYPE_INPUT) {
            d->in = p;
            d->in_size = size;
        } else if (type == IA_MEMTYPE_OUTPUT)
            d->out = p;
    }
    if (!d->in || !d->out || config_size > (size_t)d->in_size)
        goto fail;

    memcpy(d->in, config, config_size);
    if (init_with(d, (WORD32)config_size) != 0)
        goto fail;
    return 0;
fail:
    dec_free(d);
    return -1;
}

static void *
aac_open(const papaya_stream_info *s, papaya_frame_pool *pool)
{
    aac_ctx *c;

    if (!s->extradata || s->extradata_size < 2)
        return NULL;
    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->pool = pool;
    c->next_pts = PAPAYA_NOPTS;
    c->gapless_end = s->gapless_end;
    c->config = malloc(s->extradata_size);
    if (!c->config)
        goto fail;
    memcpy(c->config, s->extradata, s->extradata_size);
    c->config_size = s->extradata_size;
    if (dec_open(&c->d, c->config, c->config_size) != 0)
        goto fail;
    return c;
fail:
    free(c->config);
    free(c);
    return NULL;
}

/* Microseconds to the nearest sample at rate. */
static int64_t
round_samples(int64_t us, int rate)
{
    return (us * rate + (us < 0 ? -500000 : 500000)) / 1000000;
}

/* The decoder's output, trimmed for gapless, as one frame. */
static int
emit_pcm(aac_ctx *c, WORD32 out_bytes, void (*emit)(void *, papaya_frame *), void *sink)
{
    int rate = (int)get(&c->d, IA_XHEAAC_DEC_CONFIG_PARAM_SAMP_FREQ);
    int channels = (int)get(&c->d, IA_XHEAAC_DEC_CONFIG_PARAM_NUM_CHANNELS);
    int n, skip, keep;
    int64_t start, end;
    papaya_frame *f;
    const int16_t *in;
    float *out;

    if (rate <= 0 || channels < 1 || channels > 8)
        return -1;
    n = out_bytes / (int)sizeof(int16_t) / channels;
    if (n <= 0)
        return 0;
    /* Whole samples, so the trims are exact. */
    start = c->next_pts == PAPAYA_NOPTS ? 0 : round_samples(c->next_pts, rate);
    end = c->gapless_end > 0 ? round_samples(c->gapless_end, rate) : INT64_MAX;
    skip = start < 0 ? (int)(-start < n ? -start : n) : 0;
    keep = n - skip;
    if (c->next_pts != PAPAYA_NOPTS)
        c->next_pts += (int64_t)n * 1000000 / rate;
    if (start + n > end)
        keep = end - start > skip ? (int)(end - start - skip) : 0;
    if (keep <= 0)
        return 0;
    f = papaya_frame_get_audio(c->pool, PAPAYA_SAMPLE_F32, channels, rate, keep);
    if (!f)
        return -1;
    in = c->d.out + (size_t)skip * (size_t)channels;
    out = (float *)f->audio[0];
    for (int i = 0; i < keep * channels; i++)
        out[i] = (float)in[i] * (1.0f / 32768.0f);
    f->pts = c->next_pts == PAPAYA_NOPTS ? PAPAYA_NOPTS : (start + skip) * 1000000 / rate;
    f->duration = (int64_t)keep * 1000000 / rate;
    emit(sink, f);
    return 0;
}

/* A packet may hold several raw data blocks (an ADTS frame with more than
 * one): each call decodes one, until the packet is used up. */
static int
aac_decode_packet(void *ctx, const papaya_packet *p, void (*emit)(void *, papaya_frame *), void *sink)
{
    aac_ctx *c = ctx;
    WORD32 size;

    if (!p)
        return 0; /* nothing held back */
    if (!c->d.api || p->size == 0 || p->size > (size_t)c->d.in_size)
        return -1;
    memcpy(c->d.in, p->data, p->size);
    size = (WORD32)p->size;
    if (!c->d.init_done) {
        WORD32 used = 0;

        if (init_with(&c->d, size) != 0 || fatal(api(&c->d, IA_API_CMD_GET_CURIDX_INPUT_BUF, 0, &used)))
            return -1;
        if (!c->d.init_done)
            return 0;
        if (used > 0 && used < size)
            memmove(c->d.in, c->d.in + used, (size_t)(size - used));
        size -= used;
        if (size <= 0)
            return 0;
    }
    if (p->pts != PAPAYA_NOPTS)
        c->next_pts = p->pts;
    for (int blocks = 0; size > 0 && blocks < 8; blocks++) {
        WORD32 out_bytes = 0, used = 0;

        if (fatal(api(&c->d, IA_API_CMD_SET_INPUT_BYTES, 0, &size)) ||
            fatal(api(&c->d, IA_API_CMD_EXECUTE, IA_CMD_TYPE_DO_EXECUTE, NULL)) ||
            fatal(api(&c->d, IA_API_CMD_GET_OUTPUT_BYTES, 0, &out_bytes)) ||
            fatal(api(&c->d, IA_API_CMD_GET_CURIDX_INPUT_BUF, 0, &used)))
            return -1;
        if (emit_pcm(c, out_bytes, emit, sink) != 0)
            return -1;
        /* What is left must be another block, not the byte alignment's
         * padding: at least a few bytes. */
        if (used <= 0 || used >= size - 2)
            break;
        memmove(c->d.in, c->d.in + used, (size_t)(size - used));
        size -= used;
    }
    return 0;
}

static void
aac_flush(void *ctx)
{
    aac_ctx *c = ctx;

    dec_free(&c->d);
    dec_open(&c->d, c->config, c->config_size); /* on failure, decode reports it */
    c->next_pts = PAPAYA_NOPTS;
}

static void
aac_close(void *ctx)
{
    aac_ctx *c = ctx;

    dec_free(&c->d);
    free(c->config);
    free(c);
}

const papaya_decode_backend papaya_decode_aac = {
    "libxaac", aac_supports, aac_open, aac_decode_packet, aac_flush, aac_close,
};
