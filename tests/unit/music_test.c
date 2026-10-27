/* Audio-only files: demuxer and decoder together. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aac_encode.h"
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

/* ---- MP3: minimp3's conformance vectors through our demuxer ------------- */

static uint8_t *
slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data = NULL;
    long n;

    *size = 0;
    if (!f)
        return NULL;
    n = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    if (n >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        data = malloc((size_t)n + 1);
        if (data && fread(data, 1, (size_t)n, f) == (size_t)n)
            *size = (size_t)n;
    }
    fclose(f);
    return data;
}

/* PSNR of the decoded file against its 16-bit reference, as minimp3's own
 * test measures it; -1 if the length is off by more than two frames. */
static double
mp3_psnr(const char *name, int *rate)
{
    char path[1024];
    size_t ref_size, ref_samples, got = 0;
    uint8_t *ref;
    papaya_demuxer *dm;
    papaya_frame_pool *pool;
    papaya_decoder *d;
    papaya_packet *p;
    papaya_frame *f;
    double mse = 0;

    snprintf(path, sizeof path, "%s/%s.pcm", MINIMP3_VECTORS, name);
    ref = slurp(path, &ref_size);
    if (!ref)
        return -2;
    ref_samples = ref_size / 2;
    snprintf(path, sizeof path, "%s/%s.bit", MINIMP3_VECTORS, name);
    dm = papaya_demux_open(papaya_io_open_file(path));
    if (!dm || strcmp(papaya_demux_name(dm), "mp3") != 0) {
        papaya_demux_close(&dm);
        free(ref);
        return -3;
    }
    pool = papaya_frame_pool_new();
    d = papaya_decoder_open(papaya_demux_stream(dm, 0), pool);
    while (d && papaya_demux_read(dm, &p) == 0) {
        papaya_decoder_send(d, p);
        papaya_packet_free(&p);
        while (papaya_decoder_receive(d, &f) == PAPAYA_DECODE_OK) {
            const float *s = (const float *)f->audio[0];

            *rate = f->sample_rate;
            for (int i = 0; i < f->samples * f->channels; i++, got++) {
                double v = (double)s[i] * 32768.0, r;

                if (got >= ref_samples)
                    continue;
                v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
                r = (double)(int16_t)(ref[2 * got] | ref[2 * got + 1] << 8);
                mse += (lround(v) - r) * (lround(v) - r);
            }
            papaya_frame_unref(&f);
        }
    }
    papaya_decoder_close(&d);
    papaya_frame_pool_release(&pool);
    papaya_demux_close(&dm);
    free(ref);
    /* Like minimp3's test, Layer I and II skip the length check: their
     * standard references are a little shorter. */
    if (strncmp(name, "l1", 2) != 0 && strncmp(name, "l2", 2) != 0
        && (got + 4608u < ref_samples || got > ref_samples + 4608u)) /* two stereo frames */
        return -1;
    mse /= got ? (double)got : 1.0;
    return mse == 0 ? 99.0 : 10.0 * log10(32767.0 * 32767.0 / mse);
}

TEST
mp3_vectors(void)
{
    static const struct {
        const char *name;
        int rate;
    } v[] = {
        { "l1-fl1", 32000 },                              /* Layer I */
        { "l2-fl10", 32000 },                             /* Layer II */
        { "l3-compl", 48000 },                            /* Layer III compliance */
        { "l3-he_44khz", 44100 },
        { "l3-he_free", 44100 },                          /* free format */
        { "l3-si", 44100 },
        { "M2L3_bitrate_22_all", 22050 },                 /* MPEG-2, every bitrate */
        { "l3-nonstandard-id3v2", 48000 },                /* ID3v2 tag first */
        { "l3-nonstandard-sin1k0db_lame_vbrtag", 44100 }, /* LAME / Xing frame first */
    };

    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        int rate = 0;
        double psnr = mp3_psnr(v[i].name, &rate);

        ASSERT_EQm(v[i].name, v[i].rate, rate);
        ASSERTm(v[i].name, psnr >= 96.0);
    }
    PASS();
}

/* The LAME-tagged file: its length is the gapless one, and a seek through
 * the Xing table lands near the target. */
TEST
mp3_duration_and_seek(void)
{
    papaya_demuxer *dm = papaya_demux_open(
        papaya_io_open_file(MINIMP3_VECTORS "/l3-nonstandard-sin1k0db_lame_vbrtag.bit"));
    papaya_packet *p;

    ASSERT(dm != NULL);
    /* 1451520 bytes of 16-bit stereo reference: 362880 samples at 44.1 kHz */
    ASSERT_EQ((int64_t)362880 * 1000000 / 44100, papaya_demux_duration(dm));
    ASSERT_EQ(0, papaya_demux_seek(dm, 3000000));
    ASSERT_EQ(0, papaya_demux_read(dm, &p));
    ASSERT(p->pts > 2900000 && p->pts < 3100000);
    papaya_packet_free(&p);
    papaya_demux_close(&dm);
    PASS();
}

