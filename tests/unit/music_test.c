/* Audio-only files: demuxer and decoder together. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FLAC/metadata.h"
#include "FLAC/stream_encoder.h"
#include "ogg/ogg.h"
#include "opus.h"
#include "vorbis/vorbisenc.h"
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
                v = (double)lround(v) - r; /* the error after rounding to 16 bits */
                mse += v * v;
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

/* The rarer ADTS layouts: two raw data blocks per frame, junk between
 * frames and an ID3v1 tag at the end. They decode to the same PCM as plain
 * one-block frames of the same access units. */
typedef struct {
    bytes all;
    int sizes[512], count;
} units;

static void
keep_unit(void *user, const unsigned char *data, int size)
{
    units *u = user;

    if (u->count < 512) {
        append_unit(&u->all, data, size);
        u->sizes[u->count++] = size;
    }
}

/* ADTS frames of `per` access units each (no CRC); junk after every fifth. */
static void
write_adts(bytes *file, const units *u, int per, int junk)
{
    static const unsigned char garbage[37] = { 0xFF, 0xF1, 0x00, 0x12, 0x34 }; /* a false sync too */
    size_t at = 0;

    for (int i = 0; i < u->count; i += per) {
        int n = u->count - i < per ? u->count - i : per, payload = 0;
        unsigned char h[7];

        for (int k = 0; k < n; k++)
            payload += u->sizes[i + k];
        h[0] = 0xFF;
        h[1] = 0xF1;                      /* MPEG-4, no CRC */
        h[2] = 1 << 6 | 3 << 2;           /* LC, 48 kHz */
        h[3] = (unsigned char)(2 << 6 | (payload + 7) >> 11);
        h[4] = (unsigned char)((payload + 7) >> 3);
        h[5] = (unsigned char)(((payload + 7) & 7) << 5 | 0x1F);
        h[6] = (unsigned char)(0xFC | (n - 1));
        append_unit(file, h, 7);
        append_unit(file, u->all.data + at, payload);
        at += (size_t)payload;
        if (junk && i / per % 5 == 4)
            append_unit(file, garbage, sizeof garbage);
    }
    if (junk) {
        unsigned char tag[128] = { 'T', 'A', 'G' };

        append_unit(file, tag, sizeof tag);
    }
}

/* Decodes all of an ADTS file; the PCM goes to out (samples * 2 floats). */
static long
decode_adts(const bytes *file, float *out, long cap, int64_t *duration)
{
    papaya_demuxer *dm = papaya_demux_open(papaya_io_open_memory(file->data, file->size));
    papaya_frame_pool *pool = papaya_frame_pool_new();
    papaya_decoder *d = dm ? papaya_decoder_open(papaya_demux_stream(dm, 0), pool) : NULL;
    papaya_packet *p;
    papaya_frame *f;
    long samples = 0;

    if (!d) {
        papaya_demux_close(&dm);
        papaya_frame_pool_release(&pool);
        return -1;
    }
    *duration = papaya_demux_duration(dm);
    while (papaya_demux_read(dm, &p) == 0) {
        papaya_decoder_send(d, p);
        papaya_packet_free(&p);
        while (papaya_decoder_receive(d, &f) == PAPAYA_DECODE_OK) {
            for (int i = 0; i < f->samples * 2 && samples * 2 + i < cap * 2; i++)
                out[samples * 2 + i] = ((const float *)f->audio[0])[i];
            samples += f->samples;
            papaya_frame_unref(&f);
        }
    }
    papaya_decoder_close(&d);
    papaya_frame_pool_release(&pool);
    papaya_demux_close(&dm);
    return samples;
}

