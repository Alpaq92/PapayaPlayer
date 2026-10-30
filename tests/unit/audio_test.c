#include "greatest.h"
#include "papaya/audio_out.h"
#include "papaya/thread.h"

/* miniaudio's null device plays into nothing at real-time speed, so these run
 * on CI machines without a sound card. */

static papaya_frame *
silence(papaya_frame_pool *pool, int64_t pts, int samples)
{
    papaya_frame *f = papaya_frame_get_audio(pool, PAPAYA_SAMPLE_F32, 2, 48000, samples);

    for (int i = 0; i < samples * 2; i++)
        ((float *)f->audio[0])[i] = 0.0f;
    f->pts = pts;
    return f;
}

TEST
clock_follows_playback(void)
{
    papaya_clock clock;
    papaya_frame_pool *pool = papaya_frame_pool_new();
    papaya_audio_out_config cfg = { .sample_rate = 48000, .channels = 2, .clock = &clock,
                                    .null_device = 1, .buffer_duration = 2000000 };
    papaya_audio_out *ao;
    int64_t t;

    papaya_clock_init(&clock);
    ao = papaya_audio_out_open(&cfg);
    ASSERT(ao != NULL);
    for (int i = 0; i < 20; i++) { /* 1 s in 50 ms frames */
        papaya_frame *f = silence(pool, (int64_t)i * 50000, 2400);

        ASSERT_EQ(0, papaya_audio_out_write(ao, f, 0));
        papaya_frame_unref(&f);
    }
    papaya_sleep_us(400000);
    t = papaya_clock_get(&clock, NULL);
    /* Loose bounds: the null device's timing and the scheduler both jitter. */
    ASSERT(t != PAPAYA_NOPTS);
    ASSERT(t > 150000 && t < 650000);
    ASSERT(papaya_audio_out_queued(ao) < 900000);

    papaya_audio_out_set_paused(ao, 1);
    t = papaya_clock_get(&clock, NULL);
    papaya_sleep_us(100000);
    ASSERT(papaya_clock_get(&clock, NULL) - t < 30000);
    papaya_audio_out_set_paused(ao, 0);

    papaya_audio_out_flush(ao, 1);
    ASSERT_EQ(0, (int)papaya_audio_out_queued(ao));
    papaya_audio_out_close(&ao);
    ASSERT(ao == NULL);
    papaya_clock_destroy(&clock);
    papaya_frame_pool_release(&pool);
    PASS();
}

/* A frame in another format (HE-AAC decodes at twice the container's rate)
 * restarts the device in it; playback and the clock go on. */
TEST
adopts_the_frames_format(void)
{
    papaya_clock clock;
    papaya_frame_pool *pool = papaya_frame_pool_new();
    papaya_audio_out_config cfg = { .sample_rate = 24000, .channels = 2, .clock = &clock, .null_device = 1 };
    papaya_audio_out *ao;
    papaya_frame *bad = papaya_frame_get_audio(pool, PAPAYA_SAMPLE_S16, 2, 48000, 480);

    papaya_clock_init(&clock);
    ao = papaya_audio_out_open(&cfg);
    ASSERT(ao != NULL);
    ASSERT(papaya_audio_out_write(ao, bad, 0) < 0); /* not F32: still refused */
    papaya_frame_unref(&bad);
    for (int i = 0; i < 6; i++) { /* 48 kHz stereo, then 48 kHz mono */
        papaya_frame *f = papaya_frame_get_audio(pool, PAPAYA_SAMPLE_F32, i < 3 ? 2 : 1, 48000, 2400);

        for (int k = 0; k < 2400 * f->channels; k++)
            ((float *)f->audio[0])[k] = 0.0f;
        f->pts = (int64_t)i * 50000;
        ASSERT_EQ(0, papaya_audio_out_write(ao, f, 0));
        papaya_frame_unref(&f);
    }
    papaya_sleep_us(250000);
    ASSERT(papaya_clock_get(&clock, NULL) >= 150000); /* the mono frames play */
    papaya_audio_out_close(&ao);
    papaya_clock_destroy(&clock);
    papaya_frame_pool_release(&pool);
    PASS();
}

static int
blocked_write(void *p)
{
    papaya_audio_out *ao = p;
    papaya_frame_pool *pool = papaya_frame_pool_new();
    int r = 0;

    /* Far more than the 300 ms buffer: blocks until aborted. */
    for (int i = 0; i < 1000 && r == 0; i++) {
        papaya_frame *f = silence(pool, (int64_t)i * 50000, 2400);

        r = papaya_audio_out_write(ao, f, 0);
        papaya_frame_unref(&f);
    }
    papaya_frame_pool_release(&pool);
    return r;
}

TEST
abort_releases_writer(void)
{
    papaya_audio_out_config cfg = { .sample_rate = 48000, .channels = 2, .null_device = 1 };
    papaya_audio_out *ao = papaya_audio_out_open(&cfg);
    papaya_thread *t;

    ASSERT(ao != NULL);
    papaya_audio_out_set_paused(ao, 1); /* nothing drains */
    t = papaya_thread_start(blocked_write, ao, "writer");
    papaya_sleep_us(100000);
    papaya_audio_out_abort(ao);
    ASSERT(papaya_thread_join(t) < 0);
    papaya_audio_out_close(&ao);
    PASS();
}

SUITE(audio)
{
    RUN_TEST(clock_follows_playback);
    RUN_TEST(adopts_the_frames_format);
    RUN_TEST(abort_releases_writer);
}

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(audio);
    GREATEST_MAIN_END();
}
