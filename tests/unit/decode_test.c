#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "FLAC/stream_encoder.h"
#include "aac_encode.h"
#include "greatest.h"
#include "papaya/decode.h"
#include "papaya/demux.h"
#include "vorbis/vorbisenc.h"

#define TESTDATA PAPAYA_LIBWEBM_TESTDATA "/"
#define PI 3.14159265358979

/* Sums every frame a decoder returns. */
typedef struct {
    long samples;
    int channels, rate;
    double energy;
    int crossings;      /* sign changes in channel 0, to check the pitch */
    float last;
    int64_t first_pts;
} totals;

static void
drain(papaya_decoder *d, totals *t)
{
    papaya_frame *f;

    while (papaya_decoder_receive(d, &f) == PAPAYA_DECODE_OK) {
        const float *s = (const float *)f->audio[0];

        if (t->first_pts == PAPAYA_NOPTS)
            t->first_pts = f->pts;
        t->channels = f->channels;
        t->rate = f->sample_rate;
        for (size_t i = 0; i < (size_t)f->samples; i++) {
            const float *frame = s + i * (size_t)f->channels;

            for (int c = 0; c < f->channels; c++)
                t->energy += (double)frame[c] * frame[c];
            if ((frame[0] >= 0) != (t->last >= 0))
                t->crossings++;
            t->last = frame[0];
        }
        t->samples += f->samples;
        papaya_frame_unref(&f);
    }
}

TEST
opus_from_webm(void)
{
    papaya_demuxer *dm = papaya_demux_open(papaya_io_open_file(TESTDATA "bbb_480p_vp9_opus_1second.webm"));
    papaya_frame_pool *pool = papaya_frame_pool_new();
    const papaya_stream_info *audio = NULL;
    papaya_decoder *d;
    papaya_packet *p;
    totals t = { .first_pts = PAPAYA_NOPTS };
    long packets = 0;

    ASSERT(dm != NULL);
    for (int i = 0; i < papaya_demux_stream_count(dm); i++)
        if (papaya_demux_stream(dm, i)->kind == PAPAYA_MEDIA_AUDIO)
            audio = papaya_demux_stream(dm, i);
    ASSERT(audio != NULL);
    d = papaya_decoder_open(audio, pool);
    ASSERT(d != NULL);
    ASSERT_STR_EQ("libopus", papaya_decoder_name(d));
    while (papaya_demux_read(dm, &p) == 0) {
        if (p->stream == audio->index) {
            packets++;
            ASSERT_EQ(PAPAYA_DECODE_OK, papaya_decoder_send(d, p));
            drain(d, &t);
        }
        papaya_packet_free(&p);
    }
    papaya_decoder_send(d, NULL);
    drain(d, &t);
    ASSERT_EQ(6, t.channels);
    ASSERT_EQ(48000, t.rate);
    /* 20 ms packets of 960 samples, less the OpusHead pre-skip of 312. */
    ASSERT_EQ(packets * 960 - 312, t.samples);
    ASSERT(t.energy > 0.01);
    papaya_decoder_close(&d);
    papaya_frame_pool_release(&pool);
    papaya_demux_close(&dm);
    PASS();
}

/* ---- Vorbis: encode a sine with libvorbisenc, decode it back ----------- */

typedef struct {
    uint8_t *data;
    size_t size;
} bytes;

static void
append(bytes *b, const void *data, size_t size)
{
    uint8_t *grown = realloc(b->data, b->size + size);

    if (!grown)
        abort();
    b->data = grown;
    memcpy(b->data + b->size, data, size);
    b->size += size;
}