TEST
adts_variants(void)
{
    enum { N = 96000 };
    static short pcm[N * 2];
    static float plain[N * 2 + 4096], packed[N * 2 + 4096];
    unsigned char config[64];
    int config_size = 0;
    units u = { { NULL, 0 }, { 0 }, 0 };
    bytes one = { NULL, 0 }, two = { NULL, 0 };
    int64_t d1 = 0, d2 = 0;
    long n1, n2;

    for (size_t i = 0; i < N; i++)
        pcm[2 * i] = pcm[2 * i + 1] = (short)(12000 * sin(2 * 3.14159265358979 * 440 * (double)i / 48000));
    ASSERT(aac_encode(2, 0, 48000, 2, 128000, pcm, N, keep_unit, &u, config, &config_size) > 20);
    ASSERT(u.count < 512);
    write_adts(&one, &u, 1, 0);
    write_adts(&two, &u, 2, 1);
    n1 = decode_adts(&one, plain, N + 2048, &d1);
    n2 = decode_adts(&two, packed, N + 2048, &d2);
    /* libxaac's first frame is short; every other access unit is 1024 samples */
    ASSERT(n1 > (long)(u.count - 1) * 1024 && n1 <= (long)u.count * 1024);
    ASSERT_EQ(n1, n2);
    ASSERT_EQ(d1, d2);
    ASSERT_EQ((int64_t)u.count * 1024 * 1000000 / 48000, d2);
    ASSERT_EQ(0, memcmp(plain, packed, sizeof(float) * 2 * (size_t)(n1 < N + 2048 ? n1 : N + 2048)));
    free(u.all.data);
    free(one.data);
    free(two.data);
    PASS();
}

/* ---- Cover art: ID3 pictures before an MP3, a PICTURE block in FLAC ---- */

typedef struct {
    uint8_t data[512];
    size_t size;
} small;

static void
put_bytes(small *b, const void *p, size_t n)
{
    memcpy(b->data + b->size, p, n);
    b->size += n;
}

static void
put_be(small *b, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--)
        b->data[b->size++] = (uint8_t)(v >> (8 * i));
}

/* Appends n bytes to file; aborts the test run if memory runs out. */
static void
grow(bytes *file, const void *p, size_t n)
{
    uint8_t *grown = realloc(file->data, file->size + n);

    if (!grown) {
        free(file->data);
        abort();
    }
    memcpy(grown + file->size, p, n);
    file->data = grown;
    file->size += n;
}

/* An ID3v2 frame: id, size (syncsafe in v2.4), flags, then body. */
static void
id3_frame(small *tag, int version, const char *id, const small *body, int flags)
{
    put_bytes(tag, id, version == 2 ? 3 : 4);
    if (version == 2)
        put_be(tag, (uint32_t)body->size, 3);
    else if (version == 3)
        put_be(tag, (uint32_t)body->size, 4);
    else
        put_be(tag, (uint32_t)((body->size & 127) | (body->size >> 7 & 127) << 8), 4);
    if (version != 2)
        put_be(tag, (uint32_t)flags, 2);
    put_bytes(tag, body->data, body->size);
}

/* The bytes of tag (with its frames) as an ID3v2 file header. */
static void
id3_header(bytes *file, int version, const small *tag)
{
    uint8_t h[10] = { 'I', 'D', '3', (uint8_t)version, 0, 0, 0, 0, (uint8_t)(tag->size >> 7 & 127),
                      (uint8_t)(tag->size & 127) };

    grow(file, h, 10);
    grow(file, tag->data, tag->size);
}

static int
cover_is(papaya_demuxer *dm, const char *want)
{
    size_t n;
    const uint8_t *cover = dm ? papaya_demux_cover(dm, &n) : NULL;

    return cover && n == strlen(want) && memcmp(cover, want, n) == 0;
}

/* An MP3 behind a tag of each ID3 version: the picture is found, the front
 * cover wins over another kind, and the audio still plays. */
