#include "papaya/media.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "papaya/thread.h"

#define PAPAYA_PADDING 64
#define PAPAYA_ALIGN 64

/* ---- Buffers ------------------------------------------------------------ */

struct papaya_buffer {
    atomic_int refs;
    uint8_t *data;
    size_t size;
    papaya_buffer_free_fn free_fn;
    void *opaque;
};

static void
free_owned(void *opaque, uint8_t *data)
{
    (void)data;
    free(opaque);
}

papaya_buffer *
papaya_buffer_alloc(size_t size)
{
    /* One allocation: header, then the data aligned for SIMD loads. */
    size_t head = (sizeof(papaya_buffer) + PAPAYA_ALIGN - 1) / PAPAYA_ALIGN * PAPAYA_ALIGN;
    uint8_t *mem;
    papaya_buffer *b;

    if (size > SIZE_MAX - head - PAPAYA_PADDING - PAPAYA_ALIGN)
        return NULL;
    mem = malloc(head + size + PAPAYA_PADDING + PAPAYA_ALIGN);
    if (!mem)
        return NULL;
    b = (papaya_buffer *)mem;
    atomic_init(&b->refs, 1);
    b->data = mem + head + (PAPAYA_ALIGN - (uintptr_t)(mem + head) % PAPAYA_ALIGN) % PAPAYA_ALIGN;
    b->size = size;
    b->free_fn = free_owned;
    b->opaque = mem;
    memset(b->data + size, 0, PAPAYA_PADDING);
    return b;
}

papaya_buffer *
papaya_buffer_wrap(uint8_t *data, size_t size, papaya_buffer_free_fn free_fn, void *opaque)
{
    papaya_buffer *b = malloc(sizeof *b);

    if (!b)
        return NULL;
    atomic_init(&b->refs, 1);
    b->data = data;
    b->size = size;
    b->free_fn = free_fn;
    b->opaque = opaque;
    return b;
}

papaya_buffer *
papaya_buffer_ref(papaya_buffer *b)
{
    atomic_fetch_add_explicit(&b->refs, 1, memory_order_relaxed);
    return b;
}

void
papaya_buffer_unref(papaya_buffer **pb)
{
    papaya_buffer *b = *pb;

    *pb = NULL;
    if (!b || atomic_fetch_sub_explicit(&b->refs, 1, memory_order_acq_rel) != 1)
        return;
    if (b->free_fn == free_owned) {
        free(b->opaque); /* header and data in one block */
        return;
    }
    if (b->free_fn)
        b->free_fn(b->opaque, b->data);
    free(b);
}

uint8_t *papaya_buffer_data(const papaya_buffer *b) { return b->data; }
size_t papaya_buffer_size(const papaya_buffer *b) { return b->size; }

/* ---- Packets ------------------------------------------------------------ */

papaya_packet *
papaya_packet_new(const uint8_t *data, size_t size)
{
    papaya_packet *p = calloc(1, sizeof *p);

    if (!p)
        return NULL;
    p->buf = papaya_buffer_alloc(size);
    if (!p->buf) {
        free(p);
        return NULL;
    }
    if (data && size)
        memcpy(papaya_buffer_data(p->buf), data, size);
    p->data = papaya_buffer_data(p->buf);
    p->size = size;
    p->pts = p->dts = p->duration = PAPAYA_NOPTS;
    return p;
}

void
papaya_packet_free(papaya_packet **pp)
{
    papaya_packet *p = *pp;

    *pp = NULL;
    if (!p)
        return;
    papaya_buffer_unref(&p->buf);
    free(p);
}

/* ---- Frames and pools --------------------------------------------------- */

/* A pooled frame: the public frame first, so a papaya_frame * is a slot *. */
typedef struct slot {
    papaya_frame frame;
    atomic_int refs;
    papaya_buffer *buf;          /* one block for every plane */
    struct papaya_frame_pool *pool;
    struct slot *next;           /* free list */
} slot;

struct papaya_frame_pool {
    papaya_mutex lock;
    slot *free_list;
    int outstanding;             /* frames handed out and not yet returned */
    int released;                /* papaya_frame_pool_release was called */
};

papaya_frame_pool *
papaya_frame_pool_new(void)
{
    papaya_frame_pool *pool = calloc(1, sizeof *pool);

    if (pool)
        papaya_mutex_init(&pool->lock);
    return pool;
}

static void
pool_destroy(papaya_frame_pool *pool)
{
    while (pool->free_list) {
        slot *s = pool->free_list;

        pool->free_list = s->next;
        papaya_buffer_unref(&s->buf);
        free(s);
    }
    papaya_mutex_destroy(&pool->lock);
    free(pool);
}

void
papaya_frame_pool_release(papaya_frame_pool **ppool)
{
    papaya_frame_pool *pool = *ppool;
    int destroy;

    *ppool = NULL;
    if (!pool)
        return;
    papaya_mutex_lock(&pool->lock);
    pool->released = 1;
    destroy = pool->outstanding == 0;
    papaya_mutex_unlock(&pool->lock);
    if (destroy)
        pool_destroy(pool);
}

