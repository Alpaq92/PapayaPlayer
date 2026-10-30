#include <math.h>

#include "greatest.h"
#include "papaya/spectrum.h"

#define BANDS 32

static float wave[PAPAYA_SPECTRUM_WINDOW];

static void
sine(float hz, float amplitude)
{
    for (int i = 0; i < PAPAYA_SPECTRUM_WINDOW; i++)
        wave[i] = amplitude * sinf(2.0f * 3.14159265f * hz * (float)i / 48000.0f);
}

static int
loudest(const float *levels)
{
    int best = 0;

    for (int b = 1; b < BANDS; b++)
        if (levels[b] > levels[best])
            best = b;
    return best;
}

TEST
sine_lands_in_its_band(void)
{
    papaya_spectrum *s = papaya_spectrum_new(BANDS);
    float levels[BANDS];
    /* Bands are even in pitch from 50 Hz to 16 kHz: 1 kHz is in band
     * log(1000 / 50) / log(16000 / 50) * 32. */
    int want = (int)(logf(1000.0f / 50.0f) / logf(16000.0f / 50.0f) * BANDS);

    ASSERT(s != NULL);
    sine(1000.0f, 1.0f);
    papaya_spectrum_update(s, wave, 48000, 1.0f / 60.0f, levels);
    ASSERT_EQ(want, loudest(levels));
    ASSERT(levels[want] > 0.9f);       /* full scale is near 0 dB */
    ASSERT(levels[0] < 0.3f);          /* far from it, little leaks */
    ASSERT(levels[BANDS - 1] < 0.3f);
    papaya_spectrum_free(&s);
    s = papaya_spectrum_new(BANDS); /* a fresh one: the 1 kHz band is still falling */
    sine(100.0f, 1.0f);
    papaya_spectrum_update(s, wave, 48000, 1.0f / 60.0f, levels);
    ASSERT(loudest(levels) < want);
    ASSERT(levels[loudest(levels)] > 0.75f); /* 3.3 octaves down: tilted 10 dB */
    papaya_spectrum_free(&s);
    ASSERT(s == NULL);
    PASS();
}

TEST
quieter_is_lower(void)
{
    papaya_spectrum *a = papaya_spectrum_new(BANDS), *b = papaya_spectrum_new(BANDS);
    float loud[BANDS], soft[BANDS];
    int band;

    sine(440.0f, 1.0f);
    papaya_spectrum_update(a, wave, 48000, 0.0f, loud);
    sine(440.0f, 0.01f); /* -40 dB */
    papaya_spectrum_update(b, wave, 48000, 0.0f, soft);
    band = loudest(loud);
    ASSERT_EQ(band, loudest(soft));
    ASSERT_IN_RANGE(40.0f / 70.0f, loud[band] - soft[band], 0.05f);
    papaya_spectrum_free(&a);
    papaya_spectrum_free(&b);
    PASS();
}

TEST
falls_with_gravity(void)
{
    papaya_spectrum *s = papaya_spectrum_new(BANDS);
    float levels[BANDS], before, after_short, after_long;
    int band;

    sine(2000.0f, 1.0f);
    papaya_spectrum_update(s, wave, 48000, 1.0f / 60.0f, levels);
    band = loudest(levels);
    before = levels[band];
    papaya_spectrum_update(s, NULL, 48000, 0.1f, levels); /* paused: silence */
    after_short = levels[band];
    ASSERT(after_short < before && after_short > 0.5f);   /* falls, not drops */
    for (int i = 0; i < 20; i++)
        papaya_spectrum_update(s, NULL, 48000, 0.1f, levels);
    after_long = levels[band];
    ASSERT_EQ_FMT(0.0f, after_long, "%f");
    papaya_spectrum_free(&s);
    PASS();
}

TEST
band_count_is_checked(void)
{
    ASSERT(papaya_spectrum_new(0) == NULL);
    ASSERT(papaya_spectrum_new(PAPAYA_SPECTRUM_MAX_BANDS + 1) == NULL);
    PASS();
}

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(sine_lands_in_its_band);
    RUN_TEST(quieter_is_lower);
    RUN_TEST(falls_with_gravity);
    RUN_TEST(band_count_is_checked);
    GREATEST_MAIN_END();
}