TEST
mp3_cover(void)
{
    char path[1024];
    size_t mp3_size;
    uint8_t *mp3;
    int bad = 0; /* checked at the end, once everything is freed */

    snprintf(path, sizeof path, "%s/l3-si.bit", MINIMP3_VECTORS);
    mp3 = slurp(path, &mp3_size);
    ASSERT(mp3 != NULL);
    for (int version = 2; version <= 4; version++) {
        small tag = { { 0 }, 0 }, body = { { 0 }, 0 };
        bytes file = { NULL, 0 };
        papaya_demuxer *dm;
        papaya_packet *p;

        /* first a back cover (kind 4), then the front (3), UTF-16 descriptions */
        for (int kind = 4; kind >= 3; kind--) {
            body.size = 0;
            put_be(&body, 1, 1); /* UTF-16 with a BOM */
            if (version == 2)
                put_bytes(&body, "PNG", 3);
            else
                put_bytes(&body, "image/png", 10);
            put_be(&body, (uint32_t)kind, 1);
            put_bytes(&body, "\xff\xfe" "a\0\0\0", 6);
            put_bytes(&body, kind == 3 ? "front" : "back", kind == 3 ? 5 : 4);
            id3_frame(&tag, version, version == 2 ? "PIC" : "APIC", &body, 0);
        }
        put_bytes(&tag, "\0\0\0\0\0\0\0\0\0\0", 10); /* padding */
        id3_header(&file, version, &tag);
        grow(&file, mp3, mp3_size);
        dm = papaya_demux_open(papaya_io_open_memory(file.data, file.size));
        if (!dm || strcmp(papaya_demux_name(dm), "mp3") != 0 || !cover_is(dm, "front")
            || papaya_demux_read(dm, &p) != 0)
            bad++;
        else
            papaya_packet_free(&p);
        papaya_demux_close(&dm);
        /* cut anywhere in the tag: no crash, no stray picture */
        for (size_t cut = 1; cut < 10 + tag.size; cut += 7) {
            dm = papaya_demux_open(papaya_io_open_memory(file.data, cut));
            papaya_demux_close(&dm);
        }
        free(file.data);
    }
    /* v2.4 with the picture frame unsynchronised: FF 00 becomes FF again */
    {
        small tag = { { 0 }, 0 }, body = { { 0 }, 0 };
        bytes file = { NULL, 0 };
        papaya_demuxer *dm;

        put_be(&body, 0, 1);
        put_bytes(&body, "image/jpeg", 11);
        put_be(&body, 3, 1);
        put_be(&body, 0, 1); /* empty description */
        put_bytes(&body, "\xff\x00\xd8\xff\x00\xe0jpeg", 10);
        id3_frame(&tag, 4, "APIC", &body, 0x02);
        id3_header(&file, 4, &tag);
        grow(&file, mp3, mp3_size);
        dm = papaya_demux_open(papaya_io_open_memory(file.data, file.size));
        bad += !cover_is(dm, "\xff\xd8\xff\xe0jpeg");
        papaya_demux_close(&dm);
        free(file.data);
    }
    /* no tag: no cover */
    {
        papaya_demuxer *dm = papaya_demux_open(papaya_io_open_memory(mp3, mp3_size));
        size_t n;

        bad += !dm || papaya_demux_cover(dm, &n) != NULL || n != 0;
        papaya_demux_close(&dm);
    }
    free(mp3);
    ASSERT_EQ(0, bad);
    PASS();
}

/* ---- FLAC: libFLAC writes the file, every sample comes back ------------ */

