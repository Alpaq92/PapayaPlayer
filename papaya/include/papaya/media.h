#ifndef PAPAYA_MEDIA_H
#define PAPAYA_MEDIA_H

/* The formats every demuxer, decoder and output speaks (TECH-STACK.md step 5):
 * reference-counted buffers, compressed packets and decoded frames. Times are
 * microseconds; PAPAYA_NOPTS marks an unknown time. */

#include <stddef.h>
#include <stdint.h>

#include "papaya/export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAPAYA_NOPTS INT64_MIN

/* ---- Buffers ------------------------------------------------------------ */

typedef struct papaya_buffer papaya_buffer;
typedef void (*papaya_buffer_free_fn)(void *opaque, uint8_t *data);

/* A new buffer of size bytes (zero padding of 64 bytes past the end, which
 * SIMD readers in the decoders rely on). Reference count starts at 1. */
PAPAYA_EXPORT papaya_buffer *papaya_buffer_alloc(size_t size);
/* Wraps memory owned elsewhere; free_fn is called when the last reference goes. */
PAPAYA_EXPORT papaya_buffer *papaya_buffer_wrap(uint8_t *data, size_t size,
                                                papaya_buffer_free_fn free_fn, void *opaque);
PAPAYA_EXPORT papaya_buffer *papaya_buffer_ref(papaya_buffer *b);
PAPAYA_EXPORT void papaya_buffer_unref(papaya_buffer **b);
PAPAYA_EXPORT uint8_t *papaya_buffer_data(const papaya_buffer *b);
PAPAYA_EXPORT size_t papaya_buffer_size(const papaya_buffer *b);

/* ---- Packets ------------------------------------------------------------ */

enum {
    PAPAYA_PACKET_KEY = 1 << 0,     /* decoding can start here */
    PAPAYA_PACKET_DISCARD = 1 << 1, /* decode but do not show (e.g. Opus pre-skip) */
};

typedef struct papaya_packet {
    papaya_buffer *buf;
    const uint8_t *data;
    size_t size;
    int64_t pts, dts, duration;
    int stream;
    uint32_t flags;
} papaya_packet;

/* A packet holding a copy of data, or size uninitialised bytes if data is NULL. */
PAPAYA_EXPORT papaya_packet *papaya_packet_new(const uint8_t *data, size_t size);
PAPAYA_EXPORT void papaya_packet_free(papaya_packet **p);

/* ---- Frames ------------------------------------------------------------- */

typedef enum papaya_media_kind {
    PAPAYA_MEDIA_VIDEO,
    PAPAYA_MEDIA_AUDIO,
} papaya_media_kind;

typedef enum papaya_pixel_format {
    PAPAYA_PIX_NONE,
    PAPAYA_PIX_I420,      /* 8-bit 4:2:0 planar */
    PAPAYA_PIX_I420_10,   /* 10-bit 4:2:0 planar, 16-bit samples, low bits */
    PAPAYA_PIX_NV12,      /* 8-bit 4:2:0, interleaved UV */
    PAPAYA_PIX_P010,      /* 10-bit 4:2:0, interleaved UV, high bits */
    PAPAYA_PIX_I422,      /* 8-bit 4:2:2 planar */
    PAPAYA_PIX_I444,      /* 8-bit 4:4:4 planar */
    PAPAYA_PIX_HW,        /* hardware surface, see hw_handle */
    PAPAYA_PIX_I422_10,   /* 10-bit 4:2:2 planar, 16-bit samples, low bits */
    PAPAYA_PIX_I444_10,   /* 10-bit 4:4:4 planar, 16-bit samples, low bits */
} papaya_pixel_format;

/* Bytes per sample: 2 for the 10-bit formats, else 1. */
static inline int
papaya_pix_bytes(papaya_pixel_format pix)
{
    return pix == PAPAYA_PIX_I420_10 || pix == PAPAYA_PIX_P010 || pix == PAPAYA_PIX_I422_10
                   || pix == PAPAYA_PIX_I444_10
               ? 2
               : 1;
}

typedef enum papaya_sample_format {
    PAPAYA_SAMPLE_NONE,
    PAPAYA_SAMPLE_S16,    /* interleaved */
    PAPAYA_SAMPLE_F32,    /* interleaved */
    PAPAYA_SAMPLE_F32P,   /* planar */
} papaya_sample_format;

/* Colour description, with ITU-T H.273 code points (as in the bitstreams). */
typedef struct papaya_color {
    uint8_t primaries, transfer, matrix;
    uint8_t full_range;
    /* HDR10 static metadata; zero when absent. Luminance in cd/m². */
    float max_luminance, min_luminance;
    uint16_t max_cll, max_fall;
} papaya_color;

typedef struct papaya_frame {
    papaya_media_kind kind;
    int64_t pts, duration;

    /* video */
    papaya_pixel_format pix;
    int width, height;
    uint8_t *planes[4];
    int strides[4];
    papaya_color color;
    void *hw_handle;

    /* audio */
    papaya_sample_format sample;
    int sample_rate, channels, samples;
    uint8_t *audio[8];
} papaya_frame;

/* ---- Frame pool --------------------------------------------------------- */

/* Frames of one stream, reused instead of allocated per frame. A pool lives
 * until its last frame is released, so it can be destroyed while frames are
 * still queued for display. */
typedef struct papaya_frame_pool papaya_frame_pool;

PAPAYA_EXPORT papaya_frame_pool *papaya_frame_pool_new(void);
PAPAYA_EXPORT void papaya_frame_pool_release(papaya_frame_pool **pool);

/* A video frame with planes for the format and size, or NULL. */
PAPAYA_EXPORT papaya_frame *papaya_frame_get_video(papaya_frame_pool *pool, papaya_pixel_format pix,
                                                   int width, int height);
/* An audio frame with room for samples per channel, or NULL. */
PAPAYA_EXPORT papaya_frame *papaya_frame_get_audio(papaya_frame_pool *pool, papaya_sample_format fmt,
                                                   int channels, int sample_rate, int samples);
PAPAYA_EXPORT papaya_frame *papaya_frame_ref(papaya_frame *f);
/* Returns the frame to its pool when the last reference goes. */
PAPAYA_EXPORT void papaya_frame_unref(papaya_frame **f);

#ifdef __cplusplus
}
#endif

#endif
