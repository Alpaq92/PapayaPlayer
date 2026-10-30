#ifndef PAPAYA_DEMUX_H
#define PAPAYA_DEMUX_H

/* Demuxers: split a container into per-stream packets. One registry probes
 * the file and picks the backend (TECH-STACK.md, "Layers"). */

#include <stdint.h>

#include "papaya/export.h"
#include "papaya/io.h"
#include "papaya/media.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum papaya_codec {
    PAPAYA_CODEC_UNKNOWN,
    PAPAYA_CODEC_AV1,
    PAPAYA_CODEC_VP8,
    PAPAYA_CODEC_VP9,
    PAPAYA_CODEC_H264,
    PAPAYA_CODEC_HEVC,
    PAPAYA_CODEC_MPEG2,
    PAPAYA_CODEC_OPUS,
    PAPAYA_CODEC_VORBIS,
    PAPAYA_CODEC_FLAC,
    PAPAYA_CODEC_AAC,
    PAPAYA_CODEC_MP3,
    PAPAYA_CODEC_AC3,
    PAPAYA_CODEC_EAC3,
    PAPAYA_CODEC_PCM_S16LE,
    PAPAYA_CODEC_PCM_F32LE,
    PAPAYA_CODEC_PCM_U8,
    PAPAYA_CODEC_PCM_S24LE,
    PAPAYA_CODEC_PCM_S32LE,
    PAPAYA_CODEC_PCM_F64LE,
} papaya_codec;

PAPAYA_EXPORT const char *papaya_codec_name(papaya_codec codec);

typedef struct papaya_stream_info {
    int index;
    papaya_media_kind kind;
    papaya_codec codec;
    const char *codec_tag;     /* the container's own id, e.g. "V_VP9" */
    const uint8_t *extradata;  /* codec private data, owned by the demuxer */
    size_t extradata_size;
    char language[16];
    char title[128];
    int is_default;
    int64_t duration;          /* microseconds, PAPAYA_NOPTS if unknown */

    /* video */
    int width, height;
    papaya_color color;

    /* audio */
    int sample_rate, channels, bits_per_sample;
    int64_t codec_delay;       /* microseconds to drop at the start (Opus pre-skip) */
    int64_t seek_preroll;      /* microseconds to decode before a seek target */
    int64_t gapless_end;       /* microseconds; audio from here on is encoder
                                * padding (MP4 edit list, iTunSMPB). 0: none */
} papaya_stream_info;

enum {
    PAPAYA_DEMUX_EOF = 1,
    PAPAYA_DEMUX_ERROR = -1,
};

typedef struct papaya_demuxer papaya_demuxer;

/* Probes and opens; takes ownership of io (closed with the demuxer, or on
 * failure). Returns NULL if no demuxer recognises the data. */
PAPAYA_EXPORT papaya_demuxer *papaya_demux_open(papaya_io *io);
PAPAYA_EXPORT void papaya_demux_close(papaya_demuxer **d);

PAPAYA_EXPORT const char *papaya_demux_name(const papaya_demuxer *d);
PAPAYA_EXPORT int papaya_demux_stream_count(const papaya_demuxer *d);
PAPAYA_EXPORT const papaya_stream_info *papaya_demux_stream(const papaya_demuxer *d, int index);
PAPAYA_EXPORT int64_t papaya_demux_duration(const papaya_demuxer *d);

/* Next packet of any stream in file order: 0 and *packet set, PAPAYA_DEMUX_EOF,
 * or PAPAYA_DEMUX_ERROR. */
PAPAYA_EXPORT int papaya_demux_read(papaya_demuxer *d, papaya_packet **packet);
/* Moves so the next packets start at the keyframe at or before pts. */
PAPAYA_EXPORT int papaya_demux_seek(papaya_demuxer *d, int64_t pts);

#ifdef __cplusplus
}
#endif

#endif