static FLAC__StreamEncoderWriteStatus
flac_write(const FLAC__StreamEncoder *enc, const FLAC__byte data[], size_t size, uint32_t samples,
           uint32_t frame, void *user)
{
    (void)enc;
    (void)samples;
    (void)frame;
    append_unit(user, data, (int)size);
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

static FLAC__int32
flac_value(int i, int c, int bits)
{
    /* a sine with a little deterministic noise, so frames differ */
    double v = 0.6 * sin(2 * 3.14159265358979 * 440 * i / RATE + c) + 0.05 * (double)((i * 7919 + c) % 101 - 50) / 50;

    return (FLAC__int32)(v * (double)((1 << (bits - 1)) - 1));
}

TEST
flac_round_trip(int bits)
{
    enum { N = 3 * RATE };
    static FLAC__int32 left[N], right[N];
    const FLAC__int32 *chans[2] = { left, right };
    FLAC__StreamEncoder *enc = FLAC__stream_encoder_new();
    bytes file = { NULL, 0 };
    papaya_demuxer *dm;
    papaya_frame_pool *pool;
    papaya_decoder *d;
    papaya_packet *p;
    papaya_frame *f;
    long n = 0, wrong = 0;
    double scale = 1.0 / (double)(1 << (bits - 1));

    for (int i = 0; i < N; i++) {
        left[i] = flac_value(i, 0, bits);
        right[i] = flac_value(i, 1, bits);
    }
    FLAC__stream_encoder_set_channels(enc, 2);
    FLAC__stream_encoder_set_bits_per_sample(enc, (uint32_t)bits);
    FLAC__stream_encoder_set_sample_rate(enc, RATE);
    FLAC__stream_encoder_set_total_samples_estimate(enc, N);
    ASSERT_EQ(FLAC__STREAM_ENCODER_INIT_STATUS_OK,
              FLAC__stream_encoder_init_stream(enc, flac_write, NULL, NULL, NULL, &file));
    ASSERT(FLAC__stream_encoder_process(enc, chans, N));
    ASSERT(FLAC__stream_encoder_finish(enc));
    FLAC__stream_encoder_delete(enc);
    /* A PICTURE block after STREAMINFO, as taggers write it. */
    {
        small pic = { { 0 }, 0 };
        uint8_t *with;

        put_be(&pic, 6, 1); /* PICTURE, not the last block */
        put_be(&pic, 0, 3);
        put_be(&pic, 3, 4); /* front cover */
        put_be(&pic, 9, 4);
        put_bytes(&pic, "image/png", 9);
        put_be(&pic, 3, 4);
        put_bytes(&pic, "art", 3);
        for (int i = 0; i < 4; i++)
            put_be(&pic, 0, 4); /* width, height, depth, colours */
        put_be(&pic, 9, 4);
        put_bytes(&pic, "flacimage", 9);
        pic.data[1] = 0;
        pic.data[2] = 0;
        pic.data[3] = (uint8_t)(pic.size - 4);
        ASSERT(file.size > 42 && !(file.data[4] & 0x80)); /* STREAMINFO is not the last block */
        with = malloc(file.size + pic.size);
        memcpy(with, file.data, 42);
        memcpy(with + 42, pic.data, pic.size);
        memcpy(with + 42 + pic.size, file.data + 42, file.size - 42);
        free(file.data);
        file.data = with;
        file.size += pic.size;
    }

    dm = papaya_demux_open(papaya_io_open_memory(file.data, file.size));
    ASSERT(dm != NULL);
    ASSERT_STR_EQ("flac", papaya_demux_name(dm));
    ASSERT(cover_is(dm, "flacimage"));
    ASSERT_EQ((int64_t)N * 1000000 / RATE, papaya_demux_duration(dm));
    pool = papaya_frame_pool_new();
    d = papaya_decoder_open(papaya_demux_stream(dm, 0), pool);
    ASSERT(d != NULL);
    while (papaya_demux_read(dm, &p) == 0) {
        ASSERT_EQ_FMT((long long)n * 1000000 / RATE, (long long)p->pts, "%lld");
        ASSERT_EQ(PAPAYA_DECODE_OK, papaya_decoder_send(d, p));
        papaya_packet_free(&p);
        while (papaya_decoder_receive(d, &f) == PAPAYA_DECODE_OK) {
            const float *s = (const float *)f->audio[0];

            for (size_t i = 0; i < (size_t)f->samples; i++)
                if (lround((double)s[2 * i] / scale) != left[(size_t)n + i]
                    || lround((double)s[2 * i + 1] / scale) != right[(size_t)n + i])
                    wrong++;
            n += f->samples;
            papaya_frame_unref(&f);
        }
    }
    ASSERT_EQ(N, n);
    ASSERT_EQ(0, wrong); /* lossless */
    /* A seek lands on the frame holding the target. */
    ASSERT_EQ(0, papaya_demux_seek(dm, 1500000));
    ASSERT_EQ(0, papaya_demux_read(dm, &p));
    ASSERT(p->pts <= 1500000 && p->pts > 1500000 - (int64_t)4096 * 1000000 / RATE);
    papaya_packet_free(&p);
    papaya_decoder_close(&d);
    papaya_frame_pool_release(&pool);
    papaya_demux_close(&dm);
    for (size_t cut = 4; cut < file.size; cut += 4999) {
        papaya_demuxer *t = papaya_demux_open(papaya_io_open_memory(file.data, cut));

        while (t && papaya_demux_read(t, &p) == 0)
            papaya_packet_free(&p);
        papaya_demux_close(&t);
    }
    free(file.data);
    PASS();
}

/* ---- Ogg: Vorbis, Opus and FLAC written here, read back ---------------- */

#define OGG_SECONDS 4

/* A FLAC PICTURE block (front cover, image/png, the bytes "oggcover") in
 * base64, as Vorbis comments carry it. */
#define OGG_PICTURE "METADATA_BLOCK_PICTURE=AAAAAwAAAAlpbWFnZS9wbmcAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAhvZ2djb3Zlcg=="
/* Pages a packet into the file; flush pushes out the page at once. */
static void
ogg_put(ogg_stream_state *os, bytes *file, ogg_packet *op, int flush)
{
    ogg_page og;

    ogg_stream_packetin(os, op);
    while (flush ? ogg_stream_flush(os, &og) : ogg_stream_pageout(os, &og)) {
        append_unit(file, og.header, (int)og.header_len);
        append_unit(file, og.body, (int)og.body_len);
    }
}

static float
tone(long i, int rate)
{
    return 0.5f * (float)sin(2 * 3.14159265358979 * 440 * (double)i / rate);
}

static void
make_ogg_vorbis(bytes *file, int serial)
{
    vorbis_info vi;
    vorbis_comment vc;
    vorbis_dsp_state vd;
    vorbis_block vb;
    ogg_stream_state os;
    ogg_packet hdr[3], op;

    vorbis_info_init(&vi);
    vorbis_encode_init_vbr(&vi, 2, RATE, 0.4f);
    vorbis_comment_init(&vc);
    vorbis_comment_add(&vc, OGG_PICTURE);
    vorbis_analysis_init(&vd, &vi);
    vorbis_block_init(&vd, &vb);
    ogg_stream_init(&os, serial);
    vorbis_analysis_headerout(&vd, &vc, &hdr[0], &hdr[1], &hdr[2]);
    ogg_put(&os, file, &hdr[0], 1);
    ogg_put(&os, file, &hdr[1], 0);
    ogg_put(&os, file, &hdr[2], 1);
    for (long pos = 0, done = 0; !done;) {
        if (pos < (long)RATE * OGG_SECONDS) {
            float **buf = vorbis_analysis_buffer(&vd, 1024);

            for (int i = 0; i < 1024; i++)
                buf[0][i] = buf[1][i] = tone(pos + i, RATE);
            vorbis_analysis_wrote(&vd, 1024);
            pos += 1024;
        } else {
            vorbis_analysis_wrote(&vd, 0);
            done = 1;
        }
        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, NULL);
            vorbis_bitrate_addblock(&vb);
            while (vorbis_bitrate_flushpacket(&vd, &op))
                ogg_put(&os, file, &op, op.e_o_s != 0);
        }
    }
    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
}