TEST
vorbis_round_trip(void)
{
    enum { RATE = 44100, SECONDS = 1, CH = 2 };
    vorbis_info vi;
    vorbis_comment vc;
    vorbis_dsp_state vd;
    vorbis_block vb;
    ogg_packet hdr[3], op;
    bytes priv = { 0 };
    papaya_packet *packets[512];
    int npackets = 0;
    papaya_stream_info s = { .kind = PAPAYA_MEDIA_AUDIO, .codec = PAPAYA_CODEC_VORBIS };
    papaya_frame_pool *pool = papaya_frame_pool_new();
    papaya_decoder *d;
    totals t = { .first_pts = PAPAYA_NOPTS };
    uint8_t lace[8];
    size_t nl = 0;

    vorbis_info_init(&vi);
    ASSERT_EQ(0, vorbis_encode_init_vbr(&vi, CH, RATE, 0.4f));
    vorbis_comment_init(&vc);
    vorbis_analysis_init(&vd, &vi);
    vorbis_block_init(&vd, &vb);
    vorbis_analysis_headerout(&vd, &vc, &hdr[0], &hdr[1], &hdr[2]);
    /* Xiph lacing, as in Matroska's CodecPrivate. */
    lace[nl++] = 2;
    for (int h = 0; h < 2; h++) {
        long n = hdr[h].bytes;

        while (n >= 255) {
            lace[nl++] = 255;
            n -= 255;
        }
        lace[nl++] = (uint8_t)n;
    }
    append(&priv, lace, nl);
    for (int h = 0; h < 3; h++)
        append(&priv, hdr[h].packet, (size_t)hdr[h].bytes);

    for (int done = 0, pos = 0; !done;) {
        if (pos < RATE * SECONDS) {
            float **buf = vorbis_analysis_buffer(&vd, 1024);

            for (int i = 0; i < 1024; i++)
                for (int c = 0; c < CH; c++)
                    buf[c][i] = 0.5f * (float)sin(2 * PI * 440 * (pos + i) / RATE);
            vorbis_analysis_wrote(&vd, 1024);
            pos += 1024;
        } else {
            vorbis_analysis_wrote(&vd, 0);
            done = 1;
        }
        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, NULL);
            vorbis_bitrate_addblock(&vb);
            while (vorbis_bitrate_flushpacket(&vd, &op) && npackets < 512)
                packets[npackets++] = papaya_packet_new(op.packet, (size_t)op.bytes);
        }
    }
    s.extradata = priv.data;
    s.extradata_size = priv.size;
    d = papaya_decoder_open(&s, pool);
    ASSERT(d != NULL);
    for (int i = 0; i < npackets; i++) {
        packets[i]->pts = i == 0 ? 0 : PAPAYA_NOPTS;
        ASSERT_EQ(PAPAYA_DECODE_OK, papaya_decoder_send(d, packets[i]));
        drain(d, &t);
        papaya_packet_free(&packets[i]);
    }
    ASSERT_EQ(CH, t.channels);
    ASSERT_EQ(RATE, t.rate);
    ASSERT(labs(t.samples - (long)RATE * SECONDS) < 2048);
    /* 440 Hz: about 880 zero crossings a second. */
    ASSERT(t.crossings > 840 && t.crossings < 920);
    papaya_decoder_close(&d);
    papaya_frame_pool_release(&pool);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    free(priv.data);
    PASS();
}

/* ---- FLAC: encode with libFLAC, split into header and frames ----------- */

typedef struct {
    bytes header;
    papaya_packet *frames[256];
    int nframes;
} flac_out;

