#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "md5.h"
#include "papaya/decode.h"
#include "papaya/demux.h"

#define AV1_DATA PAPAYA_TESTDATA "/av1/"
#define VPX_DATA PAPAYA_TESTDATA "/vpx/"

/* Hashes a frame the way dav1d's md5 output does: every row of Y, then U and
 * V, width bytes per row (two per sample above 8 bits, little endian). */
static void
hash_frame(md5_ctx *m, const papaya_frame *f)
{
    int bytes = f->pix == PAPAYA_PIX_I420_10 ? 2 : 1;
    int cw = f->pix == PAPAYA_PIX_I444 ? f->width : (f->width + 1) / 2;
    int ch = f->pix == PAPAYA_PIX_I420 || f->pix == PAPAYA_PIX_I420_10 ? (f->height + 1) / 2 : f->height;

    for (int y = 0; y < f->height; y++)
        md5_update(m, f->planes[0] + (size_t)y * (size_t)f->strides[0], (size_t)f->width * (size_t)bytes);
    for (int p = 1; p <= 2; p++)
        for (int y = 0; y < ch; y++)
            md5_update(m, f->planes[p] + (size_t)y * (size_t)f->strides[p], (size_t)cw * (size_t)bytes);
}

/* Decodes every frame of file; returns the frame count, or -1. */
static int
decode_file(const char *path, char md5[33], papaya_pixel_format *pix)
{
    papaya_demuxer *dm = papaya_demux_open(papaya_io_open_file(path));
    papaya_frame_pool *pool = papaya_frame_pool_new();
    papaya_decoder *d;
    papaya_packet *p;
    papaya_frame *f;
    md5_ctx m;
    int frames = 0;

    if (!dm)
        return -1;
    d = papaya_decoder_open(papaya_demux_stream(dm, 0), pool);
    if (!d)
        return -1;
    md5_init(&m);
    while (papaya_demux_read(dm, &p) == 0) {
        if (papaya_decoder_send(d, p) != PAPAYA_DECODE_OK)
            return -1;
        papaya_packet_free(&p);
        while (papaya_decoder_receive(d, &f) == PAPAYA_DECODE_OK) {
            *pix = f->pix;
            hash_frame(&m, f);
            frames++;
            papaya_frame_unref(&f);
        }
    }
    papaya_decoder_send(d, NULL);
    while (papaya_decoder_receive(d, &f) == PAPAYA_DECODE_OK) {
        hash_frame(&m, f);
        frames++;
        papaya_frame_unref(&f);
    }
    md5_final(&m, md5);
    papaya_decoder_close(&d);
    papaya_frame_pool_release(&pool);
    papaya_demux_close(&dm);
    return frames;
}

TEST
md5_self_test(void)
{
    md5_ctx m;
    char hex[33];

    md5_init(&m);
    md5_update(&m, "The quick brown fox jumps over the lazy dog", 43);
    md5_final(&m, hex);
    ASSERT_STR_EQ("9e107d9d372bb6826bd81d3542a419d6", hex);
    PASS();
}

/* dav1d's own conformance vectors, with the MD5s from dav1d-test-data. */
TEST
av1_8bit_bit_exact(void)
{
    char md5[33];
    papaya_pixel_format pix = PAPAYA_PIX_NONE;
    int frames = decode_file(AV1_DATA "00000527.ivf", md5, &pix);

    ASSERT(frames > 0);
    ASSERT_EQ(PAPAYA_PIX_I420, pix);
    ASSERT_STR_EQ("7986b9d849114e52c5159aba52789d45", md5);
    PASS();
}

TEST
av1_10bit_bit_exact(void)
{
    char md5[33];
    papaya_pixel_format pix = PAPAYA_PIX_NONE;
    int frames = decode_file(AV1_DATA "00000671.ivf", md5, &pix);

    ASSERT(frames > 0);
    ASSERT_EQ(PAPAYA_PIX_I420_10, pix);
    ASSERT_STR_EQ("827a458a97061dfcdb8b05039ca1e531", md5);
    PASS();
}

/* libvpx's vectors list one MD5 per frame: "<md5>  <name>-WxH-NNNN.i420". */
#define MAX_REF_FRAMES 64