static void
make_ogg_opus(bytes *file, int serial)
{
    enum { FRAME = 960, PRESKIP = 312 };
    unsigned char tags[8 + 4 + 4 + 4 + sizeof OGG_PICTURE] = { 'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 0, 0, 0, 0,
                                                                1, 0, 0, 0, sizeof OGG_PICTURE - 1, 0, 0, 0 };
    unsigned char head[19] = { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, PRESKIP & 255, PRESKIP >> 8,
                               0x80, 0xBB, 0, 0, 0, 0, 0 }; /* 48000 Hz, gain 0, family 0 */
    int err;
    OpusEncoder *enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &err);
    ogg_stream_state os;
    ogg_packet op;
    long total = 48000L * OGG_SECONDS, frames = total / FRAME;
    static float pcm[FRAME * 2];
    unsigned char out[1500];

    memcpy(tags + 20, OGG_PICTURE, sizeof OGG_PICTURE - 1);
    ogg_stream_init(&os, serial);
    memset(&op, 0, sizeof op);
    op.packet = head;
    op.bytes = sizeof head;
    op.b_o_s = 1;
    ogg_put(&os, file, &op, 1);
    op.packet = (unsigned char *)tags;
    op.bytes = sizeof tags - 1;
    op.b_o_s = 0;
    op.packetno = 1;
    ogg_put(&os, file, &op, 1);
    for (long f = 0; f < frames; f++) {
        for (size_t i = 0; i < FRAME; i++)
            pcm[2 * i] = pcm[2 * i + 1] = tone(f * FRAME + (long)i, 48000);
        op.bytes = opus_encode_float(enc, pcm, FRAME, out, sizeof out);
        op.packet = out;
        op.packetno = 2 + f;
        op.granulepos = (f + 1) * FRAME + PRESKIP;
        op.e_o_s = f == frames - 1;
        ogg_put(&os, file, &op, op.e_o_s != 0);
    }
    ogg_stream_clear(&os);
    opus_encoder_destroy(enc);
}

