/* Vorbis through libvorbis (BSD). The three header packets come Xiph-laced in
 * the stream's codec private data (Matroska's layout; the Ogg demuxer will
 * produce the same). */
#include <stdlib.h>
#include <string.h>

#include "decode_internal.h"
#include "vorbis/codec.h"

typedef struct {
    vorbis_info vi;
    vorbis_comment vc;
    vorbis_dsp_state vd;
    vorbis_block vb;
    papaya_frame_pool *pool;
    int64_t next_pts;
    ogg_int64_t packetno;
} vorbis_ctx;

static int
vorbis_supports(const papaya_stream_info *s)
{
    return s->codec == PAPAYA_CODEC_VORBIS;
}

/* Splits Xiph lacing: count-1, then the sizes of all but the last packet as
 * runs of 255s plus a remainder. Returns the number of packets or -1. */
static int
split_xiph(const uint8_t *data, size_t size, const uint8_t *out[3], size_t out_size[3])
{
    size_t pos = 1, sizes[2] = { 0, 0 }, used = 0;

    if (size < 3 || data[0] != 2)
        return -1;
    for (int i = 0; i < 2; i++) {
        for (;;) {
            if (pos >= size)
                return -1;
            sizes[i] += data[pos];
            if (data[pos++] != 255)
                break;
        }
    }
    for (int i = 0; i < 2; i++) {
        out[i] = data + pos + used;
        out_size[i] = sizes[i];
        used += sizes[i];
    }
    if (pos + used > size)
        return -1;
    out[2] = data + pos + used;
    out_size[2] = size - pos - used;
    return 3;
}

static void *
vorbis_open(const papaya_stream_info *s, papaya_frame_pool *pool)
{
    const uint8_t *hdr[3];
    size_t hdr_size[3];
    vorbis_ctx *c;

    if (!s->extradata || split_xiph(s->extradata, s->extradata_size, hdr, hdr_size) != 3)
        return NULL;
    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    vorbis_info_init(&c->vi);
    vorbis_comment_init(&c->vc);
    for (int i = 0; i < 3; i++) {
        ogg_packet op = { 0 };

        op.packet = (unsigned char *)hdr[i];
        op.bytes = (long)hdr_size[i];
        op.b_o_s = i == 0;
        op.packetno = i;
        if (vorbis_synthesis_headerin(&c->vi, &c->vc, &op) != 0)
            goto fail_info;
    }
    if (c->vi.channels < 1 || c->vi.channels > 8 || vorbis_synthesis_init(&c->vd, &c->vi) != 0)
        goto fail_info;
    vorbis_block_init(&c->vd, &c->vb);
    c->pool = pool;
    c->next_pts = PAPAYA_NOPTS;
    c->packetno = 3;
    return c;
fail_info:
    vorbis_comment_clear(&c->vc);
    vorbis_info_clear(&c->vi);
    free(c);
    return NULL;
}

static int
vorbis_decode_packet(void *ctx, const papaya_packet *p, void (*emit)(void *, papaya_frame *), void *sink)
{
    vorbis_ctx *c = ctx;
    ogg_packet op = { 0 };
    float **pcm;
    int n;

    if (!p)
        return 0;
    op.packet = (unsigned char *)p->data;
    op.bytes = (long)p->size;
    op.granulepos = -1;
    op.packetno = c->packetno++;
    if (vorbis_synthesis(&c->vb, &op) != 0 || vorbis_synthesis_blockin(&c->vd, &c->vb) != 0)
        return -1;
    if (p->pts != PAPAYA_NOPTS)
        c->next_pts = p->pts;
    while ((n = vorbis_synthesis_pcmout(&c->vd, &pcm)) > 0) {
        int ch = c->vi.channels;
        papaya_frame *f = papaya_frame_get_audio(c->pool, PAPAYA_SAMPLE_F32, ch, (int)c->vi.rate, n);
        float *out;

        if (!f)
            return -1;
        out = (float *)f->audio[0];
        for (int i = 0; i < n; i++)
            for (int k = 0; k < ch; k++)
                *out++ = pcm[k][i];
        papaya_reorder_vorbis_to_wave((float *)f->audio[0], n, ch);
        f->pts = c->next_pts;
        f->duration = (int64_t)n * 1000000 / c->vi.rate;
        if (c->next_pts != PAPAYA_NOPTS)
            c->next_pts += f->duration;
        vorbis_synthesis_read(&c->vd, n);
        emit(sink, f);
    }
    return 0;
}

static void
vorbis_flush(void *ctx)
{
    vorbis_ctx *c = ctx;

    vorbis_synthesis_restart(&c->vd);
    c->next_pts = PAPAYA_NOPTS;
}

static void
vorbis_close(void *ctx)
{
    vorbis_ctx *c = ctx;

    vorbis_block_clear(&c->vb);
    vorbis_dsp_clear(&c->vd);
    vorbis_comment_clear(&c->vc);
    vorbis_info_clear(&c->vi);
    free(c);
}

const papaya_decode_backend papaya_decode_vorbis = {
    "libvorbis", vorbis_supports, vorbis_open, vorbis_decode_packet, vorbis_flush, vorbis_close,
};