static FLAC__StreamEncoderWriteStatus
flac_write(const FLAC__StreamEncoder *enc, const FLAC__byte buffer[], size_t size, uint32_t samples,
           uint32_t frame, void *opaque)
{
    flac_out *o = opaque;

    (void)enc;
    (void)frame;
    if (samples == 0)
        append(&o->header, buffer, size); /* "fLaC" and metadata */
    else if (o->nframes < 256)
        o->frames[o->nframes++] = papaya_packet_new(buffer, size);
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

TEST
flac_round_trip(void)
{
    enum { RATE = 48000, CH = 2, N = 48000 };
    FLAC__StreamEncoder *enc = FLAC__stream_encoder_new();
    static FLAC__int32 left[N], right[N];
    const FLAC__int32 *chans[2] = { left, right };
    flac_out out = { 0 };
    papaya_stream_info s = { .kind = PAPAYA_MEDIA_AUDIO, .codec = PAPAYA_CODEC_FLAC };
    papaya_frame_pool *pool = papaya_frame_pool_new();
    papaya_decoder *d;
    totals t = { .first_pts = PAPAYA_NOPTS };

    for (int i = 0; i < N; i++)
        left[i] = right[i] = (FLAC__int32)(16000 * sin(2 * PI * 440 * i / RATE));
    FLAC__stream_encoder_set_channels(enc, CH);
    FLAC__stream_encoder_set_bits_per_sample(enc, 16);
    FLAC__stream_encoder_set_sample_rate(enc, RATE);
    FLAC__stream_encoder_set_total_samples_estimate(enc, N);
    ASSERT_EQ(FLAC__STREAM_ENCODER_INIT_STATUS_OK,
              FLAC__stream_encoder_init_stream(enc, flac_write, NULL, NULL, NULL, &out));
    ASSERT(FLAC__stream_encoder_process(enc, chans, N));
    ASSERT(FLAC__stream_encoder_finish(enc));
    FLAC__stream_encoder_delete(enc);

    s.extradata = out.header.data;
    s.extradata_size = out.header.size;
    d = papaya_decoder_open(&s, pool);
    ASSERT(d != NULL);
    ASSERT(out.nframes > 5);
    for (int i = 0; i < out.nframes; i++) {
        out.frames[i]->pts = i == 0 ? 0 : PAPAYA_NOPTS;
        ASSERT_EQ(PAPAYA_DECODE_OK, papaya_decoder_send(d, out.frames[i]));
        drain(d, &t);
        papaya_packet_free(&out.frames[i]);
    }
    ASSERT_EQ(CH, t.channels);
    ASSERT_EQ(RATE, t.rate);
    ASSERT_EQ(N, t.samples); /* lossless: every sample comes back */
    ASSERT(t.crossings > 860 && t.crossings < 900);
    ASSERT_EQ(0, (int)t.first_pts);
    papaya_decoder_close(&d);
    papaya_frame_pool_release(&pool);
    free(out.header.data);
    PASS();
}

/* ---- AAC: encode with libxaac's encoder, raw access units + ASC ------- */

typedef struct {
    papaya_packet *aus[256];
    int n;
} aac_out;

static void
aac_au(void *user, const unsigned char *data, int size)
{
    aac_out *o = user;

    if (o->n < 256)
        o->aus[o->n++] = papaya_packet_new(data, (size_t)size);
}

/* One second of a 440 Hz stereo sine through the given profile; the decoder
 * is flushed halfway, as a seek would. */
TEST
aac_round_trip(int aot)
{
    enum { RATE = 48000, CH = 2, N = 48000 };
    static short pcm[N * CH];
    unsigned char config[64];
    int config_size = 0, half;
    aac_out out = { 0 };
    papaya_stream_info s = { .kind = PAPAYA_MEDIA_AUDIO, .codec = PAPAYA_CODEC_AAC };
    papaya_frame_pool *pool = papaya_frame_pool_new();
    papaya_decoder *d;
    totals t = { .first_pts = PAPAYA_NOPTS };
    /* AAC-LC frames are 1024 samples; with SBR the output doubles. */
    int frame = aot == 2 ? 1024 : 2048;

    for (size_t i = 0; i < N; i++)
        pcm[2 * i] = pcm[2 * i + 1] = (short)(12000 * sin(2 * PI * 440 * (double)i / RATE));
    ASSERT(aac_encode(aot, 0, RATE, CH, aot == 2 ? 128000 : aot == 5 ? 64000 : 32000, pcm, N, aac_au,
                      &out, config, &config_size) > 10);
    s.extradata = config;
    s.extradata_size = (size_t)config_size;
    d = papaya_decoder_open(&s, pool);
    ASSERT(d != NULL);
    ASSERT_STR_EQ("libxaac", papaya_decoder_name(d));
    half = out.n / 2;
    for (int i = 0; i < out.n; i++) {
        if (i == half) {
            drain(d, &t);
            papaya_decoder_flush(d);
        }
        out.aus[i]->pts = (int64_t)i * frame * 1000000 / RATE;
        ASSERT_EQ(PAPAYA_DECODE_OK, papaya_decoder_send(d, out.aus[i]));
        drain(d, &t);
        papaya_packet_free(&out.aus[i]);
    }
    ASSERT_EQ(CH, t.channels);
    ASSERT_EQ(RATE, t.rate);
    /* Every access unit comes back, less what the decoder holds for its
     * delay around the restart. */
    ASSERT(t.samples > (long)(out.n - 2) * frame && t.samples <= (long)out.n * frame);
    /* 440 Hz is 880 sign changes a second, over the encoder's padding; the
     * restart adds a few at the edges. */
    ASSERT(t.crossings > 800 && t.crossings < 1000);
    ASSERT(t.energy / (double)t.samples > 0.05); /* not silence: 0.37^2 / 2 per channel */
    ASSERT_EQ(0, (int)t.first_pts);
    papaya_decoder_close(&d);
    papaya_frame_pool_release(&pool);
    PASS();
}

TEST
unknown_codec_has_no_decoder(void)
{
    papaya_stream_info s = { .kind = PAPAYA_MEDIA_AUDIO, .codec = PAPAYA_CODEC_AC3 };
    papaya_frame_pool *pool = papaya_frame_pool_new();

    ASSERT(papaya_decoder_open(&s, pool) == NULL);
    s.codec = PAPAYA_CODEC_OPUS; /* no OpusHead */
    ASSERT(papaya_decoder_open(&s, pool) == NULL);
    papaya_frame_pool_release(&pool);
    PASS();
}

SUITE(decode)
{
    RUN_TEST(opus_from_webm);
    RUN_TEST(vorbis_round_trip);
    RUN_TEST(flac_round_trip);
    RUN_TEST1(aac_round_trip, 2);  /* AAC-LC */
    RUN_TEST1(aac_round_trip, 5);  /* HE-AAC */
    RUN_TEST1(aac_round_trip, 29); /* HE-AAC v2 */
    RUN_TEST(unknown_codec_has_no_decoder);
}

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(decode);
    GREATEST_MAIN_END();
}