static void
make_ogg_flac(bytes *file)
{
    enum { N = RATE * OGG_SECONDS };
    static FLAC__int32 ch[N];
    const FLAC__int32 *chans[2] = { ch, ch };
    FLAC__StreamEncoder *enc = FLAC__stream_encoder_new();
    /* the comment block given too: one libFLAC adds itself is left out of
     * the Ogg header count */
    FLAC__StreamMetadata *pic = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PICTURE);
    FLAC__StreamMetadata *meta[2] = { FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT), pic };

    pic->data.picture.type = FLAC__STREAM_METADATA_PICTURE_TYPE_FRONT_COVER;
    FLAC__metadata_object_picture_set_mime_type(pic, (char *)"image/png", true);
    FLAC__metadata_object_picture_set_data(pic, (FLAC__byte *)"oggcover", 8, true);
    FLAC__stream_encoder_set_metadata(enc, meta, 2);
    for (int i = 0; i < N; i++)
        ch[i] = (FLAC__int32)(tone(i, RATE) * 32767);
    FLAC__stream_encoder_set_channels(enc, 2);
    FLAC__stream_encoder_set_bits_per_sample(enc, 16);
    FLAC__stream_encoder_set_sample_rate(enc, RATE);
    FLAC__stream_encoder_set_total_samples_estimate(enc, N);
    FLAC__stream_encoder_init_ogg_stream(enc, NULL, flac_write, NULL, NULL, NULL, file);
    FLAC__stream_encoder_process(enc, chans, N);
    FLAC__stream_encoder_finish(enc);
    FLAC__stream_encoder_delete(enc);
    FLAC__metadata_object_delete(meta[0]);
    FLAC__metadata_object_delete(pic);
}