static int
load_md5s(const char *path, char md5s[MAX_REF_FRAMES][33])
{
    char md5_path[1024], line[256];
    FILE *ref;
    int n = 0;

    snprintf(md5_path, sizeof md5_path, "%s.md5", path);
    ref = fopen(md5_path, "r");
    if (!ref)
        return -1;
    while (n < MAX_REF_FRAMES && fgets(line, sizeof line, ref)) {
        memcpy(md5s[n], line, 32);
        md5s[n++][32] = 0;
    }
    fclose(ref);
    return n;
}

static int
check_per_frame(const char *path)
{
    static char md5s[MAX_REF_FRAMES][33];
    int expected = load_md5s(path, md5s);
    papaya_demuxer *dm = papaya_demux_open(papaya_io_open_file(path));
    papaya_frame_pool *pool = papaya_frame_pool_new();
    papaya_decoder *d = dm ? papaya_decoder_open(papaya_demux_stream(dm, 0), pool) : NULL;
    papaya_packet *p;
    papaya_frame *f;
    int frames = 0, bad = 0;

    if (expected <= 0 || !d) {
        papaya_demux_close(&dm);
        papaya_frame_pool_release(&pool);
        return -1;
    }
    while (papaya_demux_read(dm, &p) == 0) {
        if (papaya_decoder_send(d, p) != PAPAYA_DECODE_OK)
            bad++;
        papaya_packet_free(&p);
        while (papaya_decoder_receive(d, &f) == PAPAYA_DECODE_OK) {
            md5_ctx m;
            char hex[33];

            md5_init(&m);
            hash_frame(&m, f);
            md5_final(&m, hex);
            if (frames >= expected || strcmp(md5s[frames], hex) != 0)
                bad++;
            frames++;
            papaya_frame_unref(&f);
        }
    }
    if (frames != expected)
        bad++;
    papaya_decoder_close(&d);
    papaya_frame_pool_release(&pool);
    papaya_demux_close(&dm);
    return bad ? -bad : frames;
}

TEST
vp8_bit_exact(void)
{
    ASSERT_EQ(29, check_per_frame(VPX_DATA "vp80-00-comprehensive-001.ivf"));
    PASS();
}

TEST
vp9_bit_exact(void)
{
    ASSERT_EQ(10, check_per_frame(VPX_DATA "vp90-2-02-size-16x16.webm"));
    PASS();
}

TEST
vp9_10bit_bit_exact(void)
{
    ASSERT_EQ(10, check_per_frame(VPX_DATA "vp92-2-20-10bit-yuv420.webm"));
    PASS();
}

/* Where the next start code (00 00 01) begins at or after at, or size. */
static size_t
next_start(const uint8_t *p, size_t size, size_t at)
{
    for (; at + 3 <= size; at++)
        if (p[at] == 0 && p[at + 1] == 0 && p[at + 2] == 1)
            return at;
    return size;
}

/* Whether the NAL unit (header at p) starts a new access unit: a parameter
 * set, delimiter or SEI, or the first slice of a picture. */
static int
starts_au(papaya_codec codec, const uint8_t *p, size_t n, int *vcl)
{
    if (codec == PAPAYA_CODEC_H264) {
        int type = p[0] & 0x1f;

        *vcl = type >= 1 && type <= 5;
        return (type >= 6 && type <= 9) || (*vcl && n > 1 && (p[1] & 0x80));
    }
    {
        int type = (p[0] >> 1) & 0x3f;

        *vcl = type < 32;
        return (type >= 32 && type <= 35) || type == 39 || (*vcl && n > 2 && (p[2] & 0x80));
    }
}

/* An Annex B elementary stream, split into access units as a demuxer would
 * and fed one packet each; after restart_at packets (if > 0) the decoder is
 * flushed and the stream fed again from the start. Returns the frame count,
 * or -1. */