/* ---- ADTS: libxaac's encoder writes the file, ID3v2 tag in front ------- */

static void
append_unit(void *user, const unsigned char *data, int size)
{
    bytes *b = user;
    uint8_t *grown = realloc(b->data, b->size + (size_t)size);

    if (!grown)
        return;
    memcpy(grown + b->size, data, (size_t)size);
    b->data = grown;
    b->size += (size_t)size;
}

TEST
adts_round_trip(int aot)
{
    enum { IN_RATE = 48000, N = 96000 };
    static short pcm[N * 2];
    unsigned char config[64];
    int config_size = 0, frames;
    /* An empty ID3v2.4 tag first: MP3 tries the file before ADTS takes it. */
    bytes file = { NULL, 0 };
    papaya_demuxer *dm;
    const papaya_stream_info *s;
    papaya_frame_pool *pool;
    papaya_decoder *d;
    papaya_packet *p;
    papaya_frame *f;
    long samples = 0;
    int rate = 0, channels = 0;
    static const unsigned char id3[10] = { 'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0 };

    append_unit(&file, id3, sizeof id3);
    for (size_t i = 0; i < N; i++)
        pcm[2 * i] = pcm[2 * i + 1] = (short)(12000 * sin(2 * 3.14159265358979 * 440 * (double)i / IN_RATE));
    frames = aac_encode(aot, 1, IN_RATE, 2, aot == 2 ? 128000 : 64000, pcm, N, append_unit, &file, config,
                        &config_size);
    ASSERT(frames > 20);
    dm = papaya_demux_open(papaya_io_open_memory(file.data, file.size));
    ASSERT(dm != NULL);
    ASSERT_STR_EQ("adts", papaya_demux_name(dm));
    s = papaya_demux_stream(dm, 0);
    ASSERT_EQ(PAPAYA_CODEC_AAC, s->codec);
    /* HE-AAC's ADTS header carries the core: half the rate, plain AAC. */
    ASSERT_EQ(aot == 2 ? IN_RATE : IN_RATE / 2, s->sample_rate);
    ASSERT_EQ((int64_t)frames * 1024 * 1000000 / s->sample_rate, papaya_demux_duration(dm));
    pool = papaya_frame_pool_new();
    d = papaya_decoder_open(s, pool);
    ASSERT(d != NULL);
    while (papaya_demux_read(dm, &p) == 0) {
        ASSERT_EQ(PAPAYA_DECODE_OK, papaya_decoder_send(d, p));
        papaya_packet_free(&p);
        while (papaya_decoder_receive(d, &f) == PAPAYA_DECODE_OK) {
            rate = f->sample_rate;
            channels = f->channels;
            samples += f->samples;
            papaya_frame_unref(&f);
        }
    }
    ASSERT_EQ(IN_RATE, rate); /* the decoder finds the SBR itself */
    ASSERT_EQ(2, channels);
    ASSERT(samples >= (long)(frames - 2) * 1024 * (rate / s->sample_rate));
    ASSERT_EQ(0, papaya_demux_seek(dm, 1000000));
    ASSERT_EQ(0, papaya_demux_read(dm, &p));
    ASSERT(p->pts <= 1000000 && p->pts > 1000000 - 1024 * 1000000 / s->sample_rate);
    papaya_packet_free(&p);
    papaya_decoder_close(&d);
    papaya_frame_pool_release(&pool);
    papaya_demux_close(&dm);
    /* Every cut opens or is refused, and reads end. */
    for (size_t cut = 4; cut < file.size; cut += 1499) {
        papaya_demuxer *t = papaya_demux_open(papaya_io_open_memory(file.data, cut));

        while (t && papaya_demux_read(t, &p) == 0)
            papaya_packet_free(&p);
        papaya_demux_close(&t);
    }
    free(file.data);
    PASS();
}

SUITE(music)
{
    RUN_TEST(wav_layouts);
    RUN_TEST(wav_seek_is_exact);
    RUN_TEST(truncated_wav_is_safe);
    RUN_TEST(mp3_vectors);
    RUN_TEST(mp3_duration_and_seek);
    RUN_TEST1(adts_round_trip, 2); /* AAC-LC */
    RUN_TEST1(adts_round_trip, 5); /* HE-AAC */
}

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(music);
    GREATEST_MAIN_END();
}