TEST
ogg_round_trip(papaya_codec codec)
{
    bytes file = { NULL, 0 };
    int rate = codec == PAPAYA_CODEC_OPUS ? 48000 : RATE;
    papaya_demuxer *dm;
    const papaya_stream_info *s;
    papaya_frame_pool *pool;
    papaya_decoder *d;
    papaya_packet *p;
    papaya_frame *f;
    long samples = 0, crossings = 0;
    float last = 0;
    int64_t first_pts = PAPAYA_NOPTS;

    if (codec == PAPAYA_CODEC_VORBIS)
        make_ogg_vorbis(&file, 1234);
    else if (codec == PAPAYA_CODEC_OPUS)
        make_ogg_opus(&file, 99);
    else
        make_ogg_flac(&file);
    ASSERT(file.size > 1000);
    dm = papaya_demux_open(papaya_io_open_memory(file.data, file.size));
    ASSERT(dm != NULL);
    ASSERT_STR_EQ("ogg", papaya_demux_name(dm));
    ASSERT_EQ(1, papaya_demux_stream_count(dm));
    ASSERT(cover_is(dm, "oggcover"));
    s = papaya_demux_stream(dm, 0);
    ASSERT_EQ(codec, s->codec);
    ASSERT_EQ(rate, s->sample_rate);
    ASSERT_EQ(2, s->channels);
    /* The length is the last granule: the real samples, gapless. */
    ASSERT(labs((long)(papaya_demux_duration(dm) - OGG_SECONDS * 1000000LL)) < 30000);
    pool = papaya_frame_pool_new();
    d = papaya_decoder_open(s, pool);
    ASSERT(d != NULL);
    while (papaya_demux_read(dm, &p) == 0) {
        ASSERT_EQ(PAPAYA_DECODE_OK, papaya_decoder_send(d, p));
        papaya_packet_free(&p);
        while (papaya_decoder_receive(d, &f) == PAPAYA_DECODE_OK) {
            const float *x = (const float *)f->audio[0];

            if (first_pts == PAPAYA_NOPTS)
                first_pts = f->pts;
            for (size_t i = 0; i < (size_t)f->samples; i++) {
                if ((x[2 * i] >= 0) != (last >= 0))
                    crossings++;
                last = x[2 * i];
            }
            samples += f->samples;
            papaya_frame_unref(&f);
        }
    }
    ASSERT_EQ(0, (int)first_pts);
    ASSERT(labs(samples - (long)rate * OGG_SECONDS) < 4096);
    ASSERT(crossings > 880 * OGG_SECONDS - 40 && crossings < 880 * OGG_SECONDS + 40); /* 440 Hz */
    /* A seek: the first packet's time is at or before the target, close. */
    papaya_decoder_flush(d);
    ASSERT_EQ(0, papaya_demux_seek(dm, 2500000));
    for (;;) {
        ASSERT_EQ(0, papaya_demux_read(dm, &p));
        if (p->pts != PAPAYA_NOPTS)
            break;
        papaya_packet_free(&p);
    }
    ASSERT(p->pts <= 2500000 && p->pts > 2500000 - 1500000); /* within a page or two */
    papaya_packet_free(&p);
    ASSERT_EQ(0, papaya_demux_seek(dm, 0));
    ASSERT_EQ(0, papaya_demux_read(dm, &p));
    ASSERT_EQ(0, (int)p->pts);
    papaya_packet_free(&p);
    papaya_decoder_close(&d);
    papaya_frame_pool_release(&pool);
    papaya_demux_close(&dm);
    for (size_t cut = 4; cut < file.size; cut += 3001) {
        papaya_demuxer *t = papaya_demux_open(papaya_io_open_memory(file.data, cut));

        while (t && papaya_demux_read(t, &p) == 0)
            papaya_packet_free(&p);
        papaya_demux_close(&t);
    }
    free(file.data);
    PASS();
}

/* Three links back to back, each with its own serial and headers: they
 * play as one stream, times running on, and a seek lands in the right link. */
#define LINKS 3

