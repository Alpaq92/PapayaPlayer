/* Audio-only files: demuxer and decoder together. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "papaya/decode.h"
#include "papaya/demux.h"

#define RATE 44100
#define FRAMES 10000 /* not a multiple of the WAV demuxer's packet size */

/* ---- WAV: every sample layout, written here, read back exactly ---------- */

typedef struct {
    uint8_t *data;
    size_t size;
} bytes;

static void
le(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void
fourcc(uint8_t *p, const char *tag)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)tag[i];
}

/* The test signal: a ramp through the whole range, different per channel. */
static double
value(int frame, int channel)
{
    return (double)((frame * 37 + channel * 1000) % 2001 - 1000) / 1000.0 * 0.9;
}

/* A WAV of FRAMES stereo frames; tag 1 (integer) or 3 (float), optionally
 * WAVE_FORMAT_EXTENSIBLE. */
static bytes
make_wav(int tag, int bits, int extensible)
{
    int ch = 2, align = ch * bits / 8, fmt_len = extensible ? 40 : 16;
    size_t data_len = (size_t)FRAMES * (size_t)align;
    bytes w;
    uint8_t *p;

    w.size = 12 + 8 + (size_t)fmt_len + 8 + data_len;
    w.data = calloc(1, w.size);
    p = w.data;
    fourcc(p, "RIFF");
    le(p + 4, w.size - 8, 4);
    fourcc(p + 8, "WAVE");
    p += 12;
    fourcc(p, "fmt ");
    le(p + 4, (uint64_t)fmt_len, 4);
    le(p + 8, extensible ? 0xFFFE : (uint64_t)tag, 2);
    le(p + 10, (uint64_t)ch, 2);
    le(p + 12, RATE, 4);
    le(p + 16, (uint64_t)RATE * (uint64_t)align, 4);
    le(p + 20, (uint64_t)align, 2);
    le(p + 22, (uint64_t)bits, 2);
    if (extensible) {
        le(p + 24, 22, 2);
        le(p + 26, (uint64_t)bits, 2);
        le(p + 28, 3, 4);            /* front left, front right */
        le(p + 32, (uint64_t)tag, 2); /* the subformat GUID starts with the tag */
    }
    p += 8 + fmt_len;
    fourcc(p, "data");
    le(p + 4, data_len, 4);
    p += 8;
    for (int i = 0; i < FRAMES; i++)
        for (int c = 0; c < ch; c++, p += bits / 8) {
            double v = value(i, c);

            if (tag == 3 && bits == 32) {
                float f = (float)v;
                uint32_t u;

                memcpy(&u, &f, 4);
                le(p, u, 4);
            } else if (tag == 3) {
                uint64_t u;

                memcpy(&u, &v, 8);
                le(p, u, 8);
            } else if (bits == 8) {
                p[0] = (uint8_t)(128 + (int)lround(v * 128));
            } else {
                int64_t scale = (int64_t)1 << (bits - 1); /* the decoder's scale */

                le(p, (uint64_t)(int64_t)llround(v * (double)scale), bits / 8);
            }
        }
    return w;
}

/* Decodes the whole file; returns the frames and the largest error. */
static long
decode_all(papaya_demuxer *dm, double *max_err, int64_t *first_pts)
{
    papaya_frame_pool *pool = papaya_frame_pool_new();
    papaya_decoder *d = papaya_decoder_open(papaya_demux_stream(dm, 0), pool);
    papaya_packet *p;
    papaya_frame *f;
    long n = 0;

    *max_err = 0;
    *first_pts = PAPAYA_NOPTS;
    if (!d) {
        papaya_frame_pool_release(&pool);
        return -1;
    }
    while (papaya_demux_read(dm, &p) == 0) {
        papaya_decoder_send(d, p);
        papaya_packet_free(&p);
        while (papaya_decoder_receive(d, &f) == PAPAYA_DECODE_OK) {
            const float *s = (const float *)f->audio[0];

            if (*first_pts == PAPAYA_NOPTS)
                *first_pts = f->pts;
            for (int i = 0; i < f->samples; i++)
                for (int c = 0; c < f->channels; c++) {
                    double e = fabs((double)s[i * f->channels + c] - value((int)n + i, c));

                    if (e > *max_err)
                        *max_err = e;
                }
            n += f->samples;
            papaya_frame_unref(&f);
        }
    }
    papaya_decoder_close(&d);
    papaya_frame_pool_release(&pool);
    return n;
}

