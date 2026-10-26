/* VP8 and VP9 through libvpx (BSD-3). Both decode one packet to at most one
 * shown frame, in order, so each output frame takes its packet's time.
 * Pictures are copied into our frame pool. */
#include <stdlib.h>
#include <string.h>

#include "decode_internal.h"
#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"

typedef struct {
    vpx_codec_ctx_t dec;
    papaya_frame_pool *pool;
} vpx_ctx;

static int
vpx_supports(const papaya_stream_info *s)
{
    return s->codec == PAPAYA_CODEC_VP8 || s->codec == PAPAYA_CODEC_VP9;
}

static void *
vpx_open_ctx(const papaya_stream_info *s, papaya_frame_pool *pool)
{
    vpx_codec_dec_cfg_t cfg = { .threads = 4, .w = 0, .h = 0 };
    vpx_ctx *c = calloc(1, sizeof *c);

    if (!c)
        return NULL;
    if (vpx_codec_dec_init(&c->dec, s->codec == PAPAYA_CODEC_VP9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx(),
                           &cfg, 0) != VPX_CODEC_OK) {
        free(c);
        return NULL;
    }
    c->pool = pool;
    return c;
}

/* ITU-T H.273 matrix coefficients for libvpx's colour space. */
static uint8_t
matrix_of(vpx_color_space_t cs)
{
    switch (cs) {
    case VPX_CS_BT_601:
    case VPX_CS_SMPTE_170: return 6;
    case VPX_CS_BT_709: return 1;
    case VPX_CS_SMPTE_240: return 7;
    case VPX_CS_BT_2020: return 9;
    case VPX_CS_SRGB: return 0;
    default: return 2;
    }
}

static papaya_frame *
to_frame(vpx_ctx *c, const vpx_image_t *img)
{
    int hbd = (img->fmt & VPX_IMG_FMT_HIGHBITDEPTH) != 0;
    int w = (int)img->d_w, h = (int)img->d_h, cw, ch;
    papaya_pixel_format pix;
    papaya_frame *f;

    switch (img->fmt & ~VPX_IMG_FMT_HIGHBITDEPTH) {
    case VPX_IMG_FMT_I420:
    case VPX_IMG_FMT_YV12:
        if (hbd && img->bit_depth != 10)
            return NULL;
        pix = hbd ? PAPAYA_PIX_I420_10 : PAPAYA_PIX_I420;
        break;
    case VPX_IMG_FMT_I422:
        pix = PAPAYA_PIX_I422;
        break;
    case VPX_IMG_FMT_I444:
        pix = PAPAYA_PIX_I444;
        break;
    default:
        return NULL;
    }
    if (hbd && pix != PAPAYA_PIX_I420_10)
        return NULL;
    cw = (w + (int)img->x_chroma_shift) >> img->x_chroma_shift;
    ch = (h + (int)img->y_chroma_shift) >> img->y_chroma_shift;
    f = papaya_frame_get_video(c->pool, pix, w, h);
    if (!f)
        return NULL;
    for (int p = 0; p < 3; p++) {
        int pw = p ? cw : w, ph = p ? ch : h;
        /* YV12 stores V before U. */
        int src = (img->fmt == VPX_IMG_FMT_YV12 && p) ? 3 - p : p;

        for (int y = 0; y < ph; y++)
            memcpy(f->planes[p] + (size_t)y * (size_t)f->strides[p],
                   img->planes[src] + (ptrdiff_t)y * img->stride[src], (size_t)pw << hbd);
    }
    f->color.matrix = matrix_of(img->cs);
    f->color.primaries = f->color.transfer = 2;
    f->color.full_range = img->range == VPX_CR_FULL_RANGE;
    return f;
}

static int
vpx_decode_packet(void *ctx, const papaya_packet *p, void (*emit)(void *, papaya_frame *), void *sink)
{
    vpx_ctx *c = ctx;
    vpx_codec_iter_t iter = NULL;
    vpx_image_t *img;

    if (vpx_codec_decode(&c->dec, p ? p->data : NULL, p ? (unsigned int)p->size : 0, NULL, 0)
        != VPX_CODEC_OK)
        return -1;
    while ((img = vpx_codec_get_frame(&c->dec, &iter)) != NULL) {
        papaya_frame *f = to_frame(c, img);

        if (!f)
            return -1;
        f->pts = p ? p->pts : PAPAYA_NOPTS;
        f->duration = p ? p->duration : PAPAYA_NOPTS;
        emit(sink, f);
    }
    return 0;
}

static void
vpx_flush_ctx(void *ctx)
{
    (void)ctx; /* libvpx keeps no frames back; the next keyframe resets it */
}

static void
vpx_close_ctx(void *ctx)
{
    vpx_ctx *c = ctx;

    vpx_codec_destroy(&c->dec);
    free(c);
}

const papaya_decode_backend papaya_decode_vpx = {
    "libvpx", vpx_supports, vpx_open_ctx, vpx_decode_packet, vpx_flush_ctx, vpx_close_ctx,
};