static int
decode_annexb(const char *path, papaya_codec codec, int restart_at, char md5[33])
{
    papaya_stream_info s;
    papaya_frame_pool *pool = papaya_frame_pool_new();
    papaya_decoder *d;
    papaya_frame *f;
    md5_ctx m;
    uint8_t *data;
    long size;
    size_t au = 0, at;
    int frames = 0, pts = 0, have_vcl = 0;
    FILE *in = fopen(path, "rb");

    if (!in)
        return -1;
    fseek(in, 0, SEEK_END);
    size = ftell(in);
    fseek(in, 0, SEEK_SET);
    data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, in) != (size_t)size) {
        fclose(in);
        free(data);
        return -1;
    }
    fclose(in);
    memset(&s, 0, sizeof s);
    s.kind = PAPAYA_MEDIA_VIDEO;
    s.codec = codec;
    s.color.primaries = s.color.transfer = s.color.matrix = 2;
    d = papaya_decoder_open(&s, pool);
    if (!d) {
        free(data);
        return -1;
    }
    md5_init(&m);
    at = next_start(data, (size_t)size, 0);
    for (;;) {
        size_t nal = at + 3, end = next_start(data, (size_t)size, nal);
        int vcl = 0, cut = at >= (size_t)size || (starts_au(codec, data + nal, end - nal, &vcl) && have_vcl);

        if (cut && at > au) {
            papaya_packet *p = papaya_packet_new(data + au, at - au);

            if (!p)
                break;
            p->pts = pts++;
            papaya_decoder_send(d, p);
            papaya_packet_free(&p);
            while (papaya_decoder_receive(d, &f) == PAPAYA_DECODE_OK) {
                hash_frame(&m, f);
                frames++;
                papaya_frame_unref(&f);
            }
            au = at;
            have_vcl = 0;
            if (pts == restart_at) { /* as a seek back to the start does */
                papaya_decoder_flush(d);
                md5_init(&m);
                frames = 0;
                au = at = next_start(data, (size_t)size, 0);
                continue;
            }
        }
        if (at >= (size_t)size)
            break;
        have_vcl |= vcl;
        at = end;
    }
    free(data);
    papaya_decoder_send(d, NULL);
    while (papaya_decoder_receive(d, &f) == PAPAYA_DECODE_OK) {
        hash_frame(&m, f);
        frames++;
        papaya_frame_unref(&f);
    }
    md5_final(&m, md5);
    papaya_decoder_close(&d);
    papaya_frame_pool_release(&pool);
    return frames;
}

/* ITU-T conformance streams; the MD5s are of their reference YUV files. */
TEST
h264_bit_exact(void)
{
    static const struct {
        const char *file, *md5;
        int frames, restart;
    } v[] = {
        { "BA1_Sony_D.264", "114d1cf94a2fcaffda0cf1b49964bf3d", 17, 0 },      /* CAVLC, I/P */
        { "CABA3_TOSHIBA_E.264", "13651d01c5b533e6b7aa132bae7669ed", 300, 100 }, /* CABAC, B */
        { "CAPAMA3_Sand_F.264", "44d78296fb12c2e523cf02886e3ee4ad", 50, 0 },   /* PAFF + MBAFF */
    };

    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        char path[1024], md5[33];

        snprintf(path, sizeof path, "%s/h264/%s", PAPAYA_TESTDATA, v[i].file);
        ASSERT_EQ_FMTm(v[i].file, v[i].frames, decode_annexb(path, PAPAYA_CODEC_H264, v[i].restart, md5), "%d");
        ASSERT_STR_EQm(v[i].file, v[i].md5, md5);
    }
    PASS();
}

TEST
hevc_bit_exact(void)
{
    static const struct {
        const char *file, *md5;
        int frames, restart;
    } v[] = {
        { "AMP_A_Samsung_7.bin", "93a5875d58072db5539c04e1e943ed9d", 17, 9 },    /* asymmetric partitions */
        { "PICSIZE_A_Bossen_1.bin", "910ff0e6b3739af0b4aba4f6aa6704f8", 10, 0 }, /* 1056x8440, tiles */
    };

    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        char path[1024], md5[33];

        snprintf(path, sizeof path, "%s/hevc/%s", PAPAYA_TESTDATA, v[i].file);
        ASSERT_EQ_FMTm(v[i].file, v[i].frames, decode_annexb(path, PAPAYA_CODEC_HEVC, v[i].restart, md5), "%d");
        ASSERT_STR_EQm(v[i].file, v[i].md5, md5);
    }
    PASS();
}

SUITE(video)
{
    RUN_TEST(h264_bit_exact);
    RUN_TEST(hevc_bit_exact);
    RUN_TEST(md5_self_test);
    RUN_TEST(av1_8bit_bit_exact);
    RUN_TEST(av1_10bit_bit_exact);
    RUN_TEST(vp8_bit_exact);
    RUN_TEST(vp9_bit_exact);
    RUN_TEST(vp9_10bit_bit_exact);
}

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(video);
    GREATEST_MAIN_END();
}
