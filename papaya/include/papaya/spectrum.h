#ifndef PAPAYA_SPECTRUM_H
#define PAPAYA_SPECTRUM_H

/* The visualiser's analysis: the newest audio, windowed and transformed,
 * gathered into bands spaced evenly in pitch from 50 Hz up, each a level from
 * 0 to 1 (-70 dB to 0 dB of full scale). Levels jump up at once and fall
 * back with gravity, as cava's do. Drawing them is the app's. */

#include "papaya/export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAPAYA_SPECTRUM_WINDOW 2048 /* samples analysed per update */
#define PAPAYA_SPECTRUM_MAX_BANDS 256

typedef struct papaya_spectrum papaya_spectrum;

/* NULL unless 1 <= bands <= PAPAYA_SPECTRUM_MAX_BANDS. */
PAPAYA_EXPORT papaya_spectrum *papaya_spectrum_new(int bands);
PAPAYA_EXPORT void papaya_spectrum_free(papaya_spectrum **s);

/* samples: the newest PAPAYA_SPECTRUM_WINDOW mono samples at rate, or NULL
 * for silence (paused); dt: seconds since the last update. Fills levels with
 * one value per band, lowest first. */
PAPAYA_EXPORT void papaya_spectrum_update(papaya_spectrum *s, const float *samples, int rate, float dt,
                                          float *levels);

#ifdef __cplusplus
}
#endif

#endif