TEST
wav_layouts(void)
{
    static const struct {
        int tag, bits, extensible;
        papaya_codec codec;
        double tolerance; /* the format's own rounding */
    } v[] = {
        { 1, 8, 0, PAPAYA_CODEC_PCM_U8, 1.0 / 127 },
        { 1, 16, 0, PAPAYA_CODEC_PCM_S16LE, 1.0 / 32767 },
        { 1, 24, 0, PAPAYA_CODEC_PCM_S24LE, 1.0 / 8388607 },
        { 1, 32, 0, PAPAYA_CODEC_PCM_S32LE, 1e-6 },
        { 3, 32, 0, PAPAYA_CODEC_PCM_F32LE, 1e-7 },
        { 3, 64, 0, PAPAYA_CODEC_PCM_F64LE, 1e-7 },
        { 1, 24, 1, PAPAYA_CODEC_PCM_S24LE, 1.0 / 8388607 }, /* WAVE_FORMAT_EXTENSIBLE */
    };

    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        bytes w = make_wav(v[i].tag, v[i].bits, v[i].extensible);
        papaya_demuxer *dm = papaya_demux_open(papaya_io_open_memory(w.data, w.size));
        const papaya_stream_info *s;
        double err;
        int64_t first;

        ASSERT(dm != NULL);
        ASSERT_STR_EQ("wav", papaya_demux_name(dm));
        s = papaya_demux_stream(dm, 0);
        ASSERT_EQ(v[i].codec, s->codec);
        ASSERT_EQ(2, s->channels);
        ASSERT_EQ(RATE, s->sample_rate);
        ASSERT_EQ((int64_t)FRAMES * 1000000 / RATE, papaya_demux_duration(dm));
        ASSERT_EQ(FRAMES, decode_all(dm, &err, &first));
        ASSERT_EQ(0, (int)first);
        ASSERTm("sample error over the format's rounding", err <= v[i].tolerance + 1e-9);
        papaya_demux_close(&dm);
        free(w.data);
    }
    PASS();
}

TEST
wav_seek_is_exact(void)
{
    bytes w = make_wav(1, 16, 0);
    papaya_demuxer *dm = papaya_demux_open(papaya_io_open_memory(w.data, w.size));
    papaya_packet *p;

    ASSERT(dm != NULL);
    ASSERT_EQ(0, papaya_demux_seek(dm, 100000)); /* 0.1 s: frame 4410 */
    ASSERT_EQ(0, papaya_demux_read(dm, &p));
    ASSERT_EQ(100000, (int)p->pts);
    papaya_packet_free(&p);
    papaya_demux_close(&dm);
    free(w.data);
    PASS();
}

TEST
truncated_wav_is_safe(void)
{
    bytes w = make_wav(1, 16, 0);

    for (size_t cut = 4; cut < w.size; cut += 997) {
        papaya_demuxer *dm = papaya_demux_open(papaya_io_open_memory(w.data, cut));
        papaya_packet *p;

        if (!dm)
            continue;
        while (papaya_demux_read(dm, &p) == 0)
            papaya_packet_free(&p);
        papaya_demux_close(&dm);
    }
    free(w.data);
    PASS();
}

SUITE(music)
{
    RUN_TEST(wav_layouts);
    RUN_TEST(wav_seek_is_exact);
    RUN_TEST(truncated_wav_is_safe);
}

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(music);
    GREATEST_MAIN_END();
}
