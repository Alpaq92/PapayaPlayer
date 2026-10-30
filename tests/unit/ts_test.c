/* MPEG-TS: a small muxer writes the H.264 conformance stream and AAC from
 * libxaac's encoder into a transport stream; the demuxer must give back the
 * same pictures (bit-exact), the audio, the length, and seek to keyframes. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aac_encode.h"
#include "greatest.h"
#include "md5.h"
#include "papaya/decode.h"
#include "papaya/demux.h"

#define H264 PAPAYA_TESTDATA "/h264/CABA3_TOSHIBA_E.264"
#define H264_MD5 "13651d01c5b533e6b7aa132bae7669ed"
#define FRAMES 300
#define FRAME_90K 3003                          /* 29.97 fps */
#define START_90K (((int64_t)1 << 33) - (int64_t)90000 * 3) /* 3 s before the 33-bit wrap */
#define VIDEO_PID 0x100
#define AUDIO_PID 0x101

typedef struct {
    uint8_t *data;
    size_t size, cap;
} buf;

static void
put(buf *b, const void *p, size_t n)
{
    if (b->size + n > b->cap) {
        uint8_t *grown;

        b->cap = (b->size + n) * 2;
        grown = realloc(b->data, b->cap);
        if (!grown)
            abort();
        b->data = grown;
    }
    memcpy(b->data + b->size, p, n);
    b->size += n;
}

/* ---- the muxer ---------------------------------------------------------- */

typedef struct {
    buf out;
    int m2ts;           /* 192-byte packets */
    int cc[0x2000];
} mux;

static void
ts_packet(mux *m, int pid, int start, const uint8_t *data, size_t n, size_t *used)
{
    uint8_t p[188];
    size_t room = 184, take = n < room ? n : room;

    memset(p, 0xFF, sizeof p);
    p[0] = 0x47;
    p[1] = (uint8_t)((start ? 0x40 : 0) | pid >> 8);
    p[2] = (uint8_t)pid;
    if (take < room) { /* stuffing in an adaptation field */
        size_t af = room - take;

        p[3] = (uint8_t)(0x30 | (m->cc[pid] & 15));
        p[4] = (uint8_t)(af - 1);
        if (af > 1)
            p[5] = 0;
    } else {
        p[3] = (uint8_t)(0x10 | (m->cc[pid] & 15));
    }
    m->cc[pid]++;
    memcpy(p + 188 - take, data, take);
    if (m->m2ts)
        put(&m->out, "\0\0\0\0", 4);
    put(&m->out, p, 188);
    *used = take;
}

static uint32_t
crc32_mpeg(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFF;

    while (n--) {
        c ^= (uint32_t)*p++ << 24;
        for (int i = 0; i < 8; i++)
            c = c & 0x80000000 ? c << 1 ^ 0x04C11DB7 : c << 1;
    }
    return c;
}

static void
section(mux *m, int pid, uint8_t *s, size_t n)
{
    uint8_t p[1 + 1024];
    uint32_t crc;
    size_t used;

    s[1] = (uint8_t)(0xB0 | (n + 4 - 3) >> 8); /* section length, with the CRC */
    s[2] = (uint8_t)(n + 4 - 3);
    crc = crc32_mpeg(s, n);
    p[0] = 0; /* pointer field */
    memcpy(p + 1, s, n);
    p[1 + n] = (uint8_t)(crc >> 24);
    p[2 + n] = (uint8_t)(crc >> 16);
    p[3 + n] = (uint8_t)(crc >> 8);
    p[4 + n] = (uint8_t)crc;
    ts_packet(m, pid, 1, p, n + 5, &used);
}

static void
tables(mux *m)
{
    uint8_t pat[] = { 0x00, 0, 0, 0, 1, 0xC1, 0, 0, 0, 1, 0xE0 | 0x10, 0x00 };
    uint8_t pmt[] = { 0x02, 0, 0, 0, 1, 0xC1, 0, 0, 0xE0 | VIDEO_PID >> 8, VIDEO_PID & 255, 0xF0, 0,
                      0x1B, 0xE0 | VIDEO_PID >> 8, VIDEO_PID & 255, 0xF0, 0,
                      0x0F, 0xE0 | AUDIO_PID >> 8, AUDIO_PID & 255, 0xF0, 0 };

    section(m, 0, pat, sizeof pat);
    section(m, 0x1000, pmt, sizeof pmt);
}

