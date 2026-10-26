#ifndef PAPAYA_PLAYER_H
#define PAPAYA_PLAYER_H

/* The player engine (TECH-STACK.md step 5): a demux thread feeds bounded
 * packet queues, a video and an audio decode thread turn them into frames,
 * audio plays through the sound device and drives the master clock, and the
 * UI thread asks for the video frame due now. */

#include <stdint.h>

#include "papaya/demux.h"
#include "papaya/export.h"
#include "papaya/media.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct papaya_player papaya_player;

typedef struct papaya_player_config {
    int null_audio;                    /* no sound card (tests, CI) */
    void (*wake)(void *user);          /* a new video frame may be due; any thread */
    void *wake_user;
} papaya_player_config;

/* Opens a file (UTF-8 path) and starts its threads, paused. NULL if the file
 * cannot be read or has no stream we can decode. */
PAPAYA_EXPORT papaya_player *papaya_player_open(const char *path, const papaya_player_config *config);
PAPAYA_EXPORT void papaya_player_close(papaya_player **p);

PAPAYA_EXPORT void papaya_player_set_paused(papaya_player *p, int paused);
PAPAYA_EXPORT int papaya_player_paused(papaya_player *p);
/* Exact seek: playback resumes at the first frame at or after pts. */
PAPAYA_EXPORT void papaya_player_seek(papaya_player *p, int64_t pts);
PAPAYA_EXPORT void papaya_player_set_volume(papaya_player *p, float volume);

/* Playback position (master clock) and duration, microseconds. */
PAPAYA_EXPORT int64_t papaya_player_position(papaya_player *p);
PAPAYA_EXPORT int64_t papaya_player_duration(papaya_player *p);
/* Everything has been played. */
PAPAYA_EXPORT int papaya_player_ended(papaya_player *p);

PAPAYA_EXPORT const papaya_stream_info *papaya_player_video_stream(papaya_player *p);
PAPAYA_EXPORT const papaya_stream_info *papaya_player_audio_stream(papaya_player *p);

/* UI thread: the frame to show now, with a reference the caller releases, or
 * NULL if it has not changed since the last call. Late frames are dropped. */
PAPAYA_EXPORT papaya_frame *papaya_player_video_frame(papaya_player *p);
/* Microseconds until the next video frame is due, or -1 if none is queued. */
PAPAYA_EXPORT int64_t papaya_player_next_frame_in(papaya_player *p);
/* Frames dropped because they were late, since open. */
PAPAYA_EXPORT int papaya_player_dropped(papaya_player *p);

#ifdef __cplusplus
}
#endif

#endif
