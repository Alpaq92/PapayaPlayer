#ifndef PAPAYA_DECODE_INTERNAL_H
#define PAPAYA_DECODE_INTERNAL_H

#include "papaya/decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One decoder library. Backends that produce at most one frame per packet
 * can leave the queueing to the registry: decode returns the frame (or NULL)
 * for each packet, and NULL packet means drain. */
typedef struct papaya_decode_backend {
    const char *name;
    int (*supports)(const papaya_stream_info *stream);
    void *(*open)(const papaya_stream_info *stream, papaya_frame_pool *pool);
    /* Decodes one packet (NULL: drain). Appends any frames with emit. */
    int (*decode)(void *ctx, const papaya_packet *packet,
                  void (*emit)(void *sink, papaya_frame *frame), void *sink);
    void (*flush)(void *ctx);
    void (*close)(void *ctx);
} papaya_decode_backend;

extern const papaya_decode_backend papaya_decode_opus;
extern const papaya_decode_backend papaya_decode_vorbis;
extern const papaya_decode_backend papaya_decode_flac;
extern const papaya_decode_backend papaya_decode_aac;
extern const papaya_decode_backend papaya_decode_dav1d;
extern const papaya_decode_backend papaya_decode_vpx;
extern const papaya_decode_backend papaya_decode_avc;
extern const papaya_decode_backend papaya_decode_hevc;

/* Reorders interleaved float samples from Vorbis channel order (also Opus
 * mapping family 1) to WAVE order, in place. */
void papaya_reorder_vorbis_to_wave(float *samples, int frames, int channels);

#ifdef __cplusplus
}
#endif

#endif
