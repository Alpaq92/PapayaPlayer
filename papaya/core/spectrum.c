#include "papaya/spectrum.h"

#include <math.h>
#include <stdlib.h>

#define N PAPAYA_SPECTRUM_WINDOW
#define LOW_HZ 50.0f
#define HIGH_HZ 16000.0f
#define FLOOR_DB (-70.0f)
#define TILT_DB 3.0f /* per octave from 1 kHz: music's highs are quieter */
#define GRAVITY 6.0f /* how fast a falling band speeds up, levels per second squared */

struct papaya_spectrum {
    int bands;
    float window[N];
    float re[N], im[N];
    float level[PAPAYA_SPECTRUM_MAX_BANDS];
    float fall[PAPAYA_SPECTRUM_MAX_BANDS]; /* each band's falling speed */
    float sens;            /* auto-sensitivity: quiet music still reaches the top */
};

papaya_spectrum *
papaya_spectrum_new(int bands)
{
    papaya_spectrum *s;
    const float pi = 3.14159265358979f;

    if (bands < 1 || bands > PAPAYA_SPECTRUM_MAX_BANDS)
        return NULL;
    s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->bands = bands;
    s->sens = 1.0f;
    for (int i = 0; i < N; i++)
        s->window[i] = 0.5f - 0.5f * cosf(2.0f * pi * (float)i / (float)(N - 1)); /* Hann */
    return s;
}

void
papaya_spectrum_free(papaya_spectrum **s)
{
    free(*s);
    *s = NULL;
}

/* In place, radix 2. */
static void
fft(float *re, float *im)
{
    const float pi = 3.14159265358979f;

    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;

        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i];

            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (int len = 2; len <= N; len <<= 1) {
        float step = -2.0f * pi / (float)len;

        for (int i = 0; i < N; i += len) {
            for (int k = 0; k < len / 2; k++) {
                float wr = cosf(step * (float)k), wi = sinf(step * (float)k);
                int a = i + k, b = i + k + len / 2;
                float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;

                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }
}

void
papaya_spectrum_update(papaya_spectrum *s, const float *samples, int rate, float dt, float *levels)
{
    float high = HIGH_HZ, bin_hz, ratio, loudest = 0.0f;

    if (rate <= 0)
        rate = 48000;
    if (high > (float)rate * 0.45f)
        high = (float)rate * 0.45f;
    bin_hz = (float)rate / (float)N;
    ratio = powf(high / LOW_HZ, 1.0f / (float)s->bands);
    if (samples) {
        for (int i = 0; i < N; i++) {
            s->re[i] = samples[i] * s->window[i];
            s->im[i] = 0.0f;
        }
        fft(s->re, s->im);
    }
    for (int b = 0; b < s->bands; b++) {
        float target = 0.0f;

        if (samples) {
            float lo = LOW_HZ * powf(ratio, (float)b), hi = lo * ratio;
            int first = (int)(lo / bin_hz + 0.5f), last = (int)(hi / bin_hz + 0.5f);
            float peak = 0.0f, db;

            if (last <= first) /* narrower than a bin: the nearest one */
                last = first + 1;
            if (last > N / 2)
                last = N / 2;
            for (int k = first; k < last; k++) {
                float p = s->re[k] * s->re[k] + s->im[k] * s->im[k];

                if (p > peak)
                    peak = p;
            }
            /* A full-scale sine gives N / 4 through the Hann window: 0 dB. */
            db = 10.0f * log10f(peak / ((float)N * (float)N / 16.0f) + 1e-12f)
                 + TILT_DB * log2f(sqrtf(lo * hi) / 1000.0f);
            target = (db - FLOOR_DB) / -FLOOR_DB * s->sens;
            if (target > loudest)
                loudest = target;
            if (target < 0.0f)
                target = 0.0f;
            if (target > 1.0f)
                target = 1.0f;
        }
        if (target >= s->level[b]) {
            s->level[b] = target;
            s->fall[b] = 0.0f;
        } else {
            s->fall[b] += GRAVITY * dt;
            s->level[b] -= s->fall[b] * dt;
            if (s->level[b] < target)
                s->level[b] = target;
        }
        levels[b] = s->level[b];
    }
    /* As cava does: less when a band overshoots, slowly more while all stay
     * low; only while there is sound, so silence does not wind it up. */
    if (samples && loudest > 1.0f)
        s->sens *= 1.0f - 0.5f * dt;
    else if (samples && loudest > 0.05f && loudest < 0.8f && s->sens < 2.0f)
        s->sens *= 1.0f + 0.25f * dt;
}