static void
pes(mux *m, int pid, int stream_id, int64_t pts, const uint8_t *data, size_t n)
{
    buf b = { 0 };
    uint8_t h[14];
    size_t at = 0, used;
    size_t len = n + 8 > 0xFFFF || stream_id == 0xE0 ? 0 : n + 8; /* video: unbounded */

    pts &= ((int64_t)1 << 33) - 1;
    h[0] = 0;
    h[1] = 0;
    h[2] = 1;
    h[3] = (uint8_t)stream_id;
    h[4] = (uint8_t)(len >> 8);
    h[5] = (uint8_t)len;
    h[6] = 0x80;
    h[7] = 0x80; /* PTS only */
    h[8] = 5;
    h[9] = (uint8_t)(0x21 | (pts >> 29 & 0x0E));
    h[10] = (uint8_t)(pts >> 22);
    h[11] = (uint8_t)(0x01 | (pts >> 14 & 0xFE));
    h[12] = (uint8_t)(pts >> 7);
    h[13] = (uint8_t)(0x01 | (pts << 1 & 0xFE));
    put(&b, h, sizeof h);
    put(&b, data, n);
    for (int first = 1; at < b.size; first = 0, at += used)
        ts_packet(m, pid, first, b.data + at, b.size - at, &used);
    free(b.data);
}

/* ---- the content -------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t *start;      /* each access unit's offset */
    int count, cap;
    size_t size;
} units;

static units v; /* the H.264 stream, read once */

static int
read_h264(units *u)
{
    FILE *f = fopen(H264, "rb");
    long n;
    int vcl_seen = 0;

    memset(u, 0, sizeof *u);
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return -1;
    }
    u->data = malloc((size_t)n);
    u->cap = (int)(n / 4 + 1);
    u->start = malloc(sizeof *u->start * (size_t)u->cap);
    if (!u->data || !u->start || fread(u->data, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(u->data);
        free((void *)u->start);
        u->data = NULL;
        u->start = NULL;
        return -1;
    }
    fclose(f);
    u->size = (size_t)n;
    /* Access units: a parameter set, delimiter or SEI, or a picture's first
     * slice, after a slice, starts one. */
    for (size_t at = 0; at + 5 <= (size_t)n; at++) {
        const uint8_t *d = u->data + at;
        int type, vcl;

        if (d[0] || d[1] || d[2] != 1)
            continue;
        type = d[3] & 0x1F;
        vcl = type >= 1 && type <= 5;
        if (u->count == u->cap)
            break;
        if (u->count == 0 || (vcl_seen && ((type >= 6 && type <= 9) || (vcl && (d[4] & 0x80))))) {
            u->start[u->count++] = at > 0 && d[-1] == 0 ? at - 1 : at;
            vcl_seen = 0;
        }
        vcl_seen |= vcl;
    }
    return 0;
}

static void
gather(void *user, const unsigned char *data, int size)
{
    buf *b = user;
    uint32_t n = (uint32_t)size;

    put(b, &n, sizeof n); /* length, then the ADTS frame */
    put(b, data, (size_t)size);
}

/* The test file: tables, then video and audio interleaved by time. */
static void
build(buf *out, int m2ts, const units *v, const buf *aac, int aac_frames)
{
    mux m;
    size_t a_at = 0;
    int a = 0;

    memset(&m, 0, sizeof m);
    m.m2ts = m2ts;
    tables(&m);
    for (int i = 0; i < v->count; i++) {
        int64_t t = START_90K + (int64_t)i * FRAME_90K;
        size_t end = i + 1 < v->count ? v->start[i + 1] : v->size;

        if (i % 30 == 0)
            tables(&m);
        pes(&m, VIDEO_PID, 0xE0, t, v->data + v->start[i], end - v->start[i]);
        /* audio up to this video time: 1024 samples at 48 kHz = 1920 ticks */
        while (a < aac_frames && (int64_t)a * 1920 <= (int64_t)i * FRAME_90K) {
            uint32_t n;

            memcpy(&n, aac->data + a_at, sizeof n);
            pes(&m, AUDIO_PID, 0xC0, START_90K + (int64_t)a * 1920, aac->data + a_at + 4, n);
            a_at += 4 + n;
            a++;
        }
    }
    *out = m.out;
}

