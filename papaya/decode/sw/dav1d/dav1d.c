/* AV1 through dav1d (BSD-2). Pictures are copied into our frame pool for now;
 * handing dav1d our own picture allocator (zero copy) comes later. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "dav1d/dav1d.h"
#include "decode_internal.h"

typedef struct {
    Dav1dContext *dec;
    papaya_frame_pool *pool;
} dav1d_ctx;

static int
dav1d_supports(const papaya_stream_info *s)
{
    return s->codec == PAPAYA_CODEC_AV1;
}

static void *
dav1d_open_ctx(const papaya_stream_info *s, papaya_frame_pool *pool)
{
    Dav1dSettings settings;
    dav1d_ctx *c;

    (void)s;
    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    dav1d_default_settings(&settings);
    settings.n_threads = 0;        /* one per core */
    settings.max_frame_delay = 0;  /* dav1d's choice for that thread count */
    settings.logger.callback = NULL;
    if (dav1d_open(&c->dec, &settings) < 0) {
        free(c);
        return NULL;
    }
    c->pool = pool;
    return c;
}

static void
copy_plane(uint8_t *dst, int dst_stride, const uint8_t *src, ptrdiff_t src_stride, size_t row_bytes, int rows)
{
    for (int y = 0; y < rows; y++)
        memcpy(dst + (size_t)y * (size_t)dst_stride, src + (ptrdiff_t)y * src_stride, row_bytes);
}

static papaya_frame *
to_frame(dav1d_ctx *c, const Dav1dPicture *p)
{
    int hbd = p->p.bpc > 8, w = p->p.w, h = p->p.h, cw, ch;
    papaya_pixel_format pix;
    papaya_frame *f;

    switch (p->p.layout) {
    case DAV1D_PIXEL_LAYOUT_I400:
    case DAV1D_PIXEL_LAYOUT_I420:
        pix = hbd ? PAPAYA_PIX_I420_10 : PAPAYA_PIX_I420;
        cw = (w + 1) >> 1;
        ch = (h + 1) >> 1;
        break;
    case DAV1D_PIXEL_LAYOUT_I422:
        if (hbd)
            return NULL;
        pix = PAPAYA_PIX_I422;
        cw = (w + 1) >> 1;
        ch = h;
        break;
    default:
        if (hbd)
            return NULL;
        pix = PAPAYA_PIX_I444;
        cw = w;
        ch = h;
        break;
    }
    f = papaya_frame_get_video(c->pool, pix, w, h);
    if (!f)
        return NULL;
    copy_plane(f->planes[0], f->strides[0], p->data[0], p->stride[0], (size_t)w << hbd, h);
    if (p->p.layout == DAV1D_PIXEL_LAYOUT_I400) {
        for (int pl = 1; pl <= 2; pl++)
            for (int y = 0; y < ch; y++) {
                if (hbd) {
                    uint16_t *row = (uint16_t *)(f->planes[pl] + (size_t)y * (size_t)f->strides[pl]);

                    for (int x = 0; x < cw; x++)
                        row[x] = (uint16_t)(1 << (p->p.bpc - 1));
                } else {
                    memset(f->planes[pl] + (size_t)y * (size_t)f->strides[pl], 128, (size_t)cw);
                }
            }
    } else {
        copy_plane(f->planes[1], f->strides[1], p->data[1], p->stride[1], (size_t)cw << hbd, ch);
        copy_plane(f->planes[2], f->strides[2], p->data[2], p->stride[1], (size_t)cw << hbd, ch);
    }
    f->pts = p->m.timestamp == INT64_MIN ? PAPAYA_NOPTS : p->m.timestamp;
    f->duration = p->m.duration > 0 ? p->m.duration : PAPAYA_NOPTS;
    if (p->seq_hdr) {
        f->color.primaries = (uint8_t)p->seq_hdr->pri;
        f->color.transfer = (uint8_t)p->seq_hdr->trc;
        f->color.matrix = (uint8_t)p->seq_hdr->mtrx;
        f->color.full_range = (uint8_t)p->seq_hdr->color_range;
    }
    if (p->content_light) {
        f->color.max_cll = p->content_light->max_content_light_level;
        f->color.max_fall = p->content_light->max_frame_average_light_level;
    }
    if (p->mastering_display) {
        /* Luminance in 24.8 and 18.14 fixed point. */
        f->color.max_luminance = (float)p->mastering_display->max_luminance / 256.0f;
        f->color.min_luminance = (float)p->mastering_display->min_luminance / 16384.0f;
    }
    return f;
}

/* Emits every picture dav1d has ready. Returns < 0 on a decoding error. */
static int
drain_pictures(dav1d_ctx *c, void (*emit)(void *, papaya_frame *), void *sink)
{
    for (;;) {
        Dav1dPicture pic = { 0 };
        papaya_frame *f;
        int r = dav1d_get_picture(c->dec, &pic);

        if (r == DAV1D_ERR(EAGAIN))
            return 0;
        if (r < 0)
            return r;
        f = to_frame(c, &pic);
        dav1d_picture_unref(&pic);
        if (!f)
            return -1;
        emit(sink, f);
    }
}

static int
dav1d_decode_packet(void *ctx, const papaya_packet *p, void (*emit)(void *, papaya_frame *), void *sink)
{
    dav1d_ctx *c = ctx;
    Dav1dData data = { 0 };
    uint8_t *buf;

    if (!p)
        return drain_pictures(c, emit, sink); /* dav1d returns what it still holds */
    buf = dav1d_data_create(&data, p->size);
    if (!buf)
        return -1;
    memcpy(buf, p->data, p->size);
    data.m.timestamp = p->pts == PAPAYA_NOPTS ? INT64_MIN : p->pts;
    data.m.duration = p->duration == PAPAYA_NOPTS ? 0 : p->duration;
    while (data.sz > 0) {
        int r = dav1d_send_data(c->dec, &data);

        if (r < 0 && r != DAV1D_ERR(EAGAIN)) {
            dav1d_data_unref(&data);
            return -1;
        }
        if (drain_pictures(c, emit, sink) < 0) {
            dav1d_data_unref(&data);
            return -1;
        }
    }
    return 0;
}

static void
dav1d_flush_ctx(void *ctx)
{
    dav1d_flush(((dav1d_ctx *)ctx)->dec);
}

static void
dav1d_close_ctx(void *ctx)
{
    dav1d_ctx *c = ctx;

    dav1d_close(&c->dec);
    free(c);
}

const papaya_decode_backend papaya_decode_dav1d = {
    "dav1d", dav1d_supports, dav1d_open_ctx, dav1d_decode_packet, dav1d_flush_ctx, dav1d_close_ctx,
};
