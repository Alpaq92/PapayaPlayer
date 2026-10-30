#ifndef PAPAYA_AUDIO_OUT_H
#define PAPAYA_AUDIO_OUT_H

/* Sound output through miniaudio (TECH-STACK.md step 4). Decoded frames are
 * queued; the device callback plays them and sets the master clock to the
 * time actually reaching the speakers (the frame's pts, less what is still
 * buffered in the device). miniaudio resamples and remaps channels when the
 * device's native format differs. */

#include <stdint.h>

#include "papaya/clock.h"
#include "papaya/export.h"
#include "papaya/media.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct papaya_audio_out papaya_audio_out;

typedef struct papaya_audio_out_config {
    int sample_rate;          /* of the frames that will be written */
    int channels;             /* WAVE order */
    papaya_clock *clock;      /* set from the callback; may be NULL */
    int null_device;          /* no sound card (tests, CI): play into nothing in real time */
    int64_t buffer_duration;  /* how much may be queued ahead, microseconds; 0: 300 ms */
} papaya_audio_out_config;

PAPAYA_EXPORT papaya_audio_out *papaya_audio_out_open(const papaya_audio_out_config *config);
PAPAYA_EXPORT void papaya_audio_out_close(papaya_audio_out **ao);
PAPAYA_EXPORT const char *papaya_audio_out_device_name(const papaya_audio_out *ao);

/* Queues a frame (F32 interleaved) with the serial of the packet queue it
 * came from. Blocks while the buffer is full. A frame in another rate or
 * channel count than the device's (HE-AAC's SBR or PS, found only when
 * decoding) restarts the device in its format, dropping what is queued; call
 * it from one thread only.
 * Returns 0, or < 0 once the output is closing or aborted. */
PAPAYA_EXPORT int papaya_audio_out_write(papaya_audio_out *ao, papaya_frame *frame, int serial);
/* Drops everything queued (seeking); frames written after carry serial. */
PAPAYA_EXPORT void papaya_audio_out_flush(papaya_audio_out *ao, int serial);
/* Wakes and fails a blocked write (shutting down). */
PAPAYA_EXPORT void papaya_audio_out_abort(papaya_audio_out *ao);
PAPAYA_EXPORT void papaya_audio_out_set_paused(papaya_audio_out *ao, int paused);
PAPAYA_EXPORT void papaya_audio_out_set_volume(papaya_audio_out *ao, float volume);
/* Microseconds of audio queued and not yet played. */
PAPAYA_EXPORT int64_t papaya_audio_out_queued(papaya_audio_out *ao);
/* The newest n (<= 4096) samples played, mixed to mono and before the
 * volume, oldest first, for a visualiser; any thread. Returns their rate, or
 * 0 if nothing has played yet (out is then zeros) or n is out of range. */
PAPAYA_EXPORT int papaya_audio_out_scope(papaya_audio_out *ao, float *out, int n);
/* Device latency in microseconds (its own buffer). */
PAPAYA_EXPORT int64_t papaya_audio_out_latency(const papaya_audio_out *ao);

#ifdef __cplusplus
}
#endif

#endif