TEST
ogg_chained(papaya_codec codec)
{
    bytes file = { NULL, 0 };
    int rate = codec == PAPAYA_CODEC_OPUS ? 48000 : RATE, configs = 0;
    papaya_demuxer *dm;
    papaya_frame_pool *pool;
    papaya_decoder *d;
    papaya_packet *p;
    papaya_frame *f;
    long samples = 0, crossings = 0;
    float last = 0;
    int64_t total = (int64_t)LINKS * OGG_SECONDS * 1000000;

    for (int i = 0; i < LINKS; i++) {
        if (codec == PAPAYA_CODEC_VORBIS)
            make_ogg_vorbis(&file, 500 + i);
        else
            make_ogg_opus(&file, 500 + i);
    }
    dm = papaya_demux_open(papaya_io_open_memory(file.data, file.size));
    ASSERT(dm != NULL);
    ASSERT_EQ(1, papaya_demux_stream_count(dm));
    ASSERT(labs((long)(papaya_demux_duration(dm) - total)) < 30000L * LINKS);
    pool = papaya_frame_pool_new();
    d = papaya_decoder_open(papaya_demux_stream(dm, 0), pool);
    ASSERT(d != NULL);
    while (papaya_demux_read(dm, &p) == 0) {
        configs += (p->flags & PAPAYA_PACKET_CONFIG) != 0;
        ASSERT_EQ(PAPAYA_DECODE_OK, papaya_decoder_send(d, p));
        papaya_packet_free(&p);
        while (papaya_decoder_receive(d, &f) == PAPAYA_DECODE_OK) {
            const float *x = (const float *)f->audio[0];

            /* each frame's time is where the samples so far end, near enough */
            ASSERT(labs((long)(f->pts - (int64_t)samples * 1000000 / rate)) < 30000);
            for (size_t i = 0; i < (size_t)f->samples; i++) {
                if ((x[2 * i] >= 0) != (last >= 0))
                    crossings++;
                last = x[2 * i];
            }
            samples += f->samples;
            papaya_frame_unref(&f);
        }
    }
    ASSERT_EQ(LINKS - 1, configs);
    ASSERT(labs(samples - (long)rate * OGG_SECONDS * LINKS) < 4096L * LINKS);
    ASSERT(crossings > (880L * OGG_SECONDS - 40) * LINKS && crossings < (880L * OGG_SECONDS + 40) * LINKS);

    /* The decoder has the last link's headers now. Back to the start: the
     * first link's again, then time 0. */
    papaya_decoder_flush(d);
    ASSERT_EQ(0, papaya_demux_seek(dm, 0));
    ASSERT_EQ(0, papaya_demux_read(dm, &p));
    ASSERT(p->flags & PAPAYA_PACKET_CONFIG);
    ASSERT_EQ(PAPAYA_DECODE_OK, papaya_decoder_send(d, p));
    papaya_packet_free(&p);
    ASSERT_EQ(0, papaya_demux_read(dm, &p));
    ASSERT_EQ(0, (int)p->pts);
    papaya_packet_free(&p);
    /* Into the middle link: its headers, then a time within it that decodes. */
    papaya_decoder_flush(d);
    ASSERT_EQ(0, papaya_demux_seek(dm, total / 2));
    ASSERT_EQ(0, papaya_demux_read(dm, &p));
    ASSERT(p->flags & PAPAYA_PACKET_CONFIG);
    ASSERT_EQ(PAPAYA_DECODE_OK, papaya_decoder_send(d, p));
    papaya_packet_free(&p);
    for (int got = 0; !got;) {
        ASSERT_EQ(0, papaya_demux_read(dm, &p));
        ASSERT(p->pts == PAPAYA_NOPTS || (p->pts <= total / 2 && p->pts > total / 2 - 1500000));
        ASSERT_EQ(PAPAYA_DECODE_OK, papaya_decoder_send(d, p));
        papaya_packet_free(&p);
        while (papaya_decoder_receive(d, &f) == PAPAYA_DECODE_OK) {
            got = 1;
            papaya_frame_unref(&f);
        }
    }
    papaya_decoder_close(&d);
    papaya_frame_pool_release(&pool);
    papaya_demux_close(&dm);
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
    RUN_TEST(mp3_cover);
    RUN_TEST1(adts_round_trip, 2); /* AAC-LC */
    RUN_TEST1(adts_round_trip, 5); /* HE-AAC */
    RUN_TEST(adts_variants);
    RUN_TEST1(flac_round_trip, 16);
    RUN_TEST1(flac_round_trip, 24);
    RUN_TEST1(ogg_round_trip, PAPAYA_CODEC_VORBIS);
    RUN_TEST1(ogg_round_trip, PAPAYA_CODEC_OPUS);
    RUN_TEST1(ogg_round_trip, PAPAYA_CODEC_FLAC);
    RUN_TEST1(ogg_chained, PAPAYA_CODEC_VORBIS);
    RUN_TEST1(ogg_chained, PAPAYA_CODEC_OPUS);
}

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(music);
    GREATEST_MAIN_END();
}