static void
hash_frame(md5_ctx *m, const papaya_frame *f)
{
    int cw = (f->width + 1) / 2, ch = (f->height + 1) / 2;

    for (int y = 0; y < f->height; y++)
        md5_update(m, f->planes[0] + (size_t)y * (size_t)f->strides[0], (size_t)f->width);
    for (int p = 1; p <= 2; p++)
        for (int y = 0; y < ch; y++)
            md5_update(m, f->planes[p] + (size_t)y * (size_t)f->strides[p], (size_t)cw);
}

TEST
ts_round_trip(int m2ts)
{
    enum { RATE = 48000, N = RATE * 10 };
    static short pcm[N * 2];
    unsigned char config[64];
    int config_size, aac_frames, video = -1, audio = -1, pictures = 0;
    buf aac = { 0 }, file;
    papaya_demuxer *dm;
    papaya_frame_pool *pool = papaya_frame_pool_new();
    papaya_decoder *vd, *ad;
    papaya_packet *p;
    papaya_frame *f;
    md5_ctx m;
    char hex[33];
    long samples = 0;
    int64_t last_video = -1;

    if (!v.data)
        ASSERT_EQ(0, read_h264(&v));
    ASSERT_EQ(FRAMES, v.count);
    for (size_t i = 0; i < N; i++)
        pcm[2 * i] = pcm[2 * i + 1] = (short)(12000 * sin(2 * 3.14159265358979 * 440 * (double)i / RATE));
    aac_frames = aac_encode(2, 1, RATE, 2, 128000, pcm, N, gather, &aac, config, &config_size);
    ASSERT(aac_frames > 100);
    build(&file, m2ts, &v, &aac, aac_frames);

    dm = papaya_demux_open(papaya_io_open_memory(file.data, file.size));
    ASSERT(dm != NULL);
    ASSERT_STR_EQ("mpegts", papaya_demux_name(dm));
    ASSERT_EQ(2, papaya_demux_stream_count(dm));
    for (int i = 0; i < 2; i++) {
        if (papaya_demux_stream(dm, i)->codec == PAPAYA_CODEC_H264)
            video = i;
        if (papaya_demux_stream(dm, i)->codec == PAPAYA_CODEC_AAC)
            audio = i;
    }
    ASSERT(video >= 0 && audio >= 0);
    ASSERT_EQ(RATE, papaya_demux_stream(dm, audio)->sample_rate);
    /* The last picture's time, across the 33-bit wrap. */
    ASSERT_EQ((int64_t)(FRAMES - 1) * FRAME_90K * 100 / 9, papaya_demux_duration(dm));

    vd = papaya_decoder_open(papaya_demux_stream(dm, video), pool);
    ad = papaya_decoder_open(papaya_demux_stream(dm, audio), pool);
    ASSERT(vd != NULL && ad != NULL);
    md5_init(&m);
    while (papaya_demux_read(dm, &p) == 0) {
        papaya_decoder *d = p->stream == video ? vd : ad;

        if (p->stream == video) {
            ASSERT(p->pts > last_video); /* times unwrap: no jump back */
            last_video = p->pts;
        }
        ASSERT_EQ(PAPAYA_DECODE_OK, papaya_decoder_send(d, p));
        papaya_packet_free(&p);
        while (papaya_decoder_receive(vd, &f) == PAPAYA_DECODE_OK) {
            hash_frame(&m, f);
            pictures++;
            papaya_frame_unref(&f);
        }
        while (papaya_decoder_receive(ad, &f) == PAPAYA_DECODE_OK) {
            samples += f->samples;
            papaya_frame_unref(&f);
        }
    }
    papaya_decoder_send(vd, NULL);
    while (papaya_decoder_receive(vd, &f) == PAPAYA_DECODE_OK) {
        hash_frame(&m, f);
        pictures++;
        papaya_frame_unref(&f);
    }
    md5_final(&m, hex);
    ASSERT_EQ(FRAMES, pictures);
    ASSERT_STR_EQ(H264_MD5, hex); /* the pictures are bit-exact */
    ASSERT(samples > 0);

    /* A seek lands on a video keyframe at or before the target. */
    ASSERT_EQ(0, papaya_demux_seek(dm, 6000000));
    ASSERT_EQ(0, papaya_demux_read(dm, &p));
    ASSERT_EQ(video, p->stream);
    ASSERT(p->flags & PAPAYA_PACKET_KEY);
    ASSERT(p->pts <= 6000000);
    papaya_packet_free(&p);

    papaya_decoder_close(&vd);
    papaya_decoder_close(&ad);
    papaya_demux_close(&dm);
    /* Every cut opens or is refused, and reads end. */
    for (size_t cut = 100; cut < file.size; cut += 50021) {
        papaya_demuxer *t = papaya_demux_open(papaya_io_open_memory(file.data, cut));

        while (t && papaya_demux_read(t, &p) == 0)
            papaya_packet_free(&p);
        papaya_demux_close(&t);
    }
    papaya_frame_pool_release(&pool);
    free(file.data);
    free(aac.data);
    PASS();
}

