#ifndef PAPAYA_DECODE_H
#define PAPAYA_DECODE_H

/* Decoders: packets in, frames out (TECH-STACK.md, "Layers"). Send a packet,
 * then receive frames until PAPAYA_DECODE_AGAIN; send NULL at the end of the
 * stream to drain. Audio comes out as interleaved float in WAVE channel order
 * (FL FR FC LFE BL BR SL SR); video in the decoder's native pixel format. */

#include "papaya/demux.h"
#include "papaya/export.h"
#include "papaya/media.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PAPAYA_DECODE_OK = 0,
    PAPAYA_DECODE_AGAIN = 1,  /* send more packets */
    PAPAYA_DECODE_EOF = 2,    /* drained */
    PAPAYA_DECODE_ERROR = -1,
};

typedef struct papaya_decoder papaya_decoder;

/* A decoder for the stream, or NULL if no backend handles its codec. Frames
 * come from pool, which must outlive the decoder. */
PAPAYA_EXPORT papaya_decoder *papaya_decoder_open(const papaya_stream_info *stream,
                                                  papaya_frame_pool *pool);
PAPAYA_EXPORT void papaya_decoder_close(papaya_decoder **d);
PAPAYA_EXPORT const char *papaya_decoder_name(const papaya_decoder *d);

/* packet NULL starts draining. The packet stays the caller's. */
PAPAYA_EXPORT int papaya_decoder_send(papaya_decoder *d, const papaya_packet *packet);
PAPAYA_EXPORT int papaya_decoder_receive(papaya_decoder *d, papaya_frame **frame);
/* Drops buffered state, for seeking. */
PAPAYA_EXPORT void papaya_decoder_flush(papaya_decoder *d);

#ifdef __cplusplus
}
#endif

#endif
