#include <stdlib.h>

#include "greatest.h"
#include "papaya/player.h"
#include "papaya/thread.h"

#define SAMPLE PAPAYA_LIBWEBM_TESTDATA "/bbb_480p_vp9_opus_1second.webm"

typedef struct {
    int frames, late;
    int64_t first_pts, last_pts, worst_lag;
} watched;

/* Plays like a UI would: polls for the due frame every 2 ms. */
static void
watch(papaya_player *p, watched *w, int64_t for_us)
{
    int64_t end = papaya_time_us() + for_us;

    while (papaya_time_us() < end && !papaya_player_ended(p)) {
        papaya_frame *f = papaya_player_video_frame(p);

        if (f) {
            int64_t lag = papaya_player_position(p) - f->pts;

            if (w->frames == 0)
                w->first_pts = f->pts;
            if (f->pts < w->last_pts)
                w->late++; /* out of order */
            w->last_pts = f->pts;
            if (w->frames > 0 && lag > w->worst_lag)
                w->worst_lag = lag;
            w->frames++;
            papaya_frame_unref(&f);
        }
        papaya_sleep_us(2000);
    }
}

TEST
plays_in_sync_to_the_end(void)
{
    papaya_player_config cfg = { .null_audio = 1 };
    papaya_player *p = papaya_player_open(SAMPLE, &cfg);
    watched w = { 0 };

    ASSERT(p != NULL);
    ASSERT(papaya_player_video_stream(p) != NULL);
    ASSERT(papaya_player_audio_stream(p) != NULL);
    ASSERT(papaya_player_paused(p));
    papaya_player_set_paused(p, 0);
    watch(p, &w, 4000000);
    ASSERT(papaya_player_ended(p));
    ASSERT(w.frames + papaya_player_dropped(p) >= 20); /* 24 frames in the file */
    ASSERT(w.frames >= 12);
    ASSERT_EQ(0, w.late);
    ASSERT(w.first_pts < 50000);
    ASSERT(w.last_pts > 900000);
    /* Frames are shown within a frame or two of the audio clock. */
    ASSERT(w.worst_lag < 120000);
    papaya_player_close(&p);
    ASSERT(p == NULL);
    PASS();
}

TEST
seek_lands_on_target(void)
{
    papaya_player_config cfg = { .null_audio = 1 };
    papaya_player *p = papaya_player_open(SAMPLE, &cfg);
    watched w = { 0 };

    ASSERT(p != NULL);
    papaya_player_seek(p, 600000);
    papaya_player_set_paused(p, 0);
    watch(p, &w, 3000000);
    ASSERT(w.frames > 0);
    /* Exact seek: nothing before the target is shown. */
    ASSERT(w.first_pts >= 560000);
    ASSERT(w.first_pts < 700000);
    ASSERT(papaya_player_ended(p));
    papaya_player_close(&p);
    PASS();
}

TEST
pause_holds_position(void)
{
    papaya_player_config cfg = { .null_audio = 1 };
    papaya_player *p = papaya_player_open(SAMPLE, &cfg);
    watched w = { 0 };
    int64_t a, b;

    ASSERT(p != NULL);
    papaya_player_set_paused(p, 0);
    watch(p, &w, 300000);
    papaya_player_set_paused(p, 1);
    a = papaya_player_position(p);
    papaya_sleep_us(200000);
    b = papaya_player_position(p);
    ASSERT(b - a < 20000);
    ASSERT(!papaya_player_ended(p));
    papaya_player_close(&p);
    PASS();
}

TEST
close_while_playing(void)
{
    papaya_player_config cfg = { .null_audio = 1 };

    for (int i = 0; i < 5; i++) {
        papaya_player *p = papaya_player_open(SAMPLE, &cfg);

        ASSERT(p != NULL);
        papaya_player_set_paused(p, 0);
        papaya_sleep_us((int64_t)20000 * i);
        papaya_player_close(&p);
    }
    PASS();
}

TEST
rejects_missing_file(void)
{
    ASSERT(papaya_player_open(PAPAYA_LIBWEBM_TESTDATA "/no-such-file.webm", NULL) == NULL);
    PASS();
}

SUITE(player)
{
    RUN_TEST(plays_in_sync_to_the_end);
    RUN_TEST(seek_lands_on_target);
    RUN_TEST(pause_holds_position);
    RUN_TEST(close_while_playing);
    RUN_TEST(rejects_missing_file);
}

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(player);
    GREATEST_MAIN_END();
}