/* ---- LATM, and a file whose first program is empty ---------------------- */

typedef struct {
    buf b;
    uint32_t acc;
    int n;
} bitw;

static void
bw(bitw *w, uint32_t v, int bits)
{
    while (bits-- > 0) {
        w->acc = w->acc << 1 | (v >> bits & 1);
        if (++w->n == 8) {
            uint8_t c = (uint8_t)w->acc;

            put(&w->b, &c, 1);
            w->acc = 0;
            w->n = 0;
        }
    }
}

/* One AAC access unit as a LOAS frame: an AudioMuxElement with its
 * StreamMuxConfig (version 0, one program and layer) every time. */
static void
loas(buf *out, const uint8_t *asc, int asc_bits, const uint8_t *au, size_t n)
{
    bitw w = { { 0 }, 0, 0 };
    uint8_t h[3];
    size_t len;

    bw(&w, 0, 1);  /* useSameStreamMux: no, a config follows */
    bw(&w, 0, 1);  /* audioMuxVersion */
    bw(&w, 1, 1);  /* allStreamsSameTimeFraming */
    bw(&w, 0, 6);  /* numSubFrames - 1 */
    bw(&w, 0, 4);  /* numProgram - 1 */
    bw(&w, 0, 3);  /* numLayer - 1 */
    for (int i = 0; i < asc_bits; i++)
        bw(&w, asc[i / 8] >> (7 - i % 8) & 1, 1);
    bw(&w, 0, 3);  /* frameLengthType: lengths in bytes */
    bw(&w, 0xFF, 8);
    bw(&w, 0, 1);  /* otherDataPresent */
    bw(&w, 0, 1);  /* crcCheckPresent */
    for (len = n; len >= 255; len -= 255)
        bw(&w, 255, 8);
    bw(&w, (uint32_t)len, 8);
    for (size_t i = 0; i < n; i++)
        bw(&w, au[i], 8);
    if (w.n)
        bw(&w, 0, 8 - w.n);
    h[0] = 0x56;
    h[1] = (uint8_t)(0xE0 | w.b.size >> 8);
    h[2] = (uint8_t)w.b.size;
    put(out, h, 3);
    put(out, w.b.data, w.b.size);
    free(w.b.data);
}

