#ifndef AAC_ENCODE_H
#define AAC_ENCODE_H

/* Encodes interleaved 16-bit PCM with libxaac (aot 2: LC, 5: HE-AAC, 29:
 * HE-AAC v2). Calls au once per raw access unit (with adts, once per ADTS
 * frame, header included) and writes the AudioSpecificConfig to config (64
 * bytes is plenty; none with adts). Returns the number of units, or -1. */
int aac_encode(int aot, int adts, int rate, int channels, int bitrate, const short *pcm, int samples,
               void (*au)(void *user, const unsigned char *data, int size), void *user,
               unsigned char *config, int *config_size);

#endif