/* A slot whose buffer holds at least size bytes. */
static slot *
pool_take(papaya_frame_pool *pool, size_t size)
{
    slot *s;

    papaya_mutex_lock(&pool->lock);
    s = pool->free_list;
    if (s)
        pool->free_list = s->next;
    pool->outstanding++;
    papaya_mutex_unlock(&pool->lock);
    if (!s) {
        s = calloc(1, sizeof *s);
        if (!s)
            goto fail;
        s->pool = pool;
    }
    if (!s->buf || papaya_buffer_size(s->buf) < size) {
        papaya_buffer_unref(&s->buf);
        s->buf = papaya_buffer_alloc(size);
        if (!s->buf) {
            free(s);
            goto fail;
        }
    }
    memset(&s->frame, 0, sizeof s->frame);
    atomic_init(&s->refs, 1);
    return s;
fail:
    papaya_mutex_lock(&pool->lock);
    pool->outstanding--;
    papaya_mutex_unlock(&pool->lock);
    return NULL;
}

static size_t
align_up(size_t v)
{
    return (v + PAPAYA_ALIGN - 1) / PAPAYA_ALIGN * PAPAYA_ALIGN;
}

papaya_frame *
papaya_frame_get_video(papaya_frame_pool *pool, papaya_pixel_format pix, int width, int height)
{
    size_t bps, luma_stride, chroma_stride, chroma_h, size;
    int chroma_planes;
    slot *s;
    uint8_t *p;

    if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
        return NULL;
    bps = (size_t)papaya_pix_bytes(pix);
    luma_stride = align_up((size_t)width * bps);
    switch (pix) {
    case PAPAYA_PIX_I420:
    case PAPAYA_PIX_I420_10:
        chroma_stride = align_up((size_t)(width + 1) / 2 * bps);
        chroma_h = (size_t)(height + 1) / 2;
        chroma_planes = 2;
        break;
    case PAPAYA_PIX_NV12:
    case PAPAYA_PIX_P010:
        chroma_stride = align_up((size_t)(width + 1) / 2 * 2 * bps);
        chroma_h = (size_t)(height + 1) / 2;
        chroma_planes = 1;
        break;
    case PAPAYA_PIX_I422:
    case PAPAYA_PIX_I422_10:
        chroma_stride = align_up((size_t)(width + 1) / 2 * bps);
        chroma_h = (size_t)height;
        chroma_planes = 2;
        break;
    case PAPAYA_PIX_I444:
    case PAPAYA_PIX_I444_10:
        chroma_stride = luma_stride;
        chroma_h = (size_t)height;
        chroma_planes = 2;
        break;
    default:
        return NULL;
    }
    size = luma_stride * (size_t)height + chroma_stride * chroma_h * (size_t)chroma_planes;
    s = pool_take(pool, size);
    if (!s)
        return NULL;
    p = papaya_buffer_data(s->buf);
    s->frame.kind = PAPAYA_MEDIA_VIDEO;
    s->frame.pix = pix;
    s->frame.width = width;
    s->frame.height = height;
    s->frame.pts = s->frame.duration = PAPAYA_NOPTS;
    s->frame.planes[0] = p;
    s->frame.strides[0] = (int)luma_stride;
    p += luma_stride * (size_t)height;
    for (int i = 1; i <= chroma_planes; i++) {
        s->frame.planes[i] = p;
        s->frame.strides[i] = (int)chroma_stride;
        p += chroma_stride * chroma_h;
    }
    return &s->frame;
}

papaya_frame *
papaya_frame_get_audio(papaya_frame_pool *pool, papaya_sample_format fmt, int channels,
                       int sample_rate, int samples)
{
    size_t bytes = fmt == PAPAYA_SAMPLE_S16 ? 2 : 4;
    size_t plane;
    slot *s;

    if (channels <= 0 || channels > 8 || samples <= 0 || sample_rate <= 0 || fmt == PAPAYA_SAMPLE_NONE)
        return NULL;
    plane = align_up((size_t)samples * bytes * (fmt == PAPAYA_SAMPLE_F32P ? 1 : (size_t)channels));
    s = pool_take(pool, plane * (fmt == PAPAYA_SAMPLE_F32P ? (size_t)channels : 1));
    if (!s)
        return NULL;
    s->frame.kind = PAPAYA_MEDIA_AUDIO;
    s->frame.sample = fmt;
    s->frame.channels = channels;
    s->frame.sample_rate = sample_rate;
    s->frame.samples = samples;
    s->frame.pts = s->frame.duration = PAPAYA_NOPTS;
    for (int c = 0; c < (fmt == PAPAYA_SAMPLE_F32P ? channels : 1); c++)
        s->frame.audio[c] = papaya_buffer_data(s->buf) + plane * (size_t)c;
    return &s->frame;
}

papaya_frame *
papaya_frame_ref(papaya_frame *f)
{
    atomic_fetch_add_explicit(&((slot *)f)->refs, 1, memory_order_relaxed);
    return f;
}

void
papaya_frame_unref(papaya_frame **pf)
{
    slot *s = (slot *)*pf;
    papaya_frame_pool *pool;
    int destroy;

    *pf = NULL;
    if (!s || atomic_fetch_sub_explicit(&s->refs, 1, memory_order_acq_rel) != 1)
        return;
    pool = s->pool;
    papaya_mutex_lock(&pool->lock);
    s->next = pool->free_list;
    pool->free_list = s;
    pool->outstanding--;
    destroy = pool->released && pool->outstanding == 0;
    papaya_mutex_unlock(&pool->lock);
    if (destroy)
        pool_destroy(pool);
}