TEST
ts_latm_second_program(void)
{
    enum { RATE = 48000, N = RATE * 4 };
    static short pcm[N * 2];
    unsigned char config[64];
    int config_size = 0, frames;
    buf aus = { 0 };
    mux m;
    size_t at = 0;
    papaya_demuxer *dm;
    const papaya_stream_info *s;
    papaya_frame_pool *pool;
    papaya_decoder *d;
    papaya_packet *p;
    papaya_frame *f;
    long samples = 0;
    /* Program 1 has nothing playable; program 2 has LATM audio. */
    uint8_t pat[] = { 0x00, 0, 0, 0, 1, 0xC1, 0, 0, 0, 1, 0xE0 | 0x10, 0x00, 0, 2, 0xE0 | 0x10, 0x01 };
    uint8_t pmt1[] = { 0x02, 0, 0, 0, 1, 0xC1, 0, 0, 0xFF, 0xFF, 0xF0, 0 };
    uint8_t pmt2[] = { 0x02, 0, 0, 0, 2, 0xC1, 0, 0, 0xE0 | AUDIO_PID >> 8, AUDIO_PID & 255, 0xF0, 0,
                       0x11, 0xE0 | AUDIO_PID >> 8, AUDIO_PID & 255, 0xF0, 0 };

    for (size_t i = 0; i < N; i++)
        pcm[2 * i] = pcm[2 * i + 1] = (short)(12000 * sin(2 * 3.14159265358979 * 440 * (double)i / RATE));
    frames = aac_encode(2, 0, RATE, 2, 128000, pcm, N, gather, &aus, config, &config_size);
    ASSERT(frames > 100);
    ASSERT_EQ(2, config_size); /* AAC-LC's 16 bits, copied bit by bit below */
    memset(&m, 0, sizeof m);
    for (int a = 0; a < frames; a++) {
        uint32_t n;
        buf frame = { 0 };

        if (a % 20 == 0) {
            section(&m, 0, pat, sizeof pat);
            section(&m, 0x1000, pmt1, sizeof pmt1);
            section(&m, 0x1001, pmt2, sizeof pmt2);
        }
        memcpy(&n, aus.data + at, sizeof n);
        loas(&frame, config, 16, aus.data + at + 4, n);
        pes(&m, AUDIO_PID, 0xC0, 90000 + (int64_t)a * 1920, frame.data, frame.size);
        free(frame.data);
        at += 4 + n;
    }

    dm = papaya_demux_open(papaya_io_open_memory(m.out.data, m.out.size));
    ASSERT(dm != NULL);
    ASSERT_EQ(1, papaya_demux_stream_count(dm));
    s = papaya_demux_stream(dm, 0);
    ASSERT_EQ(PAPAYA_CODEC_AAC, s->codec);
    ASSERT_STR_EQ("LATM", s->codec_tag);
    ASSERT_STR_EQ("Program 2", s->title);
    ASSERT_EQ(RATE, s->sample_rate);
    ASSERT_EQ(2, s->channels);
    pool = papaya_frame_pool_new();
    d = papaya_decoder_open(s, pool);
    ASSERT(d != NULL);
    while (papaya_demux_read(dm, &p) == 0) {
        ASSERT_EQ(PAPAYA_DECODE_OK, papaya_decoder_send(d, p));
        papaya_packet_free(&p);
        while (papaya_decoder_receive(d, &f) == PAPAYA_DECODE_OK) {
            ASSERT_EQ(RATE, f->sample_rate);
            ASSERT_EQ(2, f->channels);
            samples += f->samples;
            papaya_frame_unref(&f);
        }
    }
    ASSERT(samples > (long)(frames - 3) * 1024 && samples <= (long)frames * 1024);
    papaya_decoder_close(&d);
    papaya_frame_pool_release(&pool);
    papaya_demux_close(&dm);
    free(m.out.data);
    free(aus.data);
    PASS();
}

SUITE(ts)
{
    RUN_TEST1(ts_round_trip, 0); /* .ts */
    RUN_TEST1(ts_round_trip, 1); /* .m2ts */
    RUN_TEST(ts_latm_second_program);
}

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(ts);
    free(v.data);
    free((void *)v.start);
    GREATEST_MAIN_END();
}
