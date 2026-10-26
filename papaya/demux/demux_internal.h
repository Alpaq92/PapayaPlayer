#ifndef PAPAYA_DEMUX_INTERNAL_H
#define PAPAYA_DEMUX_INTERNAL_H

#include "papaya/demux.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAPAYA_DEMUX_MAX_STREAMS 64

/* One container format. probe scores the first bytes of the file (0: not
 * this format, 100: certain); open fills the demuxer's streams with
 * papaya_demux_add_stream and returns the backend's state. */
typedef struct papaya_demux_backend {
    const char *name;
    int (*probe)(const uint8_t *head, size_t size);
    void *(*open)(papaya_demuxer *d, papaya_io *io);
    int (*read)(void *ctx, papaya_packet **packet);
    int (*seek)(void *ctx, int64_t pts);
    void (*close)(void *ctx);
} papaya_demux_backend;

/* A zeroed stream with its index set, or NULL past PAPAYA_DEMUX_MAX_STREAMS. */
papaya_stream_info *papaya_demux_add_stream(papaya_demuxer *d);
void papaya_demux_set_duration(papaya_demuxer *d, int64_t duration);

extern const papaya_demux_backend papaya_demux_webm;

#ifdef __cplusplus
}
#endif

#endif
